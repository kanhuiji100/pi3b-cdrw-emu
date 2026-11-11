# Windows USBPcap 抓包分析：空白 CD-RW

抓包文件：[CD-RW.pcapng](../CD-RW.pcapng)（USBPcap，382 KiB，5642 个 USB 包，
33.9 秒）。分析工具：[tools/usbpcap_scsi.py](../tools/usbpcap_scsi.py)。

> 抓包文件在源码工作区根目录；`deploy/install.sh` 只安装文档和工具，不会把
> 这个 382 KiB 的抓包复制到 `/opt`。在安装目录里看报告时请回到源码目录取它。

## 1. 结论

这份抓包很有价值。它不是简单的“读盘”流量，而是 Windows 资源管理器从
**识别光驱 → 读取空白 CD-RW → 弹出“刻录光盘”对话框** 的完整 SCSI 命令序列，
共重建出 **849 个 BOT 事务**：

```text
46 GET CONFIGURATION        242
03 REQUEST SENSE            123
f0 vendor (Realtek bridge)  102
25 READ CAPACITY             89
43 READ TOC/PMA/ATIP         80
51 READ DISC INFORMATION     48
00 TEST UNIT READY           41
4a GET EVENT STATUS          38
28 READ(10)                  26
12 INQUIRY                   18
ad READ DISC STRUCTURE       15
5a MODE SENSE                13
23 READ FORMAT CAPACITIES     8
ac GET PERFORMANCE            4
52 READ TRACK INFORMATION     2
```

CSW 状态：725 GOOD、121 CHECK CONDITION、3 个抓包截断。真实设备是
**Lite-On/“Slimtype” DVD A DU8AESH（DVD-RW，Realtek USB 桥）**，当前介质
profile 是 `0x000A`（CD-RW）。

最重要的发现不是某一个位，而是：**空白 CD-RW 在真实驱动器上是一个“没有
用户数据、没有 TOC，但可格式化/可写”的介质**。Windows 分别用
`READ CAPACITY`、`READ(10)`、`READ TOC`、`READ DISC INFORMATION`、
`READ TRACK INFORMATION` 和 `MODE SENSE(2Ah)` 来建立这个模型。模拟器原先
在这些命令上返回了“有 700 MB 数据”的假象。

## 2. 关键响应

### 2.1 INQUIRY

真实响应（36 字节）：

```text
05 80 00 32 5b 00 00 00
53 6c 69 6d 74 79 70 65   "Slimtype"
44 56 44 20 41 20 20 44 55 38 41 45 53 48 20 20
36 43 32 4d               "6C2M"
```

即 vendor `Slimtype`、product `DVD A  DU8AESH`、revision `6C2M`。
当前模拟器使用自己的 `OrangePi` / `DVD/BD USB 2.0` 标识，没有改；Windows
的 USB 存储枚举和 IMAPI 主要看 GET CONFIG，而不是产品字符串。

### 2.2 READ CAPACITY：空白盘 last LBA = 0

89 次 `25 00 00 00 00 00 00 00 00 00 00 00`，88 次成功，响应固定为：

```text
00 00 00 00   last LBA = 0
00 00 08 00   block size = 2048
```

这是该真实 DVD-RW 的响应，但 Windows Explorer 显示空白 CD-RW 容量时
还会使用 `READ FORMAT CAPACITIES`；模拟器若照搬 0，Explorer 会显示
0 字节并拒绝烧录（2026-11-10 实测）。因此模拟器对空白 CD-RW 返回
`capacity_lba - 1`（358399，约 702 MiB），与 Explorer 的“702 MB 可用”
一致；格式化后再按 format descriptor 缩到 294048（574 MiB）。

### 2.3 READ(10)：空白盘一律 LBA out of range

26 次 READ(10)，包括 LBA 0、LBA 1 和接近容量末尾的 LBA `0x4815F`，
全部返回 **CHECK CONDITION, sense 5/21/00（LBA out of range）**，没有数据。
原来的 BOT 读路径对 blank 盘会返回清零扇区；现在改为和真实盘一致。

