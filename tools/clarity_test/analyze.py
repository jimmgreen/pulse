"""Quantify edge spread without declaring a subjective readability winner."""
import json
from pathlib import Path
import sys
from PIL import Image

root = Path(sys.argv[1])
rows = [json.loads(line) for line in (root / 'rows.jsonl').read_text(encoding='utf-8').splitlines()]
assert len(rows) == 144
results = []
images = {}
for row in rows:
    name = f"{int(row['dip'])}dip-{round(row['scale'] * 100)}-{'dark' if row['dark'] else 'light'}.png"
    image = images.setdefault(name, Image.open(root / name).convert('RGB'))
    top = row['rect_y']
    bg, fg = (32, 242) if row['dark'] else (255, 32)
    stats = []
    for col in range(4):
        # Include all 64px of host clip and its .5px phase without including adjacent rows.
        crop = image.crop((col * 850 + 8, top, (col + 1) * 850 - 8, top + 66))
        coverage = [max(0., min(1., (rgb[0] - bg) / (fg - bg))) for rgb in crop.getdata()]
        ink = sum(v > .01 for v in coverage)
        partial = sum(.01 < v < .99 for v in coverage)
        core = sum(v >= .95 for v in coverage)
        mass = sum(coverage)
        assert ink > 20 and mass > 10, (name, row['row'], col, 'empty text')
        assert all(r == g == b for r, g, b in crop.getdata()), 'unexpected chromatic pixels'
        # No ink at left/right/bottom crop edges; clipping comparisons would be invalid.
        w, h = crop.size
        assert all(coverage[y*w] < .01 and coverage[y*w+w-1] < .01 for y in range(h)), 'horizontal clipping'
        assert max(coverage[-w:]) < .01, 'bottom clipping'
        stats.append({'ink_pixels': ink, 'partial_pixels': partial, 'core_pixels': core, 'coverage_mass': round(mass, 3),
                      'partial_per_mass': round(partial/mass, 4)})
    assert stats[2] != stats[3], (name, row['row'], 'filter toggle had no observable effect')
    result = dict(row, image=name, columns=stats)
    result['direct_partial_change_pct'] = round(100*(stats[3]['partial_pixels']/stats[2]['partial_pixels']-1), 2)
    result['direct_mass_change_pct'] = round(100*(stats[3]['coverage_mass']/stats[2]['coverage_mass']-1), 2)
    results.append(result)
summary = []
for px in sorted(set(r['physical_em'] for r in results)):
    group = [r for r in results if r['physical_em'] == px]
    m = sum(r['columns'][2]['partial_pixels'] for r in group)
    d = sum(r['columns'][3]['partial_pixels'] for r in group)
    mm = sum(r['columns'][2]['coverage_mass'] for r in group)
    dm = sum(r['columns'][3]['coverage_mass'] for r in group)
    summary.append({'physical_em': px, 'rows': len(group), 'direct_partial_change_pct': round(100*(d/m-1),2),
                    'direct_mass_change_pct': round(100*(dm/mm-1),2)})
(root/'metrics.json').write_text(json.dumps({'summary':summary,'rows':results},ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(summary,indent=2))
print('PASS 144 nonempty unclipped grayscale rows; distinct Mitchell/Direct outputs; metrics saved')
