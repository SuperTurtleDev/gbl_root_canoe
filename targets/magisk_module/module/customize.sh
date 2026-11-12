#!/system/bin/sh
ui_print "============================================="
ui_print "  Please select language / 请选择语言"
ui_print "  Vol+ = Chinese  |  Vol- = English"
ui_print "============================================="

LANG="zh"
while true; do
  keyevent=$(timeout 0.5 getevent -l 2>/dev/null)
  if echo "$keyevent" | grep -q "KEY_VOLUMEUP"; then
    LANG="zh"
    ui_print "[已选择中文 / Chinese selected]"
    break
  elif echo "$keyevent" | grep -q "KEY_VOLUMEDOWN"; then
    LANG="en"
    ui_print "[English selected / 已选择英文]"
    break
  fi
done

echo "$LANG" > "$MODPATH/lang.txt"
ksud module config set user_lang "$LANG" 2>/dev/null
sleep 1

cat > "$MODPATH/module.prop" <<EOF
id=fake_bl_efisp
version=5.0
versionCode=12
author=zaomi
EOF

if [ "$LANG" = "zh" ]; then
  echo "name=假回锁" >> "$MODPATH/module.prop"
  echo "description=自动刷新bl相关分区到非活动槽位" >> "$MODPATH/module.prop"
else
  echo "name=Fake BL EFISP" >> "$MODPATH/module.prop"
  echo "description=Automatically flash BL-related partitions to inactive slot" >> "$MODPATH/module.prop"
fi

if [ "$LANG" = "zh" ]; then
  T_VERIFY="- 正在验证设备型号"
  T_DEVICE_OK="- 设备验证完成："
  T_PERM="- 正在设置权限"
  T_EFISP_TITLE="确保你的内核没有Baseband Guard，设备BL锁已经解锁"
  T_SOC="确保你的设备是8gen5/8elitegen5"
  T_CHECK_EXP="检测漏洞中..."
  T_INSTALL_CHOICE="请选择是否第一次安装假回锁"
  T_VOL_UP="音量上为是（全新安装，需要格式化）"
  T_VOL_DOWN="音量下为否（如果之前安装过一次假回锁或者刚刚首次安装并格式化，建议选否）"
  T_TIP_YES="如果选择是，将会布置 efisp 启动项到 persist 并刷入 BDS 到 efisp，然后重启recovery 进行格式化，格式化后请安装一次这个模块来完成安装，这时选否"
  T_TIP_NO="如果选择否，将会安装OTA更新补丁，每次OTA更新后都需要打开这个模块来安装补丁，来保留BL版本，安装完成后重启系统即可"
  T_SEL_YES="选择了是，正在安装包含补丁的efisp"
  T_NO_SLOT="无法识别当前槽位，已中止安装"
  T_PATCH_FAIL="补丁应用失败，已中止安装"
  T_NO_GBL="检测到当前 ABL 没有 GBL 漏洞"
  T_ABLREPO_CONFIRM="ABL repo 中有带漏洞的旧版 ABL，是否下载并降级 abl 分区？"
  T_ABLREPO_CONFIRM_YES="音量上 = 下载并降级"
  T_ABLREPO_CONFIRM_NO="音量下 = 取消"
  T_ABLREPO_DECLINED="已取消降级，中止安装"
  T_ABLREPO_LOCAL="已从本地模块找到 ABL"
  T_ABLREPO_LOCAL_BAD="本地 ABL 校验失败，尝试云端"
  T_ABLREPO_CLOUD="正在从云端下载 ABL..."
  T_ABLREPO_CLOUD_BAD="云端 ABL 校验失败"
  T_ABLREPO_FAIL="ABL repo 查找失败，请手动刷写带 GBL 漏洞的旧版本 ABL 到 abl 分区后重试"
  T_ABLREPO_DOWNGRADE="正在降级 abl 分区..."
  T_ABLREPO_OK="abl 分区已降级"
  T_ABL_SETRW_FAIL="abl 分区设置可写失败"
  T_ABL_FLASH_FAIL="abl 分区降级刷写失败"
  T_SETRW_FAIL="efisp 分区设置可写失败"
  T_FLASH_FAIL="efisp 分区刷写失败"
  T_PERSIST_NOT_MOUNTED="persist 分区未挂载到 /mnt/vendor/persist"
  T_EFISP_DIR_FAIL="创建 efisp 启动目录失败"
  T_EFISP_WRITE_FAIL="写入 efisp 启动文件失败"
  T_PLACE_BOOT="正在布置 efisp 启动项到 persist"
  T_FLASH_BDS="正在刷入 BDS 到 efisp"
  T_DONE_YES="安装完成，请重启到recovery进行格式化，格式化后请安装一次这个模块来完成安装，这时选否"
  T_SEL_NO="选择了否，正在安装OTA更新模块"
  T_DONE_NO="安装完成，请重启系统即可"
  T_EXISTING_INSTALL="检测到已有 persist/efisp/boot.efi，自动按非首次安装处理并跳过启动分区写入"
