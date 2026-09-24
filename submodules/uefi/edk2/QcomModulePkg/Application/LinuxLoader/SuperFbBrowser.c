/*
 * Simple FAT32 file browser for the super-fastboot boot menu.
 *
 * Pick a volume, walk directories with the volume keys, and open an EFI
 * application with power to boot it once or add it to the boot menu.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbBrowserModuleTag = "SuperFbBrowser";

/* Longest single file name we will read out of a directory. */
#define SFB_NAME_CHARS  128

typedef struct {
  CHAR16   Name[SFB_NAME_CHARS];
  BOOLEAN  IsDir;
  /* The synthetic ".." row: leaves the directory, or the volume when at root. */
  BOOLEAN  IsParent;
} SFB_DIR_ENTRY;

/* ---- path helpers ------------------------------------------------------- */

STATIC
BOOLEAN
SfbIsRootPath (IN CONST CHAR16 *Path)
{
  return (BOOLEAN)(Path[0] == L'\\' && Path[1] == L'\0');
}

STATIC
VOID
SfbJoinPath (IN OUT CHAR16   *Path,
             IN UINTN        PathChars,
             IN CONST CHAR16 *Name)
{
  if (!SfbIsRootPath (Path)) {
    StrnCatS (Path, PathChars, L"\\", PathChars - StrLen (Path) - 1);
  }
  StrnCatS (Path, PathChars, Name, PathChars - StrLen (Path) - 1);
}

STATIC
VOID
SfbParentPath (IN OUT CHAR16 *Path)
{
  UINTN  Index;

  if (SfbIsRootPath (Path)) {
    return;
  }

  for (Index = StrLen (Path); Index > 0; Index--) {
    if (Path[Index - 1] == L'\\') {
      break;
    }
  }

  /* Index now sits just past the separator that starts the last component. */
  if (Index <= 1) {
    Path[0] = L'\\';
    Path[1] = L'\0';
  } else {
    Path[Index - 1] = L'\0';
  }
}

STATIC
BOOLEAN
SfbIsEfiFile (IN CONST CHAR16 *Name)
{
  UINTN         Length = StrLen (Name);
  CONST CHAR16  *Ext;

  if (Length < 5) {
    return FALSE;
  }

  Ext = Name + Length - 4;

  return (BOOLEAN)(Ext[0] == L'.' &&
                   (Ext[1] == L'e' || Ext[1] == L'E') &&
                   (Ext[2] == L'f' || Ext[2] == L'F') &&
                   (Ext[3] == L'i' || Ext[3] == L'I'));
}

/* ---- directory listing -------------------------------------------------- */

/* Parent row first, then directories, then files, each alphabetically. */
STATIC
INTN
SfbCompareDirEntries (IN CONST SFB_DIR_ENTRY *A, IN CONST SFB_DIR_ENTRY *B)
{
  UINTN  RankA = A->IsParent ? 0 : (A->IsDir ? 1 : 2);
  UINTN  RankB = B->IsParent ? 0 : (B->IsDir ? 1 : 2);

  if (RankA != RankB) {
    return (RankA < RankB) ? -1 : 1;
  }

  return StrCmp (A->Name, B->Name);
}

STATIC
VOID
SfbSortDirEntries (IN OUT SFB_DIR_ENTRY *List, IN UINTN Count)
{
  UINTN          Index;
  UINTN          Probe;
  SFB_DIR_ENTRY  Pending;

  for (Index = 1; Index < Count; Index++) {
    CopyMem (&Pending, &List[Index], sizeof (Pending));

    for (Probe = Index;
         Probe > 0 && SfbCompareDirEntries (&List[Probe - 1], &Pending) > 0;
         Probe--) {
      CopyMem (&List[Probe], &List[Probe - 1], sizeof (Pending));
    }

    CopyMem (&List[Probe], &Pending, sizeof (Pending));
  }
}

