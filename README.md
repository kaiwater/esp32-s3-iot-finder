# ESP32-S3 IoT Finder

> 2.4GHz 智能设备定位器：锁定一个 MAC，用 **Wi-Fi 每帧 RSSI** 做热冷逼近。
> ESP32-S3 + ST7735 160x128 + 4 键，可选 NRF24L01 做同频道 RPD 辅助。

这是一个独立工程，不依赖 `esp32-s3-nrf-radar` 或 `bw16-beacon-monitor` 的构建系统。
硬件沿用同一套 ESP32-S3 手持板。

---

## 这个工具解决什么问题

一批智能插座 / IoT 设备需要复位换绑，但不知道某一只在哪个房间。
把设备切到配网/探针状态，让它持续发 probe request，然后：

1. 锁定它的 MAC
2. 看 RSSI 条和 dBm 数
3. 举着走，信号变强 = 离得更近
4. 自动锁到它出现的信道，避免 13 信道跳频导致 0.5 秒才看一次
5. 找到后回路由器换绑

**它不测距，也不测方向。** RSSI 只能做同一目标、同一姿势下的相对比较。
人体遮挡、墙体、多径会让读数上下跳 10dBm 左右，所以界面同时显示中位数、峰值和最近出现时间。

---

## 硬件

| 模块 | 引脚 |
|---|---|
| ST7735 SCK | GPIO12 |
| ST7735 MOSI | GPIO11 |
| ST7735 CS | GPIO10 |
| ST7735 DC | GPIO9 |
| ST7735 RST | GPIO8 |
| ST7735 BL | GPIO7 |
| K1 | GPIO4 |
| K2 | GPIO5 |
| K3 | GPIO6 |
| K4 | GPIO14 |
| NRF24L01 SCK | GPIO13 |
| NRF24L01 MOSI | GPIO15 |
| NRF24L01 MISO | GPIO16 |
| NRF24L01 CSN / CE | 17/38、18/39、21/40（任意一个可用即可） |

- 屏幕：ST7735 **160x128**，SPI2
- NRF：SPI3，只使用 RPD 位，**不参与 MAC 识别**
- NRF 是可选的；没有 NRF 时界面显示 `RF --`，Wi-Fi 定位照常工作

> 只支持 2.4GHz。5.8GHz 图传、5GHz Wi-Fi、大多数 FPV 模拟/数字图传这套硬件看不到。

---

## 构建

需要 ESP-IDF **v5.4.2**（或兼容的 5.x）。

```sh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash
```

- 目标 flash：16MB，DIO，80MHz
- 自定义分区：`partitions.csv`，factory app **2MB**
- 不使用 CI，不提交构建产物或预编译 bin
- 依赖全部 vendored 在 `components/`，构建时关闭组件管理器联网

---

## 使用

### 1. 拿到目标 MAC

路由器 DHCP 列表、`iw dev ... station dump`、或智能设备 App 里都能看到。
目标 MAC 只写入本机 NVS，不会上传，也不会出现在源码里。

### 2. 让目标说话

智能插座平时可能几分钟才发一次心跳。**先把它复位到配网状态**，它会持续发 probe request。
目标不说话时，这个工具只能看到 `LOST`，NRF 的 `RF` 只能说明“这个频道有能量”，不能说明“是它”。

### 3. 锁定目标

三种方式：

- **串口**：`mac AA:BB:CC:DD:EE:FF`
- **学习**：长按 K2，5 秒内选当前帧数/信号综合最高的未知 STA
- **候选**：短按 K3，在最近 60 秒出现过的 STA 里循环切换

学习模式会在屏幕上显示倒计时和当前最佳候选。
候选目标切换后会重新开始统计。

### 4. 找设备

- 首次听到目标后自动锁到目标信道（`LOCK`）
- 短按 K1 在 `AUTO`（1–13 跳频）和 `LOCK` 之间切换
- 长按 K1 进入手动信道，每按一次 +1
- RSSI 中位数、峰值、帧率、最近出现时间、BSSID/SSID/厂商都在屏上
- 信号强时数字和进度条偏绿，弱时偏黄/橙/红

### 5. 按键

| 键 | 短按 | 长按 |
|---|---|---|
| K1 | 锁定 / 解除锁定信道 | 下一信道（手动锁定） |
| K2 | 峰值清零 | 重新学习目标（5 秒） |
| K3 | 下一个候选目标 | 清除目标 MAC |
| K4 | FINDER / INFO 页切换 | 重启 |

> 独立固件没有“退出该模式”这个上层页面，所以 K4 短按改成 INFO 页；长按仍然是重启。

### 6. 串口命令

USB 串口 115200：

```text
mac AA:BB:CC:DD:EE:FF   设置目标
clear                   清除目标
learn                   开始 5 秒学习
next                    选择下一个候选
lock                    锁定 / 解除锁定信道
chan                    手动下一信道
peak                    峰值清零
list                    列出最近 60 秒候选
bss                     列出最近解析到的 BSSID / SSID
clock                   打印 WNM 时间 / 时区
wnm                     打印 IE 67/69/98 计数
tz CST-8                手动设置 POSIX 时区（兜底）
status                  打印当前状态
reboot                  重启
help                    帮助
```

---

## 屏幕布局

