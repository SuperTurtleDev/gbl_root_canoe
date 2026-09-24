/*
 * Advanced submenu: USB mass-storage export of UFS LUNs and partitions.
 *
 *   Advanced
 *    `- USB Mass Storage
 *        |- Export a partition   -> LUN picker -> partition picker -> mode
 *        `- Export a LUN         -> LUN picker -> mode
 *
 * A partition is exported behind a RAM-backed fake GPT so the host sees a
 * valid one-partition disk instead of an uninitialized one; the fake tables
 * are write-protected in both mount modes. A whole LUN is exported
 * one-to-one, optionally forced read-only. The mount mode chooser runs before
 * every export.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbAdvanced.h"
#include "SuperFbMenu.h"
#include "SuperFbSynthDisk.h"
#include "SuperFbUsbMsd.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Uefi/UefiGpt.h>

#define SFB_ADV_ROW_CHARS  80
#define SFB_ADV_MAX_LUNS   MAX_LUNS

/* Snapshot of one pickable target, resolved at list build time. The handles
 * behind UFS partitions live in the platform partition driver and survive USB
 * mode switches, so the pointers stay valid across an export session; the
 * lists are nevertheless rebuilt every time a picker opens. */
typedef struct {
  EFI_PARTITION_ENTRY  *Entry;    /* GPT record; NULL for the Back row */
  EFI_BLOCK_IO_PROTOCOL *BlkIo;   /* partition child BlkIo (LBA0 = start) */
  UINT64               Bytes;
  UINT32               Lun;
} SFB_ADV_PARTITION;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL *BlkIo;   /* whole-LUN BlkIo (LogicalPartition=FALSE) */
  UINT64               Bytes;
  UINT32               Lun;
} SFB_ADV_LUN;

/* ---- tiny generic list chooser ------------------------------------------- */

/*
 * Draw Rows[0..Count) plus a trailing "Back" row and service input. Returns
 * the chosen row index, or -1 when the user picked Back (or the list is
 * empty). Scroll handling matches the boot menu.
 */
STATIC
INTN
SfbAdvChoose (IN CONST CHAR16 *Title,
              IN CONST CHAR16 *Subtitle,
              IN CONST CHAR16 (*Rows)[SFB_ADV_ROW_CHARS],
              IN UINTN        Count,
              IN UINTN        StartCursor)
{
  UINTN   Cursor = (StartCursor < Count) ? StartCursor : 0;
  UINTN   Total = Count + 1;
  UINTN   Start;
  UINTN   Index;
  UINTN   Last;
  SFB_KEY Key;

  while (TRUE) {
    SfbBeginScreen (Title, Subtitle);

    Start = SfbWindowStart (Cursor, Total, SFB_VISIBLE_ROWS);
    Last = Start + SFB_VISIBLE_ROWS;
    if (Last > Total) {
      Last = Total;
    }

    for (Index = Start; Index < Last; Index++) {
      if (Index < Count) {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Rows[Index]);
      } else {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", L"Back");
      }
    }
    if (Last < Total) {
      Print (L"    ... %u more\r\n", (UINT32)(Total - Last));
    }
    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Total, Key);
      continue;
    }
    if (Cursor < Count) {
      return (INTN)Cursor;
    }
    /* Back: debounce the confirming key before the parent redraws. */
    SfbDebounceMenuExit ();
    return -1;
  }
}

/* ---- mount mode chooser -------------------------------------------------- */

/*
 * TRUE = export read-only. Returns FALSE and leaves *ReadOnly untouched when
 * the user backs out.
 */
STATIC
BOOLEAN
SfbAdvChooseMountMode (IN CONST CHAR16 *Target,
                       OUT BOOLEAN     *ReadOnly)
{
  STATIC CONST CHAR16 Rows[2][SFB_ADV_ROW_CHARS] = {
    L"Read only",
    L"Read Write"
  };
  CHAR16 Subtitle[96];
  INTN   Chosen;

  UnicodeSPrint (Subtitle, sizeof (Subtitle), L"Mount %s how?", Target);
  Chosen = SfbAdvChoose (L"USB Mass Storage", Subtitle, Rows, 2, 0);
  if (Chosen < 0) {
    return FALSE;
  }
  *ReadOnly = (BOOLEAN)(Chosen == 0);
  return TRUE;
}

/* ---- LUN handling --------------------------------------------------------- */

/*
 * The whole-disk BlockIo behind a partition child: truncate the child's
 * device path before its final node and locate the BlockIo handle of what
 * remains, which is the LUN's own disk handle.
 */
