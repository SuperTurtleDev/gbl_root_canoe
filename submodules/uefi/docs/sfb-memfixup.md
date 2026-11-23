# Memory fix-ups for EFI-stub kernels

The BDS installs two platform fix-ups once per boot session, as the first
step of `LinuxLoaderEntry` before the storage stack, the config store and the
menu-key scan (`SfbMemFixupInstall()`, library
`QcomModulePkg/Library/SfbMemFixupLib`). They depend only on firmware
protocols present at entry and stay installed for every launch path: stored
entries, the boot menu and fastboot. There is no switch: both are
required for a mainline Linux kernel started through its EFI stub
(`LoadImage()`/`StartImage()` on the kernel `Image`) to boot on this
platform, and both are harmless for every other payload.

## EFI_MEMORY_ATTRIBUTE_PROTOCOL

**Problem.** Freshly allocated `EfiLoaderCode` pages are mapped execute-never
by the platform firmware, and the firmware does not publish the UEFI 2.10
`EFI_MEMORY_ATTRIBUTE_PROTOCOL` a loader would use to change that. The arm64
EFI stub (Linux 6.3 and later) relocates the kernel into such pages and jumps
there with the MMU still on, so the first kernel instruction faults right
after `ExitBootServices()`: no console, no panic, a silent reset.

**Fix.** The BDS publishes the protocol itself
(`QcomModulePkg/Include/Protocol/EFIMemoryAttribute.h`, standard GUID
`f4560cf6-40ec-4b4a-a192-bf1d57d0b189`) with standard semantics:

- `GetMemoryAttributes()` reports the `RP`/`RO`/`XP` attributes the live
  stage-1 translation tables apply to the range, `EFI_NO_MAPPING` when they
  are not uniform. The tables are read directly (TTBR0 of the current
  exception level, 4 KB granule, identity mapped as UEFI requires). GCD is
  not consulted for protection bits: the DXE core applies its NX policy to
  the tables without recording it there, so GCD is stale in both directions.
- `SetMemoryAttributes()` / `ClearMemoryAttributes()` merge the request into
  the attributes read from the tables, write them through
  `EFI_CPU_ARCH_PROTOCOL.SetMemoryAttributes()` (the only page-table writer
  the firmware exports; it replaces the whole attribute set, so the GCD
  cache type travels with the protection bits, as the DXE core's image
  protection does), then read the tables back. A result that does not match
  the request fails with `EFI_UNSUPPORTED`.

Parameter checking follows the specification (page alignment, non-zero
length, only `RP`/`RO`/`XP`). The implementation mirrors ArmPkg CpuDxe's
`MemoryAttribute.c`, which also answers from the live tables.

At install time the walker is validated against the BDS's own code page; if
the firmware's tables cannot be read (non-4 KB granule, unexpected layout)
the protocol is not published rather than published wrong. If the firmware
already publishes the protocol, the BDS leaves it alone.

## GetMemoryMap merge

**Problem.** The firmware memory map describes only the DDR the boot stages
themselves touched and keeps TZ-usable ranges as `RESERVED`. The TZ RAM
partition table (`EFI_RAMPARTITION_PROTOCOL`) is the authority on what the
application processor may use. An EFI-booted kernel builds memblock from the
UEFI map, not from the device tree `/memory` node, so without this it runs
on a fraction of the installed RAM (about 0.7 GiB of the omitted memory plus
0.8 GiB of released `RESERVED` memory on the reference SM8850 device).

**Fix.** `gBS->GetMemoryMap` is wrapped. Every caller sees:

1. **release** - `RESERVED` descriptors that the firmware itself records as
   normal cacheable memory (`EFI_MEMORY_WB` set) are split against the TZ
   list; the parts inside a TZ partition come back as `CONVENTIONAL` with
   their attributes kept, the rest stays `RESERVED`.
2. **add** - TZ ranges the firmware map does not describe at all are appended
   as new `CONVENTIONAL` entries with `UC|WC|WT|WB`.

A `RESERVED` descriptor **without** `WB` is passed through untouched. The
firmware registers memory it shares with other masters or uses as
device-like storage as UC-only (SMEM, hypervisor and TZ runtime areas); that
is a statement that the range is not RAM for the OS regardless of what the
TZ table says about the DDR behind it. On the reference device these are the
`0x81Cxxxxx`, `0x8AA00000-0xA7CFFFFF` and `0xD78xxxxx` entries; handing them
to the kernel as WB RAM produced an in-kernel undefined-instruction oops as
soon as init started (the first UEFI runtime-service calls). `RUNTIME_*`,
`MMIO`, ACPI entries and anything carrying `EFI_MEMORY_RUNTIME` are never
touched either.

The firmware allocator does not know the added ranges, so no later allocation
can land there and the view is stable. The `MapKey` is the firmware's own.
Nothing allocates between a caller's `GetMemoryMap()` and its
`ExitBootServices()`: the chained call lands in a pre-sized scratch buffer
that only grows on size-query calls, after which the caller allocates its
own buffer anyway. The view is recomputed from the live map on every call.

The kernel still honours the device tree `/reserved-memory` `no-map`
carveouts: `early_init_fdt_scan_reserved_mem()` re-reserves them over
anything the UEFI map hands over.

## Verification

The interval arithmetic is pure and host-tested:

```sh
QcomModulePkg/Library/SfbMemFixupLib/Test/test.sh
```

runs edge cases plus a regression against real device dumps
(`Test/fixtures/memmap.txt`, `Test/fixtures/tzmap.txt`, fetched over
fastboot) and checks that release + add + already-usable + other-described
partitions the TZ total exactly.

On the device, a kernel booted with `efi=debug` prints the merged map from
the stub, and `dmesg | grep -i 'efi: mem'` shows the added `Conventional`
ranges. The protocol path is exercised by the stub itself; a kernel that
previously reset silently after "Exiting boot services..." now reaches its
console.
