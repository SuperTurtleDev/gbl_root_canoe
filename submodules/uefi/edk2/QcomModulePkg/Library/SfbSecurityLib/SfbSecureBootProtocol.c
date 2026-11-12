/*
 * SFB_SECURE_BOOT_PROTOCOL publication: live queries against the security
 * state, read by launched EFI applications.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SfbSecureBoot.h>

STATIC BOOLEAN mInstalled = FALSE;

STATIC
BOOLEAN
EFIAPI
SfbSecureBootIsVerifiedBoot (
  IN CONST SFB_SECURE_BOOT_PROTOCOL *This
  )
{
  (VOID)This;
  return SfbSecurityVerifyEnabled ();
}

STATIC
BOOLEAN
EFIAPI
SfbSecureBootIsPrivMode (
  IN CONST SFB_SECURE_BOOT_PROTOCOL *This
  )
{
  (VOID)This;
  return SfbSecurityIsPrivileged ();
}

/* Embedded in the BDS image, so it lives as long as the boot session. */
STATIC SFB_SECURE_BOOT_PROTOCOL mSfbSecureBootProtocol = {
  SfbSecureBootIsVerifiedBoot,
  SfbSecureBootIsPrivMode
};

EFI_STATUS
SfbSecurityInstallProtocol (
  VOID
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  if (mInstalled) {
    return EFI_SUCCESS;
  }

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gSfbSecureBootProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mSfbSecureBootProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "SFB-SEC: failed to install secure boot protocol: %r\n", Status));
    return Status;
  }

  mInstalled = TRUE;
  DEBUG ((EFI_D_INFO, "SFB-SEC: secure boot protocol installed\n"));
  return EFI_SUCCESS;
}
