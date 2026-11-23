/*
 * GetMemoryMap merge: present the TZ-usable DDR the firmware map omits,
 * then apply the operator overrides from CONFIG\MEMCFG.
 *
 * The platform firmware describes only the memory its own boot stages
 * touched; the TZ RAM partition table (EFI_RAMPARTITION_PROTOCOL) is the
 * authority on what the application processor may use. An EFI-booted
 * kernel builds memblock from the UEFI map, so the BDS wraps
 * gBS->GetMemoryMap and returns a merged view built in this order:
 *
 *   release: RESERVED descriptors the firmware itself records as normal
 *            cacheable memory (EFI_MEMORY_WB) are split against the TZ
 *            list; the parts inside a TZ partition come back as
 *            CONVENTIONAL with their attributes kept, the rest stays
 *            RESERVED. A RESERVED descriptor without WB (the firmware
 *            registers memory it shares with other masters or uses as
 *            device-like storage as UC-only - SMEM, hypervisor and TZ
 *            runtime areas) is a statement that this is not RAM for the
 *            OS, whatever the TZ table says about the DDR behind it: such
 *            descriptors are passed through verbatim. RUNTIME_* / MMIO /
 *            ACPI entries and anything carrying EFI_MEMORY_RUNTIME are
 *            never touched either.
 *   add:     TZ ranges the firmware map does not describe at all become new
 *            CONVENTIONAL entries, appended after the original descriptors.
 *            The firmware allocator does not know them, so no later
 *            allocation can land there: the additions are stable.
 *   force:   every CONFIG\MEMCFG override is cut out of whatever the two
 *            steps above produced and appended with the configured type
 *            and attributes (SfbMemCfg.h). This is the operator's word:
 *            nothing is validated against TZ or the firmware.
 *
 * Recomputed on every call from the live map, so the view tracks firmware
 * descriptors that appear later. The MapKey is the firmware's own; between
 * a caller's GetMemoryMap and its ExitBootServices nothing here allocates
 * (the chained call lands in a pre-sized scratch buffer that is only ever
 * grown on size-query calls, where the caller allocates afterwards anyway).
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbMemFixup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIRamPartition.h>
#include <Protocol/SimpleFileSystem.h>

#include "SfbMemRegions.h"
#include "SfbMemCfg.h"

/* kernel is_usable_memory(): these types with EFI_MEMORY_WB are the ranges
 * the firmware already hands the kernel - never describe them twice */
#define SFB_MM_USABLE_TYPE(t)   ((t) == EfiLoaderCode ||          \
                                 (t) == EfiLoaderData ||          \
                                 (t) == EfiBootServicesCode ||    \
                                 (t) == EfiBootServicesData ||    \
                                 (t) == EfiConventionalMemory ||  \
                                 (t) == EfiACPIReclaimMemory ||   \
                                 (t) == EfiPersistentMemory)

/* attributes of the added ranges: the cacheability capabilities of normal
 * DDR; EFI_MEMORY_WB is what the kernel's is_usable_memory() requires to
 * treat CONVENTIONAL as RAM */
#define SFB_MM_ADD_ATTR         (EFI_MEMORY_UC | EFI_MEMORY_WC | \
                                 EFI_MEMORY_WT | EFI_MEMORY_WB)

/* a RESERVED descriptor eligible for the release split: firmware-declared
 * normal cacheable memory, not runtime */
#define SFB_MM_RELEASABLE(d)    ((d)->Type == EfiReservedMemoryType && \
                                 (d)->NumberOfPages != 0 && \
                                 ((d)->Attribute & EFI_MEMORY_WB) != 0 && \
                                 ((d)->Attribute & EFI_MEMORY_RUNTIME) == 0)

/* scratch headroom: the live map can grow between a size query and the
 * matching fill, and descriptors split into several pieces */
#define SFB_MM_SCRATCH_SLACK    (SFB_MEM_MAX_REGIONS * 2)

/* the assembled view; ~110 firmware descriptors on the reference device */
#define SFB_MM_VIEW_MAX         512

#define SFB_MM_CONFIG_PATH      L"CONFIG\\MEMCFG"
#define SFB_MM_CONFIG_MAX_BYTES SIZE_64KB

STATIC EFI_GET_MEMORY_MAP  mOrigGetMemoryMap;
STATIC VOID               *mScratch;
STATIC UINTN               mScratchSize;

