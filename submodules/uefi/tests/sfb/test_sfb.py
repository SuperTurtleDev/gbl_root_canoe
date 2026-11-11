#!/usr/bin/env python3
"""Host tests for the BDS config store, boot entries, USB export and the QEMU
test image. Run from anywhere:  python3 tests/sfb/test_sfb.py -v

The C half (host/test_sfb.c) compiles the real firmware sources against the
in-tree MdePkg headers and leaves raw images behind; this file checks those
images independently, checks the build wiring, and checks the QEMU disk
layout. fsck.fat and sfdisk are used as extra witnesses when installed.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
UEFI = os.path.normpath(os.path.join(HERE, "..", ".."))
EDK2 = os.path.join(UEFI, "edk2")
LOADER = os.path.join(EDK2, "QcomModulePkg", "Application", "LinuxLoader")
STORELIB = os.path.join(EDK2, "QcomModulePkg", "Library", "SfbConfigStoreLib")
sys.path.insert(0, HERE)
import mkqemu_disk  # noqa: E402

SECTOR = 512
MIB = 1024 * 1024
ESP_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")

ART = None


def setUpModule():
    global ART
    ART = tempfile.mkdtemp(prefix="sfb-test-")
    exe = os.path.join(ART, "test_sfb")
    subprocess.run([os.path.join(HERE, "build_host_tests.sh"), exe], check=True)
    run = subprocess.run([exe, ART], capture_output=True, text=True)
    sys.stderr.write(run.stdout + run.stderr)
    if run.returncode != 0:
        raise AssertionError("host C tests failed")


def tearDownModule():
    if ART and not os.environ.get("SFB_KEEP_ARTIFACTS"):
        shutil.rmtree(ART, ignore_errors=True)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def tool(name):
    return shutil.which(name) or (os.path.exists("/usr/sbin/" + name) and "/usr/sbin/" + name)


def parse_gpt_header(blk):
    (sig, rev, size, crc, _, my, alt, first, last, guid, elba, count, esize,
     ecrc) = struct.unpack_from("<8sIIIIQQQQ16sQIII", blk, 0)
    h = bytearray(blk[:size])
    struct.pack_into("<I", h, 16, 0)
    return dict(sig=sig, size=size, crc_ok=(zlib.crc32(h) & 0xFFFFFFFF) == crc, my=my,
                alt=alt, first=first, last=last, elba=elba, count=count, esize=esize, ecrc=ecrc)


def check_gpt(test, img, sector=SECTOR):
    """Return the partition entries after verifying both headers and CRCs."""
    total = len(img) // sector
    test.assertEqual(img[510:512], b"\x55\xAA")
    test.assertEqual(img[450], 0xEE)
    test.assertEqual(struct.unpack_from("<I", img, 454)[0], 1)
    test.assertEqual(struct.unpack_from("<I", img, 458)[0], min(total - 1, 0xFFFFFFFF))
    primary = parse_gpt_header(img[sector:2 * sector])
    backup = parse_gpt_header(img[(total - 1) * sector:])
    for h in (primary, backup):
        test.assertEqual(h["sig"], b"EFI PART")
        test.assertTrue(h["crc_ok"])
    test.assertEqual((primary["my"], primary["alt"]), (1, total - 1))
    test.assertEqual((backup["my"], backup["alt"]), (total - 1, 1))
    tables = []
    for h in (primary, backup):
        raw = img[h["elba"] * sector:h["elba"] * sector + h["count"] * h["esize"]]
        test.assertEqual(zlib.crc32(raw) & 0xFFFFFFFF, h["ecrc"])
        tables.append(raw)
    test.assertEqual(tables[0], tables[1])
    test.assertEqual(primary["first"], backup["first"])
    test.assertEqual(primary["last"], backup["last"])
    parts = []
    for i in range(primary["count"]):
        rec = tables[0][i * primary["esize"]:(i + 1) * primary["esize"]]
        if rec[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", rec, 32)
        test.assertGreaterEqual(first, primary["first"])
        test.assertLessEqual(last, primary["last"])
        parts.append(dict(type=uuid.UUID(bytes_le=rec[:16]), first=first, last=last,
                          name=rec[56:128].decode("utf-16-le").rstrip("\0")))
    return parts, primary


class ExportTest(unittest.TestCase):
    """USB export: protective MBR + GPT around exactly the container."""

    def test_gpt_wraps_container(self):
        img = read(os.path.join(ART, "export.img"))
        parts, hdr = check_gpt(self, img)
        self.assertEqual(len(parts), 1)
        p = parts[0]
        self.assertEqual(p["type"], ESP_GUID)
        self.assertEqual(p["name"], "efisp")
        # 2 MiB container, 512-byte sectors, standard 34-sector GPT head.
        self.assertEqual((p["first"], p["last"]), (34, 34 + 4096 - 1))
        self.assertEqual((hdr["first"], hdr["last"]), (p["first"], p["last"]))
        fat = mkqemu_disk.FatReader(img[34 * SECTOR:(34 + 4096) * SECTOR])
        self.assertEqual(fat.type, 12)
        self.assertEqual(fat.total, 4096)

    def test_sfdisk_agrees(self):
        sfdisk = tool("sfdisk")
        if not sfdisk:
            self.skipTest("sfdisk not installed")
        out = subprocess.run([sfdisk, "-V", os.path.join(ART, "export.img")],
                             capture_output=True, text=True)
        self.assertIn("No errors detected", out.stdout)


class FormatTest(unittest.TestCase):
    """The embedded formatter's volumes, parsed independently."""

    def load(self, name):
        off, length, block, want = map(int, read(os.path.join(ART, name + ".meta")).split())
        with open(os.path.join(ART, name), "rb") as f:
            f.seek(off)
            return f.read(min(length, 8 * MIB)), length, block, want, off

    def check(self, name):
        data, length, block, want, off = self.load(name)
        fat = mkqemu_disk.FatReader(data)
        self.assertEqual(fat.type, want)
        self.assertEqual(fat.total * SECTOR, length)
        self.assertEqual(struct.unpack_from("<H", data, 11)[0], 512)
        self.assertEqual(data[21], 0xF8)
        self.assertEqual(fat.fat_entry(0) & 0xFF, 0xF8)
        self.assertEqual(fat.label(), "SFBCONFIG")
        fsz = fat.fat_secs * SECTOR
        first = data[fat.reserved * SECTOR:fat.reserved * SECTOR + min(fsz, 4096)]
        second = data[(fat.reserved * SECTOR + fsz):(fat.reserved * SECTOR + fsz + len(first))]
        self.assertEqual(first, second)
        # The container starts exactly 1 MiB into the partition.
        self.assertEqual(off % block, 0)
        if want == 32:
            self.assertEqual(struct.unpack_from("<I", data, SECTOR)[0], 0x41615252)
            self.assertEqual(data[6 * SECTOR:7 * SECTOR], data[:SECTOR])
        fsck = tool("fsck.fat")
        if fsck:
            path = os.path.join(ART, name + ".vol")
            with open(os.path.join(ART, name), "rb") as src, open(path, "wb") as dst:
                src.seek(off)
                remaining = length
                while remaining:
                    chunk = src.read(min(remaining, 4 * MIB))
                    dst.write(chunk)
                    remaining -= len(chunk)
            out = subprocess.run([fsck, "-n", path], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            os.unlink(path)

    def test_fat12_qemu_size(self):
        self.check("fat12.img")

    def test_fat16(self):
        self.check("fat16.img")

    def test_fat32(self):
        self.check("fat32.img")


class BuildWiringTest(unittest.TestCase):
    """LinuxLoader.inf, the dsc and the library agree; legacy code is gone."""

    def sources(self, inf):
        text = read(inf).decode()
        section = re.search(r"\[Sources\](.*?)\n\[", text, re.S).group(1)
        return [l.strip() for l in section.splitlines() if l.strip() and not l.strip().startswith("#")]

    def test_loader_inf(self):
        inf = os.path.join(LOADER, "LinuxLoader.inf")
        srcs = self.sources(inf)
        for s in srcs:
            self.assertTrue(os.path.exists(os.path.join(LOADER, s)), s)
        for s in ("SuperFbEntryList.c", "SuperFbEntryList.h", "SuperFbEntries.c"):
            self.assertIn(s, srcs)
        for gone in ("SuperFbStore.c", "SuperFbImageDisk.c", "SuperFbImageDisk.h"):
            self.assertNotIn(gone, srcs)
            self.assertFalse(os.path.exists(os.path.join(LOADER, gone)), gone)
        self.assertRegex(read(inf).decode(), r"\n\s*SfbConfigStoreLib\s*\n")

    def test_library(self):
        inf = os.path.join(STORELIB, "SfbConfigStoreLib.inf")
        srcs = self.sources(inf)
        self.assertEqual(sorted(srcs), sorted(["SfbConfigStore.c", "SuperFbConfigDisk.h",
                                               "SuperFbConfigDisk.c", "SuperFbConfigFormat.h",
                                               "SuperFbConfigFormat.c"]))
        for s in srcs:
            self.assertTrue(os.path.exists(os.path.join(STORELIB, s)), s)
        dsc = read(os.path.join(EDK2, "QcomModulePkg", "QcomModulePkg.dsc")).decode()
        self.assertIn("SfbConfigStoreLib|QcomModulePkg/Library/SfbConfigStoreLib/SfbConfigStoreLib.inf", dsc)
        header = read(os.path.join(EDK2, "QcomModulePkg", "Include", "Library", "SfbConfigStore.h")).decode()
        public = re.findall(r"^(SfbConfigStore\w+)\s*\(", header, re.M)
        self.assertEqual(public, ["SfbConfigStoreMount", "SfbConfigStoreUnmount",
                                  "SfbConfigStoreGetFs", "SfbConfigStoreExportUsb"])

    def test_legacy_symbols_gone(self):
        legacy = re.compile(r"SfbStoreRead|SfbStoreWrite|SFB_STORE_SLOT|SFB_STORE_DEFAULT|"
                            r"SFB_STORE_CUSTOM|SFB_RECORD_TAG|SfbFatBlob|Ext4MapImage|"
                            r"Ext4OpenImageFileSystem|SfbVolumeRootPrefix|efisp\.fat|"
                            r"SfbLaunchDefaultEntry|SfbSaveDefaultEntry|SfbSaveCustomEntry|"
                            r"BOOTENTRIES|SfbEntrySubmenu")
        roots = [LOADER, STORELIB, os.path.join(EDK2, "Ext4Pkg")]
        for root in roots:
            for dirpath, _, files in os.walk(root):
                for f in files:
                    if f.endswith((".c", ".h", ".inf", ".dec")):
                        text = read(os.path.join(dirpath, f)).decode(errors="replace")
                        m = legacy.search(text)
                        self.assertIsNone(m, "%s: %s" % (os.path.join(dirpath, f), m and m.group(0)))

    def test_bds_size_guard(self):
        mk = read(os.path.join(UEFI, "Makefile")).decode()
        self.assertIn('if [ "$$size" -ge 1048576 ]', mk)
        bds = os.path.join(UEFI, "build", "BDS.efi")
        if os.path.exists(bds):
            self.assertLess(os.path.getsize(bds), MIB)


class QemuImageTest(unittest.TestCase):
    """Single GPT disk, three partitions: esp, efisp, efiboot."""

    @classmethod
    def setUpClass(cls):
        cls.bds = os.urandom(300 * 1024)
        cls.app = b"MZ" + os.urandom(4096)
        cls.img, cls.layout = mkqemu_disk.build(cls.bds, cls.app, mkqemu_disk.DEFAULT_ENTRIES)

    def part(self, index):
        p = self.layout[index]
        return self.img[p[2] * SECTOR:(p[3] + 1) * SECTOR]

    def test_layout(self):
        parts, _ = check_gpt(self, self.img)
        self.assertEqual([p["name"] for p in parts], ["esp", "efisp", "efiboot"])
        sizes = [(p["last"] - p["first"] + 1) * SECTOR for p in parts]
        self.assertEqual(sizes[:2], [8 * MIB, 3 * MIB])
        self.assertEqual(parts[0]["type"], ESP_GUID)
        for a, b in zip(parts, parts[1:]):
            self.assertLess(a["last"], b["first"])

    def test_esp_boots_bds(self):
        esp = mkqemu_disk.FatReader(self.part(0))
        self.assertEqual(esp.read("EFI\\BOOT\\BOOTAA64.EFI"), self.bds)

    def test_efisp_holds_bds_and_blank_container(self):
        efisp = self.part(1)
        self.assertEqual(efisp[:len(self.bds)], self.bds)
        # Flashed by length: nothing padded, the container is untouched blank.
        self.assertEqual(efisp[len(self.bds):], b"\0" * (len(efisp) - len(self.bds)))
        # The container BDS will compute: StartingLBA + 1 MiB .. EndingLBA.
        start, end = self.layout[1][2] + MIB // SECTOR, self.layout[1][3]
        self.assertEqual((end - start + 1) * SECTOR, 2 * MIB)

    def test_efiboot(self):
        fat = mkqemu_disk.FatReader(self.part(2))
        self.assertEqual(fat.type, 32)
        self.assertEqual(fat.label(), "EFIBOOT")
        self.assertEqual(fat.read("ENTRIES.CFG").decode(), mkqemu_disk.DEFAULT_ENTRIES)
        self.assertEqual(fat.read("EFI\\BOOT\\BOOTAA64.EFI"), self.app)
        self.assertFalse(fat.read("TOOLS\\NOTPE.EFI").startswith(b"MZ"))

    def test_oversized_bds_refused(self):
        with self.assertRaises(ValueError):
            mkqemu_disk.build(b"\0" * MIB, None, "")

    def test_fsck(self):
        fsck = tool("fsck.fat")
        if not fsck:
            self.skipTest("fsck.fat not installed")
        for index in (0, 2):
            path = os.path.join(ART, "qemu-part%d.img" % index)
            with open(path, "wb") as f:
                f.write(self.part(index))
            out = subprocess.run([fsck, "-n", path], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
