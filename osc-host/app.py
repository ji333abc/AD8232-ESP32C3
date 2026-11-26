import argparse
from collections import deque
import math
from queue import Empty, SimpleQueue
from threading import Event, Thread
import time
import tkinter as tk
from tkinter import ttk

from protocol import BAUD_RATE, GET, POINT_COUNT, ZERO_CODES, FrameReader, frequency_hz

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


BACKGROUND = "#0d141b"
PANEL = "#151f28"
GRID = "#253743"
TEXT = "#e5eef5"
MUTED = "#9bb0bd"
TRACE = "#57edbb"
CURSOR_COLORS = ("#f5bc6a", "#91b8ff")
UI_FONT = ("Microsoft YaHei UI", 10)
MONO = ("Consolas", 10)


class SerialSession:
    def __init__(self, port):
        self.port = port
        self.started_at = time.monotonic()
        self.stop = Event()
        self.done = Event()
        self.commands = SimpleQueue()
        self.frames = deque(maxlen=1)
        self.events = SimpleQueue()
        Thread(target=self.run, daemon=True).start()

    def run(self):
        reader = FrameReader()
        link = None
        try:
            link = serial.Serial(self.port, BAUD_RATE, timeout=0.05, write_timeout=0.1)
            if self.stop.is_set():
                return
            self.events.put(("opened", ""))
            while not self.stop.is_set():
                first = link.read(1)
                if not first:
                    continue
                data = first + link.read(min(link.in_waiting, 4096))
                for item in reader.feed(data):
                    if self.stop.is_set():
                        return
                    if item != GET:
                        self.frames.append(item)
                        continue
                    # A Get opens the firmware's 20 ms command window.
                    pending = {}
                    while True:
                        try:
                            pending.update(self.commands.get_nowait())
                        except Empty:
                            break
                    if pending:
                        auto = pending.pop("Auto", None)
                        packet = b"".join(f"<{key}{value}>".encode("ascii") for key, value in pending.items())
                        link.write(packet + (b"<Auto>" if auto is not None else b""))
        except (OSError, ValueError) as error:
            if not self.stop.is_set():
                self.events.put(("error", str(error)))
        finally:
            try:
                if link is not None:
                    link.close()
            except OSError as error:
                self.events.put(("error", str(error)))
            finally:
                self.done.set()


