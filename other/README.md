# STM32F103C8T6 + ST7789 + LVGL 横屏模拟心电图

后续 ESP32-C3 + AD8232 移植请先阅读 [需求与样式交接说明](ESP32C3_AD8232_交接说明.md)，其中包含最终布局、屏幕参数、预览和待完成的真实采集功能。

适用于已确认的 ST7789、原生 240×320、8 引脚四线 SPI 模块，旋转为 **320×240 横屏**、RGB565。
使用内部 HSI/2 经 PLL ×12 得到 48 MHz，APB1 为 24 MHz、APB2 为 48 MHz。
SPI1 为 Mode 0、MSB first、8 位、12 MHz，不依赖外部晶振。屏幕接线保持不变。

## 接线

| 屏幕引脚 | STM32 引脚 | 说明 |
| --- | --- | --- |
| GND | GND | 与开发板共地 |
| VCC | 3.3V | 模块供电，使用开发板稳定的 3.3V 电源 |
| SCL | PA5 | SPI1 SCK，非 I2C |
| SDA | PA7 | SPI1 MOSI，非 I2C |
| RST | PB10 | 复位，低有效 |
| DC | PB1 | 命令/数据选择 |
| CS | PB0 | 片选，低有效 |
| BL | PB11 | 用户已确认是逻辑输入，默认高电平打开背光 |

PA6 为 SPI1 MISO 预留，**不用连接屏幕**。PA13/PA14 保留给 SWD 下载调试。
本工程的 PB11 只适合模块的 BL 逻辑控制输入，不能直接驱动裸背光 LED。

## 编译、下载与预期画面

1. 若 IDE 一直开着，先关闭旧 IOC 页面（不要保存旧配置），关闭再打开工程以重新加载 `.cproject`，然后按 F5 刷新。
2. 执行 Project → Clean，再 Build Project。Debug/Release 都已配置 `-Os` 和 `-flto`；缺少它们会超出 64 KB Flash。无需重新生成代码。
3. ST-Link 连接 SWDIO/PA13、SWCLK/PA14、GND；使用 Debug 或 Run 下载。BOOT0 保持低电平。Debug 若停在 main，按 F8 继续。
4. 屏幕显示横屏界面：黑色背景、中下部横向完整的绿色心电扫描、右上角适中大小的红色 72 心率和 bpm 单位，以及随模拟心跳闪动的红色心形图标。左上方标明 SIMULATION。

## 模拟心电图

- 图形区 304×136，位于中下部，时间窗约 3.04 秒，每个横向像素表示 10 ms，模拟波形速率 100 点/秒。
- 心率固定为约 72 BPM；界面上的 BPM 是波形生成参数，不是从人体信号测出的数值。
- 波形采用整数分段插值产生，用于预览界面和刷新效果；幅度单位是显示像素，未标定为 mV 或纸速。
- 当前只显示心电图和心率，没有血氧、血压、呼吸或临床报警功能；中文暂不添加。
- 右上角心率由 33 像素高的红色七段数码图形绘制，不额外引入字体；默认固定 72 BPM，红色心形闪烁由模拟 R 峰触发。
- 每 40 ms 调度一次波形更新，仅重绘扫描附近的窄条，扫描绕回时分两段重绘。
- 不需要连接传感器、电极或新增硬件，屏幕接线沿用上表。
- 当前程序没有 ADC 采集、模拟前端、滤波或心率检测算法。后续接入测量时，需将 `ecg_sim.c` 的生成数据替换为经过前端采集和处理的数据。

软件渲染预览：`Build/ecg_preview.png`；动态预览：`Build/ecg_preview.gif`。

也可以在 PowerShell 执行 `./build.ps1`，在 `Build/` 生成 ELF、HEX、BIN 和 MAP。
脚本会从 PATH 或本机 CubeIDE 1.17.0 安装路径查找编译器，其他安装位置可使用：

```powershell
./build.ps1 -ToolchainBin '你的工具链路径/tools/bin'
```

BIN 烧录起始地址为 `0x08000000`；HEX/ELF 已带地址。
编译和链接通过不代表硬件已点亮，实际画面需要接线下载后确认。

## 代码入口与后续使用

