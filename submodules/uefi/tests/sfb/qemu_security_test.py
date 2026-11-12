#!/usr/bin/env python3
"""QEMU acceptance tests for the SFB security policy (MANUAL_TESTS.md S1-S4).

Runs the freshly built BDS.efi headless on qemu-system-aarch64 with the
project firmware (build/qemu/code.fd) and asserts the certificate-driven
matrix:

  S1  no certs            -> unsigned app boots (also the stand-in for
                             privileged mode: same open behavior)
  S2  BOOT_CERTS          -> unsigned app refused, boot menu appears
  S3  BOOT_CERTS + signed -> signed app boots
  S4  AUTH_CERTS          -> locked: Advanced shows only Enter Fastboot

The fastboot challenge (R1-R5) needs the Qualcomm USB device controller and
stays a real-device test.

Usage: python3 tests/sfb/qemu_security_test.py [-v]
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
UEFI = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import mkqemu_disk  # noqa: E402

BDS = os.path.join(UEFI, "build", "BDS.efi")
APP = os.path.join(UEFI, "build", "RebootTools.efi")
CODE_FD = os.path.join(UEFI, "build", "qemu", "code.fd")
VARS_FD = os.path.join(UEFI, "build", "qemu", "vars.fd")

CASE_TIMEOUT = 60


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def gen_key(art, name):
    key = os.path.join(art, name + ".key")
    crt = os.path.join(art, name + ".cer")
    run(["openssl", "genrsa", "-out", key, "2048"])
    run(["openssl", "req", "-new", "-x509", "-key", key, "-out", crt,
         "-outform", "DER", "-days", "30", "-subj", "/CN=qemu-" + name])
    return key, crt


def sign_app(cert, key, app, out):
    run([sys.executable,
         os.path.join(UEFI, "..", "..", "tools", "sfbsec", "sign_efi.py"),
         cert, key, app, out])


import contextlib


@contextlib.contextmanager
def _KeepDir(path):
    os.makedirs(path, exist_ok=True)
    try:
        yield path
    finally:
        pass


class Qemu:
    """One headless boot with monitor-driven key injection."""

    def __init__(self, art, tag, disk):
        self.log = os.path.join(art, "serial-%s.log" % tag)
        self.sock = os.path.join(art, "monitor-%s.sock" % tag)
        vars_path = os.path.join(art, "vars-%s.fd" % tag)
        shutil.copyfile(VARS_FD, vars_path)
        self.proc = subprocess.Popen([
            "qemu-system-aarch64",
            "-M", "virt", "-cpu", "cortex-a72", "-smp", "2", "-m", "2048",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + CODE_FD,
            "-drive", "if=pflash,format=raw,file=" + vars_path,
            "-drive", "if=none,id=hd0,format=raw,file=" + disk,
            "-device", "virtio-blk-pci,drive=hd0,bootindex=0",
            # Same input path as build/qemu/run.sh: the firmware console
            # reads a USB keyboard; without it sendkey does nothing.
            "-device", "virtio-gpu-pci",
            "-device", "qemu-xhci", "-device", "usb-kbd",
            "-display", "none",
            "-serial", "file:" + self.log,
            "-monitor", "unix:" + self.sock + ",server,nowait",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # The monitor socket is created by QEMU at startup.
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)
        self.mon = None

    def _connect(self):
        if self.mon is None:
            self.mon = socket.socket(socket.AF_UNIX)
            self.mon.settimeout(5)
            self.mon.connect(self.sock)
            self.mon.recv(4096)
        return self.mon

    def sendkey(self, key):
        mon = self._connect()
        mon.sendall(("sendkey %s\n" % key).encode())
        try:
            mon.recv(4096)
        except socket.timeout:
            pass

    def read_log(self):
        try:
            with open(self.log, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""

    def wait_for(self, needle, absent=None, timeout=CASE_TIMEOUT):
        """True once needle appears (and absent, if given, has not yet)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.read_log()
            if needle in text:
                return True
            if absent and absent in text:
                return False
            time.sleep(0.5)
        return False

    def stop(self):
        if self.mon:
            try:
                self.mon.sendall(b"quit\n")
                self.mon.recv(4096)
            except OSError:
                pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def build_disk(art, tag, app_bytes, certs):
    """certs: list of (container_path, data)."""
    container = list(certs)
    image, _ = mkqemu_disk.build(
        open(BDS, "rb").read(), app_bytes,
        mkqemu_disk.DEFAULT_ENTRIES, container_files=container)
    path = os.path.join(art, "disk-%s.img" % tag)
    with open(path, "wb") as f:
        f.write(image)
    return path


