UEFI_TOP_DIR := .

export BUILD_NATIVE_AARCH64 := ${BUILD_NATIVE_AARCH64}
export SIGN_ABL_IMAGE := ${SIGN_ABL_IMAGE}
# Standalone boot configuration for native building
ifeq ($(BUILD_NATIVE_AARCH64),true)
	export VERIFIED_BOOT := 0
	export VERIFIED_BOOT_LE := false
	export ENABLE_LE_VARIANT := false
	export VERITY_LE := false
	export DEFAULT_UNLOCK := true
	export BUILD_SYSTEM_ROOT_IMAGE := false
	export AB_RETRYCOUNT_DISABLE := false
	export DISABLE_PARALLEL_DOWNLOAD_FLASH := false
	export DYNAMIC_PARTITION_SUPPORT := 1
	export USER_BUILD_VARIANT := false
	export BOOTLOADER_OUT := $(pwd)/obj/ABL_OBJ
	export CLANG_BIN := /usr/bin/
	export CLANG_PREFIX := /usr/bin/aarch64-redhat-linux- 
	export TARGET_ARCHITECTURE := arm64
	export DISABLE_KERNEL_PROTOCOL := false
endif

ifndef $(BOOTLOADER_OUT)
	BOOTLOADER_OUT := $(shell pwd)
endif
export $(BOOTLOADER_OUT)

BUILDDIR=$(shell pwd)
export WRAPPER := $(PREBUILT_PYTHON_PATH) $(BUILDDIR)/clang-wrapper.py
export MAKEPATH := $(MAKEPATH)

export CLANG35_BIN := $(CLANG_BIN)
ifeq ($(BUILD_NATIVE_AARCH64),true)
	export FUSE_LD := $(CLANG35_BIN)/ld
	export EXTRA_GCC_ARG := -Wno-error=unused-command-line-argument
else
	export FUSE_LD := $(CLANG35_BIN)/ld.lld
	export EXTRA_GCC_ARG :=
endif
export CLANG35_GCC_TOOLCHAIN := $(CLANG35_GCC_TOOLCHAIN)
export $(BOARD_BOOTLOADER_PRODUCT_NAME)

ifeq ($(TARGET_ARCHITECTURE),arm)
export ARCHITECTURE := ARM
export CLANG35_ARM_PREFIX := $(CLANG_PREFIX)
else
export ARCHITECTURE := AARCH64
export CLANG35_AARCH64_PREFIX := $(CLANG_PREFIX)
endif

export BUILD_REPORT_DIR := $(BOOTLOADER_OUT)/build_report
ABL_OUT := $(BOOTLOADER_OUT)/Build

WORKSPACE=$(BUILDDIR)
TARGET_TOOLS := CLANG35
TARGET := DEBUG
BUILD_ROOT := $(ABL_OUT)/$(TARGET)_$(TARGET_TOOLS)
EDK_TOOLS := $(BUILDDIR)/BaseTools
EDK_TOOLS_BIN := $(EDK_TOOLS)/Source/C/bin
ABL_FV_IMG := $(BUILD_ROOT)/FV/abl.fv
ABL_FV_ELF := $(BOOTLOADER_OUT)/../../unsigned_abl.elf
ifeq ($(AUTO_VIRT_ABL), 1)
  ABL_FV_EFI := $(BOOTLOADER_OUT)/../../LinuxLoader.efi
endif
SHELL:=/bin/bash

EDK_TOOLS_SRC_FILE := $(shell find $(EDK_TOOLS) -name "*" -type f)
EDK_TOOLS_PATH_MARK_FILE := $(ABL_OUT)/BaseTools_Mark
EDK_TOOLS_GENERATE_CLEAN := $(ABL_OUT)/BaseTools_Clean
export TARGET_EDK_TOOLS_BIN := $(ABL_OUT)/Source/C/bin

