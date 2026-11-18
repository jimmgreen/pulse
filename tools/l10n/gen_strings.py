#!/usr/bin/env python3
"""Single source of truth for Pulse's UI strings.

    tools/l10n/catalog.tsv columns (tab separated):
        name     StringId enum name (unique, e.g. SettingsGeneral)
        id_name  the IDS_* macro name (unique, e.g. IDS_SETTINGS_GENERAL)
        id       numeric resource id (unique; never renumber existing rows)
        zh       Simplified Chinese text (zh-CN)
        en       English text (en-US)
        tw       Traditional Chinese text (zh-TW); empty = generate with OpenCC

    Lines starting with '#' are comments (kept in string_ids.h near their row).

Commands:
    python tools/l10n/gen_strings.py extract   # bootstrap catalog.tsv from the
                                               # current string_ids.h / enum / .rcinc
    python tools/l10n/gen_strings.py           # regenerate every output from the catalog
    python tools/l10n/gen_strings.py --check   # exit 1 when an output is stale
    python tools/l10n/gen_strings.py parity    # compare outputs against the snapshot
                                               # written by extract (cutover safety net)

Outputs (all generated; do not edit by hand):
    src/common/string_ids.h        IDS_* defines
    src/common/localization.h      the StringId enum body only
    src/common/strings.rcinc       zh-CN and en-US STRINGTABLEs
    src/common/strings_zh_tw.rcinc zh-TW STRINGTABLE
    src/common/zh_hant_table.inc   runtime Simplified->Traditional phrase/char tables
    src/setup/setup_strings.generated.inc standalone setup language table

The zh-TW text is taken from the catalog when present; otherwise it is computed
from `zh` with OpenCC s2twp protected by tools/zh_tw/glossary.tsv (the same
conversion the zh_hant.cpp runtime tables use). A new language needs one more
column and one more output file; catalog.tsv stays the only hand-edited file.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from opencc import OpenCC

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CATALOG = HERE / "catalog.tsv"
SNAPSHOT = ROOT / "build/agent_tmp/l10n_pre.json"

IDS_H = ROOT / "src/common/string_ids.h"
LOC_H = ROOT / "src/common/localization.h"
RCINC = ROOT / "src/common/strings.rcinc"
RCINC_TW = ROOT / "src/common/strings_zh_tw.rcinc"
TABLE = ROOT / "src/common/zh_hant_table.inc"

# ------------------------------------------------------------ zh conversion
_cc = OpenCC("s2twp")
_cc_chars = OpenCC("s2t")
QUOTES = str.maketrans({"\u201c": "\u300c", "\u201d": "\u300d", "\u2018": "\u300e", "\u2019": "\u300f"})


def load_tsv(path: Path):
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, value = line.split("\t", 1)
        rows.append((key, value))
    return rows


GLOSSARY = sorted(load_tsv(ROOT / "tools/zh_tw/glossary.tsv"), key=lambda kv: -len(kv[0]))


def to_hant(text: str) -> str:
    """Simplified -> Traditional (Taiwan), Pulse glossary protected."""
    protected = []
    for simplified, traditional in GLOSSARY:
        if simplified in text:
            marker = chr(0xE000 + len(protected))
            protected.append(traditional)
            text = text.replace(simplified, marker)
    text = _cc.convert(text)
    for i, traditional in enumerate(protected):
        text = text.replace(chr(0xE000 + i), traditional)
    return text.translate(QUOTES)


# ------------------------------------------------------------ literal scan
HAN = re.compile(r"[\u3400-\u9fff]")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|(L?)"((?:[^"\\\n]|\\.)*)"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
EXCLUDED = (
    "src/bench/",
    "src/ipc/ctx_menu_util.h",
    "src/index/search_kinds.cpp",
    "src/index/index_query.cpp",
    "src/app/entry_group.cpp",
    "src/app/selftest_1b2.cpp",
    "src/setup/setup_strings.cpp",  # setup table is generated from the same catalog
)
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'", "?": "?"}


def decode_cpp(body: str) -> str:
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n in "uU":
            width = 4 if n == "u" else 8
            out.append(chr(int(body[i + 2:i + 2 + width], 16)))
            i += 2 + width
        elif n == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(chr(int(body[i + 2:j], 16)))
            i = j
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    return "".join(out)


def cpp_literal(text: str) -> str:
    out = []
    for c in text:
        o = ord(c)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif 0x20 <= o < 0x7F:
            out.append(c)
        else:
            if o > 0xFFFF:
                sys.exit(f"non-BMP character in UI literal: {text!r}")
            out.append(f"\\u{o:04x}")
    return 'L"' + "".join(out) + '"'


def source_literals():
    """Wide string literals with Han characters; adjacent literals are joined."""
    found = set()
    for path in sorted((ROOT / "src").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix not in (".cpp", ".h") or rel.startswith(EXCLUDED):
            continue
        if path.name.endswith(("_test.cpp", "_test.h")):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        pending, pending_end = None, 0

        def flush():
            if pending is not None and HAN.search(pending):
                found.add(pending)

        for m in TOKEN.finditer(text):
            if m.group(2) is not None and m.group(1) == "L":
                value = decode_cpp(m.group(2))
                if pending is not None and not text[pending_end:m.start()].strip():
                    pending += value
                else:
                    flush()
                    pending = value
                pending_end = m.end()
            else:
                flush()
                pending = None
        flush()
    return found


def vocabulary():
    words = {k for k, _ in GLOSSARY}
    for line in (ROOT / "tools/zh_tw/vocabulary.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            words.add(line)
    return words


def generate_table(catalog_zh) -> str:
    phrases = {}
    for key in source_literals() | set(catalog_zh) | vocabulary():
        value = to_hant(key)
        if value != key:
            phrases[key] = value
    chars = {}
    for cp in range(0x4E00, 0xA000):
        c = chr(cp)
        t = _cc_chars.convert(c)
        if len(t) == 1 and t != c and ord(t) <= 0xFFFF:
            chars[c] = t
    out = ["// Generated by tools/l10n/gen_strings.py from UI literals in src/ and catalog.tsv.",
           "// Do not edit by hand. Sorted by UTF-16 code unit for binary search.", "",
           "constexpr ZhHantPhrase kZhHantPhrases[] = {"]
    for key in sorted(phrases):
        out.append(f"    {{{cpp_literal(key)}, {cpp_literal(phrases[key])}}},")
    out += ["};", "", "constexpr ZhHantChar kZhHantChars[] = {"]
    for key in sorted(chars):
        out.append(f"    {{0x{ord(key):04X}, 0x{ord(chars[key]):04X}}},")
    out.append("};")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- catalog
class Row:
    def __init__(self, name, id_name, id_, zh, en, tw, comments=(), trailing=""):
        self.name, self.id_name, self.id = name, id_name, id_
        self.zh, self.en, self.tw = zh, en, tw
        self.comments = list(comments)
        self.trailing = trailing


def id_name_for(enum_name: str) -> str:
    """SettingsWinE -> IDS_SETTINGS_WIN_E; explicit ids.h names win at extract."""
    snake = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", enum_name).upper()
    return "IDS_" + snake


def read_catalog():
    rows, seen_name, seen_ids_name, seen_id = [], set(), set(), {}
    pending_comments = []
    for lineno, line in enumerate(CATALOG.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            pending_comments = []
            continue
        if line.startswith("#") or line.startswith("//"):
            if line.rstrip() not in _CATALOG_HEADER_SET:
                pending_comments.append(line)
            continue
        parts = line.split("\t")
        if len(parts) not in (5, 6):
            sys.exit(f"{CATALOG.name}:{lineno}: expected 5-6 tab separated fields")
        name, id_name, id_s, zh, en = parts[:5]
        tw = parts[5] if len(parts) == 6 else ""
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            sys.exit(f"{CATALOG.name}:{lineno}: bad enum name {name!r}")
        if not re.fullmatch(r"IDS_[A-Z0-9_]+", id_name):
            sys.exit(f"{CATALOG.name}:{lineno}: bad id name {id_name!r}")
        if name in seen_name or id_name in seen_ids_name:
            sys.exit(f"{CATALOG.name}:{lineno}: duplicate name/id_name")
        id_ = int(id_s)
        if id_ in seen_id:
            sys.exit(f"{CATALOG.name}:{lineno}: duplicate id {id_} (also {seen_id[id_]})")
        seen_name.add(name)
        seen_ids_name.add(id_name)
        seen_id[id_] = name
        if not zh and not en and not tw:
            pass  # an always-empty placeholder row is allowed (composition glue)
        rows.append(Row(name, id_name, id_, zh, en, tw, pending_comments))
        pending_comments = []
    return rows


# The catalog's own header, written once by write_catalog. read_catalog drops
# these lines so they are never taken for the first row's comments (which
# repeated the header on every rewrite and leaked it into string_ids.h).
CATALOG_HEADER = [
    "# Pulse UI strings — the only file to edit by hand. Then run:",
    "#     python tools/l10n/gen_strings.py",
    "# Columns: name<TAB>id_name<TAB>id<TAB>zh<TAB>en<TAB>tw",
    "#   name     StringId enum name (unique)",
    "#   id_name  IDS_* macro name (unique)",
    "#   id       numeric resource value (unique; never renumber existing rows)",
    "#   zh       Simplified Chinese (zh-CN)",
    "#   en       English (en-US)",
    "#   tw       Traditional Chinese (zh-TW); empty = generate with OpenCC s2twp",
    "# Format placeholders (%1, %d, %s ...) must match across zh/en/tw.",
]
_CATALOG_HEADER_SET = set(CATALOG_HEADER)


def write_catalog(rows, path=CATALOG):
    lines = list(CATALOG_HEADER)
    for row in rows:
        lines.extend(row.comments)
        lines.append("\t".join([row.name, row.id_name, str(row.id), row.zh, row.en, row.tw]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def row_trailing_comment(row):
    return getattr(row, "trailing", "")


# ---------------------------------------------------------------- parsers
def parse_ids_header(path=IDS_H):
    """[(id_name, value, comments, trailing_comment)] in file order."""
    rows, pending = [], []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip().startswith("//"):
            pending.append(line.strip())
            continue
        m = re.match(r"#define\s+(IDS_\w+)\s+(\d+)(?:\s*//\s*(.*))?$", line)
        if m:
            rows.append((m.group(1), int(m.group(2)), pending, m.group(3) or ""))
            pending = []
        elif line.strip():
            pending = []
    return rows


def parse_enum(path=LOC_H):
    """[(EnumName, IDSName)] in file order."""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"enum class StringId : UINT \{(.*?)\};", text, re.S)
    if not m:
        sys.exit("StringId enum not found in localization.h")
    rows = []
    for line in m.group(1).splitlines():
        e = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(IDS_\w+)\s*,", line)
        if e:
            rows.append((e.group(1), e.group(2)))
    return rows


def parse_rcinc(path, langs):
    """{IDS_NAME: text} for the requested LANGUAGE markers, e.g. ('0x04, 0x02',)."""
    out = {}
    current = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("LANGUAGE"):
            current = line.strip()
            continue
        if not any(lang in current for lang in langs):
            continue
        m = re.match(r'\s*(IDS_\w+)\s+"(.*)"\s*$', line)
        if m:
            text = m.group(2).replace('""', '"')
            if m.group(1) in out and out[m.group(1)] != text:
                sys.exit(f"{path.name}: conflicting duplicate {m.group(1)}")
            out[m.group(1)] = text
    return out


def rc_escape(text: str) -> str:
    return text.replace('"', '""')


# ---------------------------------------------------------------- outputs
def gen_ids_header(rows) -> str:
    width = max(len(r.id_name) for r in rows) + 1
    out = ["#ifndef PULSE_STRING_IDS_H", "#define PULSE_STRING_IDS_H", ""]
    for row in rows:
        # Catalog comments may use '#'; in a header only '//' is a comment.
        out.extend(c if c.lstrip().startswith("//") else "// " + c.lstrip("#").strip() for c in row.comments)
        tail = f" // {row.trailing}" if row.trailing else ""
        out.append(f"#define {row.id_name}{' ' * max(1, width - len(row.id_name))}{row.id}{tail}")
    out += ["", "#endif  // PULSE_STRING_IDS_H"]
    return "\n".join(out) + "\n"


def gen_enum_body(rows) -> str:
    return "\n".join(f"    {r.name} = {r.id_name}," for r in rows)


def replace_enum_body(text: str, body: str) -> str:
    m = re.search(r"(enum class StringId : UINT \{)(.*?)(\};)", text, re.S)
    if not m:
        sys.exit("StringId enum not found in localization.h")
    return text[: m.start(2)] + "\n" + body + "\n" + text[m.end(2):]


def gen_strings_rcinc(rows) -> str:
    ordered = sorted(rows, key=lambda r: r.id)
    out = [
        '#include "string_ids.h"',
        "",
        "// Generated by tools/l10n/gen_strings.py from tools/l10n/catalog.tsv.",
        "// Do not edit by hand: change the catalog and regenerate.",
        "",
        "LANGUAGE 0x04, 0x02",
        "STRINGTABLE",
        "BEGIN",
    ]
    out += [f'    {r.id_name} "{rc_escape(r.zh)}"' for r in ordered]
    out += ["END", "", "LANGUAGE 0x09, 0x01", "STRINGTABLE", "BEGIN"]
    out += [f'    {r.id_name} "{rc_escape(r.en)}"' for r in ordered]
    out += ["END", "",
            "// Traditional Chinese (Taiwan), generated from the Simplified entries above.",
            '#include "strings_zh_tw.rcinc"']
    return "\n".join(out) + "\n"


def gen_strings_tw_rcinc(rows) -> str:
    ordered = sorted(rows, key=lambda r: r.id)
    out = [
        "// Generated by tools/l10n/gen_strings.py from tools/l10n/catalog.tsv. Do not edit by hand:",
        "// change the catalog (tw column) and regenerate.",
        "",
        "LANGUAGE 0x04, 0x01",
        "STRINGTABLE",
        "BEGIN",
    ]
    out += [f'    {r.id_name} "{rc_escape(r.tw)}"' for r in ordered]
    out += ["END"]
    return "\n".join(out) + "\n"


def gen_setup_strings(rows) -> str:
    header = (ROOT / "src/setup/setup_strings.h").read_text(encoding="utf-8")
    body = re.search(r"enum class Str \{(.*?)\};", header, re.S).group(1)
    names = re.findall(r"^\s*(\w+)\s*,?\s*$", body, re.M)
    by_name = {row.name: row for row in rows}
    out = ["// Generated from tools/l10n/catalog.tsv. Do not edit by hand."]
    for name in names:
        if name == "Count":
            continue
        row = by_name["Setup" + name]
        out.append("    {" + ", ".join(cpp_literal(value) for value in (row.zh, row.tw, row.en)) + "},  // " + name)
    return "\n".join(out) + "\n"


def build_outputs(rows):
    for row in rows:
        if not row.tw:
            row.tw = to_hant(row.zh)
    return {
        IDS_H: gen_ids_header(rows),
        LOC_H: replace_enum_body(LOC_H.read_text(encoding="utf-8"), gen_enum_body(rows)),
        RCINC: gen_strings_rcinc(rows),
        RCINC_TW: gen_strings_tw_rcinc(rows),
        TABLE: generate_table([r.zh for r in rows]),
        ROOT / "src/setup/setup_strings.generated.inc": gen_setup_strings(rows),
    }


# ---------------------------------------------------------------- commands
def extract():
    if CATALOG.exists() and "--force" not in sys.argv:
        sys.exit(f"{CATALOG} already exists (use --force to rebuild it)")
    ids = parse_ids_header()
    enum = parse_enum()
    zh = parse_rcinc(RCINC, ("0x04, 0x02",))
    en = parse_rcinc(RCINC, ("0x09, 0x01",))
    tw = parse_rcinc(RCINC_TW, ("0x04, 0x01",))
    ids_by_name = {}
    ids_comments = {}
    ids_trailing = {}
    problems = []
    for name, value, comments, trailing in ids:
        if name in ids_by_name:
            problems.append(f"duplicate IDS name {name}")
        ids_by_name[name] = value
        # Trailing define comments are folded into the comment block so they
        # survive the catalog round trip.
        ids_comments[name] = list(comments) + ([f"// {trailing}"] if trailing else [])
        ids_trailing[name] = ""
    enum_ids_names = [ids_name for _, ids_name in enum]
    for name, ids_name in enum:
        if ids_name not in ids_by_name:
            problems.append(f"{name} = {ids_name}: no such define in string_ids.h")
    for ids_name in ids_by_name:
        if ids_name not in enum_ids_names:
            problems.append(f"{ids_name} missing from the StringId enum")
    for label, table in (("zh", zh), ("en", en), ("tw", tw)):
        for name in table:
            if name not in ids_by_name:
                problems.append(f"{label} string {name} has no IDS define")
        for name in ids_by_name:
            if name not in table:
                problems.append(f"{name} missing from the {label} table")
    if problems:
        for p in problems[:40]:
            print("PROBLEM:", p)
        sys.exit(f"extract: {len(problems)} problems")
    rows = []
    for enum_name, ids_name in enum:
        rows.append(Row(enum_name, ids_name, ids_by_name[ids_name],
                        zh[ids_name], en[ids_name], tw[ids_name],
                        ids_comments.get(ids_name, []), ids_trailing.get(ids_name, "")))
    write_catalog(rows)
    snapshot = {
        "ids": ids_by_name,
        "enum": [list(pair) for pair in enum],
        "zh": zh,
        "en": en,
        "tw": tw,
    }
    SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
    SNAPSHOT.write_text(json.dumps(snapshot, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"wrote {CATALOG.relative_to(ROOT)} ({len(rows)} rows)")
    print(f"wrote {SNAPSHOT.relative_to(ROOT)} (pre-generation snapshot)")


def parity():
    if not SNAPSHOT.exists():
        sys.exit(f"no snapshot at {SNAPSHOT}")
    snap = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    ids = {name: value for name, value, _, _ in parse_ids_header()}
    enum = parse_enum()
    zh = parse_rcinc(RCINC, ("0x04, 0x02",))
    en = parse_rcinc(RCINC, ("0x09, 0x01",))
    tw = parse_rcinc(RCINC_TW, ("0x04, 0x01",))
    bad = 0
    checks = (
        ("ids", snap["ids"], ids),
        ("enum", [tuple(x) for x in snap["enum"]], enum),
        ("zh", snap["zh"], zh),
        ("en", snap["en"], en),
        ("tw", snap["tw"], tw),
    )
    for label, before, after in checks:
        if before == after:
            print(f"OK {label}: identical content")
            continue
        bad += 1
        if isinstance(before, dict):
            keys = set(before) | set(after)
            diffs = [k for k in sorted(keys) if before.get(k) != after.get(k)]
            print(f"MISMATCH {label}: {len(diffs)} entries differ: {diffs[:10]}")
            for k in diffs[:5]:
                print(f"   {k}: {before.get(k)!r} -> {after.get(k)!r}")
        else:
            print(f"MISMATCH {label}: {len(before)} -> {len(after)} entries")
    if bad:
        sys.exit(f"parity: {bad} tables differ from the pre-generation state")
    print("parity: generated outputs carry the exact same strings as before")


def main():
    if "extract" in sys.argv:
        extract()
        return
    if "parity" in sys.argv:
        parity()
        return
    rows = read_catalog()
    outputs = build_outputs(rows)
    stale = [p for p, text in outputs.items()
             if not p.exists() or p.read_text(encoding="utf-8") != text]
    if "--check" in sys.argv:
        if stale:
            names = ", ".join(str(p.relative_to(ROOT)) for p in stale)
            sys.exit(f"{names} stale; run tools/l10n/gen_strings.py")
        print("l10n string outputs are up to date")
        return
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