else
  T_VERIFY="- Verifying device model"
  T_DEVICE_OK="- Device verified:"
  T_PERM="- Setting permissions"
  T_EFISP_TITLE="Ensure kernel has no Baseband Guard and BL bootloader is unlocked"
  T_SOC="Ensure device is 8gen5 / 8elitegen5"
  T_CHECK_EXP="Detecting exploit..."
  T_INSTALL_CHOICE="Is this your first time installing Fake BL EFISP?"
  T_VOL_UP="Vol+ = YES (Fresh install, requires format)"
  T_VOL_DOWN="Vol- = NO (If installed before or just formatted)"
  T_TIP_YES="If YES: efisp boot entries placed on persist and BDS flashed to efisp, reboot to recovery and format data, then reinstall this module and select NO"
  T_TIP_NO="If NO: OTA patch will be installed, after each OTA, flash this module again to keep BL version"
  T_SEL_YES="Selected YES, installing patched efisp"
  T_NO_SLOT="Failed to detect current slot, abort"
  T_PATCH_FAIL="Failed to apply patch, abort"
  T_NO_GBL="Current ABL lacks the GBL vulnerability"
  T_ABLREPO_CONFIRM="An older ABL with the GBL vuln is available in the ABL repo. Download and downgrade the abl partition?"
  T_ABLREPO_CONFIRM_YES="Vol+ = download and downgrade"
  T_ABLREPO_CONFIRM_NO="Vol- = cancel"
  T_ABLREPO_DECLINED="Downgrade cancelled, aborting"
  T_ABLREPO_LOCAL="Found ABL in local module"
  T_ABLREPO_LOCAL_BAD="Local ABL verification failed, trying cloud"
  T_ABLREPO_CLOUD="Downloading ABL from cloud..."
  T_ABLREPO_CLOUD_BAD="Cloud ABL verification failed"
  T_ABLREPO_FAIL="ABL repo lookup failed. Manually flash an older ABL with the GBL vulnerability to the abl partition, then retry"
  T_ABLREPO_DOWNGRADE="Downgrading the abl partition..."
  T_ABLREPO_OK="abl partition downgraded"
  T_ABL_SETRW_FAIL="Failed to set abl to read-write"
  T_ABL_FLASH_FAIL="Failed to flash abl partition"
  T_SETRW_FAIL="Failed to set efisp to read-write"
  T_FLASH_FAIL="Failed to flash efisp"
  T_PERSIST_NOT_MOUNTED="persist is not mounted at /mnt/vendor/persist"
  T_EFISP_DIR_FAIL="efisp boot dir create failed"
  T_EFISP_WRITE_FAIL="efisp boot file write failed"
  T_PLACE_BOOT="Placing efisp boot entries on persist"
  T_FLASH_BDS="Flashing BDS to efisp"
  T_DONE_YES="Install complete. Reboot to recovery and format data, then reinstall module and choose NO"
  T_SEL_NO="Selected NO, installing OTA update patch"
  T_DONE_NO="Install complete, please reboot"
  T_EXISTING_INSTALL="Existing persist/efisp/boot.efi detected; treating this as an update and skipping boot-partition writes"
fi

ui_print "$T_VERIFY"
_model=$(getprop ro.product.model 2>/dev/null)
_name=$(getprop ro.product.name 2>/dev/null)
_inc=$(getprop ro.build.version.incremental 2>/dev/null)
ui_print "$T_DEVICE_OK $_model / $_name / $_inc"
ui_print "$T_PERM"

set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm_recursive "$MODPATH/webroot" 0 0 0755 0644
set_perm "$MODPATH/module.prop" 0 0 0644
set_perm "$MODPATH/customize.sh" 0 0 0755

detect_current_slot() {
  case "$(getprop ro.boot.slot_suffix 2>/dev/null)" in
    _a) echo _a ;;
    _b) echo _b ;;
    *) return 1 ;;
  esac
}

