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

/* The boot loader we look for on every FAT32 volume, and the optional ANSI
 * one-liner describing it. */
#define SFB_BOOT_FILE_PATH  L"\\EFI\\BOOT\\BOOTAA64.EFI"
#define SFB_DESC_FILE_PATH  L"\\EFI\\DESC"

/*
 * Optional file in a volume's root directory listing extra boot entries, one
 * per line:
 *
 *   <name>:<path relative to the boot root> [args...]
 *   %<name>:<path to another ENTRIES file relative to the boot root>
 *
 * e.g. "MEMTEST:EFI/MEMTEST.EFI". Either '/' or '\' separates path components,
 * a leading separator is optional, blank lines and lines starting with '#' are
 * ignored. A '$' prefix on the name marks a "no default" entry. Entries here
 * are listed alongside the auto-discovered boot loader.
 *
 * Anything after the first whitespace that follows the path is the entry's
 * argument line (e.g. "MAINLINE:EFI/MAINLINE.EFI --dtb boot.dtb"); it is passed
 * to the launched image as its LoadOptions the way the EDK2 shell passes a
 * command line.
 *
 * A line beginning with '%' names a submenu: the path points at another file in
 * the same BOOTENTRIES format whose entries are shown when the row is selected.
 * Paths inside that file are still relative to the boot root (the volume root
 * for FAT volumes, \efisp for ext4), not to the submenu file's own directory,
 * and the file may itself contain further '%' submenu rows, up to
 * SFB_MAX_SUBMENU_DEPTH levels deep.
 */
#define SFB_BOOTENTRIES_PATH  L"\\BOOTENTRIES"

/*
 * Optional file, looked for in a boot entry's own directory, naming UEFI driver
 * images to load and start before that entry is launched. One path per line,
 * each relative to the volume root, same line syntax as BOOTENTRIES.
 */
#define SFB_DRIVER_LIST_NAME  L"DRIVER.LIST"

/* Upper bound on the BOOTENTRIES / DRIVER.LIST text files we will read. */
#define SFB_LIST_MAX_BYTES    8192

#define SFB_DESC_CHARS       48
#define SFB_PATH_CHARS       256
/* Room for an EDK2-shell-style argument line, e.g. "--dtb boot.dtb". */
#define SFB_ARGS_CHARS       128
#define SFB_MAX_ENTRIES      24
#define SFB_MAX_DIR_ENTRIES  128

/* Deepest submenu nesting allowed. Bounds the recursion when a chain of
 * ENTRIES files points at one another; beyond this the menu shows "too deep"
 * rather than descending further. */
#define SFB_MAX_SUBMENU_DEPTH  8

#define SFB_NO_INDEX  ((UINTN)-1)

typedef enum {
  /* An EFI application living on a FAT32/ext4 volume. */
  SfbEntryEfiFile = 0,
  /* A pointer to another ENTRIES file: selecting it opens that file as a
   * submenu. Volume/Path name the ENTRIES file; Desc is the submenu title. */
  SfbEntrySubmenu,
  /* Built-in entries; no backing file, handled in code. */
  SfbEntryFastboot,
  SfbEntrySelector,
  /* Opens the Advanced submenu (USB mass-storage export and friends). */
  SfbEntryAdvanced,
  /* "Back" row at the foot of a submenu: returns to the parent menu. */
  SfbEntryBack,
  /* Power management actions offered at the end of the menu and on the
   * fastboot mode screen. */
  SfbEntryPowerOff,
  SfbEntryRestart
} SFB_ENTRY_KIND;

typedef struct {
  SFB_ENTRY_KIND            Kind;
  /* TRUE when the entry was restored from the custom-entry store record
   * rather than discovered by scanning. */
  BOOLEAN                   IsCustom;
  /* TRUE for entries whose BOOTENTRIES name began with '$': they are listed and
   * bootable, but selecting one never overwrites the saved default. */
  BOOLEAN                   NoDefault;
  CHAR16                    Desc[SFB_DESC_CHARS];
  CHAR16                    Path[SFB_PATH_CHARS];
  /*
   * Optional launch arguments from the BOOTENTRIES line, passed to the image
   * as its LoadOptions the way the EDK2 shell passes a command line: space
   * separated, NUL terminated, image name not included.
   */
  CHAR16                    Args[SFB_ARGS_CHARS];
  /* FAT volume label the entry lives on; how a stored entry finds its way
   * back to a volume after a reboot has renumbered the handles. */
  CHAR16                    VolLabel[SFB_DESC_CHARS];
  EFI_HANDLE                Volume;
  /* Owned by the entry; NULL for the built-in kinds. */
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
} SFB_BOOT_ENTRY;

