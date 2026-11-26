# AD8232 单通道心电采集固件

ESP32-C3 通过 ADS1115 读取 AD8232，在 ST7789 和板载 Wi-Fi 网页上显示波形。应用默认关闭 UART0 数据输出。主要板级参数集中在 [board_config.h](main/board_config.h)。接线按 [2026-11-18 PCB1 网表](../other/Netlist_PCB1_2026-11-18.enet)核对，屏幕初始化沿用 [STM32 交接说明](../other/ESP32C3_AD8232_交接说明.md)和原代码中的参数。

## Wi-Fi 实时查看

热点由 ESP32-C3 自己创建。需要网页时，在 `firmware/main/` 创建 `ecg_wifi.local.h`，填入自己的 WPA2 密码（8–63 个可打印 ASCII 字符）。下面的引号内请填写你选择的密码。

```c
#define ECG_WIFI_PASSWORD ""
```

如需使用双引号或反斜杠，请写成 C 字符串转义形式。此文件已加入 Git 忽略规则。未创建此文件时，固件仍会采集并在屏幕上显示波形，热点和网页功能关闭。

1. 烧录下文的 Wi-Fi 版固件，拔掉 H1 上的 USB-TTL 连线，再复位并用板载电池供电。
2. 手机或电脑连接 **`AD8232-ECG`** 热点，输入在本地配置文件中设置的密码。系统提示此 Wi-Fi 无互联网时，选择继续连接。
3. 在浏览器地址栏打开 [http://192.168.4.1](http://192.168.4.1)。烧录新版后，已打开的旧页面需要刷新一次。页面显示最近 3 秒经滤波的波形，每约 150 ms 请求一批新点。纵轴显示 AD8232 OUT 端电压，单位为 mV。

网页用橙色圆点标出 QRS 主峰，检测到心搏时心形点亮，心率以 BPM 显示。脱落、输入饱和和 ADC 读取错误会更新状态并隐藏心率，无效采样留空。Wi-Fi 重新连通后，页面会自行恢复。热点名称可在 [board_config.h](main/board_config.h) 中修改，网页随固件一起烧录。

波形下方显示实测采样率，以及开机以来累计的定时漏采、ADC 错误和显示队列丢点。对比网页负载时，可以先让板子运行一分钟，首次打开页面读取计数，再保持页面打开一分钟并比较计数增量。

## 编译与下载

使用 ESP-IDF **v6.0.2**。目标是 ESP32-C3，Flash 配置为 4 MB。先按上文设置热点密码，再用以下命令构建 Wi-Fi 版。命令使用台架验证过的 40 MHz DIO 设置，并关闭开机提示音。打开该版本的 ESP-IDF 终端，在仓库根目录执行。

```powershell
cd firmware
idf.py -B build_wifi -D SDKCONFIG=build_wifi/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.recovery.defaults' -D ECG_STARTUP_BUZZER=OFF build
idf.py -B build_wifi -p COMx -b 115200 flash
```

如果普通 PowerShell 提示无法识别 `idf.py`，可在当前窗口加载本机通过 EIM 安装的环境，再执行构建或烧录命令。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
idf.py --version
```

若 PowerShell 阻止加载脚本，打开桌面的 `IDF_v6.0.2_Powershell` 快捷方式，再切换到 `firmware` 目录。

将 `COMx` 替换为实际接到 H1 的 3.3 V USB-TTL 串口号。H1 从第一脚起依次为 GND、TXD、RXD。将 USB-TTL 的 GND、RXD、TXD 依次接到 H1 的这三脚。板载 USB-C 用于充电。

H1 只引出三根线，工程采用手动复位。烧录前先关闭串口监视器，然后按住 **BOOT（U9）**，按一下 **EN（U8）**，松开 EN 后再松开 BOOT。进入下载模式后运行 `flash`。烧录校验通过后，松开 BOOT 并再按一次 EN，启动应用。[Espressif 下载模式说明](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html)

如果 VS Code 停在 `Serial port COMx:`，先用 `Ctrl+C` 停止该任务。关闭占用同一端口的监视器或串口助手，重新插拔 USB-TTL 适配器确认串口号，再选中该端口并按上述步骤重试。在 ESP-IDF 终端可用 `python -m serial.tools.list_ports -v` 查看端口对应的设备。`esptool.py` 和 `write_flash` 的弃用提示来自旧命令别名，可通过终端里的 `idf.py flash` 查看具体错误。[Espressif 串口排查说明](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/troubleshooting.html)

先用网页验证模拟波形时，可以构建独立的模拟版本。每个构建目录会保留自己的模式选项。

```powershell
idf.py -B build_wifi_sim -D SDKCONFIG=build_wifi_sim/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.recovery.defaults' -D ECG_SIMULATE=ON -D ECG_STARTUP_BUZZER=OFF build
idf.py -B build_wifi_sim -p COMx -b 115200 flash
```

需要串口排查时，`-D ECG_DIAGNOSTICS=ON` 可开启原始采样和诊断行，`-D ECG_UART_OUTPUT=ON` 可只开启原始采样。设置 `-D ECG_WIFI=OFF` 可关闭热点。

```powershell
idf.py -B build_diag -D ECG_DIAGNOSTICS=ON build
idf.py -B build_diag -p COMx -b 115200 flash

idf.py -B build_sim -D ECG_SIMULATE=ON -D ECG_DIAGNOSTICS=ON build
idf.py -B build_sim -p COMx -b 115200 flash
```

需要用串口绘图软件看模拟波形时，可用 `idf.py -B build_sim -D ECG_DIAGNOSTICS=OFF -D ECG_UART_OUTPUT=ON build` 只输出采样，再烧录 `build_sim`。

### 启动恢复版

恢复版在独立目录使用 40 MHz DIO、打开 bootloader 日志，并将开机蜂鸣器保持静音。它保留硬件采样和显示滤波。普通构建的蜂鸣器默认打开，也可用 `-D ECG_STARTUP_BUZZER=OFF` 关闭。

```powershell
idf.py -B build_recovery -D SDKCONFIG=build_recovery/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.recovery.defaults' -D ECG_SIMULATE=OFF -D ECG_DIAGNOSTICS=ON -D ECG_WIFI=OFF -D ECG_STARTUP_BUZZER=OFF build
idf.py -B build_recovery -p COMx -b 115200 flash
```

如需测试开机提示音，可以沿用 40 MHz DIO 和诊断日志配置，单独构建开启蜂鸣器的版本。

```powershell
idf.py -B build_buzzer -D SDKCONFIG=build_buzzer/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.recovery.defaults' -D ECG_SIMULATE=OFF -D ECG_DIAGNOSTICS=ON -D ECG_WIFI=OFF -D ECG_STARTUP_BUZZER=ON build
idf.py -B build_buzzer -p COMx -b 115200 flash
```

串口中的 `# boot app=... reset=...` 记录上次复位原因。后续 `# boot acquisition ready` 和 `# boot display ready` 分别表示采样和显示已启动。还未出现应用版本时，先看 bootloader 打印的镜像或分区错误。[ESP-IDF 复位原因](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c3/api-reference/system/misc_system_api.html#reset-reason)

2026-11-20 在 PCB1 上用 COM4 核对了原版的 bootloader、分区表和应用，三段均与本地文件一致。原版约每 87 ms 出现一次 `RTC_SW_SYS_RST`。v0.3.1 恢复版构建并烧录后连续运行超过 70 秒，记录到 127.94～128.00 点/秒。ADC 错误、超时、漏采和两个队列的丢点计数均为 0。手动复位后，串口重新出数，用户确认屏幕恢复正常。恢复版同时修改了 Flash 时钟、日志和蜂鸣设置，原版复位的具体触发原因尚待确认。

## 构建验证

2026-11-20 在 Windows 上使用 ESP-IDF v6.0.2、RISC-V GCC 15.2.0、CMake 4.0.3 和 Ninja 1.12.1 实际构建了以下 v0.3.0 配置。esptool v5.3.1 确认三个应用镜像的目标均为 ESP32-C3，Flash 配置为 4 MB，校验和及镜像哈希均有效。

| 配置 | 应用镜像 | 大小 | 结果 |
| --- | --- | ---: | --- |
| 硬件采样与屏幕 | `build/ad8232_ecg.bin` | 185,760 字节 | 构建通过 |
| 硬件采样、屏幕与诊断 | `build_diag/ad8232_ecg.bin` | 186,256 字节 | 构建通过 |
| 模拟波形、屏幕与诊断 | `build_sim/ad8232_ecg.bin` | 175,568 字节 | 构建通过 |

v0.3.1 恢复版的应用镜像为 `build_recovery/ad8232_ecg.bin`，186,944 字节，已通过构建和镜像校验。

主机测试验证了扫屏回绕、同列峰值保留、缺失数据留空、BOOT 消抖及蜂鸣输出的三组低电平脉冲。此前的模拟波形测试中，连续 640 点包含 6 个 R 峰，相邻峰间隔为 106 或 107 点，符合 128 点/秒时的 72 BPM 模拟设定。

用主机编译并回放本版固件的 C 滤波算法。72 BPM 模拟波形在 5 秒内保留 6 个主峰。两份串口日志在各自选取的干扰窗口内，拟合的 50 Hz 振幅下降约 98%～99%。回放还验证了孤立尖峰、近电源轨采样以及同一列内的无效点会断开绘图。v0.3.0 显示效果待上板验证。

QRS 检测器在 40、72、120、150 和 180 BPM 模拟信号上通过回放检查，测试覆盖倒置信号、基线漂移、50 Hz 干扰、掉样与恢复。对 MIT-BIH 记录 100、108、119、200、223 的第一通道回放时，按 ±100 ms 匹配参考标注，各匹配 2267/2267、1684/1759、1983/1983、2589/2595、2599/2600 个心搏，额外检测数分别为 0、64、0、3、0。比较排除了各记录的首尾 2 秒，并以 100 倍增益将输入映射到 ADC 码。实际 AD8232 电路仍需上板核对。[MIT-BIH Arrhythmia Database](https://physionet.org/content/mitdb/1.0.0/)

v0.4.1 回放交替出现 78、84 BPM 单拍间隔的模拟波形时，启动过渡后的单次显示变化由最大 6 BPM 降为 1 BPM。模拟心率从 78 持续升到 84 BPM 时，约 7 秒后显示值达到 83 BPM。80 BPM 测试中加入一个额外心搏、删去另一个后，已稳定的显示值仍为 80 BPM。

## 接线

| 信号 | 接线 | 用途 |
| --- | --- | --- |
| AD8232 OUT | ADS1115 AIN0 | 对 GND 单端采样，PGA ±4.096 V |
| ADS1115 SDA / SCL | GPIO10 / GPIO8 | I²C 400 kHz，地址 `0x48`。R31、R23 各用 4.7 kΩ 上拉到 `3V3_D` |
| ADS1115 ALERT/RDY | `RDY` 网无主控连接 | 通过 I²C 查询转换状态 |
| AD8232 SDN | GPIO5 | 高电平工作，模拟模式拉低 |
| AD8232 LOD+ / LOD− | GPIO6 / GPIO7 | 高电平表示脱落 |
| UART0 TX / RX | GPIO21 / GPIO20 | H1 串口下载、115200 8N1 输出 |
| 蜂鸣器 | GPIO2 | 低有效驱动无源蜂鸣器，静音时为高电平 |
| BOOT | GPIO9 | 运行时短按切换 `SWEEP` 和 `SCOPE`，按住复位可进入串口下载模式 |
| ST7789 SCK / MOSI / RST / DC / CS / BL | GPIO4 / GPIO3 / GPIO1 / GPIO0 / GPIO19 / GPIO18 | U11 的第 3～8 脚。第 1 脚 GND，第 2 脚 `3V3_D` |

LCD1 型号为 GMT024-08-SPI8P，通过 U11 连接，使用已确认的高有效 BL 逻辑输入。屏幕使用 320 × 240 横屏、RGB565（`COLMOD=0x55`）、`MADCTL=0xA0`、关闭反色（`INVOFF=0x20`）、X/Y 偏移为 0，SPI Mode 0、MSB first、目标 12 MHz，像素高字节先发送。[原屏幕交接说明](../other/ESP32C3_AD8232_交接说明.md)

## 开机和屏幕

上电或复位进入应用后，蜂鸣器用 2 kHz、50% 占空比的 PWM 发出三声提示音，每声约 100 ms，间隔约 120 ms。`ECG_STARTUP_BEEP_HZ` 可调音调，`ECG_STARTUP_BEEPS` 可设为 `0` 关闭提示音。屏幕先显示红、绿、蓝、白四色条约 650 ms，然后进入黑底绿线的擦拭绘图。

两种显示模式每屏均覆盖约 3 秒。当前 360 点/秒的配置对应 1080 个采样周期。同列的多个采样保留最高和最低点，漏采留空。显示去偏置和缩放只用于绘图，`ECG_DISPLAY_CODES_PER_PIXEL` 默认是 40 ADC 码/像素。采样、刷屏和网页服务各自运行。启用串口时，由单独的串口任务输出。

波形区的大格横向为 0.2 秒/格，纵向为 20 像素/格，默认对应 800 ADC 码（AD8232 的 `OUT` 端约 100 mV）。电极输入端的幅度需要用已知信号标定。

绘图区基线上方有 108 像素，按默认比例可显示相对基线上方约 0.54 V 的 `OUT` 端变化。波形超出绘图区时，对应列的屏幕上沿或下沿会标成黄色。

v0.3.0 在显示任务中用 50 Hz 陷波和 35 Hz 二阶巴特沃思低通减少工频和高频干扰。孤立尖峰用前后相邻采样辨认，因此绘图增加一个采样周期的延迟。只有当前点比两侧各高或低至少 3000 码，且两侧彼此相差不超过 1000 码时，才将该点留空。参数集中在 `board_config.h`。[二阶数字滤波器系数公式](https://www.w3.org/TR/audio-eq-cookbook/)

实际采样读数低于或等于 0.05 V、或高于或等于 3.25 V 时，屏幕留空对应列并显示 `INPUT CLIP`。脱落、掉样、饱和或识别出的孤立尖峰会断开屏幕和网页的连线，随后以首个可用采样重新初始化滤波状态。启用串口后，成功读取的原始码可用于排查供电、接线和饱和。

上电默认进入 `SWEEP`，从左向右擦除并补画波形。短按并松开 BOOT 切到 `SCOPE`，整帧刷新 3 秒波形，有合适上升沿时将其对齐到离左边第一大格的位置，否则显示最近 3 秒。再短按返回 `SWEEP`，从左侧重新开始。两种模式下采样和网页持续更新。

断开导联显示 `LEADS OFF`，ADC 通信失败显示 `ADC ERROR`。真实和模拟波形共用 QRS 检测器。屏幕和网页在获得至少 3 个有效 R–R 间隔后显示估算 BPM，启动学习、信号异常或约 2.6 秒没有检测到新心搏时显示 `--`。模拟版的 72 BPM 由波形算出。

检测器从已滤波的波形中提取斜率包络，按背景噪声和近期 QRS 强度调整阈值，并在检测段内用相对基线的最大偏移定位正向或倒置的 QRS 主峰。相邻心搏至少间隔 250 ms。BPM 取最近最多 6 个 R–R 间隔的中位数，再用约 2.8 秒的时间常数平滑。QRS 标记仍按实际检出时刻更新。[Pan 与 Tompkins 的实时 QRS 检测论文](https://doi.org/10.1109/TBME.1985.325532)

## 采样和输出

ESP Timer 按 `ECG_SAMPLE_HZ` 唤醒采样任务。当前目标为 360 点/秒，触发间隔约 2,778 µs。ADS1115 使用 860 SPS 的单次转换配置。每个周期先检查并读取上一次转换，再触发下一次，等下一周期来取结果。如果前次转换因为延迟触发而仍在进行，保留该次序号并跳过本次触发。每个结果对应实际触发时记录的序号和时间。±4.096 V 档的码宽为 125 µV，输入电压还需满足 ADC 供电规定。[TI ADS1115 数据手册](https://www.ti.com/lit/ds/symlink/ads1115.pdf)

网页按采样序号获取增量。首次打开或游标落后超过当前窗口时，返回完整波形窗口。正常连续连接时，360 点/秒的波形正文约为 1.4 KB/s，另有 HTTP 响应头开销。漏采在序列中保留为空点。

启用 UART 后，输出任务通过 64 点队列接收数据，每行一个十进制 `int16`，以 CRLF 结束。复位时可能先收到芯片 ROM 的启动文本。ADC 读取失败时跳过该点，队列满时丢弃新点。真实采样时即使检测到导联脱落，仍输出成功读取的原始码。

诊断版约每 2 秒输出一行 `#` 开头的状态。`hz` 是窗口内有效采样率，`seq` 和 `t_us` 是最近一次成功采样的序号和触发后的时间戳（开机后微秒）。序号按定时周期累加。`adc` 是最近一次尝试的状态，`errors`、`timeouts`、`missed`、`drops` 和 `lcd_drops` 分别累计 ADC 错误、超时、未能触发的采样周期、串口队列丢点和屏幕队列丢点。模拟模式沿用 STM32 的 P/QRS/T 分段模板，输出基线为 13200 的模拟 ADC 码，名义心率为 72 BPM。[原波形源码](../other/Core/Src/ecg_sim.c)

后续增加保存功能时，可由手机或电脑持续接收并保存，优先记录原始 ADC 码、采样序号、开机后时间戳和有效性。现有网页接口传输滤波数据，适用于实时绘图和按序号标记缺口。

## 台架验收

此前恢复版的 128 点/秒采样已在 PCB1 上验证。用 ECG 模拟器或台架信号源测试当前配置的波形、显示切换和导联状态。

1. 烧录 Wi-Fi 模拟波形版。屏幕先显示红、绿、蓝、白彩条，再显示横屏绿线和 `SIMULATION`，数秒后显示约 `72 BPM`。拔掉 USB-TTL，连接热点并打开网页。检查波形持续滚动，状态为「模拟波形」，QRS 主峰处出现橙色标记。
2. 短按 BOOT 切到 `SCOPE`，确认屏幕整帧波形的上升沿对齐。再短按返回 `SWEEP`，确认从左侧开始扫描。切换期间网页应持续更新。断开再连上热点，确认网页能恢复。
3. 烧录硬件诊断版并接入信号源。记录连续多条诊断中的 `hz`、相邻 `seq` 和 `t_us` 的差值及各计数。正常时显示 `LIVE`，`hz` 应接近配置值，`adc=ESP_OK`，错误和丢点计数保持为零。
4. 断开模拟器的一根检测导联，确认屏幕显示 `LEADS OFF`，对应的 `lod_plus` 或 `lod_minus` 变为 `1`，屏幕和网页的 BPM 都变为 `--`。重新连接后，检查画面和心率能否恢复。台架可切断 ADC 应答时，检查屏幕是否显示 `ADC ERROR`，并在恢复接线后重新绘图。
