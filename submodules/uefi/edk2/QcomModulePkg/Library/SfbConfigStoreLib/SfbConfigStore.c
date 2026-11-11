/*
 * Persistent configuration store in the tail of the efisp partition. See
 * Library/SfbConfigStore.h for the layout.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Library/SfbConfigStore.h>

#include "SuperFbConfigDisk.h"
#include "SuperFbConfigFormat.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>

/* Vendor node naming the container under its parent disk's device path. */
STATIC CONST EFI_GUID  mSfbConfigNodeGuid = {
  0x5f1a3c27, 0x9b04, 0x4c6e, { 0xa1, 0x3d, 0x70, 0x2e, 0x4b, 0x95, 0xc8, 0x16 }
};

typedef struct {
  /* Where the container lives; resolved once per boot. */
  BOOLEAN                   Located;
  EFI_HANDLE                ParentHandle;
  EFI_BLOCK_IO_PROTOCOL     *Parent;
  EFI_LBA                   Start;
  EFI_LBA                   End;

  /* Published state. */
  BOOLEAN                   Mounted;
  SFB_CONFIG_DISK           Disk;
  EFI_HANDLE                Handle;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
} SFB_CONFIG_STORE;

STATIC SFB_CONFIG_STORE  mSfbConfig;

/* ---- locating the container ---------------------------------------------- */

STATIC
EFI_STATUS
SfbConfigReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  EFI_LBA                Lba,
  IN  UINTN                  Bytes,
  OUT VOID                   **Buffer,
  OUT UINTN                  *Pages
  )
{
  EFI_STATUS  Status;
  UINTN       BlockSize = BlockIo->Media->BlockSize;
  UINTN       Align = BlockIo->Media->IoAlign;

  Bytes = ((Bytes + BlockSize - 1) / BlockSize) * BlockSize;
  *Pages = EFI_SIZE_TO_PAGES (Bytes);
  *Buffer = AllocateAlignedPages (*Pages, (Align > EFI_PAGE_SIZE) ? Align
                                                                 : EFI_PAGE_SIZE);
  if (*Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, Lba, Bytes,
                                *Buffer);
  if (EFI_ERROR (Status)) {
    FreeAlignedPages (*Buffer, *Pages);
    *Buffer = NULL;
  }
  return Status;
}

/* Look for the efisp entry in one whole disk's primary GPT. */
STATIC
EFI_STATUS
SfbConfigProbeDisk (
  IN  EFI_BLOCK_IO_PROTOCOL  *Disk,
  OUT EFI_LBA                *Start,
  OUT EFI_LBA                *End
  )
{
  EFI_STATUS  Status;
  VOID        *Header = NULL;
  VOID        *Entries = NULL;
  UINTN       HeaderPages = 0;
  UINTN       EntryPages = 0;
  EFI_LBA     EntriesLba;
  UINTN       EntriesBytes;
  UINT32      BlockSize = Disk->Media->BlockSize;

  Status = SfbConfigReadBlocks (Disk, PRIMARY_PART_HEADER_LBA, BlockSize,
                                &Header, &HeaderPages);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SfbConfigGptCheckHeader (Header, BlockSize, Disk->Media->LastBlock,
                                    &EntriesLba, &EntriesBytes);
  if (!EFI_ERROR (Status)) {
    Status = SfbConfigReadBlocks (Disk, EntriesLba, EntriesBytes, &Entries,
                                  &EntryPages);
  }
  if (!EFI_ERROR (Status)) {
    Status = SfbConfigGptFindContainer (Header, Entries,
                                        SFB_CONFIG_PARTITION_NAME, BlockSize,
                                        Disk->Media->LastBlock, Start, End);
    FreeAlignedPages (Entries, EntryPages);
  }

  FreeAlignedPages (Header, HeaderPages);
  return Status;
}

