/*
 * EFI_MEMORY_ATTRIBUTE_PROTOCOL (UEFI 2.10) for firmware that does not
 * publish it. Standard semantics, no platform policy:
 *
 *   GetMemoryAttributes   reports the access attributes (RP / RO / XP) the
 *                         current stage-1 translation tables apply to the
 *                         range; EFI_NO_MAPPING when they are not uniform.
 *   Set/ClearMemoryAttributes
 *                         merge the request into the attributes read from
 *                         the translation tables, write them through
 *                         EFI_CPU_ARCH_PROTOCOL.SetMemoryAttributes (the
 *                         only page-table writer the firmware exports; it
 *                         replaces the whole attribute set, so the GCD cache
 *                         type travels with the protection bits), then read
 *                         the tables back and fail with EFI_UNSUPPORTED if
 *                         the result does not match the request.
 *
 * The translation tables are the source of truth, not the GCD map: the DXE
 * core applies its memory protection policy to the tables without recording
 * it in GCD, so GCD protection bits are stale in both directions. UEFI boot
 * services run on a 1:1 mapping, so walking TTBR0 of the current exception
 * level by physical address gives the attributes of exactly the memory a
 * loader will execute from. This mirrors ArmPkg CpuDxe's implementation,
 * which also reads the live tables (through its own MMU library).
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <PiDxe.h>
#include <Library/ArmLib.h>
#include <Chipset/AArch64.h>              /* ArmGetTCR / ArmReadCurrentEL / ArmReadHcr */
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/SfbMemFixup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/EFIMemoryAttribute.h>

#define SFB_MA_PROT_MASK   (EFI_MEMORY_RP | EFI_MEMORY_RO | EFI_MEMORY_XP)
#define SFB_MA_CACHE_MASK  (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | \
                            EFI_MEMORY_WB | EFI_MEMORY_UCE | EFI_MEMORY_WP)

/* ---- AArch64 stage-1 translation table format, 4 KB granule (ARM ARM D8) */
#define SFB_TT_TYPE_MASK        0x3ULL
#define SFB_TT_TYPE_BLOCK       0x1ULL      /* levels 1-2 */
#define SFB_TT_TYPE_TABLE       0x3ULL      /* levels 0-2 */
#define SFB_TT_TYPE_PAGE        0x3ULL      /* level 3 */
#define SFB_TT_ADDR_MASK        0x0000FFFFFFFFF000ULL
#define SFB_TT_AF               (1ULL << 10)
#define SFB_TT_AP2_RO           (1ULL << 7)
#define SFB_TT_PXN              (1ULL << 53) /* EL1&0 and EL2 with VHE */
#define SFB_TT_XN               (1ULL << 54) /* EL2 without VHE (also UXN) */
#define SFB_TT_INDEX_MASK       0x1FFULL
#define SFB_TT_PAGE_SHIFT       12
#define SFB_TT_LEVEL_BITS       9

#define SFB_TCR_T0SZ_MASK       0x3FULL
#define SFB_TCR_TG0_SHIFT       14
#define SFB_TCR_TG0_MASK        0x3ULL
#define SFB_TCR_TG0_4K          0ULL

#define SFB_HCR_E2H             (1ULL << 34)
#define SFB_CURRENT_EL_EL2      0x8

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC EFI_HANDLE              mHandle;

/* the "execute never" bit that governs code running at this exception level */
STATIC UINT64                  mXnBit;

/* ------------------------------------------------------------------------ */
/* translation table walk                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  UINT64  Base;         /* leaf (block or page) base */
  UINT64  Size;         /* leaf size: 1 GB / 2 MB / 4 KB */
  UINT64  Prot;         /* EFI_MEMORY_RP / RO / XP */
} SFB_MA_LEAF;

/*
 * Resolve the leaf descriptor mapping Addr in TTBR0 of the current EL.
 *
 * @retval EFI_SUCCESS      Leaf filled.
 * @retval EFI_NO_MAPPING   Invalid descriptor on the walk, or Addr above the
 *                          TTBR0 range.
 * @retval EFI_UNSUPPORTED  Not a 4 KB granule / unexpected T0SZ.
 */
