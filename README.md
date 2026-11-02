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
- **学习**：`*` 进菜单选 `LEARN TARGET`，或长按 `#`，5 秒内选当前帧数/信号综合最高的未知 STA
- **候选**：短按 `>` / `<`，在最近 60 秒出现过的 STA 里前后切换

`*` 打开菜单，`>` / `<` 移动，`#` 确认，`*` 返回。菜单项：
`LEARN TARGET`、`CLEAR TARGET`、`PEAK CLEAR`、`CHAN`、`SSID`、`INFO`、`REBOOT`。

学习模式会在屏幕上显示倒计时和当前最佳候选。
候选目标切换后会重新开始统计。

### 4. 找设备

- 默认保持 `AUTO` 巡航：1–13 跳频，边跳边收集 STA，不自动锁信道
- 用 `>` / `<` 选中候选后仍然巡航；短按 `#` 才锁定到目标最近出现的信道
- 再短按一次 `#` 回到 `AUTO` 巡航，继续收集其他 STA
- 长按 `>` / `<` 进入手动信道，每按一次 +1 / -1
- RSSI 中位数、峰值、帧率、最近出现时间、BSSID/SSID/厂商都在屏上
- 信号强时数字和进度条偏绿，弱时偏黄/橙/红

### 5. 按键

| 键 | 丝印 | 短按 | 长按 |
|---|---|---|---|
| K1 | `>` | 下一个候选目标 | 下一信道（手动锁定） |
| K2 | `<` | 上一个候选目标 | 上一信道（手动锁定） |
| K3 | `#` | 有目标：锁定 / 解除锁定信道；无目标：学习 | 重新学习目标（5 秒） |
| K4 | `*` | 打开菜单 / 返回上一页 | 清除目标 MAC |

菜单内：`>` / `<` 上下移动，`#` 确认，`*` 返回 FINDER。

- `#` 在没有目标时直接进入 5 秒学习；有目标时锁定/解除锁定信道
- 选择候选只改变目标，不会锁信道；只有按 `#` 才锁定，方便先收集再挑目标
- `*` 在 FINDER 页打开菜单；在菜单 / INFO / SSID 页返回
- 长按 `*` 在 FINDER 页清除目标；长按 `#` 始终重新学习
- 独立固件没有“退出该模式”这个上层页面，重启放在菜单 `REBOOT`

### 6. 串口命令

USB 串口 115200：

```text
mac AA:BB:CC:DD:EE:FF   设置目标
clear                   清除目标
learn                   开始 5 秒学习
next                    选择下一个候选
prev                    选择上一个候选
list                    列出候选 STA，带 AP BSSID / SSID / probe SSID
stas                    按 AP 分组列出 STA（含隐藏 AP、AP 报告数、已见 MAC）
lock                    锁定 / 解除锁定信道
chan                    手动下一信道
chprev                  手动上一信道
peak                    峰值清零
list                    列出最近 60 秒候选
bss                     列出最近 BSSID / SSID / BSS Load
clock                   打印 WNM 时间 / 时区
wnm                     打印 IE 67/69/98 计数
tz CST-8                手动设置 POSIX 时区（兜底）
timemode auto|direct|standard  选择 Time Value 解释方式
timeclear               清除持久化 UTC/TSF 锚点并重新等待时间
status                  打印当前状态
reboot                  重启
help                    帮助
```

---

## 屏幕布局

```text
┌────────────────────────┐
│2026-11-02  12:43:02   │
├────────────────────────┤
│ A4:C1:38:xx:xx:xx LOCK │
│ Espressif        CH 6  │
│ RSSI                   │
│      -52 dBm      HOT  │
│ ███████████████░░  78% │
│ PEAK -41        FPS 12 │
│ LAST 0.3s        SEEN  │
│ BSSID A4:C1:38:...     │
│ SSID [UTF8]  * MENU    │
│ CH 6 LOCK  CAND 4  RF42│
├────────────────────────┤
│ > NEXT < PREV # LOCK   │
│ HOLD >/< CH  HOLD # LEARN│
└────────────────────────┘
```