STATIC
EFI_STATUS
SfbConfigLocate (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       Index;

  if (mSfbConfig.Located) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                                    &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *Disk = NULL;
    EFI_LBA                Start;
    EFI_LBA                End;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid,
                                        (VOID **)&Disk)) ||
        Disk == NULL || Disk->Media == NULL || Disk->Media->LogicalPartition ||
        !Disk->Media->MediaPresent ||
        Disk->Media->BlockSize < SFB_CONFIG_SECTOR_SIZE) {
      continue;
    }

    if (!EFI_ERROR (SfbConfigProbeDisk (Disk, &Start, &End))) {
      mSfbConfig.ParentHandle = Handles[Index];
      mSfbConfig.Parent = Disk;
      mSfbConfig.Start = Start;
      mSfbConfig.End = End;
      mSfbConfig.Located = TRUE;
      DEBUG ((EFI_D_INFO, "SFB: config container LBA %lu..%lu (block %u)\n",
              Start, End, Disk->Media->BlockSize));
      Status = EFI_SUCCESS;
      break;
    }
  }

  FreePool (Handles);
  return Status;
}

/* ---- publishing ---------------------------------------------------------- */

STATIC
EFI_STATUS
SfbConfigPublish (
  VOID
  )
{
  EFI_STATUS  Status;

  mSfbConfig.Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mSfbConfig.Handle,
                  &gEfiBlockIoProtocolGuid, &mSfbConfig.Disk.BlockIo,
                  &gEfiDevicePathProtocolGuid, mSfbConfig.Path,
                  NULL);
  if (EFI_ERROR (Status)) {
    mSfbConfig.Handle = NULL;
    return Status;
  }

  /* Bind Disk I/O and FAT to this one handle only. */
  (VOID)gBS->ConnectController (mSfbConfig.Handle, NULL, NULL, TRUE);
  return EFI_SUCCESS;
}

STATIC
VOID
SfbConfigUnpublish (
  VOID
  )
{
  if (mSfbConfig.Handle == NULL) {
    return;
  }

  (VOID)gBS->DisconnectController (mSfbConfig.Handle, NULL, NULL);
  (VOID)mSfbConfig.Disk.BlockIo.FlushBlocks (&mSfbConfig.Disk.BlockIo);
  (VOID)gBS->UninstallMultipleProtocolInterfaces (
               mSfbConfig.Handle,
               &gEfiBlockIoProtocolGuid, &mSfbConfig.Disk.BlockIo,
               &gEfiDevicePathProtocolGuid, mSfbConfig.Path,
               NULL);
  mSfbConfig.Handle = NULL;
}

STATIC
BOOLEAN
SfbConfigHasFs (
  VOID
  )
{
  VOID  *Fs = NULL;

  return (BOOLEAN)(mSfbConfig.Handle != NULL &&
                   !EFI_ERROR (gBS->HandleProtocol (
                                      mSfbConfig.Handle,
                                      &gEfiSimpleFileSystemProtocolGuid,
                                      &Fs)) &&
                   Fs != NULL);
}

/* TRUE when the container's first sector is a FAT boot sector that fits it. */
STATIC
BOOLEAN
SfbConfigLooksFormatted (
  VOID
  )
{
  UINT8  Sector[SFB_CONFIG_SECTOR_SIZE];

  if (EFI_ERROR (mSfbConfig.Disk.BlockIo.ReadBlocks (
                   &mSfbConfig.Disk.BlockIo, mSfbConfig.Disk.Media.MediaId, 0,
                   sizeof (Sector), Sector))) {
    /* Unreadable: certainly not something to format over. */
    return TRUE;
  }
  return SfbFatBootSectorUsable (Sector, mSfbConfig.Disk.Sectors);
}

STATIC
VOID
SfbConfigRelease (
  VOID
  )
{
  SfbConfigUnpublish ();
  SfbConfigDiskFree (&mSfbConfig.Disk);
  if (mSfbConfig.Path != NULL) {
    FreePool (mSfbConfig.Path);
    mSfbConfig.Path = NULL;
  }
  mSfbConfig.Mounted = FALSE;
}

