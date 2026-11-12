const IMAGE_NAMES = ["abl"];

const state = {
  confirmStep: 0,
  pendingAction: null,
  moduleDir: "",
  scriptPath: "",
  status: null,
  pollTimer: null,
  prevStatusRaw: "",
  lang: "zh"
};

const i18n = {
  zh: {
    pageTitle: "假回锁 - BL Flasher",
    ksuWebUI: "KernelSU Module WebUI",
    heroDesc: "自动识别当前活动槽位，若新版本存在GBL漏洞则跳过BL刷写；将BL镜像刷写到另一槽位，并将破解ABL放入persist的efisp目录、BDS刷入efisp分区",
    slotStatus: "槽位状态",
    refresh: "刷新",
    currentSlot: "当前槽位",
    targetSlot: "目标槽位",
    imageCount: "镜像数量",
    taskStatus: "任务状态",
    flash: "刷写到另一槽位",
    bdsTools: "仅更新BDS和Tools",
    confirmBdsTools: "确认更新 BDS/Tools",
    modalBdsStep1: "将把 BDS.efi 刷入 efisp 分区，复制模块自带的 efisp 文件夹和 ENTRIES.CFG 到 persist。不会改动 ABL 与 boot.efi。",
    modalBdsStep2: "第二次确认: 这是高风险写入操作，错误的 BDS 或 efisp 写入可能导致无法进入启动菜单。确认后将立即开始。",
    toastStartBdsTools: "BDS/Tools 更新任务已启动",
    toastBdsToolsDone: "BDS/Tools 更新完成",
    clearLog: "清空日志",
    updateEfisp: "更新 efisp（默认开启）",
    debugMode: "调试模式（仅处理不刷写，efisp 目录使用模块 tmp/efisp）",
    warning: "刷写对象是 bootloader 相关分区，风险较高。开始前请确认镜像与机型严格匹配。",
    imageMap: "镜像映射",
    partition: "分区名",
    source: "源分区 (当前槽位)",
    target: "目标分区",
    action: "操作",
    waiting: "等待读取模块状态",
    log: "实时日志",
    autoPoll: "自动轮询最近 200 行",
    risk: "高风险操作",
    confirmFlash: "确认刷写",
    cancel: "取消",
    continue: "继续",
    waitingStatus: "等待检测",
    slotUnknown: "槽位未知",
    logWaiting: "等待日志输出...",
    toastRunning: "已有刷写任务在运行",
    toastStartDebug: "调试任务已启动",
    toastStartFlash: "刷写任务已启动",
    toastDebugDone: "调试完成",
    toastFlashDone: "刷写已完成",
    toastBlDone: "BL 刷写完成，但 efisp 未更新",
    toastFailed: "任务已结束（失败）",
    toastStartError: "任务启动失败",
    toastLogBusy: "任务运行中，暂时不能清空日志",
    toastLogCleared: "日志已清空",
    modalStep1Debug: "调试模式：将执行所有处理流程但不刷写分区，生成的文件保存在 tmp 目录。",
    modalStep1Normal: (slot) => `第一次确认: 将把当前槽位的 BL 分区拷贝到槽位 ${slot}`,
    modalStep2: "第二次确认: 这是高风险写入操作，错误操作可能导致目标槽位无法启动。确认后将立即开始刷写。",
    withEfisp: "，并更新 efisp。",
    noEfisp: "，不更新 efisp。",
    confirmSlot: "请确认槽位无误。",
    taskRunning:"任务运行中",
    waitOperate:"等待操作",
    copyPart:"分区拷贝",
    statusReadFail:"状态读取失败",
    startFail:"启动失败"
  },
  en: {
    pageTitle: "Fake Lock - BL Flasher",
    ksuWebUI: "KernelSU Module WebUI",
    heroDesc: "Auto-detect active slot. Skip BL flash if new build has GBL exploit. Flash BL images to inactive slot, place the cracked ABL in persist's efisp dir and flash the BDS to the efisp partition.",
    slotStatus: "Slot Status",
    refresh: "Refresh",
    currentSlot: "Current Slot",
    targetSlot: "Target Slot",
    imageCount: "Image Count",
    taskStatus: "Task Status",
    flash: "Flash To Other Slot",
    bdsTools: "Update BDS & Tools Only",
    confirmBdsTools: "Confirm BDS/Tools Update",
    modalBdsStep1: "Will flash BDS.efi to the efisp partition and copy the bundled efisp folder and ENTRIES.CFG to persist. The ABL and boot.efi are not touched.",
    modalBdsStep2: "2nd Confirm: This is a high-risk write. A wrong BDS or efisp write may prevent the boot menu from loading. It starts immediately after confirm.",
    toastStartBdsTools: "BDS/Tools update started",
    toastBdsToolsDone: "BDS/Tools update completed",
    clearLog: "Clear Log",
    updateEfisp: "Update efisp (on by default)",
    debugMode: "Debug Mode (process only, no flash; efisp dir uses module tmp/efisp)",
    warning: "Flashing bootloader partitions is high risk. Verify images match your device before starting.",
    imageMap: "Image Mapping",
    partition: "Partition",
    source: "Source (Current)",
    target: "Target",
    action: "Action",
    waiting: "Waiting for module status",
    log: "Live Log",
    autoPoll: "Auto poll last 200 lines",
    risk: "HIGH RISK",
    confirmFlash: "Confirm Flash",
    cancel: "Cancel",
    continue: "Continue",
    waitingStatus: "Waiting",
    slotUnknown: "Slot Unknown",
    logWaiting: "Waiting for log...",
    toastRunning: "Flash task is already running",
    toastStartDebug: "Debug task started",
    toastStartFlash: "Flash task started",
    toastDebugDone: "Debug completed",
    toastFlashDone: "Flash completed",
    toastBlDone: "BL flashed, but efisp not updated",
    toastFailed: "Task finished (failed)",
    toastStartError: "Failed to start task",
    toastLogBusy: "Cannot clear log while task is running",
    toastLogCleared: "Log cleared",
    modalStep1Debug: "Debug Mode: All processes run without flashing partitions. Files saved to tmp directory.",
    modalStep1Normal: (slot) => `1st Confirm: Copy BL partition from current slot to ${slot}`,
    modalStep2: "2nd Confirm: This is a high-risk write operation. Wrong action may brick the target slot. Flash will start immediately after confirm.",
    withEfisp: ", and update efisp.",
    noEfisp: ", efisp not updated.",
    confirmSlot: "Please confirm slot is correct.",
    taskRunning:"Task Running",
    waitOperate:"Waiting",
    copyPart:"Copy",
    statusReadFail:"Status Read Failed",
    startFail:"Start Failed"
  }
};