### 2.4 READ TOC：空白盘返回 5/24

80 次 READ TOC（format 0/1/2），真实盘全部返回 **CHECK CONDITION**，
`REQUEST SENSE` 为：

```text
70 00 05 00 00 00 00 0a 00 00 00 00 24 00 ...
      ^key=5 ILLEGAL REQUEST             ^ASC=24 invalid field in CDB
```

抓包里唯一一条“成功的 READ TOC”在 24.769 s 返回了
`0b da 03 28 ...`，和 0xF0 的 Realtek 厂商数据完全相同，是解析器把相邻
0xF0 数据错误关联到 TOC 上，不是真实 TOC。真实盘在空白 CD-RW 上没有 TOC。

### 2.5 READ DISC INFORMATION：空白但 track/session 字段非零

48 次 `51`，其中 8 次 allocation=34 返回 34 字节，20 次 allocation=256、
20 次 allocation=4096 返回 36 字节（前 34 字节相同，后 2 字节为零）。
核心 34 字节：

```text
00 20 10 01 01 01 01 00 ff 00 00 00 00 00 00 00
00 61 22 17 00 4f 3b 4a 00 00 00 00 00 00 00 00 00 00
```

解释：

| 字段 | 值 | 含义 |
|---|---|---|
| byte 2 | `0x10` | erasable，state=0（empty），status=0（empty） |
| byte 3 | `0x01` | number of sessions? / reserved |
| byte 4 | `0x01` | first track number = 1 |
| byte 5 | `0x01` | last track number = 1 |
| byte 6 | `0x01` | last session number = 1 |
| byte 7 | `0x00` | disc type |
| byte 8 | `0xFF` | last session lead-in start (FFh for blank) |
| bytes 16-19 | `00 61 22 17` | last session start (ATIP-derived) |
| bytes 20-23 | `00 4F 3B 4A` | lead-out 79:59:74（80 分钟盘的最大 lead-out） |

原来的模拟器在 blank 时把 first track 设成 0、byte 7 设成 `0x20`、
byte 8 设成 0，last-session 字段也不一样。Windows 的 IMAPI 会读这些字段，
现在已按抓包值对齐。

### 2.6 READ TRACK INFORMATION：48 字节模板

只有 2 次 `52`，都是 track 1，返回 48 字节：

```text
00 2e 01 01 00 05 4f 01 00 00 00 00 00 00 00 00
00 00 00 00 00 05 7d a7 00 00 00 00 00 05 7d a7
ff ff ff ff 00 00 00 00 00 00 00 00 00 00 00 00
```

关键点：

- 数据长度 `0x2E`（46），总长 48；
- track mode `0x05`，flags `0x4F`；
- NWA 在 offset 20 和 28，值 `0x00057DA7`（真实盘 359337 blocks - 2）；
- free blocks 在 offset 32，值 `0xFFFFFFFF`。

原来的模拟器返回 36 字节，NWA 放在 offset 12/16，Windows 会按错误布局解析。

### 2.7 MODE SENSE(2Ah)：46 字节写参数页

13 次 MODE SENSE，其中 page 2A 的完整响应 56 字节：

```text
00 36 20 00 00 00 00 00
2a 2e
1f 17 f1 77 29 23 10 8a 01 00 02 00 10 8a 00 10
06 e5 06 e5 00 01 00 00 00 00 06 e5 00 01 00 00
06 e5 00 00 00 00 00 00 00 00 00 00 00 00
```

- 模式头 medium type = `0x20`（CD-RW）；
- page 2A 数据长度 `0x2E`（46）；
- 读速度 `0x108A`（4234 KB/s，24x），写速度 `0x06E5`（1765 KB/s，10x）；
- 包含 write type、session format、packet size 等 Windows 写参数。

原来的模拟器只返回 28 字节 page 2A（总 38 字节），缺少后半段；Windows
的 IMAPI 在准备“实时文件系统/Mastered”时会解析这些字段。

### 2.8 GET CONFIGURATION：CD-RW 写特性

