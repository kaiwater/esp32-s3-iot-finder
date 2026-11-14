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
- 最多支持 **两个 NRF 同时在线**：每个 CSN/CE 组合使用独立的 `nrf_t` 和 SPI 设备
- 两个模块 **错开启动**：找到第一个后等 600ms 再探测第二个，避免同时拉启动电流
- 只有一个在线时，每 5 秒在后台热插拔重试第二个，不影响已在线模块
- 启动会延迟 3 秒，并对 17/38、18/39、21/40 做多轮重试；SPI 1MHz
- 两个都在线时 INFO 显示 `RF C18+C21 <duty>%`，串口 `status` 显示 `csn=18+21`
- RF SCAN 会同时用两个 NRF：1–6 与 8–13 成对扫描，7 单独扫，扫完更快

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
`LEARN TARGET`、`CLEAR TARGET`、`PEAK CLEAR`、`CHAN`、`AP LIST`、`RF SCAN`、`MW 2450`、`RF SCAN PRO`、`INFO`、`REBOOT`。

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
| K4 | `*` | 打开菜单 / 返回上一页 | 切换主题（10 套配色） |

菜单内：`>` / `<` 上下移动，`#` 确认，`*` 返回 FINDER。

- `#` 在没有目标时直接进入 5 秒学习；有目标时锁定/解除锁定信道
- 选择候选只改变目标，不会锁信道；只有按 `#` 才锁定，方便先收集再挑目标
- `*` 在 FINDER 页打开菜单；在菜单 / INFO / AP LIST / AP DETAIL / STA LIST 页返回上一级
- 长按 `*` 循环切换 10 套配色；清除目标用菜单 `CLEAR TARGET`
- 长按 `#` 始终重新学习
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
ui ap|detail|stas|rf|help|color|find  调试：跳 AP LIST / AP DETAIL / STA LIST / RF SCAN / 帮助 / 色柱 / FINDER
theme                   调试：循环 10 套配色
status                  打印当前状态
reboot                  重启
btn 1..4 [long]         模拟 K1..K4 短按/长按
a/d/s/w                 直接当 K1/K2/K3/K4 短按（大写 = 长按）
keypad [on|off]         USB 口单键模式：a/d/s/w 不用回车，Esc 退出
page                    打印当前 UI 页、菜单项、主题、Wi-Fi 状态
stream [ms]             周期性输出 RF/状态行（0=关闭，默认 1000ms）
pro                     RF Scan Pro：2400-2525MHz、1MHz 步进频谱
pro off | reset | list  停止 / 清零 / 打印高能频段列表
mw                      固定 2450MHz 观察模式
mw off | reset          回到 13 信道 / 清零 MW 统计
help                    帮助
```

`stream` **默认开启（1000ms）**，每秒输出一行，和屏幕无关；`stream 0` 关闭。

间隔和格式都存在 **NVS** 里，改完不用重新编译：

```text
fmt                      查看当前 RF 行模板
fmt reset                恢复默认模板
fmt rf S=%S ch=%c pk=%p hit=%h/%t h10=%0  [rate=%r/s] win=%ws age=%as
stream 500               改间隔并保存（0=关闭）
```

模板占位符：

| 占位 | 含义 | 占位 | 含义 |
|---|---|---|---|
| `%S` | 扫描轮数 | `%0` | 最近 10 秒命中数 |
| `%c` | 显示信道 | `%r` | 命中率（一位小数） |
| `%p` | 峰值 % | `%w` | 统计窗口秒数 |
| `%h` | 命中轮数 | `%a` | 峰值距今年龄秒数 |
| `%t` | 总轮数 | `%%` | 百分号本身 |

默认输出：

```text
I (175236) stream: rf S=1859 ch=3 pk=100 hit=574/1860 h10=31  [rate=3.1/s] win=10s age=11s
```


```text
rf S=1155 ch=2 pk=100 hit=351/1155 h10=19 rate=1.9/s win=10s age=7s
```

- `rate` 就是屏幕右上角的 `/s`（最近 10 秒滚动命中率）
- 不在 RF SCAN 时输出 `find target=... rssi=... fps=...`
- `status` 里的 `usb=` 表示原生 USB 口是否被主机枚举


### 第二个 USB 口：原生 USB 双控制台

板子上的**原生 USB 口**（ESP32-S3 的 GPIO19/20，不是 UART 桥）现在也是一个控制台：

- 插到另一台 PC，系统会出现 `ttyACM*` / `COMx`（CDC，不需要关心波特率）
- 输出：所有日志和命令回复都会**同时**镜像到 UART 和原生 USB
- 输入：PC 发同样的命令行，例如 `status`、`rfdata`、`ui rf`、`theme`
- 按钮控制：`btn 3` = 短按 K3（OK），`btn 4 long` = 长按 K4（返回/切主题）
- `page` 可以确认当前页面，方便远程按按钮

**Windows（插原生 USB 口的这台）：**

- 设备管理器 → **端口 (COM 和 LPT)** 里找新出现的 `USB 串行设备 (COMx)`；
  多数 Win10/11 自带 `usbser.sys`，不用装驱动
- 端口号不确定时：

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

- 图形串口工具（PuTTY / Tera Term）：选 `COMx`，**115200 8-N-1，无流控**，
  原生 USB 是 CDC，波特率不影响，但填 115200 最省事
- 仓库自带脚本 `tools/finder-cli.ps1`，**默认自动找口并自动重连**：

```powershell
.\tools\finder-cli.ps1 -ListPorts        # 列出 COM 口、描述和 VID/PID
.\tools\finder-cli.ps1                   # 自动选口，交互模式
.\tools\finder-cli.ps1 -Port COM4        # 直接指定
.\tools\finder-cli.ps1 -Port COM4 -Stream 1000   # 连上就每秒输出 RF
.\tools\finder-cli.ps1 -Command status -Seconds 2
.\tools\finder-cli.ps1 -Command "btn 4 long"
```

- 自动选口按 **VID_303A**（ESP32-S3 原生 USB）识别，不再看名字；CH340/CP210/FTDI 这类 UART 桥会被降权
- 固件**默认每秒输出一行 `stream: rf ... rate=...`**，所以连上就能看到数据，不需要发命令
- `-Stream <ms>` 可以覆盖默认间隔，例如 `-Stream 500`
- 想停掉持续输出，在 UART 或可用的 USB 控制台发 `stream 0`
- **烧录/重启后口消失会自动等待重连**（默认最多等 30 秒，`-ReconnectSeconds 0` 表示一直等）
- 重连后如果 COM 号变了，会重新扫描，不需要改参数
- 一条命令模式在断线重连后会自动重发当前命令（最多 3 次）
- 不加 `-Command` 时是交互模式：一直显示日志，输入命令回车发送，Ctrl+C 退出；断线会自动接上
- 打开串口时脚本会**显式拉低 DTR/RTS 并等 1.5 秒**：S3 的 USB-Serial/JTAG 可能把这两个信号当成芯片复位，
  等固件启动完再发命令，避免第一条命令丢在启动过程里

排查 USB 输入是否真的到了固件：

```text
status        # 额外一行 usb host=1 drv=1 rx=1234 lines=17
```

- `host=1`：主机在发 USB SOF，口是通的
- `drv=1`：固件里的 USB 驱动装好了
- `rx/lines`：从 USB 收到的字节/命令行数；按键没反应时先看这两个数是否在涨
- 每收到一行 USB 命令，日志里还会出现 `usb> status`，确认命令确实进来了

### 用 a/s/d/w 当四个按键（PC）

两种用法：

```text
a        K1 上
d        K2 下
s        K3 确认
w        K4 返回
```

- 小写 = 短按，大写 `A/D/S/W` = 长按（长按 K4 = 切主题，长按 K3 = 清 RF 峰值）
- 兼容原有布局：a 在左、d 在中、s 在右，w 单独在上方（按你的习惯排的）

如果不想每次按回车，在 COM4 上开单键模式：

```text
keypad on
```

之后 PC 终端里直接按 `a`/`d`/`s`/`w` 就立即生效，不用回车；大写仍是长按。
按 `Esc` 退出单键模式，回到正常的命令行。

`finder-cli.ps1` 也支持：输入 `keypad on` 后，脚本会把单个按键立即发给板子，`Esc` 退出。

- PowerShell 直接发一条命令（不装任何东西）：

```powershell
$p = New-Object System.IO.Ports.SerialPort COM7,115200,None,8,One
$p.Open(); $p.WriteLine("page"); Start-Sleep -Milliseconds 400
$p.ReadExisting(); $p.Close()
```

**Linux / macOS 例子：**

```sh
stty -F /dev/ttyACM0 raw -echo
printf 'page\r\n' > /dev/ttyACM0      # 发送命令
cat /dev/ttyACM0                      # 读日志和回复
printf 'btn 3\r\n' > /dev/ttyACM0     # 短按 OK
```

Python 例子（两边系统都能用）：

```python
import serial, time
s = serial.Serial('COM7', 115200, timeout=0.5)   # Linux/macOS 用 /dev/ttyACM0
s.write(b'status\r\n')
time.sleep(0.3)
print(s.read(4096).decode('ascii', 'ignore'))
s.write(b'btn 4 long\r\n')            # 长按返回/切主题
```

注意：

- **两个 USB 口同时接两台 PC 会引入地环路**，NRF 的 RPD 可能再次被压成 0；
  测量时建议只给板子一路供电，或给第二台 PC 的 USB 加隔离器（ADuM3160/4160 一类）
- 只有**原生 USB**（GPIO19/20）可用；如果板子的第二个口其实是另一颗 UART 桥，就没有这路控制台
- 刷机仍可走 UART 口；原生 USB 也支持 USB-JTAG 烧录
- RF SCAN 期间 Wi-Fi 是暂停的，`status` 会显示 `wifi=0`，这是正常现象

### RF SCAN PRO（2400–2525MHz）

类似收音机自动搜台：NRF 从 **2400 到 2525MHz 逐 1MHz** 扫一遍，每个点采 8ms RPD，
一轮约 **1.7 秒**，循环刷新。

- 屏幕每屏显示 **76 个信道**（2px/信道），K1/K2（`a`/`d`）左右滚动，底部有滚动条
- 屏幕显示窗口内最强信道：`ACT 87% @ 2438MHz` 或 `PK 100% @ 2431MHz`
- 短按 `#`：切换两种视图（同时打印高能列表）
  - **WATERFALL（WF，默认）**：每一轮扫描在**最上面新增一行**，旧行往下沉并逐渐变暗
    - 76 行 ≈ 130 秒历史；新能量按强度出现，旧能量自然下沉、淡出
    - 颜色：<50 绿、50–79 黄、≥80 红
    - 用来观察“刚才到现在”哪个频段在波动
  - **PEAK**：峰值驻留图，累计最大值
    - 窗口内最强的那根柱子旁边直接标频率，例如 `2432MHz`
    - 底部每 20MHz 一个刻度，左右两端显示窗口边界
