"""Serial frames used by the STM32 OSC firmware."""

from dataclasses import dataclass
import re


BAUD_RATE = 256_000
TIMER_HZ = 32_000_000
POINT_COUNT = 256
ZERO_CODES = (1817, 1816, 1803, 1786)
GET = b"<Get>"
FRAME_BYTES = 29 + POINT_COUNT * 4
WAVEFORM = re.compile(
    rb"<Time([0-9]{5})\+Fre([0-9]{5})\+Amp([0-3])\+(AC|DC)\+([0-9]{1024})>"
)


@dataclass(frozen=True, slots=True)
class Frame:
    time_ticks: int
    capture_ticks: int
    gain: int
    coupling: str
    codes: tuple[int, ...]

    @property
    def step_us(self) -> float:
        return self.time_ticks * 1_000_000 / TIMER_HZ


class FrameReader:
    """Accept partial frames, and resynchronize at the next opening delimiter."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes):
        self.buffer.extend(data)
        while self.buffer:
            start = self.buffer.find(b"<")
            if start == -1:
                self.buffer.clear()
                return
            del self.buffer[:start]
            end = self.buffer.find(b">")
            restart = self.buffer.find(b"<", 1)
            if restart != -1 and (end == -1 or restart < end):
                del self.buffer[:restart]
                continue
            if end == -1:
                if len(self.buffer) >= FRAME_BYTES:
                    self.buffer.clear()
                return

            packet = bytes(self.buffer[:end + 1])
            del self.buffer[:end + 1]
            if packet == GET:
                yield GET
                continue
            match = WAVEFORM.fullmatch(packet)
            if match is None:
                continue
            time, capture, gain, coupling, payload = match.groups()
            ticks = int(time)
            capture_ticks = int(capture)
            if not 1 <= ticks <= 65535 or capture_ticks > 65535:
                continue
            codes = tuple(int(payload[i:i + 4]) for i in range(0, len(payload), 4))
            if max(codes) <= 4095:
                yield Frame(ticks, capture_ticks, int(gain), coupling.decode("ascii"), codes)


def frequency_hz(frame: Frame) -> float | None:
    """Estimate frequency from rising midpoint crossings in the displayed frame."""
    low, high = min(frame.codes), max(frame.codes)
    amplitude = high - low
    if amplitude < 16:
        return None

    middle = (low + high) / 2
    rearm = middle - amplitude * 0.1
    armed = False
    crossings = []
    for index, (before, after) in enumerate(zip(frame.codes, frame.codes[1:])):
        if before < rearm:
            armed = True
        if armed and before < middle <= after:
            crossings.append(index + (middle - before) / (after - before))
            armed = False

    if len(crossings) < 2:
        return None
    cycles = len(crossings) - 1
    return cycles * TIMER_HZ / ((crossings[-1] - crossings[0]) * frame.time_ticks)
