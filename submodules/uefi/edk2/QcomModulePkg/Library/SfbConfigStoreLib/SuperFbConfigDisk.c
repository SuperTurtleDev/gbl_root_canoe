/*
 * Locating the efisp container in a GPT, and the bounded 512-byte Block I/O
 * that serves it. See SuperFbConfigDisk.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbConfigDisk.h"

#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#define SFB_CONFIG_DISK_MEDIA_ID  0x53464243  /* 'SFBC' */

/* ---- GPT ----------------------------------------------------------------- */

EFI_STATUS
SfbConfigContainerRange (
  IN  EFI_LBA  StartingLba,
  IN  EFI_LBA  EndingLba,
  IN  UINT32   BlockSize,
  IN  EFI_LBA  LastBlock,
  OUT EFI_LBA  *Start,
  OUT EFI_LBA  *End
  )
{
  EFI_LBA  Reserved;

  if (Start == NULL || End == NULL || BlockSize < SFB_CONFIG_SECTOR_SIZE ||
      (BlockSize & (BlockSize - 1)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  /* The BDS reservation has to end on a block boundary. */
  if ((SFB_CONFIG_BDS_RESERVED_BYTES % BlockSize) != 0) {
    return EFI_UNSUPPORTED;
  }
  if (EndingLba < StartingLba || EndingLba > LastBlock) {
    return EFI_VOLUME_CORRUPTED;
  }

  Reserved = SFB_CONFIG_BDS_RESERVED_BYTES / BlockSize;
  if (EndingLba - StartingLba + 1 <= Reserved) {
    return EFI_VOLUME_FULL;
  }
  /* Inclusive bounds: the container is EndingLba - Start + 1 blocks long. */
  if ((EndingLba - (StartingLba + Reserved) + 1) * BlockSize <
      SFB_CONFIG_MIN_BYTES) {
    return EFI_VOLUME_FULL;
  }

  *Start = StartingLba + Reserved;
  *End = EndingLba;
  return EFI_SUCCESS;
}

EFI_STATUS
SfbConfigGptCheckHeader (
  IN  CONST VOID  *Header,
  IN  UINT32      BlockSize,
  IN  EFI_LBA     LastBlock,
  OUT EFI_LBA     *EntriesLba,
  OUT UINTN       *EntriesBytes
  )
{
  CONST EFI_PARTITION_TABLE_HEADER  *Hdr = Header;
  UINT8                             Copy[SFB_CONFIG_SECTOR_SIZE];
  UINT32                            Size;
  UINT64                            Bytes;
  UINT64                            Blocks;

  if (Header == NULL || EntriesLba == NULL || EntriesBytes == NULL ||
      BlockSize < SFB_CONFIG_SECTOR_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  Size = Hdr->Header.HeaderSize;
  if (Hdr->Header.Signature != EFI_PTAB_HEADER_ID ||
      Size < 92 || Size > sizeof (Copy) ||
      Hdr->MyLBA != PRIMARY_PART_HEADER_LBA ||
      Hdr->NumberOfPartitionEntries == 0 ||
      Hdr->NumberOfPartitionEntries > SFB_CONFIG_GPT_MAX_ENTRIES ||
      Hdr->SizeOfPartitionEntry < SFB_CONFIG_GPT_MIN_ENTRY_SIZE ||
      Hdr->SizeOfPartitionEntry > SFB_CONFIG_GPT_MAX_ENTRY_SIZE ||
      (Hdr->SizeOfPartitionEntry % 8) != 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  /* CRC over the header's own bytes with the CRC field zeroed. */
  CopyMem (Copy, Header, Size);
  ((EFI_PARTITION_TABLE_HEADER *)Copy)->Header.CRC32 = 0;
  if (CalculateCrc32 (Copy, Size) != Hdr->Header.CRC32) {
    return EFI_CRC_ERROR;
  }

  Bytes = (UINT64)Hdr->NumberOfPartitionEntries * Hdr->SizeOfPartitionEntry;
  Blocks = (Bytes + BlockSize - 1) / BlockSize;
  if (Hdr->PartitionEntryLBA < 2 || Hdr->PartitionEntryLBA > LastBlock ||
      Blocks > LastBlock - Hdr->PartitionEntryLBA + 1) {
    return EFI_VOLUME_CORRUPTED;
  }

  *EntriesLba = Hdr->PartitionEntryLBA;
  *EntriesBytes = (UINTN)Bytes;
  return EFI_SUCCESS;
}

EFI_STATUS
SfbConfigGptFindContainer (
  IN  CONST VOID    *Header,
  IN  CONST VOID    *Entries,
  IN  CONST CHAR16  *Name,
  IN  UINT32        BlockSize,
  IN  EFI_LBA       LastBlock,
  OUT EFI_LBA       *Start,
  OUT EFI_LBA       *End
  )
{
  CONST EFI_PARTITION_TABLE_HEADER  *Hdr = Header;
  CONST UINT8                       *Array = Entries;
  UINTN                             Index;
  UINTN                             NameChars;

  if (Header == NULL || Entries == NULL || Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (CalculateCrc32 ((VOID *)Entries,
                      (UINTN)Hdr->NumberOfPartitionEntries *
                        Hdr->SizeOfPartitionEntry) !=
      Hdr->PartitionEntryArrayCRC32) {
    return EFI_CRC_ERROR;
  }

  NameChars = StrLen (Name);
  if (NameChars == 0 || NameChars >= 36) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Hdr->NumberOfPartitionEntries; Index++) {
    EFI_PARTITION_ENTRY  Entry;

    /* Copy out: the array stride may exceed sizeof (EFI_PARTITION_ENTRY) and
     * nothing guarantees natural alignment of each record. */
    CopyMem (&Entry, Array + Index * Hdr->SizeOfPartitionEntry,
             sizeof (Entry));

    if (IsZeroGuid (&Entry.PartitionTypeGUID)) {
      continue;
    }
    /* PartitionName is fixed width and need not be NUL terminated. */
    if (CompareMem (Entry.PartitionName, Name, NameChars * sizeof (CHAR16)) != 0 ||
        Entry.PartitionName[NameChars] != L'\0') {
      continue;
    }

    return SfbConfigContainerRange (Entry.StartingLBA, Entry.EndingLBA,
                                    BlockSize, LastBlock, Start, End);
  }

  return EFI_NOT_FOUND;
}

/* ---- bounded Block I/O --------------------------------------------------- */

STATIC
EFI_STATUS
SfbConfigDiskReady (
  IN SFB_CONFIG_DISK  *Disk,
  IN UINT32           MediaId,
  IN BOOLEAN          Write
  )
{
  EFI_BLOCK_IO_MEDIA  *Parent = Disk->Parent->Media;

  if (!Disk->Media.MediaPresent || Parent == NULL || !Parent->MediaPresent) {
    return EFI_NO_MEDIA;
  }
  if (MediaId != Disk->Media.MediaId || Parent->MediaId != Disk->ParentMediaId ||
      Parent->BlockSize != Disk->ParentBlockSize) {
    return EFI_MEDIA_CHANGED;
  }
  if (Write && (Disk->Media.ReadOnly || Parent->ReadOnly)) {
    return EFI_WRITE_PROTECTED;
  }
  return EFI_SUCCESS;
}

/*
 * One parent-block transfer. ParentLba is re-checked against the container
 * bounds here, after all arithmetic, so a mapping bug can never turn into a
 * write outside [StartLba, EndLba].
 */
STATIC
EFI_STATUS
SfbConfigDiskParentIo (
  IN SFB_CONFIG_DISK  *Disk,
  IN BOOLEAN          Write,
  IN EFI_LBA          ParentLba,
  IN UINTN            Blocks,
  IN VOID             *Buffer
  )
{
  if (Blocks == 0 || ParentLba < Disk->StartLba || ParentLba > Disk->EndLba ||
      Blocks - 1 > Disk->EndLba - ParentLba) {
    return EFI_DEVICE_ERROR;
  }
  if (Write) {
    return Disk->Parent->WriteBlocks (Disk->Parent, Disk->ParentMediaId,
                                      ParentLba, Blocks * Disk->ParentBlockSize,
                                      Buffer);
  }
  return Disk->Parent->ReadBlocks (Disk->Parent, Disk->ParentMediaId,
                                   ParentLba, Blocks * Disk->ParentBlockSize,
                                   Buffer);
}

STATIC
EFI_STATUS
SfbConfigDiskTransfer (
  IN SFB_CONFIG_DISK  *Disk,
  IN UINT32           MediaId,
  IN EFI_LBA          Lba,
  IN UINTN            Size,
  IN OUT UINT8        *Buffer,
  IN BOOLEAN          Write
  )
{
  EFI_STATUS  Status;
  UINT64      Offset;
  UINTN       Align;
  UINT32      Pbs = Disk->ParentBlockSize;

  Status = SfbConfigDiskReady (Disk, MediaId, Write);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Size == 0) {
    return EFI_SUCCESS;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Size % SFB_CONFIG_SECTOR_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  /* Bounds in container sectors, before anything is mapped to the parent. */
  if (Lba >= Disk->Sectors ||
      Size / SFB_CONFIG_SECTOR_SIZE > Disk->Sectors - Lba) {
    return EFI_INVALID_PARAMETER;
  }

  Align = Disk->Parent->Media->IoAlign;
  Offset = Lba * SFB_CONFIG_SECTOR_SIZE;

  while (Size != 0) {
    EFI_LBA  ParentLba = Disk->StartLba + Offset / Pbs;
    UINTN    InBlock = (UINTN)(Offset % Pbs);
    UINTN    Chunk;

    if (InBlock == 0 && Size >= Pbs &&
        (Align <= 1 || ((UINTN)Buffer % Align) == 0)) {
      /* Whole, aligned parent blocks go straight through. */
      Chunk = Size - (Size % Pbs);
      Status = SfbConfigDiskParentIo (Disk, Write, ParentLba, Chunk / Pbs,
                                      Buffer);
    } else {
      /* Partial or misaligned: read-modify-write one parent block. */
      Chunk = Pbs - InBlock;
      if (Chunk > Size) {
        Chunk = Size;
      }
      Status = SfbConfigDiskParentIo (Disk, FALSE, ParentLba, 1, Disk->Bounce);
      if (!EFI_ERROR (Status)) {
        if (Write) {
          CopyMem ((UINT8 *)Disk->Bounce + InBlock, Buffer, Chunk);
          Status = SfbConfigDiskParentIo (Disk, TRUE, ParentLba, 1,
                                          Disk->Bounce);
        } else {
          CopyMem (Buffer, (UINT8 *)Disk->Bounce + InBlock, Chunk);
        }
      }
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset += Chunk;
    Buffer += Chunk;
    Size -= Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbConfigDiskRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  return SfbConfigDiskTransfer ((SFB_CONFIG_DISK *)This, MediaId, Lba,
                                BufferSize, Buffer, FALSE);
}

STATIC
EFI_STATUS
EFIAPI
SfbConfigDiskWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return SfbConfigDiskTransfer ((SFB_CONFIG_DISK *)This, MediaId, Lba,
                                BufferSize, Buffer, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
SfbConfigDiskFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  SFB_CONFIG_DISK  *Disk = (SFB_CONFIG_DISK *)This;
  EFI_STATUS       Status;

  Status = SfbConfigDiskReady (Disk, Disk->Media.MediaId, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Disk->Parent->FlushBlocks (Disk->Parent);
}

STATIC
EFI_STATUS
EFIAPI
SfbConfigDiskReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  SFB_CONFIG_DISK  *Disk = (SFB_CONFIG_DISK *)This;

  (VOID)ExtendedVerification;
  return SfbConfigDiskReady (Disk, Disk->Media.MediaId, FALSE);
}

EFI_STATUS
SfbConfigDiskInit (
  OUT SFB_CONFIG_DISK        *Disk,
  IN  EFI_BLOCK_IO_PROTOCOL  *Parent,
  IN  EFI_LBA                Start,
  IN  EFI_LBA                End
  )
{
  EFI_BLOCK_IO_MEDIA  *Media;
  UINTN               Align;

  if (Disk == NULL || Parent == NULL || Parent->Media == NULL ||
      Parent->ReadBlocks == NULL || Parent->WriteBlocks == NULL ||
      Parent->FlushBlocks == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Media = Parent->Media;
  if (!Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }
  if (Media->BlockSize < SFB_CONFIG_SECTOR_SIZE || Media->BlockSize > SIZE_64KB ||
      (Media->BlockSize & (Media->BlockSize - 1)) != 0) {
    return EFI_UNSUPPORTED;
  }
  if (End < Start || End > Media->LastBlock) {
    return EFI_INVALID_PARAMETER;
  }

  Align = Media->IoAlign;
  if (Align > SIZE_64KB || (Align > 1 && (Align & (Align - 1)) != 0)) {
    return EFI_UNSUPPORTED;
  }
  if (Align < EFI_PAGE_SIZE) {
    Align = EFI_PAGE_SIZE;
  }

  ZeroMem (Disk, sizeof (*Disk));
  Disk->Parent = Parent;
  Disk->ParentBlockSize = Media->BlockSize;
  Disk->ParentMediaId = Media->MediaId;
  Disk->StartLba = Start;
  Disk->EndLba = End;
  Disk->Sectors = (End - Start + 1) * (Media->BlockSize / SFB_CONFIG_SECTOR_SIZE);

  Disk->BouncePages = EFI_SIZE_TO_PAGES (Media->BlockSize);
  Disk->Bounce = AllocateAlignedPages (Disk->BouncePages, Align);
  if (Disk->Bounce == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Disk->Media.MediaId = SFB_CONFIG_DISK_MEDIA_ID;
  Disk->Media.MediaPresent = TRUE;
  /* A partitionless FAT volume mounts like removable "superfloppy" media; the
   * LogicalPartition flag keeps partition drivers off it and keeps it out of
   * the whole-disk numbering the boot entries use. */
  Disk->Media.RemovableMedia = TRUE;
  Disk->Media.LogicalPartition = TRUE;
  Disk->Media.ReadOnly = Media->ReadOnly;
  Disk->Media.WriteCaching = Media->WriteCaching;
  Disk->Media.BlockSize = SFB_CONFIG_SECTOR_SIZE;
  Disk->Media.LastBlock = Disk->Sectors - 1;
  Disk->Media.IoAlign = 1;   /* the disk bounces to the parent's alignment */

  Disk->BlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Disk->BlockIo.Media = &Disk->Media;
  Disk->BlockIo.Reset = SfbConfigDiskReset;
  Disk->BlockIo.ReadBlocks = SfbConfigDiskRead;
  Disk->BlockIo.WriteBlocks = SfbConfigDiskWrite;
  Disk->BlockIo.FlushBlocks = SfbConfigDiskFlush;

  return EFI_SUCCESS;
}

VOID
SfbConfigDiskFree (
  IN OUT SFB_CONFIG_DISK  *Disk
  )
{
  if (Disk == NULL) {
    return;
  }
  Disk->Media.MediaPresent = FALSE;
  if (Disk->Bounce != NULL) {
    FreeAlignedPages (Disk->Bounce, Disk->BouncePages);
    Disk->Bounce = NULL;
  }
}
