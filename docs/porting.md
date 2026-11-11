# 移植设计与代码导读

## 1. 复用边界

| 文件 | 来源 | 改动 |
|---|---|---|
| `src/core/scsi.c` | RP2350 固件 | INQUIRY 身份改成宏；增加 DVD/BD profile、Format Capacities、Track Info、Disc Structure |
| `src/core/media.c` | RP2350 固件 | 修 seek 缓存失效；增加介质 profile、meta v2、DVD/BD 格式化语义 |
| `src/core/util.c` | RP2350 固件 | 无（`CDRW_HOST_TEST` 下用 `clock_gettime`） |
| `src/core/*.h` | RP2350 固件 | `storage.h` 增加 root / profile / 稀疏镜像 API |
| `src/posix/ffshim.c` | 新 | FatFs 文件 API 的 POSIX 实现 |
| `src/posix/storage_posix.c` | 新 | 取代 `storage.c` + `sd_spi.c` + FatFs |
| `src/usb/bot.c` | 新（语义照 `usb_cdrom.c`） | 传输无关的 BOT 状态机；最近命令 trace |
| `src/usb/ffs_gadget.c` | 新 | configfs + FunctionFS + ep0 线程 |
| `src/ui/*` | 新 | Linux OLED/编码器后端 + 复用 RP2350 UI |
| `src/main.c` | 新 | CLI/生命周期 |

没有链接 TinyUSB、FatFs、Pico SDK、libgpiod。`third_party/` 不需要。

## 2. FatFs shim

`src/posix/ff.h` 定义了 `FIL/FRESULT/FSIZE_T/FA_*`，`ffshim.c` 用
`open/pread/pwrite/ftruncate/fallocate/fsync` 实现：

- `f_open`：`FA_READ/FA_WRITE/FA_CREATE_ALWAYS/FA_OPEN_ALWAYS/FA_CREATE_NEW`
  映射到 `O_RDONLY/O_RDWR/O_CREAT/O_TRUNC/O_EXCL`；
- `f_lseek`：越过 EOF 时 `ftruncate`（与 FatFs 一致）；
- `f_expand`：`fallocate`，失败退化为 `posix_fallocate`/`ftruncate`；
- `f_read/f_write` 使用绝对偏移（`pread/pwrite`），不依赖 `lseek`。

`media.c` 的路径原本是 FatFs 的 `"0:/discs/X.CDR"`；本移植直接用绝对/相对
POSIX 路径，`storage_posix.c` 负责拼 `root/discs/`。

## 3. BOT 引擎的 HAL

```c
typedef struct {
    void *ctx;
    int (*read)(void *ctx, void *buf, uint32_t len, int timeout_ms);
    int (*write)(void *ctx, const void *buf, uint32_t len, int timeout_ms);
    void (*stall)(void *ctx, bool ep_out);
    void (*clear_halt)(void *ctx, bool ep_out);
} bot_io_t;
```

`bot_handle_cbw()` 完成一个 CBW：

1. 校验签名/ LUN / CDB 长度；
2. 写命令：校验 `scsi_write_params()`，按 64 KiB 块读 OUT 数据，
   `media_write_raw_blocks()`；
3. 读命令：`scsi_read_params()`，按 64 KiB 块 `media_read_blocks()` 后写 IN；
4. 其它 OUT 带数据：读进 8 KiB 命令缓冲，调 `scsi_out_command()`；
5. 其它 IN/无数据：`scsi_in_command()`，处理后写 CSW。

设计上刻意保持和 `usb_cdrw.c` 相同的语义：

- 失败时 `residue = total - done`；
- 数据方向不符/长度不符 → drain 后 CSW failed；
- 无效 CBW → 记录并返回 `-2`；
- 不支持的命令由 `scsi.c` 返回 CHECK CONDITION。

`tests/test_bot.c` 用内存 pipe 驱动同一个引擎，不需要 USB 硬件。

## 4. FunctionFS 后端

### 4.1 描述符

```c
struct usb_interface_descriptor intf = {
    .bInterfaceClass = 0x08,   // Mass Storage
    .bInterfaceSubClass = 0x06,// SCSI transparent
    .bInterfaceProtocol = 0x50,// Bulk-Only
    .bNumEndpoints = 2,
};
// ep 0x01 OUT, 0x81 IN；FS 64B，HS 512B
```