STATIC
EFI_STATUS
SfbMaLookup (
  IN  UINT64        Addr,
  OUT SFB_MA_LEAF  *Leaf
  )
{
  UINTN    Tcr = ArmGetTCR ();
  UINT64  *Table;
  UINTN    VaBits;
  UINTN    Levels;
  UINTN    Level;

  if (((Tcr >> SFB_TCR_TG0_SHIFT) & SFB_TCR_TG0_MASK) != SFB_TCR_TG0_4K) {
    return EFI_UNSUPPORTED;
  }
  VaBits = 64 - (Tcr & SFB_TCR_T0SZ_MASK);
  if (VaBits < 25 || VaBits > 48) {
    return EFI_UNSUPPORTED;
  }
  if ((Addr >> VaBits) != 0) {
    return EFI_NO_MAPPING;
  }

  /* number of lookup levels for this VA size, hence the starting level */
  Levels = (VaBits - SFB_TT_PAGE_SHIFT + SFB_TT_LEVEL_BITS - 1) / SFB_TT_LEVEL_BITS;
  Level = 4 - Levels;
  Table = (UINT64 *)(UINTN)((UINT64)(UINTN)ArmGetTTBR0BaseAddress () & SFB_TT_ADDR_MASK);

  for (;; Level++) {
    UINTN   Shift = SFB_TT_PAGE_SHIFT + SFB_TT_LEVEL_BITS * (3 - Level);
    UINT64  Desc = Table[(Addr >> Shift) & SFB_TT_INDEX_MASK];
    UINT64  Type = Desc & SFB_TT_TYPE_MASK;

    if ((Type & 1) == 0) {
      return EFI_NO_MAPPING;                /* invalid descriptor */
    }
    if (Level < 3 && Type == SFB_TT_TYPE_TABLE) {
      Table = (UINT64 *)(UINTN)(Desc & SFB_TT_ADDR_MASK);
      continue;
    }
    if (Level == 0 || (Level == 3 && Type != SFB_TT_TYPE_PAGE)) {
      return EFI_NO_MAPPING;                /* reserved encodings */
    }

    Leaf->Size = 1ULL << Shift;
    Leaf->Base = Addr & ~(Leaf->Size - 1);
    Leaf->Prot = 0;
    if ((Desc & SFB_TT_AF) == 0) {
      Leaf->Prot |= EFI_MEMORY_RP;          /* access flag clear: faults */
    }
    if ((Desc & SFB_TT_AP2_RO) != 0) {
      Leaf->Prot |= EFI_MEMORY_RO;
    }
    if ((Desc & mXnBit) != 0) {
      Leaf->Prot |= EFI_MEMORY_XP;
    }
    return EFI_SUCCESS;
  }
}

/* Callback per maximal run of leaves with identical protection, clipped to
 * the requested range. */
typedef
EFI_STATUS
(*SFB_MA_RUN_FN) (
  IN UINT64    Base,
  IN UINT64    Length,
  IN UINT64    Prot,
  IN OUT VOID *Context
  );

STATIC
EFI_STATUS
SfbMaForEachRun (
  IN UINT64         Base,
  IN UINT64         Length,
  IN SFB_MA_RUN_FN  Fn,
  IN OUT VOID      *Context
  )
{
  UINT64       End = Base + Length;
  UINT64       Cur = Base;
  UINT64       RunBase = Base;
  UINT64       RunProt = 0;
  BOOLEAN      InRun = FALSE;
  SFB_MA_LEAF  Leaf;
  EFI_STATUS   Status;

  while (Cur < End) {
    Status = SfbMaLookup (Cur, &Leaf);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (InRun && Leaf.Prot != RunProt) {
      Status = Fn (RunBase, Cur - RunBase, RunProt, Context);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      RunBase = Cur;
    }
    RunProt = Leaf.Prot;
    InRun = TRUE;
    Cur = Leaf.Base + Leaf.Size;            /* next leaf */
  }
  return Fn (RunBase, End - RunBase, RunProt, Context);
}

STATIC
BOOLEAN
SfbMaRangeValid (
  IN EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN UINT64                Length
  )
{
  if (Length == 0 || ((BaseAddress | Length) & EFI_PAGE_MASK) != 0) {
    return FALSE;
  }
  return BaseAddress + Length > BaseAddress;          /* no wrap */
}

