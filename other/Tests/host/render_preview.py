"""Package the actual headless LVGL framebuffer captures as PNG and GIF."""
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[2]
output = root / "Build"
with Image.open(output / "ecg_preview.bmp") as preview:
    preview.save(output / "ecg_preview.png")
frames = []
for path in sorted((output / "ecg_frames").glob("frame_*.bmp")):
    with Image.open(path) as image:
        frames.append(image.convert("RGB"))
assert len(frames) == 76, "Run Tests/host/run.ps1 first to record 3.04 seconds."
palette = frames[0].quantize(colors=32)
frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
frames[0].save(output / "ecg_preview.gif", save_all=True, append_images=frames[1:],
               duration=40, loop=0, disposal=2, optimize=False)
print(f"Saved {len(frames)} frames / 3.04 seconds: {output / 'ecg_preview.gif'}")