define edk_tools_generate
  mkdir -p $(ABL_OUT)/Scripts
  cp -rf $(EDK_TOOLS)/Scripts/GccBase.lds $(ABL_OUT)/Scripts

  (. ./edksetup.sh BaseTools && \
  $(MAKEPATH)make -C $(EDK_TOOLS) $(PREBUILT_HOST_TOOLS) -j1)

  mkdir -p $(TARGET_EDK_TOOLS_BIN)
  cp -rf $(EDK_TOOLS_BIN)/* $(TARGET_EDK_TOOLS_BIN)
  touch $(EDK_TOOLS_PATH_MARK_FILE)
endef

# Secure signing implementation for standalone native building
ifeq ($(SIGN_ABL_IMAGE),true)
	SECTOOL_DIR ?= /usr/bin/sectools
	SECTOOL_PROJ ?= sdm855
	SIGN_ID ?= abl
	SOC_HW_VER ?= 0x60030000
	SOC_VERS ?= 0x6003
	SECIMAGE_XML ?= secimagev3
	ABL_SIGNED ?= $(BUILDDIR)/signed_abl
	USES_SEC_POLICY_MULTIPLE_DEFAULT_SIGN ?= 1
	USES_SEC_POLICY_DEFAULT_SUBFOLDER_SIGN ?= ""
	USES_SEC_POLICY_INTEGRITY_CHECK ?= 1
endif

# Check if build was success, and try to sign the image, if requested
define validate-cum-sign
	if [ "$(BUILD_NATIVE_AARCH64)" = "true" ]; then \
		if [ ! -f $(ABL_FV_ELF) ]; then \
			echo "Failed to generate unsigned ABL image, exiting !!"; \
			exit 1; \
		fi ;\
		echo -------------------------------------------------------------- ;\
		echo "Unsigned ABL Image: $(ABL_FV_ELF)"; \
		echo -------------------------------------------------------------- ;\
		if [ "$(SIGN_ABL_IMAGE)" = "true" ]; then \
			echo "Signing requested, will attempt to sign the generated ABL image" ;\
			if [ ! -d $(ABL_SIGNED) ]; then \
				mkdir -p $(ABL_SIGNED); \
			fi ;\
			if [ ! -d $(ABL_SIGNED)/$(SECTOOL_PROJ) ]; then \
				mkdir -p $(ABL_SIGNED)/$(SECTOOL_PROJ); \
			fi ;\
			SECIMAGE_LOCAL_DIR=$(SECTOOL_DIR) \
			USES_SEC_POLICY_MULTIPLE_DEFAULT_SIGN=$(USES_SEC_POLICY_MULTIPLE_DEFAULT_SIGN) \
			USES_SEC_POLICY_DEFAULT_SUBFOLDER_SIGN=$(USES_SEC_POLICY_DEFAULT_SUBFOLDER_SIGN) \
			USES_SEC_POLICY_INTEGRITY_CHECK=$(USES_SEC_POLICY_INTEGRITY_CHECK) \
			python3 $(SECTOOL_DIR)/sectools_builder.py \
			-i $(ABL_FV_ELF) --install_file_name abl.elf \
			-t $(SECTOOL_DIR)/$(SECTOOL_DIR)/signed \
			-g $(SIGN_ID) \
			--soc_hw_version $(SOC_HW_VER) \
			--soc_vers $(SOC_VERS) \
			--config=$(SECTOOL_DIR)/config/integration/$(SECIMAGE_XML).xml \
			--install_base_dir=$(ABL_SIGNED)/$(SECTOOL_PROJ) \
			> $(ABL_SIGNED)/$(SECTOOL_PROJ)/secimage.log 2>&1 ;\
			echo Completed secimage signed appsbl \(logs in $(ABL_SIGNED)/$(SECTOOL_PROJ)/secimage.log\) ;\
			if [ ! -f $(ABL_SIGNED)/$(SECTOOL_PROJ)/abl.elf ]; then \
				echo "Failed to generate signed ABL Image !!"; \
				exit 1 ; \
			fi ;\
			echo -------------------------------------------------------------- ;\
			echo Signed ABL Image: $(ABL_SIGNED)/$(SECTOOL_PROJ)/abl.elf ;\
			echo -------------------------------------------------------------- ;\
		fi ;\
	fi
endef

# SRPM Packaging
PREFIX="abl-1.0"
TARBALL="${PREFIX}.tar.gz"
SPECFILE="srpm/abl.spec"
RPM_DIR=rpmbuild
RPMBUILDOPTS="-bs"
GITID=$(shell git log --max-count=1 --pretty=format:%H HEAD)
XZ_THREADS=$(rpm --eval %{_smp_mflags} | sed -e 's!^-j!--threads !')
define package-srpm
	rm -rf $(RPM_DIR) *.src.rpm $(TARBALL)
	mkdir -p $(RPM_DIR)/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
	git archive --prefix="$(PREFIX)"/ --format=tar "$(GITID)" | gzip -v > $(TARBALL) 
	cp -f $(SPECFILE) $(RPM_DIR)/SPECS
	cp -f $(TARBALL) $(RPM_DIR)/SOURCES  
	rpmbuild --define "_sourcedir $(RPM_DIR)/SOURCES" --define "_builddir $(RPM_DIR)/BUILD" --define "_srcrpmdir $(RPM_DIR)/SRPMS" --define "_rpmdir $(RPM_DIR)/RPMS" --define "_specdir $(RPM_DIR)/SPECS" $(RPMBUILDOPTS) $(RPM_DIR)/SPECS/*.spec
	mv $(RPM_DIR)/SRPMS/* .
	rm -rf $(RPM_DIR) $(TARBALL)
	@echo --------------------------------------------------------------
	@echo ABL SRPM: abl-1.0-1.src.rpm 
	@echo --------------------------------------------------------------
endef

# This function is to check version compatibility, used to control features based on the compiler version. \
Arguments should be return value, current version and supported version in order. \
It sets return value to true if the current version is equal or greater than the supported version.
define check_version_compatibility
	$(eval CURR_VERSION := $(shell $(2)/clang --version |& grep -i "clang version" |& sed 's/^.*[^0-9]\([0-9]*\.[0-9]*\)\..*/\1/'))
	$(eval CURR_VERSION_MAJOR := $(shell echo $(CURR_VERSION) |& cut -d. -f1))
	$(eval CURR_VERSION_MINOR := $(shell echo $(CURR_VERSION) |& cut -d. -f2))
	$(eval SUPPORTED_VERSION := $(3))
	$(eval SUPPORTED_VERSION_MAJOR := $(shell echo $(SUPPORTED_VERSION) |& cut -d. -f1))
	$(eval SUPPORTED_VERSION_MINOR := $(shell echo $(SUPPORTED_VERSION) |& cut -d. -f2))

	ifeq ($(shell expr $(CURR_VERSION_MAJOR) \> $(SUPPORTED_VERSION_MAJOR)), 1)
		$(1) := true
	endif
	ifeq ($(shell expr $(CURR_VERSION_MAJOR) \= $(SUPPORTED_VERSION_MAJOR)), 1)
		ifeq ($(shell expr $(CURR_VERSION_MINOR) \>= $(SUPPORTED_VERSION_MINOR)), 1)
			$(1) := true
		endif
	endif
