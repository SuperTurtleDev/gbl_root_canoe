/*
 * Pure interval arithmetic for the GetMemoryMap merge. See SfbMemRegions.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SfbMemRegions.h"

UINT32
SfbMemSortAndCombine (
  IN OUT SFB_MEM_REGION  *Regions,
  IN     UINT32           Count
  )
{
  UINT32          i, j, n;
  SFB_MEM_REGION  Tmp;

  if (Regions == NULL || Count == 0) {
    return 0;
  }

  /* drop zero-size entries */
  n = 0;
  for (i = 0; i < Count; i++) {
    if (Regions[i].Size != 0) {
      Regions[n++] = Regions[i];
    }
  }
  Count = n;
  if (Count == 0) {
    return 0;
  }

  /* insertion sort by Base (Count is small) */
  for (i = 1; i < Count; i++) {
    Tmp = Regions[i];
    j = i;
    while (j > 0 && Regions[j - 1].Base > Tmp.Base) {
      Regions[j] = Regions[j - 1];
      j--;
    }
    Regions[j] = Tmp;
  }

  /* merge overlapping or adjacent */
  n = 0;
  for (i = 1; i < Count; i++) {
    UINT64  CurEnd = Regions[n].Base + Regions[n].Size;
    UINT64  NxtEnd = Regions[i].Base + Regions[i].Size;

    if (Regions[i].Base <= CurEnd) {
      if (NxtEnd > CurEnd) {
        Regions[n].Size = NxtEnd - Regions[n].Base;
      }
    } else {
      n++;
      Regions[n] = Regions[i];
    }
  }
  return n + 1;
}

EFI_STATUS
SfbMemSubtract (
  IN  CONST SFB_MEM_REGION  *Parts,
  IN  UINT32                 PartCount,
  IN  CONST SFB_MEM_REGION  *Sub,
  IN  UINT32                 SubCount,
  OUT SFB_MEM_REGION        *Out,
  IN  UINT32                 OutMax,
  OUT UINT32                *OutCount
  )
{
  UINT32  i, j, n;

  if (Parts == NULL || Out == NULL || OutCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (SubCount != 0 && Sub == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  n = 0;
  for (i = 0; i < PartCount; i++) {
    UINT64  Cur = Parts[i].Base;
    UINT64  End = Parts[i].Base + Parts[i].Size;

    if (Parts[i].Size == 0) {
      continue;
    }

    for (j = 0; j < SubCount && Cur < End; j++) {
      UINT64  RS = Sub[j].Base;
      UINT64  RE = Sub[j].Base + Sub[j].Size;

      if (RE <= Cur) {
        continue;                           /* entirely below */
      }
      if (RS >= End) {
        break;                              /* sorted: rest are above */
      }
      if (RS > Cur) {                       /* keep [Cur, RS) */
        if (n >= OutMax) {
          return EFI_OUT_OF_RESOURCES;
        }
        Out[n].Base = Cur;
        Out[n].Size = RS - Cur;
        n++;
      }
      if (RE > Cur) {
        Cur = RE;                           /* skip the subtrahend */
      }
    }

    if (Cur < End) {                        /* remaining tail */
      if (n >= OutMax) {
        return EFI_OUT_OF_RESOURCES;
      }
      Out[n].Base = Cur;
      Out[n].Size = End - Cur;
      n++;
    }
  }

  *OutCount = n;
  return EFI_SUCCESS;
}

INT32
SfbMemSplitVsTz (
  IN  UINT64                 Base,
  IN  UINT64                 Size,
  IN  CONST SFB_MEM_REGION  *Tz,
  IN  UINT32                 NumTz,
  OUT SFB_MEM_PIECE         *Pieces,
  IN  UINT32                 PiecesMax
  )
{
  UINT64  Cur = Base;
  UINT64  End = Base + Size;
  UINT32  n = 0;
  UINT32  i;

  for (i = 0; i < NumTz && Cur < End; i++) {
    UINT64  Ts = Tz[i].Base;
    UINT64  Te = Tz[i].Base + Tz[i].Size;

    if (Te <= Cur) {
      continue;                             /* partition below the cursor */
    }
    if (Ts >= End) {
      break;                                /* sorted: rest is above */
    }
    if (Ts > Cur) {                         /* gap before this partition: stays RESERVED */
      if (n >= PiecesMax) {
        return -1;
      }
      Pieces[n].Base = Cur;
      Pieces[n].Size = Ts - Cur;
      Pieces[n].InTz = FALSE;
      n++;
      Cur = Ts;
    }
    if (n >= PiecesMax) {
      return -1;
    }
    Pieces[n].Base = Cur;                   /* inside this partition: released */
    Pieces[n].Size = (Te < End ? Te : End) - Cur;
    Pieces[n].InTz = TRUE;
    n++;
    Cur = Te < End ? Te : End;
  }

  if (Cur < End) {                          /* tail above every partition */
    if (n >= PiecesMax) {
      return -1;
    }
    Pieces[n].Base = Cur;
    Pieces[n].Size = End - Cur;
    Pieces[n].InTz = FALSE;
    n++;
  }
  return (INT32)n;
}

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
  )
{
  EFI_STATUS  Status;
  UINT32      n, i;

  if (Tz == NULL || Addition == NULL || Scratch == NULL || NumAddition == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (NumTz > AdditionMax || AdditionMax > ScratchMax) {
    return EFI_BUFFER_TOO_SMALL;
  }

  NumFirmwareClaims = SfbMemSortAndCombine (FirmwareClaims, NumFirmwareClaims);
  NumUsableClaims = SfbMemSortAndCombine (UsableClaims, NumUsableClaims);

  /* Addition := TZ (step 2's initial copy doubles as step 1's subject) */
  for (i = 0; i < NumTz; i++) {
    Addition[i] = Tz[i];
  }
  n = SfbMemSortAndCombine (Addition, NumTz);

  /* step 1: TZ - firmware (non-usable) claims */
  if (NumFirmwareClaims != 0) {
    Status = SfbMemSubtract (Addition, n, FirmwareClaims, NumFirmwareClaims,
                             Scratch, ScratchMax, &n);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    for (i = 0; i < n; i++) {
      Addition[i] = Scratch[i];
    }
  }

  /* step 2: Addition - firmware usable claims */
  if (NumUsableClaims != 0) {
    Status = SfbMemSubtract (Addition, n, UsableClaims, NumUsableClaims,
                             Scratch, ScratchMax, &n);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    for (i = 0; i < n; i++) {
      Addition[i] = Scratch[i];
    }
  }

  /* all inputs are page-granular, so the differences are too; drop empties */
  *NumAddition = SfbMemSortAndCombine (Addition, n);
  return EFI_SUCCESS;
}
