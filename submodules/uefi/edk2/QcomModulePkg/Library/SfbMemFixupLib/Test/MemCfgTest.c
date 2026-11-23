/*
 * Host-side tests for the CONFIG\MEMCFG parser and override application
 * (SfbMemCfg.c). Build & run: ./test.sh (runs both test binaries).
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "../SfbMemCfg.h"

static unsigned long mChecks, mFails;

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

#define N(a) (sizeof (a) / sizeof ((a)[0]))
#define CONV 7
#define RSVD 0

static SFB_MEM_DESC
D (UINT64 b, UINT64 s, UINT32 t, UINT64 a)
{
  SFB_MEM_DESC d = { b, s, t, a };
  return d;
}

static int
Same (const SFB_MEM_DESC *x, UINT64 b, UINT64 s, UINT32 t, UINT64 a)
{
  return x->Base == b && x->Size == s && x->Type == t && x->Attr == a;
}

static void
Dump (const char *tag, const SFB_MEM_DESC *l, UINT32 n)
{
  UINT32 i;
  printf ("  %s:", tag);
  for (i = 0; i < n; i++) {
    printf (" [%llx+%llx t%u a%llx]", (unsigned long long)l[i].Base,
            (unsigned long long)l[i].Size, l[i].Type, (unsigned long long)l[i].Attr);
  }
  printf ("\n");
}

/* ---- override application ------------------------------------------------ */

static void
TestApplyMiddleSplit (void)
{
  SFB_MEM_DESC l[8] = { D (0x1000, 0x8000, RSVD, 0x1) };
  UINT32 n = 1;
  SFB_MEM_DESC ov = D (0x3000, 0x2000, CONV, 0xF);

  CHECK (SfbMemApplyOverride (l, &n, N (l), &ov) == EFI_SUCCESS, "status");
  CHECK (n == 3, "middle split n=%u want 3", n);
  if (n == 3) {
    CHECK (Same (&l[0], 0x1000, 0x2000, RSVD, 0x1), "head keeps type/attr");
    CHECK (Same (&l[1], 0x5000, 0x4000, RSVD, 0x1), "tail keeps type/attr");
    CHECK (Same (&l[2], 0x3000, 0x2000, CONV, 0xF), "override appended last");
  } else {
    Dump ("got", l, n);
  }
}

static void
TestApplyWholeCover (void)
{
  SFB_MEM_DESC l[8] = { D (0x1000, 0x1000, RSVD, 0x1), D (0x2000, 0x1000, CONV, 0xF),
                        D (0x3000, 0x1000, RSVD, 0x1), D (0x9000, 0x1000, CONV, 0xF) };
  UINT32 n = 4;
  SFB_MEM_DESC ov = D (0x1000, 0x3000, CONV, 0xF);

  CHECK (SfbMemApplyOverride (l, &n, N (l), &ov) == EFI_SUCCESS, "status");
  CHECK (n == 2, "three descriptors swallowed: n=%u want 2", n);
  if (n == 2) {
    CHECK (Same (&l[0], 0x9000, 0x1000, CONV, 0xF), "untouched survivor first");
    CHECK (Same (&l[1], 0x1000, 0x3000, CONV, 0xF), "override last");
  } else {
    Dump ("got", l, n);
  }
}

static void
TestApplyHeadTailAndSpan (void)
{
  /* [1000,3000) [3000,5000) [5000,7000); override [2000,6000) clips the
   * tail of the first, swallows the second, clips the head of the third */
  SFB_MEM_DESC l[8] = { D (0x1000, 0x2000, RSVD, 0x1), D (0x3000, 0x2000, RSVD, 0x2),
                        D (0x5000, 0x2000, RSVD, 0x3) };
  UINT32 n = 3;
  SFB_MEM_DESC ov = D (0x2000, 0x4000, CONV, 0xF);

  CHECK (SfbMemApplyOverride (l, &n, N (l), &ov) == EFI_SUCCESS, "status");
  CHECK (n == 3, "span: n=%u want 3", n);
  if (n == 3) {
    CHECK (Same (&l[0], 0x1000, 0x1000, RSVD, 0x1), "tail clipped");
    CHECK (Same (&l[1], 0x6000, 0x1000, RSVD, 0x3), "head clipped");
    CHECK (Same (&l[2], 0x2000, 0x4000, CONV, 0xF), "override last");
  } else {
    Dump ("got", l, n);
  }
}

