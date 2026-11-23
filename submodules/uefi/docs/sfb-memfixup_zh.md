# 面向 EFI stub 内核的内存修正

BDS 每个启动会话只安装一次这两项平台修正，位置是 `LinuxLoaderEntry` 的第一步，
早于存储栈、配置存储和菜单键扫描（`SfbMemFixupInstall()`，库位于
`QcomModulePkg/Library/SfbMemFixupLib`）。它们只依赖入口时已存在的固件协议，
并对之后的所有启动路径（存储的启动项、启动菜单、fastboot）持续生效。没有开关：通过 EFI stub
（对内核 `Image` 执行 `LoadImage()`/`StartImage()`）启动的主线 Linux 内核在本平台上
必须依赖这两项才能开机，而它们对其他载荷无害。

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

## 验证

区间运算是纯函数，可在主机上测试：

```sh
QcomModulePkg/Library/SfbMemFixupLib/Test/test.sh
```

运行边界用例，并针对真机转储（`Test/fixtures/memmap.txt`、
`Test/fixtures/tzmap.txt`，通过 fastboot 获取）做回归，同时校验
释放 + 追加 + 已可用 + 其他已描述部分恰好划分 TZ 总量。

在设备上，以 `efi=debug` 启动的内核会在 stub 中打印合并后的内存表，
`dmesg | grep -i 'efi: mem'` 可看到追加的 `Conventional` 范围。协议路径由 stub 自身触发；
此前在 "Exiting boot services..." 之后静默复位的内核现在能到达控制台。
