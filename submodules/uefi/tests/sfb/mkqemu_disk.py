#!/usr/bin/env python3
"""Build the single-disk QEMU test image for the BDS config store.

Layout (512-byte sectors, one GPT disk, so (d,p) is always (0,p)):

  (0,0) esp      8 MiB  FAT16, \\EFI\\BOOT\\BOOTAA64.EFI = BDS.efi (firmware boots it)
  (0,1) efisp    3 MiB  BDS.efi in the first 1 MiB (written by length only),
                        container after it left blank so BDS formats it on
                        first boot
  (0,2) efiboot 40 MiB  FAT32, root ENTRIES.CFG, optional test application

No mkfs.fat or mtools: the FAT writer below is self-contained.
"""
import argparse
import struct
import sys
import uuid
import zlib

SECTOR = 512
MIB = 1024 * 1024
BDS_RESERVED = MIB

ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
BASIC_DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
NAMESPACE = uuid.UUID("5f1a3c27-9b04-4c6e-a13d-702e4b95c816")

DEFAULT_ENTRIES = (
    "# QEMU single disk: (0,0)=esp (0,1)=efisp (0,2)=efiboot\n"
    "Tests\\Hello (this volume):\\EFI\\TOOLS\\HELLO.EFI\n"
    "Tests\\Hello (by address):(0,2)\\EFI\\TOOLS\\HELLO2.EFI\n"
    "Tests\\Not a PE image:\\TOOLS\\NOTPE.EFI\n"
    "Tests\\Missing file:\\TOOLS\\MISSING.EFI\n"
)

# ---- FAT ------------------------------------------------------------------


def short_name(name):
    """The 8.3 short name: the name itself when it fits, else an AUTH_C~1
    style alias (carried alongside LFN entries)."""
    base, _, ext = name.upper().partition(".")
    base = "".join(c for c in base if c not in ' ."*+,/:;<=>?\\[]|')
    ext = "".join(c for c in ext if c not in ' ."*+,/:;<=>?\\[]|')
    if 1 <= len(base) <= 8 and len(ext) <= 3:
        return (base.ljust(8) + ext.ljust(3)).encode("ascii")
    stub, alias, n = base[:6], None, 1
    while alias is None:
        cand = "%s~%d" % (stub, n)
        if len(cand) <= 8:
            alias = (cand.ljust(8) + ext[:3].ljust(3)).encode("ascii")
        else:
            stub, n = stub[:-1], n
    return alias


