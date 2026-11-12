/*
 * SFB secure boot status protocol, published by the BDS (LinuxLoader) so any
 * EFI application can query the security state of the boot session.
 *
 * The BDS itself is the root of trust, with two independent certificate
 * classes at the root of the efisp config container:
 *
 *   BOOT_CERTS\*.cer  enables verification of every launched EFI image
 *   AUTH_CERTS\*.cer  locks privileged fastboot/Advanced operations until
 *                     a challenge-response authorization succeeds
 *
 * With no AUTH_CERTS the device enters privileged mode directly when the
 * boot menu runs. Privilege never disables image verification.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_SECURE_BOOT_PROTOCOL_H__
#define __SFB_SECURE_BOOT_PROTOCOL_H__

#include <Uefi.h>

#define SFB_SECURE_BOOT_PROTOCOL_GUID \
  { \
    0x2e7c4a19, 0xd5b0, 0x4c67, \
    { 0x9a, 0x31, 0x86, 0x0f, 0x4d, 0x2b, 0x71, 0xe3 } \
  }

typedef struct _SFB_SECURE_BOOT_PROTOCOL SFB_SECURE_BOOT_PROTOCOL;

/*
 * TRUE when BOOT_CERTS are present and every launched EFI image is being
 * verified (PE authentihash + embedded PKCS#7 signature). FALSE when the
 * certificate class is empty. Independent of IsPrivMode.
 */
typedef
BOOLEAN
(EFIAPI *SFB_SECURE_BOOT_IS_VERIFIED_BOOT) (
  IN CONST SFB_SECURE_BOOT_PROTOCOL *This
  );

/*
 * TRUE when the session is privileged: the full Advanced menu and the
 * mutating fastboot commands are available. Set directly when no AUTH_CERTS
 * were loaded (before the boot menu), or by a successful challenge response;
 * reset to FALSE on reboot.
 */
typedef
BOOLEAN
(EFIAPI *SFB_SECURE_BOOT_IS_PRIV_MODE) (
  IN CONST SFB_SECURE_BOOT_PROTOCOL *This
  );

struct _SFB_SECURE_BOOT_PROTOCOL {
  SFB_SECURE_BOOT_IS_VERIFIED_BOOT  IsVerifiedBoot;
  SFB_SECURE_BOOT_IS_PRIV_MODE      IsPrivMode;
};

extern EFI_GUID gSfbSecureBootProtocolGuid;

#endif /* __SFB_SECURE_BOOT_PROTOCOL_H__ */