- 标题和统计用 3x5 小字，RSSI 用 8x8 大字
- 右上角用 3x5 放大 2 倍显示完整日期和时间：`2026-11-02  12:43:02`
- 不显示 `CST` / `UTC` 等时区标签；日期和时间直接占满顶栏
- 没有 WNM 时间时显示 `UP HH:MM:SS`（本次开机后的服务时钟，本机不联网）
- 另有 2x5 数字小字库，用于 `PEAK / FPS / CAND` 这类次要数字，不挤占主 RSSI 区域
- 顶栏不再显示 `2.4G`（本型号只有 2.4G Wi-Fi）
- 深绿 PHO 主题，暗背景，强信号才提亮
- 底部 `RF` 是 NRF 在当前 Wi-Fi 频道中心 1MHz 的 RPD 占用，**不是 dBm**
- 非 ASCII SSID 在 FINDER 页显示 `SSID [UTF8]` / `SSID [GBK]`，进 `* -> SSID` 页看完整中文
- SSID 页显示 BSSID、信道、AP BSS Load 和 5 行中文；两种编码都能解时 `#` 切换
- 隐藏 SSID 显示 `SSID HIDDEN / AP ADVERTISES NO SSID`，BSSID 保留，可用 `stas` 按 AP 分组
- 完全听不到 AP Beacon 才显示 `NO SSID / AP BEACON NOT HEARD`

---

## WNM 时间同步

固件会被动监听 Beacon / Probe Response 里的：

- **IE 69 Time Advertisement**（注意：标准 ID 是 **69**，67 是 BSS AVB Admission Capacity）
- **IE 98 Time Zone**（ASCII POSIX TZ 字符串，例如 `CST-8`、`CET-1CEST,M3.5.0,M10.5.0/3`）

标准语义里，`time_advertisement=2` 的 10 字节 Time Value 是「TSF 为 0 时的 UTC
日历时间」，加上同一个 Beacon 的 8 字节 TSF 就得到当前 UTC。

但 **OpenWrt / hostapd 2.11 的实现有已知偏差**：它只在第一次构建时把「当前 UTC」
填进去，并且源码里留着 `FIX: need to calculate this based on the current TSF value`
注释；AP 运行久了这个值就是旧的。固件因此提供两种解释：

- `direct`（OpenWrt/hostapd 默认）：Time Value 当锚点，本机单调时钟往前走，不再加 TSF
- `tsf`（标准 802.11）：Time Value + TSF

默认 `auto`：收集多个 BSS 的样本，比较两种解释的离散度，自动选择。
也可以用 `timemode direct|standard` 手动指定。

时区元素按 POSIX TZ 解析，支持标准偏移和 `M月.周.星期` 形式的 DST 规则。

- 同步后右上角显示完整日期和时间 `2026-11-02 09:42:31`，不带时区文字
- Info 页仍显示时区名，便于诊断
- 收到更新的 Time Value 时立即重锚（3 秒死区，避免多个 BSSID 之间抖动），不用等来源超时
- 已经同步后，其他 BSS 的旧广告不会把时钟拖回过去
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

> OpenWrt/hostapd 的 Time Value 不会周期性刷新。AP 运行很久后，广告时间会落后于真实
> 时间；重启 hostapd/Wi-Fi 可以刷新一次。OpenWrt 上可以只重置 2.4G：
> `wifi up radio1`。finder 会检测到新的 Time Value 并重新锚定。
> 这是 AP 侧实现问题，不是解析器的问题。

如果愿意在 AP 侧修一下，`docs/hostapd-time-refresh.patch` 让 hostapd 在每次发
Beacon/Probe Response 时重新生成 Time Advertisement，而不是只生成一次。补丁
针对 hostapd 2.11/2.12，改完后重启 hostapd/Wi-Fi 即可。它修正的是 OpenWrt 的
"direct UTC" 行为，不改变 802.11 帧格式。

### 持久化 UTC 锚点（跨 finder 重启）

hostapd 的 Time Value 长期不变，但 Beacon 里的 8 字节 TSF 每帧都在走。finder 把
`(BSSID, TSF, UTC, ad_base, TZ)` 存进 NVS：

- 首次同步：用 Time Value 得到 UTC，同时记录同一帧的 TSF
- 同一 BSS 再次出现：`UTC_now = stored_utc + (tsf_now - stored_tsf)`
- finder 重启/断电：只要 AP 没重启（TSF 没有回退），仍按 TSF 差恢复时间
- AP 重启导致 TSF 回退：丢弃旧锚点，重新同步
- 已经同步后，其他 AP 的旧广告不会把时钟拖回去；只有更新的广告或原 BSS 才会重锚
- `timeclear` 清除锚点并重新等待新广告；`clock` 输出 `mode=persist` 表示正在用持久化锚点

NVS 只在目标 MAC 变化、锚点来源变化、时区变化或 Time Value 真正更新时写入，不做
周期性擦写。

## 中文 SSID（UTF-8 / GBK）

SSID 是任意字节串，现场可能同时存在 UTF-8 和 GBK 两种编码。固件两者都支持：