BY_NAME_DIR=/dev/block/by-name
RUNTIME_DIR=$MODPATH/tmp
PERSIST_MNT=/mnt/vendor/persist
EFISP_DIR=$PERSIST_MNT/efisp
ABLREPO_URL="https://raw.githubusercontent.com/superturtlee/gbl_root_canoe/main/ablrepo"
mkdir -p $RUNTIME_DIR

# Verify $1 against the sha256 in $2 (first whitespace-delimited token).
verify_sha256() {
  [ -f "$1" ] && [ -f "$2" ] || return 1
  expected=$(cut -d' ' -f1 "$2" | tr -d '[:space:]')
  actual=$(sha256sum "$1" | cut -d' ' -f1 | tr -d '[:space:]')
  [ -n "$expected" ] && [ "$expected" = "$actual" ]
}

# Download $1 into $2 using whichever fetcher is available.
download_url() {
  if command -v wget >/dev/null 2>&1; then
    timeout 60 wget -O "$2" "$1" >/dev/null 2>&1
  elif command -v curl >/dev/null 2>&1; then
    timeout 60 curl -fL -o "$2" "$1" >/dev/null 2>&1
  else
    return 1
  fi
}

# Fetch an older ABL with the GBL vulnerability. Looks up the local module
# bundle first, then the cloud. On success, leaves the image at
# $RUNTIME_DIR/repo_abl.img and returns 0; otherwise returns 1.
fetch_abl_from_repo() {
  product=$(getprop ro.product.name 2>/dev/null)
  [ -z "$product" ] && return 1
  local_dir="$MODPATH/ablrepo/$product"
  if [ -f "$local_dir/abl.img" ]; then
    if [ -f "$local_dir/abl.sha256" ] && verify_sha256 "$local_dir/abl.img" "$local_dir/abl.sha256"; then
      cp "$local_dir/abl.img" "$RUNTIME_DIR/repo_abl.img"
      ui_print "$T_ABLREPO_LOCAL"
      return 0
    fi
    ui_print "$T_ABLREPO_LOCAL_BAD"
  fi
  ui_print "$T_ABLREPO_CLOUD"
  if download_url "$ABLREPO_URL/$product/abl.sha256" "$RUNTIME_DIR/repo_abl.sha256" && \
     download_url "$ABLREPO_URL/$product/abl.img" "$RUNTIME_DIR/repo_abl.img"; then
    if verify_sha256 "$RUNTIME_DIR/repo_abl.img" "$RUNTIME_DIR/repo_abl.sha256"; then
      return 0
    fi
    ui_print "$T_ABLREPO_CLOUD_BAD"
  fi
  return 1
}

ui_print "$T_EFISP_TITLE"
ui_print "$T_SOC"
ui_print "$T_CHECK_EXP"
current_slot=$(detect_current_slot)

# A module update on an existing BDS install must never enter the destructive
# fresh-install path because of an accidental Volume Up press. A real boot.efi
# is a stronger signal than the directory alone, which may be left by a partial
# or failed install.
if grep -q " $PERSIST_MNT " /proc/mounts && [ -s "$EFISP_DIR/boot.efi" ]; then
  ui_print "$T_EXISTING_INSTALL"
  ui_print "$T_DONE_NO"
  rm -rf "$RUNTIME_DIR" "$MODPATH/ablrepo"
  exit 0
fi

ui_print "$T_INSTALL_CHOICE"
ui_print "$T_VOL_UP"
ui_print "$T_VOL_DOWN"
ui_print "$T_TIP_YES"
ui_print "$T_TIP_NO"

