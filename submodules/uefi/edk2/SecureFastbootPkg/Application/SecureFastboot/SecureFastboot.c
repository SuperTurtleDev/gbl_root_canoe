/** @file
  Secure fastboot preload wrapper for the SFB BDS.

  Boot entry:

    Secure Fastboot Wrapper:\efisp\secure_fastboot.efi \efisp\boot.efi

  The BDS passes everything after the image path as LoadOptions. The first
  token names the image to chainload; the rest is forwarded to it unchanged.

  When the boot session is NOT privileged (SFB_SECURE_BOOT_PROTOCOL's
  IsPrivMode() is FALSE, i.e. AUTH_CERTS are present and no challenge has
  succeeded), the wrapper hooks EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL on the
  console input handle and swallows every key stroke. The chainloaded stock
  loader's GetKeyPress() therefore never sees a key, so its fastboot and
  recovery shortcuts cannot trigger. Privileged sessions get no hook at all.

  The hook is removed when the child image returns, so the BDS menu keeps
  working if the chainload fails.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/SfbSecureBoot.h>

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *mConInEx;
STATIC EFI_INPUT_READ_KEY_EX               mOriginalReadKeyEx;
STATIC BOOLEAN                             mHooked;

/**
  Replacement ReadKeyStrokeEx: drain whatever is queued and report an idle
  console. Every key - volume up, volume down, power - is swallowed while
  the chainloaded image runs.
**/
STATIC
EFI_STATUS
EFIAPI
SfbBlockedReadKeyEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  EFI_KEY_DATA  Drained;
  EFI_STATUS    Status;

  do {
    Status = mOriginalReadKeyEx (This, &Drained);
  } while (!EFI_ERROR (Status));

  if (KeyData != NULL) {
    ZeroMem (KeyData, sizeof (*KeyData));
  }
  return EFI_NOT_READY;
}

/** Patch the console's Ex input protocol in place. */
STATIC
VOID
SfbInstallKeyBlock (VOID)
{
  EFI_STATUS  Status;

  if (mHooked) {
    return;
  }

  mConInEx = NULL;
  Status = gBS->HandleProtocol (gST->ConsoleInHandle,
                                &gEfiSimpleTextInputExProtocolGuid,
                                (VOID **)&mConInEx);
  if (EFI_ERROR (Status) || mConInEx == NULL ||
      mConInEx->ReadKeyStrokeEx == NULL) {
    DEBUG ((DEBUG_ERROR, "SFB-WRAP: no ConInEx protocol, keys stay visible\n"));
    return;
  }

  mOriginalReadKeyEx = mConInEx->ReadKeyStrokeEx;
  mConInEx->ReadKeyStrokeEx = SfbBlockedReadKeyEx;
  mHooked = TRUE;
  DEBUG ((DEBUG_INFO, "SFB-WRAP: console input blocked\n"));
}

/** Put the original ReadKeyStrokeEx back. */
STATIC
VOID
SfbRemoveKeyBlock (VOID)
{
  if (!mHooked || mConInEx == NULL || mOriginalReadKeyEx == NULL) {
    return;
  }
  mConInEx->ReadKeyStrokeEx = mOriginalReadKeyEx;
  mHooked = FALSE;
  DEBUG ((DEBUG_INFO, "SFB-WRAP: console input restored\n"));
}

/** TRUE when the BDS says this boot session is privileged. */
STATIC
BOOLEAN
SfbSessionPrivileged (VOID)
{
  SFB_SECURE_BOOT_PROTOCOL  *Sb = NULL;

  if (EFI_ERROR (gBS->LocateProtocol (&gSfbSecureBootProtocolGuid, NULL,
                                      (VOID **)&Sb)) ||
      Sb == NULL || Sb->IsPrivMode == NULL) {
    /* No protocol means no way to prove privilege: stay restrictive. */
    return FALSE;
  }
  return Sb->IsPrivMode (Sb);
}

/** NUL-terminated copy of the image's LoadOptions, or NULL. */
STATIC
CHAR16 *
SfbLoadOptionsText (
  IN EFI_LOADED_IMAGE_PROTOCOL  *Self
  )
{
  UINTN   Chars;
  CHAR16  *Text;

  if (Self->LoadOptions == NULL || Self->LoadOptionsSize < sizeof (CHAR16)) {
    return NULL;
  }
  Chars = Self->LoadOptionsSize / sizeof (CHAR16);
  Text = AllocateZeroPool ((Chars + 1) * sizeof (CHAR16));
  if (Text == NULL) {
    return NULL;
  }
  CopyMem (Text, Self->LoadOptions, Chars * sizeof (CHAR16));
  Text[Chars] = L'\0';
  return Text;
}

