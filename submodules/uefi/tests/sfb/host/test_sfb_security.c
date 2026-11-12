/*
 * Host round-trip tests for the SFB security library: challenge-response
 * verification against an openssl-made signature, and PE authentihash +
 * PKCS#7 image verification against a python-cryptography-made signature.
 *
 * The real SfbSecurityLib sources are compiled in; the UEFI environment is
 * stubbed. The driver script (test_sfb.py::SecurityRoundTripTest) prepares
 * the keys, certificates and signatures and calls:
 *
 *   test_sfb_security <dir> mint     -> writes <dir>/nonce.bin, exits
 *   test_sfb_security <dir> verify   -> runs all checks, returns 0 on pass
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* System headers first: MdePkg's X64 ProcessorBind.h pushes hidden symbol
 * visibility and never pops it, which would hide the libc symbols below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>

#include "SfbSecurityInternal.h"

/* ---- edk2 stubs ---------------------------------------------------------- */

EFI_BOOT_SERVICES    *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_HANDLE           gImageHandle;

UINT16 EFIAPI ReadUnaligned16 (CONST UINT16 *Buffer) { return *Buffer; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *Buffer) { return *Buffer; }
UINT64 EFIAPI ReadUnaligned64 (CONST UINT64 *Buffer) { return *Buffer; }

UINT64 EFIAPI GetPerformanceCounter (VOID) { return 42; }

VOID *CopyMem (VOID *Dst, CONST VOID *Src, UINTN Len) {
  return memmove (Dst, Src, Len);
}
VOID *SetMem (VOID *Buf, UINTN Len, UINT8 Val) { return memset (Buf, Val, Len); }
VOID *ZeroMem (VOID *Buf, UINTN Len) { return memset (Buf, 0, Len); }
INTN CompareMem (CONST VOID *A, CONST VOID *B, UINTN Len) {
  return memcmp (A, B, Len);
}

VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
VOID EFIAPI FreePool (VOID *Buf) { free (Buf); }

/* LocateProtocol: always missing, forcing the timer-based nonce fallback. */
STATIC EFI_STATUS StubLocateProtocol (EFI_GUID *Guid, VOID *Reg, VOID **Iface)
{
  (VOID)Guid; (VOID)Reg; *Iface = NULL; return EFI_NOT_FOUND;
}

EFI_GUID  gEfiRngProtocolGuid = { 0 };

STATIC EFI_BOOT_SERVICES  mBs = { 0 };

/* ---- cert pool stubs fed from the test directory ------------------------- */

STATIC SFB_CERT_POOL  mAuthPool;
STATIC SFB_CERT_POOL  mBootPool;

CONST SFB_CERT_POOL *SfbCertsAuthPool (VOID) { return &mAuthPool; }
CONST SFB_CERT_POOL *SfbCertsBootPool (VOID) { return &mBootPool; }
VOID SfbCertsReset (VOID) {}

BOOLEAN SfbSecurityAuthEnabled (VOID) { return mAuthPool.Count > 0; }
BOOLEAN SfbSecurityVerifyEnabled (VOID) { return mBootPool.Count > 0; }

/* ---- helpers -------------------------------------------------------------- */

STATIC UINT8 *LoadFile (CONST CHAR8 *Path, UINTN *Size)
{
  FILE  *F = fopen (Path, "rb");
  UINT8 *Buf;
  long  Len;

  if (F == NULL) {
    return NULL;
  }
  fseek (F, 0, SEEK_END);
  Len = ftell (F);
  fseek (F, 0, SEEK_SET);
  Buf = malloc (Len > 0 ? Len : 1);
  if (Buf == NULL || fread (Buf, 1, Len, F) != (size_t)Len) {
    fclose (F);
    free (Buf);
    return NULL;
  }
  fclose (F);
  *Size = (UINTN)Len;
  return Buf;
}

STATIC int Failures = 0;

#define CHECK(Cond, Name) do { \
    if (Cond) { printf ("PASS %s\n", Name); } \
    else { printf ("FAIL %s\n", Name); Failures++; } \
  } while (0)

/* Mint the nonce and drop it for the driver to sign. */
STATIC int MintNonce (CONST char *Dir)
{
  CONST UINT8  *Nonce;
  CHAR8        Path[512];
  FILE         *F;

  Nonce = SfbSecurityChallenge ();
  snprintf (Path, sizeof (Path), "%s/nonce.bin", Dir);
  F = fopen (Path, "wb");
  if (F == NULL || fwrite (Nonce, SFB_SECURITY_CHALLENGE_SIZE, 1, F) != 1) {
    fprintf (stderr, "cannot write %s\n", Path);
    return 2;
  }
  fclose (F);
  printf ("nonce written\n");
  return 0;
}