endef

# UEFI UBSAN Configuration
# ENABLE_UEFI_UBSAN := true

ifeq "$(ENABLE_UEFI_UBSAN)" "true"
	UBSAN_GCC_FLAG_UNDEFINED := -fsanitize=undefined
	UBSAN_GCC_FLAG_ALIGNMENT := -fno-sanitize=alignment
else
	UBSAN_GCC_FLAG_UNDEFINED :=
	UBSAN_GCC_FLAG_ALIGNMENT :=
endif

ifeq ($(TARGET_ARCHITECTURE), arm)
	LOAD_ADDRESS := 0X8F700000
else
	LOAD_ADDRESS := 0X9FA00000
endif

ifeq ($(ENABLE_LE_VARIANT), true)
	ENABLE_LE_VARIANT := 1
else
	ENABLE_LE_VARIANT := 0
endif

ifeq ($(ENABLE_LV_ATOMIC_AB), 1)
        ENABLE_LV_ATOMIC_AB := 1
else
        ENABLE_LV_ATOMIC_AB := 0
endif

ifeq ($(EARLY_ETH_ENABLED), 1)
	EARLY_ETH_ENABLED := 1
else
	EARLY_ETH_ENABLED := 0
endif

ifeq ($(AUTO_PATCH_ABL), 1)
	AUTO_PATCH_ABL := 1
