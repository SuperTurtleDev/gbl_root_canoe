package com.gblbds.manager.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DragHandle
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import com.gblbds.manager.R
import com.gblbds.manager.core.AppViewModel
import com.gblbds.manager.core.BootEntry
import com.gblbds.manager.core.DeployUi
import com.gblbds.manager.core.Phase
import com.gblbds.manager.core.SbState
import com.gblbds.manager.core.Screen
import com.gblbds.manager.core.UiState
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppScreen(state: UiState, staged: File?, vm: AppViewModel) {
    MaterialTheme(colorScheme =
        if (isSystemInDarkTheme()) darkColorScheme() else lightColorScheme()) {
        BackHandler(enabled = state.screen != Screen.MAIN) { vm.back() }
        Scaffold(
            topBar = {
                TopAppBar(
                    title = {
                        Text(when (state.screen) {
                            Screen.MAIN -> stringResource(R.string.app_name)
                            Screen.SETTINGS -> stringResource(R.string.settings)
                            Screen.FASTBOOT -> stringResource(R.string.sf_title)
                            Screen.PAIRING -> stringResource(R.string.pair_title)
                            Screen.DEPLOY -> stringResource(R.string.deploy_title)
                            Screen.LOGS -> stringResource(R.string.log_title)
                        })
                    },
                    navigationIcon = {
                        if (state.screen != Screen.MAIN) {
                            IconButton(onClick = { vm.back() }) {
                                Icon(Icons.AutoMirrored.Filled.ArrowBack,
                                    stringResource(R.string.btn_back))
                            }
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.primaryContainer),
                    actions = {
                        if (state.screen == Screen.MAIN) {
                            IconButton(onClick = { vm.openSettings() }) {
                                Icon(Icons.Filled.Settings,
                                    stringResource(R.string.settings))
                            }
                        }
                    })
            },
            floatingActionButton = {
                if (state.screen == Screen.MAIN && state.phase == Phase.READY) {
                    FloatingActionButton(onClick = { vm.requestAddEntry() }) {
                        Icon(Icons.Filled.Add, stringResource(R.string.add_title))
                    }
                }
            },
        ) { pad ->
            val content = Modifier.fillMaxSize().padding(pad)
            when (state.screen) {
                Screen.MAIN -> MainScreen(state, vm, content)
                Screen.SETTINGS -> SettingsScreen(state, vm, content)
                Screen.FASTBOOT -> SecureFastbootScreen(state, vm, content)
                Screen.PAIRING -> PairingScreen(state, vm, content)
                Screen.DEPLOY -> DeployScreen(state, vm, content)
                Screen.LOGS -> LogsScreen(state, vm, content)
            }
        }

        state.prompt?.let { p ->
            AlertDialog(
                onDismissRequest = { vm.dismissPrompt() },
                title = { Text(p.title) },
                text = { Text(p.body) },
                confirmButton = {
                    TextButton(onClick = { vm.confirmPrompt() }) { Text(p.confirm) }
                },
                dismissButton = {
                    p.dismiss?.let {
                        TextButton(onClick = { vm.dismissPrompt() }) { Text(it) }
                    }
                },
            )
        }

        if (staged != null) AddEntryDialog(vm)
        state.editing?.let { e -> EditEntryDialog(e, vm) }
    }
}

@Composable
private fun MainScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (state.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        Text(state.message, style = MaterialTheme.typography.bodyMedium)

        when (state.phase) {
            Phase.LOADING -> Centered(stringResource(R.string.checking))
            Phase.NO_ROOT -> Centered(stringResource(R.string.need_root))
            Phase.ERROR -> Centered(state.message)
            Phase.READY -> EntryList(state, vm)
        }
    }
}

/** Full settings page: secure boot (BOOT_CERTS) and secure fastboot entries. */
@Composable
private fun SettingsScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.sb_state_title),
            style = MaterialTheme.typography.titleSmall)
        val (label, color) = when (state.sbState) {
            SbState.DISABLED ->
                stringResource(R.string.sb_state_disabled) to
                    MaterialTheme.colorScheme.onSurfaceVariant
            SbState.ENABLED ->
                stringResource(R.string.sb_state_enabled) to
                    MaterialTheme.colorScheme.primary
            SbState.DEGRADED ->
                stringResource(R.string.sb_state_degraded) to
                    MaterialTheme.colorScheme.error
        }
        Text(label, color = color, style = MaterialTheme.typography.bodyMedium)
        if (state.sbState == SbState.DEGRADED) {
            Text(stringResource(R.string.sb_degraded_hint),
                style = MaterialTheme.typography.bodySmall)
        }
        Text(stringResource(R.string.sb_key_hint),
            style = MaterialTheme.typography.bodySmall)
        Button(
            onClick = { vm.requestEnableSecureBoot() },
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(
                if (state.sbState == SbState.DISABLED) R.string.sb_enable_run
                else R.string.sb_resign))
        }
        if (state.sbState != SbState.DISABLED) {
            OutlinedButton(
                onClick = { vm.requestDisableSecureBoot() },
                enabled = !state.busy,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.sb_disable_run))
            }
        }

        HorizontalDivider()

        Text(stringResource(R.string.sf_title),
            style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.sf_settings_hint),
            style = MaterialTheme.typography.bodySmall)
        Button(
            onClick = { vm.openSecureFastboot() },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.sf_open))
        }

        HorizontalDivider()

        Text(stringResource(R.string.deploy_title),
            style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.deploy_settings_hint),
            style = MaterialTheme.typography.bodySmall)
        Button(
            onClick = { vm.openDeploy(true) },
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.deploy_settings_entry))
        }

        HorizontalDivider()

        Button(
            onClick = { vm.openLogs() },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.log_title))
        }
    }
}