STATIC SFB_MEM_REGION      mTz[SFB_MEM_MAX_REGIONS];
STATIC UINT32              mNumTz;

STATIC SFB_MEM_OVERRIDE    mOverrides[SFB_MEM_CFG_MAX];
STATIC UINT32              mNumOverrides;

/* per-call work areas; the hook may run at TPL_NOTIFY, never re-entrantly */
STATIC SFB_MEM_REGION      mFirmwareClaims[SFB_MEM_MAX_REGIONS * 2];
STATIC SFB_MEM_REGION      mUsableClaims[SFB_MEM_MAX_REGIONS * 2];
STATIC SFB_MEM_REGION      mAddition[SFB_MEM_MAX_REGIONS];
STATIC SFB_MEM_REGION      mMergeScratch[SFB_MEM_MAX_REGIONS * 2];
STATIC SFB_MEM_PIECE       mPieces[SFB_MEM_MAX_REGIONS + 2];
STATIC SFB_MEM_DESC        mView[SFB_MM_VIEW_MAX];
STATIC BOOLEAN             mViewOverflowLogged;

STATIC
EFI_MEMORY_DESCRIPTOR *
SfbMmDesc (
  IN VOID   *Map,
  IN UINTN   DescSize,
  IN UINTN   Index
  )
{
  return (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);
}

/* (Re)size the scratch buffer so the firmware map fits with headroom. Only
 * called from size-query paths: the caller allocates afterwards anyway, so
 * the map change this causes is harmless to it. */
STATIC
EFI_STATUS
SfbMmGrowScratch (
  IN UINTN  Needed,
  IN UINTN  DescSize
  )
{
  UINTN  Size = Needed * 2 + SFB_MM_SCRATCH_SLACK * DescSize;
  VOID  *New;

  if (mScratch != NULL && mScratchSize >= Needed + SFB_MM_SCRATCH_SLACK * DescSize) {
    return EFI_SUCCESS;
  }
  New = AllocatePool (Size);
  if (New == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (mScratch != NULL) {
    FreePool (mScratch);
  }
  mScratch = New;
  mScratchSize = Size;
  return EFI_SUCCESS;
}

/*
 * Classify the live map into the two claim lists and compute the additions.
 * Returns the number of additions. Pure except for the static work areas.
 */
STATIC
UINT32
SfbMmComputeAdditions (
  IN UINTN  OrigSize,
  IN UINTN  DescSize
  )
{
  UINT32      NumFirmware = 0;
  UINT32      NumUsable = 0;
  UINT32      NumAdd = 0;
  UINTN       i;
  EFI_STATUS  Status;

  if (mNumTz == 0) {
    return 0;
  }

  for (i = 0; i < OrigSize / DescSize; i++) {
    EFI_MEMORY_DESCRIPTOR  *D = SfbMmDesc (mScratch, DescSize, i);

    if (D->NumberOfPages == 0) {
      continue;
    }
    if (SFB_MM_USABLE_TYPE (D->Type) && (D->Attribute & EFI_MEMORY_WB) != 0) {
      if (NumUsable < ARRAY_SIZE (mUsableClaims)) {
        mUsableClaims[NumUsable].Base = D->PhysicalStart;
        mUsableClaims[NumUsable].Size = (UINT64)D->NumberOfPages * EFI_PAGE_SIZE;
        NumUsable++;
      }
    } else if (NumFirmware < ARRAY_SIZE (mFirmwareClaims)) {
      mFirmwareClaims[NumFirmware].Base = D->PhysicalStart;
      mFirmwareClaims[NumFirmware].Size = (UINT64)D->NumberOfPages * EFI_PAGE_SIZE;
      NumFirmware++;
    }
  }

  Status = SfbMemComputeMerge (mTz, mNumTz,
                               mFirmwareClaims, NumFirmware,
                               mUsableClaims, NumUsable,
                               mAddition, ARRAY_SIZE (mAddition),
                               mMergeScratch, ARRAY_SIZE (mMergeScratch),
                               &NumAdd);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: merge compute failed: %r (no additions)\n",
            Status));
    return 0;
  }
  return NumAdd;
}

