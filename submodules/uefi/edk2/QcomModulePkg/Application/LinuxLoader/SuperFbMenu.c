/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"
#include "SuperFbAdvanced.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      TimerEvent = NULL;
    } else {
      /* Boot services timers count in 100ns units. */
      Status = gBS->SetTimer (TimerEvent, TimerRelative,
                              (UINT64)TimeoutMs * 10000);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (TimerEvent);
        TimerEvent = NULL;
      }
    }
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitCount = 1;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 1) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /*
     * On the handset the Qualcomm keypad driver reports the volume keys as
     * SCAN_UP and SCAN_DOWN, and power arrives as a carriage return.
     *
     * Anything left over counts as confirm: on a three-key handset there is
     * nothing else it can be, so the menu stays usable even if a platform
     * reports power differently from what is expected here.
     */
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      DEBUG ((EFI_D_VERBOSE, "SFB: confirm key scan=0x%x char=0x%x\n",
              Key.ScanCode, Key.UnicodeChar));
      Result = SfbKeySelect;
    }
    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"%s\r\n", Title);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  if (Subtitle != NULL) {
    Print (L"%s\r\n", Subtitle);
  }
  Print (L"\r\n");
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s\r\n", Footer);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  gST->ConOut->SetAttribute (gST->ConOut,
                             Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
  Print (L"%s %s %s", Selected ? L">" : L" ", Marker, Text);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n");
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s: %r\r\n", What, Status);
  Print (L"Press power to continue.\r\n");
  SfbWaitForKey (0);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (VOID)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"FASTBOOT MODE\r\n");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name, IN BOOLEAN ClearScreen)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Booting %s\r\n", (Name != NULL && Name[0] != L'\0') ? Name : L"...");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"%s\r\n", Text);

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Entering Boot Menu\r\n");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- shared screens ------------------------------------------------------- */

/*
 * Seconds to hold on an "Entering <submenu>" screen before the submenu starts
 * taking input. Short of the root menu's three-second hold on purpose: the
 * parent's confirm keystroke is already consumed, this only covers a key still
 * held or its trailing repeat events.
 */
#define SFB_ENTERING_SCREEN_DELAY_S  1

