/*
 * Locating the efisp container in a GPT, and the bounded 512-byte Block I/O
 * that serves it.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_CONFIG_DISK_H__
#define __SUPER_FB_CONFIG_DISK_H__

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Library/SfbConfigStore.h>

#define SFB_CONFIG_SECTOR_SIZE  512

/* Smallest container worth formatting: room for a FAT12 volume and a file. */
#define SFB_CONFIG_MIN_BYTES    SIZE_64KB

/* Sanity bounds on a GPT header before its contents are believed. */
#define SFB_CONFIG_GPT_MAX_ENTRIES     1024
#define SFB_CONFIG_GPT_MIN_ENTRY_SIZE  128
#define SFB_CONFIG_GPT_MAX_ENTRY_SIZE  4096

typedef struct {
  /* Must stay first: the protocol pointer is the disk pointer. */
  EFI_BLOCK_IO_PROTOCOL  BlockIo;
  EFI_BLOCK_IO_MEDIA     Media;

  EFI_BLOCK_IO_PROTOCOL  *Parent;
  UINT32                 ParentBlockSize;
  UINT32                 ParentMediaId;
  /* Parent LBAs, both inclusive: the container is exactly [StartLba, EndLba]. */
  EFI_LBA                StartLba;
  EFI_LBA                EndLba;
  /* Container size in 512-byte sectors. */
  UINT64                 Sectors;

  VOID                   *Bounce;     /* one parent block */
  UINTN                  BouncePages;
} SFB_CONFIG_DISK;

/*
 * Container bounds from one GPT entry's inclusive [StartingLba, EndingLba]:
 * Start = StartingLba + 1 MiB / BlockSize, End = EndingLba. 1 MiB must be a
 * whole number of blocks, the entry must lie on the disk, and what is left
 * after the BDS reservation must hold at least SFB_CONFIG_MIN_BYTES.
 */
EFI_STATUS
SfbConfigContainerRange (
  IN  EFI_LBA  StartingLba,
  IN  EFI_LBA  EndingLba,
  IN  UINT32   BlockSize,
  IN  EFI_LBA  LastBlock,
  OUT EFI_LBA  *Start,
  OUT EFI_LBA  *End
  );

/*
 * Validate the primary GPT header held in Header (one block read from LBA 1)
 * and report where its entry array lives and how many bytes it spans.
 */
EFI_STATUS
SfbConfigGptCheckHeader (
  IN  CONST VOID  *Header,
  IN  UINT32      BlockSize,
  IN  EFI_LBA     LastBlock,
  OUT EFI_LBA     *EntriesLba,
  OUT UINTN       *EntriesBytes
  );

/*
 * Find the entry named Name in a validated header's entry array (Entries must
 * hold the EntriesBytes reported by SfbConfigGptCheckHeader) and return the
 * container bounds per SfbConfigContainerRange.
 */
EFI_STATUS
SfbConfigGptFindContainer (
  IN  CONST VOID    *Header,
  IN  CONST VOID    *Entries,
  IN  CONST CHAR16  *Name,
  IN  UINT32        BlockSize,
  IN  EFI_LBA       LastBlock,
  OUT EFI_LBA       *Start,
  OUT EFI_LBA       *End
  );

/*
 * Build a 512-byte Block I/O over Parent's [Start, End]. Every transfer is
 * bounds-checked against the container before any parent LBA is computed.
 */
EFI_STATUS
SfbConfigDiskInit (
  OUT SFB_CONFIG_DISK        *Disk,
  IN  EFI_BLOCK_IO_PROTOCOL  *Parent,
  IN  EFI_LBA                Start,
  IN  EFI_LBA                End
  );

/* Only after the Block I/O is withdrawn and no client holds it. */
VOID
SfbConfigDiskFree (
  IN OUT SFB_CONFIG_DISK  *Disk
  );

#endif /* __SUPER_FB_CONFIG_DISK_H__ */
