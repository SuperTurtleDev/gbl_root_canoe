/*
 * Boot entries for the super-fastboot boot menu: the stored list in the efisp
 * config container (CONFIG\\ENTRIES.CFG), the volume scan that keeps it up
 * to date, the power-on boot flow and launching.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbEntriesModuleTag = "SuperFbEntries";

STATIC SFB_ENTRY_LIST  *mSfbEntries = NULL;

SFB_ENTRY_LIST *
SfbEntries (VOID)
{
  if (mSfbEntries == NULL) {
    mSfbEntries = AllocateZeroPool (sizeof (*mSfbEntries));
  }
  return mSfbEntries;
}

/* ---- resolving ---------------------------------------------------------- */

/* The config container volume, which entries without (d,p) in the stored
 * list live on; NULL while the store is unavailable. */
STATIC
EFI_HANDLE
SfbStoreVolume (VOID)
{
  EFI_HANDLE  Volume = NULL;

  (VOID)SfbConfigStoreGetFs (NULL, &Volume);
  return Volume;
}

/*
 * Point Entry at the volume its spec names: (d,p) when it has one, otherwise
 * Home, the volume holding the list it came from.
 */
STATIC
VOID
SfbResolveEntry (IN OUT SFB_BOOT_ENTRY *Entry, IN EFI_HANDLE Home)
{
  if (Entry->DevicePath != NULL) {
    FreePool (Entry->DevicePath);
    Entry->DevicePath = NULL;
  }

  Entry->Volume = Entry->Spec.HasAddress
                    ? SfbFindVolumeByAddress (Entry->Spec.Disk, Entry->Spec.Part)
                    : Home;
  if (Entry->Volume != NULL) {
    Entry->DevicePath = FileDevicePath (Entry->Volume, Entry->Spec.Path);
    if (Entry->DevicePath == NULL) {
      Entry->Volume = NULL;
    }
  }
}

STATIC
BOOLEAN
SfbVolumeHasFile (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path)
{
  EFI_FILE_PROTOCOL  *Root = NULL;
  BOOLEAN            Present;

  if (Volume == NULL || EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) ||
      Root == NULL) {
    return FALSE;
  }
  Present = SfbFileExists (Root, Path);
  Root->Close (Root);
  return Present;
}

/*
 * TRUE when Path on Volume is the image running now. Where the firmware
 * starts this loader from a volume's removable-media path (the QEMU ESP), a
 * scan must not offer it back: booting it would only start this loader again.
 */
STATIC
BOOLEAN
SfbIsRunningImage (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path)
{
  EFI_LOADED_IMAGE_PROTOCOL  *Self = NULL;
  EFI_DEVICE_PATH_PROTOCOL   *Node;

  if (EFI_ERROR (gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid,
                                      (VOID **)&Self)) ||
      Self == NULL || Self->DeviceHandle != Volume || Self->FilePath == NULL) {
    return FALSE;
  }
  Node = Self->FilePath;
  return (BOOLEAN)(DevicePathType (Node) == MEDIA_DEVICE_PATH &&
                   DevicePathSubType (Node) == MEDIA_FILEPATH_DP &&
                   SfbEntryPathEqual (((FILEPATH_DEVICE_PATH *)Node)->PathName,
                                      Path));
}

/* An entry is valid when it resolves to a file that is there right now. */
STATIC
BOOLEAN
SfbEntryPresent (IN CONST SFB_BOOT_ENTRY *Entry)
{
  return (BOOLEAN)(Entry->DevicePath != NULL &&
                   SfbVolumeHasFile (Entry->Volume, Entry->Spec.Path));
}