VOID
SfbShowEnteringScreen (IN CONST CHAR16 *What)
{
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Entering %s\r\n", (What != NULL) ? What : L"...");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);

  /* Wait for the key to be released, then drop anything typed or held during
   * the wait so it does not leak into the submenu as a spurious keypress. */
  gBS->Stall (SFB_ENTERING_SCREEN_DELAY_S * 1000 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/*
 * Debounce a menu exit: the key that confirmed Back would otherwise leak into
 * the parent menu (or its trailing repeats would) and act there immediately.
 * Hold for the entering-screen delay, then drop everything queued.
 */
VOID
SfbDebounceMenuExit (VOID)
{
  gBS->Stall (SFB_ENTERING_SCREEN_DELAY_S * 1000 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- boot menu ---------------------------------------------------------- */

typedef enum {
  SfbRowEntry = 0,   /* launches List->Entry[Entry] */
  SfbRowFolder,      /* opens the next level of the name hierarchy */
  SfbRowBack,
  SfbRowAdvanced,
  SfbRowPowerOff,
  SfbRowRestart
} SFB_ROW_KIND;

typedef struct {
  SFB_ROW_KIND  Kind;
  UINTN         Entry;
  CHAR16        Text[SFB_NAME_CHARS];
} SFB_MENU_ROW;

#define SFB_MAX_MENU_ROWS  (SFB_MAX_ENTRIES + 4)

STATIC
VOID
SfbAddRow (IN OUT SFB_MENU_ROW  *Rows,
           IN OUT UINTN         *Count,
           IN SFB_ROW_KIND      Kind,
           IN UINTN             Entry,
           IN CONST CHAR16      *Text)
{
  if (*Count >= SFB_MAX_MENU_ROWS) {
    return;
  }
  Rows[*Count].Kind = Kind;
  Rows[*Count].Entry = Entry;
  StrnCpyS (Rows[*Count].Text, SFB_NAME_CHARS, Text, SFB_NAME_CHARS - 1);
  (*Count)++;
}

/*
 * The rows of one menu level: the entries directly under Prefix, in list
 * order, with every deeper group folded into one folder row where its first
 * entry sits. The root ends with the built-ins; other levels with Back.
 */
STATIC
UINTN
SfbBuildRows (IN CONST SFB_ENTRY_LIST *List,
              IN CONST CHAR16         *Prefix,
              OUT SFB_MENU_ROW        *Rows)
{
  UINTN    Count = 0;
  UINTN    Index;
  UINTN    Row;
  CHAR16   Child[SFB_NAME_CHARS];
  BOOLEAN  IsFolder;
  BOOLEAN  Seen;

  for (Index = 0; Index < List->Count; Index++) {
    if (!SfbEntryNameChild (List->Entry[Index].Spec.Name, Prefix, Child,
                            ARRAY_SIZE (Child), &IsFolder)) {
      continue;
    }
    if (!IsFolder) {
      SfbAddRow (Rows, &Count, SfbRowEntry, Index, Child);
      continue;
    }
    Seen = FALSE;
    for (Row = 0; Row < Count; Row++) {
      if (Rows[Row].Kind == SfbRowFolder && StrCmp (Rows[Row].Text, Child) == 0) {
        Seen = TRUE;
        break;
      }
    }
    if (!Seen) {
      SfbAddRow (Rows, &Count, SfbRowFolder, SFB_NO_INDEX, Child);
    }
  }

  if (Prefix[0] == L'\0') {
    SfbAddRow (Rows, &Count, SfbRowAdvanced, SFB_NO_INDEX, L"Advanced");
    SfbAddRow (Rows, &Count, SfbRowPowerOff, SFB_NO_INDEX, L"Power Off");
    SfbAddRow (Rows, &Count, SfbRowRestart, SFB_NO_INDEX, L"Restart");
  } else {
    SfbAddRow (Rows, &Count, SfbRowBack, SFB_NO_INDEX, L"Back");
  }
  return Count;
}

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_ROW *Rows,
             IN UINTN              Count,
             IN UINTN              Cursor,
             IN CONST CHAR16       *Title)
{
  UINTN   Start;
  UINTN   Index;
  UINTN   Last;
  CHAR16  Text[SFB_NAME_CHARS + 4];

  SfbBeginScreen (Title, NULL);

  Start = SfbWindowStart (Cursor, Count, SFB_VISIBLE_ROWS);
  Last = Start + SFB_VISIBLE_ROWS;
  if (Last > Count) {
    Last = Count;
  }

  for (Index = Start; Index < Last; Index++) {
    /* Folder rows get a trailing '>' so it is obvious they open another list
     * rather than launch an image. */
    if (Rows[Index].Kind == SfbRowFolder) {
      UnicodeSPrint (Text, sizeof (Text), L"%s >", Rows[Index].Text);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Rows[Index].Text);
    }
  }

  if (Last < Count) {
    Print (L"    ... %u more\r\n", (UINT32)(Count - Last));
  }

  SfbEndScreen (L"Vol Up/Down: move   Power: select");
}

/* Refresh the shared list from the volumes and store the result. */
STATIC
VOID
SfbRefreshEntries (VOID)
{
  SFB_ENTRY_LIST  *List = SfbEntries ();

  if (List != NULL) {
    (VOID)SfbUpdateEntries (List, NULL);
    (VOID)SfbWriteStoredEntries (List);
  }
}

/*
 * Run one level of the menu, Prefix naming it ("" for the root). Returns TRUE
 * when the user picked "Enter Fastboot" in Advanced; FALSE when Back left a
 * nested level.
 */
STATIC
BOOLEAN
SfbRunMenuLevel (IN CONST CHAR16 *Prefix, IN UINTN Depth)
{
  SFB_ENTRY_LIST  *List = SfbEntries ();
  SFB_MENU_ROW    *Rows;
  UINTN           Count = 0;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  BOOLEAN         Fastboot = FALSE;
  SFB_KEY         Key;
  EFI_STATUS      Status;
  CHAR16          *ChildPrefix;

  Rows = AllocateZeroPool (SFB_MAX_MENU_ROWS * sizeof (*Rows));
  ChildPrefix = AllocateZeroPool (SFB_NAME_CHARS * 2 * sizeof (CHAR16));
  if (List == NULL || Rows == NULL || ChildPrefix == NULL) {
    goto Done;
  }

  while (TRUE) {
    if (Rebuild) {
      Count = SfbBuildRows (List, Prefix, Rows);
      if (Cursor >= Count) {
        Cursor = 0;
      }
      Rebuild = FALSE;
    }

    SfbDrawMenu (Rows, Count, Cursor, (Prefix[0] == L'\0') ? L"Boot Menu"
                                                          : Prefix);

    /* The menu is purely interactive: it waits for a key indefinitely and
     * never launches anything unattended. */
    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Count, Key);
      continue;
    }
    if (Count == 0) {
      continue;
    }

    switch (Rows[Cursor].Kind) {
    case SfbRowBack:
      /* Returning to the parent: debounce the confirming key first. */
      SfbDebounceMenuExit ();
      goto Done;

    case SfbRowFolder:
      if (Depth >= SFB_MAX_MENU_DEPTH) {
        SfbReportStatus (L"Menu too deep", EFI_BUFFER_TOO_SMALL);
        break;
      }
      if (Prefix[0] == L'\0') {
        StrCpyS (ChildPrefix, SFB_NAME_CHARS * 2, Rows[Cursor].Text);
      } else {
        UnicodeSPrint (ChildPrefix, SFB_NAME_CHARS * 2 * sizeof (CHAR16),
                       L"%s\\%s", Prefix, Rows[Cursor].Text);
      }
      if (SfbRunMenuLevel (ChildPrefix, Depth + 1)) {
        Fastboot = TRUE;
        goto Done;
      }
      Rebuild = TRUE;
      break;

    case SfbRowAdvanced:
      /* Advanced owns fastboot: a TRUE return means the user picked "Enter
       * Fastboot" there and the caller should hand over to it. */
      if (SfbRunAdvancedMenu ()) {
        Fastboot = TRUE;
        goto Done;
      }
      /* Priority, Management or a config export may have changed the list.
       * No rescan here: it would put back an entry just deleted. */
      Rebuild = TRUE;
      break;

    case SfbRowPowerOff:
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbRowRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbRowEntry:
    default:
      Status = SfbLaunchEntry (&List->Entry[Rows[Cursor].Entry], TRUE, NULL);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

Done:
  if (Rows != NULL) {
    FreePool (Rows);
  }
  if (ChildPrefix != NULL) {
    FreePool (ChildPrefix);
  }
  return Fastboot;
}

BOOLEAN
SfbRunBootMenu (VOID)
{
  /* The menu always starts from a fresh scan, stored before it is shown. */
  SfbRefreshEntries ();
  return SfbRunMenuLevel (L"", 0);
}