static void
TestApplyTouchingAndDisjoint (void)
{
  SFB_MEM_DESC l[8] = { D (0x1000, 0x1000, RSVD, 0x1), D (0x3000, 0x1000, RSVD, 0x1) };
  UINT32 n = 2;
  SFB_MEM_DESC ov = D (0x2000, 0x1000, CONV, 0xF);   /* touches both, overlaps neither */

  CHECK (SfbMemApplyOverride (l, &n, N (l), &ov) == EFI_SUCCESS, "status");
  CHECK (n == 3, "touching: n=%u want 3", n);
  if (n == 3) {
    CHECK (Same (&l[0], 0x1000, 0x1000, RSVD, 0x1), "left untouched");
    CHECK (Same (&l[1], 0x3000, 0x1000, RSVD, 0x1), "right untouched");
  }
}

static void
TestApplyOrderLaterWins (void)
{
  SFB_MEM_DESC l[8] = { D (0x1000, 0x8000, RSVD, 0x1) };
  UINT32 n = 1;
  SFB_MEM_DESC a = D (0x2000, 0x4000, CONV, 0xF);
  SFB_MEM_DESC b = D (0x4000, 0x1000, RSVD, 0x8);   /* inside a, applied after */

  CHECK (SfbMemApplyOverride (l, &n, N (l), &a) == EFI_SUCCESS, "a");
  CHECK (SfbMemApplyOverride (l, &n, N (l), &b) == EFI_SUCCESS, "b");
  /* expect: [1000,2000) R, [6000,9000) R, [2000,4000) C, [5000,6000) C, [4000,5000) R8 */
  CHECK (n == 5, "later wins: n=%u want 5", n);
  if (n == 5) {
    CHECK (Same (&l[2], 0x2000, 0x2000, CONV, 0xF), "a head survives");
    CHECK (Same (&l[3], 0x5000, 0x1000, CONV, 0xF), "a tail survives");
    CHECK (Same (&l[4], 0x4000, 0x1000, RSVD, 0x8), "b last, wins inside");
  } else {
    Dump ("got", l, n);
  }
}

static void
TestApplyCapacity (void)
{
  SFB_MEM_DESC l[2] = { D (0x1000, 0x8000, RSVD, 0x1) };
  UINT32 n = 1;
  SFB_MEM_DESC ov = D (0x3000, 0x2000, CONV, 0xF);    /* needs 3 slots */

  CHECK (SfbMemApplyOverride (l, &n, N (l), &ov) == EFI_OUT_OF_RESOURCES, "capacity");
  CHECK (n == 1 && Same (&l[0], 0x1000, 0x8000, RSVD, 0x1), "list untouched on failure");
}

/* ---- parser --------------------------------------------------------------- */

static void
TestParseGood (void)
{
  static const char txt[] =
    "# comment line\n"
    "\n"
    "mpss          0x8AA00000  0xEB00000  CONVENTIONAL  0xF\n"
    "  q6_dtb  99500000 80000 7 F   # trailing comment\r\n"
    "numtype 0xD7800000 0x1C0000 0x0 0x1\n"
    "lower  0xff800000 0x800000 reserved 0x8\n"
    "a_very_long_name_that_exceeds_thirty_one_characters 0x1000 0x1000 ACPI_NVS 0x0";
  SFB_MEM_OVERRIDE o[8];
  UINT32 n = 0, rej = 0;

  SfbMemParseConfig (txt, sizeof (txt) - 1, o, N (o), &n, &rej);
  CHECK (n == 5, "good: n=%u want 5", n);
  CHECK (rej == 0, "good: rejected=%u want 0", rej);
  if (n == 5) {
    CHECK (strcmp (o[0].Name, "mpss") == 0 && Same (&o[0].Desc, 0x8AA00000, 0xEB00000, CONV, 0xF), "line 1");
    CHECK (strcmp (o[1].Name, "q6_dtb") == 0 && Same (&o[1].Desc, 0x99500000, 0x80000, CONV, 0xF), "no 0x, numeric type, CRLF, comment");
    CHECK (Same (&o[2].Desc, 0xD7800000, 0x1C0000, RSVD, 0x1), "hex type");
    CHECK (Same (&o[3].Desc, 0xff800000, 0x800000, RSVD, 0x8), "lowercase type name");
    CHECK (o[4].Desc.Type == 10 && strlen (o[4].Name) == SFB_MEM_NAME_MAX - 1, "name truncated, ACPI_NVS=10");
  }
}

