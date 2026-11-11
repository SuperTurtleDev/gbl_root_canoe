/*
 * Embedded FAT stack for the super-fastboot boot menu.
 *
 * EnhancedFatDxe, DiskIoDxe and the English Unicode Collation driver are linked
 * into this application as static libraries. Their entry points are invoked
 * here by hand rather than by the DXE dispatcher, then a connection pass lets
 * them bind to whatever Block I/O handles the platform published.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PciIo.h>
#include <Protocol/Usb2HostController.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbFatModuleTag = "SuperFbFat";

/*
 * Entry points of the statically linked drivers. Declared locally because the
 * headers that carry them are module-private to their own packages.
 */
EFI_STATUS
EFIAPI
InitializeUnicodeCollationEng (IN EFI_HANDLE       ImageHandle,
                               IN EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS
EFIAPI
InitializeDiskIo (IN EFI_HANDLE       ImageHandle,
                  IN EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS
EFIAPI
FatEntryPoint (IN EFI_HANDLE       ImageHandle,
               IN EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS
EFIAPI
Ext4EntryPoint (IN EFI_HANDLE       ImageHandle,
                IN EFI_SYSTEM_TABLE *SystemTable);

/*
 * Both driver entry points install their EFI_DRIVER_BINDING_PROTOCOL onto the
 * handle they are handed, and a handle can only carry one of those. They also
 * ASSERT on failure, and this product builds with ASSERT_DEADLOOP enabled, so
 * each driver has to be given a private handle of its own.
 */
STATIC EFI_GUID mSfbDriverTagGuid = {
  0x7b41c0de, 0x2f95, 0x4a18,
  { 0x9c, 0x6d, 0x3e, 0x08, 0xb7, 0x52, 0xd1, 0x64 }
};

STATIC BOOLEAN mSfbFatStackStarted = FALSE;

STATIC
EFI_STATUS
SfbCreateDriverHandle (OUT EFI_HANDLE *Handle)
{
  *Handle = NULL;
  return gBS->InstallProtocolInterface (Handle,
                                        &mSfbDriverTagGuid,
                                        EFI_NATIVE_INTERFACE,
                                        NULL);
}

/*
 * Recursively connect every controller in the system.
 *
 * The fastboot-only boot path skips the BDS "connect all" pass, so on this
 * platform whole device stacks are left dispatched-but-unconnected. Most of
 * them do not matter here, but the USB host storage chain does: this platform's
 * firmware carries the Qualcomm USB host bring-up (UsbConfigDxe), the XHCI
 * PCI-emulation shim, XhciDxe, UsbBusDxe and UsbMassStorageDxe, but nothing in
 * the fastboot path ever connects them, so an attached USB drive never appears.
 */
VOID
SfbConnectAll (VOID)
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count = 0;
  UINTN       Index;
  UINTN       Connected = 0;

  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB: no handles to connect: %r\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    if (!EFI_ERROR (Status)) {
      Connected++;
    }
  }

  DEBUG ((EFI_D_INFO, "SFB: connected %u of %u handles\n",
          (UINT32)Connected, (UINT32)Count));

  FreePool (Handles);
}

EFI_STATUS
SfbStartFatStack (VOID)
{
  EFI_STATUS  Status;
  EFI_HANDLE  DiskIoHandle = NULL;
  EFI_HANDLE  FatHandle = NULL;

  if (mSfbFatStackStarted) {
    /* Re-run the connection pass only: media may have appeared since, and a USB
     * drive may have just been inserted (or a host cable attached). */
    SfbConnectAll ();
    return EFI_SUCCESS;
  }

  /*
   * EnhancedFatDxe refuses to mount a volume without a Unicode Collation
   * producer. This installs onto a handle of its own making, and a second
   * producer alongside a platform-supplied one is harmless: the FAT driver
   * picks whichever matches the platform language.
   */
  Status = InitializeUnicodeCollationEng (gImageHandle, gST);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: Unicode Collation init failed: %r\n", Status));
    return Status;
  }

  Status = SfbCreateDriverHandle (&DiskIoHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: Disk I/O handle alloc failed: %r\n", Status));
    return Status;
  }

  Status = InitializeDiskIo (DiskIoHandle, gST);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: Disk I/O driver init failed: %r\n", Status));
    return Status;
  }

  Status = SfbCreateDriverHandle (&FatHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: FAT handle alloc failed: %r\n", Status));
    return Status;
  }

  Status = FatEntryPoint (FatHandle, gST);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: FAT driver init failed: %r\n", Status));
    return Status;
  }

  /*
   * The read-only EXT4 driver mounts ext4 partitions so they can be scanned
   * and browsed like FAT volumes. Same pattern as FAT above: a private handle
   * carries its driver binding, and the connect pass at the end binds it to
   * the Disk I/O handles of any ext4 partitions. Failure here is non-fatal to
   * the FAT stack already up; ext4 volumes simply will not appear.
   */
  Status = SfbCreateDriverHandle (&FatHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: Ext4 handle alloc failed: %r\n", Status));
  } else {
    Status = Ext4EntryPoint (FatHandle, gST);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: Ext4 driver init failed: %r\n", Status));
    }
  }

  mSfbFatStackStarted = TRUE;

  SfbConnectAll ();

  return EFI_SUCCESS;
}