/*
 * Fill List with the contents of Dir, preceded by a synthetic ".." row.
 * Truncated is set when the directory holds more than SFB_MAX_DIR_ENTRIES
 * items, so the caller can say so rather than silently hiding them.
 */
STATIC
EFI_STATUS
SfbReadDirectory (IN EFI_FILE_PROTOCOL  *Dir,
                  OUT SFB_DIR_ENTRY     *List,
                  IN UINTN              Max,
                  OUT UINTN             *Count,
                  OUT BOOLEAN           *Truncated)
{
  EFI_STATUS     Status;
  EFI_FILE_INFO  *Info;
  UINTN          InfoSize;
  UINTN          BufferSize;

  *Count = 0;
  *Truncated = FALSE;

  if (Max == 0) {
    return EFI_INVALID_PARAMETER;
  }

  /* The ".." row always exists: at the root it backs out to the volume list. */
  ZeroMem (&List[0], sizeof (List[0]));
  StrCpyS (List[0].Name, SFB_NAME_CHARS, L"..");
  List[0].IsDir = TRUE;
  List[0].IsParent = TRUE;
  *Count = 1;

  InfoSize = sizeof (EFI_FILE_INFO) + SFB_NAME_CHARS * sizeof (CHAR16);
  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Dir->SetPosition (Dir, 0);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    return Status;
  }

  while (TRUE) {
    BufferSize = InfoSize;
    Status = Dir->Read (Dir, &BufferSize, Info);

    if (Status == EFI_BUFFER_TOO_SMALL) {
      /* A name longer than we budgeted for; grow once and retry this entry. */
      EFI_FILE_INFO  *Bigger = AllocateZeroPool (BufferSize);

      if (Bigger == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }
      FreePool (Info);
      Info = Bigger;
      InfoSize = BufferSize;
      continue;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    /* A zero-length read marks the end of the directory. */
    if (BufferSize == 0) {
      Status = EFI_SUCCESS;
      break;
    }

    /* We supply our own parent row and have no use for ".". */
    if (StrCmp (Info->FileName, L".") == 0 ||
        StrCmp (Info->FileName, L"..") == 0) {
      continue;
    }

    if (*Count >= Max) {
      *Truncated = TRUE;
      Status = EFI_SUCCESS;
      break;
    }

    ZeroMem (&List[*Count], sizeof (List[0]));
    StrnCpyS (List[*Count].Name, SFB_NAME_CHARS, Info->FileName,
              SFB_NAME_CHARS - 1);
    List[*Count].IsDir =
      (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) != 0);
    (*Count)++;
  }

  FreePool (Info);

  SfbSortDirEntries (List, *Count);

  return Status;
}

STATIC
EFI_STATUS
SfbOpenDirectory (IN EFI_HANDLE          Volume,
                  IN CONST CHAR16        *Path,
                  OUT EFI_FILE_PROTOCOL  **Root,
                  OUT EFI_FILE_PROTOCOL  **Dir)
{
  EFI_STATUS  Status;

  *Root = NULL;
  *Dir = NULL;

  Status = SfbOpenVolumeRoot (Volume, Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (SfbIsRootPath (Path)) {
    *Dir = *Root;
    return EFI_SUCCESS;
  }

  Status = (*Root)->Open (*Root, Dir, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    (*Root)->Close (*Root);
    *Root = NULL;
    *Dir = NULL;
  }

  return Status;
}

/* ---- action menu for a chosen EFI application --------------------------- */

/*
 * A UEFI driver is loaded, not booted: it installs protocols and returns rather
 * than taking over the machine. Offer just that. Never unwinds to the boot menu
 * (a driver is not a boot entry), so this always returns FALSE.
 */
STATIC
BOOLEAN
SfbDriverActionMenu (IN EFI_HANDLE   Volume,
                     IN CONST CHAR16 *FullPath)
{
  STATIC CONST CHAR16  *Actions[] = {
    L"Load",
    L"Back"
  };

  UINTN  Cursor = 0;

  while (TRUE) {
    UINTN       Index;
    SFB_KEY     Key;
    EFI_STATUS  Status;

    SfbBeginScreen (L"EFI Driver", FullPath);

    for (Index = 0; Index < ARRAY_SIZE (Actions); Index++) {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Actions[Index]);
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, ARRAY_SIZE (Actions), Key);
      continue;
    }

    if (Cursor == 0) {
      /* Load: start the driver, then connect controllers so it binds. */
      Status = SfbLoadDriver (Volume, FullPath);
      if (!EFI_ERROR (Status)) {
        SfbConnectAll ();
      }
      SfbReportStatus (EFI_ERROR (Status) ? L"Driver load failed"
                                          : L"Driver loaded", Status);
      continue;
    }

    return FALSE;
  }
}