- 长按 `#`：清零全部数据重新累计
- 短按 `*`：退出并恢复 Wi-Fi

PC（UART/COM4）输出：

```text
pro S=3 wf  2400:0010000000000000013000000520000091010000010000...
pro S=4 pk  2400:4110300313010396305050022311000900000092000...
```

- `wf` / `pk` 对应屏幕当前视图
- `wf` 是**本轮**每个 MHz 的实测值（瀑布最新一行），`pk` 是累计峰值
- `S=` 是第几轮
- 后面 **126 个字符**依次对应 2400..2525MHz
- 字符 `0-9` = 该 MHz 的峰值百分比 / 10（`9` = 90% 以上）

每 5 轮和停止时打印高能分布：

```text
pro top 2427MHz pk=100%
pro hot 2425-2427MHz pk=100% hit=6/15
pro hot 2433-2434MHz pk=100% hit=4/10
pro hot: none (peak < 50%)
```

- `hot` 阈值是峰值 ≥ 50%
- `hit=x/y` 表示该段内信道出现 ≥25% 能量的次数 / 访问次数

串口命令：

```text
pro              启动
pro off          停止（会打印最终高能列表，回到 13 信道 RF SCAN）
pro reset        清零
pro list         立即打印当前高能列表
ui pro           直接跳到该页面
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
- 默认深绿 PHO 主题，暗背景，强信号才提亮
- 长按 `*` 循环 10 套配色：PHO / AMBER / ICE / MAGENTA / RED / TEAL / BLUE / SOLAR / VIOLET / LIME
- 底部 `RF` 是 NRF 在当前 Wi-Fi 频道中心 1MHz 的 RPD 占用，**不是 dBm**
- 非 ASCII SSID 在 FINDER 页显示 `SSID [UTF8]` / `SSID [GBK]`
- `* -> AP LIST` 打开 AP/SSID 树：AP 列表 → AP 详情 → STA 列表
- AP 列表按“已见 STA / AP 报告 STA”排序，隐藏 AP 显示 `<AABBCC>`（BSSID 后 3 字节）
- AP 详情显示 SSID、BSSID、厂商、信道、BSS Load、已见 STA 数
- STA 列表显示 MAC、RSSI、厂商/probe SSID、最近出现；`#` 设为临时目标，`*` 返回
- 设为目标不会锁信道；回到 FINDER 后再按 `#` 才锁定
- AP 页右上角显示编码策略 `AUTO / U8 / GBK`；长按 `#` 循环切换，用来纠正 GBK/UTF-8 误判
- 二进制/控制字符 SSID 不再硬画：列表显示 `<AABBCC>`，详情标 `BINARY`
- 隐藏 SSID 显示 `<AABBCC>`，详情标 `HIDDEN`
- 隐藏 SSID 显示 `SSID HIDDEN / AP ADVERTISES NO SSID`，BSSID 保留，可用 `stas` 按 AP 分组
- 完全听不到 AP Beacon 才显示 `NO SSID / AP BEACON NOT HEARD`