/*
 * Byte offsets into the FAT boot sector. Named here rather than pulled from
 * EnhancedFatDxe's FatFileSystem.h, which is module-private to its package.
 */
#define SFB_BPB_BYTES_PER_SEC   11
#define SFB_BPB_ROOT_ENT_CNT    17
#define SFB_BPB_TOT_SEC_16      19
#define SFB_BPB_FAT_SZ_16       22
#define SFB_BPB_FAT_SZ_32       36
#define SFB_BPB_FS_TYPE_32      82
#define SFB_BPB_SIGNATURE       510

STATIC
UINT16
SfbLe16 (IN CONST UINT8 *Sector, IN UINTN Offset)
{
  return (UINT16)(Sector[Offset] | ((UINT16)Sector[Offset + 1] << 8));
}

STATIC
UINT32
SfbLe32 (IN CONST UINT8 *Sector, IN UINTN Offset)
{
  return (UINT32)Sector[Offset] |
         ((UINT32)Sector[Offset + 1] << 8) |
         ((UINT32)Sector[Offset + 2] << 16) |
         ((UINT32)Sector[Offset + 3] << 24);
}

/*
 * Decide from the boot sector alone. The FAT type is defined by the geometry
 * rather than by the "FAT16"/"FAT32" text, which is documented as informational
 * only. Any FAT width is accepted here (FAT12/16 keep a 16-bit FAT size and a
 * non-zero root entry count; FAT32 zeroes both and carries a 32-bit FAT size),
 * because the efisp configuration container may be FAT12/16 while removable
 * media are usually FAT32.
 */
BOOLEAN
SfbIsFatVolume (IN EFI_HANDLE Volume)
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo = NULL;
  UINT8                  *Sector;
  UINTN                  SectorSize;
  UINT16                 BytesPerSec;
  BOOLEAN                IsFat = FALSE;

  Status = gBS->HandleProtocol (Volume, &gEfiBlockIoProtocolGuid,
                                (VOID **)&BlockIo);
  if (EFI_ERROR (Status) || BlockIo == NULL || BlockIo->Media == NULL) {
    /* No block device behind it: this is not a partition at all. */
    return FALSE;
  }

  if (!BlockIo->Media->MediaPresent) {
    return FALSE;
  }

  SectorSize = BlockIo->Media->BlockSize;
  if (SectorSize < 512) {
    return FALSE;
  }

  Sector = AllocateAlignedPages (EFI_SIZE_TO_PAGES (SectorSize),
                                 BlockIo->Media->IoAlign > 1 ?
                                   BlockIo->Media->IoAlign : 8);
  if (Sector == NULL) {
    return FALSE;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0,
                                SectorSize, Sector);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_VERBOSE, "SFB: boot sector read failed: %r\n", Status));
    goto Done;
  }

  if (Sector[SFB_BPB_SIGNATURE] != 0x55 ||
      Sector[SFB_BPB_SIGNATURE + 1] != 0xAA) {
    goto Done;
  }

  BytesPerSec = SfbLe16 (Sector, SFB_BPB_BYTES_PER_SEC);
  if (BytesPerSec != 512 && BytesPerSec != 1024 &&
      BytesPerSec != 2048 && BytesPerSec != 4096) {
    goto Done;
  }

  /* A FAT of any width always has a FAT size in exactly one of the two BPB
   * fields. Anything else is not a FAT boot sector. */
  if (SfbLe16 (Sector, SFB_BPB_FAT_SZ_16) != 0 ||
      SfbLe32 (Sector, SFB_BPB_FAT_SZ_32) != 0) {
    IsFat = TRUE;
    goto Done;
  }

