/*
 * Certificate loading for the SFB security policy: X.509 DER files under
 * SECURITY\AUTH_CERTS and SECURITY\BOOT_CERTS in the config container.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

STATIC SFB_CERT_POOL  mAuthCerts;
STATIC SFB_CERT_POOL  mBootCerts;

/* TRUE when Name ends with the .cer extension (case-insensitive). */
STATIC
BOOLEAN
CertFileName (IN CONST CHAR16 *Name)
{
  UINTN  Len = StrLen (Name);
  UINTN  ExtLen = StrLen (SFB_SECURITY_CERT_EXT);
  UINTN  Index;

  if (Len <= ExtLen) {
    return FALSE;
  }
  for (Index = 0; Index < ExtLen; Index++) {
    CHAR16  Ch = Name[Len - ExtLen + Index];

    if (Ch >= L'A' && Ch <= L'Z') {
      Ch = (CHAR16)(Ch - L'A' + L'a');
    }
    if (Ch != SFB_SECURITY_CERT_EXT[Index]) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Append one certificate to Pool when it parses as X.509 DER. Takes ownership
 * of Der only on success. */
STATIC
EFI_STATUS
CertPoolAppend (
  IN OUT SFB_CERT_POOL  *Pool,
  IN UINT8              *Der,
  IN UINTN              DerSize
  )
{
  mbedtls_x509_crt  Crt;
  INT32             MbedErr;

  if (Pool->Count >= SFB_SECURITY_MAX_CERTS) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: cert pool full, ignoring extra cert\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  mbedtls_x509_crt_init (&Crt);
  MbedErr = mbedtls_x509_crt_parse_der (&Crt, Der, DerSize);
  mbedtls_x509_crt_free (&Crt);
  if (MbedErr != 0) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: skipping unparsable cert (-0x%x)\n",
            -MbedErr));
    return EFI_INVALID_PARAMETER;
  }

  Pool->Der[Pool->Count] = Der;
  Pool->DerSize[Pool->Count] = DerSize;
  Pool->Count++;
  return EFI_SUCCESS;
}

/* Enumerate DirPath (relative to the container root) and load every .cer file
 * into Pool. A missing directory is an empty pool, not an error. */
STATIC
VOID
LoadCertDir (
  IN OUT SFB_CERT_POOL   *Pool,
  IN CONST CHAR16        *DirPath,
  IN EFI_FILE_PROTOCOL   *Root
  )
{
  EFI_STATUS          Status;
  EFI_FILE_PROTOCOL   *Dir = NULL;
  UINT8               *InfoBuf = NULL;
  EFI_FILE_INFO       *Info;
  UINTN               InfoSize;
  EFI_FILE_PROTOCOL   *File = NULL;
  UINT8               *Der = NULL;
  UINTN               DerSize;
  UINTN               ReadSize;

  Status = Root->Open (Root, &Dir, (CHAR16 *)DirPath, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return;
  }

  InfoBuf = AllocatePool (SIZE_OF_EFI_FILE_INFO + 256);
  if (InfoBuf == NULL) {
    goto out;
  }

  Dir->SetPosition (Dir, 0);
  while (TRUE) {
    InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
    Status = Dir->Read (Dir, &InfoSize, InfoBuf);
    if (EFI_ERROR (Status) || InfoSize == 0) {
      break;
    }
    Info = (EFI_FILE_INFO *)InfoBuf;
    if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
        !CertFileName (Info->FileName)) {
      continue;
    }

    if (EFI_ERROR (Dir->Open (Dir, &File, Info->FileName,
                              EFI_FILE_MODE_READ, 0))) {
      continue;
    }

    DerSize = (UINTN)Info->FileSize;
    Der = AllocatePool (DerSize);
    if (Der != NULL) {
      ReadSize = DerSize;
      if (!EFI_ERROR (File->Read (File, &ReadSize, Der)) &&
          ReadSize == DerSize) {
        if (EFI_ERROR (CertPoolAppend (Pool, Der, DerSize))) {
          FreePool (Der);
        }
        Der = NULL;
      } else {
        FreePool (Der);
      }
    }
    File->Close (File);
    File = NULL;
  }

out:
  if (File != NULL) {
    File->Close (File);
  }
  if (InfoBuf != NULL) {
    FreePool (InfoBuf);
  }
  Dir->Close (Dir);
}

VOID
SfbCertsReset (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mAuthCerts.Count; Index++) {
    FreePool (mAuthCerts.Der[Index]);
  }
  for (Index = 0; Index < mBootCerts.Count; Index++) {
    FreePool (mBootCerts.Der[Index]);
  }
  ZeroMem (&mAuthCerts, sizeof (mAuthCerts));
  ZeroMem (&mBootCerts, sizeof (mBootCerts));
}

VOID
SfbSecurityLoadCerts (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;
  EFI_FILE_PROTOCOL            *Root = NULL;

  SfbCertsReset ();

  Status = SfbConfigStoreGetFs (&Fs, NULL);
  if (EFI_ERROR (Status) || Fs == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: no config store, security off\n"));
    return;
  }
  if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || Root == NULL) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: cannot open container root\n"));
    return;
  }

  LoadCertDir (&mAuthCerts, SFB_SECURITY_AUTH_DIR, Root);
  LoadCertDir (&mBootCerts, SFB_SECURITY_BOOT_DIR, Root);
  Root->Close (Root);

  DEBUG ((EFI_D_INFO, "SFB-SEC: %u auth certs, %u boot certs\n",
          (UINT32)mAuthCerts.Count, (UINT32)mBootCerts.Count));
}

/* ---- queries used by this library and the policy checks ------------------ */

CONST SFB_CERT_POOL *
SfbCertsAuthPool (
  VOID
  )
{
  return &mAuthCerts;
}

CONST SFB_CERT_POOL *
SfbCertsBootPool (
  VOID
  )
{
  return &mBootCerts;
}

BOOLEAN
SfbSecurityAuthEnabled (
  VOID
  )
{
  return (BOOLEAN)(mAuthCerts.Count > 0);
}

BOOLEAN
SfbSecurityVerifyEnabled (
  VOID
  )
{
  return (BOOLEAN)(mBootCerts.Count > 0);
}