### RF SCAN 页

**开机默认进入 RF SCAN**（不再先停在 FINDER）：

- 开机时直接暂停 ESP32 Wi-Fi，只让 NRF 扫描，避免 ESP32 自己的射频/本振抬高 NRF 读数
- 扫描期间目标跟踪、AP/STA 列表暂停；Wi-Fi 在退出 RF SCAN 后自动恢复
- 短按 `*` 离开 RF SCAN 到菜单，Wi-Fi 随即恢复；继续按 `*` 回到 FINDER
- 想直接进 FINDER：`*` 菜单 → `*` 返回，或串口 `ui find`

- 短按 `#`：查看图表说明（每根柱/峰值/平均值/AGE/HIT 的含义）
- 长按 `#`：清空峰值重新开始
- 短按 `*`：停止扫描并返回
- 长按 `*`：切换主题（任意页面都可）
- 右上角最大的是**最近 10 秒的滚动命中速度**（`10S 0.3/s`），3 倍字体、右对齐
- 速度下方是当前显示信道的**累计命中次数**（即 `HIT x/y` 的 x），2 倍字体
- 第二行左侧 `Sxxxxx` 是扫描轮数，`C18+C21` 是在线的 NRF
- 底部摘要 `CHn PKxx AGEnn s HITx/y`：
  - `PK` 峰值百分比
  - `AGE` 该峰值最后一次出现距今多久
  - `HIT` 出现过 ≥25% 能量的轮数 / 总轮数