static void
TestParseBad (void)
{
  static const char txt[] =
    "four_fields 0x1000 0x1000 CONVENTIONAL\n"         /* attribute missing */
    "six_fields 0x1000 0x1000 CONVENTIONAL 0xF extra\n"
    "unaligned 0x1001 0x1000 CONVENTIONAL 0xF\n"
    "zero_size 0x1000 0x0 CONVENTIONAL 0xF\n"
    "bad_type 0x1000 0x1000 FOO 0xF\n"
    "type_oor 0x1000 0x1000 15 0xF\n"
    "bad_hex 0x1000 0x1G00 CONVENTIONAL 0xF\n"
    "wrap 0xFFFFFFFFFFFFF000 0x2000 CONVENTIONAL 0xF\n"
    "ok 0x2000 0x1000 CONVENTIONAL 0xF\n";
  SFB_MEM_OVERRIDE o[8];
  UINT32 n = 0, rej = 0;

  SfbMemParseConfig (txt, sizeof (txt) - 1, o, N (o), &n, &rej);
  CHECK (n == 1, "bad: n=%u want 1", n);
  CHECK (rej == 8, "bad: rejected=%u want 8", rej);
  CHECK (n == 1 && strcmp (o[0].Name, "ok") == 0, "the valid line survives");
}

static void
TestParseEmptyAndLimits (void)
{
  SFB_MEM_OVERRIDE o[2];
  UINT32 n = 7, rej = 7;
  static const char three[] = "a 0x1000 0x1000 7 0xF\nb 0x2000 0x1000 7 0xF\nc 0x3000 0x1000 7 0xF\n";
  static const char nul[] = "a 0x1000 0x1000 7 0xF\n\0b 0x2000 0x1000 7 0xF\n";

  SfbMemParseConfig (NULL, 0, o, N (o), &n, &rej);
  CHECK (n == 0 && rej == 0, "NULL text -> empty");
  SfbMemParseConfig ("", 0, o, N (o), &n, &rej);
  CHECK (n == 0 && rej == 0, "empty text -> empty");
  SfbMemParseConfig ("# only\n\n   \n", 11, o, N (o), &n, &rej);
  CHECK (n == 0 && rej == 0, "comments only -> empty, not rejected");
  SfbMemParseConfig (three, sizeof (three) - 1, o, N (o), &n, &rej);
  CHECK (n == 2 && rej == 1, "table full: n=%u rej=%u", n, rej);
  SfbMemParseConfig (nul, sizeof (nul) - 1, o, N (o), &n, &rej);
  CHECK (n == 1 && rej == 0, "embedded NUL ends the text: n=%u rej=%u", n, rej);
}

/* ---- the shipped example against the real firmware map ------------------ */

/* find by base; the view's ranges are disjoint so base is a key */
static const SFB_MEM_DESC *
Find (const SFB_MEM_DESC *l, UINT32 n, UINT64 b)
{
  UINT32 i;
  for (i = 0; i < n; i++) {
    if (l[i].Base == b) {
      return &l[i];
    }
  }
  return NULL;
}

