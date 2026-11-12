/*
 * Volume support for the super-fastboot boot menu.
 *
 * FAT file systems come from the platform firmware's own FAT driver. The
 * read-only EXT4 driver, DiskIoDxe and the English Unicode Collation driver
 * are linked into this application as static libraries: their entry points
 * are invoked here by hand rather than by the DXE dispatcher, then a
 * connection pass binds them to the Block I/O handles the platform published.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/SfbConfigStore.h>
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
   * Ext4Dxe needs a Unicode Collation producer and Disk I/O on the block
   * handles. A second producer alongside a platform-supplied one is
   * harmless: the driver picks whichever matches the platform language.
   * FAT is already handled by the firmware, so no FAT driver is started.
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

  /*
   * The read-only EXT4 driver mounts ext4 partitions so they can be scanned
   * and browsed like FAT volumes: a private handle carries its driver
   * binding, and the connect pass at the end binds it to the Disk I/O
   * handles of any ext4 partitions. Failure here is non-fatal; ext4 volumes
   * simply will not appear.
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

typedef struct {
  EFI_HANDLE  Handle;
  UINT32      Disk;
  UINT32      Part;
} SFB_VOLUME;

STATIC EFI_STATUS SfbListVolumes (OUT SFB_VOLUME **Table, OUT UINTN *Count);

EFI_STATUS
SfbLocateVolumes (OUT EFI_HANDLE **Handles, OUT UINTN *Count)
{
  SFB_VOLUME  *Vols = NULL;
  EFI_HANDLE  *List;
  UINTN       VolCount = 0;
  UINTN       Kept = 0;
  UINTN       Index;

  *Handles = NULL;
  *Count = 0;

  if (EFI_ERROR (SfbListVolumes (&Vols, &VolCount)) || VolCount == 0) {
    return EFI_NOT_FOUND;
  }

  List = AllocateZeroPool (VolCount * sizeof (*List));
  if (List == NULL) {
    FreePool (Vols);
    return EFI_OUT_OF_RESOURCES;
  }

  /* The table IS the volume list; keep the entries that actually carry a
   * supported file system so the boot-entry scan sees only mountable media. */
  for (Index = 0; Index < VolCount; Index++) {
    if (SfbIsFatVolume (Vols[Index].Handle) ||
        SfbIsExt4Volume (Vols[Index].Handle)) {
      List[Kept++] = Vols[Index].Handle;
    }
  }
  FreePool (Vols);

  if (Kept == 0) {
    FreePool (List);
    return EFI_NOT_FOUND;
  }

  *Handles = List;
  *Count = Kept;
  return EFI_SUCCESS;
}

/* ---- volume table -------------------------------------------------------- */

/*
 * The volume list IS the platform partition table: every Ptable[Lun] entry is
 * a UFS partition handle, addressed as (Lun, j) - lun0 = disk 0, lun1 = disk
 * 1, ... The efisp config container is appended as the last disk, one past
 * the highest LUN that actually has partitions. No other volumes exist for
 * this loader: firmware volumes, USB media and other non-UFS disks are never
 * enumerated.
 *
 * The table is rebuilt on each call so a USB export session that refreshes
 * Ptable is picked up immediately. *Table must be released with FreePool ().
 */
STATIC
EFI_STATUS
SfbListVolumes (OUT SFB_VOLUME **Table, OUT UINTN *Count)
{
  SFB_VOLUME  *Vols;
  UINT32      Lun;
  UINT32      MaxLuns = GetMaxLuns ();
  UINT32      ConfigDisk = 0;
  UINTN       Used = 0;
  UINTN       j;
  EFI_HANDLE  Config = NULL;

  *Table = NULL;
  *Count = 0;
  Vols = AllocateZeroPool ((MAX_NUM_PARTITIONS + 1) * sizeof (*Vols));
  if (Vols == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Lun = 0; Lun < MaxLuns && Lun < MAX_LUNS; Lun++) {
    for (j = 0; j < Ptable[Lun].MaxHandles; j++) {
      if (Ptable[Lun].HandleInfoList[j].Handle != NULL) {
        Vols[Used].Handle = Ptable[Lun].HandleInfoList[j].Handle;
        Vols[Used].Disk = Lun;
        Vols[Used].Part = (UINT32)j;
        Used++;
        if (Lun + 1 > ConfigDisk) {
          ConfigDisk = Lun + 1;
        }
      }
    }
  }

  (VOID)SfbConfigStoreGetFs (NULL, &Config);
  if (Config != NULL) {
    Vols[Used].Handle = Config;
    Vols[Used].Disk = ConfigDisk;
    Vols[Used].Part = 0;
    Used++;
  }

  *Table = Vols;
  *Count = Used;
  return EFI_SUCCESS;
}

EFI_STATUS
SfbGetVolumeAddress (IN EFI_HANDLE Volume, OUT UINT32 *Disk, OUT UINT32 *Part)
{
  SFB_VOLUME  *Vols = NULL;
  UINTN       Count = 0;
  UINTN       Index;
  EFI_STATUS  Status = EFI_NOT_FOUND;

  if (EFI_ERROR (SfbListVolumes (&Vols, &Count))) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < Count; Index++) {
    if (Vols[Index].Handle == Volume) {
      *Disk = Vols[Index].Disk;
      *Part = Vols[Index].Part;
      Status = EFI_SUCCESS;
      break;
    }
  }
  FreePool (Vols);
  return Status;
}

EFI_HANDLE
SfbFindVolumeByAddress (IN UINT32 Disk, IN UINT32 Part)
{
  SFB_VOLUME  *Vols = NULL;
  UINTN       Count = 0;
  UINTN       Index;
  EFI_HANDLE  Found = NULL;

  if (EFI_ERROR (SfbListVolumes (&Vols, &Count))) {
    return NULL;
  }
  for (Index = 0; Index < Count; Index++) {
    if (Vols[Index].Disk == Disk && Vols[Index].Part == Part) {
      Found = Vols[Index].Handle;
      break;
    }
  }
  FreePool (Vols);
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

