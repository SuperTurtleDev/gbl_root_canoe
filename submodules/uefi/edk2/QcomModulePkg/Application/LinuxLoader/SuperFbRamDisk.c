/*
 * RAM-backed staged disks. See SuperFbRamDisk.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbRamDisk.h"
#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

/* One staged image at a time keeps the failure story simple; unstage frees
 * it. A second stage replaces nothing - it fails busy until unstaged. */
#define SFB_RAM_MAX_BYTES   (512U * 1024U * 1024U)
#define SFB_RAM_BLOCK_BYTES 512

/* The container image-disk device-path node GUID (see SuperFbFat.c): staged
 * disks publish under it so the grouped browser classes them as Virtual
 * Disks alongside efisp.fat containers. */
STATIC CONST EFI_GUID mSfbRamNodeGuid = {
  0xf1086281, 0xc184, 0x47f7, { 0xbb, 0xae, 0x30, 0x61, 0x1f, 0x90, 0xe2, 0xa4 }
};

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  BlkIo;
  EFI_BLOCK_IO_MEDIA     Media;
  VOID                   *Buffer;   /* owns the image copy */
  UINT64                 Blocks;
  EFI_HANDLE             Disk;      /* published handle */
  EFI_DEVICE_PATH_PROTOCOL *Path;
} SFB_RAM_DISK;

STATIC SFB_RAM_DISK  *mSfbRamDisk = NULL;