242 次 `46`。真实盘对 `46 01 00 00 00 00 00 01 48` 返回 176 字节，
current profile `0x000A`，12 个 profile：

```text
002B 001B 001A 0016 0015 0014 0013 0011 0010 000A 0009 0008
```

Feature 列表（从 offset 8 开始）：

| Feature | 版本/长度 | 数据 | 含义 |
|---|---|---|---|
| `0000` | 3 / 48 | 12 profiles | Profile List |
| `0001` | 0x0B / 8 | `00 00 00 07 01 00 00 00` | Core |
| `0002` | 0x07 / 4 | `02 00 00 00` | Morphing |
| `0003` | 0x0B / 4 | `2B 00 00 00` | Removable |
| `0021` | 0x0D / 8 | `3F 0F 00 01 07 00 00 00` | Incremental Streaming |
| `0023` | 0x05 / 8 | 全 0 | Formattable |
| `002D` | 0x09 / 4 | `5F 00 3F 0F` | CD Track-at-Once |
| `002E` | 0x05 / 4 | `7F 00 0D 00` | CD Mastering |
| `0037` | 0x01 / 4 | `00 0F 00 00` | **CD-RW media write support** |
| `0100` | 0x07 / 4 | 0 | Power Management |
| `0105` | 0x07 / 4 | 0 | Timeout |
| `0107` | 0x11 / 4 | `1F 00 00 00` | Real-time Streaming |
| `0108` | 0x03 / 12 | `"475120527029"` | Drive Serial |

**没有 `0x0004`（Write Protect）**。也没有 `0x0027`（CD-RW CAV write）。
`0x0037` 是 Windows/IMAPI 判断“这张 CD-RW 可以写、可以选实时文件系统”的
关键特性；原模拟器完全没有它，同时 `0x0023` 的长度是 0，还错误地宣告了
`0x0004` Write Protect。这三处是最可能直接影响 Windows 烧录对话框的差异。

### 2.9 GET PERFORMANCE：24 字节，不是空响应

4 次 `AC`，Windows 分别请求 type 0 和 `cdb[1]=0x14`：

```text
type 0:
00 00 00 14 00 00 00 00 00 00 00 00 00 05 7d a8
00 00 10 8a 00 00 06 e5

type 14:
00 00 00 14 02 00 00 00 00 00 00 00 00 00 06 e5
00 05 7d a8 00 00 06 e5
```

原模拟器只处理 `type==3 || req==4`，对 Windows 的 `type 0` / `req 0x14`
返回 8 字节全零。现在两者都返回 24 字节描述符，速度取抓包值
（24x read / 10x write）。

### 2.10 READ FORMAT CAPACITIES：36 字节

8 次 `23`，7 次成功返回 36 字节（4 个描述符），1 次抓包截断：

```text
00 00 00 20
00 05 7d a9 01 00 08 00   type 00, formatted, 359337 blocks, 2048
00 04 81 60 40 00 00 20   type 10
00 00 00 00 44 00 00 20   type 11
00 00 00 00 48 00 00 20   type 12
```

原模拟器返回 20 字节（2 个 type 0 描述符）。Windows 对当前 CD-RW 主要
使用第一个描述符，所以这个差异优先级低于 2.8/2.9；仍然记录在“剩余差异”。

### 2.11 0xF0：Realtek 桥厂商命令，不是 SCSI MMC

102 次 6 字节 CDB `f0 09 00 00 00 00`，返回 16 字节：

```text
0b da 03 28 00 02 01 29 08 00 00 00 00 ff 7d 86
```

`0B DA` 是 Realtek 的 USB vendor ID，这是 Realtek 桥的厂商命令，由 Windows
的 Realtek 驱动发出。模拟器使用 1209:CD01 和标准 USB 存储，不会收到这些
命令，**不需要实现**。

### 2.12 REQUEST SENSE 样例

```text
6/28/00  UNIT ATTENTION, medium changed
5/24/00  ILLEGAL REQUEST, invalid field in CDB   (READ TOC / READ DISC STRUCTURE)
5/21/00  ILLEGAL REQUEST, LBA out of range       (READ(10) on blank)
```