- 先校验 UTF-8；不合法再按 GBK 查表（GBK→Unicode 表由 Python codec 生成）
- 两种都“合法”时做脚本合理性检查（CJK / 拉丁 / 标点 / emoji），避免把 GBK
  的两字节序列误判成希伯来文/希腊文
- 非 ASCII SSID 用内置 **Noto Sans CJK SC 12x12** 点阵，覆盖
  U+4E00–U+9FFF、U+3000–U+303F、U+FF00–U+FFEF
- FINDER 页显示 `SSID [UTF8]` / `SSID [GBK]`；`* -> SSID` 打开 SSID 页看完整中文
- 如果两种解释都合法，SSID 页按 `#` 在 UTF8 / GBK 之间切换
- 点阵 + GBK 表约 0.5MB，2MB factory 分区仍有约 28% 余量
- 生成脚本 `tools/gen-cjk-font.py`；字体许可见 `docs/OFL.txt`、`docs/FONT-NOTICE.md`

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
- 每个候选 STA 记录它 probe 的 SSID；`list` 显示 `AP` 和 `SSID`，`stas` 按 BSSID 分组
- `stas` 从 BSS 表出发，隐藏 AP 也会列出：`AP <BSSID> SSID <hidden> AP 5 STA seen 2 <MAC...>`
- 固件只收不发：不主动 scan、不发 probe request；需要主动探测时必须显式进入 probe 模式（当前默认完全被动）
- 隐藏 SSID 的 Beacon 也会进 BSS 表并标记 `<hidden>`，按 BSSID 仍可分组；
  只有完全听不到 AP Beacon 时才显示 `AP BEACON NOT HEARD`
- 5G-only AP/STA 这套 2.4G 硬件看不到；STA 的 probe request 仍可能带出它要找的 SSID
- 解析 AP 的 BSS Load（IE 11）：站点数、信道利用率、AAC；Info 页显示为 `AP 38%/2`
- BSS Load 是 AP 聚合数据，不能替代目标 RSSI，只用于判断信道拥挤/辅助诊断
- 服务时钟优先使用 WNM `Time Advertisement`（IE 69）+ 时区（IE 98）
- UTC/TSF 锚点写入 NVS；finder 重启后用 TSF 差恢复，已同步的时钟不被旧广告拖回
- 没有 WNM 数据时回退到 `esp_timer` 开机时间 `UP HH:MM`
- 中文 SSID：UTF-8/GBK 双解码 + 12x12 CJK 点阵；SSID 页可手动切换编码
- UI 状态机：FINDER / MENU / INFO / SSID；菜单承载学习、清除、峰值、信道、SSID、重启
- OUI 厂商表由 `tools/gen-oui.py` 从 IEEE 公开 OUI 列表生成
- NRF 只读 `RPD`，跟随当前 Wi-Fi 频道中心，8 次采样得到 0–100%

### 目录

```text
main/
  main.c       按键、串口、主循环
  finder.c     Wi-Fi 嗅探、目标/候选/BSS 表、NVS、信道
  st7735.c     160x128 UI（FINDER / MENU / INFO / SSID）
  cjk_font.c   UTF-8/GBK 解码、SSID 编码判定、CJK 点阵查找
  cjk_font_data.h  12x12 CJK 点阵（生成文件）
  gbk_table.h  GBK→Unicode 表（生成文件）
  nrf24.c      NRF24L01 驱动
  nrf_aux.c    可选 RPD 旁路
  oui.c        OUI 查询
  oui_table.h  IEEE OUI 子集（生成文件）
components/
  esp_lcd_st7735/
tools/
  gen-cjk-font.py   从 Noto Sans CJK SC 生成点阵/GBK 表
  gen-oui.py        生成 OUI 表
docs/
  DESIGN.md
  FONT-NOTICE.md
  OFL.txt
  REQ-iot-finder.md
  hostapd-time-refresh.patch
partitions.csv
```

---

## 能力边界

| 能做 | 不能做 |
|---|---|
| 每帧 RSSI、MAC、信道、帧类型 | 5GHz / 5.8GHz |
| 自动锁目标信道 | 方向 / 角度 |
| BSSID / SSID / OUI 厂商 | 精确距离 |
| AP 自报 BSS Load（站点数 / 利用率） | 目标设备自身负载 |
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
`main/cjk_font_data.h` 由 **Noto Sans CJK SC** 生成，按 SIL Open Font License 1.1
分发；详见 `docs/OFL.txt` 和 `docs/FONT-NOTICE.md`。
