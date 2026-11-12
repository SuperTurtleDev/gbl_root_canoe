/*
 * Boot menu for the "super fastboot only" (TEST_ADAPTER) product.
 *
 * The loader carries its own FAT stack, so it can enumerate FAT32 volumes and
 * offer whatever removable/ESP boot loaders it finds there even on platforms
 * whose firmware exposes nothing but Block I/O.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_MENU_H__
#define __SUPER_FB_MENU_H__

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>

#include "SuperFbEntryList.h"

/* The removable-media boot loader looked for on every volume. */
#define SFB_BOOT_FILE_PATH  L"\\EFI\\BOOT\\BOOTAA64.EFI"

/* Optional boot entry list in a volume's root directory (SuperFbEntryList.h
 * describes the line format). */
#define SFB_VOLUME_ENTRIES_PATH  L"\\ENTRIES.CFG"

/* The stored, ordered boot entry list inside the efisp config container,
 * and the temporary file it is rewritten through. */
#define SFB_STORE_DIR_PATH       L"\\CONFIG"
#define SFB_STORE_ENTRIES_PATH   L"\\CONFIG\\ENTRIES.CFG"
#define SFB_STORE_TEMP_PATH      L"\\CONFIG\\ENTRIES.TMP"

/*
 * Optional file, looked for in a boot entry's own directory, naming UEFI driver
 * images to load and start before that entry is launched. One path per line,
 * on the same volume, leading '\' optional; blank lines and '#' comments are
 * ignored.
 */
#define SFB_DRIVER_LIST_NAME  L"DRIVER.LIST"

/* Upper bound on a DRIVER.LIST file. */
#define SFB_LIST_MAX_BYTES    8192

#define SFB_DESC_CHARS       48
#define SFB_MAX_DIR_ENTRIES  128

/* Deepest name hierarchy the menu descends into. */
#define SFB_MAX_MENU_DEPTH   8

typedef enum {
  SfbKeyTimeout = 0,
  SfbKeyUp,
  SfbKeyDown,
  SfbKeySelect
} SFB_KEY;

/* ---- SuperFbFat.c: embedded FAT/EXT4 stack and volume helpers ----------- */

/*
 * Install the embedded Unicode Collation, Disk I/O, FAT and read-only EXT4
 * drivers, then run the driver connection pass so FAT32 and ext4 volumes
 * surface as Simple File System instances. Safe to call more than once;
 * already-present platform drivers are left alone.
 */
EFI_STATUS
SfbStartFatStack (VOID);

/*
 * The volume list IS the UFS partition table plus the efisp config container:
 * the only volumes this loader ever enumerates, shared by the boot entry
 * scan, the file browser and the Volumes menu. Only entries with a supported
 * file system (FAT of any width or ext4) are returned. Firmware volumes, USB
 * media and all other non-UFS disks never appear. *Handles must be released
 * with FreePool ().
 */
EFI_STATUS
SfbLocateVolumes (OUT EFI_HANDLE **Handles, OUT UINTN *Count);

/* TRUE when the volume handle's block device holds a FAT12/16/32 volume. */
BOOLEAN
SfbIsFatVolume (IN EFI_HANDLE Volume);

/* TRUE when the volume handle's block device holds an ext4 file system. */
BOOLEAN
SfbIsExt4Volume (IN EFI_HANDLE Volume);

EFI_STATUS
SfbOpenVolumeRoot (IN EFI_HANDLE Volume, OUT EFI_FILE_PROTOCOL **Root);

/* TRUE when Path names an existing, readable, non-directory file. */
BOOLEAN
SfbFileExists (IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path);

/*
 * Read up to MaxBytes of the file at Path (under Root) into Buffer. *BytesRead
 * is set to how much was read. Fails only if the file cannot be opened or read;
 * a short file is not an error.
 */
EFI_STATUS
SfbReadFileBytes (IN EFI_FILE_PROTOCOL *Root,
                  IN CONST CHAR16      *Path,
                  OUT VOID             *Buffer,
                  IN UINTN             MaxBytes,
                  OUT UINTN            *BytesRead);

/*
 * TRUE when the file at Path (under Root) is a UEFI driver image rather than an
 * application, decided from the subsystem field of its PE header (boot-service
 * or runtime driver). FALSE for applications, unreadable files and non-PE data.
 */
BOOLEAN
SfbIsEfiDriverFile (IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path);

/*
 * Connect every controller in the system so that driver bindings installed by
 * a freshly loaded driver attach to the hardware they support.
 */
VOID
SfbConnectAll (VOID);

/* FAT volume label, or an empty string when unavailable. */
VOID
SfbGetVolumeLabel (IN EFI_FILE_PROTOCOL *Root,
                   OUT CHAR16           *Out,
                   IN UINTN             OutChars);

/*
 * (disk, partition) address of a volume in the volume table, both 0-based:
 * disk is the UFS LUN number (lun0 = disk 0, lun1 = disk 1, ...), partition
 * is the index of the partition in Ptable[Lun]. The efisp config container is
 * appended as the last disk (one past the highest LUN with partitions) at
 * part 0. EFI_NOT_FOUND for any handle outside the table.
 */
EFI_STATUS
SfbGetVolumeAddress (IN EFI_HANDLE Volume, OUT UINT32 *Disk, OUT UINT32 *Part);

/* The volume at (Disk, Part), or NULL. */
EFI_HANDLE
SfbFindVolumeByAddress (IN UINT32 Disk, IN UINT32 Part);

/* ---- SuperFbEntries.c: boot entries, persistence and launching ---------- */

/* The one in-memory boot entry list the boot flow, menu and Advanced share. */
SFB_ENTRY_LIST *
SfbEntries (VOID);