typedef struct {
  SFB_BOOT_ENTRY  Entry[SFB_MAX_ENTRIES];
  UINTN           Count;
  /* Entry the menu highlights first, or SFB_NO_INDEX. This may be the stored
   * default or, absent one, the first on-device entry used as a starting
   * point for the cursor and the "*" marker. */
  UINTN           DefaultIndex;
  /* TRUE only when DefaultIndex came from a stored default record, not from
   * the first-entry fallback. A power-on with no key pressed boots the default
   * straight away only when this is TRUE; otherwise the menu is shown. */
  BOOLEAN         DefaultIsPersisted;
} SFB_MENU_STATE;

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
 * *Handles must be released with FreePool ().
 *
 * The list serves the boot-entry scanner, so it is deliberately narrower than
 * "anything with a file system": FAT volumes of any width (FAT12/16/32,
 * including an efisp.fat blob mounted from the ext4 persist partition) are
 * scanned at their root, and an ext4 volume is kept when it carries the
 * legacy \efisp boot directory. The file browser builds its own list instead
 * and lists every Simple File System it finds.
 */
EFI_STATUS
SfbLocateVolumes (OUT EFI_HANDLE **Handles, OUT UINTN *Count);

/* TRUE when the volume handle's block device holds a FAT12/16/32 volume. */
BOOLEAN
SfbIsFatVolume (IN EFI_HANDLE Volume);

/* TRUE when the volume handle's block device holds an ext4 file system. */
BOOLEAN
SfbIsExt4Volume (IN EFI_HANDLE Volume);

/*
 * The volume-relative directory that acts as the boot root: "" for FAT32 (its
 * root already is) and "\efisp" for the ext4 persist partition. The scanner
 * prepends this to \EFI\BOOT\BOOTAA64.EFI and friends; the browser starts
 * browsing here.
 */
CONST CHAR16 *
SfbVolumeRootPrefix (IN EFI_HANDLE Volume);

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

/*
 * Read an ANSI text file and return its first line as a Unicode string.
 * Out is left untouched when the file is missing or empty.
 */
VOID
SfbReadAnsiDescription (IN EFI_FILE_PROTOCOL *Root,
                        IN CONST CHAR16      *Path,
                        OUT CHAR16           *Out,
                        IN UINTN             OutChars);

/* FAT volume label, or an empty string when unavailable. */
VOID
SfbGetVolumeLabel (IN EFI_FILE_PROTOCOL *Root,
                   OUT CHAR16           *Out,
                   IN UINTN             OutChars);

/* ---- efisp.fat blob mounts (SuperFbFat.c) -------------------------------- */

/* Number of efisp.fat blobs mounted so far this boot. */
UINTN
SfbFatBlobCount (VOID);

/* The ext4 volume handle the blob at Index was mounted from. */
EFI_HANDLE
SfbFatBlobSource (IN UINTN Index);

/*
 * The published image disk at Index, or NULL while it is withdrawn for a raw
 * USB export. The BlockIo stays owned by the mount.
 */
EFI_BLOCK_IO_PROTOCOL *
SfbFatBlobDisk (IN UINTN Index);

/* Withdraw the disk for an exclusive export session; Restore publishes it
 * again and rebinds FAT when the session ends. ImageDisk returns the block
 * device regardless of publish state, for the export session itself. */
EFI_STATUS
SfbFatBlobWithdraw (IN UINTN Index);
EFI_STATUS
SfbFatBlobRestore (IN UINTN Index);
EFI_BLOCK_IO_PROTOCOL *
SfbFatBlobImageDisk (IN UINTN Index);