/*
 * Offer what can be done with one .efi. Returns TRUE when the browser should
 * unwind all the way back to the boot menu, which is what adding an entry does
 * so the user immediately sees it listed.
 */
STATIC
BOOLEAN
SfbEfiActionMenu (IN EFI_HANDLE   Volume,
                  IN CONST CHAR16 *FullPath,
                  IN CONST CHAR16 *Name)
{
  STATIC CONST CHAR16  *Actions[] = {
    L"Boot (temporary)",
    L"Add to BootMenu",
    L"Back"
  };

  EFI_STATUS         Status;
  SFB_BOOT_ENTRY     Entry;
  UINTN              Cursor = 0;
  BOOLEAN            IsDriver = FALSE;
  EFI_FILE_PROTOCOL  *Root = NULL;

  /* A driver image gets its own Load/Back menu rather than the boot actions. */
  if (!EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) && Root != NULL) {
    IsDriver = SfbIsEfiDriverFile (Root, FullPath);
    Root->Close (Root);
  }
  if (IsDriver) {
    return SfbDriverActionMenu (Volume, FullPath);
  }

  Status = SfbMakeFileEntry (Volume, FullPath, Name, &Entry);
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Cannot address that file", Status);
    return FALSE;
  }

  while (TRUE) {
    UINTN    Index;
    SFB_KEY  Key;

    SfbBeginScreen (L"EFI Application", FullPath);

    for (Index = 0; Index < ARRAY_SIZE (Actions); Index++) {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Actions[Index]);
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, ARRAY_SIZE (Actions), Key);
      continue;
    }

    if (Cursor == 0) {
      /* Temporary: deliberately does not touch the default-entry variable.
       * Menu-driven launch, so clear the screen for the "Booting" banner. */
      Status = SfbLaunchEntry (&Entry, TRUE, TRUE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      continue;
    }

    if (Cursor == 1) {
      Status = SfbSaveCustomEntry (&Entry);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Could not save entry", Status);
        continue;
      }
      SfbReportStatus (L"Added to boot menu", Status);
      SfbFreeEntry (&Entry);
      return TRUE;
    }

    SfbFreeEntry (&Entry);
    return FALSE;
  }
}

/* ---- directory navigation ----------------------------------------------- */

/*
 * Returns TRUE when the browser should unwind back to the boot menu. Public:
 * the Advanced volume browser opens individual volumes through it.
 */
