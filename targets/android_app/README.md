# EFI Boot Mgr (`com.gblbds.manager`)

Android app (root required) that manages the super-fastboot BDS config
container and its boot-entry list. Serving other apps is intentionally out of
scope for now.

## Build

The Gradle wrapper downloads Gradle 9.5 itself. Android Studio (or a one-off
`cmdline-tools` bootstrap) provides the SDK; AGP downloads missing
components (`android.builder.sdkDownload=true` in `gradle.properties`) and the
NDK declared in `app/build.gradle.kts`. Point the build at your SDK with
`ANDROID_HOME` (or `ANDROID_SDK_ROOT`); nothing is tied to a checkout
location:

```
ANDROID_HOME=/path/to/sdk make build     # assets -> APK -> signed APK -> tools
ANDROID_HOME=/path/to/sdk make install   # additionally adb install -r
```

`make build` rebuilds the BDS, the secure-fastboot wrapper and the native
tools, copies them into `app/src/main/assets`, runs `assembleRelease`,
zipaligns and signs with `~/.android/debug.keystore`, and builds
`tools/fastboot_auth`. Overridable: `JAVA_HOME` (JDK 17, auto-detected),
`NDK_PATH`, `BUILD_TOOLS`, `ADB`, `KEYSTORE`, `KS_PASS`, `KEY_ALIAS`.

The plain Gradle flow still works (`./gradlew assembleRelease`) once the SDK
is provided via `ANDROID_HOME` or a local, untracked `local.properties`.

Verified toolchain: JDK 17, Android SDK Platform 37.0 / Build Tools 36.0.0,
AGP 9.3.1, Kotlin 2.4.20, Compose BOM 2026.09.00. The bundled native tools
are compiled for API 31 (`aarch64-linux-android31-clang`), matching the
toolkit / Magisk module builds in this repository.

`app/src/main/assets/` must contain:

| Asset | Source |
| --- | --- |
| `BDS.efi` | `submodules/uefi/build/BDS.efi` (`make build`) |
| `secure_fastboot.efi` | `submodules/uefi/build/SecureFastboot.efi` (`make securefastboot`) |
| `patch_abl` | `submodules/patcher` Android NDK build |
| `extractfv` | `submodules/ablfvextractor` Android NDK build (needs liblzma) |

Refresh all of them with `make assets` (or let `make build` do it), or run
the legacy helper `sh refresh_assets.sh` with `NDK_PATH` set.

The app logs a warning at startup for any missing asset; first-run functions
that need them will fail rather than silently doing nothing.

## First run / re-deploy

A fresh install (or a container without the initialization marker) opens the
deployment page. Settings > **Re-deploy** opens the same page at any time.

- **Current system ABL** - the image patched into `persist/efisp/boot.efi`:
  pick an `abl.elf/img` (patched on the device with `extractfv` + `patch_abl`),
  pick an already patched `.efi`, or - on a clean system whose efisp holds no
  BDS image - read the running slot's ABL straight from the partition.
- **ABL with the efisp vulnerability** - only needed when the running ABL
  lacks it: pick a file, download from the repo, or take the partition copy.
  The section is disabled entirely when the current slot is already
  vulnerable. Self-provided files carry the model / black-brick warning.
- **Install** patches boot.efi, flashes the BDS core, copies every additional
  system EFI (including the secure-fastboot wrapper), keeps a non-empty
  container (formats only when there is nothing usable), installs boot.efi,
  ensures the initialization marker, rewrites the system entries from their
  templates and - only when the current slot ABL lacks the vulnerability -
  flashes the vulnerable ABL to the current slot.
- Secure boot is touched only when the keystore already holds the key.

## Every later start

- Mount the container if not already mounted: `losetup -o 1MiB -S
  <container bytes>` on `efisp` (Android toybox only has short options), then
  `mount -t vfat` at `/data/adb/sfbconfig`.
- `CONFIG\INITIALIZED` in the container is the "already set up" marker. If it
  is present, the app loads `CONFIG\ENTRIES.CFG` and goes straight to the
  editor. Without it (fresh install, corrupt container, older install) the
  deployment page opens.
- Without a secure lock screen the auth-per-use keystore key is unusable, so
  secure boot is force-disabled (BOOT_CERTS certificate and keystore key
  removed) before anything else.
- The app then compares the bundled **system components** with the device and
  offers one combined update prompt when anything is outdated (see below).
- Dismissing that prompt sets a pending flag; the post-OTA operation forces
  the full component update before running.

