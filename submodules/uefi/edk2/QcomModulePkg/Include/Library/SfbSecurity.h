/*
 * Security state for the super-fastboot BDS: certificate classes stored in the
 * efisp config container, the fastboot challenge-response authorization and
 * the PE image signature verification policy.
 *
 * Certificates are X.509 DER files in the config container, classified by
 * directory at the container root:
 *
 *   AUTH_CERTS\*.cer   class A - fastboot authorization; presence of at
 *                      least one puts the device in locked mode
 *   BOOT_CERTS\*.cer   class B - EFI code signing; presence of at least one
 *                      enables image verification
 *
 * Locked mode gates the mutating fastboot commands and the Advanced menu to
 * the authorization interface only. A successful challenge response grants
 * temporary privilege for the rest of the boot. Privilege and EFI signature
 * verification are independent: BOOT_CERTS verification always runs, with
 * or without privilege. With no AUTH_CERTS the menu is entered privileged
 * directly - there is nothing to authorize against.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_SECURITY_H__
#define __SFB_SECURITY_H__

#include <Uefi.h>

/* Directory (in the config container root) holding each certificate class. */
#define SFB_SECURITY_AUTH_DIR   L"AUTH_CERTS"
#define SFB_SECURITY_BOOT_DIR   L"BOOT_CERTS"

/* Certificate file extension accepted by the loader. */
#define SFB_SECURITY_CERT_EXT   L".cer"

/* Maximum certificates loaded per class; extras are ignored with a debug log. */
#define SFB_SECURITY_MAX_CERTS  8

/* Size in bytes of the fastboot challenge nonce. */
#define SFB_SECURITY_CHALLENGE_SIZE  32

/*
 * Load both certificate classes from the mounted config container. Call once
 * after SfbConfigStoreMount, before fastboot can run or any entry launches.
 * Unreadable or unparsable certificates are skipped; a missing directory is
 * an empty class. Always succeeds - an unavailable store simply means no
 * certificates and therefore no enforcement.
 */
VOID
SfbSecurityLoadCerts (
  VOID
  );

/* TRUE when at least one class A certificate loaded: locked mode. */
BOOLEAN
SfbSecurityAuthEnabled (
  VOID
  );

/* TRUE when at least one class B certificate loaded: image verification. */
BOOLEAN
SfbSecurityVerifyEnabled (
  VOID
  );

/* TRUE when locked and authorization has not been granted this boot. */
BOOLEAN
SfbSecurityLocked (
  VOID
  );

/* TRUE when authorization was granted (or none was needed) this boot. */
BOOLEAN
SfbSecurityIsPrivileged (
  VOID
  );

/* Grant temporary privilege for the rest of the boot. */
VOID
SfbSecurityGrantPrivilege (
  VOID
  );

/* Forget loaded certificates and privilege (test seam). */
VOID
SfbSecurityReset (
  VOID
  );

/*
 * The current challenge nonce, generating a fresh one if none is pending.
 * The nonce stays valid until the next SfbSecurityVerifyChallenge or
 * SfbSecurityNewChallenge call: one attempt per nonce, success or failure.
 */
CONST UINT8 *
SfbSecurityChallenge (
  VOID
  );

/* Drop any pending nonce; the next SfbSecurityChallenge mints a new one. */
VOID
SfbSecurityNewChallenge (
  VOID
  );

/*
 * Verify a raw signature over the pending nonce (PKCS#1 v1.5 or ECDSA over
 * SHA-256 of the nonce, as produced by "openssl dgst -sha256 -sign") against
 * every class A certificate. Consumes the nonce regardless of the outcome.
 * Returns EFI_SUCCESS and grants privilege on the first match.
 */
EFI_STATUS
SfbSecurityVerifyChallenge (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  );

/*
 * Verify a PE image against the class B certificates: the embedded PKCS#7
 * (authentihash) signature must verify under at least one BOOT_CERTS key.
 * EFI_SUCCESS when verified, EFI_SECURITY_VIOLATION when not, EFI_UNSUPPORTED
 * when the image carries no signature. Verification is skipped (SUCCESS)
 * only when no class B certificate loaded - never because of privilege.
 */
EFI_STATUS
SfbSecurityVerifyImage (
  IN CONST VOID  *PeImage,
  IN UINTN       PeSize
  );

#endif /* __SFB_SECURITY_H__ */