/* ---- SuperFbStore.c: settings kept in the tail of the ESP ---------------- */

/*
 * The firmware on this platform rejects variables it does not know, so the two
 * things the menu has to remember outlive a reboot in the EFI System Partition
 * instead: two 1 KiB NUL-padded ASCII records written to the very end of the
 * partition, which is the only part of it that is safe to touch.
 */
#define SFB_STORE_SLOT_BYTES  1024
#define SFB_STORE_SLOTS       2

#define SFB_STORE_DEFAULT  0   /* the entry the menu timeout launches */
#define SFB_STORE_CUSTOM   1   /* the single user-added menu entry */

/*
 * Replace one record. Text is NUL-terminated ASCII of at most
 * SFB_STORE_SLOT_BYTES - 1 bytes; passing an empty string clears the slot.
 */
EFI_STATUS
SfbStoreWrite (IN UINTN Slot, IN CONST CHAR8 *Text);

/*
 * Read one record. Out is always NUL-terminated, and empty when the slot has
 * never been written. Fails only when the store itself is unreachable.
 */
EFI_STATUS
SfbStoreRead (IN UINTN Slot, OUT CHAR8 *Out, IN UINTN OutBytes);

/* ---- SuperFbEntries.c: entry list, persistence and launching ------------ */

VOID
SfbBuildMenu (OUT SFB_MENU_STATE *Menu);

/*
 * Build a submenu from an ENTRIES file at EntriesPath (an absolute volume path
 * on Volume) and append a trailing "Back" row. The ENTRIES file has the same
 * format as BOOTENTRIES, and every path inside it is resolved relative to the
 * same boot root (volume root for FAT32, \efisp for ext4). Returns
 * EFI_INVALID_PARAMETER for a null Volume/path; EFI_SUCCESS otherwise (an empty
 * or unreadable file simply yields a menu holding only "Back").
 */
EFI_STATUS
SfbBuildSubMenu (OUT SFB_MENU_STATE *Menu,
                 IN EFI_HANDLE      Volume,
                 IN CONST CHAR16    *EntriesPath);

VOID
SfbFreeMenu (IN OUT SFB_MENU_STATE *Menu);

/* Persist Entry as the entry the menu timeout launches. */
EFI_STATUS
SfbSaveDefaultEntry (IN CONST SFB_BOOT_ENTRY *Entry);

/* Persist Entry as the single user-added boot menu entry, replacing any
 * previous one. */
EFI_STATUS
SfbSaveCustomEntry (IN CONST SFB_BOOT_ENTRY *Entry);

/*
 * Load and start the image the entry points at. Records the entry as the new
 * default first unless Temporary is TRUE. Only returns if the launch failed or
 * the started image returned.
 *
 * ClearScreen controls the "Booting <name>" banner: TRUE clears the screen
 * first (menu-driven launch), FALSE leaves the current screen contents in place
 * (unattended default boot, which must not blank the boot splash).
 */
EFI_STATUS
SfbLaunchEntry (IN CONST SFB_BOOT_ENTRY *Entry,
                IN BOOLEAN              Temporary,
                IN BOOLEAN              ClearScreen);

/*
 * Load and start a single UEFI driver image named by a volume-relative path.
 * Does not run a connect pass; call SfbConnectAll () afterwards so the driver
 * binds to the devices it supports.
 */
EFI_STATUS
SfbLoadDriver (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path);

/*
 * Launch the stored default entry, if one is configured. Returns TRUE when a
 * persisted default existed and was attempted (on success the launched image
 * takes over and this never returns; on failure it returns TRUE and the caller
 * should fall back to the menu). Returns FALSE when no default is configured,
 * so the caller shows the menu instead.
 */
BOOLEAN
SfbLaunchDefaultEntry (VOID);

/* Fill in an entry describing PathOnVolume on Volume. */
EFI_STATUS
SfbMakeFileEntry (IN EFI_HANDLE        Volume,
                  IN CONST CHAR16      *PathOnVolume,
                  IN CONST CHAR16      *Desc,
                  OUT SFB_BOOT_ENTRY   *Entry);

VOID
SfbFreeEntry (IN OUT SFB_BOOT_ENTRY *Entry);

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