while true; do
  keyevent=$(timeout 0.5 getevent -l 2>/dev/null)
  if echo "$keyevent" | grep -q "KEY_VOLUMEUP"; then
    ui_print "$T_SEL_YES"
    if [ -z "$current_slot" ]; then
      ui_print "$T_NO_SLOT"
      abort "slot detection failed"
    fi
    abl_part="$BY_NAME_DIR/abl$current_slot"
    $MODPATH/bin/extractfv -o $RUNTIME_DIR -v "$abl_part" >> $RUNTIME_DIR/extract.log 2>&1
    $MODPATH/bin/patch_abl $RUNTIME_DIR/LinuxLoader.efi $RUNTIME_DIR/patched.efi >> $RUNTIME_DIR/patch.log 2>&1
    if [ ! -f $RUNTIME_DIR/patched.efi ]; then
      ui_print "$T_PATCH_FAIL"
      abort "patch failed"
    fi
    if grep -q "Warning: Failed to patch ABL GBL" $RUNTIME_DIR/patch.log; then
      ui_print "$T_NO_GBL"
      ui_print "$T_ABLREPO_CONFIRM"
      ui_print "$T_ABLREPO_CONFIRM_YES"
      ui_print "$T_ABLREPO_CONFIRM_NO"
      repo_confirm=""
      while [ -z "$repo_confirm" ]; do
        keyevent=$(timeout 0.5 getevent -l 2>/dev/null)
        if echo "$keyevent" | grep -q "KEY_VOLUMEUP"; then
          repo_confirm=yes
        elif echo "$keyevent" | grep -q "KEY_VOLUMEDOWN"; then
          repo_confirm=no
        fi
      done
      if [ "$repo_confirm" = "no" ]; then
        ui_print "$T_ABLREPO_DECLINED"
        abort "downgrade declined"
      fi
      if ! fetch_abl_from_repo; then
        ui_print "$T_ABLREPO_FAIL"
        abort "abl repo lookup failed"
      fi
      # Downgrade ONLY the abl partition so it has the vuln to load BDS. Do NOT
      # rebuild boot.efi from this image: some systems need the high-version
      # LinuxLoader from the current partition to boot, so boot.efi (already
      # built above from the running ABL) stays untouched. The repo ABL is
      # trusted to carry the vuln.
      ui_print "$T_ABLREPO_DOWNGRADE"
      if ! blockdev --setrw "$abl_part" >> $RUNTIME_DIR/flash.log 2>&1; then
        ui_print "$T_ABL_SETRW_FAIL"
        abort "setrw abl failed"
      fi
      if ! dd if=$RUNTIME_DIR/repo_abl.img of="$abl_part" bs=4M conv=fsync >> $RUNTIME_DIR/flash.log 2>&1; then
        ui_print "$T_ABL_FLASH_FAIL"
        abort "downgrade abl failed"
      fi
      sync
      ui_print "$T_ABLREPO_OK"
    fi

    ui_print "$T_PLACE_BOOT"
    if ! grep -q " $PERSIST_MNT " /proc/mounts; then
      ui_print "$T_PERSIST_NOT_MOUNTED"
      abort "persist not mounted"
    fi
    mkdir -p "$EFISP_DIR" || { ui_print "$T_EFISP_DIR_FAIL"; abort "efisp mkdir failed"; }
    [ -f "$EFISP_DIR/boot.efi" ] && mv "$EFISP_DIR/boot.efi" "$EFISP_DIR/boot_backup.efi"
    if ! cp $RUNTIME_DIR/patched.efi "$EFISP_DIR/boot.efi"; then
      ui_print "$T_EFISP_WRITE_FAIL"
      abort "efisp write failed"
    fi
    cp -r "$MODPATH/efisp/." "$EFISP_DIR/" || { ui_print "$T_EFISP_WRITE_FAIL"; abort "efisp write failed"; }
    cp "$MODPATH/ENTRIES.CFG" "$EFISP_DIR/../ENTRIES.CFG" || { ui_print "$T_EFISP_WRITE_FAIL"; abort "efisp write failed"; }
    sync

    ui_print "$T_FLASH_BDS"
    if ! blockdev --setrw $BY_NAME_DIR/efisp >> $RUNTIME_DIR/flash.log 2>&1; then
      ui_print "$T_SETRW_FAIL"
      abort "setrw failed"
    fi
    if ! dd if=$MODPATH/BDS.efi of=$BY_NAME_DIR/efisp bs=4M conv=fsync >> $RUNTIME_DIR/flash.log 2>&1; then
      ui_print "$T_FLASH_FAIL"
      abort "flash failed"
    fi
    sync
    ui_print "$T_DONE_YES"
    rm -rf $RUNTIME_DIR
    break
  elif echo "$keyevent" | grep -q "KEY_VOLUMEDOWN"; then
    ui_print "$T_SEL_NO"
    ui_print "$T_DONE_NO"
    rm -rf $RUNTIME_DIR
    break
  fi
done

# ablrepo is bundled only for install-time ABL downgrade lookup. Remove it so
# the device-side module dir (/data/adb/modules/fake_bl_efisp) stays lean after
# installation; the cloud URL remains available for later re-downloads.
rm -rf "$MODPATH/ablrepo"