原模拟器的 **BOT 读路径**对 blank 盘会返回清零扇区（`scsi_in_command`
层则是 5/20 invalid command）；READ TOC 返回空 4 字节 TOC。现在 BOT
分别对齐为 5/21 和 5/24。

## 3. 本次修改

代码：`src/core/scsi.c`

| 行为 | 修改 |
|---|---|
| 空白 CD-RW `READ CAPACITY` | last LBA 返回 0 |
| 空白 CD-RW `READ(10)` | 返回 5/21，不再返回清零扇区 |
| 空白 CD-RW `READ TOC` | format 0/1/2 返回 5/24 |
| 空白 CD-RW `READ DISC INFORMATION` | first track=1、disc type=0、byte8=FF、last-session 字段按抓包 |
| 空白 CD-RW `READ TRACK INFORMATION` | 48 字节模板，track mode 05、flags 4F、NWA capacity-2、free FFFFFFFF |
| CD-RW `MODE SENSE(2Ah)` | 46 字节写参数页，medium type 20，速度字段按当前设置 |
| CD `READ DISC STRUCTURE` | 返回 5/24，不再返回 4 KiB 零结构 |
| `GET CONFIGURATION` | 增加 `0037`、`0023` 补 8 字节、去掉可写盘的 `0004`；Core/Morphing/Removable/Incremental/TAO/Mastering 数据对齐；BD 特性码改为 `0040/0041` |
| `GET PERFORMANCE` | type 0 / `req 0x14` 返回 24 字节描述符 |
| 速度默认值 | 24x read / 10x write，与抓包 4234/1765 KB/s 接近 |

测试：`tests/test_scsi.c` 新增空白盘容量、READ(10)、READ TOC、Disc
Information、Track Information、mode page 2A、GET PERFORMANCE 的断言。

## 4. 和真实盘仍然不同的地方

这些不是本次必须修的，但下次 Windows 复测时可以重点看：

1. **Profile List 只有 6 个**（CD-ROM/CD-RW/DVD-ROM/DVD-RW/BD-ROM/BD-RE），
   真实 DVD-RW 报 12 个（含 CD-R、DVD-R、DVD+R/RW 等）。模拟器只广告
   自己真正支持的 profile，这是有意的；如果 Windows 因为 BD profile 仍
   选择不同流程，可以再做“按当前介质隐藏 BD profile”。
2. **READ FORMAT CAPACITIES 20 字节 vs 36 字节**（见 2.10）。当前盘第一个
   描述符已足够；如果需要完全复刻 DVD-RW 驱动的多格式描述符，再补。
3. **READ DISC INFORMATION 34 字节 vs 36 字节**。前 34 字节一致，真实盘在
   大 allocation 时多回 2 个零；Windows 两种都接受。
4. **INQUIRY 产品字符串不同**。模拟器是 `OrangePi` / `DVD/BD USB 2.0`，
   真实盘是 `Slimtype` / `DVD A  DU8AESH`。没有改，避免伪装成特定硬件。
5. **0xF0 Realtek 厂商命令** 不需要实现（见 2.11）。
6. **READ TOC format 3/4（PMA/ATIP）** 没有在抓包里出现；模拟器仍保留
   原有响应。

## 5. Windows 复测步骤

1. 确认服务是当前构建：

   ```bash
   systemctl status pi3b-cdrw --no-pager
   ```

2. 准备一张空白 CD-RW 镜像。若 `discs/` 里没有 `.CDR`：

   ```bash
   sudo systemctl stop pi3b-cdrw
   sudo /opt/pi3b-cdrw/build/pi3b-cdrw-emu \
       --root /var/lib/pi3b-cdrw --new --profile cd-rw --no-usb --no-ui
   ```

   然后用 UI 的 `Discs` 载入它，或把 unit 的 `--image` 临时指向该 `.CDR`。
   注意服务默认 `--image` 是 `DISC01.BD`；Windows 要测 CD-RW，必须让
   当前介质是 CD-RW。