EFI_STATUS
SfbMakeFileEntry (IN EFI_HANDLE      Volume,
                  IN CONST CHAR16    *PathOnVolume,
                  IN CONST CHAR16    *Name,
                  OUT SFB_BOOT_ENTRY *Entry)
{
  ZeroMem (Entry, sizeof (*Entry));

  StrnCpyS (Entry->Spec.Name, SFB_NAME_CHARS, Name, SFB_NAME_CHARS - 1);
  StrnCpyS (Entry->Spec.Path, SFB_PATH_CHARS, PathOnVolume, SFB_PATH_CHARS - 1);

  /* Every volume in the table has a (d,p) address, including the config
   * container; anything outside the volume table cannot be stored. */
  if (EFI_ERROR (SfbGetVolumeAddress (Volume, &Entry->Spec.Disk,
                                      &Entry->Spec.Part))) {
    return EFI_UNSUPPORTED;
  }
  Entry->Spec.HasAddress = TRUE;

  SfbResolveEntry (Entry, Volume);
  return (Entry->DevicePath != NULL) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

/* ---- ENTRIES.CFG files -------------------------------------------------- */

/* Read a whole ENTRIES.CFG into a NUL-terminated pool buffer. */
STATIC
EFI_STATUS
SfbReadEntriesText (IN EFI_FILE_PROTOCOL *Root,
                    IN CONST CHAR16      *Path,
                    OUT CHAR8            **Text)
{
  EFI_STATUS  Status;
  UINTN       Size = 0;

  *Text = AllocateZeroPool (SFB_ENTRIES_MAX_BYTES + 1);
  if (*Text == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SfbReadFileBytes (Root, Path, *Text, SFB_ENTRIES_MAX_BYTES, &Size);
  if (EFI_ERROR (Status)) {
    FreePool (*Text);
    *Text = NULL;
    return Status;
  }
  (*Text)[Size] = '\0';
  return EFI_SUCCESS;
}

/*
 * Append every entry line of Text to List in order. Lines without (d,p) live
 * on Home; when Home itself has an address (a partition's root ENTRIES.CFG)
 * that address is recorded, so the entry still points at the right volume
 * once it is stored elsewhere. With RequirePresent (a scan), lines whose file
 * is not there, or is this very loader, are skipped.
 */
STATIC
VOID
SfbAppendEntryLines (IN OUT SFB_ENTRY_LIST *List,
                     IN CONST CHAR8        *Text,
                     IN EFI_HANDLE         Home,
                     IN BOOLEAN            HomeHasAddress,
                     IN UINT32             HomeDisk,
                     IN UINT32             HomePart,
                     IN BOOLEAN            RequirePresent)
{
  CONST CHAR8     *Cursor = Text;
  CHAR8           Line[SFB_ENTRY_LINE_BYTES];
  SFB_BOOT_ENTRY  Entry;

  while (SfbEntryNextLine (&Cursor, Line, sizeof (Line))) {
    ZeroMem (&Entry, sizeof (Entry));
    if (!SfbEntryParseLine (Line, &Entry.Spec)) {
      continue;
    }
    if (!Entry.Spec.HasAddress && HomeHasAddress) {
      Entry.Spec.HasAddress = TRUE;
      Entry.Spec.Disk = HomeDisk;
      Entry.Spec.Part = HomePart;
    }

    SfbResolveEntry (&Entry, Home);
    if (RequirePresent &&
        (!SfbEntryPresent (&Entry) ||
         SfbIsRunningImage (Entry.Volume, Entry.Spec.Path))) {
      DEBUG ((EFI_D_INFO, "SFB: entry '%s' -> '%s' not present\n",
              Entry.Spec.Name, Entry.Spec.Path));
      if (Entry.DevicePath != NULL) {
        FreePool (Entry.DevicePath);
      }
      continue;
    }
    if (!SfbEntryListAppend (List, &Entry)) {
      DEBUG ((EFI_D_INFO, "SFB: entry '%s' duplicate or list full\n",
              Entry.Spec.Name));
    }
  }
}

EFI_STATUS
SfbLoadStoredEntries (IN OUT SFB_ENTRY_LIST *List)
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_HANDLE                       Home = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;
  CHAR8                            *Text = NULL;

  SfbEntryListFree (List);

  Status = SfbConfigStoreGetFs (&Fs, &Home);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status) || Root == NULL) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  /* A rewrite interrupted between delete and rename leaves only the fully
   * written and flushed temporary file behind; it is the current list. */
  Status = SfbReadEntriesText (Root, SFB_STORE_ENTRIES_PATH, &Text);
  if (EFI_ERROR (Status)) {
    Status = SfbReadEntriesText (Root, SFB_STORE_TEMP_PATH, &Text);
  }
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  {
    UINT32  HomeDisk;
    UINT32  HomePart;
    BOOLEAN HomeHasAddress =
      (BOOLEAN)(!EFI_ERROR (SfbGetVolumeAddress (Home, &HomeDisk, &HomePart)));

    /* Old CONFIG\ENTRIES.CFG lines without (d,p) mean "this volume"; the
     * config container now has an address, so record it like any other. */
    SfbAppendEntryLines (List, Text, Home, HomeHasAddress,
                         HomeHasAddress ? HomeDisk : 0,
                         HomeHasAddress ? HomePart : 0, FALSE);
  }
  FreePool (Text);

  DEBUG ((EFI_D_INFO, "SFB: %u stored entries\n", (UINT32)List->Count));
  return EFI_SUCCESS;
}