STATIC
EFI_STATUS
SfbAdvLunBlkIoFromPartition (IN EFI_HANDLE             PartHandle,
                             OUT EFI_BLOCK_IO_PROTOCOL **BlkIo)
{
  EFI_STATUS               Status;
  EFI_DEVICE_PATH_PROTOCOL *Dp = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_DEVICE_PATH_PROTOCOL *Last = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Trunc;
  EFI_DEVICE_PATH_PROTOCOL *Remaining;
  EFI_HANDLE               Parent = NULL;
  EFI_BLOCK_IO_PROTOCOL    *ParentBlkIo = NULL;
  UINTN                    Len;

  *BlkIo = NULL;

  Status = gBS->HandleProtocol (PartHandle, &gEfiDevicePathProtocolGuid,
                                (VOID **)&Dp);
  if (EFI_ERROR (Status) || Dp == NULL) {
    return EFI_NOT_FOUND;
  }

  Node = Dp;
  while (!IsDevicePathEnd (Node)) {
    Last = Node;
    Node = NextDevicePathNode (Node);
  }
  if (Last == NULL || Last == Dp) {
    return EFI_NOT_FOUND;
  }

  Len = (UINTN)((UINT8 *)Last - (UINT8 *)Dp);
  Trunc = AllocatePool (Len + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (Trunc == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Trunc, Dp, Len);
  SetDevicePathEndNode ((UINT8 *)Trunc + Len);

  Remaining = Trunc;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Remaining,
                                  &Parent);
  FreePool (Trunc);
  if (EFI_ERROR (Status) || Parent == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (Parent, &gEfiBlockIoProtocolGuid,
                                (VOID **)&ParentBlkIo);
  if (EFI_ERROR (Status) || ParentBlkIo == NULL || ParentBlkIo->Media == NULL ||
      ParentBlkIo->Media->LogicalPartition) {
    return EFI_NOT_FOUND;
  }

  *BlkIo = ParentBlkIo;
  return EFI_SUCCESS;
}

/*
 * Snapshot the visible UFS LUNs: every LUN that carries at least one GPT
 * partition in the platform partition table. Returns the count; Luns must
 * hold at least SFB_ADV_MAX_LUNS entries.
 */
STATIC
UINTN
SfbAdvCollectLuns (OUT SFB_ADV_LUN *Luns)
{
  UINTN  Count = 0;
  UINT32 Lun;
  UINT32 MaxLuns = GetMaxLuns ();
  UINTN  j;

  if (MaxLuns > SFB_ADV_MAX_LUNS) {
    MaxLuns = SFB_ADV_MAX_LUNS;
  }

  /* Refresh the platform partition table first: handles may have been
   * re-created since the last enumeration. */
  (VOID)EnumeratePartitions ();

  for (Lun = 0; Lun < MaxLuns; Lun++) {
    EFI_BLOCK_IO_PROTOCOL *BlkIo = NULL;

    if (Ptable[Lun].MaxHandles == 0) {
      continue;
    }
    for (j = 0; j < Ptable[Lun].MaxHandles; j++) {
      if (Ptable[Lun].HandleInfoList[j].Handle != NULL &&
          !EFI_ERROR (SfbAdvLunBlkIoFromPartition (
                          Ptable[Lun].HandleInfoList[j].Handle, &BlkIo))) {
        break;
      }
      BlkIo = NULL;
    }
    if (BlkIo == NULL || BlkIo->Media == NULL) {
      continue;
    }
    Luns[Count].BlkIo = BlkIo;
    Luns[Count].Lun = Lun;
    Luns[Count].Bytes =
      (BlkIo->Media->LastBlock + 1) * (UINT64)BlkIo->Media->BlockSize;
    Count++;
  }

  return Count;
}

/* ---- export -------------------------------------------------------------- */

/*
 * Run the mode chooser, then export the partition behind a fake-GPT wrapper.
 * The RAM GPT region stays write-protected in both modes.
 */
STATIC
VOID
SfbAdvExportPartition (IN CONST SFB_ADV_PARTITION *Part)
{
  BOOLEAN        ReadOnly = TRUE;
  SFB_SYNTH_DISK *Disk = NULL;
  CHAR16         Target[48];
  CHAR16         Detail[96];
  UINT64         Bytes;
  EFI_STATUS     Status;

  StrnCpyS (Target, ARRAY_SIZE (Target), Part->Entry->PartitionName,
            ARRAY_SIZE (Target) - 1);
  if (!SfbAdvChooseMountMode (Target, &ReadOnly)) {
    return;
  }

  Bytes = (Part->BlkIo->Media->LastBlock + 1) *
          (UINT64)Part->BlkIo->Media->BlockSize;

  Status = SfbSynthDiskCreate (Part->BlkIo,
                               0,
                               Part->BlkIo->Media->LastBlock + 1,
                               &Part->Entry->PartitionTypeGUID,
                               Part->Entry->PartitionName,
                               ReadOnly,
                               &Disk);
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Could not wrap partition", Status);
    return;
  }

  UnicodeSPrint (Detail, sizeof (Detail),
                 L"Partition: %s (%a)", Target,
                 ReadOnly ? "read-only" : "read-write");
  Status = SfbUsbMsdExportBlkIo (&Disk->BlockIo, L"USB Mass Storage",
                                 Detail, Bytes);
  SfbSynthDiskDestroy (Disk);

  if (EFI_ERROR (Status) && Status != EFI_ABORTED &&
      Status != EFI_MEDIA_CHANGED) {
    SfbReportStatus (L"Could not start mass storage", Status);
  }
}

