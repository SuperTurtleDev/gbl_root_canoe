/*
 * Embedded FAT12/16/32 formatter. See SuperFbConfigFormat.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbConfigFormat.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

/* Cluster count limits from the Microsoft FAT specification. */
#define SFB_FAT12_MAX_CLUSTERS  4084
#define SFB_FAT16_MAX_CLUSTERS  65524
#define SFB_FAT32_MAX_CLUSTERS  0x0FFFFFF4

#define SFB_FAT_ROOT_ENTRIES    512
#define SFB_FAT_DIR_ENTRY_SIZE  32
#define SFB_FAT_MAX_SPC         64      /* 32 KiB clusters */
#define SFB_FAT_MEDIA           0xF8
#define SFB_FAT_ZERO_CHUNK      64      /* sectors per zeroing write */

/* FAT32 fixed placements. */
#define SFB_FAT32_FSINFO_SECTOR       1
#define SFB_FAT32_BACKUP_BOOT_SECTOR  6
#define SFB_FAT32_RESERVED_SECTORS    32
#define SFB_FAT32_ROOT_CLUSTER        2

/* ---- little-endian helpers ----------------------------------------------- */

STATIC
VOID
SfbFatPut16 (
  OUT UINT8   *At,
  IN  UINT32  Value
  )
{
  At[0] = (UINT8)Value;
  At[1] = (UINT8)(Value >> 8);
}

STATIC
VOID
SfbFatPut32 (
  OUT UINT8   *At,
  IN  UINT32  Value
  )
{
  SfbFatPut16 (At, Value);
  SfbFatPut16 (At + 2, Value >> 16);
}

STATIC
UINT32
SfbFatGet16 (
  IN CONST UINT8  *At
  )
{
  return (UINT32)At[0] | ((UINT32)At[1] << 8);
}

STATIC
UINT32
SfbFatGet32 (
  IN CONST UINT8  *At
  )
{
  return SfbFatGet16 (At) | (SfbFatGet16 (At + 2) << 16);
}

/* ---- geometry ------------------------------------------------------------ */

STATIC
UINT8
SfbFatTypeOfClusters (
  IN UINT32  Clusters
  )
{
  if (Clusters <= SFB_FAT12_MAX_CLUSTERS) {
    return 12;
  }
  if (Clusters <= SFB_FAT16_MAX_CLUSTERS) {
    return 16;
  }
  return 32;
}

/*
 * Lay out a volume of Type with Spc sectors per cluster. The FAT size is
 * iterated to a fixed point: a FAT just big enough for the clusters left once
 * the FATs themselves are carved out. Fails when the result does not land in
 * Type's cluster count range.
 */
STATIC
BOOLEAN
SfbFatTryLayout (
  IN  UINT32          Total,
  IN  UINT8           Type,
  IN  UINT8           Spc,
  OUT SFB_FAT_LAYOUT  *L
  )
{
  UINT32  Overhead;
  UINT32  Iteration;
  UINT64  FatBytes;
  UINT32  Need;

  ZeroMem (L, sizeof (*L));
  L->FatType = Type;
  L->TotalSectors = Total;
  L->SectorsPerCluster = Spc;
  L->NumFats = 2;
  L->ReservedSectors = (Type == 32) ? SFB_FAT32_RESERVED_SECTORS : 1;
  L->RootEntries = (Type == 32) ? 0 : SFB_FAT_ROOT_ENTRIES;
  L->RootDirSectors = (L->RootEntries * SFB_FAT_DIR_ENTRY_SIZE +
                       SFB_FAT_SECTOR_SIZE - 1) / SFB_FAT_SECTOR_SIZE;
  L->FatSectors = 1;

  for (Iteration = 0; Iteration < 64; Iteration++) {
    Overhead = L->ReservedSectors + L->NumFats * L->FatSectors +
               L->RootDirSectors;
    if (Overhead >= Total) {
      return FALSE;
    }
    L->Clusters = (Total - Overhead) / Spc;

    if (Type == 12) {
      FatBytes = ((UINT64)(L->Clusters + 2) * 3 + 1) / 2;
    } else {
      FatBytes = (UINT64)(L->Clusters + 2) * (Type / 8);
    }
    Need = (UINT32)((FatBytes + SFB_FAT_SECTOR_SIZE - 1) / SFB_FAT_SECTOR_SIZE);
    if (Need <= L->FatSectors) {
      break;
    }
    L->FatSectors = Need;
  }

  Overhead = L->ReservedSectors + L->NumFats * L->FatSectors + L->RootDirSectors;
  if (Overhead >= Total) {
    return FALSE;
  }
  L->Clusters = (Total - Overhead) / Spc;
  L->DataStart = Overhead;

  if (L->Clusters == 0 || L->Clusters > SFB_FAT32_MAX_CLUSTERS) {
    return FALSE;
  }
  return (BOOLEAN)(SfbFatTypeOfClusters (L->Clusters) == Type);
}

