/*
 * Boot entry records and the ENTRIES.CFG line format. See SuperFbEntryList.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbEntryList.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#define SFB_ADDRESS_MAX_DIGITS  9

STATIC
BOOLEAN
SfbIsBlank (
  IN CHAR8  Ch
  )
{
  return (BOOLEAN)(Ch == ' ' || Ch == '\t');
}

STATIC
BOOLEAN
SfbIsPrintable (
  IN UINT32  Ch
  )
{
  return (BOOLEAN)(Ch >= 0x20 && Ch <= 0x7e);
}

STATIC
CHAR16
SfbFold (
  IN CHAR16  Ch
  )
{
  return (Ch >= L'a' && Ch <= L'z') ? (CHAR16)(Ch - L'a' + L'A') : Ch;
}

BOOLEAN
SfbEntryNextLine (
  IN OUT CONST CHAR8  **Cursor,
  OUT    CHAR8        *Line,
  IN     UINTN        LineBytes
  )
{
  CONST CHAR8  *Ptr = *Cursor;
  UINTN        Count = 0;

  if (*Ptr == '\0') {
    return FALSE;
  }

  while (*Ptr != '\0' && *Ptr != '\n') {
    if (*Ptr != '\r' && Count + 1 < LineBytes) {
      Line[Count++] = *Ptr;
    }
    Ptr++;
  }
  if (*Ptr == '\n') {
    Ptr++;
  }

  Line[Count] = '\0';
  *Cursor = Ptr;
  return TRUE;
}

/*
 * Copy the name [Start, End) into Out as '\'-separated components, each
 * trimmed of surrounding blanks, empty components dropped.
 */
STATIC
UINTN
SfbCopyName (
  IN  CONST CHAR8  *Start,
  IN  CONST CHAR8  *End,
  OUT CHAR16       *Out,
  IN  UINTN        OutChars
  )
{
  UINTN  Count = 0;

  while (Start < End) {
    CONST CHAR8  *CompEnd;

    while (Start < End && (SfbIsBlank (*Start) || *Start == '\\')) {
      Start++;
    }
    CompEnd = Start;
    while (CompEnd < End && *CompEnd != '\\') {
      CompEnd++;
    }
    /* Trim the component's trailing blanks. */
    {
      CONST CHAR8  *Last = CompEnd;

      while (Last > Start && SfbIsBlank (Last[-1])) {
        Last--;
      }
      if (Last > Start) {
        if (Count > 0 && Count + 1 < OutChars) {
          Out[Count++] = L'\\';
        }
        for (; Start < Last && Count + 1 < OutChars; Start++) {
          if (SfbIsPrintable ((UINT8)*Start)) {
            Out[Count++] = (CHAR16)*Start;
          }
        }
      }
    }
    Start = CompEnd;
  }

  Out[Count] = L'\0';
  return Count;
}

/* Parse "(d,p)" at *Text. FALSE leaves *Text untouched. */
STATIC
BOOLEAN
SfbParseAddress (
  IN OUT CONST CHAR8  **Text,
  OUT    UINT32       *Disk,
  OUT    UINT32       *Part
  )
{
  CONST CHAR8  *Ptr = *Text;
  UINT32       Value[2];
  UINTN        Which;

  if (*Ptr != '(') {
    return FALSE;
  }
  Ptr++;

  for (Which = 0; Which < 2; Which++) {
    UINTN  Digits = 0;

    while (SfbIsBlank (*Ptr)) {
      Ptr++;
    }
    Value[Which] = 0;
    while (*Ptr >= '0' && *Ptr <= '9' && Digits < SFB_ADDRESS_MAX_DIGITS) {
      Value[Which] = Value[Which] * 10 + (UINT32)(*Ptr - '0');
      Ptr++;
      Digits++;
    }
    while (SfbIsBlank (*Ptr)) {
      Ptr++;
    }
    if (Digits == 0 || *Ptr != ((Which == 0) ? ',' : ')')) {
      return FALSE;
    }
    Ptr++;
  }

  *Disk = Value[0];
  *Part = Value[1];
  *Text = Ptr;
  return TRUE;
}