STATIC
EFI_STATUS
EFIAPI
SfbRamReadBlocks (IN EFI_BLOCK_IO_PROTOCOL *This,
                  IN UINT32                MediaId,
                  IN EFI_LBA               Lba,
                  IN UINTN                 BufferSize,
                  OUT VOID                 *Buffer)
{
  SFB_RAM_DISK  *Ram = (SFB_RAM_DISK *)This;

  if (MediaId != Ram->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (Buffer == NULL || BufferSize == 0 ||
      (BufferSize % SFB_RAM_BLOCK_BYTES) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((UINT64)Lba + BufferSize / SFB_RAM_BLOCK_BYTES > Ram->Blocks) {
    return EFI_INVALID_PARAMETER;
  }
  CopyMem (Buffer, (UINT8 *)Ram->Buffer +
           (UINTN)Lba * SFB_RAM_BLOCK_BYTES, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbRamWriteBlocks (IN EFI_BLOCK_IO_PROTOCOL *This,
                   IN UINT32                MediaId,
                   IN EFI_LBA               Lba,
                   IN UINTN                 BufferSize,
                   IN VOID                  *Buffer)
{
  SFB_RAM_DISK  *Ram = (SFB_RAM_DISK *)This;

  if (MediaId != Ram->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (Buffer == NULL || BufferSize == 0 ||
      (BufferSize % SFB_RAM_BLOCK_BYTES) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((UINT64)Lba + BufferSize / SFB_RAM_BLOCK_BYTES > Ram->Blocks) {
    return EFI_INVALID_PARAMETER;
  }
  CopyMem ((UINT8 *)Ram->Buffer + (UINTN)Lba * SFB_RAM_BLOCK_BYTES,
           Buffer, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbRamReset (IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN ExtendedVerification)
{
  (VOID)This;
  (VOID)ExtendedVerification;
  return EFI_SUCCESS;
}

/* Clean text path: ensure one leading backslash, normalise '/' to '\'. */
STATIC
BOOLEAN
SfbRamCleanPath (IN CONST CHAR16 *In, OUT CHAR16 *Out, IN UINTN OutChars)
{
  UINTN  Count = 0;

  while (*In == L'\\' || *In == L'/') {
    In++;
  }
  if (*In == L'\0' || OutChars < 2) {
    return FALSE;
  }
  Out[Count++] = L'\\';
  for (; *In != L'\0' && Count + 1 < OutChars; In++) {
    CHAR16  Ch = (*In == L'/') ? L'\\' : *In;

    if (Ch < 0x20) {
      continue;
    }
    Out[Count++] = Ch;
  }
  while (Count > 1 && Out[Count - 1] == L'\\') {
    Count--;
  }
  Out[Count] = L'\0';
  return (BOOLEAN)(Count > 1);
}

EFI_STATUS
SfbRamStageImage (IN CONST CHAR16 *Path,
                  OUT CHAR16      *OutLabel OPTIONAL,
                  IN UINTN        LabelChars)
{
  EFI_STATUS     Status;
  CHAR16         Clean[SFB_PATH_CHARS];
  EFI_HANDLE     *All = NULL;
  UINTN          Count = 0;
  UINTN          Index;
  EFI_FILE_PROTOCOL *Root = NULL;
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_FILE_INFO  *Info = NULL;
  UINTN          InfoSize = 0;
  UINT64         Bytes;
  SFB_RAM_DISK   *Ram = NULL;
  VENDOR_DEVICE_PATH  Node;
  EFI_DEVICE_PATH_PROTOCOL *End;
  UINTN          PathBytes;

  if (OutLabel != NULL && LabelChars > 0) {
    OutLabel[0] = L'\0';
  }
  if (Path == NULL || !SfbRamCleanPath (Path, Clean, ARRAY_SIZE (Clean))) {
    return EFI_INVALID_PARAMETER;
  }
  if (mSfbRamDisk != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol,
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &All);
  if (EFI_ERROR (Status) || All == NULL) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  for (Index = 0; Index < Count; Index++) {
    if (!EFI_ERROR (SfbOpenVolumeRoot (All[Index], &Root)) && Root != NULL &&
        SfbFileExists (Root, Clean)) {
      break;
    }
    if (Root != NULL) {
      Root->Close (Root);
      Root = NULL;
    }
  }
  if (Root == NULL) {
    FreePool (All);
    return EFI_NOT_FOUND;
  }

  Status = Root->Open (Root, &File, Clean, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || File == NULL) {
    Root->Close (Root);
    FreePool (All);
    return Status;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    Root->Close (Root);
    FreePool (All);
    return EFI_DEVICE_ERROR;
  }
  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL ||
      EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info))) {
    if (Info != NULL) {
      FreePool (Info);
    }
    File->Close (File);
    Root->Close (Root);
    FreePool (All);
    return EFI_DEVICE_ERROR;
  }
  Bytes = Info->FileSize;
  FreePool (Info);

  if (Bytes == 0 || Bytes > SFB_RAM_MAX_BYTES ||
      (Bytes % SFB_RAM_BLOCK_BYTES) != 0) {
    DEBUG ((EFI_D_ERROR, "SFB: ram stage rejected, %Lu bytes\n", Bytes));
    File->Close (File);
    Root->Close (Root);
    FreePool (All);
    return EFI_UNSUPPORTED;
  }

  if (OutLabel != NULL && LabelChars > 0) {
    SfbGetVolumeLabel (Root, OutLabel, LabelChars);
  }

  Ram = AllocateZeroPool (sizeof (*Ram) + (UINTN)Bytes);
  if (Ram == NULL) {
    File->Close (File);
    Root->Close (Root);
    FreePool (All);
    return EFI_OUT_OF_RESOURCES;
  }
  Ram->Buffer = (VOID *)((UINT8 *)Ram + sizeof (*Ram));
  Ram->Blocks = Bytes / SFB_RAM_BLOCK_BYTES;

  {
    UINTN  Got = (UINTN)Bytes;
    Status = File->Read (File, &Got, Ram->Buffer);
    File->Close (File);
    Root->Close (Root);
    FreePool (All);
    if (EFI_ERROR (Status) || Got != (UINTN)Bytes) {
      FreePool (Ram);
      return EFI_DEVICE_ERROR;
    }
  }

  Ram->Media.MediaId = 0x52414D44;  /* 'RAMD' */
  Ram->Media.RemovableMedia = TRUE;
  Ram->Media.MediaPresent = TRUE;
  Ram->Media.LogicalPartition = TRUE;
  Ram->Media.ReadOnly = FALSE;
  Ram->Media.WriteCaching = FALSE;
  Ram->Media.BlockSize = SFB_RAM_BLOCK_BYTES;
  Ram->Media.LastBlock = Ram->Blocks - 1;
  Ram->Media.IoAlign = 1;

  Ram->BlkIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Ram->BlkIo.Media = &Ram->Media;
  Ram->BlkIo.Reset = SfbRamReset;
  Ram->BlkIo.ReadBlocks = SfbRamReadBlocks;
  Ram->BlkIo.WriteBlocks = SfbRamWriteBlocks;
  Ram->BlkIo.FlushBlocks = NULL;

  /* Container-class device path: the grouped browser files this volume under
   * Virtual Disks without knowing anything about staging. */
  ZeroMem (&Node, sizeof (Node));
  Node.Header.Type = MEDIA_DEVICE_PATH;
  Node.Header.SubType = MEDIA_VENDOR_DP;
  SetDevicePathNodeLength (&Node.Header, sizeof (Node));
  CopyMem (&Node.Guid, &mSfbRamNodeGuid, sizeof (Node.Guid));
  PathBytes = sizeof (Node) + sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Ram->Path = AllocateZeroPool (PathBytes);
  if (Ram->Path == NULL) {
    FreePool (Ram);
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Ram->Path, &Node, sizeof (Node));
  End = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Ram->Path + sizeof (Node));
  SetDevicePathEndNode (End);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Ram->Disk,
                  &gEfiBlockIoProtocolGuid, &Ram->BlkIo,
                  &gEfiDevicePathProtocolGuid, Ram->Path,
                  NULL);
  if (EFI_ERROR (Status)) {
    FreePool (Ram->Path);
    FreePool (Ram);
    return Status;
  }

  /* Bind the filesystem to this one handle only. */
  (VOID)gBS->ConnectController (Ram->Disk, NULL, NULL, TRUE);

  mSfbRamDisk = Ram;
  DEBUG ((EFI_D_INFO, "SFB: ram disk staged: %Lu bytes, '%s'\n",
          Bytes, Clean));
  return EFI_SUCCESS;
}

EFI_STATUS
SfbRamUnstageAll (VOID)
{
  EFI_STATUS  Status;

  if (mSfbRamDisk == NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->DisconnectController (mSfbRamDisk->Disk, NULL, NULL);
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
    return Status;
  }
  Status = gBS->UninstallMultipleProtocolInterfaces (
             mSfbRamDisk->Disk,
             &gEfiBlockIoProtocolGuid, &mSfbRamDisk->BlkIo,
             &gEfiDevicePathProtocolGuid, mSfbRamDisk->Path,
             NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  FreePool (mSfbRamDisk->Path);
  FreePool (mSfbRamDisk);
  mSfbRamDisk = NULL;
  return EFI_SUCCESS;
}

BOOLEAN
SfbRamAnyStaged (VOID)
{
  return (BOOLEAN)(mSfbRamDisk != NULL);
}
