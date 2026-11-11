.PHONY: clean build tools tools-clean

clean:
	rm -rf edk2/Build || true
	rm -rf edk2/Conf || true
	rm -rf ./build || true
build:
	cp -r ./Conf ./edk2/
	# This vendor EDK2 build has reused objects after included headers changed
	# (stale limits survive header updates). A fresh final EFI alone does not
	# prove fresh code: rebuild the full BDS output tree.
	rm -rf edk2/Build/RELEASE_CLANG35
	rm -f ./build/BDS.efi
	bash -c 'cd edk2 && . ./edksetup.sh && make BOARD_BOOTLOADER_PRODUCT_NAME=canoe TARGET_ARCHITECTURE=AARCH64 TARGET=RELEASE \
  		CLANG_BIN=/usr/bin/ CLANG_PREFIX=aarch64-linux-gnu- VERIFIED_BOOT_ENABLED=1 \
  		VERIFIED_BOOT_LE=0 AB_RETRYCOUNT_DISABLE=0 TARGET_BOARD_TYPE_AUTO=0 \
  		BUILD_USES_RECOVERY_AS_BOOT=0 DISABLE_PARALLEL_DOWNLOAD_FLASH=0 PVMFW_BCC_ENABLED=-DPVMFW_BCC\
  		REMOVE_CARVEOUT_REGION=1 QSPA_BOOTCONFIG_ENABLE=1 USER_BUILD_VARIANT=0 TEST_ADAPTER=1 \
  		PREBUILT_HOST_TOOLS="BUILD_CC=clang BUILD_CXX=clang++ LDPATH=-fuse-ld=lld BUILD_AR=llvm-ar"' || true
	# test if the build is successful by checking the output file
	if [ ! -f edk2/Build/RELEASE_CLANG35/AARCH64/LinuxLoader.efi ]; then \
		echo "Build failed"; \
		exit 1; \
	fi
	mkdir -p ./build
	cp edk2/Build/RELEASE_CLANG35/AARCH64/LinuxLoader.efi ./build/BDS.efi
	# efisp holds BDS in its first 1 MiB and the config FAT container after
	# it. BDS is flashed by its own length (never padded), so it must stay
	# strictly below 1 MiB or flashing it would overwrite the container.
	@size=$$(wc -c < ./build/BDS.efi); \
	if [ "$$size" -ge 1048576 ]; then \
		echo "BDS.efi is $$size bytes; it must be smaller than 1 MiB"; \
		exit 1; \
	fi
	@echo "BDS built successfully: build/BDS.efi"

# Build the standalone AndroidToolsPkg applications (RebootTools, ArbTools,
# BLTools).
# These are independent UEFI apps launched from the boot menu; they do not
# affect the main BDS image. Passes the same toolchain env as `build` so the
# CLANG35 toolchain and BaseTools resolve identically.
tools:
	cp -r ./Conf ./edk2/ 2>/dev/null || true
	# Remove all standalone-tool outputs so EDK2 cannot reuse a stale artifact.
	rm -rf edk2/Build/AndroidToolsPkg
	bash -c 'cd edk2 && . ./edksetup.sh && make tools BOARD_BOOTLOADER_PRODUCT_NAME=canoe TARGET_ARCHITECTURE=AARCH64 TARGET=RELEASE \
  		CLANG_BIN=/usr/bin/ CLANG_PREFIX=aarch64-linux-gnu- VERIFIED_BOOT_ENABLED=1 \
  		VERIFIED_BOOT_LE=0 AB_RETRYCOUNT_DISABLE=0 TARGET_BOARD_TYPE_AUTO=0 \
  		BUILD_USES_RECOVERY_AS_BOOT=0 DISABLE_PARALLEL_DOWNLOAD_FLASH=0 PVMFW_BCC_ENABLED=-DPVMFW_BCC \
  		REMOVE_CARVEOUT_REGION=1 QSPA_BOOTCONFIG_ENABLE=1 USER_BUILD_VARIANT=0 TEST_ADAPTER=1 \
  		PREBUILT_HOST_TOOLS="BUILD_CC=clang BUILD_CXX=clang++ LDPATH=-fuse-ld=lld BUILD_AR=llvm-ar"' || true
	@TOOLS_DIR=edk2/Build/AndroidToolsPkg/RELEASE_CLANG35/AARCH64; \
	if [ ! -f $$TOOLS_DIR/RebootTools.efi ] || [ ! -f $$TOOLS_DIR/ArbTools.efi ] || [ ! -f $$TOOLS_DIR/BLTools.efi ]; then \
		echo "AndroidToolsPkg build failed"; \
		exit 1; \
	fi
	mkdir -p ./build
	cp edk2/Build/AndroidToolsPkg/RELEASE_CLANG35/AARCH64/RebootTools.efi ./build/RebootTools.efi
	cp edk2/Build/AndroidToolsPkg/RELEASE_CLANG35/AARCH64/ArbTools.efi ./build/ArbTools.efi
	cp edk2/Build/AndroidToolsPkg/RELEASE_CLANG35/AARCH64/BLTools.efi ./build/BLTools.efi
	@echo "AndroidTools built successfully: build/RebootTools.efi build/ArbTools.efi build/BLTools.efi"

tools-clean:
	bash -c 'cd edk2 && . ./edksetup.sh && make tools-clean TARGET_ARCHITECTURE=AARCH64 TARGET=RELEASE \
  		CLANG_BIN=/usr/bin/ CLANG_PREFIX=aarch64-linux-gnu- \
  		PREBUILT_HOST_TOOLS="BUILD_CC=clang BUILD_CXX=clang++ LDPATH=-fuse-ld=lld BUILD_AR=llvm-ar"' || true
	rm -f ./build/RebootTools.efi ./build/ArbTools.efi ./build/BLTools.efi || true