/* What one volume offers: its removable-media loader and its ENTRIES.CFG. */
STATIC
VOID
SfbScanVolume (IN OUT SFB_ENTRY_LIST *Found, IN EFI_HANDLE Volume)
{
  EFI_FILE_PROTOCOL  *Root = NULL;
  UINT32             Disk;
  UINT32             Part;
  CHAR16             Label[SFB_DESC_CHARS];
  CHAR8              *Text = NULL;
  SFB_BOOT_ENTRY     Entry;

  /* Every volume in the UFS partition table has an address; only handles
   * outside it (never returned by SfbLocateVolumes) are skipped. */
  if (EFI_ERROR (SfbGetVolumeAddress (Volume, &Disk, &Part)) ||
      EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) || Root == NULL) {
    return;
  }

  if (SfbFileExists (Root, SFB_BOOT_FILE_PATH) &&
      !SfbIsRunningImage (Volume, SFB_BOOT_FILE_PATH)) {
    ZeroMem (&Entry, sizeof (Entry));
    SfbGetVolumeLabel (Root, Label, ARRAY_SIZE (Label));
    UnicodeSPrint (Entry.Spec.Name, sizeof (Entry.Spec.Name), L"%s (%u,%u)",
                   (Label[0] != L'\0') ? Label : L"Volume", Disk, Part);
    StrCpyS (Entry.Spec.Path, SFB_PATH_CHARS, SFB_BOOT_FILE_PATH);
    Entry.Spec.HasAddress = TRUE;
    Entry.Spec.Disk = Disk;
    Entry.Spec.Part = Part;
    SfbResolveEntry (&Entry, Volume);
    (VOID)SfbEntryListAppend (Found, &Entry);
  }

  if (!EFI_ERROR (SfbReadEntriesText (Root, SFB_VOLUME_ENTRIES_PATH, &Text))) {
    SfbAppendEntryLines (Found, Text, Volume, TRUE, Disk, Part, TRUE);
    FreePool (Text);
  }

  Root->Close (Root);
}