- `Core/Src/main.c`：初始化 ST7789/LVGL，在主循环中每隔 5 ms 调度 LVGL。
- `Core/Src/lvgl_port.c`：显示刷新回调、3840 字节局部缓冲、608 字节波形历史、紧凑红色心率、心形闪烁及局部扫描刷新。
- `Core/Src/ecg_sim.c`：独立的整数模拟波形生成器；默认 72 BPM，参数位于 `Core/Inc/ecg_sim.h`。
- `Core/Inc/lv_conf.h`：LVGL 裁剪配置，8 KB 内存池、RGB565 字节交换、单一 14 px 字体。
- `Drivers/lvgl/`：MIT 许可的 LVGL v8.3.11 源码；不包含上游 examples/demos。
- `Core/Src/st7789.c`：复位、初始化、窗口寻址、填色、文字和演示。
- `Core/Inc/st7789.h`：分辨率、偏移、颜色顺序、反色、背光极性和接口。
- `Core/Src/spi.c`、`gpio.c`：SPI1 和控制引脚初始化。
- `.ioc` 已同步 SPI1、GPIO、SWD 与内部时钟配置；应用调用位于 USER CODE 区。

```c
ST7789_FillScreen(TFT_BLACK);
ST7789_FillRect(20, 40, 100, 60, TFT_RED);
ST7789_DrawString(12, 120, "HELLO STM32", TFT_WHITE, TFT_BLACK, 2);
```

以上 `ST7789_*` 是保留的直接绘图接口；当前画面由 LVGL 管理，不要在 LVGL 刷新期间混用直接绘图。
原始竖屏测试程序 `ST7789_Demo()` 和 `ST7789_DemoTick()` 仍保留，但当前不再调用。

LVGL 当前仅启用标签和基础对象，通过绘图事件用窄矩形连接采样点，不启用复杂图表控件或掩码引擎。
不含中文字库、大图、主题、复杂效果和输入设备。单缓冲阻塞发送完成后再调用 `lv_disp_flush_ready()`。
LVGL 直接读取 `HAL_GetTick()`，不需要额外调用 `lv_tick_inc()`，主循环不依赖 RTOS。
链接脚本仍限制 64 KB Flash / 20 KB RAM，栈预留增加到 2 KB。
F103C8T6 容量紧张，增加字体或功能后必须重新检查 Flash 和 RAM 占用。
本次完整横向波形及右上角红色心率界面已通过 CubeIDE Debug 构建：Flash 61752 字节，RAM 16400 字节（含链接脚本预留堆栈）。
CubeIDE 1.17 的 Makefile 在启用 LTO 时会报告 `.su/.cyclo` 统计辅助文件未生成的警告，
这不影响 ELF 链接或烧录；以构建无 errors、ELF 正常生成及实际运行结果为准。

## 本机渲染验证

`Tests/host/run.ps1` 使用本机 GCC 编译同一 LVGL 配置和界面，在模拟 60 秒内验证：
横屏尺寸、紧凑红色心率字形、红色心形闪烁、模拟波形幅度/周期、刷新区域范围、LVGL 内存完整性及堆无持续增长。
测试还会比较局部扫描重绘与整屏重绘的逐像素结果，覆盖扫描绕回和心形亮灭，检查是否留下旧内容。
输出 `Build/ecg_preview.bmp` 和动态预览帧，这是软件渲染预览，不代表硬件实拍。
执行 `python Tests/host/render_preview.py`（需要 Pillow）可将这些帧打包为 PNG 和 3.04 秒 GIF。
PC 指针宽度与 Cortex-M3 不同，主机测试的堆用量不能直接当作 MCU 的实际用量。

## 故障定位

- 背光不亮：先确认供电、共地和 BL 极性；低电平点亮的模块将头文件中的 `ST7789_BL_ACTIVE` 改为 `GPIO_PIN_RESET`。
- 只有背光或白屏：核对 SDA=PA7、SCL=PA5，检查 CS/DC/RST 接线；用逻辑分析仪观察 CS 低时是否有 12 MHz 时钟。
- 旧程序正常，新程序花屏：缩短信号线；必要时在 SPI1 中将分频从 4 改为 8，把 SPI 降至 6 MHz，并同步修改 `.ioc`。
- 红蓝相反：将 `ST7789_MADCTL` 从 `0xA0` 改为 `0xA8`。
- 当前模组反馈白底红线，因此 `ST7789_INVERSION` 已设为 `0`（发送 INVOFF / 0x20）。若更换模组后整体颜色像底片，可尝试切换此参数。
- 当前横屏方向已按要求旋转 180°，使用 `ST7789_MADCTL=0xA0`。若需恢复原横屏方向，改回 `0x60`（BGR 模块用 `0x68`）。
- 画面偏移：此配置针对原生 240×320、零偏移面板；特殊模组需按厂商资料调整 X/Y OFFSET。

初始化命令参考 [Sitronix ST7789V 数据手册](https://dl.espressif.com/dl/schematics/ST7789V_SPEC_V1.0.pdf)。不同模组的电源/显示参数可能需按厂商初始化表微调。
