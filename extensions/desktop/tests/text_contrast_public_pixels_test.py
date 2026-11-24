from pathlib import Path
from PIL import Image
import sys
root = Path(sys.argv[1])
failures = 0
colors = {'black': (0,0,0), 'white': (255,255,255), 'gray': (128,128,128), 'green': (0,255,0), 'blue': (0,0,255)}
def lum(rgb):
    v = [c / 255 for c in rgb]
    v = [c/12.92 if c <= .04045 else ((c+.055)/1.055)**2.4 for c in v]
    return sum(c*w for c,w in zip(v,(.2126,.7152,.0722)))
def contrast(a,b):
    a,b=lum(a),lum(b)
    return (max(a,b)+.05)/(min(a,b)+.05)
for name,bg in colors.items():
    ink = max(((255,255,255),(27,32,40)), key=lambda c: contrast(c,bg))
    for theme in ('light','dark'):
        im=Image.open(root/f'public-{name}-{theme}.png').convert('RGB')
        region=im.crop((70,95,170,120))
        pixels=[region.getpixel((x,y)) for y in range(region.height) for x in range(region.width)]
        count=sum((min(pixel)>235) if ink==(255,255,255) else
                  all(abs(a-b)<=2 for a,b in zip(pixel,ink)) for pixel in pixels)
        ok=count>=10
        failures+=not ok
        print(f"[{'PASS' if ok else 'FAIL'}] {name}-{theme}: selected={ink} glyph_pixels={count}")
print(f'Public contrast pixels: 10 cases, {failures} failures')
raise SystemExit(bool(failures))
