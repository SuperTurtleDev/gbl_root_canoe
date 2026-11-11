/*
 * Persistent configuration store for the super-fastboot boot menu.
 *
 * The efisp partition is split in two:
 *
 *   [StartingLBA, StartingLBA + 1 MiB)   BDS PE image (flashed by length only)
 *   [StartingLBA + 1 MiB, EndingLBA]     one FAT volume, no partition table
 *
 * Both bounds come straight from the GPT EFI_PARTITION_ENTRY (EndingLBA is
 * inclusive). The FAT volume is published as a 512-byte "superfloppy" Block
 * I/O that can only address that sub-range, so nothing written through it can
 * reach the BDS image or anything past EndingLBA.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_CONFIG_STORE_H__
#define __SFB_CONFIG_STORE_H__

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

/* GPT partition name of the partition holding BDS and the container. */
#define SFB_CONFIG_PARTITION_NAME       L"efisp"

/* Bytes at the start of the partition that belong to the BDS PE image. */
#define SFB_CONFIG_BDS_RESERVED_BYTES   SIZE_1MB

/*
 * Receives the container for a USB export session. Container is the 512-byte
 * bounded Block I/O, not published to any driver while the sink runs. The sink
 * wraps it (e.g. in a synthesized GPT) and runs the session to completion.
 */
typedef
EFI_STATUS
(*SFB_CONFIG_EXPORT_SINK) (
  IN EFI_BLOCK_IO_PROTOCOL  *Container,
  IN VOID                   *Context
  );

/*
 * Locate the container, publish its bounded Block I/O and bind FAT to it.
 * A container the FAT driver cannot mount and whose boot sector is not a
 * usable FAT boot sector is formatted in place, flushed and mounted again.
 * Any remaining failure leaves the store unavailable; the caller boots on
 * without persistent storage. Idempotent.
 */
EFI_STATUS
SfbConfigStoreMount (
  VOID
  );

/* Flush, disconnect FAT and withdraw the Block I/O and device path. */
VOID
SfbConfigStoreUnmount (
  VOID
  );

/*
 * The container's file system and the handle carrying it. Either output may
 * be NULL. EFI_NOT_FOUND while the store is unavailable or unmounted.
 */
EFI_STATUS
SfbConfigStoreGetFs (
  OUT EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  **Fs      OPTIONAL,
  OUT EFI_HANDLE                       *Volume  OPTIONAL
  );

/*
 * Unmount, hand the container to Sink for a USB export session, then mount
 * again. Returns the sink's status, or EFI_NOT_FOUND when no container exists.
 */
EFI_STATUS
SfbConfigStoreExportUsb (
  IN SFB_CONFIG_EXPORT_SINK  Sink,
  IN VOID                    *Context
  );

#endif /* __SFB_CONFIG_STORE_H__ */