def _lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entries(name, short):
    """Long-file-name directory entries for name (VFAT LFN, UCS-2).

    Layout per the FAT specification: ordinal at 0, chars 1-5 at 1-10,
    attribute 0x0F at 11, type 0 at 12, short-name checksum at 13, chars
    6-11 at 14-25, cluster 0 at 26-27, chars 12-13 at 28-31. The last
    logical entry carries the 0x40 flag and the name is null-terminated
    then padded with 0xFFFF.
    """
    ucs = name.upper().encode("utf-16-le") + b"\x00\x00"
    ucs += b"\xff\xff" * ((-len(ucs) // 2) % 13)
    n = len(ucs) // 26
    ck = _lfn_checksum(short)
    out = b""
    for i in range(n, 0, -1):
        chunk = ucs[(i - 1) * 26:i * 26]
        e = bytearray(32)
        e[0] = i | (0x40 if i == n else 0)
        e[1:11] = chunk[0:10]
        e[11] = 0x0F
        e[12] = 0
        e[13] = ck
        e[14:26] = chunk[10:22]
        e[26:28] = b"\x00\x00"
        e[28:32] = chunk[22:26]
        out += bytes(e)
    return out


class FatWriter:
    """Minimal FAT12/16/32 image writer: 8.3 names, contiguous allocation."""

    def __init__(self, sectors, fat_type, spc, label):
        self.sectors, self.type, self.spc = sectors, fat_type, spc
        self.label = label.upper().ljust(11)[:11].encode("ascii")
        self.reserved = 32 if fat_type == 32 else 1
        self.root_entries = 0 if fat_type == 32 else 512
        self.root_secs = self.root_entries * 32 // SECTOR
        fat_secs = 1
        while True:
            data = sectors - self.reserved - 2 * fat_secs - self.root_secs
            clusters = data // spc
            if fat_type == 12:
                need_bytes = ((clusters + 2) * 3 + 1) // 2
            else:
                need_bytes = (clusters + 2) * fat_type // 8
            need = -(-need_bytes // SECTOR)
            if need <= fat_secs:
                break
            fat_secs = need
        self.fat_secs, self.clusters = fat_secs, clusters
        kind = 12 if clusters <= 4084 else 16 if clusters <= 65524 else 32
        if kind != fat_type:
            raise ValueError("%d sectors give FAT%d, not FAT%d" % (sectors, kind, fat_type))
        self.data_start = self.reserved + 2 * fat_secs + self.root_secs
        self.tree = {}

    def add(self, path, data=None):
        """Add a file (data bytes) or a directory (data None) at a \\ path."""
        node = self.tree
        parts = [p for p in path.replace("/", "\\").split("\\") if p]
        for part in parts[:-1]:
            node = node.setdefault(part.upper(), {})
        node[parts[-1].upper()] = {} if data is None else bytes(data)

    def build(self):
        img = bytearray(self.sectors * SECTOR)
        fat = [0] * (self.clusters + 2)
        fat[0] = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.type]
        fat[1] = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[self.type]
        eoc = fat[1]
        cluster_bytes = self.spc * SECTOR
        state = {"next": 2}

        def alloc(nbytes):
            count = max(1, -(-nbytes // cluster_bytes))
            first = state["next"]
            if first + count > self.clusters + 2:
                raise ValueError("image full")
            for c in range(first, first + count):
                fat[c] = c + 1 if c < first + count - 1 else eoc
            state["next"] += count
            return first

        def coff(cluster):
            return (self.data_start + (cluster - 2) * self.spc) * SECTOR

        def entry(name, attr, cluster, size):
            return (name + bytes([attr, 0]) + b"\0" * 7 +
                    struct.pack("<H", cluster >> 16) + b"\0" * 4 +
                    struct.pack("<HI", cluster & 0xFFFF, size))

        def write_dir(node, self_cluster, parent_cluster, area):
            entries = []
            if self_cluster is not None:
                entries.append(entry(b".          ", 0x10, self_cluster, 0))
                entries.append(entry(b"..         ", 0x10, parent_cluster, 0))
            else:
                entries.append(self.label + bytes([0x08]) + b"\0" * 20)
            for name in sorted(node):
                child = node[name]
                sn = short_name(name)
                upper = name.upper()
                plain = sn == ((upper.partition(".")[0].ljust(8) +
                                upper.partition(".")[2].ljust(3)).encode("ascii"))
                lfn = b"" if plain else lfn_entries(name, sn)
                if isinstance(child, dict):
                    # Room for ".": ".." plus every child's short entry and
                    # any LFN entries (all multiples of 32 bytes).
                    size = (2 + sum(
                        1 + (0 if short_name(n) == (
                            n.upper().partition(".")[0].ljust(8) +
                            n.upper().partition(".")[2].ljust(3)).encode("ascii")
                        else len(lfn_entries(n, short_name(n))) // 32)
                        for n in child)) * 32
                    cl = alloc(size)
                    sub_parent = 0 if self_cluster is None else self_cluster
                    write_dir(child, cl, sub_parent, None)
                    entries.append(lfn + entry(sn, 0x10, cl, 0))
                else:
                    cl = alloc(len(child)) if child else 0
                    if child:
                        img[coff(cl):coff(cl) + len(child)] = child
                    entries.append(lfn + entry(sn, 0x20, cl, len(child)))
            blob = b"".join(entries)
            if area is None:
                area = coff(self_cluster)
            img[area:area + len(blob)] = blob

        if self.type == 32:
            root = alloc((len(self.tree) + 1) * 32)
            write_dir(self.tree, None, None, coff(root))
        else:
            root = 0
            write_dir(self.tree, None, None, (self.reserved + 2 * self.fat_secs) * SECTOR)

        # Boot sector.
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90" if self.type == 32 else b"\xEB\x3C\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBH", bs, 11, SECTOR, self.spc, self.reserved, 2, self.root_entries)
        if self.type != 32 and self.sectors < 0x10000:
            struct.pack_into("<H", bs, 19, self.sectors)
        else:
            struct.pack_into("<I", bs, 32, self.sectors)
        bs[21] = 0xF8
        struct.pack_into("<HH", bs, 24, 63, 255)
        if self.type == 32:
            struct.pack_into("<IHHIHH", bs, 36, self.fat_secs, 0, 0, root, 1, 6)
            ext = 64
        else:
            struct.pack_into("<H", bs, 22, self.fat_secs)
            ext = 36
        bs[ext] = 0x80
        bs[ext + 2] = 0x29
        struct.pack_into("<I", bs, ext + 3, 0x51E00000 | self.type)
        bs[ext + 7:ext + 18] = self.label
        bs[ext + 18:ext + 26] = ("FAT%d   " % self.type).encode("ascii")
        bs[510:512] = b"\x55\xAA"
        img[0:SECTOR] = bs

        if self.type == 32:
            info = bytearray(SECTOR)
            struct.pack_into("<I", info, 0, 0x41615252)
            struct.pack_into("<III", info, 484, 0x61417272,
                             self.clusters - (state["next"] - 2), state["next"])
            struct.pack_into("<I", info, 508, 0xAA550000)
            img[SECTOR:2 * SECTOR] = info
            img[6 * SECTOR:7 * SECTOR] = bs
            img[7 * SECTOR:8 * SECTOR] = info

        # FATs.
        if self.type == 12:
            raw = bytearray()
            for i in range(0, len(fat), 2):
                a = fat[i]
                b = fat[i + 1] if i + 1 < len(fat) else 0
                raw += struct.pack("<I", a | (b << 12))[:3]
        elif self.type == 16:
            raw = struct.pack("<%dH" % len(fat), *fat)
        else:
            raw = struct.pack("<%dI" % len(fat), *fat)
        for copy in range(2):
            off = (self.reserved + copy * self.fat_secs) * SECTOR
            img[off:off + len(raw)] = raw
        return bytes(img)


class FatReader:
    """Just enough FAT reading to verify images in the tests."""

    def __init__(self, img):
        self.img = img
        bps, self.spc, self.reserved, self.nfats, self.root_entries = struct.unpack_from("<HBHBH", img, 11)
        total = struct.unpack_from("<H", img, 19)[0] or struct.unpack_from("<I", img, 32)[0]
        fat16 = struct.unpack_from("<H", img, 22)[0]
        self.fat_secs = fat16 or struct.unpack_from("<I", img, 36)[0]
        self.root_secs = (self.root_entries * 32 + bps - 1) // bps
        self.data_start = self.reserved + self.nfats * self.fat_secs + self.root_secs
        self.clusters = (total - self.data_start) // self.spc
        self.type = 12 if self.clusters <= 4084 else 16 if self.clusters <= 65524 else 32
        self.root_cluster = struct.unpack_from("<I", img, 44)[0] if self.type == 32 else 0
        self.total = total

    def fat_entry(self, n):
        base = self.reserved * SECTOR
        if self.type == 12:
            v = struct.unpack_from("<H", self.img, base + n * 3 // 2)[0]
            return (v >> 4) if n & 1 else (v & 0xFFF)
        if self.type == 16:
            return struct.unpack_from("<H", self.img, base + n * 2)[0]
        return struct.unpack_from("<I", self.img, base + n * 4)[0] & 0x0FFFFFFF

    def chain(self, first):
        data, c = b"", first
        eoc = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.type]
        while 2 <= c < eoc:
            off = (self.data_start + (c - 2) * self.spc) * SECTOR
            data += self.img[off:off + self.spc * SECTOR]
            c = self.fat_entry(c)
        return data

    def entries(self, cluster):
        if cluster == 0 and self.type != 32:
            off = (self.reserved + self.nfats * self.fat_secs) * SECTOR
            raw = self.img[off:off + self.root_secs * SECTOR]
        else:
            raw = self.chain(cluster or self.root_cluster)
        out = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            if e[11] & 0x08:
                # A volume label is one 11-character field, not 8.3.
                out.append((e[0:11].decode().rstrip(), e[11], 0, 0))
                continue
            name = e[0:8].decode().rstrip()
            ext = e[8:11].decode().rstrip()
            cl = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            out.append(((name + "." + ext) if ext else name, e[11], cl, struct.unpack_from("<I", e, 28)[0]))
        return out

    def read(self, path):
        cluster = 0
        parts = [p for p in path.upper().split("\\") if p]
        for i, part in enumerate(parts):
            for name, attr, cl, size in self.entries(cluster):
                if name == part:
                    if i == len(parts) - 1:
                        return None if attr & 0x10 else self.chain(cl)[:size]
                    cluster = cl
                    break
            else:
                raise FileNotFoundError(path)
        return None

    def label(self):
        for name, attr, _, _ in self.entries(0):
            if attr & 0x08:
                return name
        return ""


# ---- GPT ------------------------------------------------------------------


def gpt_disk(total_sectors, parts):
    """parts: list of (name, type uuid, first lba, last lba, data bytes)."""
    img = bytearray(total_sectors * SECTOR)
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1,
                               min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    table = bytearray(128 * 128)
    for i, (name, ptype, first, last, data) in enumerate(parts):
        rec = (ptype.bytes_le + uuid.uuid5(NAMESPACE, name).bytes_le +
               struct.pack("<QQQ", first, last, 0) + name.encode("utf-16-le").ljust(72, b"\0"))
        table[i * 128:(i + 1) * 128] = rec
        if len(data) > (last - first + 1) * SECTOR:
            raise ValueError("%s data does not fit" % name)
        img[first * SECTOR:first * SECTOR + len(data)] = data
    last_lba = total_sectors - 1
    entries_crc = zlib.crc32(table) & 0xFFFFFFFF

    def header(my, alt, entries_lba):
        h = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x00010000, 92, 0, 0,
                         my, alt, 34, last_lba - 33, uuid.uuid5(NAMESPACE, "disk").bytes_le,
                         entries_lba, 128, 128, entries_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")

    img[SECTOR:2 * SECTOR] = header(1, last_lba, 2)
    img[2 * SECTOR:34 * SECTOR] = table
    img[(last_lba - 32) * SECTOR:last_lba * SECTOR] = table
    img[last_lba * SECTOR:] = header(last_lba, 1, last_lba - 32)
    return bytes(img)


def build(bds, app, entries_text, container_files=None):
    if len(bds) >= BDS_RESERVED:
        raise ValueError("BDS.efi is %d bytes; it must be below 1 MiB" % len(bds))

    esp = FatWriter(8 * MIB // SECTOR, 16, 2, "ESP")
    esp.add("EFI\\BOOT\\BOOTAA64.EFI", bds)

    efiboot = FatWriter(40 * MIB // SECTOR, 32, 1, "EFIBOOT")
    efiboot.add("ENTRIES.CFG", entries_text.encode("ascii"))
    efiboot.add("TOOLS\\NOTPE.EFI", b"This is a text file, not a PE/COFF image.\n")
    if app is not None:
        efiboot.add("EFI\\BOOT\\BOOTAA64.EFI", app)
        efiboot.add("EFI\\TOOLS\\HELLO.EFI", app)
        efiboot.add("EFI\\TOOLS\\HELLO2.EFI", app)

    # efisp = 1 MiB BDS image + the 2 MiB config container. Without
    # container_files the container area stays blank and the BDS formats it
    # on first boot; with them the container is built here (FAT12, matching
    # SfbFatPlan for 4096 sectors) so tests can ship AUTH_CERTS/BOOT_CERTS.
    efisp_data = bds
    if container_files:
        container = FatWriter(2 * MIB // SECTOR, 12, 1, "SFBCONFIG")
        for path, data in container_files:
            container.add(path, data)
        efisp_data = bds.ljust(BDS_RESERVED, b"\0") + container.build()

    first = 2048
    layout = []
    for name, ptype, size, data in (("esp", ESP_TYPE, 8 * MIB, esp.build()),
                                    ("efisp", BASIC_DATA_TYPE, 3 * MIB, efisp_data),
                                    ("efiboot", BASIC_DATA_TYPE, 40 * MIB, efiboot.build())):
        count = size // SECTOR
        layout.append((name, ptype, first, first + count - 1, data))
        first += count
    total = first + 2048          # 1 MiB tail, room for the backup GPT
    return gpt_disk(total, layout), layout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bds", required=True, help="build/BDS.efi")
    ap.add_argument("--app", help="optional AArch64 EFI application for efiboot")
    ap.add_argument("--entries", help="ENTRIES.CFG for efiboot (default: built-in test list)")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    bds = open(args.bds, "rb").read()
    app = open(args.app, "rb").read() if args.app else None
    text = open(args.entries).read() if args.entries else DEFAULT_ENTRIES
    image, layout = build(bds, app, text)
    with open(args.output, "wb") as f:
        f.write(image)
    for name, _, start, end, _ in layout:
        print("%-8s LBA %6d..%6d (%d MiB)" % (name, start, end, (end - start + 1) * SECTOR // MIB))
    return 0


if __name__ == "__main__":
    sys.exit(main())
