/*
 * Memory fix-ups the super-fastboot BDS applies to the platform firmware so
 * that a Linux kernel started through its EFI stub (or any UEFI 2.10 aware
 * loader) boots on this platform. Both are platform features of the canoe
 * tree: always installed, no configuration.
 *
 * 1. EFI_MEMORY_ATTRIBUTE_PROTOCOL
 *    The firmware maps freshly allocated EfiLoaderCode pages execute-never
 *    and does not publish the UEFI 2.10 protocol a loader needs to change
 *    that. The arm64 EFI stub relocates the kernel into such pages and
 *    enters it with the MMU still on, so the first kernel instruction faults
 *    right after ExitBootServices (no console, no panic, a silent reset).
 *    The BDS publishes the protocol itself with standard semantics: the
 *    live stage-1 translation tables are read for the current attributes,
 *    changes go through the firmware's EFI_CPU_ARCH_PROTOCOL (combined with
 *    the GCD cache type, as the DXE core's own image protection does) and
 *    are read back from the tables before success is reported.
 *
 * 2. GetMemoryMap merge
 *    The firmware memory map omits most of the DDR the TZ RAM partition
 *    table marks as AP-usable (it only describes what the boot firmware
 *    itself touched) and keeps TZ-usable ranges as RESERVED. An EFI-booted
 *    kernel builds its memblock from the UEFI map, not from the DT /memory
 *    node, so without this the kernel runs on a fraction of the installed
 *    RAM. gBS->GetMemoryMap is wrapped so every caller sees the TZ-usable
 *    additions as CONVENTIONAL entries and in-TZ RESERVED pieces released.
 *    The firmware allocator never hands these ranges out (it does not know
 *    them), so the view is stable across later allocations and the MapKey
 *    contract is untouched.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_MEM_FIXUP_H__
#define __SFB_MEM_FIXUP_H__

#include <Uefi.h>

/*
 * Install both fix-ups. Idempotent. Each part fails independently and is
 * reported through DEBUG; the return value is the first failure, so the BDS
 * can log it, but neither failure is fatal for the boot session.
 *
 * @retval EFI_SUCCESS       Both installed (or already installed).
 * @retval EFI_NOT_FOUND     A firmware protocol the fix-up needs is missing
 *                           (EFI_CPU_ARCH_PROTOCOL / EFI_RAMPARTITION_PROTOCOL).
 * @retval other             Protocol installation or allocation failure.
 */
EFI_STATUS
SfbMemFixupInstall (
  VOID
  );

/* Individual parts, for callers that need only one. Idempotent. */
EFI_STATUS
SfbMemFixupInstallMemoryAttribute (
  VOID
  );

EFI_STATUS
SfbMemFixupInstallMemmapMerge (
  VOID
  );

/*
 * Load CONFIG\MEMCFG from the efisp config container (which must be
 * mounted) into the override table applied on top of the merged
 * GetMemoryMap view: NAME BASE SIZE TYPE ATTRIBUTE per line, see
 * Library/SfbMemFixupLib/SfbMemCfg.h. A missing or unreadable file is an
 * empty table, not an error; invalid lines are skipped and counted in the
 * debug log. Replaces any previously loaded table. Installs the
 * GetMemoryMap hook if overrides exist and the merge did not install it.
 *
 * @retval EFI_SUCCESS   Always, unless the hook had to be installed and
 *                       that failed.
 */
EFI_STATUS
SfbMemFixupLoadConfig (
  VOID
  );

#endif /* __SFB_MEM_FIXUP_H__ */
