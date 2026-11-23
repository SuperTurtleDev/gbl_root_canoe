# 面向 EFI stub 内核的内存修正

BDS 每个启动会话只安装一次这两项平台修正，位置是 `LinuxLoaderEntry` 的第一步，
早于存储栈、配置存储和菜单键扫描（`SfbMemFixupInstall()`，库位于
`QcomModulePkg/Library/SfbMemFixupLib`）。它们只依赖入口时已存在的固件协议，
并对之后的所有启动路径（存储的启动项、启动菜单、fastboot）持续生效。没有开关：通过 EFI stub
（对内核 `Image` 执行 `LoadImage()`/`StartImage()`）启动的主线 Linux 内核在本平台上
必须依赖这两项才能开机，而它们对其他载荷无害。

第三个可选部分——操作员覆盖表 `CONFIG\MEMCFG`——在配置存储挂载后加载，
按操作员的指令改写合并视图的一部分。见
[操作员覆盖（CONFIG\MEMCFG）](#操作员覆盖configmemcfg)。

## EFI_MEMORY_ATTRIBUTE_PROTOCOL

**问题。** 平台固件把新分配的 `EfiLoaderCode` 页映射为不可执行（XP），
又不提供 UEFI 2.10 的 `EFI_MEMORY_ATTRIBUTE_PROTOCOL` 供加载器修改。arm64 EFI stub
（Linux 6.3 起）会把内核搬移到这样的页里，并在 MMU 仍开启的状态下跳转过去，
于是 `ExitBootServices()` 之后的第一条内核指令就触发异常：没有控制台输出，
没有 panic，设备静默复位。

**修正。** BDS 自己发布该协议
（`QcomModulePkg/Include/Protocol/EFIMemoryAttribute.h`，标准 GUID
`f4560cf6-40ec-4b4a-a192-bf1d57d0b189`），语义完全按规范：

- `GetMemoryAttributes()` 报告当前 stage-1 转换表对该范围施加的 `RP`/`RO`/`XP`，
  不一致时返回 `EFI_NO_MAPPING`。转换表直接读取（当前异常级别的 TTBR0，4 KB 粒度，
  UEFI 要求的恒等映射）。保护位不参考 GCD：DXE core 把 NX 策略写进页表却不记录到 GCD，
  GCD 在两个方向上都可能过期。
- `SetMemoryAttributes()` / `ClearMemoryAttributes()` 把请求合并到从转换表读出的属性上，
  通过 `EFI_CPU_ARCH_PROTOCOL.SetMemoryAttributes()` 写入（固件唯一导出的页表写接口；
  它整体替换属性集合，所以 GCD 的缓存类型随保护位一起传入，与 DXE core 的镜像保护一致），
  然后重新读取转换表。结果与请求不符则返回 `EFI_UNSUPPORTED`。

参数检查遵循规范（页对齐、长度非零、仅允许 `RP`/`RO`/`XP`）。实现对应 ArmPkg CpuDxe 的
`MemoryAttribute.c`，后者同样从实时页表作答。

安装时用 BDS 自身的代码页校验页表遍历器；若固件的页表无法读取（非 4 KB 粒度、意外布局），
宁可不发布协议也不发布错误的实现。若固件本身已发布该协议，BDS 不做干预。

## GetMemoryMap 合并

**问题。** 固件内存表只描述启动阶段自己碰过的 DDR，并把 TZ 允许使用的范围保留为
`RESERVED`。TZ RAM 分区表（`EFI_RAMPARTITION_PROTOCOL`）才是应用处理器可用内存的权威来源。
EFI 方式启动的内核用 UEFI 内存表而不是设备树 `/memory` 节点建立 memblock，
因此不做此修正时内核只能看到一小部分内存（参考 SM8850 设备上约 0.7 GiB 未描述内存
加 0.8 GiB 被释放的 `RESERVED` 内存）。

**修正。** 包装 `gBS->GetMemoryMap`。所有调用者看到的视图：

1. **释放** - 只有固件自己也记为普通可缓存内存（带 `EFI_MEMORY_WB`）的 `RESERVED`
   描述符才按 TZ 列表切分；落在 TZ 分区内的部分以 `CONVENTIONAL` 返回、属性保留，
   其余保持 `RESERVED`。
2. **追加** - 固件表完全未描述的 TZ 范围以新的 `CONVENTIONAL` 条目追加，属性为
   `UC|WC|WT|WB`。
3. **强制** - 每条 `CONFIG\MEMCFG` 覆盖从上面两步的结果中切出，并以配置的类型和
   属性追加。这是操作员的指令：不对照 TZ 或固件做任何校验。

**不带** `WB` 的 `RESERVED` 描述符原样透传。固件把与其他 master 共享、或当设备内存使用的
区域登记为仅 UC（SMEM、hypervisor、TZ 运行时区域），这本身就是"这段不是给 OS 的 RAM"的声明，
与 TZ 表对背后 DDR 的描述无关。参考设备上这类条目是 `0x81Cxxxxx`、`0x8AA00000-0xA7CFFFFF`
和 `0xD78xxxxx`；把它们当 WB RAM 交给内核，init 一启动（首批 UEFI runtime 调用）就出现
内核态 undefined-instruction oops。`RUNTIME_*`、`MMIO`、ACPI 条目以及任何带
`EFI_MEMORY_RUNTIME` 的描述符同样从不改动。

固件分配器不认识追加的范围，之后的任何分配都不会落在那里，视图因此稳定。
`MapKey` 原样来自固件。调用者的 `GetMemoryMap()` 与 `ExitBootServices()` 之间不会
发生任何分配：链式调用落在预先分配的暂存缓冲区里，该缓冲区只在尺寸查询调用时增长，
而那之后调用者本来就要自行分配。视图在每次调用时都从实时内存表重新计算。

内核仍然遵守设备树 `/reserved-memory` 的 `no-map` 保留区：
`early_init_fdt_scan_reserved_mem()` 会在 UEFI 内存表交付的内存之上重新保留它们。

## 操作员覆盖（CONFIG\MEMCFG）

合并逻辑在设计上是保守的：固件没有自己登记为普通可缓存 RAM 的内存绝不改动。
但操作员有时更清楚——一个从不启动 modem remoteproc 的系统用不着固件留作
仅 UC `RESERVED` 的 modem 保留区（`PIL_Reserved`），而 UEFI 内存表不把它们标成 RAM，
内核就用不上。

配置容器里的 `CONFIG\MEMCFG` 就是这张表：

- 由 `SfbMemFixupLoadConfig()` 在容器挂载期间（`LinuxLoaderEntry` 中
  `SfbConfigStoreMount()` 之后）加载一次，之后与证书一样常驻——fastboot 运行时
  存储是卸载的。文件缺失或不可读等于空表，不是错误；非法行被跳过并在调试日志中
  计数。若 TZ 合并没有装上 `GetMemoryMap` 钩子（无 RAM 分区协议），这张表会自己
  装上钩子。
- 每行一条覆盖，格式 `NAME BASE SIZE TYPE ATTRIBUTE`——五个字段全部必填，
  `#` 开始注释。`BASE`/`SIZE` 为十六进制、4 KiB 对齐；`TYPE` 是 UEFI 内存类型，
  可用数字或名称（`CONVENTIONAL`、`RESERVED` 等）；`ATTRIBUTE` 是
  `EFI_MEMORY_*` 掩码（`0xF` = `UC|WC|WT|WB`，普通 RAM）。完整语法见
  `Library/SfbMemFixupLib/SfbMemCfg.h`。
- 每条覆盖从合并视图中切出——被跨越的描述符的头尾保留，完全落在覆盖内的描述符
  消失——然后以自己的类型和属性追加。行按文件顺序生效：重叠处后行胜出。
- 不对照 TZ 列表、固件内存表或任何其他来源做校验。把 TZ 或固件运行时内存交给
  内核的覆盖，会精确地按它所写的引发异常。

一份带注释的 canoe 示例随仓库发布：`docs/examples/MEMCFG`，释放 modem 保留区
加上厂商 PIL 保留区之间五段无主缝隙，合计约 318 MiB（235.6 MiB modem 块 +
82.3 MiB 对齐缝隙和 `adspslpi` 之上没有任何 reserved-memory 节点认领的尾部）。
同一 `PIL_Reserved` 描述符内的在用区域（gpu 微码、camera/CVP、video、cdsp、
spss、dcp、soccp、adsp）刻意保持不动。modem 块只有在内核设备树没有重新保留
这些范围时才有收益（`reserved-memory` 的 `no-map` 优先于 UEFI 内存表）；
缝隙没有对应的 DT 节点，对任何 EFI 方式启动的内核直接生效。

## 验证

区间运算、解析器和覆盖应用都是纯函数，可在主机上测试：

```sh
QcomModulePkg/Library/SfbMemFixupLib/Test/test.sh
```

运行 `MemFixupTest`（边界用例，并针对真机转储 `Test/fixtures/memmap.txt`、
`Test/fixtures/tzmap.txt`（通过 fastboot 获取）做回归，校验释放 + 追加 + 已可用 +
其他已描述部分恰好划分 TZ 总量）和 `MemCfgTest`（解析器语法与切出/追加算术，
包括把随仓示例应用到参考固件内存表）。

在设备上，以 `efi=debug` 启动的内核会在 stub 中打印合并后的内存表，
`dmesg | grep -i 'efi: mem'` 可看到追加的 `Conventional` 范围。协议路径由 stub 自身触发；
此前在 "Exiting boot services..." 之后静默复位的内核现在能到达控制台。