BOOLEAN
SfbBrowseVolume (IN EFI_HANDLE   Volume,
                 IN CONST CHAR16 *VolumeLabel,
                 IN CONST CHAR16 *BrowseRoot)
{
  CHAR16         Path[SFB_PATH_CHARS];
  SFB_DIR_ENTRY  *List;
  UINTN          Count = 0;
  UINTN          Cursor = 0;
  BOOLEAN        Truncated = FALSE;
  BOOLEAN        Reload = TRUE;

  List = AllocateZeroPool (SFB_MAX_DIR_ENTRIES * sizeof (*List));
  if (List == NULL) {
    SfbReportStatus (L"Out of memory", EFI_OUT_OF_RESOURCES);
    return FALSE;
  }

  /* Start at the volume's browse root: "\" for FAT32, "\efisp" for the ext4
   * persist volume. It is also the floor: ".." there backs out to the volume
   * list rather than climbing above it. */
  StrCpyS (Path, SFB_PATH_CHARS, BrowseRoot);

  while (TRUE) {
    UINTN                Start;
    UINTN                Last;
    UINTN                Index;
    SFB_KEY              Key;
    CONST SFB_DIR_ENTRY  *Selected;
    CHAR16               FullPath[SFB_PATH_CHARS];

    if (Reload) {
      EFI_FILE_PROTOCOL  *Root = NULL;
      EFI_FILE_PROTOCOL  *Dir = NULL;
      EFI_STATUS         Status;

      Status = SfbOpenDirectory (Volume, Path, &Root, &Dir);
      if (!EFI_ERROR (Status)) {
        Status = SfbReadDirectory (Dir, List, SFB_MAX_DIR_ENTRIES,
                                   &Count, &Truncated);
        if (Dir != Root) {
          Dir->Close (Dir);
        }
        Root->Close (Root);
      }

      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Cannot read directory", Status);
        if (StrCmp (Path, BrowseRoot) == 0) {
          /* The browse root itself is unusable; give up on this volume. */
          break;
        }
        SfbParentPath (Path);
        continue;
      }

      Cursor = 0;
      Reload = FALSE;
    }

    SfbBeginScreen (VolumeLabel, Path);

    Start = SfbWindowStart (Cursor, Count, SFB_VISIBLE_ROWS);
    Last = Start + SFB_VISIBLE_ROWS;
    if (Last > Count) {
      Last = Count;
    }

    for (Index = Start; Index < Last; Index++) {
      CONST CHAR16  *Marker;

      if (List[Index].IsDir) {
        Marker = L"[D]";
      } else if (SfbIsEfiFile (List[Index].Name)) {
        Marker = L"[E]";
      } else {
        Marker = L"   ";
      }

      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, List[Index].Name);
    }

    if (Last < Count) {
      Print (L"    ... %u more\r\n", (UINT32)(Count - Last));
    }
    if (Truncated) {
      Print (L"    (directory has more than %u entries; rest not shown)\r\n",
             (UINT32)SFB_MAX_DIR_ENTRIES);
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: open");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Count, Key);
      continue;
    }

    if (Count == 0) {
      continue;
    }

    Selected = &List[Cursor];

    if (Selected->IsParent) {
      if (StrCmp (Path, BrowseRoot) == 0) {
        /* Already at the browse root: back out to the volume list. */
        break;
      }
      SfbParentPath (Path);
      Reload = TRUE;
      continue;
    }

    if (Selected->IsDir) {
      SfbJoinPath (Path, SFB_PATH_CHARS, Selected->Name);
      Reload = TRUE;
      continue;
    }

    if (!SfbIsEfiFile (Selected->Name)) {
      SfbReportStatus (L"Not an EFI application", EFI_UNSUPPORTED);
      continue;
    }

    StrCpyS (FullPath, SFB_PATH_CHARS, Path);
    SfbJoinPath (FullPath, SFB_PATH_CHARS, Selected->Name);

    if (SfbEfiActionMenu (Volume, FullPath, Selected->Name)) {
      FreePool (List);
      return TRUE;
    }

    /* A temporary boot may have changed the volume underneath us. */
    Reload = TRUE;
  }

  FreePool (List);

  return FALSE;
}

/* ---- volume selection --------------------------------------------------- */

typedef struct {
  CHAR16  Label[SFB_DESC_CHARS];
} SFB_VOLUME_ROW;

