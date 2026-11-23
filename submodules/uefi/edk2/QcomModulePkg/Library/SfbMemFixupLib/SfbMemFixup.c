/*
 * Umbrella for the two memory fix-ups. See Library/SfbMemFixup.h.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/SfbMemFixup.h>

EFI_STATUS
SfbMemFixupInstall (
  VOID
  )
{
  EFI_STATUS  First = EFI_SUCCESS;
  EFI_STATUS  Status;

  Status = SfbMemFixupInstallMemoryAttribute ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: memory attribute protocol not installed: %r\n",
            Status));
    First = Status;
  }

  Status = SfbMemFixupInstallMemmapMerge ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SFB-MEM: GetMemoryMap merge not installed: %r\n",
            Status));
    if (!EFI_ERROR (First)) {
      First = Status;
    }
  }
  return First;
}