EFI_STATUS
SfbUpdateEntries (IN OUT SFB_ENTRY_LIST *List, OUT BOOLEAN *Changed OPTIONAL)
{
  EFI_STATUS      Status;
  SFB_ENTRY_LIST  *Found;
  BOOLEAN         Valid[SFB_MAX_ENTRIES];
  EFI_HANDLE      *Volumes = NULL;
  UINTN           VolumeCount = 0;
  UINTN           Index;
  EFI_HANDLE      Home;
  BOOLEAN         Moved;

  if (Changed != NULL) {
    *Changed = FALSE;
  }

  Found = AllocateZeroPool (sizeof (*Found));
  if (Found == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* No volumes at all is a valid outcome: every stored entry is then gone. */
  Status = SfbLocateVolumes (&Volumes, &VolumeCount);
  if (!EFI_ERROR (Status) && Volumes != NULL) {
    for (Index = 0; Index < VolumeCount; Index++) {
      SfbScanVolume (Found, Volumes[Index]);
    }
    FreePool (Volumes);
  }

  /* Handles and (d,p) numbering may have moved since the list was loaded. */
  Home = SfbStoreVolume ();
  for (Index = 0; Index < List->Count; Index++) {
    SfbResolveEntry (&List->Entry[Index], Home);
    Valid[Index] = SfbEntryPresent (&List->Entry[Index]);
  }

  Moved = SfbEntryListSync (List, Valid, Found);
  FreePool (Found);

  DEBUG ((EFI_D_INFO, "SFB: %u entries after scan (changed=%u)\n",
          (UINT32)List->Count, (UINT32)Moved));
  if (Changed != NULL) {
    *Changed = Moved;
  }
  return EFI_SUCCESS;
}

/* ---- atomic rewrite ----------------------------------------------------- */

/* Delete Path if it exists. */
STATIC
EFI_STATUS
SfbDeleteFile (IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;

  Status = Root->Open (Root, &File, (CHAR16 *)Path,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  /* Delete closes the handle whatever it returns. */
  return File->Delete (File);
}

STATIC
EFI_STATUS
SfbWriteNewFile (IN EFI_FILE_PROTOCOL *Root,
                 IN CONST CHAR16      *Path,
                 IN VOID              *Data,
                 IN UINTN             Size)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  UINTN              Written = Size;

  Status = Root->Open (Root, &File, (CHAR16 *)Path,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->Write (File, &Written, Data);
  if (!EFI_ERROR (Status) && Written != Size) {
    Status = EFI_VOLUME_FULL;
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }
  File->Close (File);
  return Status;
}

/* Rename From to the absolute path To on the same volume. */
STATIC
EFI_STATUS
SfbRenameFile (IN EFI_FILE_PROTOCOL *Root,
               IN CONST CHAR16      *From,
               IN CONST CHAR16      *To)
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  EFI_FILE_INFO      *Info = NULL;
  EFI_FILE_INFO      *Renamed;
  UINTN              InfoSize = 0;
  UINTN              NewSize;

  Status = Root->Open (Root, &File, (CHAR16 *)From,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocateZeroPool (InfoSize);
    Status = (Info == NULL) ? EFI_OUT_OF_RESOURCES
                            : File->GetInfo (File, &gEfiFileInfoGuid,
                                             &InfoSize, Info);
  }
  if (!EFI_ERROR (Status)) {
    NewSize = SIZE_OF_EFI_FILE_INFO + StrSize (To);
    Renamed = AllocateZeroPool (NewSize);
    if (Renamed == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      CopyMem (Renamed, Info, SIZE_OF_EFI_FILE_INFO);
      Renamed->Size = NewSize;
      StrCpyS (Renamed->FileName, StrLen (To) + 1, To);
      Status = File->SetInfo (File, &gEfiFileInfoGuid, NewSize, Renamed);
      FreePool (Renamed);
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  if (Info != NULL) {
    FreePool (Info);
  }
  File->Close (File);
  return Status;
}

EFI_STATUS
SfbWriteStoredEntries (IN CONST SFB_ENTRY_LIST *List)
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_HANDLE                       Volume = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;
  EFI_FILE_PROTOCOL                *Dir = NULL;
  EFI_BLOCK_IO_PROTOCOL            *BlockIo = NULL;
  CHAR8                            *Text;
  UINTN                            Size = 0;
  UINTN                            Index;

  Status = SfbConfigStoreGetFs (&Fs, &Volume);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: entries not stored: %r\n", Status));
    return Status;
  }

  Text = AllocateZeroPool (SFB_ENTRIES_MAX_BYTES + 1);
  if (Text == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < List->Count; Index++) {
    if (!SfbEntryFormatLine (&List->Entry[Index].Spec, Text + Size,
                             SFB_ENTRIES_MAX_BYTES + 1 - Size)) {
      FreePool (Text);
      return EFI_BUFFER_TOO_SMALL;
    }
    Size += AsciiStrLen (Text + Size);
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status) || Root == NULL) {
    FreePool (Text);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  Status = Root->Open (Root, &Dir, SFB_STORE_DIR_PATH,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
  if (!EFI_ERROR (Status)) {
    Dir->Close (Dir);
    /* Write the new list in full beside the old one, then swap it in: at
     * every point either the old or the new list is complete on disk. */
    Status = SfbDeleteFile (Root, SFB_STORE_TEMP_PATH);
  }
  if (!EFI_ERROR (Status)) {
    Status = SfbWriteNewFile (Root, SFB_STORE_TEMP_PATH, Text, Size);
  }
  if (!EFI_ERROR (Status)) {
    Status = SfbDeleteFile (Root, SFB_STORE_ENTRIES_PATH);
  }
  if (!EFI_ERROR (Status)) {
    Status = SfbRenameFile (Root, SFB_STORE_TEMP_PATH, SFB_STORE_ENTRIES_PATH);
  }
  if (!EFI_ERROR (Status)) {
    Status = Root->Flush (Root);
  }
  Root->Close (Root);
  FreePool (Text);

  /* The FAT driver flushes into Disk I/O; push it through to the media. */
  if (!EFI_ERROR (gBS->HandleProtocol (Volume, &gEfiBlockIoProtocolGuid,
                                       (VOID **)&BlockIo)) &&
      BlockIo != NULL) {
    (VOID)BlockIo->FlushBlocks (BlockIo);
  }

  DEBUG ((EFI_D_INFO, "SFB: stored %u entries: %r\n", (UINT32)List->Count,
          Status));
  return Status;
}

/*
 * Turn a DRIVER.LIST line into an absolute Unicode path on the volume:
 * leading whitespace and separators are stripped, '/' is normalised to '\',
 * a single leading '\' is added, and trailing spaces
 * and separators are trimmed. Returns FALSE for blank lines, comments ('#') and
 * anything that reduces to nothing.
 */
STATIC
BOOLEAN
SfbAsciiRelPathToUnicode (IN CONST CHAR8 *Rel, OUT CHAR16 *Out, IN UINTN OutChars)
{
  UINTN  Count;

  while (*Rel == ' ' || *Rel == '\t') {
    Rel++;
  }
  if (*Rel == '\0' || *Rel == '#') {
    return FALSE;
  }

  /* Whether or not the line starts with a separator, exactly one is added. */
  while (*Rel == '/' || *Rel == '\\') {
    Rel++;
  }

  if (OutChars < 2) {
    return FALSE;
  }

  Count = 0;
  Out[Count++] = L'\\';

  for (; *Rel != '\0' && Count + 1 < OutChars; Rel++) {
    CHAR8  Ch = *Rel;

    if (Ch == '/') {
      Ch = '\\';
    }
    if ((UINT8)Ch < 0x20 || (UINT8)Ch > 0x7e) {
      continue;
    }
    Out[Count++] = (CHAR16)Ch;
  }

  while (Count > 1 && (Out[Count - 1] == L' ' || Out[Count - 1] == L'\\')) {
    Count--;
  }
  Out[Count] = L'\0';

  return (BOOLEAN)(Count > 1);
}

/* ---- launching ---------------------------------------------------------- */

/*
 * The SECURITY / SECURITY2 hooks delegate to the SFB security policy: an
 * image is admitted when it verifies under a SECURITY\BOOT_CERTS key, and
 * without class B certificates (or with privilege granted this boot) every
 * image is admitted.
 */
STATIC EFI_SECURITY_ARCH_PROTOCOL          *mSfbSec      = NULL;
STATIC EFI_SECURITY2_ARCH_PROTOCOL         *mSfbSec2     = NULL;

STATIC
EFI_STATUS
EFIAPI
SfbAllowState (IN CONST EFI_SECURITY_ARCH_PROTOCOL *This,
               IN UINT32                           AuthenticationStatus,
               IN CONST EFI_DEVICE_PATH_PROTOCOL   *File)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SfbAllowAuth (IN CONST EFI_SECURITY2_ARCH_PROTOCOL *This,
              IN CONST EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
              IN VOID                              *FileBuffer,
              IN UINTN                             FileSize,
              IN BOOLEAN                           BootPolicy)
{
  EFI_STATUS  Status;

  if (FileBuffer == NULL || FileSize == 0) {
    /* Nothing to verify; let the firmware's own policy decide. */
    return EFI_SUCCESS;
  }

  Status = SfbSecurityVerifyImage (FileBuffer, FileSize);
  if (Status == EFI_UNSUPPORTED) {
    /* Unsigned image while verification is on. */
    return EFI_ACCESS_DENIED;
  }
  return Status;
}

STATIC
VOID
SfbLoadSecurityPolicy (VOID)
{
  mSfbSec = NULL;
  mSfbSec2 = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiSecurityArchProtocolGuid, NULL,
                                       (VOID **)&mSfbSec)) && mSfbSec != NULL) {
    mSfbSec->FileAuthenticationState = SfbAllowState;
  }

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiSecurity2ArchProtocolGuid, NULL,
                                       (VOID **)&mSfbSec2)) && mSfbSec2 != NULL) {
    mSfbSec2->FileAuthentication = SfbAllowAuth;
  }
}

