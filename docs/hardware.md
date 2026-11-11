# 硬件：端口、接线与 VBUS

## 1. 哪个口能做 Device

Orange Pi 3B 的 USB 口（从板边看）：

```
┌──────────────────────────────────────────────┐
│  Type-C 电源   2× USB2.0 HOST   USB3.0 HOST  │
│                                  USB2.0 OTG  │  ← 蓝色口正下方
└──────────────────────────────────────────────┘
```

- **蓝色 USB 3.0 口正下方的 USB 2.0 口** = RK3566 的 USB2 OTG，
  官方手册称为 **USB2.0 burning port**，支持 Device / HOST。
- 另外两个 USB 2.0 口和蓝色 USB 3.0 口都是纯 host，不能做 Device。
- Type-C 只负责 5V/3A 供电，不是数据口。

对应控制器（现场 DTB）：

| 控制器 | 节点 | 对应口 | 默认模式 |
|---|---|---|---|
| DWC3 USB2 OTG | `usb@fcc00000` | 蓝口下方 USB2.0 | host（需改 peripheral） |
| DWC3 USB3 | `usb@fd000000` | 蓝色 USB3.0 | host |
| EHCI/OHCI ×2 | `usb@fd800000/fd880000` | 2× USB2.0 | host |

## 2. 自环演示接线（推荐先做这个）

```
Orange Pi 3B 蓝色 USB3.0 口  ── USB-A 公对公线 ──  蓝口下方 USB2.0 口
```

- 两个口的 VBUS 都来自板载 `vcc5v0_sys`，同电位，自环安全。
- 板子继续用 Type-C 5V/3A 供电。
- 这样本板自己的 xHCI 会枚举自己的 gadget，得到 `/dev/sr0`，无需第二台电脑。

## 3. 接外部 PC

```
PC USB-A 口  ── USB-A 公对公线 ──  Orange Pi 3B 蓝口下方 USB2.0 口
Orange Pi 3B Type-C  ── 5V/3A 电源
```

**必须先关闭板子这一侧的 OTG VBUS**，否则板子和 PC 会同时往同一根 VBUS 灌 5V：

```bash
sudo tools/otg-vbus-off.sh
```

原理：

- 厂商 DTB 的 `vcc5v0_otg`（GPIO0_A5，固定调压器）默认 `regulator-always-on`；
- 脚本 unbind `reg-fixed-voltage` 驱动，把 GPIO0_A5 拉低，负载开关断开；
- 端口的 VBUS 只剩 PC 提供的 5V，PHY 能检测到 session valid；
- 板子仍由 Type-C 供电，所以不会掉电。

恢复：

```bash
sudo tools/otg-vbus-on.sh
```

> 已验证：在本板自环、GPIO0_A5=0 的情况下，设备仍正常枚举、`/dev/sr0`
> 仍能读出 ISO 的 PVD。说明"PC 供 VBUS、板子 Type-C 供电"的电气路径成立。
> 尚未用真实 PC 做整机验证。

### 更彻底：启动期 DT overlay

运行时 unbind 调压器是临时方案。生产环境建议在启动时就不启用它：

```dts
/dts-v1/;
/plugin/;

&usb_host0_xhci {
    dr_mode = "peripheral";
};

&vcc5v0_otg {
    /delete-property/ regulator-always-on;
};
```

把它编译成 dtbo，交给 U-Boot 的 `overlays=`（本板 `orangepiEnv.txt` 使用
`overlay_prefix=rk356x`），或者用 `fdtoverlay` 合并进 DTB 后备份原文件。
这样 `vcc5v0_otg` 在 probe 时不会被使能，GPIO0_A5 保持默认低电平，就不再需要
`otg-vbus-off.sh`。**修改启动 DTB 有变砖风险，务必保留原 DTB 和串口/备份启动
介质；本仓库默认不自动改启动文件。**

## 4. 运行时切换到 peripheral

```bash
sudo tools/enable-otg.sh
ls -l /sys/class/udc/
# fcc00000.usb -> .../udc/fcc00000.usb
```

脚本做三件事：

1. 确保 configfs 已挂载；
2. 把 `otg-peripheral.dtbo` 写进
   `/sys/kernel/config/device-tree/overlays/otg-peripheral/`；
3. unbind/rebind `fcc00000.usb` 的 `dwc3` 驱动（dr_mode 在 probe 时读取）。

幂等：UDC 已存在时直接返回。

## 5. 启动顺序

`deploy/install.sh` 安装的 systemd 服务按下面的顺序启动：

```
ExecStartPre=otg-vbus-off.sh       # 外部 PC：先关板子 OTG VBUS
ExecStartPre=enable-otg.sh         # overlay + dwc3 rebind，创建 UDC
ExecStartPre=-enable-i2c2.sh       # 可选：40-pin I2C2
ExecStart=build/pi3b-cdrw-emu ...  # gadget + FunctionFS + BOT
```

`pi3b-cdrw-emu` 自己会：

1. 自动 `modprobe libcomposite usb_f_fs`（重启后模块未加载也能起来）；
2. 在 `/sys/kernel/config/usb_gadget/cdrw0` 建 gadget、function、config；
3. `mount -t functionfs cdrw0 /dev/ffs-cdrw0`；
4. 写描述符、strings，打开 ep1/ep2；
5. 把 `fcc00000.usb` 写进 `UDC`；
6. 启动 ep0 控制线程、UI 线程，进入 BOT 循环。