else
	AUTO_PATCH_ABL := 0
endif

ifeq ($(DISABLE_PRINT), 1)
	DISABLE_PRINT := 1
else
	DISABLE_PRINT := 0
endif

ifeq ($(EARLY_ETH_AS_DLKM), 1)
	EARLY_ETH_AS_DLKM := 1
else
	EARLY_ETH_AS_DLKM := 0
endif

ifeq ($(AUTO_VIRT_ABL), 1)
	AUTO_VIRT_ABL := 1
else
	AUTO_VIRT_ABL := 0
endif

ifeq ($(TEST_ADAPTER), 1)
	TEST_ADAPTER := 1
else
	TEST_ADAPTER := 0
endif

ifeq ($(DISABLE_KERNEL_PROTOCOL), 1)
	DISABLE_KERNEL_PROTOCOL := 1
else
	DISABLE_KERNEL_PROTOCOL := 0
endif

ifeq ($(TARGET_SUPPORTS_EARLY_USB_INIT), 1)
	TARGET_SUPPORTS_EARLY_USB_INIT := 1
else
	TARGET_SUPPORTS_EARLY_USB_INIT := 0
endif

ifeq ($(BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL), 1)
	BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL := 1
else
	BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL := 0
endif

ifeq ($(DDR_SUPPORTS_SCT_CONFIG), true)
        DDR_SUPPORTS_SCT_CONFIG := 1
else
        DDR_SUPPORTS_SCT_CONFIG := 0
endif

ifeq "$(ABL_USE_SDLLVM)" "true"
	SDLLVM_COMPILE_ANALYZE := --compile-and-analyze
	SDLLVM_ANALYZE_REPORT := $(BUILD_REPORT_DIR)
else
	SDLLVM_COMPILE_ANALYZE :=
	SDLLVM_ANALYZE_REPORT :=
endif

ifneq "$(INIT_BIN_LE)" ""
	INIT_BIN := $(INIT_BIN_LE)
else
	INIT_BIN := "/init"
endif

ifneq "$(TARGET_AUDIO_FRAMEWORK)" ""
	TARGET_AUDIO_FRAMEWORK := $(TARGET_AUDIO_FRAMEWORK)
else
	TARGET_AUDIO_FRAMEWORK := "unknown"
endif

ifeq "$(BASE_ADDRESS)" ""
	BASE_ADDRESS := 0x80000000
endif

ifeq "$(TARGET_LINUX_BOOT_CPU_SELECTION)" "true"
	LINUX_BOOT_CPU_SELECTION_ENABLED := 1
else
	LINUX_BOOT_CPU_SELECTION_ENABLED := 0
endif

ifeq "$(TARGET_LINUX_BOOT_CPU_ID)" ""
	TARGET_LINUX_BOOT_CPU_ID := 0
endif

ifneq "$(HIBERNATION_PARTITION_NAME)" ""
	HIBERNATION_SWAP_PARTITION_NAME := $(HIBERNATION_PARTITION_NAME)
endif

ifneq "$(DISABLE_DTBO_PARTITION)" ""
	DISABLE_DTBO_PARTITION := $(DISABLE_DTBO_PARTITION)
endif