EFI_STATUS
SfbLoadDriver (IN EFI_HANDLE Volume, IN CONST CHAR16 *Path)
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_HANDLE                ImageHandle = NULL;

  if (Volume == NULL || Path == NULL || Path[0] == L'\0') {
    return EFI_INVALID_PARAMETER;
  }

  DevicePath = FileDevicePath (Volume, Path);
  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* Same verified-boot bypass the entry launch relies on; see below. */
  SfbLoadSecurityPolicy ();
  Status = gBS->LoadImage (FALSE, gImageHandle, DevicePath, NULL, 0,
                           &ImageHandle);
  FreePool (DevicePath);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: driver LoadImage '%s' failed: %r\n",
            Path, Status));
    return Status;
  }

  /* A UEFI driver installs its driver binding here and returns; the caller runs
   * the connect pass. A driver that returns an error is unloaded by the core. */
  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  DEBUG ((EFI_D_INFO, "SFB: driver '%s' start: %r\n", Path, Status));

  return Status;
}

/* Copy Path's parent directory into Out. A path with no directory component
 * (a file at the volume root) yields "\". */
STATIC
VOID
SfbDirOf (IN CONST CHAR16 *Path, OUT CHAR16 *Out, IN UINTN OutChars)
{
  UINTN  Index;
  UINTN  LastSep = 0;

  StrnCpyS (Out, OutChars, Path, OutChars - 1);

  for (Index = 0; Out[Index] != L'\0'; Index++) {
    if (Out[Index] == L'\\') {
      LastSep = Index;
    }
  }

  if (LastSep == 0) {
    Out[0] = L'\\';
    Out[1] = L'\0';
  } else {
    Out[LastSep] = L'\0';
  }
}