/**
  Split the next blank-separated token off Cursor. The token is terminated
  in place; *Rest receives the first non-blank character after it.
**/
STATIC
CHAR16 *
SfbNextToken (
  IN OUT CHAR16  **Cursor,
  OUT    CHAR16  **Rest
  )
{
  CHAR16  *At = *Cursor;
  CHAR16  *Token;

  while (*At == L' ' || *At == L'\t') {
    At++;
  }
  Token = At;
  while (*At != L'\0' && *At != L' ' && *At != L'\t') {
    At++;
  }
  if (*At != L'\0') {
    *At = L'\0';
    At++;
  }
  while (*At == L' ' || *At == L'\t') {
    At++;
  }
  *Rest = At;
  *Cursor = At;
  return Token;
}

/** Absolute path on the wrapper's own volume, '/' normalised to '\'. */
STATIC
CHAR16 *
SfbAbsolutePath (
  IN CONST CHAR16  *Token
  )
{
  UINTN   Len = StrLen (Token);
  UINTN   At = 0;
  UINTN   Index;
  CHAR16  *Path;

  Path = AllocateZeroPool ((Len + 2) * sizeof (CHAR16));
  if (Path == NULL) {
    return NULL;
  }
  if (Len == 0 || (Token[0] != L'\\' && Token[0] != L'/')) {
    Path[At++] = L'\\';
  }
  for (Index = 0; Index < Len; Index++) {
    Path[At++] = (Token[Index] == L'/') ? L'\\' : Token[Index];
  }
  Path[At] = L'\0';
  return Path;
}

EFI_STATUS
EFIAPI
SecureFastbootEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Self = NULL;
  EFI_LOADED_IMAGE_PROTOCOL  *ChildImage = NULL;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_HANDLE                 Child = NULL;
  CHAR16                     *Options;
  CHAR16                     *Cursor;
  CHAR16                     *Token;
  CHAR16                     *Rest;
  CHAR16                     *Path;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                (VOID **)&Self);
  if (EFI_ERROR (Status) || Self == NULL) {
    return EFI_NOT_FOUND;
  }

  Options = SfbLoadOptionsText (Self);
  if (Options == NULL) {
    DEBUG ((DEBUG_ERROR, "SFB-WRAP: no chainload target in LoadOptions\n"));
    return EFI_INVALID_PARAMETER;
  }

  Cursor = Options;
  Token = SfbNextToken (&Cursor, &Rest);
  if (Token[0] == L'\0') {
    DEBUG ((DEBUG_ERROR, "SFB-WRAP: empty chainload target\n"));
    FreePool (Options);
    return EFI_INVALID_PARAMETER;
  }

  Path = SfbAbsolutePath (Token);
  if (Path == NULL) {
    FreePool (Options);
    return EFI_OUT_OF_RESOURCES;
  }
  DevicePath = FileDevicePath (Self->DeviceHandle, Path);
  FreePool (Path);
  if (DevicePath == NULL) {
    FreePool (Options);
    return EFI_OUT_OF_RESOURCES;
  }

  if (!SfbSessionPrivileged ()) {
    SfbInstallKeyBlock ();
  } else {
    DEBUG ((DEBUG_INFO, "SFB-WRAP: privileged session, keys pass through\n"));
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, DevicePath, NULL, 0, &Child);
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SFB-WRAP: LoadImage '%s' failed: %r\n",
            Token, Status));
    SfbRemoveKeyBlock ();
    FreePool (Options);
    return Status;
  }

  /* Forward the remaining text as the child's LoadOptions, the way the BDS
   * forwards an entry's arguments. */
  if (Rest[0] != L'\0') {
    Status = gBS->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid,
                                  (VOID **)&ChildImage);
    if (!EFI_ERROR (Status) && ChildImage != NULL) {
      ChildImage->LoadOptions = Rest;
      ChildImage->LoadOptionsSize =
        (UINT32)((StrLen (Rest) + 1) * sizeof (CHAR16));
    }
  }

  Status = gBS->StartImage (Child, NULL, NULL);
  DEBUG ((DEBUG_INFO, "SFB-WRAP: '%s' returned: %r\n", Token, Status));

  SfbRemoveKeyBlock ();
  FreePool (Options);
  return Status;
}