3. 用 A-A USB 线连接 Orange Pi 的 OTG 口（蓝色 USB3.0 口下方的 USB2.0
   口）和 Windows；确认 `dwc3`/UDC 已绑定。

4. Windows 端用 USBPcap 从插入/挂载前开始抓包，插入后双击光驱，
   等待“刻录光盘”对话框。

5. 分析新抓包：

   ```bash
   tools/usbpcap_scsi.py windows-cdrw.pcapng --summary
   tools/usbpcap_scsi.py windows-cdrw.pcapng --op 0x46 --hex
   tools/usbpcap_scsi.py windows-cdrw.pcapng --op 0x25,0x28,0x43,0x51,0x52,0x5a,0xac,0xad --hex
   ```

6. 和本报告第 2 节对比。重点看：
   - `25` 是否 last LBA 0；
   - `28` 是否 5/21；
   - `43` 是否 5/24；
   - `51` 的 first/last track、byte 8、last-session；
   - `52` 是否 48 字节、NWA 在 offset 20/28；
   - `5A 2A` 是否 56 字节、page length 2E；
   - `46` 是否含 `0037`、`0023` 长度 8、没有 `0004`；
   - `AC` 是否 24 字节。

如果 Windows 仍然不弹“实时文件系统/Mastered”选择，把新抓包发来，按
`46/25/51/52/5A/AC` 的差异继续对齐。


## 6. 2026-11-10 复测：Explorer/ImgBurn 卡住

新抓包 `11101019.pcapng` 混入了两个设备：地址 1 是机器内置真实光驱
（115 条 0xF0 Realtek 厂商命令），地址 10 才是模拟器。按地址过滤后，
模拟器的最后一条命令是 24.883 s 的 `READ(10)` LBA0×2：

```text
READ(10) 28000000000000000200  dtl=4096
CSW status=1 residue=4096, data=0 字节
之后 23 秒没有新命令
```

`handle_read()` 在失败时只发 CSW，没有完成 CBW 声明的 4096 字节数据阶段；
Windows/ImgBurn 的 BOT 栈在等数据，所以不会去读 CSW。已在
`src/usb/bot.c` 增加 `fail_in_stage()`：失败时把剩余数据阶段用零填满，
再发 CSW，和通用 IN 命令错误路径一致。`test_bot` 覆盖了该场景。

这也说明抓包分析必须按 `usb.device_address` 过滤；工具现在支持：

```bash
tools/usbpcap_scsi.py 11101019.pcapng --addr 10 --summary
```


## 7. 2026-11-10 复测：格式化失败/0 字节

`11101034.pcapng` 只有一个设备（地址 12，模拟器），1483 个事务、290 个
CHECK CONDITION。Windows 反复读容量/TOC，并在 42–43 s 发送：

```text
BLANK A1 11
MODE SELECT 55 10 (page 05)
FORMAT UNIT 04 11, data:
  00 02 00 08 00 04 7c a0 40 00 00 20
  format blocks = 0x00047CA0 = 294048 (574 MiB)
```

这解释了截图里的“正在格式化 (574 MB)”。问题有两层：

1. 模拟器 GET CONFIG 广告了 BD-ROM/BD-RE，Windows 把 E: 认成
   **BD-ROM 驱动器**；CD-RW 被按 BD-RE 流程处理。
2. `FORMAT UNIT` 返回 GOOD，但 `media_format()` 对 CD-RW 仍保持
   `blank=true`，随后 `READ CAPACITY` 还是 0、TOC 还是 5/24，Windows
   判定格式化失败，显示 0 字节。

已修：`FORMAT UNIT` 解析 294048 blocks，CD-RW 格式化后必须是
**COMPLETE（Disc Information `0x1E`，state 3）**，`READ TRACK
INFORMATION` 返回 complete track（flags `0xB1`）。真实 `fullformat.pcapng`
成功格式化后正是这个状态，随后 Windows 才发第一条 `WRITE(10)`；EMPTY
和 INCOMPLETE 都不会触发写。CD/DVD 介质不再广告 BD profile；
`READ FORMAT CAPACITIES` 返回 4 个描述符（含 574 MiB 的 10h）。