Done:
  FreeAlignedPages (Sector, EFI_SIZE_TO_PAGES (SectorSize));

  return IsFat;
}

/*
 * The ext4 superblock sits 1024 bytes into the partition and carries the
 * 0xEF53 signature at offset 56 within it (byte 1080). FAT32 volumes answer
 * FALSE here: their first few KiB are a boot sector and FATs, never an ext4
 * superblock, so this and SfbIsFatVolume () partition the volume set
 * cleanly and a handle is never both.
 */
BOOLEAN
SfbIsExt4Volume (IN EFI_HANDLE Volume)
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo = NULL;
  UINT8                  *Sector;
  UINTN                  SectorSize;
  UINTN                  Bytes;
  UINTN                  Blocks;
  BOOLEAN                IsExt4 = FALSE;

  Status = gBS->HandleProtocol (Volume, &gEfiBlockIoProtocolGuid,
                                (VOID **)&BlockIo);
  if (EFI_ERROR (Status) || BlockIo == NULL || BlockIo->Media == NULL) {
    return FALSE;
  }

  if (!BlockIo->Media->MediaPresent) {
    return FALSE;
  }

  SectorSize = BlockIo->Media->BlockSize;
  if (SectorSize < 512) {
    return FALSE;
  }

  /* The magic is at byte 1080; read at least that far from the start. */
  Bytes = 4096;
  Blocks = (Bytes + SectorSize - 1) / SectorSize;
  Bytes = Blocks * SectorSize;

  Sector = AllocateAlignedPages (EFI_SIZE_TO_PAGES (Bytes),
                                 BlockIo->Media->IoAlign > 1 ?
                                   BlockIo->Media->IoAlign : 8);
  if (Sector == NULL) {
    return FALSE;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0,
                                Bytes, Sector);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_VERBOSE, "SFB: ext4 superblock read failed: %r\n", Status));
    goto Done;
  }

  if (SfbLe16 (Sector, 1080) == 0xEF53) {
    IsExt4 = TRUE;
  }

Done:
  FreeAlignedPages (Sector, EFI_SIZE_TO_PAGES (Bytes));

  return IsExt4;
}

EFI_STATUS
SfbOpenVolumeRoot (IN EFI_HANDLE Volume, OUT EFI_FILE_PROTOCOL **Root)
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;

  *Root = NULL;

  Status = gBS->HandleProtocol (Volume,
                                &gEfiSimpleFileSystemProtocolGuid,
                                (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Fs->OpenVolume (Fs, Root);
}

EFI_STATUS
SfbLocateVolumes (OUT EFI_HANDLE **Handles, OUT UINTN *Count)
{
  EFI_STATUS  Status;
  EFI_HANDLE  *All = NULL;
  UINTN       AllCount = 0;
  UINTN       Kept = 0;
  UINTN       Index;

  *Handles = NULL;
  *Count = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol,
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    NULL,
                                    &AllCount,
                                    &All);
  if (EFI_ERROR (Status) || All == NULL) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  /* Filter in place: FAT of any width and ext4 are the supported boot file
   * systems. Anything else is dropped - the file browser builds its own
   * unfiltered list. */
  for (Index = 0; Index < AllCount; Index++) {
    if (SfbIsFatVolume (All[Index]) || SfbIsExt4Volume (All[Index])) {
      All[Kept++] = All[Index];
    }
  }

  DEBUG ((EFI_D_INFO, "SFB: %u of %u file systems are FAT/ext4 boot media\n",
          (UINT32)Kept, (UINT32)AllCount));

  if (Kept == 0) {
    FreePool (All);
    return EFI_NOT_FOUND;
  }

  *Handles = All;
  *Count = Kept;

  return EFI_SUCCESS;
}

/* ---- (disk,partition) addressing ----------------------------------------- */

/* Device path size without its end node. */
STATIC
UINTN
SfbDevicePathBodySize (IN EFI_DEVICE_PATH_PROTOCOL *Path)
{
  return GetDevicePathSize (Path) - END_DEVICE_PATH_LENGTH;
}

