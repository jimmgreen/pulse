from pathlib import Path
from PIL import Image,ImageChops
import sys
root=Path(sys.argv[1]);failed=0
for theme in ('light','dark'):
 images={s:Image.open(root/f'{theme}-{s}.png').convert('RGBA') for s in ('before','insert','cancel','blocked')}
 checks=[('cancel restores identical pixels', ImageChops.difference(images['before'],images['cancel']).getbbox(alpha_only=False) is None),
 ('insert changes preview',ImageChops.difference(images['before'],images['insert']).getbbox(alpha_only=False) is not None),
 ('unrelated fixed group stays identical',ImageChops.difference(images['before'].crop((690,60,845,170)),images['insert'].crop((690,60,845,170))).getbbox(alpha_only=False) is None),
 ('blocked target does not move source cards',ImageChops.difference(images['before'].crop((30,50,650,240)),images['blocked'].crop((30,50,650,240))).getbbox(alpha_only=False) is None)]
 for name,ok in checks:
  failed+=not ok;print(f"[{'PASS' if ok else 'FAIL'}] {theme}: {name}")
print(f'Reorder pixels: 8 cases, {failed} failures');raise SystemExit(bool(failed))

