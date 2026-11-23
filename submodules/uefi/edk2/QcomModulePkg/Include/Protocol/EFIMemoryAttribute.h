/*
 * EFI_MEMORY_ATTRIBUTE_PROTOCOL (UEFI 2.10, section 7.2.x).
 *
 * The vendor MdePkg in this tree predates the protocol, so the definition
 * lives here. GUID, layout and semantics are the standard ones: a consumer
 * written against MdePkg/Include/Protocol/MemoryAttribute.h (for example the
 * Linux EFI stub) binds to it unchanged.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __EFI_MEMORY_ATTRIBUTE_PROTOCOL_H__
#define __EFI_MEMORY_ATTRIBUTE_PROTOCOL_H__

#include <Uefi.h>

#define EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID \
  { \
    0xf4560cf6, 0x40ec, 0x4b4a, \
    { 0xa1, 0x92, 0xbf, 0x1d, 0x57, 0xd0, 0xb1, 0x89 } \
  }

typedef struct _EFI_MEMORY_ATTRIBUTE_PROTOCOL EFI_MEMORY_ATTRIBUTE_PROTOCOL;

/*
 * Report the access attributes (EFI_MEMORY_RP / RO / XP) that apply to the
 * whole of [BaseAddress, BaseAddress + Length). Both must be page aligned.
 *
 * @retval EFI_SUCCESS            *Attributes holds the common attributes.
 * @retval EFI_INVALID_PARAMETER  Length is 0, misaligned, or Attributes NULL.
 * @retval EFI_NO_MAPPING         The range is not uniformly mapped.
 * @retval EFI_UNSUPPORTED        The range is not system memory.
 */
typedef
EFI_STATUS
(EFIAPI *EFI_GET_MEMORY_ATTRIBUTES) (
  IN  EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN  EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN  UINT64                          Length,
  OUT UINT64                         *Attributes
  );

/*
 * Set (OR in) the given access attributes on the range. Attributes must be
 * non-zero and contain only EFI_MEMORY_RP / RO / XP.
 *
 * @retval EFI_SUCCESS            Applied.
 * @retval EFI_INVALID_PARAMETER  Bad range, bad or empty attribute set.
 * @retval EFI_UNSUPPORTED        Not system memory / not supported.
 * @retval EFI_ACCESS_DENIED      The range cannot be modified.
 * @retval EFI_OUT_OF_RESOURCES   Page table split failed.
 */
typedef
EFI_STATUS
(EFIAPI *EFI_SET_MEMORY_ATTRIBUTES) (
  IN EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN UINT64                          Length,
  IN UINT64                          Attributes
  );

/* Clear (AND out) the given access attributes; same contract as Set. */
typedef
EFI_STATUS
(EFIAPI *EFI_CLEAR_MEMORY_ATTRIBUTES) (
  IN EFI_MEMORY_ATTRIBUTE_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS            BaseAddress,
  IN UINT64                          Length,
  IN UINT64                          Attributes
  );

struct _EFI_MEMORY_ATTRIBUTE_PROTOCOL {
  EFI_GET_MEMORY_ATTRIBUTES    GetMemoryAttributes;
  EFI_SET_MEMORY_ATTRIBUTES    SetMemoryAttributes;
  EFI_CLEAR_MEMORY_ATTRIBUTES  ClearMemoryAttributes;
};

extern EFI_GUID gEfiMemoryAttributeProtocolGuid;

#endif /* __EFI_MEMORY_ATTRIBUTE_PROTOCOL_H__ */
