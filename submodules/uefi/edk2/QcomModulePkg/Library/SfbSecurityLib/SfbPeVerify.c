/*
 * PE image signature verification for the SFB security policy: compute the
 * Secure-Boot authentihash of the image and verify the embedded PKCS#7
 * signature against the class B (BOOT_CERTS) keys.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>

#include <mbedtls/md.h>
#include <mbedtls/pkcs7.h>
#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

/* ---- PE parsing ---------------------------------------------------------- */

#define PE_SIGNATURE       0x00004550  /* "PE\0\0" */
#define PE_DIR_SECURITY    4
#define PE_OPT_MAGIC_32    0x010b
#define PE_OPT_MAGIC_64    0x020b

typedef struct {
  UINT32  OffsetToData;   /* for the security dir: file offset of cert table */
  UINT32  Size;
} PE_DATA_DIR;

typedef struct {
  CONST UINT8  *Image;
  UINTN        ImageSize;
  UINT32       CheckSumOffset;   /* file offset of the optional header checksum */
  UINT32       SecDirOffset;     /* file offset of the security data directory */
  CONST PE_DATA_DIR *SecDir;     /* mapped SecDirOffset within the image */
  UINT32       SizeOfHeaders;
} PE_INFO;

STATIC
BOOLEAN
ReadU16 (
  IN CONST UINT8  *Buf,
  IN UINTN        ImageSize,
  IN UINTN        Offset,
  OUT UINT16      *Value
  )
{
  if (Offset + sizeof (UINT16) > ImageSize) {
    return FALSE;
  }
  *Value = ReadUnaligned16 ((CONST UINT16 *)(Buf + Offset));
  return TRUE;
}

STATIC
BOOLEAN
ReadU32 (
  IN CONST UINT8  *Buf,
  IN UINTN        ImageSize,
  IN UINTN        Offset,
  OUT UINT32      *Value
  )
{
  if (Offset + sizeof (UINT32) > ImageSize) {
    return FALSE;
  }
  *Value = ReadUnaligned32 ((CONST UINT32 *)(Buf + Offset));
  return TRUE;
}

/* Locate the pieces of a PE/PE+ image the authentihash must skip. */
STATIC
BOOLEAN
PeParse (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize,
  OUT PE_INFO    *Info
  )
{
  CONST UINT8  *Buf = Image;
  UINT32       PeOff;
  UINT32       Signature;
  UINT16       OptMagic;
  UINT32       OptOff;
  UINT32       DataDirOff;

  ZeroMem (Info, sizeof (*Info));
  Info->Image = Buf;
  Info->ImageSize = ImageSize;

  if (ImageSize < sizeof (UINT16) + sizeof (UINT32)) {
    return FALSE;
  }
  if (!ReadU32 (Buf, ImageSize, 0x3c, &PeOff) ||
      !ReadU32 (Buf, ImageSize, PeOff, &Signature) ||
      Signature != PE_SIGNATURE) {
    return FALSE;
  }

  /* COFF header is 20 bytes; the optional header follows. CheckSum sits at
   * offset 64 of the optional header in both PE32 and PE32+. */
  OptOff = PeOff + 4 + 20;
  if (!ReadU16 (Buf, ImageSize, OptOff, &OptMagic) ||
      (OptMagic != PE_OPT_MAGIC_32 && OptMagic != PE_OPT_MAGIC_64)) {
    return FALSE;
  }
  Info->CheckSumOffset = OptOff + 64;

  /* Data directories sit after the fixed part of the optional header:
   * 96 bytes (PE32) / 112 bytes (PE32+). */
  DataDirOff = OptOff + (OptMagic == PE_OPT_MAGIC_32 ? 96 : 112);

  Info->SecDirOffset = DataDirOff + PE_DIR_SECURITY * sizeof (PE_DATA_DIR);
  if (Info->SecDirOffset + sizeof (PE_DATA_DIR) > ImageSize) {
    return FALSE;
  }
  Info->SecDir = (CONST PE_DATA_DIR *)(Buf + Info->SecDirOffset);

  if (!ReadU32 (Buf, ImageSize, OptOff + 60, &Info->SizeOfHeaders) ||
      Info->SizeOfHeaders == 0 || Info->SizeOfHeaders > ImageSize) {
    return FALSE;
  }
  return TRUE;
}

/* ---- authentihash -------------------------------------------------------- */

/* Hash one contiguous range, clipped to the image bounds. */
STATIC
VOID
HashRange (
  IN mbedtls_md_context_t  *Md,
  IN CONST UINT8           *Buf,
  IN UINTN                 ImageSize,
  IN UINTN                 Start,
  IN UINTN                 End
  )
{
  if (Start >= End) {
    return;
  }
  if (End > ImageSize) {
    End = ImageSize;
  }
  if (Start >= End) {
    return;
  }
  (VOID)mbedtls_md_update (Md, Buf + Start, End - Start);
}

/*
 * The Secure-Boot message digest of the image: everything is hashed except
 * the COFF checksum field, the security data-directory entry and the
 * certificate table itself.
 */
