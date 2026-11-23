package com.gblbds.manager.core

import android.app.Application
import android.net.Uri
import android.util.Base64
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.gblbds.manager.R
import com.gblbds.manager.root.RootShell
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedInputStream
import java.io.File
import java.io.InputStream
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.Signature
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate

/** UI phases. Deployment has its own page, so the first-run wizard is gone. */
enum class Phase { LOADING, NO_ROOT, READY, ERROR }

/** Top-level pages. Settings is a page, not a dropdown/dialog. */
enum class Screen { MAIN, SETTINGS, FASTBOOT, PAIRING, DEPLOY, LOGS }

data class Prompt(val title: String, val body: String, val confirm: String,
                  val dismiss: String? = null,
                  val onConfirm: suspend () -> Unit,
                  val onDismiss: (suspend () -> Unit)? = null)

/** Secure-boot states: off, enforced, or enforced with unsigned images. */
enum class SbState { DISABLED, ENABLED, DEGRADED }

/** A pending signature the UI must authenticate (BiometricPrompt) and return. */
data class SignRequest(val digestInfo: ByteArray, val fileName: String)

/** One computer paired for fastboot authorization (a cert in AUTH_CERTS). */
data class AuthCert(val fileName: String, val cn: String, val fingerprint: String)

/** Live state of the pairing service while the PAIRING page is open. */
data class PairingUi(
    val address: String = "",
    val code: String = "",
    val status: String = "",
    val done: Boolean = false,
)

/**
 * Deployment page state. The current-slot ABL probe runs when the page
 * opens; the vulnerable-ABL section is only usable when that probe says the
 * running ABL lacks the efisp vulnerability.
 */
data class DeployUi(
    val efispClean: Boolean = true,        // efisp holds no MZ (BDS) image
    val slotVulnerable: Boolean? = null,   // null = probe still running
    val currentLabel: String? = null,      // non-null = current source ready
    val currentIsEfi: Boolean = false,
    val vulnLabel: String? = null,
    val vulnVerified: Boolean? = null,     // null = none, false = no vulnerability
    val done: Boolean = false,
)

data class UiState(
    val phase: Phase = Phase.LOADING,
    val message: String = "",
    val entries: List<BootEntry> = emptyList(),
    val busy: Boolean = false,
    val prompt: Prompt? = null,
    val requestFile: FileRequest? = null,
    val editing: BootEntry? = null,   // entry open in the edit dialog
    val screen: Screen = Screen.MAIN,
    val sbState: SbState = SbState.DISABLED,
    val authCerts: List<AuthCert> = emptyList(),
    val pairing: PairingUi? = null,
    val deploy: DeployUi? = null,
    val signRequest: SignRequest? = null,
    val log: List<String> = emptyList(),
)

