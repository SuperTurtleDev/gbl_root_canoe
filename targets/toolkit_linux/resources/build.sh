set -euo pipefail

SCRIPTDIR=$(dirname "$0")
cd "$SCRIPTDIR"

./bin/extractfv ./images/abl.img -o ./ || { echo "ERROR: extractfv failed"; exit 1; }
if [ ! -f ./LinuxLoader.efi ]; then
  echo "ERROR: extractfv produced no LinuxLoader.efi"
  exit 1
fi

mv ./LinuxLoader.efi ./ABL_original.efi
if ! ./bin/patch_abl ./ABL_original.efi ./efisp/boot.efi "$@" > ./patch_log.txt 2>&1; then
  cat ./patch_log.txt
  echo "ERROR: patch_abl failed"
  exit 1
fi
cat ./patch_log.txt
if [ ! -f ./efisp/boot.efi ]; then
  echo "ERROR: patch_abl produced no efisp/boot.efi"
  exit 1
fi

if grep -q "Warning: Failed to patch ABL GBL" ./patch_log.txt; then
  gbl_ok=no
  echo ""
  echo "WARNING: No GBL exploit found in this ABL (Failed to patch ABL GBL)."
  echo "efisp/boot.efi is still produced and valid, but the abl partition must be"
  echo "downgraded to an older ABL with the GBL vulnerability before booting."
  echo "警告：此 ABL 中未找到 GBL 漏洞（Failed to patch ABL GBL）。"
  echo "efisp/boot.efi 仍已生成且有效，但开机前必须将 abl 分区降级为带 GBL 漏洞的旧版 ABL。"
else
  gbl_ok=yes
fi

echo ""
echo "========================================"
echo "Patched. Outputs:"
echo "  efisp/boot.efi     - cracked ABL loader (fake re-lock), the ANDROID boot entry"
echo "  efisp/BOOTENTRIES  - boot entry list (includes the tools submenu)"
echo "  efisp/tools/       - tools submenu (Reboot / BL / ARB tools)"
echo "  BDS.efi            - superfastboot BDS (flash raw to the efisp partition)"
echo "  ABL_original.efi   - original unpatched loader (for analysis; do NOT flash)"
echo ""
echo "Note: the toolkit is manual-install only; superfb does not provide automated"
echo "installation for toolkit users."
echo ""
echo "---- Manual install flow (English) ----"
echo "1. Copy the efisp/ folder to the persist boot root:"
echo "     cp -r efisp/. /mnt/vendor/persist/efisp/"
echo "   (create /mnt/vendor/persist/efisp first if needed, e.g. via MT Manager)"
echo "2. sync"
if [ "$gbl_ok" = "no" ]; then
  echo "3. Downgrade the abl partition to an older ABL with the GBL vulnerability"
  echo "   (efisp/boot.efi and the abl partition do not need to match versions)"
  echo "4. Flash BDS.efi to the efisp partition:"
  echo "     dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M"
else
  echo "3. Flash BDS.efi to the efisp partition:"
  echo "     dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M"
fi
echo ""
echo "---- 手动安装步骤 (中文) ----"
echo "1. 将 efisp/ 文件夹复制到 persist 启动根目录："
echo "     cp -r efisp/. /mnt/vendor/persist/efisp/"
echo "   （如不存在请先创建 /mnt/vendor/persist/efisp，例如用 MT 管理器）"
echo "2. sync"
if [ "$gbl_ok" = "no" ]; then
  echo "3. 将 abl 分区降级为带 GBL 漏洞的旧版 ABL"
  echo "   （efisp/boot.efi 与 abl 分区版本不必一致）"
  echo "4. 将 BDS.efi 刷入 efisp 分区："
  echo "     dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M"
else
  echo "3. 将 BDS.efi 刷入 efisp 分区："
  echo "     dd if=BDS.efi of=/dev/block/by-name/efisp bs=4M"
fi
echo "========================================"