/* Append Child to a directory path, inserting a separator unless the directory
 * is the root. */
STATIC
VOID
SfbJoinChild (IN OUT CHAR16 *Path, IN UINTN OutChars, IN CONST CHAR16 *Child)
{
  if (!(Path[0] == L'\\' && Path[1] == L'\0')) {
    StrnCatS (Path, OutChars, L"\\", OutChars - StrLen (Path) - 1);
  }
  StrnCatS (Path, OutChars, Child, OutChars - StrLen (Path) - 1);
}

/*
 * Load the drivers named in the DRIVER.LIST file sitting in EntryPath's own
 * directory, if that file exists, then connect controllers so the drivers bind.
 * Each line is a driver path on the same volume. Missing list or
 * missing drivers are not fatal: the entry still launches.
 */
STATIC
VOID
SfbPreloadDrivers (IN EFI_HANDLE Volume, IN CONST CHAR16 *EntryPath)
{
  EFI_FILE_PROTOCOL  *Root = NULL;
  CHAR16             ListPath[SFB_PATH_CHARS];
  CHAR8              *Buffer;
  UINTN              Size = 0;
  CONST CHAR8        *Cursor;
  CHAR8              Line[SFB_PATH_CHARS];
  BOOLEAN            LoadedAny = FALSE;

  if (Volume == NULL || EntryPath == NULL) {
    return;
  }

  SfbDirOf (EntryPath, ListPath, SFB_PATH_CHARS);
  SfbJoinChild (ListPath, SFB_PATH_CHARS, SFB_DRIVER_LIST_NAME);

  if (EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) || Root == NULL) {
    return;
  }

  Buffer = AllocateZeroPool (SFB_LIST_MAX_BYTES + 1);
  if (Buffer == NULL) {
    Root->Close (Root);
    return;
  }

  if (EFI_ERROR (SfbReadFileBytes (Root, ListPath, Buffer, SFB_LIST_MAX_BYTES,
                                   &Size))) {
    /* No DRIVER.LIST beside the entry: nothing to preload. */
    FreePool (Buffer);
    Root->Close (Root);
    return;
  }
  Buffer[Size] = '\0';
  Root->Close (Root);

  DEBUG ((EFI_D_INFO, "SFB: DRIVER.LIST '%s' for entry '%s'\n",
          ListPath, EntryPath));

  Cursor = Buffer;
  while (SfbEntryNextLine (&Cursor, Line, sizeof (Line))) {
    CHAR16  RelPath[SFB_PATH_CHARS];

    if (!SfbAsciiRelPathToUnicode (Line, RelPath, SFB_PATH_CHARS)) {
      continue;
    }
    if (!EFI_ERROR (SfbLoadDriver (Volume, RelPath))) {
      LoadedAny = TRUE;
    }
  }

  FreePool (Buffer);

  if (LoadedAny) {
    SfbConnectAll ();
  }
}