服务默认 `--image /var/lib/pi3b-cdrw/discs/DISC01.BD --encoder`，并已
`systemctl enable`。自环模式见 README。

## 6. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `/sys/class/udc` 为空 | 未应用 overlay 或未 rebind；跑 `sudo tools/enable-otg.sh` |
| `gadget setup failed` | 不是 root；configfs 未挂载；gadget 名/挂载点被占用（先 `pkill -x pi3b-cdrw-emu`，或 `--gadget cdrw1`） |
| `mount functionfs: Device or resource busy` | 上次异常退出残留；`umount /dev/ffs-cdrw` 后重试 |
| `configfs: cannot link function` | 符号链接 target 必须是绝对路径（本实现已处理） |
| 主机看不到设备 | 线插错口；OTG 口 VBUS 被关但主机没供 VBUS；换一根**能传数据**的 A-A 线 |
| `dwc3 ... request was not queued to ep0out` | 退出时的 benign warning，可忽略 |
| USB/IP 后系统卡住 | `usbip-vudc` teardown oops；重启清理，不要用于生产 |
| 读第一张 ISO 的 LBA16 数据错位 | 旧版 `media.c` seek 缓存 bug；本仓库已修 |
| 外部 PC 口发热/供电异常 | 未执行 `otg-vbus-off.sh`，两端 VBUS 顶在一起 |

## 7. 电源估算

- 板子本体（WiFi + 4×A55）约 3–6 W；
- USB gadget 由主机供 VBUS（外部模式）或本板供（自环），不驱动电机/激光；
- 建议 Type-C 5V/3A，和官方要求一致。

## 8. 40-pin 本地 UI：OLED + 编码器

目标：不接 SSH，也能用面板选盘、弹出/加载、新建空白盘、擦除。

### 8.1 I2C2（OLED）

排针物理 3/5 是 `SDA.2/SCL.2`，对应 RK3566 的 **I2C2**（`i2c@fe5b0000`），
pinctrl 默认就是 `i2c2m1-xfer`：GPIO4_B4 = 140（SDA）、GPIO4_B5 = 141（SCL）。
厂商 DTB 把控制器设成 `status = "disabled"`，所以只有 `/dev/i2c-0` 和 HDMI 的
`/dev/i2c-6`。

`tools/i2c2.dts` 只做两件事：

```dts
&i2c2 {
    status = "okay";
    clock-frequency = <400000>;
};
```

`tools/enable-i2c2.sh` 通过 configfs 热应用，**不需要重启**；实测
`/dev/i2c-2` 立即出现。开机自启由 systemd 的 `ExecStartPre` 调用。

> 如果启动时报 `ffs: cannot create /sys/kernel/config/usb_gadget/...:
> Operation not permitted`，那是 `libcomposite`/`usb_f_fs` 模块没加载
> （重启后常见）。v2.2 的 daemon 会自动 `modprobe`；也可手动：
> `sudo modprobe libcomposite usb_f_fs`。

OLED 接线：

| OLED | 排针 |
|---|---|
| VCC | 1 或 17（3.3 V，不要接 5 V） |
| GND | 6 / 9 / 14 / 20 |
| SDA | 3 |
| SCL | 5 |

地址一般是 `0x3C`（少数模块 `0x3D`，驱动会自动尝试）。验证：

```bash
sudo tools/enable-i2c2.sh
sudo i2cdetect -y 2        # 期望 3c
```

### 8.2 EC11 / KY-040（编码器）

| 编码器 | 排针 | GPIO | gpiochip |
|---|---|---|---|
| A | 11 | 118 | gpiochip3 line 22 |
| B | 13 | 128 | gpiochip4 line 0 |
| SW | 18 | 129 | gpiochip4 line 1 |
| + | 17 | 3.3 V | |
| GND | 20 | GND | |

驱动用 **GPIO v2 UAPI**（`/dev/gpiochipN`）直接申请三条输入线，并打开内部
上拉，不依赖 libgpiod：

```c
req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
                   GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
```

KY-040 自带 10 k 上拉；裸 EC11 也能用内部上拉。按键是低有效（按下接地）。
方向反了就对调 A/B，或运行时用 `--encoder-a/--encoder-b` 换 GPIO。

> 编码器**默认不启用**。没接线时浮空线可能产生假脉冲，所以只有显式
> `--encoder` 才会申请这三个 GPIO。OLED 可以单独工作。

驱动用独立线程每 **2 ms** 采样 A/B，UI 线程只消费累计事件；每格默认 4 个
正交跳变（KY-040 常见值）。如果转很多格才动一次，用 `--encoder-steps 2`；
如果一格跳两下，用 4。

排查接线时加 `--encoder-debug`，正常应看到
`state 0 -> 1 -> 3 -> 2 -> 0` 的完整序列；只在 0/1 或 2/3 之间跳，说明 A/B
有一根没接通，或接到了同一个 GPIO。

实机已识别：`oled: SSD1306 at i2c-2 0x3C`、`encoder: EC11 ready on GPIO
118/128/129`。旋转/按键事件会打印 `UI: input d=... p=... l=... view=...`，
便于确认方向和消抖。

### 8.3 无面板时的验证

```bash
sudo ./build/pi3b-cdrw-emu --root . --image ./discs/DISC01.DVD \
     --no-usb --oled-mock /tmp/oled.pgm
python3 tools/pgm2png.py /tmp/oled.pgm /tmp/oled.png
```

`--oled-mock` 走同一套绘图代码，只是把 128x64 帧存成 PGM；用于确认布局，
不依赖真实 I2C 面板。
