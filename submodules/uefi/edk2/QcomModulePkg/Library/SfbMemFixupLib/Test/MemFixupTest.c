/*
 * Host-side tests for the GetMemoryMap merge arithmetic (SfbMemRegions.c).
 *
 * Build & run: ./test.sh (no EDK2, plain cc). Coverage:
 *   - interval edges: head / tail / middle split, exact cover, cover
 *     spanning several regions, touching (no overlap), zero-size drops
 *   - the three merge steps end to end on synthetic tables
 *   - RESERVED-vs-TZ split edges
 *   - regression against real device dumps (fixtures/memmap.txt and
 *     fixtures/tzmap.txt, fetched over fastboot on an SM8850 device): the
 *     expected addition list was computed independently, plus a
 *     conservation check that the three steps partition the TZ total.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../SfbMemRegions.h"

#define MAXR 128

static unsigned long mChecks;
static unsigned long mFails;

#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    mChecks++;                                                               \
    if (!(cond)) {                                                           \
      mFails++;                                                              \
      printf ("FAIL %s:%d: ", __func__, __LINE__);                           \
      printf (__VA_ARGS__);                                                  \
      printf ("\n");                                                         \
    }                                                                        \
  } while (0)

#define N(a)  (sizeof (a) / sizeof ((a)[0]))

static UINT32
Make (const UINT64 bases[], const UINT64 sizes[], UINT32 n, SFB_MEM_REGION *r)
{
  UINT32  i;
  for (i = 0; i < n; i++) {
    r[i].Base = bases[i];
    r[i].Size = sizes[i];
  }
  return n;
}

static UINT32
Merge (SFB_MEM_REGION *Tz, UINT32 nTz, SFB_MEM_REGION *Firm, UINT32 nF,
       SFB_MEM_REGION *Use, UINT32 nU, SFB_MEM_REGION *Add)
{
  SFB_MEM_REGION  Scr[MAXR * 2];
  UINT32          n = 0;
  EFI_STATUS      st;

  st = SfbMemComputeMerge (Tz, nTz, Firm, nF, Use, nU, Add, MAXR, Scr, N (Scr), &n);
  CHECK (st == EFI_SUCCESS, "merge status %llx", (unsigned long long)st);
  return n;
}

static void
TestSubtractEdges (void)
{
  SFB_MEM_REGION  Tz[MAXR], Claims[MAXR], Add[MAXR];
  UINT32          n;

  const UINT64 tb[] = { 0x1000 }, ts[] = { 0x1000 };

  /* middle split */
  const UINT64 cb[] = { 0x1400 }, cs[] = { 0x400 };
  Make (tb, ts, 1, Tz);
  Make (cb, cs, 1, Claims);
  n = Merge (Tz, 1, Claims, 1, NULL, 0, Add);
  CHECK (n == 2, "middle split: n=%u want 2", n);
  if (n == 2) {
    CHECK (Add[0].Base == 0x1000 && Add[0].Size == 0x400, "head piece");
    CHECK (Add[1].Base == 0x1800 && Add[1].Size == 0x800, "tail piece");
  }

  /* exact cover */
  const UINT64 cb2[] = { 0x1000 }, cs2[] = { 0x1000 };
  Make (tb, ts, 1, Tz);
  Make (cb2, cs2, 1, Claims);
  n = Merge (Tz, 1, Claims, 1, NULL, 0, Add);
  CHECK (n == 0, "exact cover: n=%u want 0", n);

  /* head / tail bites */
  const UINT64 cb3[] = { 0x1000 }, cs3[] = { 0x200 };
  Make (tb, ts, 1, Tz);
  Make (cb3, cs3, 1, Claims);
  n = Merge (Tz, 1, Claims, 1, NULL, 0, Add);
  CHECK (n == 1 && Add[0].Base == 0x1200 && Add[0].Size == 0xe00, "head bite");

  const UINT64 cb4[] = { 0x1e00 }, cs4[] = { 0x200 };
  Make (tb, ts, 1, Tz);
  Make (cb4, cs4, 1, Claims);
  n = Merge (Tz, 1, Claims, 1, NULL, 0, Add);
  CHECK (n == 1 && Add[0].Base == 0x1000 && Add[0].Size == 0xe00, "tail bite");

  /* touching is not overlap */
  const UINT64 cb5[] = { 0x0, 0x2000 }, cs5[] = { 0x1000, 0x1000 };
  Make (tb, ts, 1, Tz);
  Make (cb5, cs5, 2, Claims);
  n = Merge (Tz, 1, Claims, 2, NULL, 0, Add);
  CHECK (n == 1 && Add[0].Base == 0x1000 && Add[0].Size == 0x1000,
         "touching boundaries must not carve");

  /* claim spanning several TZ partitions */
  const UINT64 tb6[] = { 0x100000, 0x300000 }, ts6[] = { 0x100000, 0x100000 };
  const UINT64 cb6[] = { 0x180000 }, cs6[] = { 0x200000 };
  Make (tb6, ts6, 2, Tz);
  Make (cb6, cs6, 1, Claims);
  n = Merge (Tz, 2, Claims, 1, NULL, 0, Add);
  CHECK (n == 2 && Add[0].Base == 0x100000 && Add[0].Size == 0x80000 &&
         Add[1].Base == 0x380000 && Add[1].Size == 0x80000,
         "spanning claim across merged partitions, n=%u", n);

  /* zero-size inputs are dropped */
  const UINT64 tb7[] = { 0x1000, 0x5000 }, ts7[] = { 0x1000, 0 };
  Make (tb7, ts7, 2, Tz);
  n = Merge (Tz, 2, NULL, 0, NULL, 0, Add);
  CHECK (n == 1 && Add[0].Base == 0x1000, "zero-size TZ entry dropped, n=%u", n);
}

