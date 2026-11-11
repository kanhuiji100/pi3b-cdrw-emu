# 把 RP2350 光驱模拟器搬到 Orange Pi 3B：CD-RW / DVD-RW / BD-RE 可行性研究与实测

## 摘要

结论：**可行，而且性能提升约 50–60 倍**。Orange Pi 3B 的 RK3566 有一颗
USB 2.0 OTG 控制器，物理上引到了"蓝色 USB 3.0 口正下方的 USB 2.0 口"。
厂商 DTB 默认把它配置成 host，但用一个 live device-tree overlay 把
`dr_mode` 改成 `peripheral` 并重新绑定 `dwc3`，就能在不重启的情况下得到
UDC。用户态用 FunctionFS 实现 Mass Storage Bulk-Only Transport，复用原固件的
SCSI MMC 命令集和光盘状态机，即可让主机把它识别成真正的可重写光驱。

实机结果：Linux `usb-storage` 枚举出 `/dev/sr0`，ISO 挂载和逐字节比对通过；
`wodim -tao` 刻录 CD-RW；`growisofs` 刻录 4.7 GB DVD-RW 与 25 GB BD-RE，
回读 sha256 一致、可挂载；顺序读 31 MB/s、写 20.6 MB/s（138x CD / 15x DVD /
4.6x BD）。BLANK、FORMAT UNIT、托盘弹出/加载均正常。

## 1. 目标

原项目 [rp2350-cdrw-emu](../../rp2350-cdrw-emu) 用 RP2350 + TinyUSB 实现
USB CD-RW。它受限于：

- USB Full-Speed 12 Mbps，实测读约 500 KB/s、写约 400 KB/s，只能稳定 4x；
- SPI microSD 12 MHz；
- 4 MB Flash、无文件系统缓存；
- 自写 UI（OLED + EC11）。

本研究的目的是：**保留其经过验证的 SCSI/光盘语义，把传输和存储换成
Linux 原生能力，直接跑在 Orange Pi 3B 上**，并量化收益与风险。

## 2. 硬件可行性

### 2.1 官方接口