BOOLEAN
SfbEntryParseLine (
  IN  CONST CHAR8     *Line,
  OUT SFB_ENTRY_SPEC  *Spec
  )
{
  CONST CHAR8  *Colon;
  CONST CHAR8  *Ptr;
  UINTN        Count;

  ZeroMem (Spec, sizeof (*Spec));

  while (SfbIsBlank (*Line)) {
    Line++;
  }
  if (*Line == '\0' || *Line == '#') {
    return FALSE;
  }

  for (Colon = Line; *Colon != '\0' && *Colon != ':'; Colon++) {
  }
  if (*Colon != ':' ||
      SfbCopyName (Line, Colon, Spec->Name, SFB_NAME_CHARS) == 0) {
    return FALSE;
  }

  Ptr = Colon + 1;
  while (SfbIsBlank (*Ptr)) {
    Ptr++;
  }
  if (*Ptr == '(') {
    if (!SfbParseAddress (&Ptr, &Spec->Disk, &Spec->Part)) {
      return FALSE;
    }
    Spec->HasAddress = TRUE;
  }

  /* Path: up to the first blank, always absolute, '\' separated, no
   * trailing separator. */
  Count = 0;
  Spec->Path[Count++] = L'\\';
  while (*Ptr == '\\' || *Ptr == '/') {
    Ptr++;
  }
  for (; *Ptr != '\0' && !SfbIsBlank (*Ptr); Ptr++) {
    CHAR8  Ch = (*Ptr == '/') ? '\\' : *Ptr;

    if (!SfbIsPrintable ((UINT8)Ch)) {
      continue;
    }
    if (Ch == '\\' && Spec->Path[Count - 1] == L'\\') {
      continue;   /* collapse "\\" */
    }
    if (Count + 1 >= SFB_PATH_CHARS) {
      return FALSE;
    }
    Spec->Path[Count++] = (CHAR16)Ch;
  }
  while (Count > 1 && Spec->Path[Count - 1] == L'\\') {
    Count--;
  }
  Spec->Path[Count] = L'\0';
  if (Count <= 1) {
    return FALSE;
  }

  /* The rest of the line, trimmed, is the argument line. */
  while (SfbIsBlank (*Ptr)) {
    Ptr++;
  }
  Count = 0;
  for (; *Ptr != '\0' && Count + 1 < SFB_ARGS_CHARS; Ptr++) {
    if (SfbIsPrintable ((UINT8)*Ptr) || *Ptr == '\t') {
      Spec->Args[Count++] = (*Ptr == '\t') ? L' ' : (CHAR16)*Ptr;
    }
  }
  while (Count > 0 && Spec->Args[Count - 1] == L' ') {
    Count--;
  }
  Spec->Args[Count] = L'\0';

  return TRUE;
}

/* Append printable ASCII from Text, skipping Skip (0 = none). */
STATIC
BOOLEAN
SfbPutText (
  IN OUT CHAR8         *Out,
  IN OUT UINTN         *At,
  IN     UINTN         OutBytes,
  IN     CONST CHAR16  *Text,
  IN     CHAR8         Skip
  )
{
  for (; *Text != L'\0'; Text++) {
    if (!SfbIsPrintable (*Text) || (Skip != 0 && *Text == (CHAR16)Skip)) {
      continue;
    }
    if (*At + 1 >= OutBytes) {
      return FALSE;
    }
    Out[(*At)++] = (CHAR8)*Text;
  }
  Out[*At] = '\0';
  return TRUE;
}

STATIC
BOOLEAN
SfbPutNumber (
  IN OUT CHAR8   *Out,
  IN OUT UINTN   *At,
  IN     UINTN   OutBytes,
  IN     UINT32  Value
  )
{
  CHAR16  Digits[12];
  UINTN   Count = 0;
  CHAR16  Text[12];
  UINTN   Index;

  do {
    Digits[Count++] = (CHAR16)(L'0' + Value % 10);
    Value /= 10;
  } while (Value != 0);

  for (Index = 0; Index < Count; Index++) {
    Text[Index] = Digits[Count - 1 - Index];
  }
  Text[Count] = L'\0';
  return SfbPutText (Out, At, OutBytes, Text, 0);
}

