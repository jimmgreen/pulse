"""Render the 7-Zip archive icon styles from archive_icon_styles_mockup.html into .ico files.

Each style becomes <ext>.ico under extensions/7zip/icons (「文档」 in the folder itself, the others in
icons/<style>/), with frames 16-256 px; 16/20/24 px use the simplified art. Requires Edge and Pillow.

    python tools/generate_7zip_archive_icons.py [doc] [folder] [color]
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MOCKUP = ROOT / "archive_icon_styles_mockup.html"
STYLES = ("doc", "folder", "color")
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 256]
NAMES = ["zip", "zipx", "7z", "rar", "tar", "gz", "xz", "bz2", "lzma", "z", "cab", "lzh", "lha", "arj", "cpio"]
ROW = 264


def render(style: str, work: Path) -> Image.Image:
    edge = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft/Edge/Application/msedge.exe"
    cols, x = [], 0
    for size in SIZES:
        cols.append(x)
        x += size + 8
    width, height = x, ROW * len(NAMES)
    png = work / f"{style}.png"
    url = MOCKUP.as_uri() + f"?export={style}"
    result = subprocess.run([str(edge), "--headless", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
        "--default-background-color=00000000", "--force-device-scale-factor=1", f"--window-size={width},{height}",
        "--virtual-time-budget=3000", "--user-data-dir=" + str(work / "browser"), "--screenshot=" + str(png), url],
        capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode or not png.exists():
        raise RuntimeError(result.stderr.decode(errors="replace"))
    sheet = Image.open(png).convert("RGBA")
    if sheet.size != (width, height) or sheet.getpixel((width - 1, height - 1))[3] != 0:
        raise RuntimeError(f"{style}: unexpected sheet {sheet.size} or opaque background")
    return sheet, cols


def main(styles):
    with tempfile.TemporaryDirectory(prefix="pulse-7zip-icons-") as tmp:
        work = Path(tmp)
        for style in styles:
            sheet, cols = render(style, work)
            out_dir = ROOT / "extensions/7zip/icons" / ("" if style == "doc" else style)
            out_dir.mkdir(parents=True, exist_ok=True)
            for row, name in enumerate(NAMES):
                frames = []
                for size, left in zip(SIZES, cols):
                    frame = sheet.crop((left, row * ROW, left + size, row * ROW + size))
                    bounds = frame.getchannel("A").getbbox()
                    if not bounds:
                        raise RuntimeError(f"{style}/{name}: empty {size}px frame")
                    frames.append(frame)
                path = out_dir / f"{name}.ico"
                frames[-1].save(path, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
                check = Image.open(path)
                got = sorted(check.info["sizes"])
                if got != sorted((s, s) for s in SIZES):
                    raise RuntimeError(f"{path}: frames {got}")
                # The 16 px frame must be the drawn small art, not a downscale of 256.
                if check.ico.getimage((16, 16)).tobytes() != frames[0].tobytes():
                    raise RuntimeError(f"{path}: 16 px frame differs from the rendered one")
            print(f"{style}: {len(NAMES)} icons, frames {SIZES} -> {out_dir.relative_to(ROOT)}")


if __name__ == "__main__":
    picked = [s for s in sys.argv[1:] if s in STYLES] or list(STYLES)
    main(picked)