const elements = {
  stateChip: document.getElementById("stateChip"),
  slotChip: document.getElementById("slotChip"),
  currentSlot: document.getElementById("currentSlot"),
  targetSlot: document.getElementById("targetSlot"),
  imageCount: document.getElementById("imageCount"),
  taskMessage: document.getElementById("taskMessage"),
  updatedAt: document.getElementById("updatedAt"),
  imageTableBody: document.getElementById("imageTableBody"),
  logOutput: document.getElementById("logOutput"),
  flashButton: document.getElementById("flashButton"),
  bdsToolsButton: document.getElementById("bdsToolsButton"),
  clearLogButton: document.getElementById("clearLogButton"),
  refreshButton: document.getElementById("refreshButton"),
  confirmModal: document.getElementById("confirmModal"),
  confirmText: document.getElementById("confirmText"),
  nextConfirmButton: document.getElementById("nextConfirmButton"),
  cancelConfirmButton: document.getElementById("cancelConfirmButton"),
  updateEfispCheckbox: document.getElementById("updateEfispCheckbox"),
  debugModeCheckbox: document.getElementById("debugModeCheckbox"),
  pageTitle: document.getElementById("pageTitle")
};

function applyLanguage(lang) {
  state.lang = lang;
  const t = i18n[lang];
  document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
  elements.pageTitle.textContent = t.pageTitle;
  document.querySelector("#lblKsuWebUI").textContent = t.ksuWebUI;
  document.querySelector(".hero-copy").textContent = t.heroDesc;
  document.querySelector("#lblSlotStatus").textContent = t.slotStatus;
  document.querySelector("#lblCurrentSlot").textContent = t.currentSlot;
  document.querySelector("#lblTargetSlot").textContent = t.targetSlot;
  document.querySelector("#lblImageCount").textContent = t.imageCount;
  document.querySelector("#lblTaskStatus").textContent = t.taskStatus;
  document.querySelector("#lblUpdateEfisp").textContent = t.updateEfisp;
  document.querySelector("#lblDebugMode").textContent = t.debugMode;
  document.querySelector("#lblWarning").textContent = t.warning;
  document.querySelector("#lblImageMap").textContent = t.imageMap;
  document.querySelector("#tblPartition").textContent = t.partition;
  document.querySelector("#tblSource").textContent = t.source;
  document.querySelector("#tblTarget").textContent = t.target;
  document.querySelector("#tblAction").textContent = t.action;
  document.querySelector("#tblWaiting").textContent = t.waiting;
  document.querySelector("#lblLog").textContent = t.log;
  document.querySelector("#lblAutoPoll").textContent = t.autoPoll;
  document.querySelector("#modalRisk").textContent = t.risk;
  document.querySelector("#modalTitle").textContent = t.confirmFlash;
  if (elements.logOutput.textContent === "等待日志输出..." || elements.logOutput.textContent === "Waiting for log...") {
    elements.logOutput.textContent = t.logWaiting;
  }
  if (elements.stateChip.textContent === "等待检测" || elements.stateChip.textContent === "Waiting") {
    elements.stateChip.textContent = t.waitingStatus;
  }
  if (elements.slotChip.textContent === "槽位未知" || elements.slotChip.textContent === "Slot Unknown") {
    elements.slotChip.textContent = t.slotUnknown;
  }
  document.querySelectorAll("[data-i18n]").forEach(el => {
    const key = el.dataset.i18n;
    if (t[key]) el.textContent = t[key];
  });
}

