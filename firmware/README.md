# AD8232 单通道心电采集固件

现用固件为 **v0.4.1**，目标采样率为 360 点/秒。上电默认进入 `SWEEP` 扫屏，短按 BOOT 切换到 `SCOPE` 整帧显示，再按一次返回。两种模式均显示约 3 秒的波形。

## Wi-Fi 实时查看

需要用浏览器查看时，在 `firmware/main/` 创建 `ecg_wifi.local.h`，把自己的 WPA2 密码填入引号内。密码须为 8–63 个可打印 ASCII 字符，双引号和反斜杠需要按 C 字符串规则转义。

```c
#define ECG_WIFI_PASSWORD ""
```

该文件由 Git 忽略。没有这个文件时，板子照常采集和显示，热点保持关闭。烧录后连接板载热点 **`AD8232-ECG`**，再打开板载网页。手机提示此网络无互联网时，选择继续连接。[打开实时波形](http://192.168.4.1)

## 编译与烧录

在 **ESP-IDF v6.0.2** 的 PowerShell 中，从仓库根目录执行。当前配置使用 4 MB Flash、40 MHz DIO，并关闭开机蜂鸣。

```powershell
cd firmware
idf.py -B build_wifi -D SDKCONFIG=build_wifi/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.recovery.defaults' -D ECG_STARTUP_BUZZER=OFF build
```

将 3.3 V USB-TTL 接到 H1。H1 从第 1 脚起依次为 GND、板端 TXD、板端 RXD，分别接转接器的 GND、RXD、TXD。关闭串口监视器，按住 **BOOT（U9）**，按一下 **EN（U8）**，松开 EN 再松开 BOOT。将 `COMx` 换成实际端口后执行烧录。

```powershell
idf.py -B build_wifi -p COMx -b 115200 flash
```

烧录完成后按 EN 启动。脱机查看时，拔掉 USB-TTL 并用板载电池供电。板载 USB-C 用于充电。[Espressif 下载模式说明](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/advanced-topics/boot-mode-selection.html)

## 接线

| 信号 | 接线 |
| --- | --- |
| AD8232 OUT | ADS1115 AIN0（单端对 GND，PGA ±4.096 V） |
| ADS1115 SDA / SCL | GPIO10 / GPIO8（地址 `0x48`，各通过 4.7 kΩ 上拉至 `3V3_D`） |
| AD8232 SDN / LOD+ / LOD− | GPIO5 / GPIO6 / GPIO7 |
| ST7789 SCK / MOSI / RST / DC / CS / BL | GPIO4 / GPIO3 / GPIO1 / GPIO0 / GPIO19 / GPIO18 |
| UART0 TX / RX | GPIO21 / GPIO20 |
| BOOT / 蜂鸣器 | GPIO9 / GPIO2 |

ST7789 使用 320 × 240 横屏，U11 第 1 脚为 GND，第 2 脚为 `3V3_D`。板级引脚、采样与显示参数见源码。[board_config.h](main/board_config.h)
