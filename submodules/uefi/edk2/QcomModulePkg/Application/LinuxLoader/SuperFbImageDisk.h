/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SUPER_FB_IMAGE_DISK_H
#define SUPER_FB_IMAGE_DISK_H
#include <Library/Ext4ImageMap.h>
#include <Protocol/BlockIo.h>
#include <Uefi.h>
typedef struct
{
  EFI_BLOCK_IO_PROTOCOL Block;
  EFI_BLOCK_IO_MEDIA Media;
  EXT4_IMAGE_MAP *Map;
  VOID *Bounce;
  UINTN BouncePages;
  UINT32 ParentBlockSize;
  BOOLEAN Active;
} SFB_IMAGE_DISK;
/* Map ownership remains with the caller. It must outlive Disk. */
EFI_STATUS SfbImageDiskInit (SFB_IMAGE_DISK *Disk, EXT4_IMAGE_MAP *Map);
/* Only after protocols are detached and all clients have released this disk. */
VOID SfbImageDiskDestroy (SFB_IMAGE_DISK *Disk);
#endif
