# 实机验证记录

日期：2026-11-09  
硬件：Orange Pi 3B（RK3566，8 GB，Debian 13 Trixie）  
内核：`6.6.0-rc5-rockchip-rk356x`（厂商 BSP）  
接线：蓝色 USB3.0 口 ↔ 蓝口下方 USB2.0 口，USB-A 公对公自环；Type-C 供电  
工具：`wodim 1.1.11`、`dd`、`blkid`、`mount`、`usbip`

## 1. 构建与单元测试

```console
$ make -j4
$ make test
scsi tests: all passed
test_media: all passed
test_bot: all passed
```

## 2. 把 OTG 切成 peripheral

```console
$ sudo tools/enable-otg.sh
enable-otg: fcc00000.usb ready (not attached)

$ ls /sys/class/udc
fcc00000.usb  usbip-vudc.0
```

dmesg 只有一条 overlay 警告：

```
OF: overlay: WARNING: memory leak will occur if overlay removed,
    property: /usb@fcc00000/dr_mode
```

## 3. 枚举

```console
$ sudo ./build/pi3b-cdrw-emu --root . \
      --image ./examples/demo.iso --udc fcc00000.usb
...
[  ...] ffs: bound to fcc00000.usb (/dev/ffs-cdrw)
[  ...] ffs: BIND
[  ...] ffs: ENABLE
[  ...] ffs: SETUP type=A1 req=FE val=0000 idx=0000 len=1   # GET_MAX_LUN
```

主机侧（本板 xHCI）：

```
usb 5-1: new high-speed USB device number 3 using xhci-hcd
usb 5-1: New USB device found, idVendor=1209, idProduct=cd01, bcdDevice= 1.00
usb 5-1: Product: CD-RW USB Drive
usb 5-1: Manufacturer: Orange Pi
usb-storage 5-1:1.0: USB Mass Storage device detected
scsi host1: usb-storage 5-1:1.0
scsi 1:0:0:0: CD-ROM  OrangePi CD-RW USB 2.0  1.00 PQ: 0 ANSI: 5
sr 1:0:0:0: [sr0] scsi3-mmc drive: 4x/4x cd/rw tray
sr 1:0:0:0: Attached scsi CD-ROM sr0
```

```console
$ lsblk
sr0  11:0  1  624K  0 rom
```

## 4. 读取 ISO

```console
$ blkid /dev/sr0
/dev/sr0: BLOCK_SIZE="2048" UUID="2026-11-04-11-47-26-00"
          LABEL="RPCDRW_DEMO" TYPE="iso9660"

$ sudo dd if=/dev/sr0 of=/tmp/sr0-read.iso bs=2048 count=312 status=none
$ sha256sum /tmp/sr0-read.iso examples/demo.iso
6ec765a1...03b7f5  /tmp/sr0-read.iso
6ec765a1...03b7f5  examples/demo.iso

$ sudo mount -o ro /dev/sr0 /mnt/cdrw-test
$ find /mnt -maxdepth 1 -type f -printf '%s %p\n'
262144 /mnt/cdrw-test/DATA.BIN
   369 /mnt/cdrw-test/README.TXT
```

## 5. 空白盘 + wodim 刻录 + 回读

```console
$ sudo ./build/pi3b-cdrw-emu --root . --new --no-usb
created blank disc: ./discs/DISC01.CDR
media: CD-RW blank  capacity 358400 sectors  writable yes

$ sudo ./build/pi3b-cdrw-emu --root . \
      --image ./discs/DISC01.CDR --udc fcc00000.usb
...
$ sudo wodim -v -tao dev=/dev/sr0 speed=4 /tmp/sr0-read.iso
...
Track 01: Total bytes read/written: 638976/638976 (312 sectors).
Writing  time:    0.082s
Average write speed  61.4x.
Fixating...
BURN-Free was never needed.

$ sudo dd if=/dev/sr0 of=/tmp/sr0-burned.iso bs=2048 count=312 status=none
$ sha256sum /tmp/sr0-burned.iso examples/demo.iso
6ec765a1...03b7f5  /tmp/sr0-burned.iso
6ec765a1...03b7f5  examples/demo.iso
```