```text
┌────────────────────────┐
│ IOT FIND      2.4G     │
│                CST 09:42│
├────────────────────────┤
│ A4:C1:38:xx:xx:xx LOCK │
│ Espressif        CH 6  │
│ RSSI                   │
│      -52 dBm      HOT  │
│ ███████████████░░  78% │
│ PEAK -41        FPS 12 │
│ LAST 0.3s        SEEN  │
│ BSSID A4:C1:38:...     │
│ SSID  Home_2.4G        │
│ CH 6 LOCK  CAND 4  RF42│
├────────────────────────┤
│ K1 LOCK K2 LEARN K3... │
│ K3 HOLD CLEAR  K4 INFO │
└────────────────────────┘
```

- 标题和统计用 3x5 小字，RSSI 用 8x8 大字
- 右上角 `UP HH:MM` 是**本次开机后的服务时钟**（不是网络时间；本机不联网）
- 另有一套 2x5 数字小字库，用于服务时钟和 `PEAK / FPS / CAND` 这类次要数字，不挤占主 RSSI 区域
- 深绿 PHO 主题，暗背景，强信号才提亮
- 底部 `RF` 是 NRF 在当前 Wi-Fi 频道中心 1MHz 的 RPD 占用，**不是 dBm**

---

## WNM 时间同步

固件会被动监听 Beacon / Probe Response 里的：

- **IE 69 Time Advertisement**（注意：标准 ID 是 **69**，67 是 BSS AVB Admission Capacity）
- **IE 98 Time Zone**（ASCII POSIX TZ 字符串，例如 `CST-8`、`CET-1CEST,M3.5.0,M10.5.0/3`）

`time_advertisement=2` 的 10 字节 Time Value 是「TSF 为 0 时的 UTC 日历时间」，
加上同一个 Beacon 的 8 字节 TSF 就得到当前 UTC。
时区元素按 POSIX TZ 解析，支持标准偏移和 `M月.周.星期` 形式的 DST 规则。

- 同步后右上角显示 `CST 09:42`，Info 页显示完整日期、时间和时区
- Info 页底部 `SYNC xx:xx:xx` 显示时间来源 BSSID 的后 3 字节
- 没有时间元素时回退为 `UP HH:MM`（开机时长）
- 只解析 2.4GHz；5GHz 的 BSS 这套硬件收不到
- AP 的 **2.4G wifi-iface** 上要有：

```sh
option time_advertisement '2'
option time_zone 'CST-8'
```

串口 `wnm` 可以看 IE 67/69/98 的计数；`tz CST-8` 可以手动兜底。
部分 hostapd 把 Time Zone 只放进 Probe Response；如果只看到 UTC，等附近有客户端探测，
或者直接用 `tz CST-8` 设置本地时区。

## 实现要点

- `esp_wifi_set_promiscuous(true)`，过滤器 `MGMT | DATA | CTRL`
- 回调里只做地址匹配、目标统计入环形队列；解析、候选表、UI 都放主任务
- 目标匹配 `addr1/addr2/addr3`（4 地址数据帧也看 `addr4`），所以 ACK / RTS / CTS 也能贡献帧率和 RSSI
- RSSI：最近 5 帧中位数；峰值本次会话保持
- 帧率：1 秒窗口 + 4 点滑动平均
- 状态机：`SEEN < 5s` / `WEAK 5–30s` / `LOST > 30s`
- 默认 1–13 跳频，每信道 150ms；听到目标后自动锁定
- 国家码 `01` + 手动 1–13，避免 12/13 被默认禁用
- 被动学习 BSSID/SSID，不调用 `esp_wifi_scan`，不发 probe request
- 服务时钟优先使用 WNM `Time Advertisement`（IE 69）+ 时区（IE 98）
- 没有 WNM 数据时回退到 `esp_timer` 开机时间 `UP HH:MM`
- OUI 厂商表由 `tools/gen-oui.py` 从 IEEE 公开 OUI 列表生成
- NRF 只读 `RPD`，跟随当前 Wi-Fi 频道中心，8 次采样得到 0–100%

### 目录

```text
main/
  main.c       按键、串口、主循环
  finder.c     Wi-Fi 嗅探、目标/候选/BSS 表、NVS、信道
  st7735.c     160x128 UI
  nrf24.c      NRF24L01 驱动
  nrf_aux.c    可选 RPD 旁路
  oui.c        OUI 查询
  oui_table.h  IEEE OUI 子集（生成文件）
components/
  esp_lcd_st7735/
docs/
  DESIGN.md
  REQ-iot-finder.md
partitions.csv
```

---

## 能力边界

| 能做 | 不能做 |
|---|---|
| 每帧 RSSI、MAC、信道、帧类型 | 5GHz / 5.8GHz |
| 自动锁目标信道 | 方向 / 角度 |
| BSSID / SSID / OUI 厂商 | 精确距离 |
| 帧率 / 最近出现 / 峰值 | 解调私有协议 |
| NRF 同频道能量辅助 | 用 NRF 识别 MAC |
| 802.11v WNM UTC / 时区时间 | 5GHz BSS、无 WNM 的 AP |

- 单根全向天线没有方向信息，只能靠人走 + 热冷
- 不同设备发射功率不同，跨设备 RSSI 不可比
- 目标必须发帧；完全静默的设备无法定位
- 目标如果使用随机 MAC，OUI 会显示 `LOCAL ADMIN`
- 时间同步依赖 2.4G AP 广播 IE 69/98；5G-only SSID、未开启 WNM 的 AP 不会提供时间
- `RSSI` 受人体、墙体、多径影响，建议看趋势，不要看单帧

---

## License

MIT。`components/esp_lcd_st7735` 来自 Waveshare/Espressif 组件，按其原 MIT 许可使用。