EFI_STATUS
SfbLaunchEntry (IN CONST SFB_BOOT_ENTRY *Entry,
                IN BOOLEAN              ClearScreen,
                OUT BOOLEAN             *NotPeImage OPTIONAL)
{
  EFI_STATUS  Status;
  EFI_HANDLE  ImageHandle = NULL;
  CHAR16      *ExitData = NULL;
  UINTN       ExitDataSize = 0;

  if (NotPeImage != NULL) {
    *NotPeImage = FALSE;
  }
  if (Entry->DevicePath == NULL || Entry->Volume == NULL) {
    return EFI_NOT_FOUND;
  }

  /*
   * Announce the launch: "Booting <name>". Clearing the screen first is only
   * done for a menu-driven launch; an unattended boot leaves the screen (e.g.
   * the boot splash) untouched.
   */
  SfbShowBootingScreen (Entry->Spec.Name, ClearScreen);

  /*
   * Preload any drivers the entry asks for via a DRIVER.LIST in its directory,
   * so a loader that needs, say, a file-system or graphics driver present finds
   * it already bound before it starts.
   */
  SfbPreloadDrivers (Entry->Volume, Entry->Spec.Path);

  SfbLoadSecurityPolicy ();
  Status = gBS->LoadImage (FALSE, gImageHandle, Entry->DevicePath,
                           NULL, 0, &ImageHandle);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB: LoadImage '%s' failed: %r\n",
            Entry->Spec.Path, Status));
    /* The core answers EFI_UNSUPPORTED for data that is not a PE/COFF image
     * and for an image built for another machine type. */
    if (NotPeImage != NULL && Status == EFI_UNSUPPORTED) {
      *NotPeImage = TRUE;
    }
    return Status;
  }

  /*
   * Carry the entry's argument line to the image the way the EDK2 shell
   * carries a command line: LoadOptions holds the arguments, NUL terminated,
   * without the image name. An image that wants them reads LoadOptions off
   * its own EFI_LOADED_IMAGE_PROTOCOL.
   */
  if (Entry->Spec.Args[0] != L'\0') {
    EFI_LOADED_IMAGE_PROTOCOL  *Loaded = NULL;

    if (!EFI_ERROR (gBS->OpenProtocol (ImageHandle,
                                       &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&Loaded,
                                       gImageHandle, NULL,
                                       EFI_OPEN_PROTOCOL_GET_PROTOCOL))) {
      Loaded->LoadOptions = (VOID *)(UINTN)Entry->Spec.Args;
      Loaded->LoadOptionsSize =
        (UINT32)((StrLen (Entry->Spec.Args) + 1) * sizeof (CHAR16));
    }
  }

  Status = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
  DEBUG ((EFI_D_INFO, "SFB: '%s' returned: %r\n", Entry->Spec.Path, Status));

  /* An application that returns has already been unloaded by the core; only
   * the exit data is ours to release. */
  if (ExitData != NULL) {
    FreePool (ExitData);
  }

  return Status;
}