`.meta`（小端）：

```
size 48 magic b'RPCDRW01'
version=1 flags=0x0002 capacity=358400 data_lba=312 nwa=312 track_start=0
 COMPLETE
```

## 6. 64 MiB 吞吐与回读

```console
$ xorriso -as mkisofs -o /tmp/big.iso -V BIGTEST \
      -graft-points /BIG.BIN=/tmp/big.bin
Written to medium : 32951 sectors

$ sudo wodim -v -tao dev=/dev/sr0 speed=4 /tmp/big.iso
...
Track 01: Total bytes read/written: 67483648/67483648 (32951 sectors).
Writing  time:    3.185s
Average write speed 138.3x.
Min drive buffer fill was 0%
BURN-Free was never needed.

$ sudo dd if=/dev/sr0 of=/tmp/big-read.iso bs=2048 count=32951 status=none
$ sha256sum /tmp/big.iso /tmp/big-read.iso
b6754a43...4fb343  /tmp/big.iso
b6754a43...4fb343  /tmp/big-read.iso

$ blkid /dev/sr0
/dev/sr0: LABEL="BIGTEST" TYPE="iso9660"
$ sudo mount -o ro /dev/sr0 /mnt/big && ls -l /mnt/big
-rw-rw-r-- 1 orangepi orangepi 67108864 BIG.BIN
```

顺序读：

```console
$ sudo dd if=/dev/sr0 of=/dev/null bs=65536 count=1024 iflag=direct
67108864 bytes copied, 2.16563 s, 31.0 MB/s
$ sudo dd if=/dev/sr0 of=/dev/null bs=65536 count=1024
67108864 bytes copied, 2.24186 s, 29.9 MB/s
```

## 7. 快速擦除

```console
$ sudo wodim -v -blank=fast dev=/dev/sr0
Blanking PMA, TOC, pregap
Blanking time:    0.026s

$ sudo dd if=/dev/sr0 of=/tmp/blank.bin bs=2048 count=4 status=none
$ od -An -tx1 -N32 /tmp/blank.bin
 00 00 00 00 ...（全 0）

.meta: flags=0x0001 data_lba=0 nwa=0  → BLANK
```

## 8. 弹出 / 加载

```console
$ sudo eject /dev/sr0
$ sudo eject -t /dev/sr0
```

驱动日志中 `START STOP UNIT (1Bh)` 共出现 20 次，状态均为 0；弹出后主机
`blkid` 无介质，加载后恢复。

## 9. VBUS 关闭验证（外部 PC 前置条件）

```console
$ sudo tools/otg-vbus-off.sh
otg-vbus-off: unbound vcc5v0-otg-regulator
otg-vbus-off: GPIO0_A5=0; the host must supply VBUS
otg-vbus-off: board power must come from the Type-C connector

$ sudo dd if=/dev/sr0 ... skip=16 count=1 | od -An -tx1 -N6
 01 43 44 30 30 31        # ISO PVD "CD001" 仍可读
```

说明 OTG 口在板侧 VBUS 关闭时，仍能靠对端 host 口的 VBUS 正常枚举/读取，
即"PC 供 VBUS + 板子 Type-C 供电"的接法成立。

## 10. USB/IP 实验（负面结果）

`usbip-vudc` 也能枚举：

```
usb 1-1: New USB device found, idVendor=1209, idProduct=cd01
usb-storage 1-1:1.0: USB Mass Storage device detected
```

但关闭 FunctionFS 时内核在 `vep_dequeue()` 崩溃：

```
CPU: 3 PID: ... Comm: pi3b-cdrw-emu
pc : vep_dequeue+0x2c/0xec [usbip_vudc]
Call trace:
 vep_dequeue
 usb_ep_dequeue
 functionfs_unbind
 ffs_func_unbind
 purge_configs_funcs
 configfs_composite_unbind
 gadget_unbind_driver
 ...
 ffs_ep0_release
```