/** Paired computers (AUTH_CERTS) with removal, plus the pairing entry point. */
@Composable
private fun SecureFastbootScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.sf_paired_title),
            style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.sf_reboot_hint),
            style = MaterialTheme.typography.bodySmall)

        if (state.authCerts.isEmpty()) {
            Centered(stringResource(R.string.sf_empty))
        } else {
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(state.authCerts, key = { it.fileName }) { cert ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(cert.cn, style = MaterialTheme.typography.bodyLarge)
                            if (cert.fingerprint.isNotEmpty()) {
                                Text(stringResource(R.string.sf_fingerprint,
                                        cert.fingerprint.take(32)),
                                    style = MaterialTheme.typography.bodySmall,
                                    fontFamily = FontFamily.Monospace)
                            }
                        }
                        IconButton(onClick = { vm.deleteAuthCert(cert.fileName) }) {
                            Icon(Icons.Filled.Delete,
                                stringResource(R.string.sf_delete))
                        }
                    }
                    HorizontalDivider()
                }
            }
        }

        Button(
            onClick = { vm.startPairing() },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.sf_pair_btn))
        }
    }
}

/** Pairing page: shows the address and code the computer must use. */
@Composable
private fun PairingScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    val p = state.pairing
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp),
        horizontalAlignment = Alignment.CenterHorizontally) {
        Text(stringResource(R.string.pair_run_hint),
            style = MaterialTheme.typography.bodyMedium)
        if (p != null) {
            Text("fastboot_auth pair ${p.address}",
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(8.dp))
            Text(stringResource(R.string.pair_code_label),
                style = MaterialTheme.typography.titleSmall)
            Text(p.code,
                style = MaterialTheme.typography.displayMedium,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(8.dp))
            Text(p.status, style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center)
            Spacer(Modifier.height(8.dp))
            if (p.done) {
                Button(onClick = { vm.finishPairing() }) {
                    Text(stringResource(R.string.pair_done))
                }
            } else {
                TextButton(onClick = { vm.cancelPairing() }) {
                    Text(stringResource(R.string.btn_cancel))
                }
            }
        }
    }
}

/**
 * Deployment page. Two sources: the current system ABL (patched into
 * boot.efi, or an already patched EFI) and - only when the running ABL lacks
 * the efisp vulnerability - an ABL that carries it.
 */
