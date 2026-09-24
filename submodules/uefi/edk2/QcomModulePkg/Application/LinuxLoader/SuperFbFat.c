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
   * The read-only EXT4 driver mounts the ext4 persist partition so its \efisp
   * directory can be scanned and browsed like a FAT32 volume. Same pattern as
   * FAT above: a private handle carries its driver binding, and the connect
   * pass at the end binds it to the Disk I/O handles of any ext4 partitions.
   * Failure here is non-fatal to the FAT stack already up, but the persist
   * volume simply will not appear.
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
 * because the efisp.fat boot blobs are FAT16 while removable media are usually
 * FAT32.
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

/*
 * The subdirectory that plays the role of a FAT32 volume root on a given
 * volume: empty for genuine FAT32 (its root already is the scan root) and
 * \efisp for the ext4 persist partition, whose boot files live there. The
 * entry scanner and the browser prepend this to the well-known boot file
 * paths and use it as the browse floor respectively.
 */
CONST CHAR16 *
SfbVolumeRootPrefix (IN EFI_HANDLE Volume)
{
  return SfbIsExt4Volume (Volume) ? L"\\efisp" : L"";
}

/*
 * TRUE when Path names an existing directory on Volume. Ext4 volumes are only
 * treated as boot volumes when they carry \efisp, so an ext4 partition whose
 * \efisp directory has not been created is never scanned or offered in the
 * browser. FAT32 volumes are never gated on this: their root is the boot root.
 */
STATIC
BOOLEAN
SfbVolumeHasDir (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root = NULL;
  EFI_FILE_PROTOCOL  *Dir = NULL;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  BOOLEAN            IsDir = FALSE;

  if (EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) || Root == NULL) {
    return FALSE;
  }

  Status = Root->Open (Root, &Dir, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || Dir == NULL) {
    Root->Close (Root);
    return FALSE;
  }

  InfoSize = 0;
  Status = Dir->GetInfo (Dir, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocateZeroPool (InfoSize);
    if (Info != NULL) {
      Status = Dir->GetInfo (Dir, &gEfiFileInfoGuid, &InfoSize, Info);
      if (!EFI_ERROR (Status)) {
        IsDir = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) != 0);
      }
      FreePool (Info);
    }
  }

  Dir->Close (Dir);
  Root->Close (Root);

  return IsDir;
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

/* ---- efisp.fat blob volumes ---------------------------------------------- */

/*
 * A FAT boot blob kept as the \efisp.fat file inside the ext4 persist
 * partition. The file is published as a read-only 512-byte BlockIo disk on its
 * own handle; the resident DiskIo/FAT drivers then bind to it after a connect
 * pass, and the FAT filesystem inside the blob shows up as an ordinary FAT
 * volume - scanned at its root like any other FAT media, and browsable from
 * its root in the file browser. The backing file stays open for the whole
 * session: the ext4 driver here is read-only, so nothing ever writes through.
 */
#define SFB_FAT_BLOB_BLOCK_BYTES  512
#define SFB_FAT_BLOB_MIN_BYTES    (64U * 1024U)
#define SFB_FAT_BLOB_MAX_MOUNTS   4
#define SFB_FAT_BLOB_MEDIA_ID     0x46415442  /* 'FATB' */

/* Distinguishes the blob image-disk handles in the device path namespace. */
STATIC CONST EFI_GUID mSfbFatBlobDevPathGuid = {
  0x1f4d1d61, 0x6e4b, 0x4d3e,
  { 0x9d, 0x0f, 0x2f, 0x8e, 0xd1, 0x3f, 0x22, 0x90 }
};

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  BlkIo;
  EFI_BLOCK_IO_MEDIA     Media;
  EFI_FILE_PROTOCOL      *File;
  UINT64                 Blocks;
} SFB_FAT_BLOB_DISK;

/* This vendor tree's DevicePath.h carries MEDIA_VENDOR_DP but no
 * VENDOR_MEDIA_DEVICE_PATH typedef, so the node is spelled out here. */
#pragma pack(1)
typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;
  EFI_GUID                  Guid;
} SFB_VENDOR_MEDIA_DP;
#pragma pack()

typedef struct {
  EFI_HANDLE         Source;    /* ext4 SFS handle the blob file lives under */
  EFI_HANDLE         Disk;      /* handle carrying our BlockIo + DevicePath */
  SFB_FAT_BLOB_DISK  *Blob;
} SFB_FAT_BLOB_MOUNT;

STATIC SFB_FAT_BLOB_MOUNT  mSfbFatBlobMounts[SFB_FAT_BLOB_MAX_MOUNTS];
STATIC UINTN               mSfbFatBlobMountCount = 0;

