from pathlib import Path
import sys
from PIL import Image, ImageChops

root = Path(sys.argv[1])
failures = 0
for theme in ("light", "dark"):
    for scenario in range(3):
        name = f"{theme}-{scenario}"
        partial = Image.open(root / f"{name}-partial.png").convert("RGBA")
        full = Image.open(root / f"{name}-full.png").convert("RGBA")
        delta = ImageChops.difference(partial, full)
        changed = any(band.getbbox() is not None for band in delta.split())
        failures += changed
        print(f"[{'FAIL' if changed else 'PASS'}] {name}: pixel_exact={not changed}")
print(f"RESULT: failures={failures}")
raise SystemExit(bool(failures))