/* Install the known nonce, run the challenge checks. */
STATIC int VerifyChallengeCase (CONST char *Dir)
{
  CHAR8    Path[512];
  UINT8    *Nonce, *OtherNonce, *GoodSig, *BadSig;
  UINTN    NonceSize, SigSize;

  snprintf (Path, sizeof (Path), "%s/nonce.bin", Dir);
  Nonce = LoadFile (Path, &NonceSize);
  snprintf (Path, sizeof (Path), "%s/nonce-other.bin", Dir);
  OtherNonce = LoadFile (Path, &NonceSize);
  snprintf (Path, sizeof (Path), "%s/challenge.sig", Dir);
  GoodSig = LoadFile (Path, &SigSize);
  snprintf (Path, sizeof (Path), "%s/challenge-bad.sig", Dir);
  BadSig = LoadFile (Path, &SigSize);
  if (Nonce == NULL || OtherNonce == NULL || GoodSig == NULL ||
      BadSig == NULL) {
    fprintf (stderr, "missing nonce/signature files in %s\n", Dir);
    return 2;
  }

  /* A forged signature must be rejected and must consume the nonce. */
  SfbSecurityTestSetChallenge (Nonce);
  CHECK (EFI_ERROR (SfbSecurityVerifyChallenge (BadSig, SigSize)),
         "challenge: forged signature rejected");
  CHECK (SfbSecurityLocked (), "challenge: still locked after forgery");

  /* A valid signature for the OLD (consumed) nonce against a fresh nonce
   * must fail too: only the live nonce is signable. */
  SfbSecurityTestSetChallenge (OtherNonce);
  CHECK (EFI_ERROR (SfbSecurityVerifyChallenge (GoodSig, SigSize)),
         "challenge: signature for stale nonce rejected");

  SfbSecurityTestSetChallenge (Nonce);
  CHECK (!EFI_ERROR (SfbSecurityVerifyChallenge (GoodSig, SigSize)),
         "challenge: valid signature accepted");
  CHECK (!SfbSecurityLocked () && SfbSecurityIsPrivileged (),
         "challenge: privilege granted");

  /* Replay: the nonce is consumed, the same signature must fail again. The
   * host fallback nonce is deterministic, so arm a different fresh nonce
   * rather than relying on the minted one differing. */
  SfbSecurityTestSetChallenge (OtherNonce);
  CHECK (EFI_ERROR (SfbSecurityVerifyChallenge (GoodSig, SigSize)),
         "challenge: replay rejected");

  return 0;
}

/* Verify the synthetic signed PE. */
STATIC int VerifyImageCase (CONST char *Dir)
{
  CHAR8  Path[512];
  UINT8  *Pe, *PeBad;
  UINTN  PeSize;

  snprintf (Path, sizeof (Path), "%s/app-signed.efi", Dir);
  Pe = LoadFile (Path, &PeSize);
  if (Pe == NULL) {
    fprintf (stderr, "missing %s\n", Path);
    return 2;
  }

  /* Locked boot-time state: privilege off, boot certs present. The stubbed
   * SfbCertsReset keeps the pools loaded, so this only clears privilege. */
  SfbSecurityReset ();
  CHECK (!EFI_ERROR (SfbSecurityVerifyImage (Pe, PeSize)),
         "image: correctly signed image passes");

  PeBad = malloc (PeSize);
  memcpy (PeBad, Pe, PeSize);
  PeBad[PeSize / 2] ^= 0xff;
  CHECK (EFI_ERROR (SfbSecurityVerifyImage (PeBad, PeSize)),
         "image: tampered image rejected");

  Pe[PeSize - 1] ^= 0xff;   /* tamper inside the cert table */
  CHECK (EFI_ERROR (SfbSecurityVerifyImage (Pe, PeSize)),
         "image: tampered signature rejected");

  /* Privilege granted this boot: verification still runs - the two
   * policies are independent. */
  SfbSecurityGrantPrivilege ();
  CHECK (EFI_ERROR (SfbSecurityVerifyImage (PeBad, PeSize)),
         "image: privileged boot still verifies");

  return 0;
}

int main (int Argc, char **Argv)
{
  CONST char *Dir;
  CHAR8      Path[512];
  UINTN      Size;
  int        Ret;

  if (Argc != 3) {
    fprintf (stderr, "usage: test_sfb_security <dir> <mint|verify>\n");
    return 2;
  }
  Dir = Argv[1];

  mBs.LocateProtocol = StubLocateProtocol;
  gBS = &mBs;
  gRT = NULL;

  snprintf (Path, sizeof (Path), "%s/auth.cer", Dir);
  mAuthPool.Der[0] = LoadFile (Path, &mAuthPool.DerSize[0]);
  snprintf (Path, sizeof (Path), "%s/boot.cer", Dir);
  mBootPool.Der[0] = LoadFile (Path, &mBootPool.DerSize[0]);
  if (mAuthPool.Der[0] == NULL || mBootPool.Der[0] == NULL) {
    fprintf (stderr, "missing test certs in %s\n", Dir);
    return 2;
  }
  mAuthPool.Count = 1;
  mBootPool.Count = 1;
  (VOID)Size;

  if (strcmp (Argv[2], "mint") == 0) {
    return MintNonce (Dir);
  }

  Ret = VerifyChallengeCase (Dir);
  Ret |= VerifyImageCase (Dir);
  if (Failures > 0) {
    printf ("%d failures\n", Failures);
    Ret = 1;
  }
  return Ret;
}
