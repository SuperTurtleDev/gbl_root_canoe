# Build orchestration for the EFI Boot Mgr app: system assets, APK, signing
# and the companion host tools.
#
# Every path is relative to this directory or taken from the environment; no
# checkout-specific absolute path is baked in.
#
#   ANDROID_HOME=/path/to/sdk make build
#   ANDROID_HOME=/path/to/sdk make install
#
# Overridable: JAVA_HOME, NDK_PATH, BUILD_TOOLS, ADB, KEYSTORE, KS_PASS,
# KEY_ALIAS.

SDK ?= $(ANDROID_HOME)
SDK := $(if $(strip $(SDK)),$(SDK),$(ANDROID_SDK_ROOT))

BUILD_TOOLS ?= $(shell ls -d $(SDK)/build-tools/* 2>/dev/null | sort -V | tail -n 1)
ADB ?= $(if $(wildcard $(SDK)/platform-tools/adb),$(SDK)/platform-tools/adb,adb)

# Matches ndkVersion in app/build.gradle.kts.
NDK_VERSION ?= 29.0.14206865
NDK_PATH ?= $(SDK)/ndk/$(NDK_VERSION)

# AGP 9 needs a JDK with javac; scan the usual install roots unless the caller
# provides JAVA_HOME.
JAVA_HOME ?= $(shell for d in "$$HOME/.jdks"/* /usr/lib/jvm/*; do \
	[ -x "$$d/bin/javac" ] && "$$d/bin/javac" -version 2>&1 | grep -q '^javac 17' && \
	{ echo "$$d"; break; }; done)

KEYSTORE ?= $(HOME)/.android/debug.keystore
KS_PASS ?= android
KEY_ALIAS ?= androiddebugkey

OUT := build
ASSETS := app/src/main/assets
GRADLE_APK := app/build/outputs/apk/release

.PHONY: all clean check_env assets submodule_uefi submodule_patcher \
	submodule_ablfvextractor apk sign build install tools tools_fastboot_auth

all: build

clean:
	rm -rf $(OUT) app/build

check_env:
	@test -n "$(SDK)" || { echo "ANDROID_HOME (or ANDROID_SDK_ROOT) is not set"; exit 1; }
	@test -x "$(NDK_PATH)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang" \
		|| { echo "NDK not found at $(NDK_PATH); set NDK_PATH"; exit 1; }

submodule_uefi:
	$(MAKE) -C ../../submodules/uefi build securefastboot
	cp ../../submodules/uefi/build/BDS.efi $(ASSETS)/BDS.efi
	cp ../../submodules/uefi/build/SecureFastboot.efi $(ASSETS)/secure_fastboot.efi

submodule_patcher:
	$(MAKE) -C ../../submodules/patcher build_android NDK_PATH=$(NDK_PATH)
	cp ../../submodules/patcher/build/patch_abl_android $(ASSETS)/patch_abl

submodule_ablfvextractor:
	NDK_PATH=$(NDK_PATH) $(MAKE) -C ../../submodules/ablfvextractor build_android
	cp ../../submodules/ablfvextractor/build/extractfv_android $(ASSETS)/extractfv

assets: check_env submodule_uefi submodule_patcher submodule_ablfvextractor

apk: assets
	@test -n "$(JAVA_HOME)" || { echo "JDK 17 not found; set JAVA_HOME"; exit 1; }
	JAVA_HOME=$(JAVA_HOME) ANDROID_HOME=$(SDK) ./gradlew assembleRelease

sign: apk
	@test -x "$(BUILD_TOOLS)/zipalign" || { echo "zipalign not found under $(BUILD_TOOLS)"; exit 1; }
	mkdir -p $(OUT)
	$(BUILD_TOOLS)/zipalign -f -p 4 $(GRADLE_APK)/app-release-unsigned.apk \
		$(OUT)/app-release-aligned.apk
	$(BUILD_TOOLS)/apksigner sign --ks $(KEYSTORE) --ks-pass pass:$(KS_PASS) \
		--key-pass pass:$(KS_PASS) --ks-key-alias $(KEY_ALIAS) \
		--out $(OUT)/app-release-signed.apk $(OUT)/app-release-aligned.apk
	rm -f $(OUT)/app-release-aligned.apk
	cp $(GRADLE_APK)/app-release-unsigned.apk $(OUT)/app-release-unsigned.apk

tools: tools_fastboot_auth

tools_fastboot_auth:
	$(MAKE) -C ../../tools/fastboot_auth

build: sign tools

install: build
	$(ADB) install -r $(OUT)/app-release-signed.apk
