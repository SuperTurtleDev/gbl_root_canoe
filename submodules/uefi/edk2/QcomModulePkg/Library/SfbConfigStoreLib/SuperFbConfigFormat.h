/*
 * Embedded FAT12/16/32 formatter for the efisp configuration container.
 *
 * The container is formatted on the device the first time it fails to mount,
 * so the build ships nothing but the BDS image and needs no host-side
 * mkfs.fat or mtools.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_CONFIG_FORMAT_H__
#define __SUPER_FB_CONFIG_FORMAT_H__

#include <Uefi.h>
#include <Protocol/BlockIo.h>

#define SFB_FAT_SECTOR_SIZE  512

/* Volume label written into the BPB and the root directory. */
#define SFB_FAT_VOLUME_LABEL  "SFBCONFIG  "

typedef struct {
  UINT8   FatType;            /* 12, 16 or 32 */
  UINT32  TotalSectors;
  UINT8   SectorsPerCluster;
  UINT16  ReservedSectors;
  UINT8   NumFats;
  UINT16  RootEntries;        /* 0 for FAT32 */
  UINT32  RootDirSectors;     /* 0 for FAT32 */
  UINT32  FatSectors;         /* per FAT */
  UINT32  DataStart;          /* first sector of cluster 2 */
  UINT32  Clusters;
} SFB_FAT_LAYOUT;

/*
 * Choose a FAT type and geometry for Sectors 512-byte sectors, following the
 * Microsoft FAT specification's cluster count rules: FAT12 below 4085
 * clusters, FAT16 below 65525, FAT32 above.
 */
EFI_STATUS
SfbFatPlan (
  IN  UINT64          Sectors,
  OUT SFB_FAT_LAYOUT  *Layout
  );

/* Encode the boot sector for Layout into Sector (512 bytes). */
VOID
SfbFatBuildBootSector (
  IN  CONST SFB_FAT_LAYOUT  *Layout,
  IN  UINT32                VolumeId,
  OUT UINT8                 *Sector
  );

/*
 * TRUE when Sector holds a FAT boot sector that fits a volume of Sectors
 * 512-byte sectors. The store only formats a container whose boot sector
 * fails this test, so an existing volume is never wiped just because the FAT
 * driver did not bind to it.
 */
BOOLEAN
SfbFatBootSectorUsable (
  IN CONST UINT8  *Sector,
  IN UINT64       Sectors
  );

/* Write a fresh, empty FAT volume over the whole of BlockIo and flush it. */
EFI_STATUS
SfbFatFormat (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  );

#endif /* __SUPER_FB_CONFIG_FORMAT_H__ */