随后 vudc 锁被永久持有，相关进程进入 D 状态，需要重启清理。
**结论：USB/IP 仅用于无 UDC 时的冒烟测试，不进入生产路径。**

## 11. 已知日志噪声

正常退出时偶见：

```
dwc3 fcc00000.usb: request ... was not queued to ep0out
```

发生在 unbind 后关闭 ep0 的瞬间；下一次绑定正常，可忽略。

开机枚举时 sr_mod 可能打印：

```
sr 1:0:0:0: [sr0] Hmm, seems the drive doesn't support multisession CD's
```

这是空白/单会话盘的能力探测提示，不影响读写；原 RP2350 固件在 Linux 下也有
同类提示。

## 12. DVD-RW（v2.1）

```console
$ sudo ./build/pi3b-cdrw-emu --root . --profile dvd-rw --new --no-usb
media: DVD-RW blank
  capacity  : 2295104 sectors (4590208 KiB)
$ ls -lh discs/DISC01.DVD
-rw-r--r-- 1 orangepi orangepi 4.4G ...   # 稀疏
$ du -h discs/DISC01.DVD
2.0M
```

`dvd+rw-mediainfo`：

```
Mounted Media:         13h, DVD-RW Restricted Overwrite
READ FORMAT CAPACITIES:
 reserved:  2295104*2048=4700372992
 15h(800):  2295104*2048=4700372992
 10h(800):  2295104*2048=4700372992
 13h(800):  2295104*2048=4700372992
READ TRACK INFORMATION[#1]:
 Track State:           blank
 Next Writable Address: 0*2KB
 Free Blocks:           2295104*2KB
READ CAPACITY:          2295104*2048=4700372992
```

`growisofs` 写入 + 回读：

```console
$ sudo growisofs -Z /dev/sr0=./examples/demo.iso
...
/dev/sr0: flushing cache

$ sudo dd if=/dev/sr0 of=/tmp/dvd-read.iso bs=2048 count=320 status=none
$ cmp -n 638976 /tmp/dvd-read.iso examples/demo.iso && echo IDENTICAL
IDENTICAL

$ sudo blkid /dev/sr0
... LABEL="RPCDRW_DEMO" TYPE="iso9660"
$ sudo mount -o ro /dev/sr0 /mnt && ls /mnt
README.TXT  DATA.BIN
```

`.meta`：`flags=0x0004`（appendable）、`data=320`、`profile=0x0013`。
`growisofs -Z` 没有 finalize，所以 Disc Status 是 appendable；数据完整可读。

## 13. BD-RE（v2.1）

```console
$ sudo ./build/pi3b-cdrw-emu --root . --profile bd-re --new --no-usb
media: BD-RE blank
  capacity  : 12219392 sectors (24438784 KiB)
$ du -h discs/DISC01.BD
2.0M
```

`dvd+rw-mediainfo`：

```
Mounted Media:         43h, BD-RE
 Media ID:              OPI3B/
READ FORMAT CAPACITIES:
 reserved:  12219392*2048=25025314816
 30h(800):  12219392*2048=25025314816
 31h(800):  12219392*2048=25025314816
READ CAPACITY:          12219392*2048=25025314816
```

`growisofs`（检测到 blank，自动 FORMAT UNIT）：

```console
$ sudo growisofs -Z /dev/sr0=./examples/demo.iso
/dev/sr0: pre-formatting blank BD-RE for 25.0GB...
...
/dev/sr0: flushing cache

$ sudo dd if=/dev/sr0 of=/tmp/bd-read.iso bs=2048 count=312 status=none
$ cmp -n 638976 /tmp/bd-read.iso examples/demo.iso && echo IDENTICAL
IDENTICAL
$ sudo blkid /dev/sr0
... LABEL="RPCDRW_DEMO" TYPE="iso9660"
$ sudo mount -o ro /dev/sr0 /mnt && ls /mnt
README.TXT  DATA.BIN
```