- 每根柱外层是峰值保持，内层是平均值；`rfdata` 可看到 streak/max_streak
- `Sxxxxx` 数字在涨说明扫描任务在跑
- 显示信道带 10 个百分点峰值迟滞，避免稍强的另一个信道让 `HIT x/y` 看起来被重置

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
- FINDER 页显示 `SSID [UTF8]` / `SSID [GBK]`；`* -> AP LIST` 打开 AP/SSID 树
- AP 列表、详情和 STA 列表都用同一套 UTF-8/GBK 自动判定和 CJK 点阵
- 隐藏 SSID 在列表里显示 `<AABBCC>`，详情里标 `HIDDEN`
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
- 中文 SSID：UTF-8/GBK 双解码 + 12x12 CJK 点阵；AP/STA 树使用同一套渲染
- UI 状态机：FINDER / MENU / INFO / SSID；菜单承载学习、清除、峰值、信道、SSID、重启
- OUI 厂商表由 `tools/gen-oui.py` 从 IEEE 公开 OUI 列表生成
- NRF 只读 `RPD`，跟随当前 Wi-Fi 频道中心，8 次采样得到 0–100%
- NRF 探测会重新挂载对应 CSN 的 SPI 设备；旧代码只探测第一组 CSN，现已修正
- 串口日志 `NRF24L01 on CSN xx` 只出现在 UART；屏幕上看 INFO 页的 `RF OK Cxx`

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
