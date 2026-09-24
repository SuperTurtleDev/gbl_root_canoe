/*
 * RAM-backed staged disks: an image file read into memory and published as a
 * writable BlockIo disk, mountable like any other volume.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_RAM_DISK_H__
#define __SUPER_FB_RAM_DISK_H__

#include <Uefi.h>

/*
 * Read Path (relative to some volume's root; '\' separators, leading one
 * optional) from the first volume that carries it, copy it into memory and
 * publish the copy as a read-write RAM disk. The published disk carries the
 * container device-path node, so the volume that mounts on it shows up under
 * Virtual Disks in the grouped browser and is scanned for boot entries.
 *
 * Returns EFI_SUCCESS and, when OutLabel has room, the source volume's label.
 */
EFI_STATUS
SfbRamStageImage (IN CONST CHAR16 *Path,
                  OUT CHAR16      *OutLabel OPTIONAL,
                  IN UINTN        LabelChars);

/* Tear down every staged RAM disk and free its memory. */
EFI_STATUS
SfbRamUnstageAll (VOID);

/* TRUE when at least one staged disk is published. */
BOOLEAN
SfbRamAnyStaged (VOID);

#endif /* __SUPER_FB_RAM_DISK_H__ */