/* GCD cache type of the page at Addr; EFI_MEMORY_WB when none is recorded
 * (the CPU protocol maps an empty cache type to an invalid attribute index) */
STATIC
UINT64
SfbMaCacheType (
  IN UINT64  Addr
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  UINT64                           Cache = 0;

  if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (Addr, &Desc))) {
    Cache = Desc.Attributes & SFB_MA_CACHE_MASK;
  }
  return Cache != 0 ? Cache : EFI_MEMORY_WB;
}

/* ------------------------------------------------------------------------ */
/* GetMemoryAttributes                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
  BOOLEAN  First;
  UINT64   Prot;
} SFB_MA_GET_CTX;

STATIC
EFI_STATUS
SfbMaGetRun (
  IN UINT64    Base,
  IN UINT64    Length,
  IN UINT64    Prot,
  IN OUT VOID *Context
  )
{
  SFB_MA_GET_CTX  *Ctx = Context;

  if (Ctx->First) {
    Ctx->Prot = Prot;
    Ctx->First = FALSE;
    return EFI_SUCCESS;
  }
  return Ctx->Prot == Prot ? EFI_SUCCESS : EFI_NO_MAPPING;
}

STATIC
EFI_STATUS
EFIAPI
SfbMaGetMemoryAttributes (
  IN  EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN  EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN  UINT64                          Length,
  OUT UINT64                         *Attributes
  )
{
  SFB_MA_GET_CTX  Ctx = { TRUE, 0 };
  EFI_STATUS      Status;

  if (Attributes == NULL || !SfbMaRangeValid (BaseAddress, Length)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SfbMaForEachRun (BaseAddress, Length, SfbMaGetRun, &Ctx);
  DEBUG ((EFI_D_INFO, "SFB-MEM: GetMemoryAttributes(0x%lx, 0x%lx) = 0x%lx: %r\n",
          BaseAddress, Length, Ctx.Prot, Status));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *Attributes = Ctx.Prot;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Set / Clear                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
  UINT64  SetMask;
  UINT64  ClearMask;
} SFB_MA_CHANGE_CTX;

typedef struct {
  UINT64  Want;
} SFB_MA_VERIFY_CTX;

STATIC
EFI_STATUS
SfbMaVerifyRun (
  IN UINT64    Base,
  IN UINT64    Length,
  IN UINT64    Prot,
  IN OUT VOID *Context
  )
{
  SFB_MA_VERIFY_CTX  *Ctx = Context;

  if (Prot != Ctx->Want) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: 0x%lx+0x%lx reads back 0x%lx, wanted 0x%lx\n",
            Base, Length, Prot, Ctx->Want));
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SfbMaChangeRun (
  IN UINT64    Base,
  IN UINT64    Length,
  IN UINT64    Prot,
  IN OUT VOID *Context
  )
{
  SFB_MA_CHANGE_CTX  *Ctx = Context;
  SFB_MA_VERIFY_CTX   Verify;
  UINT64              NewProt = (Prot | Ctx->SetMask) & ~Ctx->ClearMask;
  UINT64              Cache;
  EFI_STATUS          Status;

  if (NewProt == Prot) {
    return EFI_SUCCESS;                     /* already in the requested state */
  }

  /* the CPU protocol replaces the whole attribute set: cache type + protection */
  Cache = SfbMaCacheType (Base);
  Status = mCpu->SetMemoryAttributes (mCpu, Base, Length, Cache | NewProt);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: CpuArch SetMemoryAttributes(0x%lx, 0x%lx, 0x%lx): %r\n",
            Base, Length, Cache | NewProt, Status));
    return Status;
  }

  /* the tables are the contract: confirm the request landed */
  Verify.Want = NewProt;
  return SfbMaForEachRun (Base, Length, SfbMaVerifyRun, &Verify);
}

