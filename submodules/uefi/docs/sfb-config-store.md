# BDS boot entries and the efisp config store

This describes how the super-fastboot BDS (`build/BDS.efi`) stores its boot
entries, where they come from, and in which order they are tried.

## The efisp partition

The `efisp` GPT partition holds two things:

| Range (from the GPT entry, `EndingLBA` inclusive) | Content |
| --- | --- |
| `StartingLBA` up to `StartingLBA + 1 MiB / BlockSize - 1` | `BDS.efi`, a plain PE image |
| `StartingLBA + 1 MiB / BlockSize` up to `EndingLBA` | one FAT volume (the config container), no partition table |

- The build does not pad `BDS.efi`. `make build` fails if the image reaches 1 MiB.
- Flash BDS by its own length only, for example
  `dd if=BDS.efi of=<efisp> bs=4096 conv=notrunc,fsync`. A raw write of the
  file never touches the container behind it.
- `fastboot erase efisp`, or any write of a full-partition image, wipes the
  container. BDS then formats a fresh, empty one on the next boot.
- BDS reaches the container only through a 512-byte Block I/O bounded to
  exactly that LBA range. No write through it can land before the 1 MiB mark
  or past `EndingLBA`.

On every boot BDS mounts the container. If the FAT driver cannot mount it and
its first sector is not a usable FAT boot sector, BDS formats it, as FAT12,
FAT16 or FAT32 depending on size, then mounts it again. If that still fails,
BDS boots on without persistent storage. A container that holds a valid-looking
FAT boot sector is never formatted automatically, even if mounting it failed.

## Boot entries

The stored entry list is the file `CONFIG\ENTRIES.CFG` in the container. It
holds one entry per line, and line order is boot order. There is no default
entry: an unattended boot tries the first entry, then the next, and so on.
The file is created at runtime and is not shipped in any package.

### Line format

```
<name>:[(<disk>,<partition>)]<absolute path on the volume> [arguments]
```

```
Android Tools\Reboot Tools:(1,1)\efisp\tools\RebootTools.efi
Android Tools\Reboot Tools:\efisp\tools\RebootTools.efi
```

- A `\` in the name nests the entry in the menu. The lines above appear as
  "Android Tools >" with "Reboot Tools" inside.
- `(d,p)` is the p-th partition of the d-th whole disk that UEFI enumerates.
  Both numbers are 0-based, and p is the GPT entry number minus one.
- Without `(d,p)` the path is on the volume that holds the ENTRIES.CFG being
  read.
- `/` is accepted in place of `\` in the path.
- Anything after the first blank following the path is passed to the image as
  its LoadOptions.
- Blank lines and lines starting with `#` are ignored.
- The old `$` and `%` markers, `SFB1|...` records and volume-label matching are
  gone.

### Where entries come from

The scan visits every FAT and ext4 volume that is a GPT partition:

1. If `\EFI\BOOT\BOOTAA64.EFI` exists, the scan adds an entry named after the
   volume label and its address, for example `EFIBOOT (0,2)`. The image BDS
   itself was started from is skipped.
2. If the volume root holds `ENTRIES.CFG`, each line whose file exists is added.
   Lines without `(d,p)` are recorded with that volume's address.

The scan then syncs with the stored list:

- Stored entries whose file is gone are removed.
- The remaining entries keep their order.
- Newly found entries are appended at the end.

The result is written back to `CONFIG\ENTRIES.CFG` atomically. BDS writes a
temporary file, flushes it, deletes the old file, renames the new one into
place and flushes again.

### Power-on sequence

1. Load `CONFIG\ENTRIES.CFG`. A missing file or an unavailable store gives an
   empty list.
2. If the list is empty, scan, sync and store.
3. If it is still empty, show the boot menu.
4. If the menu key (Volume Up) was pressed, scan, sync and store, then show
   the menu.
5. Otherwise try every entry in order. When LoadImage rejects a file as not a
   PE/COFF image, or as built for another architecture, that entry is deleted
   and the list is stored again. Missing files, security refusals, low memory
   and other errors keep the entry, and BDS moves on to the next one.
6. If every entry fails, show the boot menu.

The boot menu also rescans and stores whenever it opens.

### Advanced menu

- **Boot Entries Priority**: pick an entry, move it with Volume Up and Volume
  Down, and confirm with Power. The new order is stored.
- **Boot Entries Management**: pick an entry and confirm to delete it. The
  list is stored.
- **USB Mass Storage > Export Config Store**: the container is unmounted and
  exported behind a synthesized GPT, with one ESP-typed partition covering it,
  so the host sees an ordinary FAT partition. It is mounted again afterwards,
  and the stored list is re-read in case the host edited it.

A deleted entry whose file a volume still offers comes back the next time a
scan runs. That happens after an empty list, on the menu key, or when the menu
opens. To remove it for good, remove the file or its `ENTRIES.CFG` line.

## Security certificates

The container also holds the security certificate classes, as X.509 DER
(`.cer`) files classified by directory:

| Directory | Class | Effect when non-empty |
| --- | --- | --- |
| `AUTH_CERTS` | A - fastboot authorization | Locked mode until a challenge response verifies |
| `BOOT_CERTS` | B - EFI code signing | Every launched EFI must carry a verifiable signature |

Both features are purely certificate-driven: an empty (or missing) directory
means the feature is off and BDS behaves exactly as before. See
[sfb-security.md](sfb-security.md) for the full model - the challenge
protocol, the fastboot command gating and the signing tools under
`tools/sfbsec/` in the top-level repository.