static void
TestThreeSteps (void)
{
  SFB_MEM_REGION  Tz[MAXR], Firm[MAXR], Use[MAXR], Add[MAXR];
  UINT32          n;

  /* TZ = [1M,3M) + [5M,6M); firmware RESERVED eats [1.5M,2M);
   * usable claims [2.5M,3M) and [5M,5.5M);
   * expected Addition = [1M,1.5M) + [2M,2.5M) + [5.5M,6M) */
  const UINT64 tb[] = { 0x100000, 0x500000 }, ts[] = { 0x200000, 0x100000 };
  const UINT64 fb[] = { 0x180000 }, fs[] = { 0x80000 };
  const UINT64 ub[] = { 0x280000, 0x500000 }, us[] = { 0x80000, 0x80000 };
  Make (tb, ts, 2, Tz);
  Make (fb, fs, 1, Firm);
  Make (ub, us, 2, Use);
  n = Merge (Tz, 2, Firm, 1, Use, 2, Add);
  CHECK (n == 3, "three steps: n=%u want 3", n);
  if (n == 3) {
    CHECK (Add[0].Base == 0x100000 && Add[0].Size == 0x80000, "piece 0");
    CHECK (Add[1].Base == 0x200000 && Add[1].Size == 0x80000, "piece 1");
    CHECK (Add[2].Base == 0x580000 && Add[2].Size == 0x80000, "piece 2");
  }
}

static void
TestSplitEdges (void)
{
  SFB_MEM_REGION  Tz[4];
  SFB_MEM_PIECE   P[8];
  INT32           n;

  const UINT64 tb[] = { 0x1000 }, ts[] = { 0x1000 };
  Make (tb, ts, 1, Tz);

  n = SfbMemSplitVsTz (0x1800, 0x800, Tz, 1, P, 8);
  CHECK (n == 1 && P[0].InTz && P[0].Base == 0x1800 && P[0].Size == 0x800,
         "fully inside: n=%d", n);

  n = SfbMemSplitVsTz (0x0, 0x1000, Tz, 1, P, 8);
  CHECK (n == 1 && !P[0].InTz && P[0].Base == 0 && P[0].Size == 0x1000,
         "fully outside: n=%d", n);

  n = SfbMemSplitVsTz (0x1800, 0x1000, Tz, 1, P, 8);
  CHECK (n == 2 && P[0].InTz && P[0].Base == 0x1800 && P[0].Size == 0x800 &&
         !P[1].InTz && P[1].Base == 0x2000 && P[1].Size == 0x800,
         "tail straddle: n=%d", n);

  n = SfbMemSplitVsTz (0x800, 0x1000, Tz, 1, P, 8);
  CHECK (n == 2 && !P[0].InTz && P[0].Base == 0x800 && P[0].Size == 0x800 &&
         P[1].InTz && P[1].Base == 0x1000 && P[1].Size == 0x800,
         "head straddle: n=%d", n);

  n = SfbMemSplitVsTz (0x0, 0x4000, Tz, 1, P, 8);
  CHECK (n == 3 && !P[0].InTz && P[1].InTz && !P[2].InTz &&
         P[2].Base == 0x2000 && P[2].Size == 0x2000,
         "span one partition: n=%d", n);

  const UINT64 tb2[] = { 0x1000, 0x3000 }, ts2[] = { 0x1000, 0x1000 };
  Make (tb2, ts2, 2, Tz);
  n = SfbMemSplitVsTz (0x0, 0x5000, Tz, 2, P, 8);
  CHECK (n == 5 && !P[0].InTz && P[1].InTz && !P[2].InTz &&
         P[3].InTz && !P[4].InTz && P[4].Size == 0x1000,
         "span two partitions: n=%d", n);

  n = SfbMemSplitVsTz (0x1000, 0x1000, Tz, 2, P, 0);
  CHECK (n == -1, "capacity guard: n=%d", n);
  n = SfbMemSplitVsTz (0x0, 0x5000, Tz, 2, P, 3);
  CHECK (n == -1, "capacity guard mid-walk: n=%d", n);
}