`.meta`：`flags=0x0002`（complete）、`data=312`、`profile=0x0043`。

## 14. CD-RW 回归（v2.1）

加入 DVD/BD profile 后重新跑 CD-RW：

```console
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.CDR --udc fcc00000.usb
$ sudo wodim -v -tao dev=/dev/sr0 speed=4 ./examples/demo.iso
Track 01: Total bytes read/written: 638976/638976 (312 sectors).
Average write speed  54.4x.
Fixating time:    0.006s
$ sudo dd if=/dev/sr0 of=/tmp/cd-reg-read.iso bs=2048 count=312 status=none
$ sha256sum /tmp/cd-reg-read.iso examples/demo.iso
6ec765a1...03b7f5  /tmp/cd-reg-read.iso
6ec765a1...03b7f5  examples/demo.iso
```

v2.1 起 INQUIRY 产品串为 `DVD/BD USB 2.0`，USB 产品串为
`DVD/BD USB Drive`；VID:PID 仍为 `1209:CD01`。

## 15. 说明

- 早期 CD 记录中的产品串 `CD-RW USB 2.0` 是 v2.0 的历史输出。
- 一次 `usbip-vudc` teardown oops 最终导致板子重启，重启后
  `/sys/class/udc` 和 configfs overlay 清空；重新执行
  `tools/enable-otg.sh` 即可恢复。主路径（DWC3）不受影响。

## 16. 本地 OLED / 编码器（v2.2）

### 16.1 I2C2 热启用

```console
$ sudo tools/enable-i2c2.sh
enable-i2c2: /dev/i2c-2 ready
$ sudo i2cdetect -y 2
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- --
...
```

当前没有接 SSD1306，所以扫描为空；daemon 会打印
`oled: no SSD1306 found ...` 并自动 headless，USB 不受影响。

### 16.2 mock 帧渲染

DVD-RW（appendable）：

```console
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.DVD \
       --no-usb --oled-mock /tmp/oled-dvd.pgm
[ ... ] oled: mock panel -> /tmp/oled-dvd.pgm
USB disabled; UI running (Ctrl-C to exit)
$ python3 tools/pgm2png.py /tmp/oled-dvd.pgm /tmp/oled-dvd.png
```

BD-RE（complete）：

```console
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.BD \
       --no-usb --oled-mock /tmp/oled-bd.pgm
[ ... ] media: BD-RE READY cap=12219392 data=312 nwa=312 meta=yes
[ ... ] oled: mock panel -> /tmp/oled-bd.pgm
```

渲染结果见 [images/ui-dvd.png](images/ui-dvd.png) 和
[images/ui-bd.png](images/ui-bd.png)：标题、profile、盘名、状态、容量/已用、
写入百分比和 USB 状态都在 128x64 内。

无 USB 时 `USB --`；主机完成 SETCONFIG 后为 `USB HS`。

### 16.3 编码器

```console
$ sudo ./build/pi3b-cdrw-emu ... --no-usb --encoder
[ ... ] encoder: EC11 ready on GPIO 118/128/129
```

GPIO v2 申请、内部上拉、A/B/按键读数链路都通。**没有接编码器时**，浮空线
会产生假脉冲（实测一次就跳进了菜单），所以默认不启用，必须显式
`--encoder`；接好 KY-040 后三根线被主动驱动，不会浮空。

早期编码器和 UI 在同一个 50 ms 线程里采样，一个 detent 的 4 个跳变经常被
漏掉，表现为按键灵敏、旋转几乎不动。现在编码器有独立线程每 2 ms 采样，
UI 线程只消费累计事件；服务日志会打印
`encoder: EC11 ready ... (4 steps/detent)`。若硬件是 2 跳变/格，用
`--encoder-steps 2`。需要看原始波形时可加 `--encoder-debug`，日志会打印
`encoder: state X -> Y`；完整序列是 `0 -> 1 -> 3 -> 2 -> 0`（或反向）。