/* Sectors-per-cluster the Microsoft specification tables recommend. */
STATIC
UINT8
SfbFatPreferredSpc (
  IN UINT8   Type,
  IN UINT32  Total
  )
{
  if (Type == 16) {
    return (Total <= 32680) ? 2 : (Total <= 262144) ? 4 :
           (Total <= 524288) ? 8 : 16;
  }
  if (Type == 32) {
    return (Total <= 16777216) ? 8 : (Total <= 33554432) ? 16 :
           (Total <= 67108864) ? 32 : 64;
  }
  return 1;   /* FAT12: the smallest cluster that fits is the best one */
}

EFI_STATUS
SfbFatPlan (
  IN  UINT64          Sectors,
  OUT SFB_FAT_LAYOUT  *Layout
  )
{
  UINT32  Total;
  UINT8   Type;
  UINT8   Spc;

  if (Layout == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Sectors < SIZE_64KB / SFB_FAT_SECTOR_SIZE) {
    return EFI_VOLUME_FULL;
  }
  if (Sectors > MAX_UINT32) {
    return EFI_UNSUPPORTED;
  }
  Total = (UINT32)Sectors;

  /* Volume size picks the type (FAT16 from 4.1 MiB, FAT32 from 512 MiB, as
   * the specification recommends); a type that cannot be laid out at any
   * cluster size falls back to the next narrower one. */
  Type = (Total < 8400) ? 12 : (Total < 1048576) ? 16 : 32;

  for (; Type >= 12; Type = (UINT8)(Type == 32 ? 16 : Type - 4)) {
    if (SfbFatTryLayout (Total, Type, SfbFatPreferredSpc (Type, Total),
                         Layout)) {
      return EFI_SUCCESS;
    }
    for (Spc = 1; Spc <= SFB_FAT_MAX_SPC; Spc = (UINT8)(Spc * 2)) {
      if (SfbFatTryLayout (Total, Type, Spc, Layout)) {
        return EFI_SUCCESS;
      }
    }
    if (Type == 12) {
      break;
    }
  }

  return EFI_UNSUPPORTED;
}

/* ---- on-disk structures -------------------------------------------------- */

VOID
SfbFatBuildBootSector (
  IN  CONST SFB_FAT_LAYOUT  *L,
  IN  UINT32                VolumeId,
  OUT UINT8                 *Sector
  )
{
  UINT8  *Ext;

  ZeroMem (Sector, SFB_FAT_SECTOR_SIZE);

  /* Jump over the BPB to a boot stub that just halts; never executed here. */
  Sector[0] = 0xEB;
  Sector[1] = (L->FatType == 32) ? 0x58 : 0x3C;
  Sector[2] = 0x90;
  CopyMem (Sector + 3, "MSWIN4.1", 8);

  SfbFatPut16 (Sector + 11, SFB_FAT_SECTOR_SIZE);
  Sector[13] = L->SectorsPerCluster;
  SfbFatPut16 (Sector + 14, L->ReservedSectors);
  Sector[16] = L->NumFats;
  SfbFatPut16 (Sector + 17, L->RootEntries);
  if (L->FatType != 32 && L->TotalSectors < 0x10000) {
    SfbFatPut16 (Sector + 19, L->TotalSectors);
  } else {
    SfbFatPut32 (Sector + 32, L->TotalSectors);
  }
  Sector[21] = SFB_FAT_MEDIA;
  SfbFatPut16 (Sector + 24, 63);    /* sectors per track: informational */
  SfbFatPut16 (Sector + 26, 255);   /* heads: informational */

  if (L->FatType == 32) {
    SfbFatPut32 (Sector + 36, L->FatSectors);
    SfbFatPut32 (Sector + 44, SFB_FAT32_ROOT_CLUSTER);
    SfbFatPut16 (Sector + 48, SFB_FAT32_FSINFO_SECTOR);
    SfbFatPut16 (Sector + 50, SFB_FAT32_BACKUP_BOOT_SECTOR);
    Ext = Sector + 64;
  } else {
    SfbFatPut16 (Sector + 22, L->FatSectors);
    Ext = Sector + 36;
  }

  /* Extended BPB: drive number, signature, serial, label, type string. */
  Ext[0] = 0x80;
  Ext[2] = 0x29;
  SfbFatPut32 (Ext + 3, VolumeId);
  CopyMem (Ext + 7, SFB_FAT_VOLUME_LABEL, 11);
  CopyMem (Ext + 18,
           (L->FatType == 12) ? "FAT12   " :
           (L->FatType == 16) ? "FAT16   " : "FAT32   ",
           8);

  Sector[510] = 0x55;
  Sector[511] = 0xAA;
}

BOOLEAN
SfbFatBootSectorUsable (
  IN CONST UINT8  *Sector,
  IN UINT64       Sectors
  )
{
  UINT32  Spc;
  UINT32  Total;
  UINT32  FatSize;

  if (Sector == NULL || Sector[510] != 0x55 || Sector[511] != 0xAA) {
    return FALSE;
  }
  if (SfbFatGet16 (Sector + 11) != SFB_FAT_SECTOR_SIZE) {
    return FALSE;
  }
  Spc = Sector[13];
  if (Spc == 0 || (Spc & (Spc - 1)) != 0) {
    return FALSE;
  }
  if (SfbFatGet16 (Sector + 14) == 0 || Sector[16] == 0) {
    return FALSE;
  }
  Total = SfbFatGet16 (Sector + 19);
  if (Total == 0) {
    Total = SfbFatGet32 (Sector + 32);
  }
  FatSize = SfbFatGet16 (Sector + 22);
  if (FatSize == 0) {
    FatSize = SfbFatGet32 (Sector + 36);
  }
  return (BOOLEAN)(Total != 0 && Total <= Sectors && FatSize != 0);
}