/* ---- power-on flow ------------------------------------------------------ */

/* Unattended launch: never clears the screen (boot splash). */
STATIC
EFI_STATUS
SfbStartupLaunch (IN  CONST SFB_BOOT_ENTRY *Entry,
                  OUT BOOLEAN              *NotPeImage,
                  IN  VOID                 *Context)
{
  EFI_STATUS  Status;

  (VOID)Context;
  Status = SfbLaunchEntry (Entry, FALSE, NotPeImage);
  if (*NotPeImage) {
    DEBUG ((EFI_D_ERROR, "SFB: dropping '%s': not a PE/COFF image\n",
            Entry->Spec.Name));
  }
  return Status;
}

STATIC
EFI_STATUS
SfbStartupStore (IN CONST SFB_ENTRY_LIST *List, IN VOID *Context)
{
  (VOID)Context;
  return SfbWriteStoredEntries (List);
}

VOID
SfbRunStartup (IN BOOLEAN MenuRequested)
{
  SFB_ENTRY_LIST  *List = SfbEntries ();

  if (List == NULL) {
    return;
  }

  /* 1. The stored list; a missing file or store simply yields nothing. */
  (VOID)SfbLoadStoredEntries (List);

  /* 2. Nothing stored: build the list from the volumes and store it. */
  if (List->Count == 0) {
    (VOID)SfbUpdateEntries (List, NULL);
    (VOID)SfbWriteStoredEntries (List);
  }

  /* 3. Still nothing to boot: the menu. */
  if (List->Count == 0) {
    return;
  }

  /* 5. Menu requested at power-on: refresh and store, then the menu. */
  if (MenuRequested) {
    (VOID)SfbUpdateEntries (List, NULL);
    (VOID)SfbWriteStoredEntries (List);
    return;
  }

  /* 4. Try every entry in order. A launched image that boots never comes
   * back; one that fails or returns hands over to the next entry. Only an
   * entry whose file is not a loadable PE/COFF image is dropped - missing
   * media, a security refusal or low memory may all be gone next boot. */
  SfbEntryListBootInOrder (List, SfbStartupLaunch, SfbStartupStore, NULL);

  /* Every entry failed: fall back to the menu. */
}
