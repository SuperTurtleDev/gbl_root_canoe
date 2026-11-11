/*
 * Boot entry records and the ENTRIES.CFG line format.
 *
 * One entry per line, line order is boot order:
 *
 *   <name>:[(<disk>,<partition>)]<absolute path on the volume> [args...]
 *
 *   Android Tools\Reboot Tools:(1,1)\efisp\tools\RebootTools.efi
 *   Android Tools\Reboot Tools:\efisp\tools\RebootTools.efi
 *
 * '\' in the name nests the entry in menus ("Android Tools" > "Reboot
 * Tools"). (d,p) names the p-th partition of the d-th whole disk the firmware
 * enumerates, both 0-based; without it the path is on the volume that holds
 * the ENTRIES.CFG being read. '/' is accepted for '\' in the path. Anything
 * after the first blank following the path is passed to the image as its
 * LoadOptions. Blank lines and lines starting with '#' are ignored.
 *
 * Nothing in this file touches firmware services, so it builds on the host
 * for the unit tests.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_ENTRY_LIST_H__
#define __SUPER_FB_ENTRY_LIST_H__

#include <Uefi.h>
#include <Protocol/DevicePath.h>

#define SFB_NAME_CHARS         96
#define SFB_PATH_CHARS         256
#define SFB_ARGS_CHARS         128
#define SFB_MAX_ENTRIES        64

/* Upper bound on an ENTRIES.CFG file, and on one line of it. */
#define SFB_ENTRIES_MAX_BYTES  32768
#define SFB_ENTRY_LINE_BYTES   (SFB_NAME_CHARS + SFB_PATH_CHARS + SFB_ARGS_CHARS + 32)

#define SFB_NO_INDEX           ((UINTN)-1)

/* What a line says, independent of what the firmware currently sees. */
typedef struct {
  CHAR16   Name[SFB_NAME_CHARS];    /* '\'-separated menu hierarchy */
  CHAR16   Path[SFB_PATH_CHARS];    /* absolute on the volume, leading '\' */
  CHAR16   Args[SFB_ARGS_CHARS];    /* LoadOptions; may be empty */
  BOOLEAN  HasAddress;              /* FALSE: the volume holding the file */
  UINT32   Disk;
  UINT32   Part;
} SFB_ENTRY_SPEC;

/* A spec plus where it resolved to this boot. */
typedef struct {
  SFB_ENTRY_SPEC            Spec;
  EFI_HANDLE                Volume;      /* NULL when it did not resolve */
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath; /* owned; NULL when unresolved */
} SFB_BOOT_ENTRY;

typedef struct {
  SFB_BOOT_ENTRY  Entry[SFB_MAX_ENTRIES];
  UINTN           Count;
} SFB_ENTRY_LIST;

/*
 * Copy the next line of the NUL-terminated text at *Cursor into Line and
 * advance past its newline. '\r' is dropped. FALSE once the text is used up.
 */
BOOLEAN
SfbEntryNextLine (
  IN OUT CONST CHAR8  **Cursor,
  OUT    CHAR8        *Line,
  IN     UINTN        LineBytes
  );

/* Parse one line. FALSE for blank/comment lines and malformed ones. */
BOOLEAN
SfbEntryParseLine (
  IN  CONST CHAR8     *Line,
  OUT SFB_ENTRY_SPEC  *Spec
  );

/* Encode Spec as one line, newline included. FALSE when it does not fit. */
BOOLEAN
SfbEntryFormatLine (
  IN  CONST SFB_ENTRY_SPEC  *Spec,
  OUT CHAR8                 *Out,
  IN  UINTN                 OutBytes
  );

/* Volume paths compared the way FAT compares them: ASCII case-insensitive. */
BOOLEAN
SfbEntryPathEqual (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  );

/*
 * TRUE when both specs launch the same thing: same address, same path
 * (ASCII case-insensitive, as FAT is) and same arguments. Names do not count.
 */
BOOLEAN
SfbEntrySameTarget (
  IN CONST SFB_ENTRY_SPEC  *A,
  IN CONST SFB_ENTRY_SPEC  *B
  );

/* Index of the entry with Spec's target, or SFB_NO_INDEX. */
UINTN
SfbEntryListFind (
  IN CONST SFB_ENTRY_LIST  *List,
  IN CONST SFB_ENTRY_SPEC  *Spec
  );

/*
 * Append Entry, taking ownership of its device path. A duplicate target or a
 * full list is refused: the entry's device path is released and FALSE is
 * returned.
 */
BOOLEAN
SfbEntryListAppend (
  IN OUT SFB_ENTRY_LIST  *List,
  IN OUT SFB_BOOT_ENTRY  *Entry
  );

/* Drop the entry at Index, keeping the order of the rest. */
VOID
SfbEntryListRemove (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     UINTN           Index
  );

/* Move the entry at From to position To, shifting the ones in between. */
VOID
SfbEntryListMove (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     UINTN           From,
  IN     UINTN           To
  );

/*
 * Bring the stored list up to date with a fresh scan: entries whose Valid[i]
 * is FALSE are removed, the rest keep their order, and every discovered
 * target not already present is appended in discovery order. Discovered is
 * consumed (left empty). TRUE when List changed.
 */
BOOLEAN
SfbEntryListSync (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     CONST BOOLEAN   *Valid,
  IN OUT SFB_ENTRY_LIST  *Discovered
  );

VOID
SfbEntryListFree (
  IN OUT SFB_ENTRY_LIST  *List
  );

/*
 * Launch one entry. *NotPeImage is TRUE when the image was refused as not a
 * loadable PE/COFF (or foreign-architecture) image.
 */
typedef
EFI_STATUS
(*SFB_ENTRY_LAUNCH) (
  IN  CONST SFB_BOOT_ENTRY  *Entry,
  OUT BOOLEAN               *NotPeImage,
  IN  VOID                  *Context
  );

/* Persist List. */
typedef
EFI_STATUS
(*SFB_ENTRY_STORE) (
  IN CONST SFB_ENTRY_LIST  *List,
  IN VOID                  *Context
  );

/*
 * Try every entry in list order. An entry whose launch reports a non-PE/COFF
 * image is removed and List is stored once per removal; every other failure
 * keeps the entry and moves on. Returns when the list is exhausted (an image
 * that boots never returns here).
 */
VOID
SfbEntryListBootInOrder (
  IN OUT SFB_ENTRY_LIST    *List,
  IN     SFB_ENTRY_LAUNCH  Launch,
  IN     SFB_ENTRY_STORE   Store,
  IN     VOID              *Context
  );

/*
 * Where Name sits relative to the menu level Prefix ("" for the root,
 * otherwise a name path without trailing '\'). FALSE when Name is not below
 * Prefix. Otherwise Child receives the next path component and *IsFolder
 * tells whether more components follow it.
 */
BOOLEAN
SfbEntryNameChild (
  IN  CONST CHAR16  *Name,
  IN  CONST CHAR16  *Prefix,
  OUT CHAR16        *Child,
  IN  UINTN         ChildChars,
  OUT BOOLEAN       *IsFolder
  );

#endif /* __SUPER_FB_ENTRY_LIST_H__ */