EFI_STATUS
SfbConfigStoreMount (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *ParentPath;
  VENDOR_DEVICE_PATH        Node;

  if (mSfbConfig.Mounted) {
    return EFI_SUCCESS;
  }

  Status = SfbConfigLocate ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: no config container: %r\n", Status));
    return Status;
  }

  Status = SfbConfigDiskInit (&mSfbConfig.Disk, mSfbConfig.Parent,
                              mSfbConfig.Start, mSfbConfig.End);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: config disk init: %r\n", Status));
    return Status;
  }

  ParentPath = DevicePathFromHandle (mSfbConfig.ParentHandle);
  if (ParentPath == NULL) {
    SfbConfigRelease ();
    return EFI_NOT_READY;
  }
  ZeroMem (&Node, sizeof (Node));
  Node.Header.Type = MEDIA_DEVICE_PATH;
  Node.Header.SubType = MEDIA_VENDOR_DP;
  SetDevicePathNodeLength (&Node.Header, sizeof (Node));
  CopyGuid (&Node.Guid, &mSfbConfigNodeGuid);
  mSfbConfig.Path = AppendDevicePathNode (ParentPath, &Node.Header);
  if (mSfbConfig.Path == NULL) {
    SfbConfigRelease ();
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SfbConfigPublish ();
  if (!EFI_ERROR (Status) && !SfbConfigHasFs ()) {
    if (SfbConfigLooksFormatted ()) {
      /* A FAT volume is there but did not mount: leave it alone rather than
       * wipe data the FAT driver merely failed to bind to. */
      DEBUG ((EFI_D_ERROR, "SFB: config FAT present but not mountable\n"));
      Status = EFI_VOLUME_CORRUPTED;
    } else {
      DEBUG ((EFI_D_INFO, "SFB: formatting config container\n"));
      SfbConfigUnpublish ();
      Status = SfbFatFormat (&mSfbConfig.Disk.BlockIo);
      if (!EFI_ERROR (Status)) {
        Status = SfbConfigPublish ();
      }
      if (!EFI_ERROR (Status) && !SfbConfigHasFs ()) {
        Status = EFI_NOT_STARTED;
      }
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: config store unavailable: %r\n", Status));
    SfbConfigRelease ();
    return Status;
  }

  mSfbConfig.Mounted = TRUE;
  return EFI_SUCCESS;
}

VOID
SfbConfigStoreUnmount (
  VOID
  )
{
  if (!mSfbConfig.Mounted) {
    return;
  }
  SfbConfigRelease ();
}

EFI_STATUS
SfbConfigStoreGetFs (
  OUT EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  **Fs      OPTIONAL,
  OUT EFI_HANDLE                       *Volume  OPTIONAL
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Found = NULL;

  if (Fs != NULL) {
    *Fs = NULL;
  }
  if (Volume != NULL) {
    *Volume = NULL;
  }

  if (!mSfbConfig.Mounted ||
      EFI_ERROR (gBS->HandleProtocol (mSfbConfig.Handle,
                                      &gEfiSimpleFileSystemProtocolGuid,
                                      (VOID **)&Found)) ||
      Found == NULL) {
    return EFI_NOT_FOUND;
  }

  if (Fs != NULL) {
    *Fs = Found;
  }
  if (Volume != NULL) {
    *Volume = mSfbConfig.Handle;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
SfbConfigStoreExportUsb (
  IN SFB_CONFIG_EXPORT_SINK  Sink,
  IN VOID                    *Context
  )
{
  EFI_STATUS       Status;
  SFB_CONFIG_DISK  Export;

  if (Sink == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SfbConfigLocate ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* The host gets the container exclusively: nothing in firmware keeps a
   * cached FAT view of it while the session runs. */
  SfbConfigStoreUnmount ();

  Status = SfbConfigDiskInit (&Export, mSfbConfig.Parent, mSfbConfig.Start,
                              mSfbConfig.End);
  if (!EFI_ERROR (Status)) {
    Status = Sink (&Export.BlockIo, Context);
    (VOID)Export.BlockIo.FlushBlocks (&Export.BlockIo);
    SfbConfigDiskFree (&Export);
  }

  /* Whatever the host did, pick the container up again (formatting it if
   * the host left it unrecognisable). */
  (VOID)SfbConfigStoreMount ();
  return Status;
}