static void
TestExampleOnDeviceMap (void)
{
  /* the firmware descriptors the example overrides live in: a TZ-merge
   * addition, the monolithic UC-only PIL_Reserved, and the WB RESERVED
   * range above it (memmap.txt entries 0102/0103) */
  SFB_MEM_DESC l[24] = { D (0x82600000ULL, 0x08400000ULL, CONV, 0xF),
                         D (0x8AA00000ULL, 0x1D300000ULL, RSVD, 0x1),
                         D (0xA7D00000ULL, 0x02C80000ULL, RSVD, 0x100F) };
  UINT32 n = 3, i;
  static const char cfg[] =
    "mpss          0x8AA00000  0xEB00000  CONVENTIONAL  0xF\n"
    "q6_mpss_dtb   0x99500000  0x80000    CONVENTIONAL  0xF\n"
    "ipa_fw        0x99580000  0x10000    CONVENTIONAL  0xF\n"
    "ipa_gsi       0x99590000  0xA000     CONVENTIONAL  0xF\n"
    "pil_gap_ucode_cam    0x9959C000  0x64000    CONVENTIONAL  0xF\n"
    "pil_gap_cdsp_spu     0x9CE80000  0x2E00000  CONVENTIONAL  0xF\n"
    "pil_gap_spss_dcp     0x9FFE0000  0x70000    CONVENTIONAL  0xF\n"
    "pil_gap_soccp_adsp   0xA0910000  0xA70000   CONVENTIONAL  0xF\n"
    "pil_gap_adsp_tail    0xA6400000  0x1900000  CONVENTIONAL  0xF";
  SFB_MEM_OVERRIDE o[12];
  UINT32 no = 0, rej = 0;
  UINT64 released = 0, rsvdLeft = 0;
  /* modem block + the five PIL gaps */
  const UINT64 want = 0xEB00000ULL + 0x80000 + 0x10000 + 0xA000 +
                      0x64000 + 0x2E00000 + 0x70000 + 0xA70000 + 0x1900000;
  /* what must survive inside PIL_Reserved as UC RESERVED */
  static const struct { UINT64 b, s; const char *what; } keep[] = {
    { 0x9959A000ULL, 0x002000ULL, "gpu_microcode" },
    { 0x99600000ULL, 0x3880000ULL, "camera..q6_cdsp_dtb" },
    { 0x9FC80000ULL, 0x360000ULL, "spu shared + spss" },
    { 0xA0050000ULL, 0x8C0000ULL, "dcp..soccp_dtb" },
    { 0xA1380000ULL, 0x5080000ULL, "q6_adsp_dtb + adspslpi" },
  };
  /* the released ranges themselves */
  static const struct { UINT64 b, s; } free_[] = {
    { 0x8AA00000ULL, 0xEB00000ULL },
    { 0x99500000ULL, 0x80000ULL },
    { 0x99580000ULL, 0x10000ULL },
    { 0x99590000ULL, 0xA000ULL },
    { 0x9959C000ULL, 0x64000ULL },
    { 0x9CE80000ULL, 0x2E00000ULL },
    { 0x9FFE0000ULL, 0x70000ULL },
    { 0xA0910000ULL, 0xA70000ULL },
    { 0xA6400000ULL, 0x1900000ULL },
  };

  SfbMemParseConfig (cfg, sizeof (cfg) - 1, o, N (o), &no, &rej);
  CHECK (no == 9 && rej == 0, "example parses: %u/%u", no, rej);
  for (i = 0; i < no; i++) {
    CHECK (SfbMemApplyOverride (l, &n, N (l), &o[i].Desc) == EFI_SUCCESS, "apply %u", i);
  }
  for (i = 0; i < n; i++) {
    if (l[i].Type == CONV && l[i].Base >= 0x8AA00000ULL && l[i].Base < 0xA7D00000ULL) {
      released += l[i].Size;
    }
    if (l[i].Type == RSVD && l[i].Attr == 0x1) {
      rsvdLeft += l[i].Size;
    }
  }
  CHECK (released == want, "released %llu MiB want %llu MiB",
         (unsigned long long)(released >> 20), (unsigned long long)(want >> 20));
  CHECK (released >= 0x12C00000ULL, "300 MiB target: only %llu MiB",
         (unsigned long long)(released >> 20));
  CHECK (released + rsvdLeft == 0x1D300000ULL, "PIL conservation: %llu + %llu",
         (unsigned long long)released, (unsigned long long)rsvdLeft);
  for (i = 0; i < N (keep); i++) {
    const SFB_MEM_DESC  *d = Find (l, n, keep[i].b);
    CHECK (d != NULL && d->Size == keep[i].s && d->Type == RSVD && d->Attr == 0x1,
           "kept %s: %llx+%llx t%u a%llx", keep[i].what,
           d ? (unsigned long long)d->Base : 0, d ? (unsigned long long)d->Size : 0,
           d ? d->Type : 0, d ? (unsigned long long)d->Attr : 0);
  }
  for (i = 0; i < N (free_); i++) {
    const SFB_MEM_DESC  *d = Find (l, n, free_[i].b);
    CHECK (d != NULL && d->Size == free_[i].s && d->Type == CONV && d->Attr == 0xF,
           "freed 0x%llx: got %llx+%llx t%u a%llx",
           (unsigned long long)free_[i].b,
           d ? (unsigned long long)d->Base : 0, d ? (unsigned long long)d->Size : 0,
           d ? d->Type : 0, d ? (unsigned long long)d->Attr : 0);
  }
  /* the WB RESERVED above PIL survives untouched */
  {
    const SFB_MEM_DESC  *d = Find (l, n, 0xA7D00000ULL);
    CHECK (d != NULL && d->Size == 0x2C80000ULL && d->Type == RSVD && d->Attr == 0x100F,
           "above-PIL WB RESERVED intact");
  }
}

int
main (void)
{
  TestApplyMiddleSplit ();
  TestApplyWholeCover ();
  TestApplyHeadTailAndSpan ();
  TestApplyTouchingAndDisjoint ();
  TestApplyOrderLaterWins ();
  TestApplyCapacity ();
  TestParseGood ();
  TestParseBad ();
  TestParseEmptyAndLimits ();
  TestExampleOnDeviceMap ();
  printf ("%s: %lu checks, %lu failures\n", mFails ? "FAILED" : "PASSED", mChecks, mFails);
  return mFails ? 1 : 0;
}