按键改为**松手时短按、按住 800 ms 长按**：按住不会再立即触发菜单项；
状态页长按无动作，菜单/选盘/确认页长按返回。

删除镜像：`MENU -> Delete disc`，确认后删除镜像 + `.meta` + 对应
`.last_disc`；当前加载的盘会提示 `EJECT FIRST`。`test_media` 覆盖了
`storage_delete_disc()` 的文件/`.meta`/last 清理。

### 16.4 USB 路径下的 UI 线程

用 `--oled-mock` 启动完整 USB daemon，主机枚举出 `/dev/sr0`，UI 线程每 50 ms
刷新。实测在 UI 运行时：

```console
$ sudo wodim -v -blank=fast dev=/dev/sr0
$ sudo wodim -v -tao dev=/dev/sr0 speed=4 ./examples/demo.iso
$ dd if=/dev/sr0 of=/tmp/cd3b.iso bs=2048 count=312 status=none
$ cmp -n 638976 /tmp/cd3b.iso examples/demo.iso && echo WRITE_READ_IDENTICAL
WRITE_READ_IDENTICAL
```

PGM 帧从 `BLANK` 自动变成 `READY 624 KB / USB READY`，说明 UI 与 SCSI 的
媒体锁没有死锁，状态也能跟上主机操作。`LOCK` 标记会在主机持锁时出现。
实时画面见 [images/ui-cd-usb.png](images/ui-cd-usb.png)。

### 16.5 实机识别

接上实机 SSD1306（0x3C）和 EC11（排针 11/13/18）后启动：

```console
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.BD --encoder
[ ... ] oled: SSD1306 at i2c-2 0x3C
[ ... ] encoder: EC11 ready on GPIO 118/128/129
[ ... ] ffs: bound to fcc00000.usb (/dev/ffs-cdrw0)
```

面板显示和按键操作仍需现场确认；旋转/按键会打印
`UI: input d=... p=... l=... view=...`，可据此检查方向和消抖。若方向相反，
对调 A/B 或使用 `--encoder-a/--encoder-b`。

### 16.6 重启后模块未加载

重启后曾出现：

```console
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.BD --encoder
[ffs: cannot create /sys/kernel/config/usb_gadget/cdrw0: Operation not permitted]
```

原因是 `/sys/kernel/config/usb_gadget` 不存在（`libcomposite`/`usb_f_fs`
未加载），configfs 对创建子系统目录返回 EPERM。手动 `modprobe` 后恢复。
v2.2 的 `ffs_gadget_setup()` 现在会先自动 `modprobe`
`libcomposite` + `usb_f_fs`；卸掉模块后重跑 daemon 验证自动加载成功：

```console
$ sudo rmmod usb_f_fs libcomposite
$ sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.BD --encoder
[ ... ] oled: SSD1306 at i2c-2 0x3C
[ ... ] encoder: EC11 ready on GPIO 118/128/129
[ ... ] ffs: BIND
```

## 17. systemd 开机自启（v2.2）

```console
$ sudo deploy/install.sh
Created symlink /etc/systemd/system/multi-user.target.wants/pi3b-cdrw.service ...
● pi3b-cdrw.service - Orange Pi 3B USB CD/DVD/BD emulator
     Loaded: loaded (/etc/systemd/system/pi3b-cdrw.service; enabled)
     Active: active (running)
    Process: ... ExecStartPre=otg-vbus-off.sh (status=0/SUCCESS)
    Process: ... ExecStartPre=enable-otg.sh (status=0/SUCCESS)
    Process: ... ExecStartPre=enable-i2c2.sh (status=0/SUCCESS)
   Main PID: ... (pi3b-cdrw-emu)
      Tasks: 3
```

- 状态目录 `/var/lib/pi3b-cdrw`，光盘库 `/var/lib/pi3b-cdrw/discs`。
- 日志里同时出现 `oled: SSD1306 at i2c-2 0x3C`、`encoder: EC11 ready`、
  `ffs: BIND`；主线程、ep0、UI、编码器采样共 4 个 task。