FunctionFS 只提供 **接口 + 端点** 描述符；设备/配置描述符由 configfs
(`idVendor/idProduct/bcdUSB/bMaxPower`) 生成。

### 4.2 挂载与绑定顺序

```
mkdir configfs gadget/function/config + symlink
mount -t functionfs <instance> /dev/ffs-<instance>
open ep0
write descriptors (v2, FS+HS)
write strings (16 + langid + NUL-terminated ASCII, 精确长度)
open ep1 (OUT), ep2 (IN)
echo <udc> > UDC
```

顺序不能反：FunctionFS 只有在 descriptors + strings 都写入后才进入 ACTIVE 并
创建 ep 文件；UDC 绑定会立刻触发枚举。

### 4.3 ep0 线程

`src/usb/ffs_gadget.c::ep0_thread_fn` 独占 ep0：

- `poll(ep0, 200ms)` → `read()` 事件数组；
- `FUNCTIONFS_SETUP`：
  - class `GET_MAX_LUN(0xFE)` → 写 1 字节 0；
  - class `0xFF`（BOT reset）→ 置 `reset_requested`，写 0 字节完成状态阶段；
  - standard `CLEAR_FEATURE(ENDPOINT_HALT)` → `ioctl(FUNCTIONFS_CLEAR_HALT)`，
    置 reset；
  - 其它：有数据就回 0 长度，避免主机超时。
- 主线程在每次 bulk 读写前检查 `reset_requested`，调用 `bot_reset()`。

FunctionFS 没有用户态 bulk-stall ioctl，所以 `bot.c` 的 `stall` 只记录日志，
依赖主机的 BOT reset 重新同步。

## 5. 介质 profile（CD-RW / DVD-RW / BD-RE）

### 5.1 数据模型

```c
typedef enum {
    MEDIA_KIND_ISO, MEDIA_KIND_CDRW, MEDIA_KIND_DVDRW, MEDIA_KIND_BDRE,
} media_kind_t;

typedef struct {
    ...
    media_kind_t kind;
    uint16_t profile;   /* 0x0008 / 0x000A / 0x0010 / 0x0013 / 0x0040 / 0x0043 */
} media_status_t;
```

- 扩展名决定初始 profile：`.cdr/.dvd/.bd`；`.meta` v2 的 `profile` 字段优先。
- v1 `.meta` 没有 profile，按 CD-RW 处理，向后兼容。
- DVD/BD 容量由 profile 决定（4.7 GB / 25 GB），镜像用 `ftruncate` 建稀疏文件；
  首尾各 1 MiB 仍会清零，便于快速擦除。
- `media_format()` 对 CD-RW 仍留 blank；对 DVD/BD 置为 **appendable**
  （`blank=false, session_open=true, data_lba=0`），这是 growisofs 的硬要求。

### 5.2 SCSI 侧要补什么

| 命令 | 关键实现 |
|---|---|
| GET CONFIGURATION `46h` | profile list（CD-ROM/CD-RW/DVD-ROM/DVD-RW/BD-ROM/BD-RE）+ 各 profile 的 feature；BD 需要 Page 2A 的 DVD read 位 |
| READ FORMAT CAPACITIES `23h` | DVD-RW: `00/00/15/10/13`；BD-RE: `00/30/31`；descriptor 0 低 2 位表示 formatted |
| READ TRACK INFORMATION `52h` | DVD-RW 的 `track[6]` bit4=fixed packet，`track+20`=16 |
| FORMAT UNIT `04h` | 忽略参数，DVD/BD 变 appendable；CD 仍 blank |
| READ DISC STRUCTURE `ADh` | DVD format 0: book type `30h`；BD format 0: `DI` + Media ID；`FFh`: 结构列表 |
| GET PERFORMANCE `ACh` | type 3 与 type 4 的 descriptor 布局不同；type 4 的 `+4/+12` 是写速度，`+8` 是 end LBA |
| MODE SENSE `5Ah` page 2A | DVD/BD 至少要有 DVD-ROM read 位（bit 3），否则 mediainfo 判成非 DVD |

