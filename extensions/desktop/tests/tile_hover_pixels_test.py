"""Compare real renderer snapshots; ignore only the target's outer shadow."""
from pathlib import Path
import sys
from PIL import Image, ImageChops

root = Path(sys.argv[1])
failures = 0
for theme in ("light", "dark"):
    images = {name: Image.open(root / f"{theme}-{name}.png").convert("RGB")
              for name in ("idle", "hover_a", "hover_b", "loose", "clear")}
    for name, target in (("hover_a", 0), ("hover_b", 1)):
        for index in range(2):
            x = 24 + index * 340
            # Interior includes app highlight, excludes title/edit/outer elevation.
            rect = ((x + 10, 80, x + 290, 246) if index == target
                    else (x - 16, 8, x + 316, 280))
            delta = ImageChops.difference(images["idle"].crop(rect), images[name].crop(rect))
            pixels = delta.tobytes()
            changed = sum(pixels[i:i + 3] != b"\x00\x00\x00" for i in range(0, len(pixels), 3))
            passed = changed > 0 if index == target else changed == 0
            failures += not passed
            print(f"[{'PASS' if passed else 'FAIL'}] {theme} {name} tile {index}: changed_pixels={changed}")
    for name in ("loose", "clear"):
        changed = ImageChops.difference(images["idle"], images[name]).getbbox() is not None
        failures += changed
        print(f"[{'FAIL' if changed else 'PASS'}] {theme} {name}: identical_to_idle={not changed}")
print(f"RESULT: failures={failures}")
raise SystemExit(bool(failures))