data class FileRequest(val kind: Kind, val mime: String) {
    enum class Kind { ABL_CURRENT, ABL_VULN, PATCHED_EFI, EFI }
}

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui

    private val _staged = MutableStateFlow<File?>(null)
    val staged: StateFlow<File?> = _staged

    private companion object {
        const val PAIR_PORT = 37021
        const val PAIR_ACCEPT_TIMEOUT_MS = 120_000
        const val PAIR_READ_TIMEOUT_MS = 30_000
        const val PAIR_LINE_MAX = 65_536
        const val PAIR_CERT_MAX = 16_384
    }

    private lateinit var shell: RootShell
    private lateinit var device: Device
    private lateinit var nativeDir: File
    private var containerBytes: Long = 0
    private var pendingDeletes: List<String> = emptyList()
    private var pendingRenames: List<Pair<String, String>> = emptyList()
    // Set when the user dismisses the system component update prompt; the
    // post-OTA operation then forces the update before doing anything else.
    private var sysUpdatePending = false

    private fun s(id: Int) = getApplication<Application>().getString(id)
    private fun s(id: Int, vararg args: Any) =
        getApplication<Application>().getString(id, *args)

    private fun update(block: UiState.() -> UiState) {
        _ui.update { it.block() }
    }

    private fun log(msg: String) = update { copy(log = (log + msg).takeLast(200)) }

    /** Patch-engine output goes into the log viewer verbatim, line by line. */
    private fun logOutput(output: String) {
        if (output.isBlank()) return
        for (line in output.lines()) {
            if (line.isNotBlank()) log(line)
        }
    }

    private fun logStep(step: Device.Step) = logOutput(step.detail)

    private fun fail(msg: String) = update {
        copy(phase = Phase.ERROR, message = msg, busy = false, log = log + msg)
    }

    fun start() {
        if (::shell.isInitialized && _ui.value.phase != Phase.LOADING) return
        val app = getApplication<Application>()
        shell = RootShell()
        nativeDir = File(app.filesDir, "bin").apply { mkdirs() }
        device = Device(shell, nativeDir, app.resources)
        viewModelScope.launch { boot(app) }
    }

    private suspend fun installAssets(context: Application) =
        withContext(Dispatchers.IO) {
            val names = listOf("BDS.efi", "secure_fastboot.efi", "patch_abl", "extractfv")
            val available = context.assets.list("")?.toList().orEmpty()
            for (name in names) {
                val out = File(nativeDir, name)
                if (name !in available) {
                    log(s(R.string.log_asset_missing, name))
                    continue
                }
                context.assets.open(name).use { i ->
                    out.outputStream().use { o -> i.copyTo(o) }
                }
                shell.run("chmod 0755 '${out.absolutePath}'")
            }
        }

    private suspend fun boot(context: Application) {
        update { copy(phase = Phase.LOADING, message = s(R.string.checking)) }
        if (!withContext(Dispatchers.IO) { shell.available() }) {
            update { copy(phase = Phase.NO_ROOT, message = s(R.string.need_root)) }
            return
        }
        installAssets(context)
        log(s(R.string.log_root_ok))

        val bytes = device.containerBytes()
        if (bytes == null) {
            fail(s(R.string.err_efisp_bounds))
            return
        }
        containerBytes = bytes
        log(s(R.string.log_container_bytes, bytes))

        val mounted = device.mounted()
        var corrupt = false
        if (!mounted) {
            val m = device.mountContainer(bytes)
            if (!m.ok) {
                log(s(R.string.log_container_corrupt, m.message))
                corrupt = true
            } else {
                log(s(R.string.log_container_mounted))
            }
        } else {
            log(s(R.string.log_already_mounted))
        }

        // Without a secure lock screen the auth-per-use keystore key cannot
        // be used. Leaving BOOT_CERTS enabled would refuse every image that
        // cannot be re-signed, blocking OTA and new boot entries.
        if (!HardwareKey.hasSecureLockScreen(context)) {
            if (device.bootCertExists() || HardwareKey.exists()) {
                log(s(R.string.log_sb_forced_off))
                if (device.bootCertExists()) {
                    val rm = device.deleteBootCert()
                    log(rm.message)
                }
                HardwareKey.delete()
            }
        }

        if (!corrupt) {
            val lines = device.readEntries()
            if (device.initialized()) {
                if (lines != null) {
                    log(s(R.string.log_marker_skip))
                    showReadyOrUpdate(lines, s(R.string.ready_initialized))
                    return
                }
                log(s(R.string.log_marker_unreadable))
            } else {
                log(s(R.string.log_deploy_auto))
            }
        }
        // Not initialized (or corrupt): the deployment page decides what to
        // keep and what to install.
        openDeploy(false)
    }

    private fun showReady(lines: List<String>, message: String) {
        update {
            copy(phase = Phase.READY, busy = false, message = message,
                 entries = lines.mapNotNull { BootEntry.parse(it) })
        }
        refreshSbState()
    }

    /**
     * Normal start (marker present): show the editor, then compare the
     * bundled system components with the device. One BDS update covers the
     * BDS core, the additional EFIs and the system boot entries; anything
     * outdated is offered in a single prompt.
     */
    private suspend fun showReadyOrUpdate(lines: List<String>, message: String) {
        showReady(lines, message)
        val outdated = SystemEfi.values().filter { !systemEfiCurrent(it) }
        val entriesOutdated = systemEntriesOutdated(lines)
        if (outdated.isEmpty() && !entriesOutdated) return

        val parts = outdated.map { s(it.labelRes) } +
            (if (entriesOutdated) listOf(s(R.string.sys_efi_entries)) else emptyList())
        val list = parts.joinToString(", ")
        log(s(R.string.log_sys_update, list))
        update {
            copy(prompt = Prompt(
                title = s(R.string.sys_update_title),
                body = s(R.string.sys_update_msg, list),
                confirm = s(R.string.sys_update_btn),
                dismiss = s(R.string.btn_cancel),
                onConfirm = { runSystemUpdate() },
                onDismiss = { sysUpdatePending = true }))
        }
    }

    // ---- system component update ---------------------------------------------

    /**
     * Content comparison only - update detection never looks at signatures;
     * the per-file signer decides what needs signing after the update.
     */
    private suspend fun systemEfiCurrent(efi: SystemEfi): Boolean =
        systemEfiContentCurrent(efi)

    /**
     * Content comparison with the signature stripped on both sides: the
     * bundled asset is unsigned, the device copy may be signed, and a
     * re-sign must not look like a content change.
     */
    private suspend fun systemEfiContentCurrent(efi: SystemEfi): Boolean {
        val asset = File(nativeDir, efi.asset)
        if (!asset.exists()) return false
        return when (efi) {
            SystemEfi.BDS -> device.bdsMatches(asset)
            else -> {
                val name = efi.destName ?: return false
                val local = File(nativeDir, "sys_check.efi")
                val got = device.readRootFile("${Device.EFISP_DIR}/$name", local)
                    ?: return false
                withContext(Dispatchers.IO) {
                    try {
                        PeSigner.canonicalHash(PeSigner.prepare(asset.readBytes()).second)
                            .contentEquals(
                                PeSigner.canonicalHash(
                                    PeSigner.prepare(local.readBytes()).second))
                    } catch (e: Exception) {
                        false
                    }
                }
            }
        }
    }

    /**
     * Render each system entry template once with the current persist address
     * and check that exact line is in the stored list. Missing or different
     * lines are outdated; nothing else is inspected.
     */
    private suspend fun systemEntriesOutdated(lines: List<String>): Boolean {
        val (d, p) = device.persistAddress() ?: return false
        val address = "$d,$p"
        for (t in SYSTEM_BOOT_ENTRIES) {
            if (!systemTargetExists(t.target)) continue
            val want = t.line(address)
            if (lines.none { it.trim() == want }) return true
        }
        return false
    }

    private suspend fun systemTargetExists(target: String): Boolean =
        when (target) {
            Device.BOOT_PATH -> device.bootEfiExists()
            Device.BOOT_BACKUP_PATH -> device.bootBackupExists()
            else -> true
        }

    /**
     * Apply one BDS update. Nothing is skipped: the BDS core is flashed, all
     * additional EFIs are copied and every system entry is rewritten from its
     * template. Signing runs afterwards and skips only images that already
     * carry a valid, trusted signature.
     */
    private suspend fun runSystemUpdate() {
        update { copy(busy = true, message = s(R.string.sys_update_busy), prompt = null) }

        // 1. BDS core.
        val bds = device.flashBds(File(nativeDir, SystemEfi.BDS.asset))
        log(bds.message)
        if (!bds.ok) { fail(bds.message); return }

        // 2. All additional EFIs.
        for (efi in SystemEfi.values()) {
            if (efi == SystemEfi.BDS) continue
            val name = efi.destName ?: continue
            val r = device.installEfispEfi(File(nativeDir, efi.asset), name,
                force = true)
            log(r.message)
            if (!r.ok) { fail(r.message); return }
        }

        // 3. All system boot entries from the templates.
        syncSystemEntries()

        // 4. Secure boot: a device without the key never enabled it, so
        //    signAll() does nothing there. Valid signatures are skipped
        //    inside the per-file signer.
        signAll()

        sysUpdatePending = false
        val lines = device.readEntries().orEmpty()
        update {
            copy(entries = lines.mapNotNull { BootEntry.parse(it) },
                busy = false, message = s(R.string.sys_update_done))
        }
        refreshSbState()
    }

    /**
     * Rewrite the hardcoded system entries from their templates. The entry
     * that currently boots a system loader is replaced in place (position and
     * id kept); duplicates are dropped; a missing entry is appended.
     */
    private suspend fun syncSystemEntries() {
        val (d, p) = device.persistAddress() ?: return
        val address = "$d,$p"
        val lines = device.readEntries() ?: return
        val entries = lines.mapNotNull { BootEntry.parse(it) }.toMutableList()
        var changed = false

        for (t in SYSTEM_BOOT_ENTRIES) {
            if (!systemTargetExists(t.target)) continue
            val want = t.line(address)
            val template = BootEntry.parse(want) ?: continue
            val matches = entries.indices.filter { entries[it].targets(t.target) }
            if (matches.isEmpty()) {
                entries.add(template)
                changed = true
                continue
            }
            val at = matches.first()
            if (entries[at].toLine() != want) {
                entries[at] = template.copy(id = entries[at].id)
                changed = true
            }
            for (i in matches.drop(1).sortedDescending()) {
                entries.removeAt(i)
                changed = true
            }
        }

        if (changed) {
            val w = device.writeEntries(entries.map { it.toLine() })
            log(w.message)
            if (!w.ok) return
        }
        update { copy(entries = entries) }
    }

    // ---- deployment ----------------------------------------------------------

    private var deployCurrentFile: File? = null
    private var deployCurrentIsEfi = false
    private var deployPrepared: File? = null
    private var deployVulnFile: File? = null
    private var deployReturn: Screen = Screen.MAIN

    /**
     * Open the deployment page: from first boot when the container is not
     * initialized, or from Settings > Re-deploy. The current-slot ABL probe
     * runs automatically; the vulnerable-ABL section depends on its result.
     */
    fun openDeploy(fromSettings: Boolean) = viewModelScope.launch {
        deployReturn = if (fromSettings) Screen.SETTINGS else Screen.MAIN
        deployCurrentFile = null
        deployCurrentIsEfi = false
        deployPrepared = null
        deployVulnFile = null
        update {
            copy(screen = Screen.DEPLOY, busy = true,
                deploy = DeployUi(), message = s(R.string.deploy_probing))
        }
        val clean = !device.efispHasMz()
        val abl = device.currentAblFile()
        val vuln = abl?.let {
            val probe = device.probeGbl(it)
            logStep(probe)
            probe.ok
        }
        update {
            copy(busy = false,
                message = when (vuln) {
                    null -> s(R.string.deploy_err_no_abl)
                    true -> s(R.string.deploy_slot_vuln)
                    false -> s(R.string.deploy_slot_clean)
                },
                deploy = deploy?.copy(efispClean = clean, slotVulnerable = vuln))
        }
    }

    fun pickDeployCurrentAbl() = update {
        copy(requestFile = FileRequest(FileRequest.Kind.ABL_CURRENT, "*/*"))
    }

    fun pickDeployPatchedEfi() = update {
        copy(requestFile = FileRequest(FileRequest.Kind.PATCHED_EFI,
            "application/octet-stream"))
    }

    /** Self-provided vulnerable ABL: keep the model / black-brick warning. */
    fun pickDeployVulnAbl() = update {
        copy(prompt = Prompt(
            title = s(R.string.confirm_model_title),
            body = s(R.string.confirm_model_body),
            confirm = s(R.string.confirm_pick),
            dismiss = s(R.string.btn_cancel),
            onConfirm = {
                update {
                    copy(requestFile = FileRequest(FileRequest.Kind.ABL_VULN, "*/*"))
                }
            }))
    }

    /** Current system ABL straight from the running slot (clean system only). */
    fun usePartitionCurrent() = viewModelScope.launch {
        val abl = device.currentAblFile() ?: return@launch
        update { copy(busy = true, message = s(R.string.deploy_checking)) }
        val patched = device.patchAbl(abl)
        logOutput(patched.output)
        if (patched.file == null) {
            update { copy(busy = false, message = s(R.string.deploy_err_not_abl)) }
            return@launch
        }
        deployCurrentFile = abl
        deployCurrentIsEfi = false
        deployPrepared = patched.file
        update {
            copy(busy = false,
                deploy = deploy?.copy(
                    currentLabel = s(R.string.deploy_from_partition_label)))
        }
    }

    /** The running slot is already vulnerable: no ABL file is needed. */
    fun usePartitionVuln() = update {
        copy(deploy = deploy?.copy(
            vulnLabel = s(R.string.deploy_from_partition_label),
            vulnVerified = true))
    }

    fun downloadVulnAbl() = viewModelScope.launch {
        update { copy(busy = true, message = s(R.string.busy_download)) }
        val f = device.fetchRepoAbl()
        if (f == null) {
            update { copy(busy = false, message = s(R.string.busy_repo)) }
            return@launch
        }
        checkVulnAbl(f, s(R.string.deploy_repo_label))
    }

    private suspend fun acceptCurrentAbl(f: File, label: String) {
        update { copy(busy = true, message = s(R.string.deploy_checking)) }
        val patched = device.patchAbl(f)
        logOutput(patched.output)
        if (patched.file == null) {
            update { copy(busy = false, message = s(R.string.deploy_err_not_abl)) }
            return
        }
        deployCurrentFile = f
        deployCurrentIsEfi = false
        deployPrepared = patched.file
        update { copy(busy = false, deploy = deploy?.copy(currentLabel = label)) }
    }

    private suspend fun checkVulnAbl(f: File, label: String) {
        update { copy(busy = true, message = s(R.string.deploy_checking)) }
        val probe = device.probeGbl(f)
        logStep(probe)
        deployVulnFile = if (probe.ok) f else null
        update {
            copy(busy = false,
                message = if (probe.ok) s(R.string.deploy_status_vuln)
                          else s(R.string.deploy_status_no_vuln),
                deploy = deploy?.copy(
                    vulnLabel = if (probe.ok) label else null,
                    vulnVerified = probe.ok))
        }
    }

    private suspend fun acceptPatchedEfi(f: File, label: String) {
        val isPe = withContext(Dispatchers.IO) {
            try {
                val head = ByteArray(2)
                val n = f.inputStream().use { it.read(head) }
                n == 2 && head[0] == 'M'.code.toByte() && head[1] == 'Z'.code.toByte()
            } catch (e: Exception) {
                false
            }
        }
        if (!isPe) {
            update { copy(message = s(R.string.deploy_err_not_pe)) }
            return
        }
        deployCurrentFile = f
        deployCurrentIsEfi = true
        deployPrepared = null
        update { copy(deploy = deploy?.copy(currentLabel = label)) }
    }

    /**
     * The SAF picker returned: a uri, or null when the user backed out.
     * The request is cleared either way - a stale requestFile would make the
     * next tap a no-op, because setting the same value again does not change
     * the state and the picker effect never re-runs.
     */
    fun onFilePicked(kind: FileRequest.Kind, uri: Uri?) = viewModelScope.launch {
        update { copy(requestFile = null) }
        if (uri == null) return@launch
        val name = when (kind) {
            FileRequest.Kind.ABL_CURRENT -> "deploy_current.abl"
            FileRequest.Kind.ABL_VULN -> "deploy_vuln.abl"
            FileRequest.Kind.PATCHED_EFI -> "deploy_boot.efi"
            FileRequest.Kind.EFI -> "staged.efi"
        }
        val label = withContext(Dispatchers.IO) { displayName(uri) } ?: name
        val f = try {
            copyUri(uri, name)
        } catch (e: Exception) {
            log(s(R.string.err_copy_failed, name, e.message ?: ""))
            return@launch
        }
        when (kind) {
            FileRequest.Kind.ABL_CURRENT -> acceptCurrentAbl(f, label)
            FileRequest.Kind.ABL_VULN -> checkVulnAbl(f, label)
            FileRequest.Kind.PATCHED_EFI -> acceptPatchedEfi(f, label)
            FileRequest.Kind.EFI -> { _staged.value = f }
        }
    }

    /** SAF display name (the picked file's own name), or null. */
    private fun displayName(uri: Uri): String? = try {
        androidx.documentfile.provider.DocumentFile
            .fromSingleUri(getApplication(), uri)?.name
    } catch (e: Exception) {
        null
    }

    private suspend fun copyUri(uri: Uri, name: String): File =
        withContext(Dispatchers.IO) {
            val out = File(nativeDir, name)
            getApplication<Application>().contentResolver.openInputStream(uri)!!
                .use { i -> out.outputStream().use { o -> i.copyTo(o) } }
            out
        }

    /** Install button on the deployment page. */
    fun requestDeploy() = viewModelScope.launch { runDeploy() }

    private fun deployFail(msg: String) = update {
        copy(busy = false, message = msg, log = (log + msg).takeLast(200))
    }

    private suspend fun runDeploy() {
        val ui = _ui.value.deploy ?: return
        if (ui.done) return
        val current = deployCurrentFile ?: run {
            update { copy(message = s(R.string.deploy_need_current)) }
            return
        }
        if (ui.slotVulnerable == null) {
            update { copy(message = s(R.string.deploy_probing)) }
            return
        }
        if (ui.slotVulnerable == false &&
            (deployVulnFile == null || ui.vulnVerified != true)) {
            update { copy(message = s(R.string.deploy_need_vuln)) }
            return
        }

        update { copy(busy = true, message = s(R.string.deploy_busy)) }

        // 1. boot.efi: an already patched EFI, or a fresh patch of the ABL.
        val bootEfi = if (deployCurrentIsEfi) current else {
            val prepared = deployPrepared
            if (prepared != null) prepared else {
                val p = device.patchAbl(current)
                logOutput(p.output)
                p.file
            }
        }
        if (bootEfi == null) { deployFail(s(R.string.deploy_err_patch)); return }

        // 2. BDS core first, then the additional EFIs. The system entries
        //    point at the wrapper, so it must be on persist before the
        //    container is seeded.
        val bds = device.flashBds(File(nativeDir, SystemEfi.BDS.asset))
        log(bds.message)
        if (!bds.ok) { deployFail(bds.message); return }

        for (efi in SystemEfi.values()) {
            if (efi == SystemEfi.BDS) continue
            val name = efi.destName ?: continue
            val r = device.installEfispEfi(File(nativeDir, efi.asset), name,
                force = true)
            log(r.message)
            if (!r.ok) { deployFail(r.message); return }
        }

        // 3. Keep a non-empty container; initialize only when there is
        //    nothing usable to keep.
        val entries = device.readEntries()
        if (entries.isNullOrEmpty()) {
            val r = device.initializeContainer(containerBytes)
            log(r.message)
            if (!r.ok) { deployFail(r.message); return }
        } else {
            log(s(R.string.log_container_kept))
        }

        // 4. boot.efi: the previous one becomes boot_backup.efi.
        val inst = device.installBootEfi(bootEfi)
        log(inst.message)
        if (!inst.ok) { deployFail(inst.message); return }

        // 5. A kept container may predate the initialization marker.
        if (!device.initialized()) {
            val m = device.markInitialized()
            log(m.message)
        }

        // 6. Hardcoded, wrapper-routed system entries.
        syncSystemEntries()

        // 7. Vulnerable ABL last: only when the running ABL lacks it.
        if (ui.slotVulnerable == false) {
            val vuln = deployVulnFile!!
            val probe = device.probeGbl(vuln)
            logStep(probe)
            if (!probe.ok) { deployFail(s(R.string.deploy_err_no_vuln)); return }
            val flash = device.flashVulnAbl(vuln)
            log(flash.message)
            if (!flash.ok) { deployFail(flash.message); return }
        }

        // 8. Secure boot: only when the user has a key (i.e. enabled it).
        //    Per-file signing skips images that already verify, so the deploy
        //    never re-prompts for unchanged files.
        signAll()

        val lines = device.readEntries().orEmpty()
        update {
            copy(phase = Phase.READY, busy = false, message = s(R.string.deploy_done),
                entries = lines.mapNotNull { BootEntry.parse(it) },
                deploy = deploy?.copy(done = true))
        }
        refreshSbState()
    }

    // ---- entry editing -------------------------------------------------------

    fun requestAddEntry() = update {
        copy(requestFile = FileRequest(FileRequest.Kind.EFI, "*/*"))
    }

    fun cancelAdd() { _staged.value = null }

    /** New entry from the picked EFI file plus the dialog fields. */
    fun onAddDialog(name: String, fileName: String, args: String) =
        viewModelScope.launch {
            val f = _staged.value ?: return@launch
            _staged.value = null
            val safe = sanitizeFileName(fileName) ?: return@launch
            val (d, p) = device.persistAddress() ?: run {
                fail(s(R.string.err_no_persist_addr)); return@launch
            }
            update {
                copy(entries = entries + BootEntry(
                    id = BootEntry.newId(), name = name.trim(),
                    address = "($d,$p)", path = "\\efisp\\$safe",
                    args = args.trim(), stagedFile = f.absolutePath),
                    message = s(R.string.msg_staged))
            }
        }

    private fun sanitizeFileName(fileName: String): String? {
        val safe = fileName.trim()
        if (safe.isBlank()) { log(s(R.string.err_empty)); return null }
        if (!safe.endsWith(".efi", true)) { log(s(R.string.err_not_efi)); return null }
        if (safe.any { it !in 'a'..'z' && it !in 'A'..'Z' && it != '.' && it != '_' }) {
            log(s(R.string.err_name_chars)); return null
        }
        if (safe.startsWith('.') || safe.length <= 4 ||
            safe.substring(0, safe.length - 4).isBlank()) {
            log(s(R.string.err_name_chars)); return null
        }
        return safe
    }

    fun startEdit(index: Int) = update {
        if (index in entries.indices) copy(editing = entries[index]) else this
    }

    fun cancelEdit() = update { copy(editing = null) }

    /**
     * Apply the edit dialog. System entries only allow renaming the display
     * name. For user entries the target file may be renamed too - a rename of
     * an already-copied file becomes mv on the volume at save time.
     */
    fun onEditDialog(name: String, fileName: String, args: String) {
        val e = _ui.value.editing ?: return
        val safe = if (e.isSystem) null else sanitizeFileName(fileName)
        val renamed = safe != null && safe != e.fileName &&
            e.stagedFile == null && e.path.startsWith("\\efisp\\", true)
        if (renamed) {
            pendingRenames = pendingRenames + (e.fileName to safe)
        }
        update {
            copy(editing = null, entries = entries.map {
                if (it.id != e.id) it else {
                    var entry = it.copy(name = name.trim())
                    // System entries get their path/args from the hardcoded
                    // templates; only user entries carry editable arguments.
                    if (!it.isSystem) {
                        entry = entry.copy(args = args.trim())
                    }
                    if (safe != null && safe != e.fileName) {
                        entry = entry.copy(path = "\\efisp\\$safe")
                    }
                    entry
                }
            }, message = s(R.string.msg_staged))
        }
    }

    fun moveEntry(from: Int, to: Int) {
        if (from == to) return
        val list = _ui.value.entries.toMutableList()
        if (from !in list.indices || to !in list.indices) return
        list.add(to, list.removeAt(from))
        update { copy(entries = list, message = s(R.string.msg_order)) }
    }

    /** Current position of an entry id - drag handlers must resolve the
     *  index live because reordering moves entries under stale closures. */
    fun indexOfEntry(id: Long): Int =
        _ui.value.entries.indexOfFirst { it.id == id }

    fun removeEntry(index: Int) = viewModelScope.launch {
        val list = _ui.value.entries.toMutableList()
        if (index !in list.indices) return@launch
        val e = list.removeAt(index)
        if (e.isSystem) {
            update { copy(message = s(R.string.msg_system_protected)) }
            return@launch
        }
        val name = e.fileName
        val onEfisp = e.path.startsWith("\\efisp\\", true) && e.stagedFile == null
        if (onEfisp) {
            pendingDeletes = pendingDeletes + name
            update { copy(entries = list, message = s(R.string.msg_removed_file)) }
        } else {
            update { copy(entries = list, message = s(R.string.msg_removed_kept)) }
        }
    }

    fun save() = viewModelScope.launch {
        update { copy(busy = true, message = s(R.string.busy_save)) }
        val list = _ui.value.entries
        val added = mutableListOf<String>()

        for (e in list) {
            val src = e.stagedFile ?: continue
            val name = e.fileName
            val dst = "${Device.EFISP_DIR}/$name"
            val r = shell.run(
                "mkdir -p ${device.q(Device.EFISP_DIR)} && " +
                    "cp ${device.q(src)} ${device.q(dst)} && sync")
            if (!r.ok) { fail(s(R.string.err_copy_failed, name, r.err)); return@launch }
            added.add(name)
        }
        for (name in pendingDeletes) {
            shell.run("rm -f ${device.q("${Device.EFISP_DIR}/$name")} && sync")
        }
        for ((old, new) in pendingRenames) {
            val src = "${Device.EFISP_DIR}/$old"
            val dst = "${Device.EFISP_DIR}/$new"
            shell.run("mv ${device.q(src)} ${device.q(dst)} && sync")
        }
        pendingDeletes = emptyList()
        pendingRenames = emptyList()

        val w = device.writeEntries(list.map { it.toLine() })
        log(w.message)
        if (!w.ok) { fail(w.message); return@launch }
        update {
            copy(entries = list.map { it.copy(stagedFile = null) },
                busy = false, message = s(R.string.msg_saved))
        }
        // With secure boot on, each newly added EFI gets a fingerprint-signed
        // signature right away; a declined prompt degrades the state.
        for (name in added) {
            signIfEnabled("${Device.EFISP_DIR}/$name")
        }
    }

    /** Post-OTA retention button: confirm, then run device.otaPostUpdate(). */
    fun requestOtaUpdate() = update {
        copy(prompt = Prompt(
            title = s(R.string.ota_title),
            body = s(R.string.ota_body),
            confirm = s(R.string.ota_run),
            dismiss = s(R.string.btn_cancel),
            onConfirm = { runOtaUpdate() }))
    }

    private suspend fun runOtaUpdate() {
        update { copy(busy = true, message = s(R.string.ota_busy)) }
        // A component update the user dismissed earlier is applied in full
        // before the OTA operation; otherwise at least the BDS core is
        // refreshed first.
        if (sysUpdatePending) {
            runSystemUpdate()
            if (_ui.value.phase == Phase.ERROR) return
            update { copy(busy = true, message = s(R.string.ota_busy)) }
        } else {
            val bds = device.flashBds(File(nativeDir, SystemEfi.BDS.asset))
            log(bds.message)
            if (!bds.ok) { fail(bds.message); return }
        }
        val r = device.otaPostUpdate()
        logStep(r)
        log(r.message)
        update { copy(busy = false, message = r.message) }
        // boot_backup.efi now exists: make sure its wrapper-routed entry is
        // there (and re-route a direct boot.efi entry if this install predates
        // the wrapper).
        syncSystemEntries()
        if (r.ok) {
            // The freshly patched boot.efi must be signed when secure boot
            // is on - one biometric prompt, right here.
            signIfEnabled("${Device.EFISP_DIR}/boot.efi")
        }
    }

    // ---- navigation ----------------------------------------------------------

    fun openSettings() = update { copy(screen = Screen.SETTINGS) }
    fun backToMain() = update { copy(screen = Screen.MAIN) }
    fun backToSettings() = update { copy(screen = Screen.SETTINGS) }

    fun back() {
        when (_ui.value.screen) {
            Screen.MAIN -> {}
            Screen.SETTINGS -> backToMain()
            Screen.FASTBOOT -> backToSettings()
            Screen.PAIRING -> cancelPairing()
            Screen.DEPLOY -> update {
                copy(screen = deployReturn, deploy = null, busy = false)
            }
            Screen.LOGS -> backToSettings()
        }
    }

    fun openLogs() = update { copy(screen = Screen.LOGS) }

    fun clearLog() = update { copy(log = emptyList()) }

    fun openSecureFastboot() = viewModelScope.launch {
        update { copy(screen = Screen.FASTBOOT, authCerts = emptyList()) }
        loadAuthCerts()
    }

    // ---- secure fastboot pairing ---------------------------------------------

    private class PairException(val resId: Int) : Exception()

    private var pairJob: Job? = null
    @Volatile private var pairServer: ServerSocket? = null

    private fun pairStatus(text: String) = update {
        copy(pairing = pairing?.copy(status = text))
    }

    private suspend fun loadAuthCerts() {
        val certs = mutableListOf<AuthCert>()
        for (name in device.listAuthCertFiles()) {
            val local = File(nativeDir, "auth_list.cer")
            val got = device.readAuthCert(name, local)
            val der = got?.let { withContext(Dispatchers.IO) { it.readBytes() } }
            val cert = der?.let { parseCert(it) }
            certs.add(AuthCert(name, cert?.let { cnOf(it) } ?: name,
                               der?.let { fingerprint(it) } ?: ""))
        }
        update { copy(authCerts = certs) }
    }

    fun deleteAuthCert(name: String) = viewModelScope.launch {
        val r = device.deleteAuthCert(name)
        log(r.message)
        update { copy(message = r.message) }
        loadAuthCerts()
    }

    /** Open the pairing page and start the one-shot pairing service. */
    fun startPairing() {
        if (pairJob?.isActive == true) return
        val code = (100000 + SecureRandom().nextInt(900000)).toString()
        val ip = localIpv4()
        update {
            copy(screen = Screen.PAIRING,
                 pairing = PairingUi(
                     address = if (ip == null) s(R.string.pair_no_ip)
                               else "$ip:$PAIR_PORT",
                     code = code,
                     status = if (ip == null) s(R.string.pair_no_ip)
                              else s(R.string.pair_waiting)))
        }
        if (ip == null) return
        pairJob = viewModelScope.launch(Dispatchers.IO) { runPairing(code) }
    }

    fun cancelPairing() {
        pairJob?.cancel()
        pairJob = null
        try { pairServer?.close() } catch (_: Exception) {}
        pairServer = null
        update { copy(screen = Screen.FASTBOOT, pairing = null) }
        // A finished pairing added a certificate; leaving early only closes
        // the service, but reloading is cheap and covers both cases.
        viewModelScope.launch { loadAuthCerts() }
    }

    /** Back from the PAIRING page after success. */
    fun finishPairing() = cancelPairing()

    override fun onCleared() {
        pairJob?.cancel()
        try { pairServer?.close() } catch (_: Exception) {}
        pairServer = null
        super.onCleared()
    }

    private suspend fun runPairing(code: String) {
        var server: ServerSocket? = null
        var client: Socket? = null
        try {
            server = ServerSocket()
            server.reuseAddress = true
            server.bind(InetSocketAddress(PAIR_PORT))
            server.soTimeout = PAIR_ACCEPT_TIMEOUT_MS
            pairServer = server
            client = server.accept()
            client.soTimeout = PAIR_READ_TIMEOUT_MS
            pairStatus(s(R.string.pair_connected))
            pairExchange(client, code)
        } catch (e: PairException) {
            pairStatus(s(R.string.pair_failed, s(e.resId)))
        } catch (e: Exception) {
            if (pairJob?.isActive == true) {
                pairStatus(s(R.string.pair_failed,
                    e.message ?: e.javaClass.simpleName))
            }
        } finally {
            try { client?.close() } catch (_: Exception) {}
            try { server?.close() } catch (_: Exception) {}
            pairServer = null
        }
    }

    /**
     * One pairing exchange, mirroring `fastboot_auth pair`:
     *
     *   PAIR <code>   -> OK | ERR
     *   CERT <b64>    -> CHALLENGE <b64 nonce>
     *   SIG <b64>     -> DONE <fingerprint> | ERR
     *
     * The challenge proves the computer holds the key inside the certificate
     * it just handed over. Only then is the certificate installed.
     */
    private suspend fun pairExchange(sock: Socket, code: String) {
        val input = BufferedInputStream(sock.getInputStream())
        val output = sock.getOutputStream()

        fun send(line: String) {
            output.write((line + "\n").toByteArray())
            output.flush()
        }
        fun fail(resId: Int, wire: String): Nothing {
            send(wire)
            throw PairException(resId)
        }

        val hello = readPairLine(input) ?: fail(R.string.pair_err_protocol, "ERR protocol")
        if (!hello.startsWith("PAIR ")) fail(R.string.pair_err_protocol, "ERR protocol")
        if (hello.substring(5) != code) fail(R.string.pair_err_code, "ERR code")
        send("OK")

        val certLine = readPairLine(input) ?: fail(R.string.pair_err_protocol, "ERR protocol")
        if (!certLine.startsWith("CERT ")) fail(R.string.pair_err_protocol, "ERR protocol")
        val der = decodeB64(certLine.substring(5))
            ?: fail(R.string.pair_err_cert, "ERR cert")
        if (der.isEmpty() || der.size > PAIR_CERT_MAX) {
            fail(R.string.pair_err_cert, "ERR cert")
        }
        val cert = parseCert(der) ?: fail(R.string.pair_err_cert, "ERR cert")
        if (cert.publicKey.algorithm != "RSA") fail(R.string.pair_err_key, "ERR key")
        val cn = cnOf(cert)

        val nonce = ByteArray(32)
        SecureRandom().nextBytes(nonce)
        send("CHALLENGE " + encodeB64(nonce))

        val sigLine = readPairLine(input) ?: fail(R.string.pair_err_protocol, "ERR protocol")
        if (!sigLine.startsWith("SIG ")) fail(R.string.pair_err_protocol, "ERR protocol")
        val sig = decodeB64(sigLine.substring(4)) ?: fail(R.string.pair_err_sig, "ERR sig")
        val verified = try {
            val verifier = Signature.getInstance("SHA256withRSA")
            verifier.initVerify(cert.publicKey)
            verifier.update(nonce)
            verifier.verify(sig)
        } catch (e: Exception) {
            false
        }
        if (!verified) fail(R.string.pair_err_sig, "ERR sig")

        val installed = device.installAuthCert(der, sanitizeCertName(cn))
        if (!installed.ok) {
            send("ERR store")
            throw PairException(R.string.pair_err_store)
        }
        send("DONE " + fingerprint(der))
        log(s(R.string.log_paired, cn))
        update { copy(pairing = pairing?.copy(done = true,
            status = s(R.string.pair_success, cn))) }
    }

    private fun readPairLine(input: InputStream): String? {
        val sb = StringBuilder()
        while (sb.length < PAIR_LINE_MAX) {
            val b = input.read()
            if (b < 0) return if (sb.isEmpty()) null else sb.toString()
            if (b == '\n'.code) return sb.toString()
            if (b != '\r'.code) sb.append(b.toChar())
        }
        return null
    }

    private fun encodeB64(data: ByteArray): String =
        Base64.encodeToString(data, Base64.NO_WRAP)

    private fun decodeB64(text: String): ByteArray? = try {
        Base64.decode(text, Base64.DEFAULT)
    } catch (e: Exception) {
        null
    }

    private fun parseCert(der: ByteArray): X509Certificate? = try {
        CertificateFactory.getInstance("X.509")
            .generateCertificate(der.inputStream()) as X509Certificate
    } catch (e: Exception) {
        null
    }

    /** CN of the subject DN, without a regex (RFC 2253: CN=user@host). */
    private fun cnOf(cert: X509Certificate): String {
        val dn = cert.subjectX500Principal.name
        var at = 0
        while (at < dn.length) {
            val idx = dn.indexOf("CN=", at)
            if (idx < 0) break
            if (idx == 0 || dn[idx - 1] == ',' || dn[idx - 1] == '+') {
                val sb = StringBuilder()
                var i = idx + 3
                while (i < dn.length && dn[i] != ',' && dn[i] != '+') {
                    if (dn[i] == '\\' && i + 1 < dn.length) {
                        sb.append(dn[i + 1])
                        i += 2
                    } else {
                        sb.append(dn[i])
                        i++
                    }
                }
                if (sb.isNotEmpty()) return sb.toString()
            }
            at = idx + 3
        }
        return "computer"
    }

    private fun fingerprint(der: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(der)
            .joinToString("") { "%02X".format(it) }

    /** CN -> a FAT-safe file name: one file per computer. */
    private fun sanitizeCertName(cn: String): String {
        val sb = StringBuilder()
        for (c in cn) {
            sb.append(if (c.isLetterOrDigit() || c == '@' || c == '.' ||
                          c == '_' || c == '-') c else '_')
        }
        var name = sb.toString().trim('_', '.')
        if (name.isEmpty()) name = "computer"
        if (name.length > 64) name = name.substring(0, 64)
        return "$name.cer"
    }

    /** First non-loopback IPv4, preferring Wi-Fi. */
    private fun localIpv4(): String? {
        var fallback: String? = null
        try {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (addr in nif.inetAddresses) {
                    val v4 = addr as? Inet4Address ?: continue
                    val ip = v4.hostAddress ?: continue
                    if (nif.name.startsWith("wlan")) return ip
                    if (fallback == null) fallback = ip
                }
            }
        } catch (_: Exception) {
        }
        return fallback
    }

    // ---- secure boot ---------------------------------------------------------

    /** Handled by the UI: run the BiometricPrompt and call onSignResult(). */
    private var signWaiter: CompletableDeferred<ByteArray?>? = null

    suspend fun requestSignature(digestInfo: ByteArray, fileName: String):
        ByteArray? {
        val def = CompletableDeferred<ByteArray?>()
        signWaiter = def
        update { copy(signRequest = SignRequest(digestInfo, fileName)) }
        return def.await()
    }

    /** Called by the UI with the signature, or null when the prompt failed. */
    fun onSignResult(signature: ByteArray?) {
        signWaiter?.complete(signature)
        signWaiter = null
        update { copy(signRequest = null) }
    }

    /**
     * Sign one EFI on persist: read, strip, prompt, assemble, write back.
     * A file that already carries a valid signature by the current keystore
     * certificate is skipped without prompting.
     */
    private suspend fun signFileFlow(path: String): Boolean {
        val local = File(nativeDir, "sb_work.efi")
        if (device.readRootFile(path, local) == null) {
            log(s(R.string.err_sb_read, path)); return false
        }
        val blob = withContext(Dispatchers.IO) { local.readBytes() }
        val cert = HardwareKey.certDer()
        if (cert == null) {
            log(s(R.string.err_sb_nokey)); return false
        }
        val trusted = withContext(Dispatchers.IO) {
            try {
                PeSigner.isTrustedSignature(blob, cert)
            } catch (e: Exception) {
                false
            }
        }
        if (trusted) return true

        // PE parsing, hashing and assembly copy the whole image; the caller
        // runs in viewModelScope (Dispatchers.Main), so keep them off it.
        val (stripped, canonical) = try {
            withContext(Dispatchers.IO) { PeSigner.prepare(blob) }
        } catch (e: Exception) {
            log(s(R.string.err_sb_pe, path)); return false
        }
        // Hash locally; only the 51-byte DigestInfo enters the secure
        // element (streaming megabytes into StrongBox froze the app).
        val digestInfo = withContext(Dispatchers.IO) {
            PeSigner.digestInfo(PeSigner.canonicalHash(canonical))
        }
        val sig = requestSignature(digestInfo, path.substringAfterLast('/'))
        if (sig == null) {
            log(s(R.string.err_sb_auth, path.substringAfterLast('/')))
            return false
        }
        try {
            withContext(Dispatchers.IO) {
                // Remove a possible stale root-owned copy first: overwriting
                // it directly was an EACCES crash.
                if (local.exists() && !local.canWrite()) local.delete()
                local.writeBytes(PeSigner.assembleSigned(stripped, cert, sig))
            }
        } catch (e: Exception) {
            log(s(R.string.err_sb_write, local.name)); return false
        }
        if (!device.writeRootFile(local, path)) {
            log(s(R.string.err_sb_write, path)); return false
        }
        log(s(R.string.log_sb_signed, path.substringAfterLast('/')))
        return true
    }

    /** Re-check DISABLED / ENABLED / DEGRADED (unsigned or untrusted images). */
    fun refreshSbState() = viewModelScope.launch {
        if (!device.bootCertExists()) {
            update { copy(sbState = SbState.DISABLED) }; return@launch
        }
        val trusted = HardwareKey.certDer()
        var ok = trusted != null
        for (path in device.listEfispEfiFiles()) {
            if (!ok) break
            val local = File(nativeDir, "sb_check.efi")
            val got = device.readRootFile(path, local)
            val signed = got != null && withContext(Dispatchers.IO) {
                try {
                    PeSigner.isTrustedSignature(local.readBytes(), trusted)
                } catch (e: Exception) {
                    false
                }
            }
            if (!signed) ok = false
        }
        update { copy(sbState = if (ok) SbState.ENABLED else SbState.DEGRADED) }
    }

    /** Enable flow: create the key when needed, then sign what is unsigned. */
    fun requestEnableSecureBoot() = update {
        copy(prompt = Prompt(
            title = s(R.string.sb_enable_title),
            body = s(R.string.sb_enable_body),
            confirm = s(R.string.sb_enable_run),
            dismiss = s(R.string.btn_cancel),
            onConfirm = { enableSecureBoot() }))
    }

    private suspend fun enableSecureBoot() {
        update { copy(busy = true, message = s(R.string.sb_busy)) }
        withContext(Dispatchers.IO) { HardwareKey.generate() }
        val ok = signAll()
        update {
            copy(busy = false,
                message = if (ok) s(R.string.sb_msg_signed_ok)
                          else s(R.string.sb_msg_aborted))
        }
        refreshSbState()
    }

    /** Disable flow: drop the certificate, then the keystore key. */
    fun requestDisableSecureBoot() = update {
        copy(prompt = Prompt(
            title = s(R.string.sb_disable_title),
            body = s(R.string.sb_disable_body),
            confirm = s(R.string.sb_disable_run),
            dismiss = s(R.string.btn_cancel),
            onConfirm = { disableSecureBoot() }))
    }

    private suspend fun disableSecureBoot() {
        update { copy(busy = true, message = s(R.string.sb_busy)) }
        val rm = device.deleteBootCert()
        log(rm.message)
        if (!rm.ok && device.bootCertExists()) {
            update { copy(busy = false, message = rm.message) }
            return
        }
        HardwareKey.delete()
        update { copy(busy = false, message = s(R.string.sb_disable_done)) }
        refreshSbState()
    }

    /**
     * Sign every EFI under persist/efisp that does not already carry a valid
     * signature by the current key, then write BOOT_CERTS. A device without
     * the secure-boot key never enabled it: do nothing at all.
     */
    private suspend fun signAll(): Boolean {
        if (!HardwareKey.exists()) return false
        update { copy(busy = true, message = s(R.string.sb_busy)) }
        val cert = HardwareKey.certDer()
        if (cert == null) {
            log(s(R.string.err_sb_nokey))
            update { copy(busy = false) }
            return false
        }
        val files = device.listEfispEfiFiles()
        if (files.isEmpty()) {
            log(s(R.string.err_sb_nofiles))
            update { copy(busy = false) }
            return false
        }
        var ok = true
        for (path in files) {
            if (!signFileFlow(path)) {
                log(s(R.string.err_sb_aborted))
                ok = false
                break
            }
        }
        if (ok) {
            val w = device.writeBootCert(cert)
            log(w.message)
            ok = w.ok
        }
        update { copy(busy = false) }
        refreshSbState()
        return ok
    }

    /** Sign a single new/updated file when secure boot is already on. */
    private suspend fun signIfEnabled(path: String) {
        if (!device.bootCertExists()) return
        update { copy(busy = true, message = s(R.string.sb_busy_sign)) }
        if (signFileFlow(path)) {
            update { copy(busy = false, message = s(R.string.sb_msg_signed_ok)) }
        } else {
            update { copy(busy = false, message = s(R.string.sb_msg_degraded)) }
        }
        refreshSbState()
    }

    fun dismissPrompt() = viewModelScope.launch {
        val p = _ui.value.prompt ?: return@launch
        update { copy(prompt = null) }
        p.onDismiss?.invoke()
    }

    fun confirmPrompt() = viewModelScope.launch {
        val p = _ui.value.prompt ?: return@launch
        update { copy(prompt = null) }
        p.onConfirm()
    }
}