/* sorted-walk intersection sum of two sorted+merged lists */
static UINT64
IntersectSum (const SFB_MEM_REGION *a, UINT32 na, const SFB_MEM_REGION *b, UINT32 nb)
{
  UINT64  sum = 0;
  UINT32  i = 0, j = 0;

  while (i < na && j < nb) {
    UINT64  s = a[i].Base > b[j].Base ? a[i].Base : b[j].Base;
    UINT64  e1 = a[i].Base + a[i].Size, e2 = b[j].Base + b[j].Size;
    UINT64  e = e1 < e2 ? e1 : e2;
    if (s < e) {
      sum += e - s;
    }
    if (e1 <= e2) {
      i++;
    }
    if (e2 <= e1) {
      j++;
    }
  }
  return sum;
}

/* kernel is_usable_memory(): types 1,2,3,4,7,9,14 with WB (0x8) */
static int
IsUsable (unsigned ty, unsigned long long attr)
{
  return (ty == 1 || ty == 2 || ty == 3 || ty == 4 || ty == 7 || ty == 9 ||
          ty == 14) && (attr & 0x8);
}

typedef struct {
  unsigned            Type;
  unsigned long long  Attr;
  unsigned long long  Start;
  unsigned long long  End;      /* inclusive, as dumped */
} DumpEntry;

static UINT32
LoadMemmap (const char *path, DumpEntry *out, UINT32 max)
{
  FILE  *f = fopen (path, "r");
  char   line[512];
  UINT32 n = 0;

  if (f == NULL) {
    return 0;
  }
  while (fgets (line, sizeof (line), f) != NULL && n < max) {
    char type[64];
    if (sscanf (line, "%*d %63s %x 0x%llx %llx-%llx", type, &out[n].Type,
                &out[n].Attr, &out[n].Start, &out[n].End) == 5) {
      n++;
    }
  }
  fclose (f);
  return n;
}