VOID
SfbRunFileBrowser (VOID)
{
  EFI_STATUS      Status;
  EFI_HANDLE      *Volumes = NULL;
  UINTN           VolumeCount = 0;
  SFB_VOLUME_ROW  *Rows = NULL;
  UINTN           RowCount;
  UINTN           Cursor = 0;
  UINTN           Index;

  SfbShowEnteringScreen (L"File Browser");

  /* Media may have been inserted since the loader started. */
  SfbStartFatStack ();

  /*
   * The browser lists every volume that carries a Simple File System, however
   * the firmware produced it - removable FAT sticks, the ext4 persist
   * partition, mounted efisp.fat blobs, anything else recognisable. Browsing
   * always starts at the volume's own root; no per-type prefix is applied.
   */
  Status = gBS->LocateHandleBuffer (ByProtocol,
                                    &gEfiSimpleFileSystemProtocolGuid,
                                    NULL, &VolumeCount, &Volumes);
  if (EFI_ERROR (Status) || Volumes == NULL || VolumeCount == 0) {
    SfbReportStatus (L"No file system volumes found",
                     EFI_ERROR (Status) ? Status : EFI_NOT_FOUND);
    if (Volumes != NULL) {
      FreePool (Volumes);
    }
    return;
  }

  Rows = AllocateZeroPool (VolumeCount * sizeof (*Rows));
  if (Rows == NULL) {
    SfbReportStatus (L"Out of memory", EFI_OUT_OF_RESOURCES);
    FreePool (Volumes);
    return;
  }

  for (Index = 0; Index < VolumeCount; Index++) {
    EFI_FILE_PROTOCOL  *Root = NULL;
    CHAR16             Label[SFB_DESC_CHARS];

    Label[0] = L'\0';
    if (!EFI_ERROR (SfbOpenVolumeRoot (Volumes[Index], &Root)) &&
        Root != NULL) {
      SfbGetVolumeLabel (Root, Label, SFB_DESC_CHARS);
      Root->Close (Root);
    }

    /* Tag the ext4 persist volume so it is told apart from FAT32 media. */
    if (SfbIsExt4Volume (Volumes[Index])) {
      if (Label[0] != L'\0') {
        StrnCatS (Label, SFB_DESC_CHARS, L" (ext4)",
                  SFB_DESC_CHARS - StrLen (Label) - 1);
      } else {
        StrCpyS (Label, SFB_DESC_CHARS, L"ext4");
      }
    }

    if (Label[0] == L'\0') {
      UnicodeSPrint (Rows[Index].Label, sizeof (Rows[Index].Label),
                     L"Volume %u", (UINT32)Index);
    } else {
      UnicodeSPrint (Rows[Index].Label, sizeof (Rows[Index].Label),
                     L"Volume %u: %s", (UINT32)Index, Label);
    }
  }

  /* One extra row for "Back". */
  RowCount = VolumeCount + 1;

  while (TRUE) {
    UINTN    Start;
    UINTN    Last;
    SFB_KEY  Key;

    SfbBeginScreen (L"EFI Program Selector", L"Choose a volume to browse.");

    Start = SfbWindowStart (Cursor, RowCount, SFB_VISIBLE_ROWS);
    Last = Start + SFB_VISIBLE_ROWS;
    if (Last > RowCount) {
      Last = RowCount;
    }

    for (Index = Start; Index < Last; Index++) {
      if (Index == VolumeCount) {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", L"Back");
      } else {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L"[V]", Rows[Index].Label);
      }
    }

    if (Last < RowCount) {
      Print (L"    ... %u more\r\n", (UINT32)(RowCount - Last));
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, RowCount, Key);
      continue;
    }

    if (Cursor == VolumeCount) {
      SfbDebounceMenuExit ();
      break;
    }

    {
      /* Every volume is browsed from its own root, FAT and ext4 alike; the
       * per-type "\efisp" floor the scanner still applies is deliberately not
       * applied here. */
      if (SfbBrowseVolume (Volumes[Cursor], Rows[Cursor].Label, L"\\")) {
        break;
      }
    }
  }

  FreePool (Rows);
  FreePool (Volumes);
}