export SDLLVM_COMPILE_ANALYZE := $(SDLLVM_COMPILE_ANALYZE)
export SDLLVM_ANALYZE_REPORT := $(SDLLVM_ANALYZE_REPORT)

ifdef $(SAFESTACK_SUPPORTED_CLANG_VERSION)
CLANG_SUPPORTS_SAFESTACK := false
$(eval $(call check_version_compatibility, CLANG_SUPPORTS_SAFESTACK, $(CLANG_BIN), $(SAFESTACK_SUPPORTED_CLANG_VERSION)))
endif

ifeq "$(ABL_SAFESTACK)" "true"
	ifeq "$(CLANG_SUPPORTS_SAFESTACK)" "true"
		LLVM_ENABLE_SAFESTACK := -fsanitize=safe-stack
		LLVM_SAFESTACK_USE_PTR := -mllvm -safestack-use-pointer-address
		LLVM_SAFESTACK_COLORING := -mllvm -safe-stack-coloring=true
	endif
else
	LLVM_ENABLE_SAFESTACK :=
	LLVM_SAFESTACK_USE_PTR :=
	LLVM_SAFESTACK_COLORING :=
endif
export LLVM_ENABLE_SAFESTACK := $(LLVM_ENABLE_SAFESTACK)
export LLVM_SAFESTACK_USE_PTR := $(LLVM_SAFESTACK_USE_PTR)
export LLVM_SAFESTACK_COLORING := $(LLVM_SAFESTACK_COLORING)

CLANG_NEED_EXTRA_DLINK_FLAGS := false
$(eval $(call check_version_compatibility, CLANG_NEED_EXTRA_DLINK_FLAGS, $(CLANG_BIN), 17.0))

ifeq "$(CLANG_NEED_EXTRA_DLINK_FLAGS)" "true"
	export CLANG_EXTRA_DLINK_FLAGS := -Wl,--no-relax -Wl,--apply-dynamic-relocs
endif

.PHONY: all cleanall srpm tools tools-clean securefastboot securefastboot-clean

all: ABL_FV_ELF
	$(call validate-cum-sign)

cleanall:
	@. ./edksetup.sh BaseTools && \
	build -p $(WORKSPACE)/QcomModulePkg/QcomModulePkg.dsc -a $(ARCHITECTURE) -t $(TARGET_TOOLS) -b $(TARGET) -j build_modulepkg.log cleanall
	rm -rf $(WORKSPACE)/QcomModulePkg/Bin64
	rm -rf $(TARGET_EDK_TOOLS_BIN)

# Standalone AndroidToolsPkg applications (RebootTools, ArbTools). These are
# independent UEFI applications built from their own DSC; they do not touch the
# main ABL firmware image. The output goes to Build/AndroidToolsPkg so it never
# collides with the main QcomModulePkg output tree. Depends on the BaseTools
# build mark so the C compiler tools exist.
ANDROID_TOOLS_DSC := $(WORKSPACE)/AndroidToolsPkg/AndroidToolsPkg.dsc
ANDROID_TOOLS_OUT := Build/AndroidToolsPkg/$(TARGET)_$(TARGET_TOOLS)/$(ARCHITECTURE)

tools: $(EDK_TOOLS_PATH_MARK_FILE)
	@. ./edksetup.sh BaseTools && \
	build -p $(ANDROID_TOOLS_DSC) \
	-a $(ARCHITECTURE) \
	-t $(TARGET_TOOLS) \
	-b $(TARGET) \
	-j build_androidtools.log
	@echo "AndroidToolsPkg built: $(ANDROID_TOOLS_OUT)"

tools-clean:
	@. ./edksetup.sh BaseTools && \
	build -p $(ANDROID_TOOLS_DSC) -a $(ARCHITECTURE) -t $(TARGET_TOOLS) -b $(TARGET) -j build_androidtools.log cleanall
	rm -rf $(WORKSPACE)/AndroidToolsPkg/Bin64