/*
 * The whole disks in firmware enumeration order: every Block I/O handle that
 * is not a logical partition. *Disks must be released with FreePool ().
 */
STATIC
EFI_STATUS
SfbListDisks (OUT EFI_HANDLE **Disks, OUT UINTN *Count)
{
  EFI_STATUS  Status;
  EFI_HANDLE  *All = NULL;
  UINTN       AllCount = 0;
  UINTN       Index;
  UINTN       Kept = 0;

  *Disks = NULL;
  *Count = 0;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                                    &AllCount, &All);
  if (EFI_ERROR (Status) || All == NULL) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < AllCount; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *BlockIo = NULL;

    if (!EFI_ERROR (gBS->HandleProtocol (All[Index], &gEfiBlockIoProtocolGuid,
                                         (VOID **)&BlockIo)) &&
        BlockIo != NULL && BlockIo->Media != NULL &&
        !BlockIo->Media->LogicalPartition &&
        DevicePathFromHandle (All[Index]) != NULL) {
      All[Kept++] = All[Index];
    }
  }

  *Disks = All;
  *Count = Kept;
  return EFI_SUCCESS;
}

/*
 * The GPT entry number of the partition whose device path is Path, when Path
 * is exactly the disk's path (Prefix bytes) plus one Hard Drive node. Anything
 * else below a disk - the config container's vendor node, nested media - is
 * not a partition and has no address.
 */
STATIC
EFI_STATUS
SfbPartitionIndex (IN EFI_DEVICE_PATH_PROTOCOL *Path,
                   IN UINTN                    Prefix,
                   OUT UINT32                  *Part)
{
  EFI_DEVICE_PATH_PROTOCOL  *Node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Path + Prefix);
  UINT32                    Number;

  if (IsDevicePathEnd (Node) || !IsDevicePathEnd (NextDevicePathNode (Node)) ||
      DevicePathType (Node) != MEDIA_DEVICE_PATH ||
      DevicePathSubType (Node) != MEDIA_HARDDRIVE_DP ||
      DevicePathNodeLength (Node) != sizeof (HARDDRIVE_DEVICE_PATH)) {
    return EFI_NOT_FOUND;
  }

  Number = ((HARDDRIVE_DEVICE_PATH *)Node)->PartitionNumber;
  if (Number == 0) {
    return EFI_NOT_FOUND;
  }
  *Part = Number - 1;
  return EFI_SUCCESS;
}

EFI_STATUS
SfbGetVolumeAddress (IN EFI_HANDLE Volume, OUT UINT32 *Disk, OUT UINT32 *Part)
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_HANDLE                *Disks = NULL;
  UINTN                     DiskCount = 0;
  UINTN                     Index;

  Path = DevicePathFromHandle (Volume);
  if (Path == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = SfbListDisks (&Disks, &DiskCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < DiskCount; Index++) {
    EFI_DEVICE_PATH_PROTOCOL  *DiskPath = DevicePathFromHandle (Disks[Index]);
    UINTN                     Prefix = SfbDevicePathBodySize (DiskPath);

    if (SfbDevicePathBodySize (Path) <= Prefix ||
        CompareMem (Path, DiskPath, Prefix) != 0) {
      continue;
    }
    /* A shorter disk path can prefix a deeper disk's partitions; keep
     * looking until the disk the partition sits directly on. */
    if (!EFI_ERROR (SfbPartitionIndex (Path, Prefix, Part))) {
      *Disk = (UINT32)Index;
      Status = EFI_SUCCESS;
      break;
    }
  }

  FreePool (Disks);
  return Status;
}

EFI_HANDLE
SfbFindVolumeByAddress (IN UINT32 Disk, IN UINT32 Part)
{
  EFI_HANDLE  *Volumes = NULL;
  UINTN       Count = 0;
  UINTN       Index;
  EFI_HANDLE  Found = NULL;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol,
                                          &gEfiSimpleFileSystemProtocolGuid,
                                          NULL, &Count, &Volumes)) ||
      Volumes == NULL) {
    return NULL;
  }

  for (Index = 0; Index < Count && Found == NULL; Index++) {
    UINT32  D;
    UINT32  P;

    if (!EFI_ERROR (SfbGetVolumeAddress (Volumes[Index], &D, &P)) &&
        D == Disk && P == Part) {
      Found = Volumes[Index];
    }
  }

  FreePool (Volumes);
  return Found;
}