细节和主机工具的实测要求见 [research.md](research.md#8-dvd-rw--bd-re-扩展v21)。

### 5.3 CLI

```bash
# 新建指定 profile 的空白盘
pi3b-cdrw-emu --profile dvd-rw --new --no-usb   # -> discs/DISC01.DVD
pi3b-cdrw-emu --profile bd-re  --new --no-usb   # -> discs/DISC01.BD

# 插入时按扩展名自动识别
pi3b-cdrw-emu --image ./discs/DISC01.BD
```

`--list` 会显示每个镜像的 profile；`.meta` v2 写入 `profile`。

## 6. 本地 UI（OLED / EC11）

### 6.1 文件

| 文件 | 作用 |
|---|---|
| `src/ui/oled_linux.c` | SSD1306 `i2c-dev` 后端；API 与固件 `oled.c` 相同；`--oled-mock` 输出 PGM |
| `src/ui/encoder_linux.c` | EC11 正交解码 + 按键；GPIO v2 UAPI，不依赖 libgpiod |
| `src/ui/ui.c` | 固件 UI 的 Linux 适配：profile 感知、新建 CD/DVD/BD、擦除、调试页 |
| `src/ui/usb_status.c` | 把 gadget 状态和 BOT trace 暴露给 UI |
| `src/ui/font5x7.c` | 原 5x7 字体 |

### 6.2 UI 线程与媒体锁

FunctionFS 的 epfile 有个坑：端点存在时 `poll()` 总是返回就绪，随后阻塞
`read()` 会一直等到下一个 CBW。所以不能靠“50 ms poll 超时”在 BOT 线程里
跑 UI（实测 UI 会停在启动画面）。现在 UI 有独立线程：

```c
static void *ui_thread_fn(void *arg)
{
    while (!g_stop) {
        ui_task();
        usleep(50000);
    }
    return NULL;
}
```

UI 和 BOT 都会碰 media/SCSI，用一个递归互斥锁串行化：

- `scsi_in_command()` / `scsi_out_command()` 的 public wrapper 持锁执行整条命令；
- UI 的载入/弹出/新建/擦除/删除持锁调用 `media_*`/`storage_delete_disc`，
  状态快照也持锁复制；
- OLED 绘制/flush 不持锁，避免拖慢 USB；
- BOT 的 CBW read 仍是阻塞的，不为 UI 改动数据路径。

这样 UI 每 50 ms 刷新一次；主机连续读写时 UI 只在快照/操作时短暂等待，
不会和 bulk 数据抢 CPU。`ui_task()` 开头复制一份 `media_status_t`，绘制函数
只读这份快照。

### 6.3 OLED

- 打开 `/dev/i2c-N`，`I2C_SLAVE` 后用 SMBus quick 探测 0x3C/0x3D；
- 命令写 `0x00 + cmd`，数据写 `0x40 + 32B`，与固件一致；
- 初始化失败/写失败只把 `oled_present` 置 false，USB 光驱继续跑；
- `--oled-mock PATH` 不走 I2C，每次 flush 写 128x64 P5 PGM，
  `tools/pgm2png.py` 转 PNG，用于无面板时验证布局。

### 6.4 编码器

- 用 `<linux/gpio.h>` 的 `GPIO_V2_GET_LINE_IOCTL` 申请 3 条输入线，
  `GPIO_V2_LINE_FLAG_BIAS_PULL_UP` 打开内部上拉；
- **独立线程每 2 ms 采样** A/B，解码后的 delta/press/long 放进带 mutex 的
  累计事件；UI 线程 50 ms 只负责取走。之前 UI 线程 50 ms 才读一次，会漏掉
  一个 detent 的 4 个跳变，表现为“按键灵、旋转几乎不动”；
- 按键 20 ms 消抖；**短按在松手时上报**，按住 800 ms 上报长按（这样按住
  不会先触发菜单项）；每格跳变数默认 4，可用 `--encoder-steps 2` 适配
  2 跳变/格的 EC11；
- 默认 **不启用**，必须 `--encoder`，避免没接线时浮空线产生假按键；
- 默认 GPIO 118/128/129（排针 11/13/18），可 `--encoder-a/b/btn` 覆盖；
- 删除镜像由 `storage_delete_disc()` 完成：校验路径在 root 下、删除镜像和
  `.meta`、清理指向它的 `.last_disc`；当前加载的盘由 UI 阻止删除。

### 6.5 状态与调试页

`ffs_gadget_bound()/configured()` 读 `volatile bool`，UI 显示 `USB HS`/`USB --`。
`bot.c` 维护 8 条最近命令的 ring（opcode、方向、状态、LBA、块数、sense），
SCSI debug 页直接读它；ring 由一把小 mutex 保护，UI 和 BOT 可以并发访问。

### 6.6 测试

`make test` 覆盖 SCSI/media/BOT；UI 用 mock 帧验证：

```bash
sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.BD \
     --no-usb --oled-mock /tmp/oled.pgm
python3 tools/pgm2png.py /tmp/oled.pgm /tmp/oled.png
```

真实 OLED/编码器未接线时，`i2cdetect -y 2` 为空，daemon 自动退回 headless。

## 7. 构建开关

```bash
make                 # build/pi3b-cdrw-emu
make test            # 3 组测试
```

`Makefile` 里的身份宏：

```
-DCDRW_INQUIRY_VENDOR='"OrangePi"'
-DCDRW_INQUIRY_PRODUCT='"CD-RW USB 2.0"'
-DCDRW_INQUIRY_T10ID='"ORANGEPI_CDRW_01"'
```

SCSI INQUIRY 的 Vendor/Product 固定 8/16 字节，内部用空格补齐；改宏即可换身份。
USB VID/PID 在 `src/usb/ffs_gadget.c` 的 `USB_VID/USB_PID`。

## 8. 扩展点

- **换传输**：实现一个新的 `bot_io_t`，复用 `bot.c`。例如 USB/IP、gadgetfs、
  或把 CBW 从 Unix socket 收进来做协议测试。
- **换存储**：实现 `storage.h` 的 POSIX 版本即可；`media.c` 只依赖
  `f_open/f_read/...` 和 `storage_*` 少数函数。
- **控制接口**：在 `main.c` 里加 Unix socket，调用 `media_load()` /
  `media_eject()` / `media_new_blank()`，并调用
  `scsi_notify_media_change()` 让主机重新读 TOC。
- **上报速度**：`scsi.c` 的 `scsi_init()` 把 4x 改成 32x/48x，并同步
  `GET PERFORMANCE`、Mode Page 2A、ATIP 参考速度的硬编码。
- **UI**：`/dev/i2c-*` + SSD1306、libgpiod + EC11，逻辑可参考原
  `oled.c/encoder.c/ui.c`。

## 9. 测试

```bash
make test
```

- `test_scsi`：原固件的 SCSI 响应测试（INQUIRY/容量/TOC/Disc Info/…）。
- `test_media`：CD-RW 写读/meta/CLOSE/BLANK；DVD-RW 与 BD-RE 的
  `media_create_blank_profile`、`media_format`（appendable）、写读、profile
  持久化、GET CONFIGURATION 与 READ FORMAT CAPACITIES descriptor 类型；
  加载 demo.iso、INQUIRY/READ CAPACITY。
- `test_bot`：内存 pipe 上的 INQUIRY、READ CAPACITY、READ(10) 读 ISO、
  WRITE(10) 写 CDR、无效 CBW。

实机验证步骤见 [verification.md](verification.md)。

## 10. 已知代码级限制

- 单 LUN（`GET_MAX_LUN` 恒 0）；
- DVD-RW 用 `growisofs -Z` 后没有 finalize，Disc Status 为 appendable；
- DVD 的 legacy lead-out 字段展示为 0（不影响数据）；
- WRITE 缓冲 64 KiB，源扇区 2352 时按 `(65536/2352)*2352` 分块；
- `scsi_in_command` 的响应缓冲 64 KiB，足够现有命令；
- 控制请求只处理 GET_MAX_LUN / BOT reset / CLEAR_HALT，其它返回 0 长度；
- 退出时偶发 dwc3 ep0 警告，不影响功能。