STATIC
EFI_STATUS
SfbMaChange (
  IN CONST CHAR8          *Op,
  IN EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN UINT64                Length,
  IN UINT64                SetMask,
  IN UINT64                ClearMask
  )
{
  SFB_MA_CHANGE_CTX  Ctx;
  UINT64             Requested = SetMask | ClearMask;
  EFI_STATUS         Status;

  if (Requested == 0 || (Requested & ~SFB_MA_PROT_MASK) != 0 ||
      !SfbMaRangeValid (BaseAddress, Length)) {
    DEBUG ((EFI_D_WARN, "SFB-MEM: %a(0x%lx, 0x%lx, 0x%lx): invalid parameter\n",
            Op, BaseAddress, Length, Requested));
    return EFI_INVALID_PARAMETER;
  }
  Ctx.SetMask = SetMask;
  Ctx.ClearMask = ClearMask;
  Status = SfbMaForEachRun (BaseAddress, Length, SfbMaChangeRun, &Ctx);
  if (Status == EFI_NO_MAPPING) {
    Status = EFI_UNSUPPORTED;               /* Set/Clear have no NO_MAPPING */
  }
  DEBUG ((EFI_D_INFO, "SFB-MEM: %a(0x%lx, 0x%lx, 0x%lx): %r\n",
          Op, BaseAddress, Length, Requested, Status));
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SfbMaSetMemoryAttributes (
  IN EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN UINT64                          Length,
  IN UINT64                          Attributes
  )
{
  return SfbMaChange ("SetMemoryAttributes", BaseAddress, Length, Attributes, 0);
}

STATIC
EFI_STATUS
EFIAPI
SfbMaClearMemoryAttributes (
  IN EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN UINT64                          Length,
  IN UINT64                          Attributes
  )
{
  return SfbMaChange ("ClearMemoryAttributes", BaseAddress, Length, 0, Attributes);
}

/* Embedded in the BDS image, so it lives as long as the boot session. */
STATIC EFI_MEMORY_ATTRIBUTE_PROTOCOL  mProtocol = {
  SfbMaGetMemoryAttributes,
  SfbMaSetMemoryAttributes,
  SfbMaClearMemoryAttributes
};

/* ------------------------------------------------------------------------ */
/* install                                                                   */
/* ------------------------------------------------------------------------ */

EFI_STATUS
SfbMemFixupInstallMemoryAttribute (
  VOID
  )
{
  EFI_STATUS   Status;
  VOID        *Existing = NULL;
  SFB_MA_LEAF  Probe;

  if (mHandle != NULL) {
    return EFI_SUCCESS;
  }

  /* a firmware that publishes the protocol owns its page tables */
  Status = gBS->LocateProtocol (&gEfiMemoryAttributeProtocolGuid, NULL, &Existing);
  if (!EFI_ERROR (Status) && Existing != NULL) {
    DEBUG ((EFI_D_INFO, "SFB-MEM: firmware publishes EFI_MEMORY_ATTRIBUTE_PROTOCOL\n"));
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status) || mCpu == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: no EFI_CPU_ARCH_PROTOCOL: %r\n", Status));
    mCpu = NULL;
    return EFI_NOT_FOUND;
  }
  if (gDS == NULL) {
    return EFI_NOT_FOUND;
  }

  /* which XN bit applies to code at this EL (VHE EL2 uses the EL1 layout) */
  if (ArmReadCurrentEL () == SFB_CURRENT_EL_EL2 && (ArmReadHcr () & SFB_HCR_E2H) == 0) {
    mXnBit = SFB_TT_XN;
  } else {
    mXnBit = SFB_TT_PXN;
  }

  /* the walker must understand this firmware's tables: probe our own code */
  Status = SfbMaLookup ((UINT64)(UINTN)SfbMemFixupInstallMemoryAttribute, &Probe);
  if (EFI_ERROR (Status) || (Probe.Prot & EFI_MEMORY_XP) != 0) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: translation table walk unusable (%r, prot 0x%lx)\n",
            Status, Probe.Prot));
    return EFI_UNSUPPORTED;
  }

  Status = gBS->InstallProtocolInterface (&mHandle,
                                          &gEfiMemoryAttributeProtocolGuid,
                                          EFI_NATIVE_INTERFACE, &mProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: EFI_MEMORY_ATTRIBUTE_PROTOCOL install: %r\n",
            Status));
    mHandle = NULL;
    return Status;
  }
  DEBUG ((EFI_D_INFO, "SFB-MEM: EFI_MEMORY_ATTRIBUTE_PROTOCOL installed (EL%d, %a)\n",
          (UINT32)(ArmReadCurrentEL () >> 2), mXnBit == SFB_TT_XN ? "XN" : "PXN"));
  return EFI_SUCCESS;
}
