/*
 * CONFIG\MEMCFG: operator overrides applied to the GetMemoryMap view the
 * BDS hands to every loader. Pure (no UEFI services, no logging), compiles
 * on the host with -DSFB_HOST_TEST.
 *
 * File format, one override per line, '#' starts a comment:
 *
 *   NAME  BASE  SIZE  TYPE  ATTRIBUTE
 *
 *   NAME       free token, logging only (truncated to 31 chars)
 *   BASE/SIZE  hex, optional 0x prefix, both 4 KiB aligned, SIZE > 0
 *   TYPE       UEFI memory type: a number (decimal or 0x hex, 0..14) or
 *              one of RESERVED LOADER_CODE LOADER_DATA BOOT_SERVICES_CODE
 *              BOOT_SERVICES_DATA RUNTIME_SERVICES_CODE RUNTIME_SERVICES_DATA
 *              CONVENTIONAL UNUSABLE ACPI_RECLAIM ACPI_NVS MMIO MMIO_PORT
 *              PAL_CODE PERSISTENT (case-insensitive)
 *   ATTRIBUTE  hex, optional 0x prefix, the EFI_MEMORY_* bit mask
 *
 * All five fields are mandatory; a line with fewer or more tokens is
 * rejected as a whole (no defaults are filled in).
 *
 * Application: each override is cut out of every descriptor it intersects
 * (head and tail of a straddled descriptor survive, a descriptor inside the
 * override disappears, a descriptor containing the override is split in
 * two) and then appended as a descriptor of its own. Lines are applied in
 * file order, so a later line wins over an earlier one where they overlap.
 * The table is a force: no range is checked against TZ, firmware runtime
 * regions or anything else.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_MEM_CFG_H__
#define __SFB_MEM_CFG_H__

#include "SfbMemRegions.h"

#define SFB_MEM_CFG_MAX       32      /* overrides per table */
#define SFB_MEM_NAME_MAX      32      /* incl. NUL */
#define SFB_MEM_TYPE_MAX      14      /* EfiPersistentMemory */

/* one descriptor of the memory map view; mirrors EFI_MEMORY_DESCRIPTOR
 * without the virtual address (always 0 in a boot-time map) */
typedef struct {
  UINT64  Base;
  UINT64  Size;
  UINT32  Type;
  UINT64  Attr;
} SFB_MEM_DESC;

typedef struct {
  CHAR8         Name[SFB_MEM_NAME_MAX];
  SFB_MEM_DESC  Desc;
} SFB_MEM_OVERRIDE;

/*
 * Cut Ov out of List (in place) and append it. Count is updated. At most
 * two entries are added (one split, the override itself).
 *
 * @retval EFI_OUT_OF_RESOURCES  Max too small for the result; List is
 *                               left unmodified.
 */
EFI_STATUS
SfbMemApplyOverride (
  IN OUT SFB_MEM_DESC        *List,
  IN OUT UINT32              *Count,
  IN     UINT32               Max,
  IN     CONST SFB_MEM_DESC  *Ov
  );

/*
 * Parse Text[0..Len) in the MEMCFG format. Invalid lines are skipped and
 * counted in *Rejected; parsing stops when Max entries are filled (the
 * rest count as rejected). Never fails: an empty or garbage file yields
 * *Count == 0.
 */
VOID
SfbMemParseConfig (
  IN  CONST CHAR8        *Text,
  IN  UINTN               Len,
  OUT SFB_MEM_OVERRIDE   *Out,
  IN  UINT32              Max,
  OUT UINT32             *Count,
  OUT UINT32             *Rejected
  );

/* UEFI memory type name for logging ("?" when out of range). */
CONST CHAR8 *
SfbMemTypeName (
  IN UINT32  Type
  );

#endif /* __SFB_MEM_CFG_H__ */