## System component update

One "BDS update" consists of three parts, all defined by built-in lists:

1. **BDS core** - `BDS.efi` in the raw efisp partition.
2. **Additional EFIs** - `secure_fastboot.efi` under `persist/efisp`.
3. **System boot entries** - hardcoded templates rendered with the persist
   address, e.g. `Android:(5,8)\efisp\secure_fastboot.efi \efisp\boot.efi`
   (and the same for `boot_backup.efi`).

Detection compares content only: the BDS by raw bytes, EFIs with the
signature segment stripped on both sides (the bundled asset is unsigned, the
device copy may be signed), and each entry template rendered once and matched
against `ENTRIES.CFG`. Signatures are not part of the detection.

On confirm **nothing is skipped**: the BDS is flashed, every additional EFI is
force-copied, every system entry is rewritten from its template, and then
`signAll()` runs. When the keystore has no secure-boot key the user never
enabled it and no secure-boot operation happens at all.

## Boot entries

- **Add**: file picker selects an `.efi`; the user enters the entry name and
  the destination file name. On save the file is copied into
  `persist/efisp/` under that name and the entry `Name:(d,p)\efisp\<file>`
  is written, where `(d,p)` is the persist volume's address
  (`sda`=LUN0, `sdb`=LUN1, …; `p` = the GPT partition number itself,
  e.g. `/dev/block/sdf8` -> `(5,8)`, matching the device's existing
  `CONFIG\ENTRIES.CFG`).
- **Delete**: removes the entry and, on save, deletes its `.efi` file too
  (`boot.efi` / `boot_backup.efi` are never deleted).
- **Save**: writes `CONFIG\ENTRIES.CFG` and `sync`s.

## FAT formatter

`core/FatFormatter.kt` is a byte-for-byte port of the BDS embedded formatter
(`SuperFbConfigFormat.c`): same geometry iteration, boot sector, FAT EOC
markers, volume label and (for FAT32) FSInfo. The image is written with
`dd ... seek=2048 count=<sectors>` so a longer efisp partition is handled
without touching the following partition.

## Settings

The top-right gear opens a full settings page (there is no dropdown and no
dialog): secure boot state and the enable/re-sign action, then the **Secure
fastboot** entry.

## Secure boot (BOOT_CERTS)

The key is an RSA-2048 AndroidKeyStore identity (StrongBox when available),
auth-per-use, bound to the fingerprint. Enabling creates the key when needed,
signs every `.efi` under `persist/efisp` and writes `BOOT_CERTS/BOOT.CER`; the
state shows enabled, disabled, or degraded when an image lacks a valid
signature by the current key.

- Signing is **lazy**: the per-file signer first verifies an existing
  signature (WIN_CERTIFICATE + signer certificate equals the keystore
  certificate + RSA verification over the canonical stream). Files that
  already verify are skipped without a fingerprint prompt.
- **Disable** removes `BOOT_CERTS/BOOT.CER` first, then the keystore key.
  Signed EFI files stay as they are but are no longer verified.
- Without a secure lock screen the key cannot be used, so startup force-
  disables secure boot (certificate + key removed) to keep OTA and new boot
  entries working.
- The BDS enforces from the next boot.

## Secure fastboot (AUTH_CERTS)

- The page lists paired computers (one certificate each, `CN=user@host`,
  SHA-256 fingerprint shown) and can remove them.
- **Pair a computer** opens a one-shot pairing service on TCP port 37021 and
  shows the phone's address plus a six-digit code. On the computer:
  `fastboot_auth pair <ip>:37021`, then enter the code.
- The computer hands over a self-signed certificate; the phone answers with
  a random challenge and only installs the certificate into
  `AUTH_CERTS` when the computer proves it holds the matching
  private key. One connection attempt per code.
- The BDS reads `AUTH_CERTS` at boot, so a paired or removed certificate
  takes effect on the next boot. With at least one certificate the device
  boots locked; unlock a boot session with `fastboot_auth auth`.
- Every **system loader** entry (`boot.efi` and `boot_backup.efi`) is
  chainloaded through `secure_fastboot.efi`: non-privileged sessions get all
  console keys blocked before the stock loader runs, so its fastboot and
  recovery shortcuts cannot trigger. The entries are hardcoded templates
  rendered with the persist address and refreshed by the component update.
- The host tool is `tools/fastboot_auth` (C, OpenSSL). The old
  `tools/sfbsec/fastboot_unlock.sh` helper is gone.