BOOLEAN
SfbEntryFormatLine (
  IN  CONST SFB_ENTRY_SPEC  *Spec,
  OUT CHAR8                 *Out,
  IN  UINTN                 OutBytes
  )
{
  UINTN  At = 0;

  if (OutBytes == 0) {
    return FALSE;
  }
  Out[0] = '\0';

  /* A ':' in the name would end it early on the way back in. */
  if (!SfbPutText (Out, &At, OutBytes, Spec->Name, ':') ||
      !SfbPutText (Out, &At, OutBytes, L":", 0)) {
    return FALSE;
  }
  if (Spec->HasAddress &&
      (!SfbPutText (Out, &At, OutBytes, L"(", 0) ||
       !SfbPutNumber (Out, &At, OutBytes, Spec->Disk) ||
       !SfbPutText (Out, &At, OutBytes, L",", 0) ||
       !SfbPutNumber (Out, &At, OutBytes, Spec->Part) ||
       !SfbPutText (Out, &At, OutBytes, L")", 0))) {
    return FALSE;
  }
  /* A blank in the path would split it into path and arguments. */
  if (!SfbPutText (Out, &At, OutBytes, Spec->Path, ' ')) {
    return FALSE;
  }
  if (Spec->Args[0] != L'\0' &&
      (!SfbPutText (Out, &At, OutBytes, L" ", 0) ||
       !SfbPutText (Out, &At, OutBytes, Spec->Args, 0))) {
    return FALSE;
  }
  /* The line terminator is not printable text, so it is added directly. */
  if (At + 1 >= OutBytes) {
    Out[0] = '\0';
    return FALSE;
  }
  Out[At++] = '\n';
  Out[At] = '\0';
  return TRUE;
}

BOOLEAN
SfbEntryPathEqual (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  for (; *A != L'\0' || *B != L'\0'; A++, B++) {
    if (SfbFold (*A) != SfbFold (*B)) {
      return FALSE;
    }
  }
  return TRUE;
}

BOOLEAN
SfbEntrySameTarget (
  IN CONST SFB_ENTRY_SPEC  *A,
  IN CONST SFB_ENTRY_SPEC  *B
  )
{
  if (A->HasAddress != B->HasAddress) {
    return FALSE;
  }
  if (A->HasAddress && (A->Disk != B->Disk || A->Part != B->Part)) {
    return FALSE;
  }
  return (BOOLEAN)(SfbEntryPathEqual (A->Path, B->Path) &&
                   StrCmp (A->Args, B->Args) == 0);
}

UINTN
SfbEntryListFind (
  IN CONST SFB_ENTRY_LIST  *List,
  IN CONST SFB_ENTRY_SPEC  *Spec
  )
{
  UINTN  Index;

  for (Index = 0; Index < List->Count; Index++) {
    if (SfbEntrySameTarget (&List->Entry[Index].Spec, Spec)) {
      return Index;
    }
  }
  return SFB_NO_INDEX;
}

STATIC
VOID
SfbEntryRelease (
  IN OUT SFB_BOOT_ENTRY  *Entry
  )
{
  if (Entry->DevicePath != NULL) {
    FreePool (Entry->DevicePath);
    Entry->DevicePath = NULL;
  }
  Entry->Volume = NULL;
}

BOOLEAN
SfbEntryListAppend (
  IN OUT SFB_ENTRY_LIST  *List,
  IN OUT SFB_BOOT_ENTRY  *Entry
  )
{
  if (List->Count >= SFB_MAX_ENTRIES ||
      SfbEntryListFind (List, &Entry->Spec) != SFB_NO_INDEX) {
    SfbEntryRelease (Entry);
    return FALSE;
  }
  CopyMem (&List->Entry[List->Count], Entry, sizeof (*Entry));
  List->Count++;
  /* Ownership moved into the list. */
  Entry->DevicePath = NULL;
  return TRUE;
}