# Standalone SecureFastbootPkg preload wrapper. Built from its own DSC so it
# never collides with the main QcomModulePkg output tree; it is launched from
# the config container, not linked into the BDS FV.
SECURE_FASTBOOT_DSC := $(WORKSPACE)/SecureFastbootPkg/SecureFastbootPkg.dsc
SECURE_FASTBOOT_OUT := Build/SecureFastbootPkg/$(TARGET)_$(TARGET_TOOLS)/$(ARCHITECTURE)

securefastboot: $(EDK_TOOLS_PATH_MARK_FILE)
	@. ./edksetup.sh BaseTools && \
	build -p $(SECURE_FASTBOOT_DSC) \
	-a $(ARCHITECTURE) \
	-t $(TARGET_TOOLS) \
	-b $(TARGET) \
	-j build_securefastboot.log
	@echo "SecureFastbootPkg built: $(SECURE_FASTBOOT_OUT)"

securefastboot-clean:
	@. ./edksetup.sh BaseTools && \
	build -p $(SECURE_FASTBOOT_DSC) -a $(ARCHITECTURE) -t $(TARGET_TOOLS) -b $(TARGET) -j build_securefastboot.log cleanall
	rm -rf $(WORKSPACE)/SecureFastbootPkg/Bin64

$(EDK_TOOLS_PATH_MARK_FILE): $(EDK_TOOLS_SRC_FILE)
	@$(call edk_tools_generate)