@Composable
private fun DeployScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    val d = state.deploy ?: DeployUi()
    val idle = !state.busy && !d.done
    Column(
        modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (state.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        Text(state.message, style = MaterialTheme.typography.bodyMedium)

        // ---- current system ABL ---------------------------------------------
        Text(stringResource(R.string.deploy_section_current),
            style = MaterialTheme.typography.titleSmall)
        Text(d.currentLabel ?: stringResource(R.string.deploy_status_none),
            color = if (d.currentLabel != null) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.bodyMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = { vm.pickDeployCurrentAbl() },
                enabled = idle,
                modifier = Modifier.weight(1f),
            ) { Text(stringResource(R.string.deploy_pick_abl)) }
            OutlinedButton(
                onClick = { vm.pickDeployPatchedEfi() },
                enabled = idle,
                modifier = Modifier.weight(1f),
            ) { Text(stringResource(R.string.deploy_pick_patched)) }
        }
        OutlinedButton(
            onClick = { vm.usePartitionCurrent() },
            enabled = idle && d.efispClean,
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.deploy_from_partition)) }
        if (!d.efispClean) {
            Text(stringResource(R.string.deploy_partition_hint),
                style = MaterialTheme.typography.bodySmall)
        }

        HorizontalDivider()

        // ---- vulnerable ABL ---------------------------------------------------
        Text(stringResource(R.string.deploy_section_vuln),
            style = MaterialTheme.typography.titleSmall)
        val vulnStatus = when {
            d.slotVulnerable == true ->
                stringResource(R.string.deploy_status_vuln)
            d.vulnVerified == true ->
                stringResource(R.string.deploy_status_vuln) +
                    (d.vulnLabel?.let { " · $it" } ?: "")
            d.vulnVerified == false ->
                stringResource(R.string.deploy_status_no_vuln)
            else -> stringResource(R.string.deploy_status_none)
        }
        Text(vulnStatus,
            color = when {
                d.slotVulnerable == true || d.vulnVerified == true ->
                    MaterialTheme.colorScheme.primary
                d.vulnVerified == false -> MaterialTheme.colorScheme.error
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
            style = MaterialTheme.typography.bodyMedium)
        // A running ABL that already carries the vulnerability disables the
        // whole section: there is nothing to flash.
        val vulnEditable = idle && d.slotVulnerable == false
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = { vm.pickDeployVulnAbl() },
                enabled = vulnEditable,
                modifier = Modifier.weight(1f),
            ) { Text(stringResource(R.string.deploy_pick_abl)) }
            OutlinedButton(
                onClick = { vm.downloadVulnAbl() },
                enabled = vulnEditable,
                modifier = Modifier.weight(1f),
            ) { Text(stringResource(R.string.deploy_cloud)) }
        }
        OutlinedButton(
            onClick = { vm.usePartitionVuln() },
            enabled = vulnEditable && d.efispClean,
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.deploy_from_partition)) }
        if (d.slotVulnerable == true) {
            Text(stringResource(R.string.deploy_vuln_disabled_hint),
                style = MaterialTheme.typography.bodySmall)
        }

        HorizontalDivider()

        val canInstall = idle && d.currentLabel != null &&
            d.slotVulnerable != null &&
            (d.slotVulnerable == true || d.vulnVerified == true)
        Button(
            onClick = { vm.requestDeploy() },
            enabled = canInstall,
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.deploy_install)) }
        if (d.done) {
            Text(stringResource(R.string.deploy_reboot_hint),
                style = MaterialTheme.typography.bodySmall)
        }
    }
}