- `systemctl stop` 后 gadget 目录清空、UDC 回到 `not attached`；
  `systemctl start` 重新创建并识别 OLED/编码器。
- `systemctl is-enabled` 为 `enabled`；本次会话未重启整机，冷启动由该 unit
  的 ExecStartPre 走同一套脚本。
- USB 拔出时 `ffs: ep_out read: ... endpoint shutdown`，BOT worker 返回 -1，
  程序正常 `exit 0`；unit 用 `Restart=always` + `RestartSec=2` 自动拉起，
  OLED/编码器/UDC 重新就绪，不需要 shell。手动 `systemctl stop` 验证为
  inactive 且不会被拉起；`systemctl start` 后恢复 4 个 task。


## 18. Windows CD-RW 抓包回归（2026-11）

用 Windows + USBPcap 抓取真实 DVD-RW 驱动器装入空白 CD-RW 后资源管理器
读取并弹出“刻录光盘”对话框的 849 个 SCSI 事务，详见
[Windows USBPcap 抓包分析](windows-capture.md)。据此修正：

- 空白 CD-RW `READ CAPACITY` 返回真实容量（358399，Explorer 显示约 702 MiB）。
  真实 DVD-RW 抓包曾返回 0，但 Windows 用 `READ FORMAT CAPACITIES` 显示容量；
  模拟器返回 0 会让 Explorer 显示 0 字节并拒绝烧录，因此不照搬该 0；
- 空白 CD-RW `READ(10)` 返回 5/21，不再返回清零扇区；
- 空白 CD-RW `READ TOC` 返回 5/24，不再返回空 TOC；
- `READ DISC INFORMATION` 的 first/last track、disc type、byte 8、
  last-session 字段按抓包；
- `READ TRACK INFORMATION` 改为 48 字节模板，NWA 位于 offset 20/28；
- `MODE SENSE(2Ah)` 改为 56 字节，page 2A 数据 46 字节；
- `READ DISC STRUCTURE` 对 CD 返回 5/24；
- `GET CONFIGURATION` 增加 `0037` CD-RW media write support、`0023`
  8 字节 Formattable，去掉可写盘的 `0004`；
- `GET PERFORMANCE` type 0 / `14h` 返回 24 字节；
- CD 默认速度 24x read / 10x write。

`test_scsi` 已覆盖这些响应；Windows 端仍需按报告第 5 节复测。


## 19. Windows READ(10) 卡住修复（2026-11-10）

新抓包 `11101019.pcapng` 同时包含两个 USB 设备：

- **地址 1**：机器内置真实光驱（INQUIRY 为 Slimtype；115 条 0xF0 Realtek
  厂商命令）；
- **地址 10**：模拟器（INQUIRY 为 OrangePi；34 条命令）。

模拟器地址 10 的时间线：

```text
24.814  INQUIRY
24.842  GET CONFIG / MODE SENSE / GET PERFORMANCE
24.847  READ CAPACITY  -> CHECK CONDITION（带 8 字节零数据）
24.848  READ CAPACITY  -> GOOD
24.877  READ TOC fmt2  -> CHECK CONDITION（带 804 字节零数据）
24.883  READ(10) LBA0 x2 -> CHECK CONDITION，0 字节数据，CSW residue 4096
之后 23 秒模拟器没有任何新命令
```

`handle_read()` 在 `scsi_read_params()` 失败时只发了 CSW，没有完成 CBW 声明的
4096 字节 IN 数据阶段。Windows/ImgBurn 的 BOT 栈会等满这 4096 字节才去读
CSW，所以 Explorer 和 ImgBurn 一起卡住。真实光驱在 READ(10) 失败时会结束
数据阶段/让主机中止；模拟器的 FunctionFS 路径没有 stall 兜底，必须补数据。

修复：`src/usb/bot.c` 的 `fail_in_stage()` 在失败时用零填满剩余数据阶段，
再发 `CSW status=1/residue=0`；通用 IN 命令的错误路径也统一使用它。
`test_bot` 新增空白 CD-RW READ(10) 失败测试，断言 `4096 + 13` 字节和
CSW residue 0。

