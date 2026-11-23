/*
 * CONFIG\MEMCFG parsing and override application. See SfbMemCfg.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SfbMemCfg.h"

#define SFB_MEM_PAGE_MASK   0xFFFULL

STATIC CONST CHAR8 *CONST mTypeNames[SFB_MEM_TYPE_MAX + 1] = {
  "RESERVED", "LOADER_CODE", "LOADER_DATA", "BOOT_SERVICES_CODE",
  "BOOT_SERVICES_DATA", "RUNTIME_SERVICES_CODE", "RUNTIME_SERVICES_DATA",
  "CONVENTIONAL", "UNUSABLE", "ACPI_RECLAIM", "ACPI_NVS", "MMIO",
  "MMIO_PORT", "PAL_CODE", "PERSISTENT"
};

CONST CHAR8 *
SfbMemTypeName (
  IN UINT32  Type
  )
{
  return Type <= SFB_MEM_TYPE_MAX ? mTypeNames[Type] : "?";
}

/* ------------------------------------------------------------------------ */
/* override application                                                      */
/* ------------------------------------------------------------------------ */

EFI_STATUS
SfbMemApplyOverride (
  IN OUT SFB_MEM_DESC        *List,
  IN OUT UINT32              *Count,
  IN     UINT32               Max,
  IN     CONST SFB_MEM_DESC  *Ov
  )
{
  UINT64  B = Ov->Base;
  UINT64  E = Ov->Base + Ov->Size;
  UINT32  n0 = *Count;                      /* loop bound: parked tail excluded */
  UINT32  n = *Count;
  UINT32  i, w;
  UINT32  Extra = 1;                        /* the override itself */

  /* capacity: one possible split plus the override */
  for (i = 0; i < n; i++) {
    UINT64  s = List[i].Base, e = List[i].Base + List[i].Size;
    if (s < B && e > E) {
      Extra++;
      break;                                /* descriptors are disjoint: at most one */
    }
  }
  if (n + Extra > Max) {
    return EFI_OUT_OF_RESOURCES;
  }

  /* carve, compacting in place (w <= i always). The straddle tail is parked
   * at List[n0], past every entry the loop reads, or it would be visited -
   * and copied - twice. */
  w = 0;
  for (i = 0; i < n0; i++) {
    SFB_MEM_DESC  D = List[i];
    UINT64        s = D.Base, e = D.Base + D.Size;

    if (e <= B || s >= E) {
      List[w++] = D;                        /* disjoint */
    } else if (s >= B && e <= E) {
      /* inside the override: dropped */
    } else if (s < B && e > E) {
      /* straddles: head stays here, tail parks at the old end */
      D.Size = B - s;
      List[w++] = D;
      List[n0].Base = E;
      List[n0].Size = e - E;
      List[n0].Type = D.Type;
      List[n0].Attr = D.Attr;
      n++;
    } else if (s < B) {
      D.Size = B - s;                       /* overlap at the tail of D */
      List[w++] = D;
    } else {
      D.Base = E;                           /* overlap at the head of D */
      D.Size = e - E;
      List[w++] = D;
    }
  }
  /* the tail piece (if any) sits at index n0; move it down */
  for (i = n0; i < n; i++) {
    List[w++] = List[i];
  }
  List[w++] = *Ov;
  *Count = w;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* parser                                                                    */
/* ------------------------------------------------------------------------ */

STATIC BOOLEAN
SfbMemIsSpace (
  IN CHAR8  C
  )
{
  return C == ' ' || C == '\t' || C == '\r' || C == '\v' || C == '\f';
}

STATIC CHAR8
SfbMemUpper (
  IN CHAR8  C
  )
{
  return (C >= 'a' && C <= 'z') ? (CHAR8)(C - 'a' + 'A') : C;
}

/* hex with optional 0x/0X prefix; whole token must be consumed */
STATIC BOOLEAN
SfbMemParseHex (
  IN  CONST CHAR8  *S,
  IN  UINTN         Len,
  OUT UINT64       *Out
  )
{
  UINT64  V = 0;
  UINTN   i = 0;

  if (Len >= 2 && S[0] == '0' && (S[1] == 'x' || S[1] == 'X')) {
    i = 2;
  }
  if (i >= Len || Len - i > 16) {
    return FALSE;
  }
  for (; i < Len; i++) {
    CHAR8  C = SfbMemUpper (S[i]);
    UINT64 D;
    if (C >= '0' && C <= '9') {
      D = (UINT64)(C - '0');
    } else if (C >= 'A' && C <= 'F') {
      D = (UINT64)(C - 'A' + 10);
    } else {
      return FALSE;
    }
    V = (V << 4) | D;
  }
  *Out = V;
  return TRUE;
}

STATIC BOOLEAN
SfbMemParseDec (
  IN  CONST CHAR8  *S,
  IN  UINTN         Len,
  OUT UINT64       *Out
  )
{
  UINT64  V = 0;
  UINTN   i;

  if (Len == 0 || Len > 19) {
    return FALSE;
  }
  for (i = 0; i < Len; i++) {
    if (S[i] < '0' || S[i] > '9') {
      return FALSE;
    }
    V = V * 10 + (UINT64)(S[i] - '0');
  }
  *Out = V;
  return TRUE;
}

STATIC BOOLEAN
SfbMemParseType (
  IN  CONST CHAR8  *S,
  IN  UINTN         Len,
  OUT UINT32       *Out
  )
{
  UINT64  V;
  UINT32  t;

  if (Len >= 2 && S[0] == '0' && (S[1] == 'x' || S[1] == 'X')) {
    if (!SfbMemParseHex (S, Len, &V)) {
      return FALSE;
    }
  } else if (S[0] >= '0' && S[0] <= '9') {
    if (!SfbMemParseDec (S, Len, &V)) {
      return FALSE;
    }
  } else {
    for (t = 0; t <= SFB_MEM_TYPE_MAX; t++) {
      CONST CHAR8  *N = mTypeNames[t];
      UINTN         k;
      for (k = 0; k < Len && N[k] != '\0'; k++) {
        if (SfbMemUpper (S[k]) != N[k]) {
          break;
        }
      }
      if (k == Len && N[k] == '\0') {
        *Out = t;
        return TRUE;
      }
    }
    return FALSE;
  }
  if (V > SFB_MEM_TYPE_MAX) {
    return FALSE;
  }
  *Out = (UINT32)V;
  return TRUE;
}

/* next whitespace-delimited token in [*Pos, End); FALSE at end of line */
STATIC BOOLEAN
SfbMemNextToken (
  IN     CONST CHAR8  *Line,
  IN     UINTN         End,
  IN OUT UINTN        *Pos,
  OUT    UINTN        *TokStart,
  OUT    UINTN        *TokLen
  )
{
  UINTN  p = *Pos;

  while (p < End && SfbMemIsSpace (Line[p])) {
    p++;
  }
  if (p >= End) {
    return FALSE;
  }
  *TokStart = p;
  while (p < End && !SfbMemIsSpace (Line[p])) {
    p++;
  }
  *TokLen = p - *TokStart;
  *Pos = p;
  return TRUE;
}

STATIC BOOLEAN
SfbMemParseLine (
  IN  CONST CHAR8        *Line,
  IN  UINTN               Len,
  OUT SFB_MEM_OVERRIDE   *Ov
  )
{
  UINTN   Pos = 0, Ts, Tl, i;
  UINT64  Base, Size, Attr;
  UINT32  Type;

  /* NAME */
  if (!SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl)) {
    return FALSE;
  }
  for (i = 0; i < Tl && i < SFB_MEM_NAME_MAX - 1; i++) {
    Ov->Name[i] = Line[Ts + i];
  }
  Ov->Name[i] = '\0';

  /* BASE SIZE */
  if (!SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl) ||
      !SfbMemParseHex (Line + Ts, Tl, &Base)) {
    return FALSE;
  }
  if (!SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl) ||
      !SfbMemParseHex (Line + Ts, Tl, &Size)) {
    return FALSE;
  }
  if (Size == 0 || ((Base | Size) & SFB_MEM_PAGE_MASK) != 0 ||
      Base + Size < Base) {
    return FALSE;
  }

  /* TYPE ATTRIBUTE */
  if (!SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl) ||
      !SfbMemParseType (Line + Ts, Tl, &Type)) {
    return FALSE;
  }
  if (!SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl) ||
      !SfbMemParseHex (Line + Ts, Tl, &Attr)) {
    return FALSE;
  }
  /* trailing garbage is an error, not silently ignored */
  if (SfbMemNextToken (Line, Len, &Pos, &Ts, &Tl)) {
    return FALSE;
  }

  Ov->Desc.Base = Base;
  Ov->Desc.Size = Size;
  Ov->Desc.Type = Type;
  Ov->Desc.Attr = Attr;
  return TRUE;
}