Orange Pi 3B 官方参数（[香橙派产品页](http://www.orangepi.cn/html/hardWare/computerAndMicrocontrollers/parameter/Orange-Pi-3B.html)）：

> USB：1x USB 2.0 支持 Device 或 HOST 模式；1x USB 3.0 HOST；2x USB 2.0 HOST

官方用户手册中，用于 MaskROM/eMMC 烧录的 **"USB2.0 burning port"** 就是
**蓝色 USB 3.0 口正下方的 USB 2.0 口**，并且明确要求用 **USB-A 公对公数据线**
连接 PC。这正是 Device 模式的物理入口。

### 2.2 RK3566 控制器与 DTB 现状

现场解编译 `/boot/dtb/rockchip/rk3566-orangepi-3b.dtb` 可见：

- `usb@fcc00000`（`usb_host0_xhci`，DWC3）使用 `usb2phy0` 的 **otg-port**，
  `dr_mode = "host"`，`maximum-speed = "high-speed"`；
- `usb@fd000000`（`usb_host1_xhci`）使用 `usb2phy0` 的 host-port + USB3 combo PHY，
  对应蓝色 USB 3.0 口；
- `usb@fd800000/fd880000` 是两组 EHCI/OHCI，对应另外两个 USB 2.0 host 口；
- 供电：`vcc5v0-otg-regulator`（GPIO0_A5，`enable-active-high`）控制 OTG 口 VBUS；
  `vcc5v0-host-regulator`（GPIO0_A6）控制 host 口 VBUS。

也就是说，**硬件具备 peripheral 能力，缺的只是 DTB 配置**。

上游内核的 Orange Pi 3B DTS 也把 `usb_host0_xhci` 固定为 host
（[Jonas Karlman 的 DTS 补丁](https://patchew.org/linux/20240626230319.1425316-1-jonas@kwiboo.se/20240626230319.1425316-3-jonas@kwiboo.se/)），
所以不能指望发行版默认帮你切好。

### 2.3 运行时切换（不用重启）

内核启用了 `CONFIG_OF_OVERLAY`、`CONFIG_OF_CONFIGFS`、`CONFIG_USB_CONFIGFS_F_FS`。
因此可以：

```bash
mkdir -p /sys/kernel/config/device-tree/overlays/otg-peripheral
cat otg-peripheral.dtbo > /sys/kernel/config/device-tree/overlays/otg-peripheral/dtbo

echo fcc00000.usb > /sys/bus/platform/drivers/dwc3/unbind
sleep 0.3
echo fcc00000.usb > /sys/bus/platform/drivers/dwc3/bind
ls /sys/class/udc        # fcc00000.usb
```

overlay 源码只有一句：

```dts
&usb_host0_xhci { dr_mode = "peripheral"; };
```

这一步已经实机验证，dmesg 只有一条无害的 "memory leak will occur if overlay
removed" 警告。脚本见 `tools/enable-otg.sh`。

### 2.4 VBUS：外部 PC 的唯一硬件坑

厂商 DTB 把 `vcc5v0_otg` 标成 `regulator-always-on`。本板自环（USB3.0 host 口
↔ OTG 口）时两端 5 V 同源，安全；但接**外部 PC** 时，板子和 PC 会同时驱动
VBUS。

已验证的运行时规避：

1. `echo vcc5v0-otg-regulator > /sys/bus/platform/drivers/reg-fixed-voltage/unbind`
2. 把 GPIO0_A5 拉低（`tools/otg-vbus-off.sh` 用 sysfs GPIO 保持低电平）。

关掉后，本板 host 口通过 A-A 线给 OTG 口提供的 VBUS 仍然让设备正常枚举和读取，
说明"PC 供 VBUS、板子 Type-C 供电"的接法是可行的。更彻底的做法是在启动前
删除 `regulator-always-on`，见 [hardware.md](hardware.md)。

## 3. 软件路径对比

| 方案 | PDT=5 光驱 | 写 CD-RW | 直接 USB | 复用现有核心 | 结论 |
|---|---|---|---|---|---|
| `g_mass_storage`（configfs） | ✅ `cdrom=1` | ❌ 只读 | ✅ | ❌ | 只能做只读 ISO，不能满足刻录 |
| LIO / `tcmu` | ⚠️ 需要额外设备类型 | ⚠️ | ✅ | ❌ | 面向块设备，MMC 语义不匹配 |
| USB/IP `usbip-vudc` | ✅（自定义 gadget） | ✅ | ❌ 要走网络 | ✅ | 可枚举，但内核 teardown oops，实验性 |
| **FunctionFS + 自写 BOT** | ✅ | ✅ | ✅ | ✅ | **选用** |

关键点：`g_mass_storage` 的 `cdrom=1` 确实返回 PDT=5，但它不实现
`RESERVE TRACK` / `WRITE` / `BLANK` / `CLOSE TRACK`，只是只读 ISO 的快捷方式。
本项目的价值恰好在可写 CD-RW 语义，所以必须自己处理 BOT 和 SCSI。

FunctionFS 允许用户态提供 **接口/端点描述符**（Mass Storage，SubClass 06h，
Protocol 50h，两个 Bulk 端点），内核 composite 负责设备描述符、字符串、标准
控制请求和 SET_CONFIGURATION；用户态只需处理：

- `GET_MAX_LUN (0xFE)` → 返回 0；
- Bulk-Only Reset `(0xFF)`；
- `CLEAR_FEATURE(ENDPOINT_HALT)`；
- Bulk OUT 上的 31 字节 CBW、数据阶段、13 字节 CSW。

## 4. 移植设计

### 4.1 分层

```
src/core/scsi.c        1317 行，原样复用（仅 INQUIRY 字符串改为宏）
src/core/media.c        693 行，原样复用 + 1 个 seek 缓存 bug 修复
src/core/util.c         原样复用（util_millis 用 CLOCK_MONOTONIC）
src/posix/ffshim.c      FatFs 文件 API → pread/pwrite/fallocate/lseek
src/posix/storage_posix.c  storage.h → 目录扫描/新建/擦除/last-disc
src/usb/bot.c           BOT 状态机，HAL 只有 read/write/stall/clear_halt
src/usb/ffs_gadget.c    configfs + FunctionFS + ep0 线程
src/main.c              CLI、介质加载、信号、生命周期
```

`media.c` 完全没有改逻辑，只通过 `ff.h` shim 使用 Linux 文件。这样 SCSI 行为
与原固件保持逐字节一致，原 `tests/test_scsi.c` 也能直接复用。

### 4.2 FunctionFS 细节

- configfs：`idVendor=0x1209`、`idProduct=0xCD01`、`bcdUSB=0x0200`、
  `bcdDevice=0x0100`，一个 config、一个 `ffs.cdrw` function。
- 描述符：Interface `08/06/50`，Bulk OUT `0x01`、Bulk IN `0x81`；
  FS 64 字节、HS 512 字节。
- 挂载：`mount -t functionfs cdrw /dev/ffs-cdrw`（源名 = configfs 实例名）。
- 端点顺序：ep1 = OUT（描述符里的第一个），ep2 = IN。
- 必须先写描述符 + strings，FunctionFS 才进入 ACTIVE、创建 ep1/ep2；
  之后才能写 UDC 绑定。
- 设备字符串/序列号由 configfs 提供，接口字符串用 FunctionFS 的 ASCII 格式。

### 4.3 为什么需要 ep0 线程

第一版是单线程：`poll(ep0, ep_out)` → 处理 SETUP → 读 CBW → 写数据 → 写 CSW。
实机出现死锁：

```
BOT: CBW op=12 ...            # INQUIRY
ffs: ENABLE
ffs: SETUP req=FE ...         # GET_MAX_LUN 在 INQUIRY 数据阶段之后到达
ffs: GET_MAX_LUN reply failed: Connection reset by peer   # 20 秒后
dwc3: request ... was not queued to ep0out
```

主机把 GET_MAX_LUN 和第一个 CBW 并发发出；主线程阻塞在 `write(ep_in, CSW)`
（FunctionFS 每个端点只有一个 request，等主机读走上一个包），无法处理 ep0，
主机等 20 秒后复位。把 ep0 事件处理放进独立线程后问题消失。

### 4.4 缓冲区与吞吐

- CBW/CSW 小包走 ep0 之外的两个 bulk 端点；
- 数据阶段 64 KiB 缓冲，按 2048 的整数倍分块；
- WRITE 直接 `media_write_raw_blocks`，支持 2048/2352/2336/2056… 源扇区；
- READ 按 64 KiB 从文件读、写 bulk；
- 没有做原固件的双缓冲预取——在 A55 + 内核页缓存下没有必要。

## 5. 关键问题与修复

### 5.1 `media.c` 的 seek 缓存失效（真实 bug）

`read_iso_label()` 和 `infer_cdrw_state()` 直接 `f_read()`，却没有更新
`media.c` 自己的 `s_pos/s_pos_valid` 缓存。`f_read` 已经移动了底层文件指针，
下一次 `seek_file()` 若命中缓存就会跳过 `f_lseek`，从错误位置读。

复现：加载 ISO 后第一次 `READ(10) LBA=16` 返回的是 **LBA 17** 的数据（首字节
`0x02` 而不是 `0x01`），第二次读又正常。修复：这两个函数每次 `f_read` 后
`s_pos_valid = false`。

这个 bug 在 RP2350 固件里同样存在（FatFs 的 `f_read` 也会推进文件指针），只是
很少在 LBA 16 命中。建议上游一并修复。

### 5.2 configfs 符号链接的相对路径

`configfs_symlink()` 按**调用进程的 cwd** 解析 target。用
`../../functions/ffs.cdrw` 会得到 `ENOENT`。改用绝对路径
`/sys/kernel/config/usb_gadget/<name>/functions/ffs.<name>`。

### 5.3 FunctionFS strings 不能有尾部填充

内核 `__ffs_data_got_strings()` 在解析完 `str_count` 个字符串后要求
`len == 0`。用固定 32 字节数组会多出 16 个 0，返回 `EINVAL`。改成按
`16 + 2 + strlen+1` 精确构造。

### 5.4 USB/IP vudc 的 teardown oops

`usbip-vudc` 路径可以枚举、`usbip list -d` 能看到 gadget，但在关闭 FunctionFS
时内核在 `vep_dequeue()` 空指针崩溃：

```
vep_dequeue+0x2c/0xec [usbip_vudc]
usb_ep_dequeue
functionfs_unbind
ffs_func_unbind
...
ffs_ep0_release
```

随后 vudc 自旋锁被永久持有，相关进程进入 D 状态，只能重启清理。因此
`--loopback` 仅作实验/调试，不作为推荐路径。真正的目标是本板 DWC3。

### 5.5 dwc3 teardown 警告

正常退出时偶尔打印：

```
dwc3 fcc00000.usb: request ... was not queued to ep0out
```

发生在 unbind 后关闭 ep0 的瞬间，不影响下一次绑定，属于 benign warning。

## 6. 性能实测

方法：本板 USB3.0 host 口 ↔ OTG 口 A-A 自环；主机侧用标准 Linux
`usb-storage`/`sr_mod`；`dd` 测顺序读，`wodim` 测刻录。

### 6.1 读

```
64 MiB, O_DIRECT:   31.0 MB/s
64 MiB, buffered:   29.9 MB/s
```

USB 2.0 HS 理论 480 Mbps = 40 MB/s（不含协议开销），实测 31 MB/s 约为
理论值的 78%，已经接近 BOT + bulk 的实际上限。

### 6.2 写

```
wodim -v -tao ... /tmp/big.iso
Track 01: Total bytes read/written: 67483648/67483648 (32951 sectors)
Writing  time:    3.185s
Average write speed 138.3x
BURN-Free was never needed
回读 sha256: 一致
```

20.6 MB/s ≈ 138x CD。写比读慢，原因是 SCSI WRITE 路径每块都要经过
`media_write_blocks` 更新元数据 + 内核文件写入，且主机侧 wodim 的 FIFO 会
限制突发；仍有充足余量。

注意：盘仍上报 **4x**（`GET PERFORMANCE`/ATIP 沿用 RP2350 的保守值），
`wodim` 显示 4x 但实际写入远快于 4x。这是"上报速度"和"实际吞吐"分离的结果，
不影响正确性。

### 6.3 与 RP2350 对比

| | RP2350 | Orange Pi 3B | 提升 |
|---|---|---|---|
| USB | FS 12 Mbps | HS 480 Mbps | 40x 理论 |
| 读 | ~0.5 MB/s | 31 MB/s | ~62x |
| 写 | ~0.4 MB/s | 20.6 MB/s | ~51x |
| 4x 刻录 700 MiB | ~20 分钟 | ~35 秒（按 20 MB/s） | ~34x |

## 7. 可靠性与边界

- **掉电/重启**：`.CDR` 数据由内核页缓存 + `fsync`（SYNCHRONIZE CACHE /
  CLOSE SESSION）保证；`.meta` 最后写，CRC 校验，损坏时回退到内容推断。
- **空间**：`fallocate` 预分配 700 MiB，避免刻录中途 ENOSPC；文件系统需支持
  `fallocate`（ext4/xfs/btrfs 均可）。
- **多主机**：同一 UDC 只能被一个 host 枚举；自环和外部 PC 二选一。
- **托盘**：`START STOP UNIT` 的 eject 会让 `media_eject()` 关闭文件，
  load 时 `media_reload()` 重新打开同一路径；不会真正断电。
- **并发控制请求**：GET_MAX_LUN/BOT reset 由 ep0 线程处理，和 bulk 数据阶段
  并发安全（不同端点）。

## 8. DVD-RW / BD-RE 扩展（v2.1）

USB 2.0 HS 的实测写吞吐是 20.6 MB/s，换算成光盘倍速：

| 介质 | 1x 数据率 | 20.6 MB/s 相当于 |
|---|---|---|
| CD | 150 KB/s | 137x |
| DVD | 1.385 MB/s | 14.9x |
| BD | 4.5 MB/s | 4.6x |

所以带宽不再是限制：DVD-RW 8x、BD-RE 4x 都很宽裕，25 GB 满盘按 20 MB/s
约 21 分钟。真正的工作量在 **MMC profile、Format Capacities、Track Info 和
Disc Structure**，而不是传输。

### 8.1 介质模型

新增两个 profile 与文件扩展名：

| 扩展名 | Profile | 容量 | 镜像 |
|---|---|---|---|
| `.DVD` | `0x0013` DVD-RW Restricted Overwrite | 2295104 扇区（4.7 GB） | 稀疏 |
| `.BD` | `0x0043` BD-RE | 12219392 扇区（25 GB） | 稀疏 |

`.meta` 升到 v2，在原 `reserved[0]` 位置写入 `profile`（48 字节布局不变），
v1 旧镜像仍按 CD-RW 读取。DVD/BD 镜像用 `ftruncate` 建稀疏文件，只写
首尾各 1 MiB 用于快速擦除/格式化；`du` 实际约 2 MiB。

### 8.2 主机工具真正要求什么

直接读了 dvd+rw-tools 7.1 与 libburn 的源码，并用 `growisofs` /
`dvd+rw-format` / `dvd+rw-mediainfo` 实测，关键点：

1. **GET CONFIGURATION** 的当前 profile 必须是 `0x13` / `0x43`，profile list
   要包含它们；BD 还需要 Page 2A 的 DVD read 位（bit 3），否则
   `dvd+rw-mediainfo` 直接报 "not a DVD unit"。
2. **READ FORMAT CAPACITIES**：
   - DVD-RW：`0x00`（full formatted）、`0x15`（quick format）、`0x10`
     （full format）、`0x13`（quick grow）。
   - BD-RE：`0x00`、`0x30`（带 spare area）、`0x31`（无 spare area）。
   - `growisofs` 找 `0x00` 时**从 descriptor #1 开始**（跳过 #0），所以
     DVD-RW 要把 `0x00` 再放一份在 #1。
   - descriptor 0 的低 2 位表示 formatted（2）/unformatted（0）。
3. **READ TRACK INFORMATION**：DVD-RW 的 `track[6]` bit4 必须表示
   “fixed packet”，并在 `track+20` 给出 packet size（16），否则
   `dvd+rw-format` 报 "track is not formatted for fixed packet size"。
4. **FORMAT UNIT**：DVD/BD 格式化后必须是 **appendable**（Disc Status=1，
   session_open），不能像 CD-RW 那样留成 blank；否则 `growisofs` 报
   "media is not appendable"。
5. **READ DISC STRUCTURE**：DVD 的 format 0 返回 book type `0x30`
   （DVD-RW）；BD 的 format 0 返回 `DI` 和 Media ID。Structure List（0xFF）
   返回 Physical Format / Recording Management 两个条目。
6. **GET PERFORMANCE**：type 3 与 type 4 的 16 字节 descriptor 字段布局不同；
   type 4 的 `+4/+12` 是写速度，`+8` 是 end LBA，否则 growisofs 会把
   end LBA 当速度，打印出 “1697x”。

### 8.3 实机结果

```console
$ dvd+rw-mediainfo /dev/sr0
 Mounted Media:         13h, DVD-RW Restricted Overwrite
 ...
 READ FORMAT CAPACITIES:
  reserved:  2295104*2048=4700372992
  15h(800):  2295104*2048=4700372992
  10h(800):  2295104*2048=4700372992
  13h(800):  2295104*2048=4700372992
 READ CAPACITY: 2295104*2048=4700372992

$ growisofs -Z /dev/sr0=examples/demo.iso
 ...
 /dev/sr0: flushing cache
```

- DVD-RW：写入 320 扇区（growisofs 按 16 扇区补齐），前 312 扇区与源 ISO
  逐字节一致，`blkid` 识别 iso9660，可挂载；Disc 状态 appendable。
- BD-RE：`growisofs` 自动 `FORMAT UNIT` + 写入 312 扇区 + close，Disc 状态
  complete，回读一致，可挂载。
- `dvd+rw-mediainfo` 对 BD-RE 显示 `43h`、25 GB、Media ID `OPI3B`。

### 8.4 已知边界

- DVD-RW 通过 `growisofs -Z` 写入后没有 finalize（工具只在 quickgrow 时
  CLOSE），所以 Disc Status 是 appendable；数据完整可读，但不等同于
  DVD-ROM 兼容的 complete 盘。
- 只验证 ISO9660 数据盘；UDF、DVD-Video、音频 CD 未验证。
- DVD 的 legacy lead-out 在 mediainfo 里显示 0（结构字段展示瑕疵）。
- 25 GB BD-RE 满盘约 21 分钟（20 MB/s），受存储和 USB 实际吞吐限制。

## 9. 结论与后续

核心功能已经**直接运行在 Orange Pi 3B 上**，且完成了从枚举、读取、CD-RW
刻录、DVD-RW/BD-RE 格式化+刻录、校验、擦除到弹出/加载的闭环。相比 RP2350，
瓶颈从 USB FS + SPI SD 转移到了 USB HS 协议开销，读 31 MB/s、写 20.6 MB/s
已足够支持 137x CD、15x DVD、4.6x BD。v2.2 又加了本地 UI：运行时启用
排针 I2C2，SSD1306 状态/菜单 + EC11 操作。UI 用独立线程每 50 ms 刷新，
通过一把递归媒体锁与 SCSI 命令串行化；编码器默认关闭，避免未接线时浮空
引脚误触发。

后续优先级：

1. **控制接口**：Unix socket / CLI，实现运行时 `load/eject/new/blank`，
   配合 `scsi_notify_media_change()` 让主机重新读 TOC。
2. **上报速度**：把 `GET PERFORMANCE`、Mode Page 2A、ATIP 参考速度从 4x
   改为 32x/48x，减少主机侧保守限速。
3. **启动期 DT overlay**：在 U-Boot 阶段应用 `dr_mode=peripheral` 并删除
   `vcc5v0_otg` 的 `always-on`，免去运行时 unbind/rebind 和 VBUS 脚本。
4. **Windows 验证**：ImgBurn 刻录/Verify；IMAPI 的限制沿用原项目结论。
5. **OLED/EC11**：用 `/dev/i2c-*` + libgpiod 复刻原 UI；或做一个小型 Web UI。
6. **多 LUN 工具盘**：LUN0 数据盘 + LUN1 只读工具 ISO。

## 10. 参考

- [Orange Pi 3B 产品参数](http://www.orangepi.cn/html/hardWare/computerAndMicrocontrollers/parameter/Orange-Pi-3B.html)
- [Orange Pi 3B Wiki（接口、烧录、USB2.0 burning port）](http://www.orangepi.org/orangepiwiki/index.php/Orange_Pi_3B)
- [Linux upstream Orange Pi 3B DTS 补丁](https://patchew.org/linux/20240626230319.1425316-1-jonas@kwiboo.se/20240626230319.1425316-3-jonas@kwiboo.se/)
- [FunctionFS 文档](https://docs.kernel.org/usb/functionfs.html)
- [USB Gadget configfs 文档](https://docs.kernel.org/usb/gadget_configfs.html)
- [USB Mass Storage Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
- [USB/IP 协议](https://docs.kernel.org/usb/usbip_protocol.html)
- [T10 MMC-4 命令集](https://www.cs.rit.edu/usr/local/pub/wrc/doc/hardware/scsi/scsi3/cmdsets/mmc4r01g.pdf)
