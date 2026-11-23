/*
 * Pure interval arithmetic for the GetMemoryMap merge (no UEFI services, no
 * logging). Compiles inside EDK2 and on a host with -DSFB_HOST_TEST, see
 * Test/ next to this file.
 *
 * Interval convention: every region is half-open [Base, Base + Size). The
 * TZ partition table (Base + AvailableLength) and the EFI descriptors
 * (PhysicalStart .. + NumberOfPages * 4K) are both natively half-open, so no
 * closed/open conversion happens anywhere. Two regions merely "touch" when
 * one's end equals the other's base; touching is not overlap and never
 * carves anything.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_MEM_REGIONS_H__
#define __SFB_MEM_REGIONS_H__

#ifdef SFB_HOST_TEST
  #include <stdint.h>
  #include <stddef.h>
  typedef uint8_t   UINT8;
  typedef uint32_t  UINT32;
  typedef uint64_t  UINT64;
  typedef int32_t   INT32;
  typedef size_t    UINTN;
  typedef char      CHAR8;
  typedef int       BOOLEAN;
  typedef UINT64    EFI_STATUS;
  #define TRUE  1
  #define FALSE 0
  #define IN
  #define OUT
  #define CONST const
  #define STATIC static
  #define VOID  void
  #define MAX_BIT               (1ULL << 63)
  #define ENCODE_ERROR(a)       ((EFI_STATUS)(MAX_BIT | (a)))
  #define EFI_SUCCESS           0
  #define EFI_INVALID_PARAMETER ENCODE_ERROR (2)
  #define EFI_BUFFER_TOO_SMALL  ENCODE_ERROR (5)
  #define EFI_OUT_OF_RESOURCES  ENCODE_ERROR (9)
  #define EFI_ERROR(s)          (((int64_t)(s)) < 0)
#else
  #include <Uefi.h>
#endif

/* Upper bound on TZ partitions / merge pieces handled; the SM8850 table has
 * 11 entries, the firmware map ~110 descriptors. */
#define SFB_MEM_MAX_REGIONS   64

typedef struct {
  UINT64  Base;
  UINT64  Size;
} SFB_MEM_REGION;

/* One piece of a RESERVED descriptor split against the TZ partition list:
 * InTz = TRUE means the piece lies inside a TZ partition and is released
 * (re-emitted as CONVENTIONAL); FALSE means it stays RESERVED. */
typedef struct {
  UINT64   Base;
  UINT64   Size;
  BOOLEAN  InTz;
} SFB_MEM_PIECE;

/*
 * Sort regions by Base (in place) and merge overlapping / adjacent ones.
 * Zero-size regions are dropped. Returns the number of merged regions.
 */
UINT32
SfbMemSortAndCombine (
  IN OUT SFB_MEM_REGION  *Regions,
  IN     UINT32           Count
  );

/*
 * Out := Parts minus the (sorted, merged) Sub regions. Handles a subtrahend
 * at the head, in the middle, at the tail of a part, covering the whole part,
 * or spanning several parts. Zero-length parts are dropped.
 *
 * @retval EFI_OUT_OF_RESOURCES  OutMax too small.
 */
EFI_STATUS
SfbMemSubtract (
  IN  CONST SFB_MEM_REGION  *Parts,
  IN  UINT32                 PartCount,
  IN  CONST SFB_MEM_REGION  *Sub,
  IN  UINT32                 SubCount,
  OUT SFB_MEM_REGION        *Out,
  IN  UINT32                 OutMax,
  OUT UINT32                *OutCount
  );

/*
 * Split [Base, Base + Size) against the sorted+merged TZ list into
 * alternating released / unreleased pieces, walking left to right. A region
 * fully inside TZ yields one released piece, fully outside one unreleased
 * piece, straddlers split at every boundary.
 *
 * @retval >= 0  number of pieces written
 * @retval -1    PiecesMax too small
 */
INT32
SfbMemSplitVsTz (
  IN  UINT64                 Base,
  IN  UINT64                 Size,
  IN  CONST SFB_MEM_REGION  *Tz,
  IN  UINT32                 NumTz,
  OUT SFB_MEM_PIECE         *Pieces,
  IN  UINT32                 PiecesMax
  );

/*
 * Three-step GetMemoryMap merge:
 *
 *   Step 1  TZ - non-usable firmware claims (RESERVED / RUNTIME_* / MMIO ...)
 *   Step 2  (result of step 1) - usable firmware claims (never describe the
 *           same memory twice)
 *   Step 3  the caller appends Addition as synthetic CONVENTIONAL entries
 *
 * FirmwareClaims and UsableClaims are sorted and merged in place (firmware
 * maps are not address ordered). Scratch must hold at least AdditionMax
 * entries. TZ is trusted to be AP-usable memory.
 *
 * @retval EFI_BUFFER_TOO_SMALL  AdditionMax < NumTz or ScratchMax too small.
 */
EFI_STATUS
SfbMemComputeMerge (
  IN  SFB_MEM_REGION  *Tz,
  IN  UINT32           NumTz,
  IN  SFB_MEM_REGION  *FirmwareClaims,
  IN  UINT32           NumFirmwareClaims,
  IN  SFB_MEM_REGION  *UsableClaims,
  IN  UINT32           NumUsableClaims,
  OUT SFB_MEM_REGION  *Addition,
  IN  UINT32           AdditionMax,
  OUT SFB_MEM_REGION  *Scratch,
  IN  UINT32           ScratchMax,
  OUT UINT32          *NumAddition
  );

#endif /* __SFB_MEM_REGIONS_H__ */
