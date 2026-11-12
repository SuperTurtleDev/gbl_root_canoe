/*
 * Shared internals of SfbSecurityLib: the certificate pools.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_SECURITY_INTERNAL_H__
#define __SFB_SECURITY_INTERNAL_H__

#include <Uefi.h>
#include <Library/SfbSecurity.h>

typedef struct {
  UINT8  *Der[SFB_SECURITY_MAX_CERTS];
  UINTN  DerSize[SFB_SECURITY_MAX_CERTS];
  UINTN  Count;
} SFB_CERT_POOL;

CONST SFB_CERT_POOL *SfbCertsAuthPool (VOID);

CONST SFB_CERT_POOL *SfbCertsBootPool (VOID);

/* Free both pools (SfbSecurityReset calls this). */
VOID SfbCertsReset (VOID);

/* Test seam: install a known nonce as the pending challenge. */
VOID SfbSecurityTestSetChallenge (IN CONST UINT8 *Nonce);

#endif /* __SFB_SECURITY_INTERNAL_H__ */