VOID
SfbMemParseConfig (
  IN  CONST CHAR8        *Text,
  IN  UINTN               Len,
  OUT SFB_MEM_OVERRIDE   *Out,
  IN  UINT32              Max,
  OUT UINT32             *Count,
  OUT UINT32             *Rejected
  )
{
  UINTN  Pos = 0;

  *Count = 0;
  *Rejected = 0;
  if (Text == NULL) {
    return;
  }

  while (Pos < Len) {
    UINTN  Start = Pos;
    UINTN  End;
    UINTN  k;

    while (Pos < Len && Text[Pos] != '\n' && Text[Pos] != '\0') {
      Pos++;
    }
    End = Pos;
    if (Pos < Len && Text[Pos] == '\0') {
      Len = Pos;                            /* embedded NUL ends the text */
    }
    Pos++;                                  /* skip '\n' */

    /* strip comment and trailing whitespace */
    for (k = Start; k < End; k++) {
      if (Text[k] == '#') {
        End = k;
        break;
      }
    }
    while (End > Start && SfbMemIsSpace (Text[End - 1])) {
      End--;
    }
    while (Start < End && SfbMemIsSpace (Text[Start])) {
      Start++;
    }
    if (Start == End) {
      continue;                             /* blank or comment-only */
    }

    if (*Count >= Max) {
      (*Rejected)++;
      continue;
    }
    if (SfbMemParseLine (Text + Start, End - Start, &Out[*Count])) {
      (*Count)++;
    } else {
      (*Rejected)++;
    }
  }
}
