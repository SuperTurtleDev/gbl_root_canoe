/*
 * Host-side stand-ins for the handful of EDK2 BaseLib, BaseMemoryLib and
 * MemoryAllocationLib routines the boot menu's pure modules call, so those
 * modules compile unchanged against the real MdePkg headers.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* System headers first: X64 ProcessorBind.h pushes hidden visibility. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>


EFI_BOOT_SERVICES  *gBS = NULL;

VOID *EFIAPI CopyMem (VOID *D, CONST VOID *S, UINTN N) { return memmove (D, S, N); }
VOID *EFIAPI SetMem (VOID *B, UINTN N, UINT8 V) { return memset (B, V, N); }
VOID *EFIAPI ZeroMem (VOID *B, UINTN N) { return memset (B, 0, N); }
INTN EFIAPI CompareMem (CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp (A, B, N); }
GUID *EFIAPI CopyGuid (GUID *D, CONST GUID *S) { return memmove (D, S, sizeof (GUID)); }
BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return memcmp (A, B, sizeof (GUID)) == 0; }
BOOLEAN EFIAPI IsZeroGuid (CONST GUID *G)
{
  static const GUID Zero;
  return memcmp (G, &Zero, sizeof (GUID)) == 0;
}

/* This tree's BaseLib.h wraps these in inline fortify checks that call the
 * double-underscore implementations. */
UINTN EFIAPI __StrLen (CONST CHAR16 *S) { UINTN N = 0; while (S[N]) N++; return N; }
UINTN EFIAPI __StrSize (CONST CHAR16 *S) { return (__StrLen (S) + 1) * sizeof (CHAR16); }
UINTN EFIAPI __AsciiStrLen (CONST CHAR8 *S) { return strlen (S); }

INTN EFIAPI StrCmp (CONST CHAR16 *A, CONST CHAR16 *B)
{
  while (*A && *A == *B) { A++; B++; }
  return (INTN)*A - (INTN)*B;
}

INTN EFIAPI StrnCmp (CONST CHAR16 *A, CONST CHAR16 *B, UINTN N)
{
  if (N == 0) return 0;
  while (*A && *A == *B && N > 1) { A++; B++; N--; }
  return (INTN)*A - (INTN)*B;
}

RETURN_STATUS EFIAPI __StrnCpyS (CHAR16 *D, UINTN Max, CONST CHAR16 *S, UINTN Len)
{
  UINTN N = 0;
  if (Max == 0) return RETURN_INVALID_PARAMETER;
  while (N < Len && S[N] && N + 1 < Max) { D[N] = S[N]; N++; }
  D[N] = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS EFIAPI __StrCpyS (CHAR16 *D, UINTN Max, CONST CHAR16 *S)
{
  return __StrnCpyS (D, Max, S, Max);
}

UINT32 EFIAPI CalculateCrc32 (VOID *Data, UINTN Size)
{
  UINT32 Crc = 0xFFFFFFFF;
  CONST UINT8 *P = Data;
  while (Size--) {
    UINTN Bit;
    Crc ^= *P++;
    for (Bit = 0; Bit < 8; Bit++) Crc = (Crc >> 1) ^ (0xEDB88320 & (0 - (Crc & 1)));
  }
  return ~Crc;
}

VOID *EFIAPI AllocatePool (UINTN N) { return malloc (N ? N : 1); }
VOID *EFIAPI AllocateZeroPool (UINTN N) { return calloc (1, N ? N : 1); }
VOID EFIAPI FreePool (VOID *P) { free (P); }

VOID *EFIAPI AllocateAlignedPages (UINTN Pages, UINTN Align)
{
  VOID *P = NULL;
  if (Align < 4096) Align = 4096;
  if (posix_memalign (&P, Align, Pages * 4096) != 0) return NULL;
  return P;
}

VOID EFIAPI FreeAlignedPages (VOID *P, UINTN Pages) { (void)Pages; free (P); }

VOID FortifyFail (CONST CHAR8 *Name, CONST UINTN Line)
{
  fprintf (stderr, "fortify: %s:%u\n", Name, (unsigned)Line);
  abort ();
}
