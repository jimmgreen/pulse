"""Exercise the renderer's actual Apps hover predicate without a desktop session."""
import pathlib
import re
import sys
from types import SimpleNamespace

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig")
match = re.search(r"else if \(([^\n]*interaction\.hovered_path == apps\[i\]->path)\)", source)
if not match:
    raise SystemExit("[FAIL] Apps hover predicate not found")
predicate = match.group(1).replace("&&", "and").replace("apps[i]->path", "app_path")
cases = [
    ("target Apps tile highlights matching item", "apps_a", "apps_a", "editor.exe", "editor.exe", True),
    ("second Apps tile does not highlight same item", "apps_b", "apps_a", "editor.exe", "editor.exe", False),
    ("third Apps tile does not highlight same item", "apps_c", "apps_a", "editor.exe", "editor.exe", False),
    ("different item stays idle", "apps_a", "apps_a", "editor.exe", "terminal.exe", False),
    ("loose desktop hover does not highlight Apps tile", "apps_a", "", "editor.exe", "editor.exe", False),
    ("switching tiles clears previous tile", "apps_a", "apps_b", "editor.exe", "editor.exe", False),
    ("switching tiles highlights new tile", "apps_b", "apps_b", "editor.exe", "editor.exe", True),
    ("leaving tile clears hover", "apps_a", "", "", "editor.exe", False),
    ("stack owner id highlights active Apps member", "stack_owner", "stack_owner", "editor.exe", "editor.exe", True),
]
failures = 0
for name, tile_id, hovered_tile_id, hovered_path, app_path, expected in cases:
    interaction = SimpleNamespace(hovered_path=hovered_path)
    actual = eval(predicate, {"__builtins__": {}}, {
        "hovered": tile_id == hovered_tile_id,
        "interaction": interaction,
        "app_path": app_path,
    })
    passed = actual == expected
    failures += not passed
    print(f"[{'PASS' if passed else 'FAIL'}] {name}: actual={actual} expected={expected}")
print(f"RESULT: {len(cases) - failures}/{len(cases)} passed")
raise SystemExit(bool(failures))
