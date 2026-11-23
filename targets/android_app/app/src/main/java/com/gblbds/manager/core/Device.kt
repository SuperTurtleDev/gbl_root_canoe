package com.gblbds.manager.core

import android.content.res.Resources
import com.gblbds.manager.R
import com.gblbds.manager.root.RootShell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Device-side operations. All paths/constants mirror the magisk module
 * (bl_flasher.sh / customize.sh) and the BDS config-store layout. Every user
 * visible message goes through the string resources for i18n.
 */
class Device(private val shell: RootShell, private val nativeDir: File,
             private val res: Resources,
             private val appUid: Int = android.os.Process.myUid()) {

    private fun s(id: Int): String = res.getString(id)
    private fun s(id: Int, vararg args: Any): String = res.getString(id, *args)

    companion object {
        const val BY_NAME = "/dev/block/by-name"
        const val PERSIST_MNT = "/mnt/vendor/persist"
        const val EFISP_DIR = "$PERSIST_MNT/efisp"
        const val ENTRIES_PATH = "$PERSIST_MNT/ENTRIES.CFG"
        const val MNT = "/data/adb/sfbconfig"
        const val CONFIG_ENTRIES = "$MNT/CONFIG/ENTRIES.CFG"
        // Written once the container has been formatted and seeded with the
        // initial BootOrder. Its presence means "this BDS install is already
        // set up" - do not re-run the first-run flow on every app start.
        const val INIT_MARKER = "$MNT/CONFIG/INITIALIZED"
        const val INIT_MARKER_VALUE = "gblbds-manager-v1"
        const val BOOT_CERT = "$MNT/BOOT_CERTS/BOOT.CER"
        const val AUTH_DIR = "$MNT/AUTH_CERTS"
        const val REPO_URL =
            "https://raw.githubusercontent.com/superturtlee/gbl_root_canoe/main/ablrepo"
        // Secure-fastboot preload wrapper: every system loader goes through it.
        const val WRAPPER_NAME = "secure_fastboot.efi"
        const val WRAPPER_PATH = "\\efisp\\secure_fastboot.efi"
        const val BOOT_PATH = "\\efisp\\boot.efi"
        const val BOOT_BACKUP_PATH = "\\efisp\\boot_backup.efi"
    }

    data class Step(val ok: Boolean, val message: String,
                    val detail: String = "")

    /** Result of a patch-engine run: the output file plus its raw log. */
    data class PatchResult(val file: File?, val output: String)

    private suspend fun sh(script: String): RootShell.Result =
        withContext(Dispatchers.IO) { shell.run(script) }

    /** Quote one argument for the root shell. */
    fun q(s: String) = "'" + s.replace("'", "'\\''") + "'"

    suspend fun currentSlot(): String = withContext(Dispatchers.IO) {
        shell.run("getprop ro.boot.slot_suffix").out.trim()
    }

    /** efisp container length in bytes, from sysfs GPT start/size (inclusive end). */
    suspend fun containerBytes(): Long? = withContext(Dispatchers.IO) {
        val real = shell.run("readlink -f $BY_NAME/efisp").out.trim()
        if (real.isEmpty()) return@withContext null
        // /dev/block/sdXN -> /sys/class/block/sdXN/start , size
        val name = real.substringAfterLast('/')
        // Partition sysfs lives under /sys/class/block (not /sys/block, which
        // only holds whole disks on Android kernels).
        val size = shell.run(
            "cat ${q("/sys/class/block/$name/size")} 2>/dev/null || " +
                "cat ${q("/sys/block/$name/size")} 2>/dev/null").out.trim().toLongOrNull()
        if (size == null || size <= 2048) return@withContext null
        // size is the partition's 512-byte sector count; the BDS image takes
        // the first 1 MiB, the container is everything after it.
        (size * 512L) - FatFormatter.BDS_RESERVED_BYTES
    }

    suspend fun persistMounted(): Boolean = withContext(Dispatchers.IO) {
        shell.run("grep -q ' $PERSIST_MNT ' /proc/mounts").ok
    }

    /** TRUE when efisp starts with an MZ (PE/EFI) image - a BDS is installed. */
    suspend fun efispHasMz(): Boolean = withContext(Dispatchers.IO) {
        shell.run("test \"\$(dd if=$BY_NAME/efisp bs=2 count=1 2>/dev/null)\" = MZ").ok
    }

    /** The running slot's ABL partition image, or null when there is no slot. */
    suspend fun currentAblFile(): File? = withContext(Dispatchers.IO) {
        val slot = currentSlot()
        if (slot != "_a" && slot != "_b") return@withContext null
        val real = shell.run("readlink -f $BY_NAME/abl$slot").out.trim()
        if (real.isEmpty()) null else File(real)
    }

    /**
     * (disk,part) of the persist partition under the BDS volume table.
     * sda = LUN0, sdb = LUN1, ...; part is the GPT partition number itself
     * (the device's existing CONFIG/ENTRIES.CFG uses (5,8) for /dev/block/sdf8),
     * matching Ptable[Lun].HandleInfoList[j].
     */
    suspend fun persistAddress(): Pair<Int, Int>? = withContext(Dispatchers.IO) {
        val real = shell.run("readlink -f $BY_NAME/persist").out.trim()
        // e.g. /dev/block/sdf8 -> disk 'f' (= LUN5), partition 8.
        // Parse by hand: no regular expressions in this codebase.
        val prefix = "/dev/block/sd"
        if (!real.startsWith(prefix) || real.length <= prefix.length + 1) {
            return@withContext null
        }
        val diskChar = real[prefix.length]
        if (diskChar < 'a' || diskChar > 'z') return@withContext null
        var part = 0
        for (i in (prefix.length + 1) until real.length) {
            val c = real[i]
            if (c < '0' || c > '9') return@withContext null
            part = part * 10 + (c - '0')
        }
        val disk = diskChar - 'a'
        if (part <= 0 || part > 128) null else disk to part
    }

    /**
     * The initial global BootOrder line: the Android template rendered for
     * the persist volume, i.e. chainloaded through the secure-fastboot
     * wrapper with fastboot keys blocked for non-privileged sessions.
     */
    suspend fun initialEntry(): String? {
        val (d, p) = persistAddress() ?: return null
        return SYSTEM_BOOT_ENTRIES.first().line("$d,$p")
    }

    /**
     * Put one of the app's bundled EFIs on persist/efisp under [name].
     * force=false keeps an existing copy (it may already carry a signature);
     * the update path passes force=true to pick up the bundled version.
     */
    suspend fun installEfispEfi(src: File, name: String, force: Boolean): Step =
        withContext(Dispatchers.IO) {
            if (!src.exists()) {
                return@withContext Step(false, s(R.string.err_sys_efi_asset, src.name))
            }
            if (!persistMounted()) {
                return@withContext Step(false,
                    s(R.string.err_persist_not_mounted, PERSIST_MNT))
            }
            val dst = "$EFISP_DIR/$name"
            if (!force && shell.run("test -s ${q(dst)}").ok) {
                return@withContext Step(true, s(R.string.msg_sys_efi_present, name))
            }
            val r = shell.run(
                "mkdir -p ${q(EFISP_DIR)} && " +
                    "cp ${q(src.absolutePath)} ${q(dst)} && sync")
            Step(r.ok, if (r.ok) s(R.string.msg_sys_efi_written, name) else r.err)
        }

    suspend fun bootEfiExists(): Boolean = withContext(Dispatchers.IO) {
        shell.run("test -s $EFISP_DIR/boot.efi").ok
    }

    /** TRUE when the OTA flow left the previous loader as boot_backup.efi. */
    suspend fun bootBackupExists(): Boolean = withContext(Dispatchers.IO) {
        shell.run("test -s $EFISP_DIR/boot_backup.efi").ok
    }

    suspend fun mounted(): Boolean = withContext(Dispatchers.IO) {
        shell.run("grep -q ' $MNT ' /proc/mounts").ok
    }

    /** Format the container (fresh, allow overwriting old data) and dd it in. */
    suspend fun formatContainer(bytes: Long): Step = withContext(Dispatchers.IO) {
        val sectors = (bytes / FatFormatter.SECTOR).toInt()
        if (sectors <= 0 || sectors * FatFormatter.SECTOR.toLong() != bytes) {
            return@withContext Step(false, s(R.string.err_container_not_aligned, bytes))
        }
        val image = FatFormatter.format(sectors)
        val tmp = File(nativeDir, "container.img")
        tmp.writeBytes(image)
        // seek=2048 (1 MiB / 512). conv=notrunc is deliberately NOT used:
        // dd writes exactly the image length, stopping at the partition end.
        val r = shell.run(
            "blockdev --setrw $BY_NAME/efisp && " +
                "dd if=${q(tmp.absolutePath)} of=$BY_NAME/efisp bs=512 seek=2048 " +
                "count=$sectors conv=fsync && sync")
        tmp.delete()
        Step(r.ok, if (r.ok) s(R.string.msg_container_formatted, bytes / 1024 / 1024) else r.err)
    }

    /** Attach loop with offset=1MiB and sizelimit=containerBytes, then mount vfat. */
    suspend fun mountContainer(bytes: Long): Step = withContext(Dispatchers.IO) {
        if (mounted()) return@withContext Step(true, s(R.string.msg_already_mounted))
        // Android's toybox losetup only accepts short options (-o / -S);
        // util-linux style --offset / --sizelimit gives "unknown option offset".
        val r = shell.run(
            "mkdir -p $MNT && " +
                "LOOP=\$(losetup -f) && " +
                "losetup -o ${FatFormatter.BDS_RESERVED_BYTES} -S $bytes " +
                "\$LOOP $BY_NAME/efisp && " +
                "mount -t vfat -o rw,sync \$LOOP $MNT && " +
                "echo \$LOOP")
        if (!r.ok) {
            // A corrupt/unmountable FAT is a full re-initialization signal.
            return@withContext Step(false, s(R.string.err_mount_failed, r.err))
        }
        Step(true, s(R.string.msg_mounted_at, MNT))
    }

    suspend fun unmountContainer(): Unit = withContext(Dispatchers.IO) {
        shell.run("umount $MNT 2>/dev/null; losetup -d \$(losetup -j $BY_NAME/efisp | cut -d: -f1) 2>/dev/null; true")
    }

    /** Read CONFIG\ENTRIES.CFG from the mounted container. */
    suspend fun readEntries(): List<String>? = withContext(Dispatchers.IO) {
        val r = shell.run("cat ${q(CONFIG_ENTRIES)} 2>/dev/null")
        if (!r.ok) null else r.out.lines().filter { it.isNotBlank() }
    }

    /** Write entries atomically-ish and sync, creating CONFIG\ if needed. */
    suspend fun writeEntries(lines: List<String>): Step = withContext(Dispatchers.IO) {
        val tmp = File(nativeDir, "ENTRIES.CFG")
        tmp.writeText(lines.joinToString("\n") + "\n")
        val r = shell.run(
            "mkdir -p $MNT/CONFIG && " +
                "cp ${q(tmp.absolutePath)} ${q(CONFIG_ENTRIES)} && sync")
        tmp.delete()
        Step(r.ok, if (r.ok) s(R.string.msg_saved_sync) else r.err)
    }

    /** TRUE when the container carries the initialized marker. */
    suspend fun initialized(): Boolean = withContext(Dispatchers.IO) {
        shell.run("grep -q '$INIT_MARKER_VALUE' ${q(INIT_MARKER)} 2>/dev/null").ok
    }

    /** Stamp the container after a successful fresh initialization. */
    suspend fun markInitialized(): Step = withContext(Dispatchers.IO) {
        val r = shell.run(
            "mkdir -p $MNT/CONFIG && " +
                "echo '$INIT_MARKER_VALUE' > ${q(INIT_MARKER)} && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_marker_written) else r.err)
    }

    // ---- secure boot ---------------------------------------------------------

    /** TRUE once BOOT_CERTS/BOOT.CER is in the container (BDS enforces). */
    suspend fun bootCertExists(): Boolean = withContext(Dispatchers.IO) {
        shell.run("test -s $BOOT_CERT").ok
    }

    /** Write the boot certificate into the container's BOOT_CERTS directory. */
    suspend fun writeBootCert(certDer: ByteArray): Step = withContext(Dispatchers.IO) {
        val tmp = File(nativeDir, "BOOT.CER")
        tmp.writeBytes(certDer)
        val r = shell.run(
            "mkdir -p $MNT/BOOT_CERTS && " +
                "cp ${q(tmp.absolutePath)} ${q(BOOT_CERT)} && sync")
        tmp.delete()
        Step(r.ok, if (r.ok) s(R.string.msg_cert_written) else r.err)
    }

    /** Certificate file names in AUTH_CERTS (empty when absent). */
    suspend fun listAuthCertFiles(): List<String> = withContext(Dispatchers.IO) {
        val r = shell.run("ls -1 ${q(AUTH_DIR)} 2>/dev/null")
        if (r.ok) r.out.lines().filter { it.isNotBlank() } else emptyList()
    }

    /** Read one AUTH_CERTS certificate into app storage for parsing. */
    suspend fun readAuthCert(name: String, into: File): File? =
        readRootFile("$AUTH_DIR/$name", into)

    /** Install (or replace) a paired computer's certificate, then sync. */
    suspend fun installAuthCert(der: ByteArray, name: String): Step =
        withContext(Dispatchers.IO) {
            val tmp = File(nativeDir, "AUTH.CER")
            tmp.writeBytes(der)
            val r = shell.run(
                "mkdir -p ${q(AUTH_DIR)} && " +
                    "cp ${q(tmp.absolutePath)} ${q("$AUTH_DIR/$name")} && sync")
            tmp.delete()
            Step(r.ok, if (r.ok) s(R.string.msg_auth_cert_written) else r.err)
        }

    suspend fun deleteAuthCert(name: String): Step = withContext(Dispatchers.IO) {
        val r = shell.run("rm -f ${q("$AUTH_DIR/$name")} && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_auth_cert_removed) else r.err)
    }

    /** All .efi files under persist/efisp that BDS could launch. */
    suspend fun listEfispEfiFiles(): List<String> = withContext(Dispatchers.IO) {
        val r = shell.run("find $EFISP_DIR -iname '*.efi' 2>/dev/null | sort")
        if (r.ok) r.out.lines().filter { it.isNotBlank() } else emptyList()
    }

    /**
     * Pull a root-owned file into app-private storage. The root shell writes
     * it, so ownership must be handed back to the app uid afterwards - a
     * root-owned 0600 file in files/bin cannot be rewritten by the app and
     * crashed the signing flow with EACCES.
     */
    suspend fun readRootFile(path: String, into: File): File? =
        withContext(Dispatchers.IO) {
            val r = shell.run(
                "cat ${q(path)} > ${q(into.absolutePath)} && " +
                    "chown $appUid:$appUid ${q(into.absolutePath)} && " +
                    "chmod 0600 ${q(into.absolutePath)}")
            if (r.ok && into.length() > 0) into else null
        }

    /** Push an app-private file back over a root-owned path + sync. */
    suspend fun writeRootFile(from: File, path: String): Boolean =
        withContext(Dispatchers.IO) {
            shell.run("cp ${q(from.absolutePath)} ${q(path)} && sync").ok
        }

    /** Full container initialization: fresh FAT + global BootOrder in CONFIG only. */
    suspend fun initializeContainer(bytes: Long): Step {
        val entry = initialEntry()
            ?: return Step(false, s(R.string.err_no_persist_addr))
        if (!persistMounted()) return Step(false, s(R.string.err_persist_not_mounted, PERSIST_MNT))
        val fmt = formatContainer(bytes)
        if (!fmt.ok) return fmt
        // Detach any stale loop before re-mounting a freshly formatted image.
        unmountContainer()
        val mnt = mountContainer(bytes)
        if (!mnt.ok) return mnt
        val written = writeEntries(listOf(entry))
        if (!written.ok) return written
        return markInitialized()
    }

    /** Flash BDS.efi to the raw efisp partition, by its own length only. */
    suspend fun flashBds(bds: File): Step = withContext(Dispatchers.IO) {
        val r = shell.run(
            "blockdev --setrw $BY_NAME/efisp && " +
                "dd if=${q(bds.absolutePath)} of=$BY_NAME/efisp bs=4096 " +
                "conv=notrunc,fsync && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_bds_flashed) else r.err)
    }

    /**
     * TRUE when the BDS image at the start of efisp is byte-identical to bds.
     * BDS is flashed by its own length and never padded, so exactly that
     * prefix counts: an older, shorter image leaves stale bytes behind it.
     * The comparison runs entirely in the root shell - only the image length
     * is read from the raw partition (toybox cmp -s, prefix on stdin).
     */
    suspend fun bdsMatches(bds: File): Boolean = withContext(Dispatchers.IO) {
        val size = bds.length()
        if (size <= 0) return@withContext false
        val blocks = (size + 4095) / 4096
        shell.run(
            "dd if=$BY_NAME/efisp bs=4096 count=$blocks 2>/dev/null | " +
                "head -c $size | cmp -s - ${q(bds.absolutePath)}").ok
    }

    /** GBL vulnerability probe on an ABL image via the bundled patcher. */
    suspend fun probeGbl(abl: File): Step = withContext(Dispatchers.IO) {
        val r = patchAbl(abl)
        val vulnerable = r.file != null &&
            !r.output.contains("Warning: Failed to patch ABL GBL")
        Step(vulnerable,
            if (vulnerable) s(R.string.msg_vuln_found) else s(R.string.msg_vuln_absent),
            r.output)
    }

    /**
     * Extract and patch an ABL image. The raw output of both engine tools is
     * returned so the caller can put it into the log viewer.
     */
    suspend fun patchAbl(abl: File): PatchResult = withContext(Dispatchers.IO) {
        val dir = File(nativeDir, "patch").apply { mkdirs() }
        val patched = File(dir, "patched.efi")
        val logFile = File(dir, "patch.log")
        shell.run("rm -f ${q(patched.absolutePath)} ${q(logFile.absolutePath)}")
        val r = shell.run(
            "${q(File(nativeDir, "extractfv").absolutePath)} -o ${q(dir.absolutePath)} " +
                "-v ${q(abl.absolutePath)} > ${q(logFile.absolutePath)} 2>&1 && " +
                "${q(File(nativeDir, "patch_abl").absolutePath)} " +
                "${q(File(dir, "LinuxLoader.efi").absolutePath)} " +
                "${q(patched.absolutePath)} >> ${q(logFile.absolutePath)} 2>&1; " +
                "cat ${q(logFile.absolutePath)}")
        val ok = patched.exists() && patched.length() > 0
        PatchResult(if (ok) patched else null, r.out)
    }

    /**
     * Install a prepared boot.efi, keeping any existing one as
     * boot_backup.efi (one level, like the OTA flow).
     */
    suspend fun installBootEfi(patched: File): Step = withContext(Dispatchers.IO) {
        if (!persistMounted()) {
            return@withContext Step(false, s(R.string.err_persist_not_mounted, PERSIST_MNT))
        }
        val r = shell.run(
            "mkdir -p $EFISP_DIR && " +
                "mv $EFISP_DIR/boot.efi $EFISP_DIR/boot_backup.efi 2>/dev/null; " +
                "cp ${q(patched.absolutePath)} $EFISP_DIR/boot.efi && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_boot_efi_written) else r.err)
    }

    /** Remove BOOT_CERTS/BOOT.CER; secure boot stays off until re-signed. */
    suspend fun deleteBootCert(): Step = withContext(Dispatchers.IO) {
        val r = shell.run("rm -f ${q(BOOT_CERT)} && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_boot_cert_removed) else r.err)
    }

    /** Flash a vulnerability-carrying ABL to the CURRENT slot only. */
    suspend fun flashVulnAbl(abl: File): Step = withContext(Dispatchers.IO) {
        val slot = currentSlot()
        if (slot != "_a" && slot != "_b") return@withContext Step(false, s(R.string.err_no_slot))
        val dst = "$BY_NAME/abl$slot"
        val r = shell.run(
            "blockdev --setrw $dst && " +
                "dd if=${q(abl.absolutePath)} of=$dst bs=4M conv=fsync && sync")
        Step(r.ok, if (r.ok) s(R.string.msg_vuln_abl_flashed, dst) else r.err)
    }

    /**
     * Post-OTA retention, mirroring the magisk module's "flash to other slot"
     * (bl_flasher.sh update-efisp). Run after an OTA has been written to the
     * inactive slot and before rebooting:
     *
     *   1. patch the TARGET (OTA-written) ABL into a new persist/efisp/boot.efi,
     *      keeping the previous one as boot_backup.efi
     *   2. re-flash the BDS onto efisp
     *   3. unless the target ABL carries the GBL vulnerability, copy the
     *      CURRENT (old-BL) ABL over the target slot so the next boot keeps
     *      the old bootloader
     *
     * The caller appends boot_backup.efi to the boot order afterwards, so the
     * previous loader stays selectable from the BDS menu.
     *
     * Refuses a likely duplicate run when boot.efi exists and both slots
     * already hold the same ABL.
     */
    suspend fun otaPostUpdate(): Step = withContext(Dispatchers.IO) {
        val slot = currentSlot()
        if (slot != "_a" && slot != "_b") {
            return@withContext Step(false, s(R.string.err_no_slot))
        }
        val target = if (slot == "_a") "_b" else "_a"
        val currentAbl = "$BY_NAME/abl$slot"
        val targetAbl = "$BY_NAME/abl$target"

        if (bootEfiExists() && shell.run("cmp -s $currentAbl $targetAbl").ok) {
            return@withContext Step(false,
                s(R.string.err_ota_duplicate))
        }

        val dir = File(nativeDir, "ota").apply { mkdirs() }
        val patched = File(dir, "patched.efi")
        val logFile = File(dir, "patch.log")
        shell.run("rm -f ${q(patched.absolutePath)} ${q(logFile.absolutePath)}")
        val r = shell.run(
            "${q(File(nativeDir, "extractfv").absolutePath)} -o ${q(dir.absolutePath)} " +
                "-v $targetAbl > ${q(logFile.absolutePath)} 2>&1 && " +
                "${q(File(nativeDir, "patch_abl").absolutePath)} " +
                "${q(File(dir, "LinuxLoader.efi").absolutePath)} " +
                "${q(patched.absolutePath)} >> ${q(logFile.absolutePath)} 2>&1; " +
                "cat ${q(logFile.absolutePath)}")
        if (!patched.exists()) {
            return@withContext Step(false,
                s(R.string.err_ota_patch, r.out.take(200)), r.out)
        }
        val hasVuln = !r.out.contains("Warning: Failed to patch ABL GBL")

        if (!persistMounted()) {
            return@withContext Step(false,
                s(R.string.err_persist_not_mounted, PERSIST_MNT), r.out)
        }

        // 1. boot.efi: keep the previous one, install the newly patched one.
        val put = shell.run(
            "mv $EFISP_DIR/boot.efi $EFISP_DIR/boot_backup.efi 2>/dev/null; " +
                "cp ${q(patched.absolutePath)} $EFISP_DIR/boot.efi && sync")
        if (!put.ok) {
            return@withContext Step(false, s(R.string.err_boot_efi_write, put.err), r.out)
        }

        // 2. Refresh the BDS itself.
        val bds = flashBds(File(nativeDir, "BDS.efi"))
        if (!bds.ok) return@withContext Step(false, bds.message, r.out)

        // 3. Keep the old BL unless the OTA ABL already loads efisp by itself.
        if (hasVuln) {
            return@withContext Step(true,
                s(R.string.msg_ota_skip_copy), r.out)
        }
        val cp = shell.run(
            "blockdev --setrw $targetAbl && " +
                "dd if=$currentAbl of=$targetAbl bs=4M conv=fsync && sync")
        Step(cp.ok, if (cp.ok)
            s(R.string.msg_ota_done, targetAbl) else cp.err, r.out)
    }

    /** Try the repo for a vulnerability ABL; returns the downloaded file or null. */
    suspend fun fetchRepoAbl(): File? = withContext(Dispatchers.IO) {
        val product = shell.run("getprop ro.product.name").out.trim()
        if (product.isEmpty()) return@withContext null
        val out = File(nativeDir, "repo_abl.img")
        val url = "$REPO_URL/$product/abl.img"
        val r = shell.run(
            "wget -O ${q(out.absolutePath)} ${q(url)} 2>/dev/null || " +
                "curl -fL -o ${q(out.absolutePath)} ${q(url)} 2>/dev/null")
        if (r.ok && out.length() > 0) out else null
    }
}
