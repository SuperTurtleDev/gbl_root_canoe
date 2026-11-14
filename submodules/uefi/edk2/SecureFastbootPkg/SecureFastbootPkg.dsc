#/** @file
#  SecureFastbootPkg platform description. Builds the standalone
#  SecureFastboot preload wrapper. Self-contained: standard EDK2 base
#  classes plus the SFB secure-boot protocol from QcomModulePkg.dec.
#
#  Build with:
#    build -p SecureFastbootPkg/SecureFastbootPkg.dsc -a AARCH64 -b RELEASE
#
#  Copyright (c) 2026, contributors to the canoe ABL tree.
#  SPDX-License-Identifier: BSD-3-Clause
#**/

################################################################################
#
# Defines Section
#
################################################################################
[Defines]
  PLATFORM_NAME                  = SecureFastbootPkg
  PLATFORM_GUID                  = 0C4F6A92-58D3-4B71-A9E6-3F2D81C05B47
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/SecureFastbootPkg
  SUPPORTED_ARCHITECTURES        = ARM|AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

################################################################################
#
# Library Class mappings - standard EDK2 implementations only.
#
################################################################################
[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[LibraryClasses.ARM]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[LibraryClasses.AARCH64]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

################################################################################
#
# Build Options
#
# Mirror QcomModulePkg.dsc's link flags exactly (see AndroidToolsPkg.dsc):
# -Wl,-Ttext=0x0 so the old CLANG35 GenFw resolves relative relocations, and
# the makefile-provided CLANG_EXTRA_DLINK_FLAGS (--no-relax /
# --apply-dynamic-relocs) so lld does not emit adr sequences this GenFw
# cannot convert.
#
################################################################################
[BuildOptions]
  *_CLANG35_AARCH64_DLINK_FLAGS = -Wl,-Ttext=0x0
  *_CLANG35_AARCH64_DLINK_FLAGS = $(CLANG_EXTRA_DLINK_FLAGS)

################################################################################
#
# Components
#
################################################################################
[Components.common]
  SecureFastbootPkg/Application/SecureFastboot/SecureFastboot.inf
