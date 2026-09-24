/* Bounded Block I/O over an initialized ext4 file's frozen extent map.
 * This code never allocates ext4 blocks or changes filesystem metadata.
 * SPDX-License-Identifier: BSD-3-Clause */
#include "SuperFbImageDisk.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC EFI_STATUS Ready (SFB_IMAGE_DISK *D, UINT32 Id, BOOLEAN Write)
{
  EFI_BLOCK_IO_MEDIA *Parent;
  if (!D->Active || !D->Media.MediaPresent)
    return EFI_NO_MEDIA;
  Parent = D->Map->Parent->Media;
  if (Parent == NULL || !Parent->MediaPresent)
    return EFI_NO_MEDIA;
  if (Id != D->Media.MediaId || Parent->MediaId != D->Map->MediaId ||
      Parent->BlockSize != D->ParentBlockSize)
    return EFI_MEDIA_CHANGED;
  if (Write && (D->Media.ReadOnly || Parent->ReadOnly))
    return EFI_WRITE_PROTECTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Transfer (SFB_IMAGE_DISK *D, UINT32 Id, EFI_LBA Lba, UINTN Size, VOID *Buffer,
                            BOOLEAN Write)
{
  EFI_STATUS Status = Ready (D, Id, Write);
  UINT64 Offset;
  UINTN Index = 0;
  UINT8 *Bytes = Buffer;
  if (EFI_ERROR (Status))
    return Status;
  if (Size == 0)
    return EFI_SUCCESS;
  if (Buffer == NULL)
    return EFI_INVALID_PARAMETER;
  if (Size % 512 != 0)
    return EFI_BAD_BUFFER_SIZE;
  if (Lba >= D->Map->Bytes / 512 || Size / 512 > D->Map->Bytes / 512 - Lba)
    return EFI_INVALID_PARAMETER;
  Offset = Lba * 512;
  while (Size != 0)
  {
    EXT4_IMAGE_RANGE *R;
    UINT64 Physical, ParentLba;
    UINTN InBlock, Chunk;
    while (Index < D->Map->Count &&
           Offset >= D->Map->Ranges[Index].Logical + D->Map->Ranges[Index].Bytes)
      Index++;
    if (Index == D->Map->Count)
      return EFI_VOLUME_CORRUPTED;
    R = &D->Map->Ranges[Index];
    if (Offset < R->Logical)
      return EFI_VOLUME_CORRUPTED;
    Physical = R->Physical + Offset - R->Logical;
    ParentLba = Physical / D->ParentBlockSize;
    InBlock = (UINTN)(Physical % D->ParentBlockSize);
    Chunk = D->ParentBlockSize - InBlock;
    if (Chunk > Size)
      Chunk = Size;
    if (Chunk > R->Logical + R->Bytes - Offset)
      Chunk = (UINTN)(R->Logical + R->Bytes - Offset);
    Status = Ready (D, Id, Write);
    if (EFI_ERROR (Status))
      return Status;
    if (!Write || InBlock != 0 || Chunk != D->ParentBlockSize)
    {
      Status = D->Map->Parent->ReadBlocks (D->Map->Parent, D->Map->MediaId, ParentLba,
                                           D->ParentBlockSize, D->Bounce);
      if (EFI_ERROR (Status))
        return Status;
    }
    if (Write)
    {
      CopyMem ((UINT8 *)D->Bounce + InBlock, Bytes, Chunk);
      Status = D->Map->Parent->WriteBlocks (D->Map->Parent, D->Map->MediaId, ParentLba,
                                            D->ParentBlockSize, D->Bounce);
      if (EFI_ERROR (Status))
        return Status;
    }
    else
      CopyMem (Bytes, (UINT8 *)D->Bounce + InBlock, Chunk);
    Offset += Chunk;
    Bytes += Chunk;
    Size -= Chunk;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Read (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id, EFI_LBA Lba, UINTN Size,
                               VOID *Buffer)
{
  return Transfer ((SFB_IMAGE_DISK *)This, Id, Lba, Size, Buffer, FALSE);
}
STATIC EFI_STATUS EFIAPI Write (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id, EFI_LBA Lba, UINTN Size,
                                VOID *Buffer)
{
  return Transfer ((SFB_IMAGE_DISK *)This, Id, Lba, Size, Buffer, TRUE);
}
STATIC EFI_STATUS EFIAPI Flush (EFI_BLOCK_IO_PROTOCOL *This)
{
  SFB_IMAGE_DISK *D = (SFB_IMAGE_DISK *)This;
  EFI_STATUS Status = Ready (D, D->Media.MediaId, FALSE);
  if (EFI_ERROR (Status))
    return Status;
  return D->Map->Parent->FlushBlocks (D->Map->Parent);
}
STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended)
{
  SFB_IMAGE_DISK *D = (SFB_IMAGE_DISK *)This;
  (VOID) Extended;
  return Ready (D, D->Media.MediaId, FALSE);
}
EFI_STATUS SfbImageDiskInit (SFB_IMAGE_DISK *D, EXT4_IMAGE_MAP *Map)
{
  EFI_BLOCK_IO_MEDIA *Media;
  UINT64 Total = 0, ParentBytes;
  UINTN I, J, Align;
  if (D == NULL || Map == NULL || Map->Parent == NULL || Map->Parent->Media == NULL ||
      !Ext4ImageSizeValid (Map->Bytes) || Map->Count == 0 || Map->Count > EXT4_IMAGE_MAX_EXTENTS || Map->Parent->ReadBlocks == NULL ||
      Map->Parent->WriteBlocks == NULL || Map->Parent->FlushBlocks == NULL)
    return EFI_INVALID_PARAMETER;
  Media = Map->Parent->Media;
  if (!Media->MediaPresent || Media->ReadOnly || Media->MediaId != Map->MediaId)
    return EFI_NOT_READY;
  if (Media->BlockSize < 512 || Media->BlockSize > 65536 ||
      (Media->BlockSize & (Media->BlockSize - 1)) != 0 || Media->LastBlock == MAX_UINT64 ||
      Media->LastBlock + 1 > MAX_UINT64 / Media->BlockSize)
    return EFI_UNSUPPORTED;
  Align = Media->IoAlign;
  if (Align > 65536 || (Align > 1 && (Align & (Align - 1)) != 0))
    return EFI_UNSUPPORTED;
  if (Align < EFI_PAGE_SIZE)
    Align = EFI_PAGE_SIZE;
  ParentBytes = (Media->LastBlock + 1) * Media->BlockSize;
  for (I = 0; I < Map->Count; I++)
  {
    EXT4_IMAGE_RANGE *R = &Map->Ranges[I];
    if (R->Logical != Total || R->Bytes == 0 || R->Bytes % 512 != 0 || R->Physical % 512 != 0 ||
        R->Bytes > Map->Bytes - Total || R->Physical >= ParentBytes ||
        R->Bytes > ParentBytes - R->Physical)
      return EFI_VOLUME_CORRUPTED;
    for (J = 0; J < I; J++)
      if (R->Physical < Map->Ranges[J].Physical + Map->Ranges[J].Bytes &&
          Map->Ranges[J].Physical < R->Physical + R->Bytes)
        return EFI_VOLUME_CORRUPTED;
    Total += R->Bytes;
  }
  if (Total != Map->Bytes)
    return EFI_VOLUME_CORRUPTED;
  ZeroMem (D, sizeof (*D));
  D->Map = Map;
  D->ParentBlockSize = Media->BlockSize;
  D->BouncePages = EFI_SIZE_TO_PAGES (Media->BlockSize);
  D->Bounce = AllocateAlignedPages (D->BouncePages, Align);
  if (D->Bounce == NULL)
    return EFI_OUT_OF_RESOURCES;
  D->Media.MediaId = Map->MediaId;
  D->Media.MediaPresent = TRUE;
  /* A standalone FAT USB LUN must mount as ordinary removable media. */
  D->Media.RemovableMedia = TRUE;
  D->Media.LogicalPartition = TRUE;
  D->Media.WriteCaching = TRUE;
  D->Media.BlockSize = 512;
  D->Media.LastBlock = D->Map->Bytes / 512 - 1;
  D->Media.IoAlign = 1;
  D->Block.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  D->Block.Media = &D->Media;
  D->Block.Reset = Reset;
  D->Block.ReadBlocks = Read;
  D->Block.WriteBlocks = Write;
  D->Block.FlushBlocks = Flush;
  D->Active = TRUE;
  return EFI_SUCCESS;
}
VOID SfbImageDiskDestroy (SFB_IMAGE_DISK *D)
{
  if (D == NULL)
    return;
  D->Active = FALSE;
  D->Media.MediaPresent = FALSE;
  if (D->Bounce != NULL)
    FreeAlignedPages (D->Bounce, D->BouncePages);
  D->Bounce = NULL;
}