BOOLEAN
SfbFileExists (IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  BOOLEAN            IsFile = FALSE;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || File == NULL) {
    return FALSE;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocateZeroPool (InfoSize);
    if (Info != NULL) {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
      if (!EFI_ERROR (Status)) {
        IsFile = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) == 0);
      }
      FreePool (Info);
    }
  }

  File->Close (File);

  return IsFile;
}

EFI_STATUS
SfbReadFileBytes (IN EFI_FILE_PROTOCOL *Root,
                  IN CONST CHAR16      *Path,
                  OUT VOID             *Buffer,
                  IN UINTN             MaxBytes,
                  OUT UINTN            *BytesRead)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  UINTN              ReadSize = MaxBytes;

  *BytesRead = 0;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || File == NULL) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  Status = File->Read (File, &ReadSize, Buffer);
  File->Close (File);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *BytesRead = ReadSize;

  return EFI_SUCCESS;
}

/*
 * Decide application vs driver from the PE image's subsystem field. The layout
 * checked here is fixed for both PE32 and PE32+: a DOS "MZ" header carries the
 * offset of the PE signature at 0x3C, the COFF header follows the 4-byte
 * signature, and the optional header's 16-bit Subsystem sits 68 bytes into it.
 * Reading a header-sized prefix is enough; anything that does not parse as a PE
 * image is reported as "not a driver" so the caller falls back to app handling.
 */
BOOLEAN
SfbIsEfiDriverFile (IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path)
{
  /*
   * Must be large enough to contain the PE optional header's Subsystem field.
   * EDK2/GCC/CLANG images place the PE signature well past the classic 0x40-ish
   * offset (typically e_lfanew ~= 0xE58 for AARCH64 output), so a 512-byte
   * prefix stops short of Subsystem (which lands near file offset 0xEB4) and the
   * bounds check below would wrongly report every such image as "not a driver".
   * 8 KiB comfortably covers e_lfanew for all images this menu launches.
   */
  UINT8   Header[8192];
  UINTN   Read = 0;
  UINT32  PeOffset;
  UINTN   SubsystemAt;
  UINT16  Subsystem;

  if (EFI_ERROR (SfbReadFileBytes (Root, Path, Header, sizeof (Header), &Read))) {
    return FALSE;
  }

  /* Need at least the DOS header and its e_lfanew field. */
  if (Read < 0x40) {
    return FALSE;
  }
  if (SfbLe16 (Header, 0) != EFI_IMAGE_DOS_SIGNATURE) {   /* 'MZ' */
    return FALSE;
  }

  PeOffset = SfbLe32 (Header, 0x3C);

  /* Optional header starts after the 4-byte PE signature and 20-byte COFF
   * header; Subsystem is 68 bytes into it. */
  SubsystemAt = (UINTN)PeOffset + 4 + 20 + 68;
  if (SubsystemAt + sizeof (UINT16) > Read) {
    return FALSE;
  }
  if (SfbLe32 (Header, PeOffset) != EFI_IMAGE_NT_SIGNATURE) {  /* 'PE\0\0' */
    return FALSE;
  }

  Subsystem = SfbLe16 (Header, SubsystemAt);

  return (BOOLEAN)(Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER ||
                   Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER);
}

VOID
SfbGetVolumeLabel (IN EFI_FILE_PROTOCOL *Root,
                   OUT CHAR16           *Out,
                   IN UINTN             OutChars)
{
  EFI_STATUS                       Status;
  EFI_FILE_SYSTEM_VOLUME_LABEL     *Label;
  UINTN                            InfoSize;

  if (OutChars == 0) {
    return;
  }
  Out[0] = L'\0';

  InfoSize = 0;
  Status = Root->GetInfo (Root,
                          &gEfiFileSystemVolumeLabelInfoIdGuid,
                          &InfoSize,
                          NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  Label = AllocateZeroPool (InfoSize);
  if (Label == NULL) {
    return;
  }

  Status = Root->GetInfo (Root,
                          &gEfiFileSystemVolumeLabelInfoIdGuid,
                          &InfoSize,
                          Label);
  if (!EFI_ERROR (Status)) {
    StrnCpyS (Out, OutChars, Label->VolumeLabel, OutChars - 1);
  }

  FreePool (Label);
}