/* Run the mode chooser, then export the whole LUN one-to-one. */
STATIC
VOID
SfbAdvExportLun (IN CONST SFB_ADV_LUN *Lun)
{
  BOOLEAN        ReadOnly = TRUE;
  SFB_SYNTH_DISK *Disk = NULL;
  CHAR16         Target[48];
  CHAR16         Detail[96];
  EFI_STATUS     Status;

  UnicodeSPrint (Target, sizeof (Target), L"LUN %u", Lun->Lun);
  if (!SfbAdvChooseMountMode (Target, &ReadOnly)) {
    return;
  }

  Status = SfbPassDiskCreate (Lun->BlkIo, ReadOnly, &Disk);
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Could not wrap LUN", Status);
    return;
  }

  UnicodeSPrint (Detail, sizeof (Detail),
                 L"Whole LUN %u (%a)", Lun->Lun,
                 ReadOnly ? "read-only" : "read-write");
  Status = SfbUsbMsdExportBlkIo (&Disk->BlockIo, L"USB Mass Storage",
                                 Detail, Lun->Bytes);
  SfbSynthDiskDestroy (Disk);

  if (EFI_ERROR (Status) && Status != EFI_ABORTED &&
      Status != EFI_MEDIA_CHANGED) {
    SfbReportStatus (L"Could not start mass storage", Status);
  }
}

/* ---- pickers -------------------------------------------------------------- */

/* Snapshot of the partitions on one LUN, resolved from the platform table. */
STATIC
UINTN
SfbAdvCollectPartitions (IN UINT32 Lun, OUT SFB_ADV_PARTITION **Out)
{
  SFB_ADV_PARTITION *Parts;
  UINTN             Count = 0;
  UINTN             j;
  EFI_PARTITION_ENTRY *Entry;
  EFI_STATUS        Status;

  *Out = NULL;
  if (Ptable[Lun].MaxHandles == 0) {
    return 0;
  }

  Parts = AllocateZeroPool (Ptable[Lun].MaxHandles * sizeof (*Parts));
  if (Parts == NULL) {
    return 0;
  }

  for (j = 0; j < Ptable[Lun].MaxHandles; j++) {
    Entry = NULL;
    Status = gBS->HandleProtocol (Ptable[Lun].HandleInfoList[j].Handle,
                                  &gEfiPartitionRecordGuid,
                                  (VOID **)&Entry);
    if (EFI_ERROR (Status) || Entry == NULL) {
      continue;
    }
    if (Ptable[Lun].HandleInfoList[j].BlkIo == NULL ||
        Ptable[Lun].HandleInfoList[j].BlkIo->Media == NULL) {
      continue;
    }
    Parts[Count].Entry = Entry;
    Parts[Count].BlkIo = Ptable[Lun].HandleInfoList[j].BlkIo;
    Parts[Count].Lun = Lun;
    Parts[Count].Bytes =
      (Entry->EndingLBA - Entry->StartingLBA + 1) * 512ULL;
    Count++;
  }

  if (Count == 0) {
    FreePool (Parts);
    return 0;
  }
  *Out = Parts;
  return Count;
}