STATIC
EFI_STATUS
EFIAPI
SfbFatBlobReadBlocks (IN EFI_BLOCK_IO_PROTOCOL *This,
                      IN UINT32                MediaId,
                      IN EFI_LBA               Lba,
                      IN UINTN                 BufferSize,
                      OUT VOID                 *Buffer)
{
  SFB_FAT_BLOB_DISK  *Blob = (SFB_FAT_BLOB_DISK *)This;
  EFI_STATUS         Status;
  UINTN              Size;

  if (MediaId != Blob->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (Buffer == NULL || BufferSize == 0 ||
      (BufferSize % SFB_FAT_BLOB_BLOCK_BYTES) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((UINT64)Lba + BufferSize / SFB_FAT_BLOB_BLOCK_BYTES > Blob->Blocks) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Blob->File->SetPosition (Blob->File,
                                    (UINT64)Lba * SFB_FAT_BLOB_BLOCK_BYTES);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Size = BufferSize;
  Status = Blob->File->Read (Blob->File, &Size, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Size != BufferSize) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbFatBlobWriteBlocks (IN EFI_BLOCK_IO_PROTOCOL *This,
                       IN UINT32                MediaId,
                       IN EFI_LBA               Lba,
                       IN UINTN                 BufferSize,
                       IN VOID                  *Buffer)
{
  (VOID)This;
  (VOID)MediaId;
  (VOID)Lba;
  (VOID)BufferSize;
  (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
SfbFatBlobReset (IN EFI_BLOCK_IO_PROTOCOL *This, IN BOOLEAN ExtendedVerification)
{
  (VOID)This;
  (VOID)ExtendedVerification;
  return EFI_SUCCESS;
}

/*
 * Open \efisp.fat on every ext4 volume that carries one and publish it as a
 * BlockIo disk, so the FAT stack can mount the blob. Idempotent: volumes that
 * already have a blob mount are skipped, so repeated calls (every menu
 * rebuild) only pick up newly appeared media.
 */
STATIC
VOID
SfbMountEfispFatVolumes (VOID)
{
  EFI_STATUS         Status;
  EFI_HANDLE         *All = NULL;
  UINTN              Count = 0;
  UINTN              Index;
  UINTN              Mount;
  BOOLEAN            MountedAny = FALSE;

  if (!mSfbFatStackStarted) {
    return;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol,
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &Count, &All);
  if (EFI_ERROR (Status) || All == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    EFI_FILE_PROTOCOL     *Root = NULL;
    EFI_FILE_PROTOCOL     *File = NULL;
    EFI_FILE_INFO         *Info = NULL;
    UINTN                 InfoSize = 0;
    UINT64                Bytes;
    SFB_FAT_BLOB_DISK     *Blob = NULL;
    EFI_HANDLE            Disk = NULL;
    UINT8                 *Path = NULL;
    BOOLEAN               Known = FALSE;

    if (!SfbIsExt4Volume (All[Index])) {
      continue;
    }
    for (Mount = 0; Mount < mSfbFatBlobMountCount; Mount++) {
      if (mSfbFatBlobMounts[Mount].Source == All[Index]) {
        Known = TRUE;
        break;
      }
    }
    if (Known || mSfbFatBlobMountCount >= SFB_FAT_BLOB_MAX_MOUNTS) {
      continue;
    }

    if (EFI_ERROR (SfbOpenVolumeRoot (All[Index], &Root)) || Root == NULL) {
      continue;
    }
    if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)L"\\efisp.fat",
                               EFI_FILE_MODE_READ, 0)) || File == NULL) {
      Root->Close (Root);
      continue;
    }

    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    Info = AllocateZeroPool (InfoSize);
    if (Info == NULL ||
        EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info))) {
      if (Info != NULL) {
        FreePool (Info);
      }
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    Bytes = Info->FileSize;
    FreePool (Info);

    if (Bytes < SFB_FAT_BLOB_MIN_BYTES ||
        (Bytes % SFB_FAT_BLOB_BLOCK_BYTES) != 0) {
      DEBUG ((EFI_D_INFO,
              "SFB: efisp.fat ignored, %Lu bytes is not a sane blob\n", Bytes));
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Blob = AllocateZeroPool (sizeof (*Blob));
    if (Blob == NULL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    Blob->File = File;   /* stays open for the session */
    Blob->Blocks = Bytes / SFB_FAT_BLOB_BLOCK_BYTES;

    Blob->Media.MediaId = SFB_FAT_BLOB_MEDIA_ID;
    Blob->Media.RemovableMedia = FALSE;
    Blob->Media.MediaPresent = TRUE;
    Blob->Media.LogicalPartition = FALSE;
    Blob->Media.ReadOnly = TRUE;
    Blob->Media.WriteCaching = FALSE;
    Blob->Media.BlockSize = SFB_FAT_BLOB_BLOCK_BYTES;
    Blob->Media.LastBlock = Blob->Blocks - 1;
    Blob->Media.IoAlign = 1;

    Blob->BlkIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    Blob->BlkIo.Media = &Blob->Media;
    Blob->BlkIo.Reset = SfbFatBlobReset;
    Blob->BlkIo.ReadBlocks = SfbFatBlobReadBlocks;
    Blob->BlkIo.WriteBlocks = SfbFatBlobWriteBlocks;
    Blob->BlkIo.FlushBlocks = NULL;

    /* One vendor-media node plus an end node: enough of a device path for the
     * resident drivers to bind, and a namespace of our own. */
    Path = AllocateZeroPool (sizeof (SFB_VENDOR_MEDIA_DP) +
                             sizeof (EFI_DEVICE_PATH_PROTOCOL));
    if (Path != NULL) {
      SFB_VENDOR_MEDIA_DP   *Node = (SFB_VENDOR_MEDIA_DP *)Path;
      EFI_DEVICE_PATH_PROTOCOL  *End =
        (EFI_DEVICE_PATH_PROTOCOL *)(Path + sizeof (SFB_VENDOR_MEDIA_DP));

      Node->Header.Type = MEDIA_DEVICE_PATH;
      Node->Header.SubType = MEDIA_VENDOR_DP;
      SetDevicePathNodeLength (&Node->Header, sizeof (*Node));
      CopyGuid (&Node->Guid, &mSfbFatBlobDevPathGuid);
      SetDevicePathEndNode (End);

      Status = gBS->InstallMultipleProtocolInterfaces (
                      &Disk,
                      &gEfiBlockIoProtocolGuid, &Blob->BlkIo,
                      &gEfiDevicePathProtocolGuid, Path,
                      NULL);
    } else {
      Status = EFI_OUT_OF_RESOURCES;
    }

    if (EFI_ERROR (Status) || Disk == NULL) {
      DEBUG ((EFI_D_ERROR, "SFB: efisp.fat publish failed: %r\n", Status));
      File->Close (File);
      FreePool (Blob);
      Root->Close (Root);
      continue;
    }

    mSfbFatBlobMounts[mSfbFatBlobMountCount].Source = All[Index];
    mSfbFatBlobMounts[mSfbFatBlobMountCount].Disk = Disk;
    mSfbFatBlobMounts[mSfbFatBlobMountCount].Blob = Blob;
    mSfbFatBlobMountCount++;
    MountedAny = TRUE;

    DEBUG ((EFI_D_INFO,
            "SFB: efisp.fat published: %Lu bytes, %Lu blocks\n",
            Bytes, Blob->Blocks));

    Root->Close (Root);
  }

  FreePool (All);

  if (MountedAny) {
    /* DiskIo and FAT are already resident; one connect pass binds them to the
     * new image disks so the blob's filesystem surfaces as a volume. */
    SfbConnectAll ();
  }
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

  /* Publish any efisp.fat boot blobs first so they enumerate like any other
   * FAT volume below. */
  SfbMountEfispFatVolumes ();

  Status = gBS->LocateHandleBuffer (ByProtocol,
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    NULL,
                                    &AllCount,
                                    &All);
  if (EFI_ERROR (Status) || All == NULL) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  /* Filter in place: the boot volumes are FAT of any width (removable media
   * and mounted efisp.fat blobs) plus the ext4 persist partition, whose \efisp
   * directory is the legacy boot root. An ext4 volume without \efisp is
   * dropped from the scan list: if it carried an efisp.fat, that blob is now
   * its own FAT volume. Anything else is dropped too - the file browser
   * builds its own unfiltered list. */
  for (Index = 0; Index < AllCount; Index++) {
    if (SfbIsFatVolume (All[Index]) ||
        (SfbIsExt4Volume (All[Index]) &&
         SfbVolumeHasDir (All[Index], L"\\efisp"))) {
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
SfbReadAnsiDescription (IN EFI_FILE_PROTOCOL *Root,
                        IN CONST CHAR16      *Path,
                        OUT CHAR16           *Out,
                        IN UINTN             OutChars)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  CHAR8              Buffer[256];
  UINTN              ReadSize;
  UINTN              Index;

  if (OutChars == 0) {
    return;
  }

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || File == NULL) {
    return;
  }

  ReadSize = sizeof (Buffer);
  Status = File->Read (File, &ReadSize, Buffer);
  File->Close (File);

  if (EFI_ERROR (Status) || ReadSize == 0) {
    return;
  }

  /* First line only, and only printable 7-bit characters: the console cannot
   * render anything else usefully and the file is specified as ANSI. */
  for (Index = 0; Index < ReadSize && Index < OutChars - 1; Index++) {
    CHAR8 Ch = Buffer[Index];

    if (Ch == '\0' || Ch == '\r' || Ch == '\n') {
      break;
    }
    if (Ch < 0x20 || (UINT8)Ch > 0x7e) {
      Ch = ' ';
    }
    Out[Index] = (CHAR16)Ch;
  }

  /* Leave the caller's fallback in place rather than blanking it. */
  if (Index > 0) {
    Out[Index] = L'\0';
  }
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