STATIC
BOOLEAN
AuthentiHash (
  IN CONST PE_INFO  *Info,
  OUT UINT8         Hash[32]
  )
{
  mbedtls_md_context_t  Md;
  CONST UINT8          *Zero8;
  UINTN                SecDataStart;
  UINTN                SecDataEnd;
  BOOLEAN              Ok = FALSE;
  STATIC CONST UINT8   Zeros[8] = { 0 };

  Zero8 = Zeros;

  mbedtls_md_init (&Md);
  if (mbedtls_md_setup (&Md, mbedtls_md_info_from_type (MBEDTLS_MD_SHA256),
                        0) != 0 ||
      mbedtls_md_starts (&Md) != 0) {
    goto out;
  }

  SecDataStart = Info->SecDir->OffsetToData;
  SecDataEnd = SecDataStart + Info->SecDir->Size;
  if (SecDataEnd < SecDataStart || SecDataEnd > Info->ImageSize) {
    goto out;
  }

  /* Headers up to the checksum, skip checksum, headers up to the security
   * directory entry, the entry counts as zeros, then the rest of the
   * headers. */
  HashRange (&Md, Info->Image, Info->ImageSize, 0, Info->CheckSumOffset);
  HashRange (&Md, Info->Image, Info->ImageSize, Info->CheckSumOffset + 4,
             Info->SecDirOffset);
  (VOID)mbedtls_md_update (&Md, Zero8, 8);
  HashRange (&Md, Info->Image, Info->ImageSize,
             Info->SecDirOffset + sizeof (PE_DATA_DIR), Info->SizeOfHeaders);

  /* Section data up to the certificate table, then past it to the end. */
  HashRange (&Md, Info->Image, Info->ImageSize, Info->SizeOfHeaders,
             SecDataStart);
  HashRange (&Md, Info->Image, Info->ImageSize, SecDataEnd,
             Info->ImageSize);

  Ok = (BOOLEAN)(mbedtls_md_finish (&Md, Hash) == 0);

out:
  mbedtls_md_free (&Md);
  return Ok;
}

/* ---- verification -------------------------------------------------------- */

EFI_STATUS
SfbSecurityVerifyImage (
  IN CONST VOID  *PeImage,
  IN UINTN       PeSize
  )
{
  PE_INFO               Info;
  UINT8                 Hash[32];
  CONST SFB_CERT_POOL   *Pool = SfbCertsBootPool ();
  UINTN                 Index;
  mbedtls_pkcs7         Pkcs7;
  mbedtls_x509_crt      Crt;
  INT32                 MbedErr;
  BOOLEAN               Verified = FALSE;

  if (PeImage == NULL || PeSize == 0) {
    return EFI_INVALID_PARAMETER;
  }

  /* No class B keys: no verification. Privilege never exempts an image
   * from verification - the two policies are independent. */
  if (Pool->Count == 0) {
    return EFI_SUCCESS;
  }

  if (!PeParse (PeImage, PeSize, &Info)) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: image is not PE\n"));
    return EFI_SECURITY_VIOLATION;
  }
  if (Info.SecDir->Size == 0 || Info.SecDir->OffsetToData == 0) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: image carries no signature\n"));
    return EFI_UNSUPPORTED;
  }
  if (!AuthentiHash (&Info, Hash)) {
    return EFI_SECURITY_VIOLATION;
  }

  /* The certificate table is a sequence of 8-byte-aligned WIN_CERTIFICATE
   * blobs; the signed-data blob carries the PKCS#7 DER. Verify each one
   * against every BOOT_CERTS key. */
  {
    UINTN  Off = Info.SecDir->OffsetToData;
    UINTN  End = Off + Info.SecDir->Size;

    while (Off + 8 <= End && !Verified) {
      /* WIN_CERTIFICATE: wLength (UINT16), wRevision, wCertificateType,
       * then the certificate body, padded to 8-byte multiples. */
      UINT16  Length = ReadUnaligned16 (
                         (CONST UINT16 *)(Info.Image + Off));
      UINT16  Revision = ReadUnaligned16 (
                           (CONST UINT16 *)(Info.Image + Off + 2));
      UINT16  CertType = ReadUnaligned16 (
                           (CONST UINT16 *)(Info.Image + Off + 4));

      if (Length < 8 || Off + Length > End) {
        break;
      }
      if (CertType == 0x0002 /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */ &&
          Revision == 0x0200) {
        mbedtls_pkcs7_init (&Pkcs7);
        /* mbedtls_pkcs7_parse_der returns the non-negative content type on
         * success and a negative error code on failure. */
        MbedErr = mbedtls_pkcs7_parse_der (&Pkcs7,
                                           Info.Image + Off + 8,
                                           Length - 8);
        if (MbedErr >= 0) {
          for (Index = 0; Index < Pool->Count && !Verified; Index++) {
            mbedtls_x509_crt_init (&Crt);
            if (mbedtls_x509_crt_parse_der (&Crt, Pool->Der[Index],
                                            Pool->DerSize[Index]) == 0) {
              Verified = (BOOLEAN)(mbedtls_pkcs7_signed_hash_verify (
                                     &Pkcs7, &Crt, Hash, sizeof (Hash)) == 0);
            }
            mbedtls_x509_crt_free (&Crt);
          }
        }
        mbedtls_pkcs7_free (&Pkcs7);
      }
      Off += (Length + 7) & ~(UINTN)7;
    }
  }

  if (!Verified) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: image signature rejected\n"));
    return EFI_SECURITY_VIOLATION;
  }
  DEBUG ((EFI_D_INFO, "SFB-SEC: image signature verified\n"));
  return EFI_SUCCESS;
}