/*
 * Replace List with CONFIG\ENTRIES.CFG from the config container, in file
 * order. A missing file or unavailable store yields an empty list; entries
 * whose volume is absent stay in the list unresolved.
 */
EFI_STATUS
SfbLoadStoredEntries (IN OUT SFB_ENTRY_LIST *List);

/*
 * Scan every supported volume (EFI\BOOT\BOOTAA64.EFI and root ENTRIES.CFG)
 * and sync List with what was found: invalid entries are removed, valid ones
 * keep their order, new ones are appended. TRUE in *Changed when List moved.
 */
EFI_STATUS
SfbUpdateEntries (IN OUT SFB_ENTRY_LIST *List, OUT BOOLEAN *Changed OPTIONAL);

/*
 * Rewrite CONFIG\ENTRIES.CFG from List, atomically: temporary file, flush,
 * delete, rename, flush. Fails without side effects on boot when the store is
 * unavailable.
 */
EFI_STATUS
SfbWriteStoredEntries (IN CONST SFB_ENTRY_LIST *List);

/*
 * Steps 1-5 of the power-on flow, everything before the boot menu: load the
 * stored entries (scanning and storing when there are none), and, unless the
 * menu was requested or there is nothing to boot, try each entry in order,
 * dropping the ones whose image is not a loadable PE/COFF. Returns when the
 * boot menu should run.
 */
VOID
SfbRunStartup (IN BOOLEAN MenuRequested);

/* Fill in an entry for PathOnVolume on Volume, addressed by its (d,p). */
EFI_STATUS
SfbMakeFileEntry (IN EFI_HANDLE        Volume,
                  IN CONST CHAR16      *PathOnVolume,
                  IN CONST CHAR16      *Name,
                  OUT SFB_BOOT_ENTRY   *Entry);

/*
 * Load and start the image the entry points at. Only returns if the launch
 * failed or the started image returned. *NotPeImage is set TRUE when LoadImage
 * refused the file as an unsupported (non-PE/COFF or foreign-architecture)
 * image; EFI_NOT_FOUND, EFI_SECURITY_VIOLATION and the like leave it FALSE.
 *
 * ClearScreen controls the "Booting <name>" banner: TRUE clears the screen
 * first (menu-driven launch), FALSE leaves the current screen contents in place
 * (unattended boot, which must not blank the boot splash).
 */
EFI_STATUS
SfbLaunchEntry (IN CONST SFB_BOOT_ENTRY *Entry,
                IN BOOLEAN              ClearScreen,
                OUT BOOLEAN             *NotPeImage OPTIONAL);

/*
 * Load and start a single UEFI driver image named by a volume-relative path.
 * Does not run a connect pass; call SfbConnectAll () afterwards so the driver
 * binds to the devices it supports.
 */
EFI_STATUS
SfbLoadDriver (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path);

/* ---- SuperFbMenu.c: console UI ----------------------------------------- */

/*
 * Draw the boot menu and service it until something is launched. This is the
 * only entry point LinuxLoader needs.
 *
 * Returns TRUE when the user picked the built-in "Enter Fastboot" entry, which
 * the caller is expected to honour; FALSE means the menu has nothing left to do.
 */
BOOLEAN
SfbRunBootMenu (VOID);

/* Simple FAT32 browser: pick a volume, walk directories, act on a .efi. */
VOID
SfbRunFileBrowser (VOID);

/*
 * Browse one volume from an absolute path on it; VolumeLabel names the screen.
 * Returns TRUE when the caller should unwind back to the boot menu.
 */
BOOLEAN
SfbBrowseVolume (IN EFI_HANDLE   Volume,
                 IN CONST CHAR16 *VolumeLabel,
                 IN CONST CHAR16 *BrowseRoot);

/*
 * Clear the console and announce fastboot. Called on the way out of the menu so
 * the last thing the menu drew does not stay on screen while fastboot waits for
 * a host that may take a while to show up.
 */
VOID
SfbShowFastbootMode (VOID);

/*
 * Clear the console, show "Entering Boot Menu", and hold for a few seconds so
 * a volume key still held from power-on is released before the menu starts
 * taking input. The input buffer is drained afterwards so that held key does
 * not leak in as a spurious keypress.
 */
VOID
SfbShowEnteringMenu (VOID);

/*
 * Announce that an entry is being launched, so the menu the user picked from
 * does not stay on screen while the image loads. Title is "Booting <Name>".
 *
 * When ClearScreen is TRUE the console is cleared first (menu launch); when
 * FALSE the current screen is left as-is (unattended default boot).
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen);

/*
 * Clear the console, show "Entering <What>", and hold for one second so a
 * volume key held from the parent screen is released before the submenu starts
 * taking input. The input buffer is drained afterwards so that held key does
 * not leak in as a spurious keypress.
 */
VOID
SfbShowEnteringScreen (IN CONST CHAR16 *What);

/*
 * Debounce a menu exit (Back): hold briefly and drain the input queue so the
 * confirming key cannot act on the parent menu.
 */
VOID
SfbDebounceMenuExit (VOID);

/* Wait for a key. TimeoutMs of 0 waits indefinitely. */
SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs);

/* ---- shared console helpers (SuperFbMenu.c) ----------------------------- */

/* Rows of list content a screen shows before it starts scrolling. */
#define SFB_VISIBLE_ROWS  12

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle OPTIONAL);

VOID
SfbEndScreen (IN CONST CHAR16 *Footer);

VOID
SfbDrawRow (IN BOOLEAN      Selected,
            IN CONST CHAR16 *Marker,
            IN CONST CHAR16 *Text);

/* First row of the visible window, chosen to keep Cursor inside it. */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows);

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key);

/* Show a status line and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status);

#endif /* __SUPER_FB_MENU_H__ */
