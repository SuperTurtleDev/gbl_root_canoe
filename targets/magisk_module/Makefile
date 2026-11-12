clean:
	rm -rf build || true
copy_resources:
	mkdir -p build/module/efisp/tools
	cp -r module/* build/module/
	[ -d ../../ablrepo ] && cp -r ../../ablrepo ./build/module/ablrepo || true
submodule_uefi: copy_resources
	cd ../../submodules/uefi && make build && make tools
	cp ../../submodules/uefi/build/BDS.efi ./build/module/BDS.efi
	cp ../../submodules/uefi/build/ArbTools.efi ./build/module/efisp/tools/ArbTools.efi
	cp ../../submodules/uefi/build/BLTools.efi ./build/module/efisp/tools/BLTools.efi
	cp ../../submodules/uefi/build/RebootTools.efi ./build/module/efisp/tools/RebootTools.efi
submodule_patcher: copy_resources
	cd ../../submodules/patcher && make build_android
	cp ../../submodules/patcher/build/patch_abl_android ./build/module/bin/patch_abl
submodule_ablfvextractor: copy_resources
	cd ../../submodules/ablfvextractor && make build_android
	cp ../../submodules/ablfvextractor/build/extractfv_android ./build/module/bin/extractfv
build: submodule_uefi submodule_patcher submodule_ablfvextractor
	# zip
	cd build/module && zip -r ../module_android.zip ./
