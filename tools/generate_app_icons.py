"""Render the approved SVG into large-artwork Windows icons. Requires Edge and Pillow."""
from pathlib import Path
import os
import subprocess
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 20, 24, 28, 32, 40, 48, 64, 96, 128, 256)

def main():
    edge = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft/Edge/Application/msedge.exe"
    with tempfile.TemporaryDirectory(prefix="pulse-icon-") as tmp:
        work = Path(tmp)
        svg = (ROOT / "assets/pulse-logo.svg").read_text(encoding="utf-8")
        page = work / "render.html"
        page.write_text('<style>html,body{margin:0;background:transparent}svg{display:block;width:1024px;height:1024px}</style>' + svg, encoding="utf-8")
        png = work / "master.png"
        result = subprocess.run([str(edge), "--headless", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
            "--default-background-color=00000000", "--force-device-scale-factor=1", "--window-size=1024,1024",
            "--user-data-dir=" + str(work / "browser"), "--screenshot=" + str(png), page.as_uri()],
            capture_output=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode or not png.exists():
            raise RuntimeError(result.stderr.decode(errors="replace"))
        master = Image.open(png).convert("RGBA")
        assert master.size == (1024, 1024) and master.getpixel((0, 0))[3] == 0
        master.save(ROOT / "assets/pulse-logo.png")
        master.save(ROOT / "src/app/pulse.ico", format="ICO", sizes=[(n,n) for n in SIZES])
        master.resize((128,128), Image.Resampling.LANCZOS).save(ROOT / "assets/global-search-logo.png")
        print("Generated transparent PNG and ICO frames:", SIZES)
        for n in SIZES:
            icon = Image.open(ROOT / "src/app/pulse.ico").ico.getimage((n,n))
            bounds = icon.getchannel("A").point(lambda x: 255 if x >= 128 else 0).getbbox()
            print(f"{n}px artwork: {bounds}")

if __name__ == "__main__":
    main()