抓包工具现在支持按设备地址过滤，避免把真实光驱的 0xF0 流量算到模拟器：

```bash
tools/usbpcap_scsi.py 11101019.pcapng --addr 10 --summary
tools/usbpcap_scsi.py 11101019.pcapng --addr 10 --op 0x28 --hex
```


## 20. 空白 CD-RW 格式化、容量与 profile（2026-11-10）

抓包 `11101034.pcapng`（1483 个事务，设备地址 12，全部是模拟器）：

```text
FORMAT UNIT  CDB 04 11 00 00 00 00, data 12 字节
  00 02 00 08 00 04 7c a0 40 00 00 20
  format descriptor: 0x00047CA0 = 294048 blocks = 574 MiB
```

修复前 `media_format()` 对 CD-RW 故意保持 `blank=true`，FORMAT UNIT 返回
GOOD 后 Windows 立刻 `READ CAPACITY` 得到 0、`READ TOC` 得到 5/24，于是
判定“Windows 无法完成格式化”。同时 GET CONFIG 仍广告 BD-ROM/BD-RE，
Windows 把驱动器标成 **BD-ROM 驱动器**，并尝试 BD-RE 的实时文件系统。

修复：

- 新增 `media_format_capacity(blocks)`；`FORMAT UNIT` 从参数列表 offset 4
  读取 blocks，格式化后设置 `capacity_lba=294048`、CD-RW 为
  `blank=false, formatted=true, complete=true, session_open=false`
  （Disc Information `0x1E`，COMPLETE/state 3），`READ TRACK INFORMATION`
  返回 complete track（flags `0xB1`，leadout 在 offset 24/28）。
  `READ CAPACITY` 返回 294047，`READ TOC` 返回 track 1/leadout 294048，
  `READ(10)` 可读零扇区。真实成功抓包（fullformat.pcapng）在完整格式化
  后正是 `0x1E`；EMPTY 和 INCOMPLETE 都不会触发 Windows 的 WRITE。
  DVD-RW/BD-RE 保持 appendable。
- `READ FORMAT CAPACITIES` 对 CD-RW 返回抓包中的 4 个描述符：
  `00h` 名义 700 MiB、`10h` 294048 blocks（574 MiB）、`11h/12h` 空。
- 对 CD/DVD 介质，GET CONFIG 只广告 4 个 profile（CD-ROM/CD-RW/
  DVD-ROM/DVD-RW）；BD-ROM/BD-RE 只在 BD 介质时出现，BD 特性也只在
  BD 介质时广告。这样 Windows 不再把 CD-RW 当成 BD-ROM。
- `test_media` 覆盖 `FORMAT UNIT` 参数列表、格式化后的 READ CAPACITY/
  READ TOC/Disc Information；`test_scsi` 覆盖 36 字节 format capacities
  和 4-profile 列表。

仍然保留：空白盘格式化前 `READ CAPACITY=0`、`READ TOC=5/24`，这与真实
DVD-RW 抓包一致；格式化后才有 574 MiB 容量。

## 21. 真实完整格式化基准（2026-11-10）

`fullformat.pcapng`（真实空白 CD-RW，619 秒，3320 个事务，170 条 WRITE）：

- `BLANK` 后真实盘用 `TEST UNIT READY` 返回 `2/04/07` 约 31 秒；
- `FORMAT UNIT`（blocks=295264）后返回 `2/04/04`（format in progress）
  约 8 分钟；
- 格式化完成后 `READ DISC INFORMATION` 为 `0x1E`（COMPLETE/state 3），
  Track Information 为 complete track（flags `0xB1`）；
- 之后才出现第一条 `WRITE(10)`。

模拟器现在：FORMAT UNIT 后保持 COMPLETE 状态，并在 2.5 秒内让
`TEST UNIT READY` 返回 `2/04/04`，模拟这个“进行中→完成”过渡，随后
Windows 才读取完成状态并开始写 UDF。
