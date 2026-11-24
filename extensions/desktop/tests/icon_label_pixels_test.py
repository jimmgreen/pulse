from pathlib import Path
from PIL import Image, ImageChops
import sys

root = Path(sys.argv[1])
failures = 0
def check(ok, name):
    global failures
    failures += not ok
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")

for theme in ('light', 'dark'):
    for surface in ('loose', 'group', 'apps'):
        def frame(style, mode='idle'):
            return Image.open(root / f'{theme}-{surface}-{style}-{mode}.png').convert('RGB')
        for a, b in ((0, 1), (0, 2), (1, 2)):
            check(ImageChops.difference(frame(a), frame(b)).getbbox() is not None,
                  f'{theme} {surface}: styles {a}/{b} render differently')
        check(ImageChops.difference(frame(0, 'hover'), frame(2, 'hover')).getbbox() is None,
              f'{theme} {surface}: hidden label reappears on hover')
        if surface == 'apps':
            check(ImageChops.difference(frame(2), frame(2, 'other')).getbbox() is None,
                  f'{theme} apps: another tile hover does not reveal label')
print(f'RESULT: failures={failures}')
raise SystemExit(bool(failures))