/** Standalone log viewer (Settings > Log). */
@Composable
private fun LogsScreen(state: UiState, vm: AppViewModel, modifier: Modifier) {
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            TextButton(onClick = { vm.clearLog() }) {
                Text(stringResource(R.string.logs_clear))
            }
        }
        if (state.log.isEmpty()) {
            Centered(stringResource(R.string.logs_empty))
        } else {
            LazyColumn(Modifier.weight(1f, fill = false)) {
                items(state.log) { l ->
                    Text(l, style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

@Composable
private fun Centered(text: String) {
    Box(Modifier.fillMaxWidth().padding(24.dp), Alignment.Center) { Text(text) }
}

@Composable
private fun ColumnScope.EntryList(state: UiState, vm: AppViewModel) {
    val listState = rememberLazyListState()
    val density = LocalDensity.current

    // Drag state keyed by the stable entry id, never by the slot index: a
    // reorder moves entries under the gesture closure, and a stale index would
    // grab the wrong row on the next drag. The index is resolved live from the
    // ViewModel on every step. The dragged card follows the finger via
    // translationY; crossing a slot height swaps it with the neighbour.
    var draggingId by remember { mutableLongStateOf(-1L) }
    var dragOffset by remember { mutableFloatStateOf(0f) }

    fun slotHeight(): Float {
        val info = listState.layoutInfo.visibleItemsInfo
        if (info.isEmpty()) return with(density) { 78.dp.toPx() }
        return info.sumOf { it.size }.toFloat() / info.size +
            with(density) { 6.dp.toPx() }
    }

    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Button(onClick = { vm.save() }, enabled = !state.busy) {
            Text(stringResource(R.string.btn_save))
        }
        OutlinedButton(onClick = { vm.requestOtaUpdate() }, enabled = !state.busy) {
            Text(stringResource(R.string.ota_btn))
        }
        Text(stringResource(R.string.hint_drag),
            style = MaterialTheme.typography.bodySmall)
    }

    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxWidth().weight(1f),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        itemsIndexed(state.entries, key = { _, e -> e.id }) { i, e ->
            val isDragging = e.id == draggingId
            ElevatedCard(
                Modifier.fillMaxWidth()
                    .zIndex(if (isDragging) 1f else 0f)
                    .animateItem()
                    .graphicsLayer {
                        translationY = if (isDragging) dragOffset else 0f
                        scaleX = if (isDragging) 1.03f else 1f
                        scaleY = if (isDragging) 1.03f else 1f
                        alpha = if (isDragging) 0.95f else 1f
                        shadowElevation = if (isDragging) 24f else 0f
                    },
                colors = CardDefaults.elevatedCardColors(
                    containerColor = if (isDragging)
                        MaterialTheme.colorScheme.primaryContainer
                    else MaterialTheme.colorScheme.surface,
                ),
            ) {
                Row(Modifier.padding(start = 4.dp, end = 12.dp, top = 8.dp, bottom = 8.dp),
                    verticalAlignment = Alignment.CenterVertically) {

                    // Drag handle: press and move; the card follows the finger.
                    // Only the stable e.id is captured - the current index is
                    // resolved through the ViewModel on every step so a
                    // reordered list can never hand the gesture a stale slot.
                    Icon(Icons.Filled.DragHandle,
                        stringResource(R.string.drag_handle),
                        Modifier
                            .pointerInput(e.id) {
                                detectDragGestures(
                                    onDragStart = {
                                        draggingId = e.id
                                        dragOffset = 0f
                                    },
                                    onDrag = { change, amount ->
                                        change.consume()
                                        dragOffset += amount.y
                                        val h = slotHeight()
                                        while (h > 0f && abs(dragOffset) >= h) {
                                            val from = vm.indexOfEntry(draggingId)
                                            if (from < 0) break
                                            val to = if (dragOffset > 0f) from + 1 else from - 1
                                            if (to !in state.entries.indices) {
                                                break   // at the list edge
                                            }
                                            vm.moveEntry(from, to)
                                            dragOffset -= if (to > from) h else -h
                                        }
                                    },
                                    onDragEnd = { draggingId = -1L; dragOffset = 0f },
                                    onDragCancel = { draggingId = -1L; dragOffset = 0f },
                                )
                            }
                            .padding(8.dp))

                    Column(Modifier.weight(1f)
                        .clickable { vm.startEdit(i) }) {
                        Row(verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(e.name, style = MaterialTheme.typography.titleMedium)
                            if (e.isSystem) {
                                AssistChip(onClick = {}, label = {
                                    Text(stringResource(R.string.entry_system),
                                        style = MaterialTheme.typography.labelSmall)
                                })
                            }
                        }
                        Text(
                            "${e.address.orEmpty()}${e.path}" +
                                (if (e.args.isNotBlank()) " ${e.args}" else ""),
                            style = MaterialTheme.typography.bodySmall)
                        if (e.stagedFile != null) Text(stringResource(R.string.staged_pending),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary)
                    }

                    if (!e.isSystem) {
                        IconButton(onClick = { vm.removeEntry(i) }) {
                            Icon(Icons.Filled.Delete, stringResource(R.string.delete))
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun AddEntryDialog(vm: AppViewModel) {
    var name by remember { mutableStateOf("") }
    var file by remember { mutableStateOf("") }
    var args by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = { vm.cancelAdd() },
        title = { Text(stringResource(R.string.add_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.add_hint),
                    style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(value = name, onValueChange = { name = it },
                    label = { Text(stringResource(R.string.field_name)) },
                    singleLine = true)
                OutlinedTextField(value = file, onValueChange = { file = it },
                    label = { Text(stringResource(R.string.field_file)) },
                    singleLine = true)
                OutlinedTextField(value = args, onValueChange = { args = it },
                    label = { Text(stringResource(R.string.field_args)) },
                    singleLine = true)
            }
        },
        confirmButton = {
            TextButton(onClick = { vm.onAddDialog(name, file, args) }) {
                Text(stringResource(R.string.btn_ok))
            }
        },
        dismissButton = {
            TextButton(onClick = { vm.cancelAdd() }) {
                Text(stringResource(R.string.btn_cancel))
            }
        },
    )
}

@Composable
private fun EditEntryDialog(e: BootEntry, vm: AppViewModel) {
    var name by remember(e.id) { mutableStateOf(e.name) }
    var file by remember(e.id) { mutableStateOf(e.fileName) }
    var args by remember(e.id) { mutableStateOf(e.args) }
    AlertDialog(
        onDismissRequest = { vm.cancelEdit() },
        title = { Text(stringResource(R.string.edit_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (e.isSystem) {
                    Text(stringResource(R.string.edit_hint_system_name),
                        style = MaterialTheme.typography.bodySmall)
                }
                OutlinedTextField(value = name, onValueChange = { name = it },
                    label = { Text(stringResource(R.string.field_name)) },
                    singleLine = true)
                if (!e.isSystem) {
                    OutlinedTextField(value = file, onValueChange = { file = it },
                        label = { Text(stringResource(R.string.field_file)) },
                        singleLine = true)
                    OutlinedTextField(value = args, onValueChange = { args = it },
                        label = { Text(stringResource(R.string.field_args)) },
                        singleLine = true)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { vm.onEditDialog(name, file, args) }) {
                Text(stringResource(R.string.btn_ok))
            }
        },
        dismissButton = {
            TextButton(onClick = { vm.cancelEdit() }) {
                Text(stringResource(R.string.btn_cancel))
            }
        },
    )
}