STATIC
BOOLEAN
SfbMmViewPush (
  IN OUT UINT32  *Count,
  IN     UINT64   Base,
  IN     UINT64   Size,
  IN     UINT32   Type,
  IN     UINT64   Attr
  )
{
  if (*Count >= SFB_MM_VIEW_MAX) {
    return FALSE;
  }
  mView[*Count].Base = Base;
  mView[*Count].Size = Size;
  mView[*Count].Type = Type;
  mView[*Count].Attr = Attr;
  (*Count)++;
  return TRUE;
}

/*
 * Assemble the complete view in mView: original descriptors in order
 * (releasable RESERVED ones split against TZ), the additions, then the
 * overrides. The entry count is exact, so the size reported on a query is
 * exactly what the matching fill produces.
 *
 * @retval EFI_OUT_OF_RESOURCES  SFB_MM_VIEW_MAX exceeded; caller falls back
 *                               to the firmware's own map.
 */
STATIC
EFI_STATUS
SfbMmBuildView (
  IN  UINTN    OrigSize,
  IN  UINTN    Ds,
  IN  UINT32   NumAdd,
  OUT UINT32  *ViewCount
  )
{
  UINT32  n = 0;
  UINTN   i;
  UINT32  j;

  for (i = 0; i < OrigSize / Ds; i++) {
    EFI_MEMORY_DESCRIPTOR  *D = SfbMmDesc (mScratch, Ds, i);
    UINT64                  Size = (UINT64)D->NumberOfPages * EFI_PAGE_SIZE;
    INT32                   np, k;

    if (!SFB_MM_RELEASABLE (D)) {
      if (!SfbMmViewPush (&n, D->PhysicalStart, Size, D->Type, D->Attribute)) {
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }
    np = SfbMemSplitVsTz (D->PhysicalStart, Size, mTz, mNumTz,
                          mPieces, ARRAY_SIZE (mPieces));
    if (np <= 0) {                          /* capacity guard: keep verbatim */
      if (!SfbMmViewPush (&n, D->PhysicalStart, Size, D->Type, D->Attribute)) {
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }
    for (k = 0; k < np; k++) {
      if (!SfbMmViewPush (&n, mPieces[k].Base, mPieces[k].Size,
                          mPieces[k].InTz ? EfiConventionalMemory : D->Type,
                          D->Attribute)) {
        return EFI_OUT_OF_RESOURCES;
      }
    }
  }

  for (j = 0; j < NumAdd; j++) {
    if (!SfbMmViewPush (&n, mAddition[j].Base, mAddition[j].Size,
                        EfiConventionalMemory, SFB_MM_ADD_ATTR)) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  for (j = 0; j < mNumOverrides; j++) {
    if (EFI_ERROR (SfbMemApplyOverride (mView, &n, SFB_MM_VIEW_MAX,
                                        &mOverrides[j].Desc))) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  *ViewCount = n;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbMmHookedGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  IN OUT EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  OUT UINTN                     *MapKey,
  OUT UINTN                     *DescriptorSize,
  OUT UINT32                    *DescriptorVersion
  )
{
  EFI_STATUS  Status;
  UINTN       OrigSize;
  UINTN       Ds;
  UINTN       TotalSize;
  UINT32      NumAdd;
  UINT32      ViewCount = 0;
  BOOLEAN     Query;

  if (MemoryMapSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Query = (MemoryMap == NULL);

  /* always chain through the scratch buffer: the caller's buffer only fits
   * the merged size we reported, the original fills less */
  OrigSize = mScratchSize;
  Status = mOrigGetMemoryMap (&OrigSize, mScratch, MapKey, DescriptorSize,
                              DescriptorVersion);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Ds = *DescriptorSize;
    if (Query) {
      if (EFI_ERROR (SfbMmGrowScratch (OrigSize, Ds))) {
        return EFI_OUT_OF_RESOURCES;
      }
      OrigSize = mScratchSize;
      Status = mOrigGetMemoryMap (&OrigSize, mScratch, MapKey, DescriptorSize,
                                  DescriptorVersion);
    } else {
      /* a fill that outgrew the scratch: report a generous size so the
       * caller's retry (query + fill) lands in a grown buffer */
      *MemoryMapSize = OrigSize * 2 + SFB_MM_SCRATCH_SLACK * Ds;
      DEBUG ((EFI_D_WARN, "SFB-MEM: scratch outgrown on fill (%lu), retry\n",
              (UINT64)OrigSize));
      return EFI_BUFFER_TOO_SMALL;
    }
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Ds = *DescriptorSize;

  NumAdd = SfbMmComputeAdditions (OrigSize, Ds);
  Status = SfbMmBuildView (OrigSize, Ds, NumAdd, &ViewCount);
  if (EFI_ERROR (Status)) {
    /* view too large: hand out the firmware's own map rather than fail */
    if (!mViewOverflowLogged) {
      DEBUG ((EFI_D_ERROR, "SFB-MEM: view exceeds %u entries, merge bypassed\n",
              SFB_MM_VIEW_MAX));
      mViewOverflowLogged = TRUE;
    }
    if (Query || *MemoryMapSize < OrigSize) {
      *MemoryMapSize = OrigSize;
      return EFI_BUFFER_TOO_SMALL;
    }
    CopyMem (MemoryMap, mScratch, OrigSize);
    *MemoryMapSize = OrigSize;
    return EFI_SUCCESS;
  }

  TotalSize = (UINTN)ViewCount * Ds;
  if (Query || *MemoryMapSize < TotalSize) {
    *MemoryMapSize = TotalSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  {
    EFI_MEMORY_DESCRIPTOR  *Out = MemoryMap;
    UINT32                  i;

    for (i = 0; i < ViewCount; i++) {
      ZeroMem (Out, Ds);
      Out->Type          = mView[i].Type;
      Out->PhysicalStart = mView[i].Base;
      Out->VirtualStart  = 0;
      Out->NumberOfPages = mView[i].Size / EFI_PAGE_SIZE;
      Out->Attribute     = mView[i].Attr;
      Out = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Out + Ds);
    }
    *MemoryMapSize = TotalSize;
  }
  return EFI_SUCCESS;
}

/* Take over gBS->GetMemoryMap. Idempotent. */
STATIC
EFI_STATUS
SfbMmInstallHook (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Size = 0;
  UINTN       Key = 0;
  UINTN       DescSize = 0;
  UINT32      DescVer = 0;

  if (mOrigGetMemoryMap != NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescSize, &DescVer);
  if (Status != EFI_BUFFER_TOO_SMALL || DescSize == 0) {
    return EFI_DEVICE_ERROR;
  }
  Status = SfbMmGrowScratch (Size, DescSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mOrigGetMemoryMap = gBS->GetMemoryMap;
  gBS->GetMemoryMap = SfbMmHookedGetMemoryMap;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SfbMmLoadTzPartitions (
  VOID
  )
{
  EFI_STATUS                  Status;
  EFI_RAMPARTITION_PROTOCOL  *RamPart = NULL;
  RamPartitionEntry          *Parts;
  UINT32                      Num = 0;
  UINT32                      i;

  Status = gBS->LocateProtocol (&gEfiRamPartitionProtocolGuid, NULL,
                                (VOID **)&RamPart);
  if (EFI_ERROR (Status) || RamPart == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: no EFI_RAMPARTITION_PROTOCOL: %r\n", Status));
    return EFI_NOT_FOUND;
  }
  Status = RamPart->GetRamPartitions (RamPart, NULL, &Num);
  if (Status != EFI_BUFFER_TOO_SMALL || Num == 0 || Num > SFB_MEM_MAX_REGIONS) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: GetRamPartitions size query: %r (%u)\n",
            Status, Num));
    return EFI_PROTOCOL_ERROR;
  }
  Parts = AllocateZeroPool (Num * sizeof (*Parts));
  if (Parts == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = RamPart->GetRamPartitions (RamPart, Parts, &Num);
  if (EFI_ERROR (Status) || Num == 0) {
    FreePool (Parts);
    return EFI_PROTOCOL_ERROR;
  }
  for (i = 0; i < Num; i++) {
    mTz[i].Base = Parts[i].Base;
    mTz[i].Size = Parts[i].AvailableLength;
  }
  FreePool (Parts);
  mNumTz = SfbMemSortAndCombine (mTz, Num);
  return EFI_SUCCESS;
}

EFI_STATUS
SfbMemFixupInstallMemmapMerge (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINTN       Key = 0;
  UINTN       DescSize = 0;
  UINT32      DescVer = 0;
  UINT32      NumAdd;
  UINT32      j;
  UINT64      Total = 0;

  if (mNumTz != 0) {
    return EFI_SUCCESS;
  }

  Status = SfbMmLoadTzPartitions ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SfbMmInstallHook ();
  if (EFI_ERROR (Status)) {
    mNumTz = 0;
    return Status;
  }

  /* report what the merge adds right now */
  Size = mScratchSize;
  Status = mOrigGetMemoryMap (&Size, mScratch, &Key, &DescSize, &DescVer);
  if (!EFI_ERROR (Status)) {
    NumAdd = SfbMmComputeAdditions (Size, DescSize);
    for (j = 0; j < NumAdd; j++) {
      Total += mAddition[j].Size;
      DEBUG ((EFI_D_INFO, "SFB-MEM: + 0x%016lx-0x%016lx\n", mAddition[j].Base,
              mAddition[j].Base + mAddition[j].Size - 1));
    }
    DEBUG ((EFI_D_INFO, "SFB-MEM: GetMemoryMap merge installed: %u TZ partition(s), "
            "%u addition(s), %lu MiB\n", mNumTz, NumAdd, Total >> 20));
  }
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* CONFIG\MEMCFG                                                             */
/* ------------------------------------------------------------------------ */

/* Read CONFIG\MEMCFG into a pool buffer. Any failure returns NULL: the
 * caller treats that as an empty table. */
STATIC
CHAR8 *
SfbMmReadConfigFile (
  OUT UINTN  *Size
  )
{
  EFI_STATUS                        Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *File = NULL;
  EFI_FILE_INFO                    *Info = NULL;
  UINTN                             InfoSize;
  CHAR8                            *Buf = NULL;
  UINTN                             ReadSize;

  *Size = 0;
  Status = SfbConfigStoreGetFs (&Fs, NULL);
  if (EFI_ERROR (Status) || Fs == NULL) {
    return NULL;
  }
  if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || Root == NULL) {
    return NULL;
  }
  Status = Root->Open (Root, &File, SFB_MM_CONFIG_PATH, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status) || File == NULL) {
    return NULL;
  }

  InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
  Info = AllocatePool (InfoSize);
  if (Info != NULL &&
      !EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info)) &&
      Info->FileSize != 0 && Info->FileSize <= SFB_MM_CONFIG_MAX_BYTES) {
    ReadSize = (UINTN)Info->FileSize;
    Buf = AllocatePool (ReadSize + 1);
    if (Buf != NULL) {
      if (EFI_ERROR (File->Read (File, &ReadSize, Buf))) {
        FreePool (Buf);
        Buf = NULL;
      } else {
        Buf[ReadSize] = '\0';
        *Size = ReadSize;
      }
    }
  }
  if (Info != NULL) {
    FreePool (Info);
  }
  File->Close (File);
  return Buf;
}

EFI_STATUS
SfbMemFixupLoadConfig (
  VOID
  )
{
  CHAR8      *Text;
  UINTN       Len;
  UINT32      Rejected = 0;
  UINT32      i;
  EFI_STATUS  Status = EFI_SUCCESS;

  mNumOverrides = 0;

  Text = SfbMmReadConfigFile (&Len);
  if (Text == NULL) {
    DEBUG ((EFI_D_INFO, "SFB-MEM: no CONFIG\\MEMCFG, override table empty\n"));
    return EFI_SUCCESS;
  }

  SfbMemParseConfig (Text, Len, mOverrides, ARRAY_SIZE (mOverrides),
                     &mNumOverrides, &Rejected);
  FreePool (Text);

  for (i = 0; i < mNumOverrides; i++) {
    DEBUG ((EFI_D_INFO, "SFB-MEM: MEMCFG %a: 0x%016lx+0x%lx %a attr 0x%lx\n",
            mOverrides[i].Name, mOverrides[i].Desc.Base, mOverrides[i].Desc.Size,
            SfbMemTypeName (mOverrides[i].Desc.Type), mOverrides[i].Desc.Attr));
  }
  if (Rejected != 0) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: MEMCFG: %u invalid line(s) ignored\n", Rejected));
  }

  /* overrides without the TZ merge (no RAM partition protocol) still need
   * the hook */
  if (mNumOverrides != 0 && mOrigGetMemoryMap == NULL) {
    Status = SfbMmInstallHook ();
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB-MEM: MEMCFG: GetMemoryMap hook: %r, table dropped\n",
              Status));
      mNumOverrides = 0;
    }
  }
  DEBUG ((EFI_D_INFO, "SFB-MEM: MEMCFG: %u override(s) active\n", mNumOverrides));
  return Status;
}