static void
TestRealData (const char *MmapPath, const char *TzPath)
{
  FILE            *f;
  char             line[512];
  SFB_MEM_REGION   Tz[MAXR], Firm[MAXR], Use[MAXR], Add[MAXR];
  DumpEntry        Map[MAXR * 2];
  UINT32           nTz = 0, nFirm = 0, nUse = 0, nMap, n, i;

  /* address-sorted, matching the algorithm's output order */
  static const struct { UINT64 base, size; } want[] = {
    { 0x000080000000ULL, 0x00000000d60000ULL },
    { 0x000080d61000ULL, 0x000000004df000ULL },
    { 0x000081300000ULL, 0x00000000100000ULL },
    { 0x000081a60000ULL, 0x000000001a0000ULL },
    { 0x000081ca0000ULL, 0x00000000044000ULL },
    { 0x000081cf4000ULL, 0x0000000000c000ULL },
    { 0x000082000000ULL, 0x000000004a0000ULL },
    { 0x000082600000ULL, 0x00000008400000ULL },
    { 0x0000aa980000ULL, 0x00000005770000ULL },
    { 0x0000c328b000ULL, 0x00000000b45000ULL },
    { 0x0000d79c0000ULL, 0x00000000040000ULL },
    { 0x0000d7c00000ULL, 0x00000000429000ULL },
    { 0x0000e25a0000ULL, 0x000001a260000ULL },
  };

  f = fopen (TzPath, "r");
  CHECK (f != NULL, "open %s", TzPath);
  if (f == NULL) {
    return;
  }
  while (fgets (line, sizeof (line), f) != NULL) {
    unsigned long long  s, e;
    if (sscanf (line, "%*d %llx-%llx", &s, &e) == 2 && nTz < MAXR) {
      Tz[nTz].Base = s;
      Tz[nTz].Size = e - s + 1;
      nTz++;
    }
  }
  fclose (f);

  nMap = LoadMemmap (MmapPath, Map, N (Map));
  CHECK (nMap > 0, "parse %s", MmapPath);
  for (i = 0; i < nMap; i++) {
    SFB_MEM_REGION  r = { Map[i].Start, Map[i].End - Map[i].Start + 1 };
    if (IsUsable (Map[i].Type, Map[i].Attr)) {
      Use[nUse++] = r;
    } else {
      Firm[nFirm++] = r;
    }
  }
  printf ("parsed: tz=%u firm=%u usable=%u\n", nTz, nFirm, nUse);

  /* conservation: TZ total == usable-in-TZ + released(RESERVED in TZ) +
   * additions + other-described-in-TZ; each term computed independently,
   * so equality proves the steps partition TZ */
  {
    SFB_MEM_REGION  UseS[MAXR], FirmS[MAXR], TzS[MAXR];
    SFB_MEM_PIECE   P[SFB_MEM_MAX_REGIONS + 2];
    UINT32          nU, nF, nTzS, nGaps;
    UINT64          tzTotal = 0, useInTz, released = 0, otherInTz = 0, gapSum = 0;

    memcpy (UseS, Use, nUse * sizeof (Use[0]));
    memcpy (FirmS, Firm, nFirm * sizeof (Firm[0]));
    memcpy (TzS, Tz, nTz * sizeof (Tz[0]));
    nU = SfbMemSortAndCombine (UseS, nUse);
    nF = SfbMemSortAndCombine (FirmS, nFirm);
    nTzS = SfbMemSortAndCombine (TzS, nTz);
    for (i = 0; i < nTzS; i++) {
      tzTotal += TzS[i].Size;
    }
    useInTz = IntersectSum (UseS, nU, TzS, nTzS);

    for (i = 0; i < nMap; i++) {
      SFB_MEM_REGION  one = { Map[i].Start, Map[i].End - Map[i].Start + 1 };
      if (Map[i].Type == 0) {
        INT32  pn = SfbMemSplitVsTz (one.Base, one.Size, TzS, nTzS, P, N (P));
        INT32  k;
        CHECK (pn > 0, "split of RESERVED %llx", Map[i].Start);
        for (k = 0; k < pn; k++) {
          if (P[k].InTz) {
            released += P[k].Size;
          }
        }
      } else if (!IsUsable (Map[i].Type, Map[i].Attr)) {
        otherInTz += IntersectSum (&one, 1, TzS, nTzS);
      }
    }

    nGaps = Merge (TzS, nTzS, FirmS, nF, UseS, nU, Add);
    for (i = 0; i < nGaps; i++) {
      gapSum += Add[i].Size;
    }
    printf ("conservation: TZ=%llu useInTz=%llu released=%llu additions=%llu "
            "otherInTz=%llu\n",
            (unsigned long long)tzTotal, (unsigned long long)useInTz,
            (unsigned long long)released, (unsigned long long)gapSum,
            (unsigned long long)otherInTz);
    CHECK (useInTz + released + gapSum + otherInTz == tzTotal,
           "conservation: %llu != %llu",
           (unsigned long long)(useInTz + released + gapSum + otherInTz),
           (unsigned long long)tzTotal);
  }

  n = Merge (Tz, nTz, Firm, nFirm, Use, nUse, Add);
  CHECK (n == N (want), "real data: n=%u want %zu", n, N (want));
  for (i = 0; i < n && i < N (want); i++) {
    CHECK (Add[i].Base == want[i].base && Add[i].Size == want[i].size,
           "real data range %u: %llx+%llx want %llx+%llx", i,
           (unsigned long long)Add[i].Base, (unsigned long long)Add[i].Size,
           (unsigned long long)want[i].base, (unsigned long long)want[i].size);
  }
}

int
main (int argc, char **argv)
{
  TestSubtractEdges ();
  TestSplitEdges ();
  TestThreeSteps ();
  if (argc >= 3) {
    TestRealData (argv[1], argv[2]);
  } else {
    printf ("(real-data regression skipped: pass memmap.txt tzmap.txt)\n");
  }
  printf ("%s: %lu checks, %lu failures\n",
          mFails ? "FAILED" : "PASSED", mChecks, mFails);
  return mFails ? 1 : 0;
}