VOID
SfbEntryListRemove (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     UINTN           Index
  )
{
  if (Index >= List->Count) {
    return;
  }
  SfbEntryRelease (&List->Entry[Index]);
  CopyMem (&List->Entry[Index], &List->Entry[Index + 1],
           (List->Count - Index - 1) * sizeof (List->Entry[0]));
  List->Count--;
  ZeroMem (&List->Entry[List->Count], sizeof (List->Entry[0]));
}

VOID
SfbEntryListMove (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     UINTN           From,
  IN     UINTN           To
  )
{
  SFB_BOOT_ENTRY  Moving;

  if (From >= List->Count || To >= List->Count || From == To) {
    return;
  }
  CopyMem (&Moving, &List->Entry[From], sizeof (Moving));
  if (From < To) {
    CopyMem (&List->Entry[From], &List->Entry[From + 1],
             (To - From) * sizeof (Moving));
  } else {
    CopyMem (&List->Entry[To + 1], &List->Entry[To],
             (From - To) * sizeof (Moving));
  }
  CopyMem (&List->Entry[To], &Moving, sizeof (Moving));
}

BOOLEAN
SfbEntryListSync (
  IN OUT SFB_ENTRY_LIST  *List,
  IN     CONST BOOLEAN   *Valid,
  IN OUT SFB_ENTRY_LIST  *Discovered
  )
{
  BOOLEAN  Changed = FALSE;
  UINTN    Kept = 0;
  UINTN    Index;

  for (Index = 0; Index < List->Count; Index++) {
    if (Valid[Index]) {
      if (Kept != Index) {
        CopyMem (&List->Entry[Kept], &List->Entry[Index], sizeof (List->Entry[0]));
      }
      Kept++;
    } else {
      SfbEntryRelease (&List->Entry[Index]);
      Changed = TRUE;
    }
  }
  for (Index = Kept; Index < List->Count; Index++) {
    ZeroMem (&List->Entry[Index], sizeof (List->Entry[0]));
  }
  List->Count = Kept;

  for (Index = 0; Index < Discovered->Count; Index++) {
    if (SfbEntryListAppend (List, &Discovered->Entry[Index])) {
      Changed = TRUE;
    }
  }
  ZeroMem (Discovered, sizeof (*Discovered));

  return Changed;
}

VOID
SfbEntryListBootInOrder (
  IN OUT SFB_ENTRY_LIST    *List,
  IN     SFB_ENTRY_LAUNCH  Launch,
  IN     SFB_ENTRY_STORE   Store,
  IN     VOID              *Context
  )
{
  UINTN    Index = 0;
  BOOLEAN  NotPeImage;

  while (Index < List->Count) {
    NotPeImage = FALSE;
    (VOID)Launch (&List->Entry[Index], &NotPeImage, Context);
    if (NotPeImage) {
      SfbEntryListRemove (List, Index);
      (VOID)Store (List, Context);
      continue;
    }
    Index++;
  }
}

VOID
SfbEntryListFree (
  IN OUT SFB_ENTRY_LIST  *List
  )
{
  UINTN  Index;

  for (Index = 0; Index < List->Count; Index++) {
    SfbEntryRelease (&List->Entry[Index]);
  }
  ZeroMem (List, sizeof (*List));
}

BOOLEAN
SfbEntryNameChild (
  IN  CONST CHAR16  *Name,
  IN  CONST CHAR16  *Prefix,
  OUT CHAR16        *Child,
  IN  UINTN         ChildChars,
  OUT BOOLEAN       *IsFolder
  )
{
  UINTN  PrefixChars = StrLen (Prefix);
  UINTN  Count = 0;

  if (PrefixChars != 0) {
    if (StrnCmp (Name, Prefix, PrefixChars) != 0 || Name[PrefixChars] != L'\\') {
      return FALSE;
    }
    Name += PrefixChars + 1;
  }
  if (*Name == L'\0') {
    return FALSE;
  }

  while (*Name != L'\0' && *Name != L'\\') {
    if (Count + 1 < ChildChars) {
      Child[Count++] = *Name;
    }
    Name++;
  }
  Child[Count] = L'\0';
  *IsFolder = (BOOLEAN)(*Name == L'\\');
  return TRUE;
}
