# BDS 启动项与 efisp 配置存储

本文说明 super-fastboot BDS（`build/BDS.efi`）如何保存启动项、启动项从何而来，以及按什么顺序尝试启动。

## efisp 分区

`efisp` GPT 分区包含两部分：

| 区间（取自 GPT 分区项，`EndingLBA` 为闭区间） | 内容 |
| --- | --- |
| `StartingLBA` 至 `StartingLBA + 1 MiB / BlockSize - 1` | `BDS.efi`，普通 PE 镜像 |
| `StartingLBA + 1 MiB / BlockSize` 至 `EndingLBA` | 唯一的 FAT 卷（配置容器），无分区表 |

- 构建不会把 `BDS.efi` 补齐到 1 MiB。镜像达到 1 MiB 时 `make build` 直接失败。
- 刷写 BDS 只写文件本身长度，例如 `dd if=BDS.efi of=<efisp> bs=4096 conv=notrunc,fsync`。按文件长度写入不会触及其后的容器。
- `fastboot erase efisp` 或写入整分区镜像会清空容器，下次启动时 BDS 会重新格式化出一个空容器。
- BDS 只通过一个限定在该 LBA 区间内的 512 字节 Block I/O 访问容器。经由它的写入不可能落在 1 MiB 之前或 `EndingLBA` 之后。

每次启动 BDS 都会挂载容器。若 FAT 驱动无法挂载，且首扇区不是可用的 FAT 引导扇区，BDS 会按容量现场格式化为 FAT12、FAT16 或 FAT32，然后重新挂载。仍然失败时，BDS 不带持久存储继续启动。已有看起来有效的 FAT 引导扇区的容器即使挂载失败也不会被自动格式化。

## 启动项

持久化的启动项列表是容器内的 `CONFIG\ENTRIES.CFG`。每行一个启动项，行序即启动尝试顺序。没有默认启动项：无人操作时先试第一项，失败再试下一项。该文件在运行时生成，不随任何包分发。

### 行格式

```
<名称>:[(<磁盘>,<分区>)]<卷内绝对路径> [参数]
```

```
Android Tools\Reboot Tools:(1,1)\efisp\tools\RebootTools.efi
Android Tools\Reboot Tools:\efisp\tools\RebootTools.efi
```

- 名称中的 `\` 表示分级菜单。上例显示为 "Android Tools >"，其下是 "Reboot Tools"。
- `(d,p)` 表示 UEFI 枚举到的第 d 块整盘上的第 p 个分区。两者都从 0 开始，p 等于 GPT 分区项序号减一。
- 不写 `(d,p)` 时，路径位于提供当前 ENTRIES.CFG 的那个卷上。
- 路径中可以用 `/` 代替 `\`。
- 路径后第一个空白之后的内容作为 LoadOptions 传给镜像。
- 空行和 `#` 开头的行会被忽略。
- 旧的 `$`、`%` 标记、`SFB1|...` 记录和卷标匹配均已删除。

### 启动项来源

扫描会遍历所有属于 GPT 分区的 FAT 和 ext4 卷：

1. 卷上存在 `\EFI\BOOT\BOOTAA64.EFI` 时，加入一个以卷标加地址命名的启动项，例如 `EFIBOOT (0,2)`。BDS 自身所在的镜像会被跳过。
2. 卷根目录有 `ENTRIES.CFG` 时，其中文件确实存在的每一行都会加入。未写 `(d,p)` 的行记录为该卷的地址。

随后与已存储的列表同步：

- 文件已不存在的已存储项被删除。
- 其余项保持原有顺序。
- 新发现的项追加到末尾。

结果以原子方式写回 `CONFIG\ENTRIES.CFG`：先写临时文件并刷盘，再删除旧文件，把新文件改名到位，最后再刷盘一次。

### 开机流程

1. 读取 `CONFIG\ENTRIES.CFG`。文件不存在或存储不可用时得到空列表。
2. 列表为空时扫描、同步并写盘。
3. 仍为空时进入启动菜单。
4. 按下菜单键（音量上）时扫描、同步并写盘，然后进入菜单。
5. 否则按顺序逐项尝试。LoadImage 判定文件不是 PE/COFF 镜像或架构不匹配时，删除该项并重新写盘。文件缺失、安全校验拒绝、内存不足等其它错误保留该项，继续尝试下一项。
6. 全部失败时进入启动菜单。

启动菜单每次打开时也会重新扫描并写盘。

### Advanced 菜单

- **Boot Entries Priority**：选中一项，用音量上和音量下移动，按电源键确认。新顺序会写盘。
- **Boot Entries Management**：选中一项并确认删除，列表随即写盘。
- **USB Mass Storage > Export Config Store**：卸载容器，并在其外层合成一份 GPT，唯一的 ESP 类型分区恰好覆盖容器，宿主机看到的是普通 FAT 分区。导出结束后重新挂载，并重新读取存储列表，以防宿主机修改过它。

被删除的项如果其文件仍在某个卷上，下次扫描时会被重新加入。扫描发生在列表为空、按下菜单键或打开菜单时。要彻底去掉它，请删除该文件或对应的 `ENTRIES.CFG` 行。

## 安全证书

容器中还存放安全证书类，以 X.509 DER（`.cer`）文件、按目录分类：

| 目录 | 类别 | 非空时的效果 |
| --- | --- | --- |
| `AUTH_CERTS` | A 类 —— fastboot 授权 | 进入锁定模式，直到挑战应答验证通过 |
| `BOOT_CERTS` | B 类 —— EFI 代码签名 | 所有被启动的 EFI 必须带可验证的签名 |

两个功能完全由证书驱动：目录为空（或不存在）即关闭该功能，BDS 行为与之前完全一致。完整模型（挑战协议、fastboot 命令门控、顶层仓库 `tools/sfbsec/` 下的签名工具）见 [sfb-security.md](sfb-security.md)。