function getKsuBridge() {
  return globalThis.ksu || window.ksu || null;
}

function shellQuote(value) {
  return `'${String(value).replace(/'/g, `'\\''`)}'`;
}

function toast(message) {
  getKsuBridge()?.toast?.(message);
}

function moduleInfo() {
  const bridge = getKsuBridge();
  if (!bridge) throw new Error("No Webui");

  if (bridge.moduleInfo) {
    const raw = bridge.moduleInfo();
    return typeof raw === "string" ? JSON.parse(raw) : raw;
  }

  const found = extractStdout(
    bridge.exec(
      'for d in /data/adb/modules/*/; do [ -f "${d}bin/bl_flasher.sh" ] && printf "%s" "${d%/}" && break; done'
    )
  ).trim();
  if (!found) throw new Error("Module Not Found");
  return { moduleDir: found };
}

function extractStdout(raw) {
  if (raw == null) return "";
  if (typeof raw === "string") {
    try {
      const obj = JSON.parse(raw);
      if (typeof obj?.stdout === "string") return obj.stdout;
      if (typeof obj?.out === "string") return obj.out;
    } catch {}
    return raw;
  }
  if (typeof raw?.stdout === "string") return raw.stdout;
  if (typeof raw?.out === "string") return raw.out;
  return String(raw);
}

function exec(command) {
  const bridge = getKsuBridge();
  if (!bridge?.exec) return "";
  return extractStdout(bridge.exec(command)).replace(/\t/g, "\n");
}

function runScript(action, arg) {
  const parts = [`MODDIR=${shellQuote(state.moduleDir)}`, "sh", shellQuote(state.scriptPath), action];
  if (arg) parts.push(shellQuote(arg));
  return exec(parts.join(" "));
}

function parseKeyValueOutput(output) {
  const info = {};
  for (const line of output.split(/\r?\n/)) {
    if (!line) continue;
    const eq = line.indexOf("=");
    if (eq > 0) info[line.slice(0, eq)] = line.slice(eq + 1);
  }
  return info;
}

function escapeHtml(str) {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function renderTable(currentSlot, targetSlot) {
  const t = i18n[state.lang];
  if (currentSlot === "-" || targetSlot === "-") {
    elements.imageTableBody.innerHTML = `<tr><td colspan="4" class="empty-row">${t.waiting}</td></tr>`;
    return;
  }
  elements.imageTableBody.innerHTML = IMAGE_NAMES.map(name => {
    const src = `/dev/block/by-name/${name}${currentSlot}`;
    const dst = `/dev/block/by-name/${name}${targetSlot}`;
    return `<tr><td>${escapeHtml(name)}</td><td class="caption">${escapeHtml(src)}</td><td>${escapeHtml(dst)}</td><td><span class="status-pill ok">${t.copyPart}</span></td></tr>`;
  }).join("");
}

function renderStatus(status) {
  state.status = status;
  const t = i18n[state.lang];
  const cur = status.CURRENT_SLOT || "-";
  const tar = status.TARGET_SLOT || "-";
  const run = status.RUNNING === "1";
  const st = status.STATE || "idle";
  const msg = status.MESSAGE || t.waitOperate;
  elements.currentSlot.textContent = cur;
  elements.targetSlot.textContent = tar;
  elements.imageCount.textContent = IMAGE_NAMES.length;
  elements.taskMessage.textContent = msg;
  elements.updatedAt.textContent = status.UPDATED_AT || "-";
  elements.stateChip.textContent = run ? t.taskRunning : `${state.lang === "zh" ? "状态" : "Status"}: ${st}`;
  elements.stateChip.className = "chip";
  if (st === "success") elements.stateChip.classList.add("chip-success");
  else if (st === "error") elements.stateChip.classList.add("chip-danger");
  else if (st === "warning" || run) elements.stateChip.classList.add("chip-warn");
  elements.slotChip.textContent = (cur !== "-" && tar !== "-") ? `${state.lang === "zh" ? "当前" : "Current"} ${cur} → ${state.lang === "zh" ? "目标" : "Target"} ${tar}` : t.slotUnknown;
  elements.flashButton.disabled = run || cur === "-" || tar === "-";
  elements.bdsToolsButton.disabled = run;
  elements.clearLogButton.disabled = run;
  renderTable(cur, tar);
}

function refreshStatus() {
  try {
    const raw = runScript("status");
    if (raw === state.prevStatusRaw) return state.status;
    state.prevStatusRaw = raw;
    const s = parseKeyValueOutput(raw);
    if(s.USER_LANG === "en"){
      applyLanguage("en");
    }else if(s.USER_LANG === "zh"){
      applyLanguage("zh");
    }
    renderStatus(s);
    return s;
  } catch (e) {
    const t = i18n[state.lang];
    elements.stateChip.textContent = t.statusReadFail;
    elements.stateChip.className = "chip chip-danger";
    elements.taskMessage.textContent = e.message;
    return null;
  }
}

function refreshLog() {
  try {
    const log = runScript("tail", "200").trim();
    elements.logOutput.textContent = log || i18n[state.lang].logWaiting;
    elements.logOutput.scrollTop = elements.logOutput.scrollHeight;
  } catch (e) {
    elements.logOutput.textContent = `${state.lang === "zh" ? "日志读取失败" : "Log Read Failed"}: ${e.message}`;
  }
}

function closeConfirmModal() {
  state.confirmStep = 0;
  state.pendingAction = null;
  elements.confirmModal.classList.add("hidden");
  elements.confirmModal.setAttribute("aria-hidden", "true");
  elements.nextConfirmButton.textContent = i18n[state.lang].continue;
}

function openConfirmModal(action) {
  const t = i18n[state.lang];
  state.pendingAction = action;
  state.confirmStep = 1;
  if (action === "bds-tools") {
    document.querySelector("#modalTitle").textContent = t.confirmBdsTools;
    elements.confirmText.textContent = t.modalBdsStep1;
    elements.nextConfirmButton.textContent = t.continue;
  } else {
    document.querySelector("#modalTitle").textContent = t.confirmFlash;
    const tar = state.status?.TARGET_SLOT || "?";
    const efisp = elements.updateEfispCheckbox?.checked;
    const dbg = elements.debugModeCheckbox?.checked;
    let msg = dbg ? t.modalStep1Debug : t.modalStep1Normal(tar);
    if (!dbg) {
      msg += efisp ? t.withEfisp : t.noEfisp;
      msg += t.confirmSlot;
    }
    elements.confirmText.textContent = msg;
    elements.nextConfirmButton.textContent = dbg ? (state.lang === "zh" ? "开始调试" : "Start Debug") : t.continue;
  }
  elements.confirmModal.classList.remove("hidden");
  elements.confirmModal.setAttribute("aria-hidden", "false");
}

function handleConfirmProgress() {
  const t = i18n[state.lang];
  if (state.confirmStep === 1) {
    if (state.pendingAction === "bds-tools") {
      state.confirmStep = 2;
      elements.confirmText.textContent = t.modalBdsStep2;
      elements.nextConfirmButton.textContent = state.lang === "zh" ? "开始更新" : "Start Update";
      return;
    }
    const dbg = elements.debugModeCheckbox?.checked;
    if (!dbg) {
      state.confirmStep = 2;
      elements.confirmText.textContent = t.modalStep2;
      elements.nextConfirmButton.textContent = state.lang === "zh" ? "确认刷写" : "Confirm Flash";
      return;
    }
    // debug: single-step confirm, proceed directly
    closeConfirmModal();
    startFlash();
    return;
  }
  const action = state.pendingAction;
  closeConfirmModal();
  if (action === "bds-tools") startBdsTools();
  else startFlash();
}

function startFlash() {
  const t = i18n[state.lang];
  const efisp = elements.updateEfispCheckbox?.checked;
  const dbg = elements.debugModeCheckbox?.checked;
  let mode = dbg ? "debug" : (efisp ? "update-efisp" : "skip-efisp");
  try {
    const out = parseKeyValueOutput(runScript("start", mode));
    if (out.ALREADY_RUNNING) toast(t.toastRunning);
    else if (out.STARTED === "1") toast(dbg ? t.toastStartDebug : t.toastStartFlash);
    else if (out.FINISHED === "success") toast(dbg ? t.toastDebugDone : t.toastFlashDone);
    else if (out.FINISHED === "warning") toast(t.toastBlDone);
    else if (out.FINISHED === "error") toast(t.toastFailed);
    else toast(t.toastStartError);
  } catch (e) { toast(`${t.startFail}: ${e.message}`); }
  manualRefresh();
}

function startBdsTools() {
  const t = i18n[state.lang];
  try {
    const out = parseKeyValueOutput(runScript("start", "update-bds-tools"));
    if (out.ALREADY_RUNNING) toast(t.toastRunning);
    else if (out.STARTED === "1") toast(t.toastStartBdsTools);
    else if (out.FINISHED === "success") toast(t.toastBdsToolsDone);
    else if (out.FINISHED === "error") toast(t.toastFailed);
    else toast(t.toastStartError);
  } catch (e) { toast(`${t.startFail}: ${e.message}`); }
  manualRefresh();
}

function clearLog() {
  const t = i18n[state.lang];
  try {
    const out = parseKeyValueOutput(runScript("clear-log"));
    if (out.BUSY === "1") { toast(t.toastLogBusy); return; }
    toast(t.toastLogCleared);
  } catch (e) { toast(`${state.lang === "zh" ? "清空失败" : "Clear Failed"}: ${e.message}`); }
  manualRefresh();
}

function poll() {
  const s = refreshStatus();
  if (s?.RUNNING === "1") refreshLog();
  schedulePoll(s?.RUNNING === "1" ? 3000 : 8000);
}

function schedulePoll(ms) {
  clearTimeout(state.pollTimer);
  state.pollTimer = setTimeout(poll, ms);
}

function manualRefresh() {
  clearTimeout(state.pollTimer);
  state.prevStatusRaw = "";
  refreshStatus();
  refreshLog();
  schedulePoll(state.status?.RUNNING === "1" ? 3000 : 8000);
}

async function init() {
  try {
    const info = moduleInfo();
    if(!info) return;
    state.moduleDir = info.moduleDir;
    state.scriptPath = `${state.moduleDir}/bin/bl_flasher.sh`;
    refreshStatus();
  } catch (e) {
    elements.stateChip.textContent = state.lang === "zh" ? "WebUI 初始化失败" : "WebUI Init Failed";
    elements.stateChip.className = "chip chip-danger";
    elements.taskMessage.textContent = e.message;
    elements.flashButton.disabled = true;
    elements.bdsToolsButton.disabled = true;
    elements.clearLogButton.disabled = true;
    return;
  }

  elements.refreshButton.addEventListener("click", manualRefresh);
  elements.flashButton.addEventListener("click", () => openConfirmModal("flash"));
  elements.bdsToolsButton.addEventListener("click", () => openConfirmModal("bds-tools"));
  elements.clearLogButton.addEventListener("click", clearLog);
  elements.cancelConfirmButton.addEventListener("click", closeConfirmModal);
  elements.nextConfirmButton.addEventListener("click", handleConfirmProgress);
  elements.confirmModal.addEventListener("click", e => e.target === elements.confirmModal && closeConfirmModal());

  schedulePoll(3000);
}

init();
