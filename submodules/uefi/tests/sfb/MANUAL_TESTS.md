# Manual acceptance: config store and boot entries

Automated host tests: `python3 tests/sfb/test_sfb.py -v` (from `submodules/uefi`).
The steps below need a person at a QEMU GUI or a real device.

## QEMU (single GPT disk: esp, efisp, efiboot)

In QEMU there is one disk, so every address is `(0,p)`: esp is `(0,0)`, efisp
is `(0,1)`, efiboot is `(0,2)`. Use the Up arrow for Volume Up, Up/Down to move
and Enter to select. These are the keys the firmware console already reports;
nothing was added for QEMU.

Prepare the image (BDS from `make build`, any AArch64 EFI app for efiboot, for
example `build/RebootTools.efi` from `make tools`):

```
python3 tests/sfb/mkqemu_disk.py --bds build/BDS.efi --app build/RebootTools.efi -o qemu-disk.img
```

Boot it with your usual AArch64 UEFI QEMU command line, attaching
`qemu-disk.img` as a raw disk. Helper to read the container on the host after
powering QEMU off (efisp starts at LBA 18432, container 2048 sectors later):

```
dd if=qemu-disk.img of=container.img bs=512 skip=20480 count=4096
fsck.fat -n container.img            # optional
python3 -c "import sys; sys.path.insert(0,'tests/sfb'); import mkqemu_disk as m; \
print(m.FatReader(open('container.img','rb').read()).read('CONFIG\\\\ENTRIES.CFG').decode())"
```

| # | Step | Expected |
| --- | --- | --- |
| Q1 | First boot of a fresh image, no key | Container gets formatted (label `SFBCONFIG`). `CONFIG\ENTRIES.CFG` lists `EFIBOOT (0,2)`, `Tests\Hello (this volume)`, `Tests\Hello (by address)` and `Tests\Not a PE image`, in that order, all with `(0,2)`. No line for `Tests\Missing file` or for the ESP's own `BOOTAA64.EFI`. The first entry is launched. |
| Q2 | Reboot, press Up within 1 s | "Entering Boot Menu", then the menu: `EFIBOOT (0,2)`, `Tests >`, Advanced, Power Off, Restart. `Tests >` opens the three test entries. |
| Q3 | Advanced > Boot Entries Priority: move `Tests\Not a PE image` to the top, reboot without a key | NOTPE is tried first, fails, and its line is gone from `CONFIG\ENTRIES.CFG`. The next entry launches. |
| Q4 | Advanced > Boot Entries Management: delete an entry, power off | The line is gone from `CONFIG\ENTRIES.CFG`, other lines keep their order. |
| Q5 | Rewrite BDS only: `dd if=build/BDS.efi of=qemu-disk.img bs=512 seek=18432 conv=notrunc`, boot | Entries from before are still there; container not reformatted. |
| Q6 | Edit efiboot `ENTRIES.CFG` to add a line for an existing file, boot with Up | New entry appended at the end of the stored list; old order kept. |
| Q7 | Store unavailable: attach the disk read-only (`readonly=on`) on a fresh image | Container cannot be formatted. BDS still scans, tries entries and shows the menu. Nothing is written. |
| Q8 | Format refused for a valid-looking FAT: write a FAT boot sector with a corrupt FAT to the container start, boot | Store reported unavailable in the log, container left as is, boot continues. |

## Real device

| # | Step | Expected |
| --- | --- | --- |
| D1 | Flash `BDS.efi` by its length into efisp, boot | First boot formats the container; entries are built from the volumes and stored. |
| D2 | Reflash a new `BDS.efi` by its length, boot | Stored entries and their order survive. |
| D3 | `fastboot erase efisp`, flash BDS again, boot | Container is reformatted empty; entries are rebuilt by a scan. |
| D4 | Make the container unformattable (for example a write-protected LUN), boot | Boot continues without persistent storage; menu and fastboot work. |
| D5 | Advanced > USB Mass Storage > Export Config Store, read-write | Host sees a GPT disk with one ESP partition named `efisp`, FAT label `SFBCONFIG`, holding `CONFIG\ENTRIES.CFG`. Edits made on the host are used after the session ends. |
| D6 | Same export, read-only | Host can read; writes do not reach the device. |
| D7 | Volume Up at power-on | Menu opens after a rescan; `(d,p)` names match the UFS LUN and GPT entry numbers. |

## Security (certificates, lock, EFI verification)

Fastboot USB cannot run under QEMU (the fastboot stack needs the Qualcomm USB
device controller), so the challenge flow is real-device only. S1-S6 are
automated by `python3 tests/sfb/qemu_security_test.py` (headless QEMU with
the in-tree firmware and monitor-driven key injection); they cover the
certificate-driven matrix. `tests/sfb/security_roundtrip.py` covers the
crypto on the host.

| # | Step | Expected |
| --- | --- | --- |
| S1 | QEMU, no `AUTH_CERTS`/`BOOT_CERTS` (or absent) | Behavior identical to before: full Advanced menu, any EFI app boots. |
| S2 | QEMU, `BOOT_CERTS` holds `boot.cer`, efiboot app unsigned | Entry refused; log shows `SFB-SEC: image carries no signature` / `rejected`. |
| S3 | QEMU, same, app signed with `tools/sfbsec/sign_efi.py` | App boots. |
| S4 | QEMU, `AUTH_CERTS` holds `auth.cer` | Advanced shows only `Enter Fastboot` with the lock hint. |
| S5 | QEMU, `AUTH_CERTS` + `BOOT_CERTS` present, unsigned app | Same as S2: verification still active at boot, independent of the auth lock. |
| S6 | QEMU, only `BOOT_CERTS`, signed app, open menu > Advanced | Full Advanced rows (privilege granted directly with no AUTH_CERTS), no lock hint; signed app boots. |
| R1 | Real device, `AUTH_CERTS` present, `fastboot flash boot ...` | `FAIL sb-locked`; only challenge commands work. |
| R2 | Real device, `tools/sfbsec/fastboot_unlock.sh auth.key` | `OKAY Unlocked`; full fastboot for this boot; reboot re-locks. |
| R3 | Real device, replay the same `sb_challenge.sig` again | `FAIL` - nonce consumed after the first attempt. |
| R4 | Real device, sign with a foreign key | `FAIL Challenge rejected`; still locked. |
| R5 | Real device, unlock then boot an unsigned EFI with `BOOT_CERTS` present | Refused - privilege and verification are independent. |