class TracePlot(tk.Canvas):
    def __init__(self, parent, on_change):
        super().__init__(parent, background=PANEL, highlightthickness=0, width=520, height=330)
        self.on_change = on_change
        self.values = ()
        self.step = 0
        self.unit = "码"
        self.cursors = [0.23, 0.64]
        self.enabled = True
        self.active = 0
        self.area = (76, 26, 480, 286)
        self.bind("<Configure>", lambda event: self.draw())
        self.bind("<Button-1>", self.grab_cursor)
        self.bind("<B1-Motion>", self.move_cursor)

    def indices(self):
        return [round(position * (len(self.values) - 1)) for position in self.cursors]

    def grab_cursor(self, event):
        if self.values and self.enabled:
            left, _, right, _ = self.area
            position = (event.x - left) / (right - left)
            self.active = min(range(2), key=lambda index: abs(self.cursors[index] - position))
            self.move_cursor(event)

    def move_cursor(self, event):
        if self.values and self.enabled:
            left, _, right, _ = self.area
            self.cursors[self.active] = min(1, max(0, (event.x - left) / (right - left)))
            self.draw()
            self.on_change()

    def draw(self):
        self.delete("all")
        width, height = self.winfo_width(), self.winfo_height()
        if width < 160 or height < 100:
            return
        left, top, right, bottom = 76, 31, width - 28, height - 48
        self.area = left, top, right, bottom
        self.create_rectangle(left, top, right, bottom, fill=BACKGROUND, outline=GRID)
        if self.values:
            low, high = min(self.values), max(self.values)
            padding = max((high - low) * 0.12, 0.001 if self.unit == "V" else 8)
            low, high = low - padding, high + padding
        else:
            low, high = 0, 4095

        def y_at(value):
            return bottom - (value - low) / (high - low) * (bottom - top)

        self.create_text(left, 13, text="输入电压 (V)" if self.unit == "V" else "ADC 原始码", anchor="w", fill=MUTED, font=UI_FONT)
        for division in range(9):
            y = top + (bottom - top) * division / 8
            self.create_line(left, y, right, y, fill=GRID)
            if division % 2 == 0:
                value = high - (high - low) * division / 8
                self.create_text(left - 10, y, text=f"{value:.4g}", anchor="e", fill=MUTED, font=MONO)

        span_us = (len(self.values) - 1) * self.step if self.values else 0
        time_scale, time_unit = (1000, "ms") if span_us >= 1000 else (1, "µs")
        for division in range(11):
            x = left + (right - left) * division / 10
            self.create_line(x, top, x, bottom, fill=GRID)
            if self.values and division % 2 == 0:
                self.create_text(x, bottom + 16, text=f"{span_us * division / (10 * time_scale):.3g}", fill=MUTED, font=MONO)
        self.create_text(right, bottom + 36, text=f"时间 ({time_unit})", anchor="e", fill=MUTED, font=UI_FONT)

        if not self.values:
            self.create_text((left + right) / 2, (top + bottom) / 2, text="等待波形", fill=MUTED, font=UI_FONT)
            return

        def x_at(index):
            return left + index * (right - left) / (len(self.values) - 1)

        points = [coordinate for index, value in enumerate(self.values) for coordinate in (x_at(index), y_at(value))]
        self.create_line(points, fill=TRACE, width=2)
        if self.enabled:
            for name, index, color in zip("AB", self.indices(), CURSOR_COLORS):
                x, y = x_at(index), y_at(self.values[index])
                self.create_line(x, top, x, bottom, fill=color, dash=(4, 4))
                self.create_oval(x - 4, y - 4, x + 4, y + 4, fill=color, outline=PANEL)
                self.create_text(x, top - 9, text=name, fill=color, font=MONO)