def check(name, cond, failures):
    print(("PASS %s" % name) if cond else ("FAIL %s" % name))
    if not cond:
        failures.append(name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    for path in (BDS, APP, CODE_FD, VARS_FD):
        if not os.path.exists(path):
            print("missing %s (run `make build` / `make tools` first)" % path)
            return 2
    if not shutil.which("qemu-system-aarch64"):
        print("qemu-system-aarch64 not installed")
        return 2

    failures = []
    keep = os.environ.get("SFB_KEEP_ARTIFACTS")
    cm = _KeepDir(keep) if keep else tempfile.TemporaryDirectory(prefix="sfb-qemu-")
    with cm as art:
        boot_key, boot_crt = gen_key(art, "boot")
        _, auth_crt = gen_key(art, "auth")
        signed = os.path.join(art, "app-signed.efi")
        sign_app(boot_crt, boot_key, APP, signed)
        unsigned = open(APP, "rb").read()
        signed_b = open(signed, "rb").read()
        boot_certs = [("BOOT_CERTS\\BOOT.CER", open(boot_crt, "rb").read())]
        auth_certs = [("AUTH_CERTS\\AUTH.CER", open(auth_crt, "rb").read())]

        # S1: no certificates - open device, unsigned app boots. This is
        # also the behavioral stand-in for privileged mode (same policy:
        # nothing enforced).
        q = Qemu(art, "s1", build_disk(art, "s1", unsigned, []))
        try:
            check("S1 no certs: unsigned app boots",
                  q.wait_for("Entering Reboot Tools"), failures)
        finally:
            q.stop()

        # S2: BOOT_CERTS present - the unsigned app is refused and the
        # boot menu comes up instead.
        q = Qemu(art, "s2", build_disk(art, "s2", unsigned, boot_certs))
        try:
            ok = q.wait_for("Entering Boot Menu")
            text = q.read_log()
            check("S2 boot certs: unsigned app refused", ok, failures)
            check("S2 boot certs: app did not run",
                  "Entering Reboot Tools" not in text, failures)
        finally:
            q.stop()

        # S3: BOOT_CERTS present - the signed app boots.
        q = Qemu(art, "s3", build_disk(art, "s3", signed_b, boot_certs))
        try:
            check("S3 boot certs: signed app boots",
                  q.wait_for("Entering Reboot Tools"), failures)
        finally:
            q.stop()

        # S4: AUTH_CERTS present - locked. Hold Up during the power-on window
        # to open the menu, walk down to Advanced and check the lock hint;
        # the mass-storage row must NOT be offered.
        def to_menu(q):
            # The volume-up window is the first second of BDS execution,
            # which starts only after the platform BDS loads BOOTAA64.EFI;
            # wait for that point before injecting the key.
            q.wait_for("BdsDxe: starting", timeout=45)
            for _ in range(15):
                q.sendkey("up")
                time.sleep(0.1)
            if not q.wait_for("Entering Boot Menu"):
                return False
            # Wait until the menu itself is rendered: the announcement
            # screen is shown first and keys sent during it are dropped.
            return q.wait_for("EFIBOOT (0,2)", timeout=10)

        def to_advanced(q):
            # Rows: EFIBOOT, Tests >, Advanced, Power Off, Restart.
            time.sleep(1)
            q.sendkey("down")
            time.sleep(0.5)
            q.sendkey("down")
            time.sleep(0.5)
            q.sendkey("ret")

        q = Qemu(art, "s4", build_disk(art, "s4", unsigned, auth_certs))
        try:
            opened = to_menu(q)
            check("S4 auth certs: boot menu", opened, failures)
            if opened:
                to_advanced(q)
                ok = q.wait_for("only fastboot authorization")
                text = q.read_log()
                check("S4 auth certs: locked Advanced hint", ok, failures)
                check("S4 auth certs: no mass storage row",
                      "USB Mass Storage" not in text, failures)
        finally:
            q.stop()

        # S5: both classes present. Verification is independent of the
        # auth lock: an unsigned app is still refused at power-on, though
        # the menu would be locked if reached.
        q = Qemu(art, "s5", build_disk(art, "s5", unsigned,
                                       boot_certs + auth_certs))
        try:
            ok = q.wait_for("Entering Boot Menu")
            text = q.read_log()
            check("S5 both certs: unsigned app refused at boot", ok, failures)
            check("S5 both certs: app did not run",
                  "Entering Reboot Tools" not in text, failures)
        finally:
            q.stop()

        # S6: only BOOT_CERTS. Entering the menu must be privileged directly
        # (full Advanced rows), while verification still applies at boot.
        q = Qemu(art, "s6", build_disk(art, "s6", signed_b, boot_certs))
        try:
            opened = to_menu(q)
            check("S6 boot-only: boot menu", opened, failures)
            if opened:
                to_advanced(q)
                ok = q.wait_for("USB Mass Storage")
                text = q.read_log()
                check("S6 boot-only: full (privileged) Advanced", ok, failures)
                check("S6 boot-only: no lock hint",
                      "only fastboot authorization" not in text, failures)
        finally:
            q.stop()

    if failures:
        print("%d failures" % len(failures))
        return 1
    print("all QEMU security cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