ABL_FV_IMG: $(EDK_TOOLS_PATH_MARK_FILE)
	@. ./edksetup.sh BaseTools && \
	build -p $(WORKSPACE)/QcomModulePkg/QcomModulePkg.dsc \
	-a $(ARCHITECTURE) \
	-t $(TARGET_TOOLS) \
	-b $(TARGET) \
	-D ABL_OUT_DIR=$(ABL_OUT) \
	-D VERIFIED_BOOT_LE=$(VERIFIED_BOOT_LE) \
	-D TEST_ADAPTER=$(TEST_ADAPTER) \
	-D VERIFIED_BOOT_ENABLED=$(VERIFIED_BOOT_ENABLED) \
	-D ROOT_PARTLABEL_SUPPORT=$(ROOT_PARTLABEL_SUPPORT) \
	-D USE_DUMMY_BCC=$(USE_DUMMY_BCC) \
	-D EARLY_ETH_ENABLED=$(EARLY_ETH_ENABLED) \
	-D AUTO_LVGVM_ABL=$(AUTO_LVGVM_ABL) \
	-D HIBERNATION_SUPPORT_NO_AES=$(HIBERNATION_SUPPORT_NO_AES) \
	-D HIBERNATION_SUPPORT_AES=$(HIBERNATION_SUPPORT_AES) \
	-D HIBERNATION_TZ_ENCRYPTION=$(HIBERNATION_TZ_ENCRYPTION) \
	-D APPEND_RAM_PARTITIONS_TO_MEM_NODE=$(APPEND_RAM_PARTITIONS_TO_MEM_NODE) \
	-D EARLY_ETH_AS_DLKM=$(EARLY_ETH_AS_DLKM) \
	-D BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL=$(BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL) \
	-D AB_RETRYCOUNT_DISABLE=$(AB_RETRYCOUNT_DISABLE) \
	-D TARGET_BOARD_TYPE_AUTO=$(TARGET_BOARD_TYPE_AUTO) \
	-D VERITY_LE=$(VERITY_LE) \
	-D USER_BUILD_VARIANT=$(USER_BUILD_VARIANT) \
	-D DISABLE_PARALLEL_DOWNLOAD_FLASH=$(DISABLE_PARALLEL_DOWNLOAD_FLASH) \
	-D ENABLE_LE_VARIANT=$(ENABLE_LE_VARIANT) \
	-D AUTO_PATCH_ABL=$(AUTO_PATCH_ABL) \
	-D DISABLE_PRINT=$(DISABLE_PRINT) \
	-D WEAR_OS=$(WEAR_OS) \
	-D ENABLE_LV_ATOMIC_AB=$(ENABLE_LV_ATOMIC_AB) \
	-D BUILD_USES_RECOVERY_AS_BOOT=$(BUILD_USES_RECOVERY_AS_BOOT) \
	-D INIT_BIN=$(INIT_BIN) \
	-D UBSAN_UEFI_GCC_FLAG_UNDEFINED=$(UBSAN_GCC_FLAG_UNDEFINED) \
	-D UBSAN_UEFI_GCC_FLAG_ALIGNMENT=$(UBSAN_GCC_FLAG_ALIGNMENT) \
	-D NAND_SQUASHFS_SUPPORT=$(NAND_SQUASHFS_SUPPORT) \
	-D BASE_ADDRESS=$(BASE_ADDRESS) \
	-D LINUX_BOOT_CPU_SELECTION_ENABLED=$(LINUX_BOOT_CPU_SELECTION_ENABLED) \
	-D TARGET_LINUX_BOOT_CPU_ID=$(TARGET_LINUX_BOOT_CPU_ID) \
	-D HIBERNATION_SWAP_PARTITION_NAME=$(HIBERNATION_SWAP_PARTITION_NAME) \
	-D DISABLE_DTBO_PARTITION=$(DISABLE_DTBO_PARTITION) \
	-D SUPPORT_AB_BOOT_LXC=$(SUPPORT_AB_BOOT_LXC) \
	-D TARGET_SUPPORTS_EARLY_USB_INIT=$(TARGET_SUPPORTS_EARLY_USB_INIT) \
	-D TARGET_AUDIO_FRAEMEWORK=$(TARGET_AUDIO_FRAMEWORK) \
        -D ENABLE_EARLY_SERVICES=$(ENABLE_EARLY_SERVICES) \
        -D KERNEL_LOAD_ADDRESS=$(KERNEL_LOAD_ADDRESS) \
        -D KERNEL_SIZE_RESERVED=$(KERNEL_SIZE_RESERVED) \
	-D DISABLE_KERNEL_PROTOCOL=$(DISABLE_KERNEL_PROTOCOL) \
	-D NAND_UBI_VOLUME_FLASHING_ENABLED=$(NAND_UBI_VOLUME_FLASHING_ENABLED) \
	-D REMOVE_CARVEOUT_REGION=$(REMOVE_CARVEOUT_REGION) \
	-D QSPA_BOOTCONFIG_ENABLE=$(QSPA_BOOTCONFIG_ENABLE) \
	-D AUTO_VIRT_ABL=$(AUTO_VIRT_ABL) \
	-D DDR_SUPPORTS_SCT_CONFIG=$(DDR_SUPPORTS_SCT_CONFIG) \
	-j build_modulepkg.log $*

	cp $(BUILD_ROOT)/FV/FVMAIN_COMPACT.Fv $(ABL_FV_IMG)
ifeq ($(AUTO_VIRT_ABL), 1)
	cp $(BUILD_ROOT)/$(ARCHITECTURE)/QcomModulePkg/Application/LinuxLoader/LinuxLoader/$(TARGET)/LinuxLoader.efi $(ABL_FV_EFI)
endif

$(EDK_TOOLS_GENERATE_CLEAN): $(EDK_TOOLS_PATH_MARK_FILE)
	@$(MAKEPATH)make -C $(BUILDDIR)/BaseTools/Source/C clean > /dev/null
	touch $(EDK_TOOLS_GENERATE_CLEAN)

BASETOOLS_CLEAN: ABL_FV_IMG
	@rm -rf $(BUILDDIR)/Conf/BuildEnv.sh

ABL_FV_ELF: BASETOOLS_CLEAN $(EDK_TOOLS_GENERATE_CLEAN)
	python3 $(WORKSPACE)/QcomModulePkg/Tools/image_header.py $(ABL_FV_IMG) $(ABL_FV_ELF) $(LOAD_ADDRESS) elf 32 nohash

srpm:
	$(call package-srpm)
