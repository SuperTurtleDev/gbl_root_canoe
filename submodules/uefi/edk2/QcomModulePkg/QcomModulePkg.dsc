#/*
# * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
# *
# * Redistribution and use in source and binary forms, with or without
# * modification, are permitted provided that the following conditions are
# * met:
# * * Redistributions of source code must retain the above copyright
# *  notice, this list of conditions and the following disclaimer.
# *  * Redistributions in binary form must reproduce the above
# * copyright notice, this list of conditions and the following
# * disclaimer in the documentation and/or other materials provided
# *  with the distribution.
# *   * Neither the name of The Linux Foundation nor the names of its
# * contributors may be used to endorse or promote products derived
# * from this software without specific prior written permission.
# *
# * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
# * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
# * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
# * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
# * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
# * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#*/

#/*
# * Changes from Qualcomm Technologies, Inc. are provided under the following license:
# *
# * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# * SPDX-License-Identifier: BSD-3-Clause-Clear
#*/

#/** @file QcomModulePkg.dsc
# QcomModule package.

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = QcomModulePkg
  PLATFORM_GUID                  = 4476742F-4C2D-BA9D-992A-CB82914F5E58
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = $(ABL_OUT_DIR)
  SUPPORTED_ARCHITECTURES        = ARM|AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = QcomModulePkg/QcomModulePkg.fdf

[LibraryClasses.common]
  BaseStackCheckLib|MdePkg/Library/BaseStackCheckLib/BaseStackCheckLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
  LibUfdt|EmbeddedPkg/Library/LibUfdt/LibUfdt.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  !if $(AUTO_VIRT_ABL)
      BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
      DebugLib|QcomModulePkg/Library/DebugLib/DebugLib.inf
  !else
      BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
      DebugLib|MdeModulePkg/Library/PeiDxeDebugLibReportStatusCode/PeiDxeDebugLibReportStatusCode.inf
      ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  !endif
  DebugPrintErrorLevelLib|MdeModulePkg/Library/DxeDebugPrintErrorLevelLib/DxeDebugPrintErrorLevelLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  LzmaDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf

  # Support stack for the read-only EXT4 driver below, linked into
  # LinuxLoader. FAT volumes are handled by the platform firmware's own FAT
  # driver; EnhancedFatDxe is deliberately not linked into this image.
  UnicodeCollationEngLib|MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishLib.inf
  DiskIoDxeLib|MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoLib.inf
  # Read-only EXT4 driver, linked in alongside the FAT stack so the boot menu
  # can mount ext4 partitions. Started by hand from SfbStartFatStack,
  # exactly like the three above; it binds to Disk I/O handles the connect pass
  # has already built and installs Simple File System on the ext4 partition.
  Ext4DxeLib|Ext4Pkg/Ext4Dxe/Ext4DxeLib.inf
  # Boot menu config store: the FAT container in efisp after the BDS image.
  SfbConfigStoreLib|QcomModulePkg/Library/SfbConfigStoreLib/SfbConfigStoreLib.inf
  # Security policy: SECURITY certs in the container, fastboot challenge
  # authorization and PE image signature verification.
  SfbSecurityLib|QcomModulePkg/Library/SfbSecurityLib/SfbSecurityLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  # Security-boot crypto: mbedtls-backed BaseCryptLib (X.509 parse, RSA/ECDSA
  # verify, SHA-256, PKCS#7). Trimmed mbedtls config keeps BDS under 1 MiB.
  MbedTlsLib|CryptoPkg/Library/MbedTlsLib/MbedTlsLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLibMbedTls/BaseCryptLib.inf
  RngLib|MdePkg/Library/BaseRngLibNull/BaseRngLibNull.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf


[LibraryClasses.ARM]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  CompilerIntrinsicsLib|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[LibraryClasses.AARCH64]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  CompilerIntrinsicsLib|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  # ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

