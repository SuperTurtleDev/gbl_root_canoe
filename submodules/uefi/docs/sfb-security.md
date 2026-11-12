# SFB security model (SECURITY certificates, fastboot lock, EFI verification)

The BDS is the root of trust. Two certificate classes live in the efisp
config container as X.509 DER files, classified by directory (not by
certificate contents):

- `AUTH_CERTS\*.cer` - **class A**, fastboot authorization
- `BOOT_CERTS\*.cer` - **class B**, EFI code signing

Everything is certificate-driven, and the two classes are **independent**:
privilege never exempts an image from BOOT_CERTS verification.

Boot flow: the device starts **non-privileged**. The BOOT_CERTS list loads
first; an empty list means no signature verification. Before the boot menu
runs, the AUTH_CERTS list loads; when it is empty there is nothing to
authorize against and privilege is granted directly (fully open menus -
also the previous behavior). With AUTH_CERTS present the device stays
locked until a challenge response succeeds.

| AUTH_CERTS | BOOT_CERTS | Behavior |
| --- | --- | --- |
| empty | empty | Fully open (previous behavior) |
| non-empty | empty | Locked privileged operations until challenged; no image verification |
| empty | non-empty | Open menus; signed EFI enforcement always |
| non-empty | non-empty | Locked privileged operations + signed EFI enforcement always |

Certificates are loaded once at boot, while the container is mounted, and
kept in memory - fastboot runs with the store unmounted. At most
`SFB_SECURITY_MAX_CERTS` (8) per class load; unparsable files are skipped.

## Class A: fastboot challenge-response authorization

With class A certificates present the device is **locked**. In locked mode:

- The Advanced menu shows only `Enter Fastboot` - the authorization
  interface.
- fastboot rejects `flash:`, `erase:`, `boot` and partition `fetch:` with
  `FAIL sb-locked...`. Only `getvar:`, `download:`, `fetch:sb_challenge`,
  `flash:sb_challenge.sig`, `reboot` and `exit` are available.

Unlock flow (SSH-style; one attempt per nonce, success or failure):

```
fastboot fetch sb_challenge challenge.bin      # 32-byte nonce
openssl dgst -sha256 -sign auth.key challenge.bin > sb_challenge.sig
fastboot flash sb_challenge.sig sb_challenge.sig
```

The signature is a raw RSA/ECDSA signature (PKCS#1 v1.5 over SHA-256 of the
nonce, as `openssl dgst -sign` produces). It is verified against every
AUTH_CERTS key; a match grants **temporary privilege for the rest of the
boot**: full fastboot command set and full Advanced menu. Signature
verification is unaffected - BOOT_CERTS enforcement always runs. A reboot
re-locks. The nonce is generated from the
EFI RNG protocol when available, otherwise from timer/performance-counter
mixing; it is zeroed after every attempt, so a captured challenge cannot be
replayed and the same nonce cannot be signed twice.

Helper: `tools/sfbsec/fastboot_unlock.sh <auth.key>` runs the whole flow.

## Class B: EFI image verification

With class B certificates present, every image BDS launches is verified
through the EFI Security2 arch protocol (`SfbAllowAuth` in
`SuperFbEntries.c` -> `SfbSecurityVerifyImage`):

1. Parse the PE, locate the certificate table (security data directory).
2. Compute the **authentihash**: SHA-256 over the whole file except the COFF
   checksum field, the security data-directory entry and the certificate
   table itself.
3. Verify the embedded PKCS#7 (WIN_CERTIFICATE, type PKCS_SIGNED_DATA)
   signature over that hash against every BOOT_CERTS key.

An unsigned image is refused; a tampered one is refused. Privilege never
exempts an image: verification runs with or without it.

Signing tool: `tools/sfbsec/sign_efi.py <boot.cer> <boot.key> <app.efi>` -
appends the PKCS#7 (openssl `smime -sign -noattr -binary`) as the
certificate table entry. Note this is the SFB profile (signature directly
over the authentihash stream), matching the firmware's authentihash
implementation in `SfbPeVerify.c`, not the Windows Authenticode profile.

## Generating keys and installing certificates

```
tools/sfbsec/gen_certs.sh outdir/
# outdir/auth.cer -> AUTH_CERTS/   (locks the device)
# outdir/boot.cer -> BOOT_CERTS/   (enables verification)
```

Copy the `.cer` files into the container - via the toolkit persist folder
(like `ENTRIES.CFG`), USB export of the config store, or by editing the
container image directly. Keep the `.key` files private; the device never
needs them. Removing all `.cer` files from a directory switches that feature
off again on the next boot.

## Verification

Verification is on a real device: there are deliberately no test hooks in
the production image, and the certificate-driven switches are the test
seams. Add/remove `.cer` files to switch each class on and off, run
`tools/sfbsec/fastboot_unlock.sh <auth.key>` for the challenge flow, and
boot signed/unsigned EFI apps to check BOOT_CERTS enforcement.

## SFB_SECURE_BOOT_PROTOCOL (for EFI applications)

The BDS publishes `SFB_SECURE_BOOT_PROTOCOL`
(`QcomModulePkg/Include/Protocol/SfbSecureBoot.h`, GUID
`2e7c4a19-d5b0-4c67-9a31-860f4d2b71e3`) on its own handle once the
certificate classes are loaded, before any entry launches. Any application
can locate it:

```c
#include <Protocol/SfbSecureBoot.h>

SFB_SECURE_BOOT_PROTOCOL *Sb;
if (!EFI_ERROR (gBS->LocateProtocol (&gSfbSecureBootProtocolGuid, NULL,
                                     (VOID **)&Sb))) {
  Print (L"verified boot: %u, privileged: %u\n",
         Sb->IsVerifiedBoot (Sb), Sb->IsPrivMode (Sb));
}
```

- `IsVerifiedBoot()` - TRUE when BOOT_CERTS is non-empty and every launched
  EFI image is being verified. Independent of privilege.
- `IsPrivMode()` - TRUE when the session is privileged (no AUTH_CERTS, or a
  successful challenge response). FALSE while locked; resets on reboot.

The values are live queries against the BDS security state, so a successful
`fastboot_unlock.sh` is reflected immediately. Add
`gSfbSecureBootProtocolGuid` to the consuming module's `[Protocols]`
section.