/* Export-a-partition: LUN picker, then partition picker on that LUN. */
STATIC
VOID
SfbAdvExportPartitionFlow (VOID)
{
  SFB_ADV_LUN Luns[SFB_ADV_MAX_LUNS];
  CHAR16      (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN       LunCount;
  UINTN       Index;
  INTN        Chosen;
  UINT32      Lun;
  SFB_ADV_PARTITION *Parts;
  UINTN       PartCount;

  SfbShowEnteringScreen (L"Export a partition");

  while (TRUE) {
    LunCount = SfbAdvCollectLuns (Luns);
    if (LunCount == 0) {
      SfbReportStatus (L"No storage LUNs found", EFI_NOT_FOUND);
      return;
    }

    Rows = AllocateZeroPool (LunCount * sizeof (*Rows));
    if (Rows == NULL) {
      return;
    }
    for (Index = 0; Index < LunCount; Index++) {
      UnicodeSPrint (Rows[Index], sizeof (Rows[Index]), L"LUN %u (%Lu MiB)",
                     Luns[Index].Lun,
                     Luns[Index].Bytes / (1024 * 1024));
    }

    Chosen = SfbAdvChoose (L"Export a partition",
                           L"Pick the LUN holding the partition.",
                           Rows, LunCount, 0);
    FreePool (Rows);
    if (Chosen < 0) {
      return;
    }
    Lun = Luns[Chosen].Lun;

    /* Partition picker on the chosen LUN. */
    while (TRUE) {
      SfbShowEnteringScreen (L"Partition Picker");

      PartCount = SfbAdvCollectPartitions (Lun, &Parts);
      if (PartCount == 0) {
        SfbReportStatus (L"No partitions on this LUN", EFI_NOT_FOUND);
        break;
      }

      Rows = AllocateZeroPool (PartCount * sizeof (*Rows));
      if (Rows == NULL) {
        FreePool (Parts);
        break;
      }
      for (Index = 0; Index < PartCount; Index++) {
        UnicodeSPrint (Rows[Index], sizeof (Rows[Index]), L"%s (%Lu MiB)",
                       Parts[Index].Entry->PartitionName,
                       Parts[Index].Bytes / (1024 * 1024));
      }

      Chosen = SfbAdvChoose (L"Export a partition",
                             L"Pick the partition to export.",
                             Rows, PartCount, 0);
      FreePool (Rows);
      if (Chosen < 0) {
        FreePool (Parts);
        break;
      }
      SfbAdvExportPartition (&Parts[Chosen]);
      FreePool (Parts);
      /* Loop: media state changed during the export; re-list. */
    }
    /* Loop: the LUN list is re-resolved too. */
  }
}

/* Export-a-LUN: LUN picker, then direct export. */
STATIC
VOID
SfbAdvExportLunFlow (VOID)
{
  SFB_ADV_LUN Luns[SFB_ADV_MAX_LUNS];
  CHAR16      (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN       LunCount;
  UINTN       Index;
  INTN        Chosen;

  SfbShowEnteringScreen (L"Export a LUN");

  while (TRUE) {
    LunCount = SfbAdvCollectLuns (Luns);
    if (LunCount == 0) {
      SfbReportStatus (L"No storage LUNs found", EFI_NOT_FOUND);
      return;
    }

    Rows = AllocateZeroPool (LunCount * sizeof (*Rows));
    if (Rows == NULL) {
      return;
    }
    for (Index = 0; Index < LunCount; Index++) {
      UnicodeSPrint (Rows[Index], sizeof (Rows[Index]), L"LUN %u (%Lu MiB)",
                     Luns[Index].Lun,
                     Luns[Index].Bytes / (1024 * 1024));
    }

    Chosen = SfbAdvChoose (L"Export a LUN",
                           L"Pick the LUN to export whole.",
                           Rows, LunCount, 0);
    FreePool (Rows);
    if (Chosen < 0) {
      return;
    }
    SfbAdvExportLun (&Luns[Chosen]);
  }
}

/* ---- virtual disk (efisp.fat) export -------------------------------------- */

/*
 * Label the blob mounted from SourceVolume as virtual(LUNx:name:idx): the LUN,
 * the GPT name and the GPT entry number of the mother partition carrying the
 * efisp.fat file, so the row names where the image lives.
 */
STATIC
VOID
SfbAdvVirtualLabel (IN EFI_HANDLE Source, OUT CHAR16 *Out, IN UINTN OutChars)
{
  EFI_PARTITION_ENTRY  *Entry = NULL;
  UINT32               Lun;
  UINT32               MaxLuns = GetMaxLuns ();
  UINTN                j;

  for (Lun = 0; Lun < MaxLuns && Lun < MAX_LUNS; Lun++) {
    for (j = 0; j < Ptable[Lun].MaxHandles; j++) {
      if (Ptable[Lun].HandleInfoList[j].Handle != Source) {
        continue;
      }
      if (EFI_ERROR (gBS->HandleProtocol (Ptable[Lun].HandleInfoList[j].Handle,
                                          &gEfiPartitionRecordGuid,
                                          (VOID **)&Entry)) || Entry == NULL) {
        break;
      }
      UnicodeSPrint (Out, OutChars, L"virtual(LUN%u:%s:%u)",
                     Lun, Entry->PartitionName, (UINT32)(j + 1));
      return;
    }
  }
  StrCpyS (Out, OutChars, L"virtual(efisp.fat)");
}

/*
 * Export an auto-mounted efisp.fat container to the host. The published FAT
 * view is withdrawn first (disconnect, flush, uninstall) so the host owns the
 * disk exclusively - mirroring the 7.x container's export order - and
 * republished when the session ends. Read-only exports go through the
 * pass-through wrapper so a host write is swallowed instead of reaching a
 * vendor driver error path.
 */
STATIC
VOID
SfbAdvExportVirtualFlow (VOID)
{
  CHAR16              (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN               Slots[24];
  UINTN               Count;
  UINTN               Shown;
  INTN                Chosen;
  UINTN               Blob;
  EFI_STATUS          Status;
  BOOLEAN             ReadOnly = TRUE;
  CHAR16              Target[64];
  CHAR16              Detail[96];
  SFB_SYNTH_DISK      *Wrap = NULL;
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  UINT64              Bytes;

  SfbShowEnteringScreen (L"Export a Virtual Disk");

  while (TRUE) {
    Count = SfbFatBlobCount ();
    if (Count == 0) {
      SfbReportStatus (L"No efisp.fat is mounted", EFI_NOT_FOUND);
      return;
    }

    Rows = AllocateZeroPool (Count * sizeof (*Rows));
    if (Rows == NULL) {
      return;
    }

    Shown = 0;
    for (Blob = 0; Blob < Count && Shown < 24; Blob++) {
      if (SfbFatBlobDisk (Blob) == NULL) {
        /* Withdrawn for an export already; leave it out of the list. */
        continue;
      }
      SfbAdvVirtualLabel (SfbFatBlobSource (Blob), Rows[Shown],
                          SFB_ADV_ROW_CHARS);
      Slots[Shown] = Blob;
      Shown++;
    }

    if (Shown == 0) {
      FreePool (Rows);
      SfbReportStatus (L"No efisp.fat is mounted", EFI_NOT_FOUND);
      return;
    }

    Chosen = SfbAdvChoose (L"Export a Virtual Disk",
                           L"Pick the mounted container to export.",
                           Rows, Shown, 0);
    FreePool (Rows);
    if (Chosen < 0) {
      return;
    }
    Blob = Slots[Chosen];

    SfbAdvVirtualLabel (SfbFatBlobSource (Blob), Target, ARRAY_SIZE (Target));
    if (!SfbAdvChooseMountMode (Target, &ReadOnly)) {
      continue;
    }

    Status = SfbFatBlobWithdraw (Blob);
    if (EFI_ERROR (Status)) {
      SfbReportStatus (L"Could not withdraw the container", Status);
      (VOID)SfbFatBlobRestore (Blob);
      continue;
    }

    BlkIo = SfbFatBlobImageDisk (Blob);
    if (BlkIo == NULL) {
      SfbReportStatus (L"Container unavailable", EFI_NOT_FOUND);
      (VOID)SfbFatBlobRestore (Blob);
      continue;
    }

    if (ReadOnly) {
      Status = SfbPassDiskCreate (BlkIo, TRUE, &Wrap);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Could not wrap container", Status);
        (VOID)SfbFatBlobRestore (Blob);
        continue;
      }
      Bytes = (Wrap->Media.LastBlock + 1) * (UINT64)Wrap->Media.BlockSize;
      BlkIo = &Wrap->BlockIo;
    } else {
      Bytes = (BlkIo->Media->LastBlock + 1) *
              (UINT64)BlkIo->Media->BlockSize;
    }

    UnicodeSPrint (Detail, sizeof (Detail), L"%s (%a)", Target,
                   ReadOnly ? "read-only" : "read-write");
    Status = SfbUsbMsdExportBlkIo (BlkIo, L"USB Mass Storage", Detail, Bytes);

    if (Wrap != NULL) {
      SfbSynthDiskDestroy (Wrap);
      Wrap = NULL;
    }
    (VOID)SfbFatBlobRestore (Blob);

    if (EFI_ERROR (Status) && Status != EFI_ABORTED &&
        Status != EFI_MEDIA_CHANGED) {
      SfbReportStatus (L"Could not start mass storage", Status);
    }
    /* Loop: the container list is re-resolved after the session. */
  }
}

/* ---- grouped volume browser (Advanced > Volumes) -------------------------- */

/* The container image-disk device-path node GUID, mirrored from SuperFbFat.c
 * to classify any container volume - ours or one published by another BDS
 * copy - as a virtual disk. */
STATIC CONST EFI_GUID mSfbAdvContainerNodeGuid = {
  0xf1086281, 0xc184, 0x47f7, { 0xbb, 0xae, 0x30, 0x61, 0x1f, 0x90, 0xe2, 0xa4 }
};

/* TRUE when the handle's device path carries the container vendor node. */
STATIC
BOOLEAN
SfbAdvIsVirtualVolume (IN EFI_HANDLE Volume)
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  Dp = DevicePathFromHandle (Volume);
  if (Dp == NULL) {
    return FALSE;
  }
  for (Node = Dp; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if (Node->Type == MEDIA_DEVICE_PATH && Node->SubType == MEDIA_VENDOR_DP &&
        (DevicePathNodeLength (Node) >= sizeof (VENDOR_DEVICE_PATH)) &&
        CompareGuid (&((VENDOR_DEVICE_PATH *)Node)->Guid,
                     &mSfbAdvContainerNodeGuid)) {
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * The whole-disk handle behind a volume: truncate the volume's device path
 * before its final node and locate the BlockIo handle of what remains.
 */
STATIC
EFI_HANDLE
SfbAdvParentDiskHandle (IN EFI_HANDLE Volume)
{
  EFI_STATUS               Status;
  EFI_DEVICE_PATH_PROTOCOL *Dp = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_DEVICE_PATH_PROTOCOL *Last = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Trunc;
  EFI_DEVICE_PATH_PROTOCOL *Remaining;
  EFI_HANDLE               Parent = NULL;
  EFI_BLOCK_IO_PROTOCOL    *BlkIo = NULL;
  UINTN                    Len;

  Status = gBS->HandleProtocol (Volume, &gEfiDevicePathProtocolGuid,
                                (VOID **)&Dp);
  if (EFI_ERROR (Status) || Dp == NULL) {
    return NULL;
  }
  Node = Dp;
  while (!IsDevicePathEnd (Node)) {
    Last = Node;
    Node = NextDevicePathNode (Node);
  }
  if (Last == NULL || Last == Dp) {
    return NULL;
  }

  Len = (UINTN)((UINT8 *)Last - (UINT8 *)Dp);
  Trunc = AllocatePool (Len + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (Trunc == NULL) {
    return NULL;
  }
  CopyMem (Trunc, Dp, Len);
  SetDevicePathEndNode ((UINT8 *)Trunc + Len);

  Remaining = Trunc;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Remaining, &Parent);
  FreePool (Trunc);
  if (EFI_ERROR (Status) || Parent == NULL) {
    return NULL;
  }
  if (EFI_ERROR (gBS->HandleProtocol (Parent, &gEfiBlockIoProtocolGuid,
                                      (VOID **)&BlkIo)) ||
      BlkIo == NULL || BlkIo->Media == NULL ||
      BlkIo->Media->LogicalPartition) {
    return NULL;
  }
  return Parent;
}

/* UFS LUN number of a whole-disk handle, or -1 when it is not a UFS LUN. */
STATIC
INTN
SfbAdvLunOfDisk (IN EFI_HANDLE Disk)
{
  UINT32  Lun;
  UINT32  MaxLuns = GetMaxLuns ();
  UINTN   j;

  for (Lun = 0; Lun < MaxLuns && Lun < MAX_LUNS; Lun++) {
    for (j = 0; j < Ptable[Lun].MaxHandles; j++) {
      if (Ptable[Lun].HandleInfoList[j].Handle == NULL) {
        continue;
      }
      if (SfbAdvParentDiskHandle (Ptable[Lun].HandleInfoList[j].Handle) == Disk) {
        return (INTN)Lun;
      }
    }
  }
  return -1;
}

#define SFB_ADV_GROUPS_MAX  16
#define SFB_ADV_VROWS_MAX   48

typedef struct {
  BOOLEAN    IsHeader;
  EFI_HANDLE Volume;
  CHAR16     Text[SFB_ADV_ROW_CHARS];
} SFB_ADV_VROW;

/* Volume label of one SFS handle, or an empty string. */
STATIC
VOID
SfbAdvVolumeLabel (IN EFI_HANDLE Volume, OUT CHAR16 *Label, IN UINTN LabelChars)
{
  EFI_FILE_PROTOCOL  *Root = NULL;

  Label[0] = L'\0';
  if (!EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) && Root != NULL) {
    SfbGetVolumeLabel (Root, Label, LabelChars);
    Root->Close (Root);
  }
}

/*
 * The grouped volume browser: one header per storage group - "LUN x" for each
 * UFS LUN, the volume label for other disks such as USB sticks - followed by
 * that group's file systems, then a "Virtual Disks" group holding every
 * container volume. Selecting a volume browses it from its root through the
 * ordinary file browser.
 */
STATIC
VOID
SfbRunVolumesMenu (VOID)
{
  EFI_STATUS   Status;
  EFI_HANDLE   *All = NULL;
  UINTN        Count = 0;
  UINTN        Index;
  EFI_HANDLE   Groups[SFB_ADV_GROUPS_MAX];
  INTN         GroupLun[SFB_ADV_GROUPS_MAX];
  UINTN        GroupCount = 0;
  UINTN        GroupIdx;
  UINTN        Move;
  SFB_ADV_VROW *Rows = NULL;
  UINTN        RowCount = 0;
  UINTN        Selectable[SFB_ADV_VROWS_MAX];
  UINTN        SelectCount = 0;
  UINTN        Cursor = 0;
  SFB_KEY      Key;
  BOOLEAN      HaveVirtual = FALSE;

  SfbShowEnteringScreen (L"Volumes");

  while (TRUE) {
    Status = gBS->LocateHandleBuffer (ByProtocol,
                                      &gEfiSimpleFileSystemProtocolGuid,
                                      NULL, &Count, &All);
    if (EFI_ERROR (Status) || All == NULL || Count == 0) {
      SfbReportStatus (L"No file system volumes found",
                       EFI_ERROR (Status) ? Status : EFI_NOT_FOUND);
      if (All != NULL) {
        FreePool (All);
      }
      return;
    }

    Rows = AllocateZeroPool ((Count + SFB_ADV_GROUPS_MAX + 2) * sizeof (*Rows));
    if (Rows == NULL) {
      FreePool (All);
      return;
    }
    RowCount = 0;
    GroupCount = 0;
    HaveVirtual = FALSE;

    /* Discover the physical groups, UFS LUNs in ascending order. */
    for (Index = 0; Index < Count; Index++) {
      EFI_HANDLE  Parent;
      INTN        Lun;

      if (SfbAdvIsVirtualVolume (All[Index])) {
        HaveVirtual = TRUE;
        continue;
      }
      Parent = SfbAdvParentDiskHandle (All[Index]);
      if (Parent == NULL) {
        continue;
      }
      Lun = SfbAdvLunOfDisk (Parent);

      for (GroupIdx = 0; GroupIdx < GroupCount; GroupIdx++) {
        if (Groups[GroupIdx] == Parent) {
          break;
        }
      }
      if (GroupIdx < GroupCount || GroupCount >= SFB_ADV_GROUPS_MAX) {
        continue;
      }
      if (Lun >= 0) {
        for (GroupIdx = 0; GroupIdx < GroupCount; GroupIdx++) {
          if (GroupLun[GroupIdx] >= 0 && GroupLun[GroupIdx] > Lun) {
            break;
          }
        }
        for (Move = GroupCount; Move > GroupIdx; Move--) {
          Groups[Move] = Groups[Move - 1];
          GroupLun[Move] = GroupLun[Move - 1];
        }
      } else {
        GroupIdx = GroupCount;
      }
      Groups[GroupIdx] = Parent;
      GroupLun[GroupIdx] = Lun;
      GroupCount++;
    }

    /* Emit each group: header, then its volumes. */
    for (GroupIdx = 0; GroupIdx < GroupCount && RowCount < SFB_ADV_VROWS_MAX;
         GroupIdx++) {
      CHAR16  First[SFB_DESC_CHARS];

      First[0] = L'\0';
      for (Index = 0; Index < Count && RowCount < SFB_ADV_VROWS_MAX; Index++) {
        CHAR16  Label[SFB_DESC_CHARS];

        if (SfbAdvIsVirtualVolume (All[Index]) ||
            SfbAdvParentDiskHandle (All[Index]) != Groups[GroupIdx]) {
          continue;
        }
        SfbAdvVolumeLabel (All[Index], Label, SFB_DESC_CHARS);
        if (First[0] == L'\0' && Label[0] != L'\0') {
          StrnCpyS (First, SFB_DESC_CHARS, Label, SFB_DESC_CHARS - 1);
        }
        Rows[RowCount].IsHeader = FALSE;
        Rows[RowCount].Volume = All[Index];
        StrnCpyS (Rows[RowCount].Text, SFB_ADV_ROW_CHARS,
                  (Label[0] != L'\0') ? Label : L"Volume",
                  SFB_ADV_ROW_CHARS - 1);
        RowCount++;
      }

      Rows[RowCount].IsHeader = TRUE;
      Rows[RowCount].Volume = NULL;
      if (GroupLun[GroupIdx] >= 0) {
        UnicodeSPrint (Rows[RowCount].Text, sizeof (Rows[RowCount].Text),
                       L"LUN %d", (INT32)GroupLun[GroupIdx]);
      } else {
        UnicodeSPrint (Rows[RowCount].Text, sizeof (Rows[RowCount].Text),
                       L"Disk %s", First);
      }
      RowCount++;
    }

    /* Virtual Disks group, last. */
    if (HaveVirtual && RowCount < SFB_ADV_VROWS_MAX) {
      Rows[RowCount].IsHeader = TRUE;
      Rows[RowCount].Volume = NULL;
      StrCpyS (Rows[RowCount].Text, SFB_ADV_ROW_CHARS, L"Virtual Disks");
      RowCount++;

      for (Index = 0; Index < Count && RowCount < SFB_ADV_VROWS_MAX; Index++) {
        CHAR16  Label[SFB_DESC_CHARS];

        if (!SfbAdvIsVirtualVolume (All[Index])) {
          continue;
        }
        SfbAdvVolumeLabel (All[Index], Label, SFB_DESC_CHARS);
        Rows[RowCount].IsHeader = FALSE;
        Rows[RowCount].Volume = All[Index];
        StrnCpyS (Rows[RowCount].Text, SFB_ADV_ROW_CHARS,
                  (Label[0] != L'\0') ? Label : L"container",
                  SFB_ADV_ROW_CHARS - 1);
        RowCount++;
      }
    }

    FreePool (All);
    All = NULL;

    /* Back row. */
    Rows[RowCount].IsHeader = FALSE;
    Rows[RowCount].Volume = NULL;
    StrCpyS (Rows[RowCount].Text, SFB_ADV_ROW_CHARS, L"Back");
    RowCount++;
    Cursor = 0;

    while (TRUE) {
      UINTN  Start;
      UINTN  Last;
      UINTN  Drawn;
      UINTN  Row;

      SelectCount = 0;
      for (Index = 0; Index < RowCount && SelectCount < SFB_ADV_VROWS_MAX;
           Index++) {
        if (!Rows[Index].IsHeader) {
          Selectable[SelectCount++] = Index;
        }
      }
      if (Cursor >= SelectCount) {
        Cursor = 0;
      }

      SfbBeginScreen (L"Volumes", L"Browse a volume from its root.");

      Start = SfbWindowStart (Cursor, SelectCount, SFB_VISIBLE_ROWS);
      Last = Start + SFB_VISIBLE_ROWS;
      if (Last > SelectCount) {
        Last = SelectCount;
      }
      for (Drawn = Start; Drawn < Last; Drawn++) {
        Row = Selectable[Drawn];
        SfbDrawRow ((BOOLEAN)(Drawn == Cursor),
                    (Rows[Row].Volume == NULL) ? L" " : L"[V]",
                    Rows[Row].Text);
      }
      if (Last < SelectCount) {
        Print (L"    ... %u more\r\n", (UINT32)(SelectCount - Last));
      }
      SfbEndScreen (L"Vol Up/Down: move   Power: select");

      Key = SfbWaitForKey (0);
      if (Key == SfbKeyUp || Key == SfbKeyDown) {
        SfbMoveCursor (&Cursor, SelectCount, Key);
        continue;
      }

      Row = Selectable[Cursor];
      if (Rows[Row].Volume == NULL) {
        SfbDebounceMenuExit ();
        FreePool (Rows);
        return;
      }
      if (SfbBrowseVolume (Rows[Row].Volume, Rows[Row].Text, L"\\")) {
        SfbDebounceMenuExit ();
        FreePool (Rows);
        return;
      }
      /* Media may have changed while browsing; rebuild the groups. */
      break;
    }

    FreePool (Rows);
    Rows = NULL;
  }
}

STATIC
VOID
SfbRunMassStorageMenu (VOID)
{
  STATIC CONST CHAR16 Rows[3][SFB_ADV_ROW_CHARS] = {
    L"Export a partition >",
    L"Export a LUN >",
    L"Export a Virtual Disk >"
  };
  INTN Chosen;

  SfbShowEnteringScreen (L"USB Mass Storage");

  while (TRUE) {
    Chosen = SfbAdvChoose (L"USB Mass Storage",
                           L"Expose device storage to the host over USB.",
                           Rows, 3, 0);
    if (Chosen < 0) {
      return;
    }
    if (Chosen == 0) {
      SfbAdvExportPartitionFlow ();
    } else if (Chosen == 1) {
      SfbAdvExportLunFlow ();
    } else {
      SfbAdvExportVirtualFlow ();
    }
  }
}

VOID
SfbRunAdvancedMenu (VOID)
{
  STATIC CONST CHAR16 Rows[2][SFB_ADV_ROW_CHARS] = {
    L"USB Mass Storage >",
    L"Volumes >"
  };
  INTN Chosen;

  SfbShowEnteringScreen (L"Advanced");

  while (TRUE) {
    Chosen = SfbAdvChoose (L"Advanced", NULL, Rows, 2, 0);
    if (Chosen < 0) {
      return;
    }
    if (Chosen == 0) {
      SfbRunMassStorageMenu ();
    } else {
      SfbRunVolumesMenu ();
    }
  }
}