class ScopeApp:
    def __init__(self, root):
        self.root = root
        root.title("OSC 示波器上位机")
        root.geometry("1080x690")
        root.configure(background=BACKGROUND)
        root.protocol("WM_DELETE_WINDOW", self.close)

        self.session = None
        self.connected = False
        self.frame = self.latest = None
        self.calibration = [None] * 4
        self.demo = False
        self.demo_at = 0
        self.demo_reader = FrameReader()
        self.frozen = False
        self.port = tk.StringVar()
        self.status = tk.StringVar(value="未连接")
        self.gain = tk.StringVar(value="0")
        self.coupling = tk.StringVar(value="DC")
        self.ticks = tk.StringVar(value="320")
        self.scale = tk.StringVar()
        self.capture = tk.StringVar(value="—")
        self.cal_status = tk.StringVar()
        self.cursor_status = tk.StringVar(value="—")
        self.cursor_enabled = tk.BooleanVar(value=True)

        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background=BACKGROUND)
        style.configure("Panel.TFrame", background=PANEL)
        style.configure("TLabel", background=BACKGROUND, foreground=TEXT, font=UI_FONT)
        style.configure("Muted.TLabel", foreground=MUTED)
        style.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        style.configure("PanelMuted.TLabel", background=PANEL, foreground=MUTED)
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 22, "bold"))
        style.configure("Metric.TLabel", background=PANEL, foreground=TEXT, font=("Consolas", 16, "bold"))
        style.configure("TButton", background=GRID, foreground=TEXT, padding=(9, 6), font=UI_FONT)
        style.map("TButton", background=[("active", "#36515e")], foreground=[("disabled", "#718692")])
        style.configure("Connect.TButton", background=TRACE, foreground=BACKGROUND)
        style.map("Connect.TButton", background=[("active", "#93ffda")])
        style.configure("TCheckbutton", background=PANEL, foreground=TEXT, font=UI_FONT)
        style.map("TCheckbutton", background=[("active", PANEL)])

        content = ttk.Frame(root, padding=(20, 16))
        content.pack(fill="both", expand=True)
        header = ttk.Frame(content)
        header.pack(fill="x", pady=(0, 14))
        ttk.Label(header, text="OSC 示波器", style="Title.TLabel").pack(side="left")
        ttk.Label(header, textvariable=self.status, style="Muted.TLabel").pack(side="right")

        bar = ttk.Frame(content)
        bar.pack(fill="x", pady=(0, 12))
        ttk.Label(bar, text="串口").pack(side="left", padx=(0, 8))
        self.ports = ttk.Combobox(bar, textvariable=self.port, width=10, font=MONO)
        self.ports.pack(side="left", padx=(0, 6))
        ttk.Button(bar, text="刷新", command=self.refresh_ports).pack(side="left", padx=(0, 6))
        self.connect_button = ttk.Button(bar, text="连接", command=self.connect, style="Connect.TButton")
        self.connect_button.pack(side="left", padx=(0, 12))
        ttk.Label(bar, text="256000 · 8N1", style="Muted.TLabel", font=MONO).pack(side="left")
        self.pause_button = ttk.Button(bar, text="暂停显示", command=self.toggle_pause)
        self.pause_button.pack(side="right")
        self.demo_button = ttk.Button(bar, text="演示", command=self.toggle_demo)
        self.demo_button.pack(side="right", padx=(0, 6))

        body = ttk.Frame(content)
        body.pack(fill="both", expand=True)
        controls = ttk.Frame(body, style="Panel.TFrame", padding=(14, 12))
        controls.pack(side="right", fill="y", padx=(12, 0))
        self.plot = TracePlot(body, self.update_cursors)
        self.plot.pack(side="left", fill="both", expand=True)

        self.panel_label(controls, "采集设置", bold=True)
        self.panel_label(controls, "采样计数（16–65535）", muted=True)
        ttk.Entry(controls, textvariable=self.ticks, width=22).pack(fill="x", pady=(2, 6))
        self.panel_label(controls, "增益档", muted=True)
        self.gain_box = ttk.Combobox(controls, textvariable=self.gain, values=("0", "1", "2", "3"), state="readonly", width=20)
        self.gain_box.pack(fill="x", pady=(2, 6))
        self.gain_box.bind("<<ComboboxSelected>>", lambda event: self.show_calibration())
        self.panel_label(controls, "耦合", muted=True)
        ttk.Combobox(controls, textvariable=self.coupling, values=("DC", "AC"), state="readonly", width=20).pack(fill="x", pady=(2, 6))
        actions = ttk.Frame(controls, style="Panel.TFrame")
        actions.pack(fill="x", pady=(1, 8))
        self.apply_button = ttk.Button(actions, text="应用", command=self.apply_settings)
        self.apply_button.pack(side="left", fill="x", expand=True, padx=(0, 4))
        self.auto_button = ttk.Button(actions, text="自动", command=self.auto)
        self.auto_button.pack(side="right", fill="x", expand=True)
        ttk.Label(controls, textvariable=self.capture, style="PanelMuted.TLabel").pack(anchor="w")

        ttk.Separator(controls).pack(fill="x", pady=9)
        self.panel_label(controls, "所选档位的电压标定", bold=True)
        self.panel_label(controls, "斜率 (V/码)，留空使用原始码", muted=True)
        ttk.Entry(controls, textvariable=self.scale, width=22).pack(fill="x", pady=(2, 4))
        ttk.Button(controls, text="应用标定", command=self.set_calibration).pack(fill="x")
        ttk.Label(controls, textvariable=self.cal_status, style="PanelMuted.TLabel").pack(anchor="w", pady=(3, 0))

        metrics = ttk.Frame(content)
        metrics.pack(fill="x", pady=(12, 0))
        metrics.columnconfigure((0, 1, 2, 3), weight=1, uniform="metric")
        self.metrics = {label: tk.StringVar(value="—") for label in ("峰峰值", "采样间隔", "波形频率（估算）")}
        for index, (label, variable) in enumerate(self.metrics.items()):
            tile = ttk.Frame(metrics, padding=(12, 8), style="Panel.TFrame")
            tile.grid(row=0, column=index, sticky="nsew", padx=(0, 8))
            ttk.Label(tile, text=label, style="PanelMuted.TLabel").pack(anchor="w")
            ttk.Label(tile, textvariable=variable, style="Metric.TLabel").pack(anchor="w")
        tile = ttk.Frame(metrics, padding=(12, 8), style="Panel.TFrame")
        tile.grid(row=0, column=3, sticky="nsew")
        ttk.Checkbutton(tile, text="双游标", variable=self.cursor_enabled, command=self.toggle_cursors).pack(anchor="w")
        ttk.Label(tile, textvariable=self.cursor_status, style="Panel.TLabel").pack(anchor="w")

        self.refresh_ports()
        self.show_calibration()
        self.sync_buttons()
        root.update_idletasks()
        root.minsize(max(870, root.winfo_reqwidth()), root.winfo_reqheight())
        self.tick()

    def panel_label(self, parent, text, *, muted=False, bold=False):
        style = "PanelMuted.TLabel" if muted else "Panel.TLabel"
        font = ("Microsoft YaHei UI", 10, "bold") if bold else UI_FONT
        ttk.Label(parent, text=text, style=style, font=font).pack(anchor="w", pady=(0, 1))

    def refresh_ports(self):
        if list_ports is None:
            return
        devices = [port.device for port in sorted(list_ports.comports())]
        self.ports.configure(values=devices)
        if devices and not self.port.get():
            self.port.set(devices[0])

    def sync_buttons(self):
        if self.session is None:
            self.connect_button.configure(text="连接", state="normal")
        elif self.session.stop.is_set():
            self.connect_button.configure(text="正在释放", state="disabled")
        else:
            self.connect_button.configure(text="断开" if self.connected else "取消连接", state="normal")
        self.demo_button.configure(text="退出演示" if self.demo else "演示")
        self.pause_button.configure(
            text="继续显示" if self.frozen else "暂停显示",
            state="normal" if self.connected or self.demo else "disabled",
        )
        for button in (self.apply_button, self.auto_button):
            button.configure(state="normal" if self.connected else "disabled")

    def clear_trace(self):
        self.latest = None
        self.frozen = False
        self.show_frame(None)
        self.sync_buttons()

    def connect(self):
        if self.session:
            if not self.session.stop.is_set():
                self.disconnect(f"正在释放 {self.session.port}…")
            return
        if serial is None:
            self.status.set("串口功能需要安装 pySerial")
            return
        port = self.port.get().strip()
        if not port:
            self.status.set("请选择串口")
            return
        self.demo = False
        self.session = SerialSession(port)
        self.clear_trace()
        self.status.set(f"正在打开 {port}…")

    def disconnect(self, status="未连接"):
        if self.session:
            self.session.stop.set()
        self.connected = False
        self.clear_trace()
        self.status.set(status)

    def toggle_demo(self):
        self.demo = not self.demo
        self.disconnect("演示 · 1 kHz 正弦波" if self.demo else "未连接")
        self.demo_at = 0

    def toggle_pause(self):
        if not (self.demo or self.connected):
            return
        self.frozen = not self.frozen
        self.sync_buttons()
        if not self.frozen:
            self.show_frame(self.latest)

    def toggle_cursors(self):
        self.plot.enabled = self.cursor_enabled.get()
        self.plot.draw()
        self.update_cursors()

    def apply_settings(self):
        if self.connected:
            try:
                ticks = int(self.ticks.get())
            except ValueError:
                ticks = 0
            if not 16 <= ticks <= 65535:
                self.status.set("请输入 16–65535 之间的采样计数")
                return
            self.session.commands.put({"Time": ticks, "Amp": self.gain.get(), "Coup": self.coupling.get()})
            self.status.set("采集设置已排队 · 等待 <Get>")

    def auto(self):
        if self.connected:
            self.session.commands.put({"Auto": ""})
            self.status.set("自动设置已排队 · 等待 <Get>")

    def show_calibration(self):
        gain = int(self.gain.get())
        scale = self.calibration[gain]
        self.scale.set("" if scale is None else f"{scale:.10g}")
        self.cal_status.set(f"档 {gain} 的零点为 {ZERO_CODES[gain]} 码")

    def set_calibration(self):
        text = self.scale.get().strip()
        try:
            scale = float(text) if text else None
            if scale is not None and (not math.isfinite(scale) or scale == 0 or abs(scale) > 1000):
                raise ValueError
        except ValueError:
            self.cal_status.set("请输入有效的非零斜率")
            return
        self.calibration[int(self.gain.get())] = scale
        self.show_calibration()
        self.show_frame(self.frame)

    def poll_serial(self):
        session = self.session
        if session is None:
            return None
        # Once done is set, no more events can be posted by this worker.
        done = session.done.is_set()
        while True:
            try:
                event, detail = session.events.get_nowait()
            except Empty:
                break
            if session.stop.is_set():
                continue
            if event == "opened":
                self.connected = True
                self.sync_buttons()
                self.status.set(f"{session.port} 已连接 · 等待波形")
            else:
                message = "串口连接中断" if self.connected else "打开失败"
                self.disconnect(f"{session.port} {message} · {detail}")

        if done:
            self.session = None
            self.connected = False
            if self.status.get() == f"正在释放 {session.port}…":
                self.status.set("未连接")
            self.sync_buttons()
            return None
        if session.stop.is_set():
            return None
        if not self.connected:
            if time.monotonic() - session.started_at >= 3:
                self.status.set(f"{session.port} 打开耗时较长 · 可取消连接")
            return None

        try:
            frame = session.frames.pop()
        except IndexError:
            return None
        if self.latest is None:
            self.status.set(f"{session.port} 已连接")
        elif self.status.get().endswith("等待 <Get>"):
            self.status.set(f"{session.port} 已连接 · 以当前帧参数为准")
        return frame

    def tick(self):
        frame = self.poll_serial()
        if self.demo and time.monotonic() >= self.demo_at:
            self.demo_at = time.monotonic() + 0.12
            points = [1817 + round(800 * math.sin(math.tau * index / 100)) for index in range(POINT_COUNT)]
            payload = "".join(f"{point:04d}" for point in points)
            frame = next(self.demo_reader.feed(f"<Time00320+Fre32000+Amp0+DC+{payload}>".encode("ascii")))

        if frame is not None:
            self.latest = frame
            if not self.frozen:
                self.show_frame(frame)
        self.root.after(25, self.tick)

    def show_frame(self, frame):
        self.frame = frame
        if frame is None:
            self.plot.values = ()
            self.plot.unit = "码"
            self.capture.set("—")
            for metric in self.metrics.values():
                metric.set("—")
        else:
            scale = self.calibration[frame.gain]
            zero = ZERO_CODES[frame.gain]
            self.plot.values = tuple((code - zero) * scale for code in frame.codes) if scale is not None else frame.codes
            self.plot.unit = "V" if scale is not None else "码"
            self.plot.step = frame.step_us
            self.capture.set(f"当前帧  {frame.time_ticks} · 档 {frame.gain} · {frame.coupling}")
            self.metrics["峰峰值"].set(f"{max(self.plot.values) - min(self.plot.values):.5g} {self.plot.unit}")
            self.metrics["采样间隔"].set(f"{frame.step_us:.6g} µs")
            hz = frequency_hz(frame)
            self.metrics["波形频率（估算）"].set(f"{hz:.5g} Hz" if hz is not None else "—")
        self.plot.draw()
        self.update_cursors()

    def update_cursors(self):
        if not self.frame or not self.plot.enabled:
            self.cursor_status.set("—")
            return
        a, b = self.plot.indices()
        delta_time = abs(b - a) * self.frame.step_us
        delta_value = self.plot.values[b] - self.plot.values[a]
        self.cursor_status.set(f"Δt {delta_time:.5g} µs · Δy {delta_value:+.5g} {self.plot.unit}")

    def close(self):
        if self.session:
            self.session.stop.set()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="STM32 OSC 示波器上位机")
    parser.add_argument("--demo", action="store_true", help="启动时显示模拟波形")
    args = parser.parse_args()
    root = tk.Tk()
    app = ScopeApp(root)
    if args.demo:
        app.toggle_demo()
    root.mainloop()


if __name__ == "__main__":
    main()
