.PHONY: clean copy_resources submodule_uefi submodule_patcher submodule_ablfvextractor build test

clean:
	rm -rf build || true
copy_resources:
	mkdir -p build/toolkit/bin
	mkdir -p build/toolkit/efisp/tools
	cp -r resources/* build/toolkit/
submodule_uefi: copy_resources
	cd ../../submodules/uefi && make build && make tools
	cp ../../submodules/uefi/build/BDS.efi ./build/toolkit/BDS.efi
	cp ../../submodules/uefi/build/ArbTools.efi ./build/toolkit/efisp/tools/ArbTools.efi
	cp ../../submodules/uefi/build/BLTools.efi ./build/toolkit/efisp/tools/BLTools.efi
	cp ../../submodules/uefi/build/RebootTools.efi ./build/toolkit/efisp/tools/RebootTools.efi
submodule_patcher: copy_resources
	cd ../../submodules/patcher && make build_windows
	cp ../../submodules/patcher/build/patch_abl.exe ./build/toolkit/bin/patch_abl.exe
submodule_ablfvextractor: copy_resources
	cd ../../submodules/ablfvextractor && make build_windows
	cp ../../submodules/ablfvextractor/build/extractfv.exe ./build/toolkit/bin/extractfv.exe
build: submodule_uefi submodule_patcher submodule_ablfvextractor
	# zip
	cd build/toolkit && zip -r ../toolkit_windows.zip ./

# Does not rebuild the unrelated UEFI payloads or touch a connected device.
test: submodule_patcher submodule_ablfvextractor
	python3 tests/test_wine.py
