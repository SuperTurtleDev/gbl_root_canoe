/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef EXT4_IMAGE_MAP_H
#define EXT4_IMAGE_MAP_H
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#define EXT4_IMAGE_BYTES (32U * 1024U * 1024U)
#define EXT4_IMAGE_MIN_BYTES (8U * 1024U * 1024U)
#define EXT4_IMAGE_MAX_BYTES (256U * 1024U * 1024U)
#define EXT4_IMAGE_STEP_BYTES EXT4_IMAGE_MIN_BYTES
#define EXT4_IMAGE_MAX_EXTENTS (EXT4_IMAGE_MAX_BYTES / 1024U)
STATIC inline BOOLEAN Ext4ImageSizeValid (UINT64 Bytes) {
  return Bytes >= EXT4_IMAGE_MIN_BYTES && Bytes <= EXT4_IMAGE_MAX_BYTES && Bytes % EXT4_IMAGE_STEP_BYTES == 0;
}
typedef struct {
  UINT64 Logical;
  UINT64 Physical;
  UINT64 Bytes;
} EXT4_IMAGE_RANGE;
typedef struct {
  EFI_BLOCK_IO_PROTOCOL *Parent;
  UINT32 MediaId;
  UINT64 Bytes;
  /* UUID bytes, little-endian inode number and generation. No C padding is
   * included in the 24-byte export identity. */
  UINT8 Identity[24];
  UINTN Count;
  EXT4_IMAGE_RANGE Ranges[EXT4_IMAGE_MAX_EXTENTS];
} EXT4_IMAGE_MAP;
/* Open through this image's ext4 provider. A foreign provider is released only
 * through its identified driver; underlying disk protocols remain connected. */
EFI_STATUS Ext4OpenImageFileSystem (EFI_HANDLE Controller, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **Fs);
/* Release only this image's ext4 binding. Already unowned is success; a driver
 * refusal is retained. DiskIo, BlockIo and foreign filesystem owners stay up. */
EFI_STATUS Ext4ReleaseImageFileSystem (EFI_HANDLE Controller);
/* File must come from this embedded ext4 driver. Caller frees the returned map.
 * It is session evidence only: release before exporting persist or remounting.
 * No filesystem allocation, repair, or writes are performed here. */
EFI_STATUS Ext4MapImage (EFI_FILE_PROTOCOL *File, EXT4_IMAGE_MAP **Map);
#endif