[BuildOptions.common]
  GCC:*_*_*_ARCHCC_FLAGS  = -Wno-shift-negative-value -fstack-protector-all -Wno-varargs -fno-common -Wno-misleading-indentation -Wno-unknown-warning-option
  GCC:*_*_*_DLINK_FLAGS = -Wl,-Ttext=0x0
  GCC:*_*_*_CC_FLAGS = -DZ_SOLO
  # Single quotes, not backslash quotes: the CC_RESP aggregation rewrites
  # backslashes to forward slashes, which delivered /"canoe/" to the compiler
  # - a token sequence, not a string literal - and broke every consumer of
  # this macro on the next full rebuild.
  GCC:*_*_*_CC_FLAGS = -DPRODUCT_NAME='"$(BOARD_BOOTLOADER_PRODUCT_NAME)"'

  GCC:*_*_*_DLINK_FLAGS = $(CLANG_EXTRA_DLINK_FLAGS)
  !ifdef $(TARGET_AUDIO_FRAMEWORK)
  GCC:*_*_*_CC_FLAGS = -DAUDIO_FRAMEWORK='$(TARGET_AUDIO_FRAMEWORK)'
  !endif
  !if $(VERIFIED_BOOT_LE)
      GCC:*_*_*_CC_FLAGS = -DVERIFIED_BOOT_LE
  !endif
  !if $(TEST_ADAPTER)
      GCC:*_*_*_CC_FLAGS = -DTEST_ADAPTER
  !endif
  !if $(ROOT_PARTLABEL_SUPPORT)
      GCC:*_*_*_CC_FLAGS = -DROOT_PARTLABEL_SUPPORT
  !endif
  !if $(SUPPORT_AB_BOOT_LXC)
      GCC:*_*_*_CC_FLAGS = -DSUPPORT_AB_BOOT_LXC
  !endif
  !if $(EARLY_ETH_ENABLED)
      GCC:*_*_*_CC_FLAGS = -DEARLY_ETH_ENABLED
  !endif
  !if $(AUTO_LVGVM_ABL)
      GCC:*_*_*_CC_FLAGS = -DAUTO_LVGVM_ABL
  !endif
  !if $(HIBERNATION_SUPPORT_NO_AES)
      GCC:*_*_*_CC_FLAGS = -DHIBERNATION_SUPPORT_NO_AES
      GCC:*_*_*_PP_FLAGS = -DHIBERNATION_SUPPORT_NO_AES
  !endif
  !if $(HIBERNATION_SUPPORT_AES)
      GCC:*_*_*_CC_FLAGS = -DHIBERNATION_SUPPORT_AES
      GCC:*_*_*_PP_FLAGS = -DHIBERNATION_SUPPORT_AES
  !endif
  !if $(HIBERNATION_TZ_ENCRYPTION)
      GCC:*_*_*_CC_FLAGS = -DHIBERNATION_TZ_ENCRYPTION
      GCC:*_*_*_PP_FLAGS = -DHIBERNATION_TZ_ENCRYPTION
  !endif
  !if $(HIBERNATION_SWAP_PARTITION_NAME)
      GCC:*_*_*_CC_FLAGS = -DHIBERNATION_SWAP_PARTITION_NAME='L"$(HIBERNATION_SWAP_PARTITION_NAME)"'
      GCC:*_*_*_PP_FLAGS = -DHIBERNATION_SWAP_PARTITION_NAME='L"$(HIBERNATION_SWAP_PARTITION_NAME)"'
  !endif
  !if $(DISABLE_DTBO_PARTITION)
      GCC:*_*_*_CC_FLAGS = -DDISABLE_DTBO_PARTITION
      GCC:*_*_*_PP_FLAGS = -DDISABLE_DTBO_PARTITION
  !endif
  !if $(APPEND_RAM_PARTITIONS_TO_MEM_NODE)
      GCC:*_*_*_CC_FLAGS = -DAPPEND_RAM_PARTITIONS_TO_MEM_NODE
      GCC:*_*_*_PP_FLAGS = -DAPPEND_RAM_PARTITIONS_TO_MEM_NODE
  !endif
  !if $(DDR_SUPPORTS_SCT_CONFIG) == 1
      GCC:*_*_*_CC_FLAGS = -DDDR_SUPPORTS_SCT_CONFIG
  !endif
  !if $(AB_RETRYCOUNT_DISABLE)
      GCC:*_*_*_CC_FLAGS = -DAB_RETRYCOUNT_DISABLE
  !endif
  !if $(TARGET_BOARD_TYPE_AUTO)
      GCC:*_*_*_CC_FLAGS = -DTARGET_BOARD_TYPE_AUTO
  !endif
  !if $(EARLY_ETH_AS_DLKM) == 1
      GCC:*_*_*_CC_FLAGS = -DEARLY_ETH_AS_DLKM
  !endif
  !if $(BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL) == 1
      GCC:*_*_*_CC_FLAGS = -DBOOTIMAGE_LOAD_VERIFY_IN_PARALLEL
  !endif
  !if $(VERITY_LE)
      GCC:*_*_*_CC_FLAGS = -DVERITY_LE
  !endif
  !if $(USER_BUILD_VARIANT) == 0
      GCC:*_*_*_CC_FLAGS = -DENABLE_UPDATE_PARTITIONS_CMDS -DENABLE_BOOT_CMD -DENABLE_DEVICE_CRITICAL_LOCK_UNLOCK_CMDS
  !else
      GCC:*_*_*_CC_FLAGS = -DUSER_BUILD_VARIANT
  !endif
  !if $(ENABLE_LE_VARIANT) == 1
      GCC:*_*_*_CC_FLAGS = -DENABLE_LE_VARIANT
  !endif
  !if $(AUTO_PATCH_ABL) == 1
      GCC:*_*_*_CC_FLAGS = -DAUTO_PATCH_ABL
  !endif
    !if $(DISABLE_PRINT) == 1
      GCC:*_*_*_CC_FLAGS = -DDISABLE_PRINT
  !endif
  !if $(WEAR_OS)
      GCC:*_*_*_CC_FLAGS = -DWEAR_OS
  !endif
  !if $(ENABLE_LV_ATOMIC_AB) == 1
      GCC:*_*_*_CC_FLAGS = -DENABLE_LV_ATOMIC_AB
  !endif
  !if $(DISABLE_PARALLEL_DOWNLOAD_FLASH) == 1
      GCC:*_*_*_CC_FLAGS = -DDISABLE_PARALLEL_DOWNLOAD_FLASH
  !endif
  !if $(BUILD_USES_RECOVERY_AS_BOOT)
      GCC:*_*_*_CC_FLAGS = -DBUILD_USES_RECOVERY_AS_BOOT
  !endif
  !ifdef $(INIT_BIN)
      GCC:*_*_*_CC_FLAGS = -DINIT_BIN='"$(INIT_BIN)"'
  !endif
  !if $(NAND_SQUASHFS_SUPPORT)
      GCC:*_*_*_CC_FLAGS = -DNAND_SQUASHFS_SUPPORT
  !endif
  !if $(VERIFIED_BOOT_ENABLED)
      GCC:*_*_*_CC_FLAGS = -DVERIFIED_BOOT_ENABLED
  !endif
  !if $(USE_DUMMY_BCC)
      GCC:*_*_*_CC_FLAGS = -DUSE_DUMMY_BCC
  !endif
  !if $(BASE_ADDRESS)
      GCC:*_*_*_CC_FLAGS = -DBASE_ADDRESS=$(BASE_ADDRESS)
  !endif
  !if $(TARGET_LINUX_BOOT_CPU_ID)
      GCC:*_*_*_CC_FLAGS = -DTARGET_LINUX_BOOT_CPU_ID=$(TARGET_LINUX_BOOT_CPU_ID)
  !endif
  !if $(ENABLE_EARLY_SERVICES)
      GCC:*_*_*_CC_FLAGS = -DENABLE_EARLY_SERVICES=$(ENABLE_EARLY_SERVICES)
  !endif
  !if $(KERNEL_LOAD_ADDRESS)
      GCC:*_*_*_CC_FLAGS = -DKERNEL_LOAD_ADDRESS=$(KERNEL_LOAD_ADDRESS)
  !endif
  !if $(KERNEL_SIZE_RESERVED)
      GCC:*_*_*_CC_FLAGS = -DKERNEL_SIZE_RESERVED=$(KERNEL_SIZE_RESERVED)
  !endif
  !if $(DISABLE_KERNEL_PROTOCOL)
      GCC:*_*_*_CC_FLAGS = -DDISABLE_KERNEL_PROTOCOL=$(DISABLE_KERNEL_PROTOCOL)
  !endif
  !if $(NAND_UBI_VOLUME_FLASHING_ENABLED)
      GCC:*_*_*_CC_FLAGS = -DNAND_UBI_VOLUME_FLASHING_ENABLED
  !endif
  !if $(TARGET_SUPPORTS_EARLY_USB_INIT)
      GCC:*_*_*_CC_FLAGS = -DTARGET_SUPPORTS_EARLY_USB_INIT
  !endif
  !if $(TARGET_SUPPORTS_EARLY_USB_INIT)
      GCC:*_*_*_PP_FLAGS = -DTARGET_SUPPORTS_EARLY_USB_INIT
  !endif
  !if $(REMOVE_CARVEOUT_REGION) == 1
      GCC:*_*_*_CC_FLAGS = -DREMOVE_CARVEOUT_REGION
  !endif
  !if $(QSPA_BOOTCONFIG_ENABLE) ==1
      GCC:*_*_*_CC_FLAGS = -DQSPA_BOOTCONFIG_ENABLE
  !endif
  !if $(AUTO_VIRT_ABL)
      GCC:*_*_*_CC_FLAGS = -DAUTO_VIRT_ABL
  !endif


