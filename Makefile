submodule_uefi_clean:
	cd submodules/uefi && make clean
submodule_patcher_clean:
	cd submodules/patcher && make clean
submodule_ablfvextractor_clean:
	cd submodules/ablfvextractor && make clean
clean_submodules: submodule_uefi_clean submodule_patcher_clean submodule_ablfvextractor_clean

target_toolkit_windows_clean:
	cd targets/toolkit_windows && make clean
target_toolkit_linux_clean:
	cd targets/toolkit_linux && make clean
target_magisk_module_clean:
	cd targets/magisk_module && make clean
target_toolkit_android_clean:
	cd targets/toolkit_android && make clean
target_android_app_clean:
	cd targets/android_app && make clean
targets_clean: clean_submodules target_toolkit_windows_clean target_toolkit_linux_clean target_magisk_module_clean target_toolkit_android_clean target_android_app_clean

clean: targets_clean clean_submodules

target_toolkit_windows:
	cd targets/toolkit_windows && make build
target_toolkit_linux:
	cd targets/toolkit_linux && make build
target_magisk_module:
	cd targets/magisk_module && make build
target_toolkit_android:
	cd targets/toolkit_android && make build
target_android_app:
	cd targets/android_app && make build

dev_target_extract_and_patch:
	cd dev_targets/extract_and_patch && make patch

# tools not dependency of main project, build separately
tools_vbmetafixer_linux:
	cd tools/vbmetafixer && make build
tools_vbmetafixer_windows:
	cd tools/vbmetafixer && make build_windows
tools_vbmetafixer_android:
	cd tools/vbmetafixer && make build_android
tools_fastboot_auth:
	cd tools/fastboot_auth && make