STATIC
EFI_STATUS
SfbFatWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT32                 Sector,
  IN UINT32                 Count,
  IN VOID                   *Buffer
  )
{
  return BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, Sector,
                               (UINTN)Count * SFB_FAT_SECTOR_SIZE, Buffer);
}

EFI_STATUS
SfbFatFormat (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  EFI_STATUS      Status;
  SFB_FAT_LAYOUT  L;
  UINT8           *Buffer;
  UINT8           *Sector;
  UINT32          Clear;
  UINT32          At;
  UINT32          Count;
  UINT32          Fat;
  UINT32          RootSector;

  if (BlockIo == NULL || BlockIo->Media == NULL ||
      BlockIo->Media->BlockSize != SFB_FAT_SECTOR_SIZE) {
    return EFI_INVALID_PARAMETER;
  }
  if (BlockIo->Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  Status = SfbFatPlan (BlockIo->Media->LastBlock + 1, &L);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buffer = AllocateZeroPool (SFB_FAT_ZERO_CHUNK * SFB_FAT_SECTOR_SIZE);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Sector = Buffer;

  /* Clear reserved area, FATs and root directory (the root cluster on
   * FAT32) so no stale entry from earlier contents survives. */
  Clear = L.DataStart + ((L.FatType == 32) ? L.SectorsPerCluster : 0);
  for (At = 0; At < Clear; At += Count) {
    Count = MIN (Clear - At, (UINT32)SFB_FAT_ZERO_CHUNK);
    Status = SfbFatWrite (BlockIo, At, Count, Buffer);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  /* First sector of every FAT: media descriptor, end-of-chain markers, and
   * on FAT32 the end-of-chain of the single-cluster root directory. */
  ZeroMem (Sector, SFB_FAT_SECTOR_SIZE);
  if (L.FatType == 12) {
    Sector[0] = SFB_FAT_MEDIA;
    Sector[1] = 0xFF;
    Sector[2] = 0xFF;
  } else if (L.FatType == 16) {
    SfbFatPut16 (Sector, 0xFF00 | SFB_FAT_MEDIA);
    SfbFatPut16 (Sector + 2, 0xFFFF);
  } else {
    SfbFatPut32 (Sector, 0x0FFFFF00 | SFB_FAT_MEDIA);
    SfbFatPut32 (Sector + 4, 0x0FFFFFFF);
    SfbFatPut32 (Sector + 8, 0x0FFFFFFF);
  }
  for (Fat = 0; Fat < L.NumFats; Fat++) {
    Status = SfbFatWrite (BlockIo, L.ReservedSectors + Fat * L.FatSectors, 1,
                          Sector);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  /* Volume label entry, first in the root directory. */
  ZeroMem (Sector, SFB_FAT_SECTOR_SIZE);
  CopyMem (Sector, SFB_FAT_VOLUME_LABEL, 11);
  Sector[11] = 0x08;   /* ATTR_VOLUME_ID */
  RootSector = (L.FatType == 32) ? L.DataStart
                                 : L.ReservedSectors + L.NumFats * L.FatSectors;
  Status = SfbFatWrite (BlockIo, RootSector, 1, Sector);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (L.FatType == 32) {
    /* FSInfo and its backup: one cluster (the root) in use. */
    ZeroMem (Sector, SFB_FAT_SECTOR_SIZE);
    SfbFatPut32 (Sector, 0x41615252);
    SfbFatPut32 (Sector + 484, 0x61417272);
    SfbFatPut32 (Sector + 488, L.Clusters - 1);
    SfbFatPut32 (Sector + 492, SFB_FAT32_ROOT_CLUSTER + 1);
    SfbFatPut32 (Sector + 508, 0xAA550000);
    Status = SfbFatWrite (BlockIo, SFB_FAT32_FSINFO_SECTOR, 1, Sector);
    if (!EFI_ERROR (Status)) {
      Status = SfbFatWrite (BlockIo, SFB_FAT32_BACKUP_BOOT_SECTOR + 1, 1,
                            Sector);
    }
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  /* Boot sector last: until it lands, the volume does not look like FAT and
   * an interrupted format is simply retried on the next boot. */
  SfbFatBuildBootSector (&L, 0x53464200 ^ L.TotalSectors, Sector);
  if (L.FatType == 32) {
    Status = SfbFatWrite (BlockIo, SFB_FAT32_BACKUP_BOOT_SECTOR, 1, Sector);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }
  Status = SfbFatWrite (BlockIo, 0, 1, Sector);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = BlockIo->FlushBlocks (BlockIo);

Done:
  FreePool (Buffer);
  return Status;
}