[PcdsFixedAtBuild.common]

# DEBUG_ASSERT_ENABLED       0x01
# DEBUG_PRINT_ENABLED        0x02
# DEBUG_CODE_ENABLED         0x04
# CLEAR_MEMORY_ENABLED       0x08
# ASSERT_BREAKPOINT_ENABLED  0x10
# ASSERT_DEADLOOP_ENABLED    0x20

  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x06

# Consumed by the embedded FAT stack. Pinned here rather than left to the
# package defaults because the platform resolves PcdLib to the null instance,
# so every PCD these drivers touch has to resolve at build time.
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultLang|"eng"
  gEfiMdePkgTokenSpaceGuid.PcdUefiVariableDefaultPlatformLang|"en-US"
  gEfiMdeModulePkgTokenSpaceGuid.PcdDiskIoDataBufferBlockNum|64

[PcdsFeatureFlag.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdUnicodeCollationSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdUnicodeCollation2Support|TRUE

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform
#
################################################################################
[Components.common]

	QcomModulePkg/Application/LinuxLoader/LinuxLoader.inf {
		<LibraryClasses>
			DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
			UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
			UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
			CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
			ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
			BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
			DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
			HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
			PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
			DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

			FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
			LibUfdt|EmbeddedPkg/Library/LibUfdt/LibUfdt.inf
			ArmSmcLib|ArmPkg/Library/ArmSmcLib/ArmSmcLib.inf
			StackCanary|QcomModulePkg/Library/StackCanary/StackCanary.inf
			FastbootLib|QcomModulePkg/Library/FastbootLib/FastbootLib.inf
	}
