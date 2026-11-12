/*
 * Challenge-response authorization state for the SFB security policy.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Rng.h>

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

/* Entropy for the nonce: the EFI RNG protocol when present, otherwise a mix of
 * the performance counter, the platform timer and a boot-session counter,
 * churned through SHA-256 so any single weak source still yields a spread. */
STATIC
VOID
FillNonce (
  OUT UINT8  *Nonce
  )
{
  STATIC UINT64  SessionSalt = 0;
  EFI_STATUS     Status;
  EFI_RNG_PROTOCOL  *Rng = NULL;
  mbedtls_md_context_t  Md;
  UINT64         Mix[3];

  SessionSalt++;

  Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
  if (!EFI_ERROR (Status) && Rng != NULL &&
      !EFI_ERROR (Rng->GetRNG (Rng, NULL, SFB_SECURITY_CHALLENGE_SIZE,
                               Nonce))) {
    return;
  }

  Mix[0] = SessionSalt;
  Mix[1] = GetPerformanceCounter ();
  Mix[2] = (UINT64)(UINTN)&SessionSalt ^ (UINT64)(UINTN)Nonce;
  if (gRT != NULL) {
    EFI_TIME  Now;
    ZeroMem (&Now, sizeof (Now));
    gRT->GetTime (&Now, NULL);
    Mix[2] ^= ((UINT64)Now.Year << 40) ^ ((UINT64)Now.Month << 32) ^
              ((UINT64)Now.Day << 24) ^ ((UINT64)Now.Hour << 16) ^
              ((UINT64)Now.Minute << 8) ^ (UINT64)Now.Second ^
              ((UINT64)Now.Nanosecond << 20);
  }

  mbedtls_md_init (&Md);
  if (mbedtls_md_setup (&Md, mbedtls_md_info_from_type (MBEDTLS_MD_SHA256),
                        0) == 0 &&
      mbedtls_md_starts (&Md) == 0 &&
      mbedtls_md_update (&Md, (CONST UINT8 *)Mix, sizeof (Mix)) == 0 &&
      mbedtls_md_finish (&Md, Nonce) == 0) {
    mbedtls_md_free (&Md);
    return;
  }
  mbedtls_md_free (&Md);

  /* Fallback when no hash could be computed: timer bits only. This is weaker
   * but still unique per attempt within a boot session. */
  CopyMem (Nonce, Mix, SFB_SECURITY_CHALLENGE_SIZE);
}

STATIC BOOLEAN  mPrivileged = FALSE;
STATIC UINT8    mChallenge[SFB_SECURITY_CHALLENGE_SIZE];
STATIC BOOLEAN  mChallengeValid = FALSE;

BOOLEAN
SfbSecurityLocked (
  VOID
  )
{
  return (BOOLEAN)(SfbSecurityAuthEnabled () && !mPrivileged);
}

BOOLEAN
SfbSecurityIsPrivileged (
  VOID
  )
{
  /* Privilege only ever comes from a successful challenge response - never
   * from the mere absence of class A certificates. */
  return mPrivileged;
}

VOID
SfbSecurityGrantPrivilege (
  VOID
  )
{
  mPrivileged = TRUE;
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
  DEBUG ((EFI_D_INFO, "SFB-SEC: privilege granted for this boot\n"));
}

VOID
SfbSecurityReset (
  VOID
  )
{
  mPrivileged = FALSE;
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
  SfbCertsReset ();
}

VOID
SfbSecurityTestSetChallenge (
  IN CONST UINT8  *Nonce
  )
{
  CopyMem (mChallenge, Nonce, sizeof (mChallenge));
  mChallengeValid = TRUE;
}

VOID
SfbSecurityNewChallenge (
  VOID
  )
{
  FillNonce (mChallenge);
  mChallengeValid = TRUE;
}

CONST UINT8 *
SfbSecurityChallenge (
  VOID
  )
{
  if (!mChallengeValid) {
    SfbSecurityNewChallenge ();
  }
  return mChallenge;
}

/* Verify Signature over SHA-256(Message) with the key of one certificate. */
STATIC
BOOLEAN
VerifyWithCert (
  IN mbedtls_x509_crt  *Crt,
  IN CONST UINT8             *Message,
  IN UINTN                   MessageSize,
  IN CONST UINT8             *Signature,
  IN UINTN                   SignatureSize
  )
{
  UINT8  Hash[32];
  INT32  MbedErr;

  if (mbedtls_md (mbedtls_md_info_from_type (MBEDTLS_MD_SHA256), Message,
                  MessageSize, Hash) != 0) {
    return FALSE;
  }

  MbedErr = mbedtls_pk_verify (&Crt->pk, MBEDTLS_MD_SHA256,
                               Hash, sizeof (Hash),
                               Signature, SignatureSize);
  return (BOOLEAN)(MbedErr == 0);
}

EFI_STATUS
SfbSecurityVerifyChallenge (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  )
{
  CONST SFB_CERT_POOL  *Pool = SfbCertsAuthPool ();
  CONST UINT8          *Nonce;
  UINTN                Index;
  mbedtls_x509_crt     Crt;
  BOOLEAN              Verified = FALSE;

  /* One attempt per nonce, whatever the outcome. */
  Nonce = SfbSecurityChallenge ();
  mChallengeValid = FALSE;

  if (Pool->Count == 0) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: challenge with no auth certs\n"));
    return EFI_SECURITY_VIOLATION;
  }

  for (Index = 0; Index < Pool->Count && !Verified; Index++) {
    mbedtls_x509_crt_init (&Crt);
    if (mbedtls_x509_crt_parse_der (&Crt, Pool->Der[Index],
                                    Pool->DerSize[Index]) == 0) {
      Verified = VerifyWithCert (&Crt, Nonce, SFB_SECURITY_CHALLENGE_SIZE,
                                 Signature, SignatureSize);
    }
    mbedtls_x509_crt_free (&Crt);
  }

  ZeroMem ((VOID *)Nonce, SFB_SECURITY_CHALLENGE_SIZE);

  if (Verified) {
    SfbSecurityGrantPrivilege ();
    return EFI_SUCCESS;
  }

  DEBUG ((EFI_D_ERROR, "SFB-SEC: challenge response rejected\n"));
  return EFI_SECURITY_VIOLATION;
}
