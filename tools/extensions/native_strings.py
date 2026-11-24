"""strings.tsv -> C++ header of pulse::sdk::LocalText accessors (stdlib only).

strings.tsv (UTF-8, tab separated, one string per line):

    # comments and blank lines are ignored
    key	zh-CN	zh-TW	en
    MenuTitle	整理文件	整理檔案	Tidy up
    Moved	已移动 {0} 个文件	已移動 {0} 個檔案	Moved {0} files

- The header row is optional; when present it must read key, zh-CN, zh-TW, en.
- Keys are C++ identifiers and become function names: strings::MenuTitle().
- Simplified Chinese is required. Empty Traditional or English falls back to
  Simplified at run time (LocalText::In).
- {0}..{9} are arguments; every non-empty column must use the same set. Keys
  with arguments take std::string_view parameters: strings::Moved("3").
- Escapes: \\t tab, \\n newline, \\\\ backslash.

The generated header is plain C++ (non-ASCII bytes are escaped, so it compiles
without /utf-8) and can be committed; `pulse_ext build` refreshes it whenever
the project has a strings.tsv.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SOURCE_NAME = "strings.tsv"
OUTPUT_NAME = "strings.gen.hpp"
HEADER = ("key", "zh-CN", "zh-TW", "en")
MAX_BYTES = 1024 * 1024
MAX_ENTRIES = 4096
MAX_TEXT_BYTES = 4096
KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}")
NAMESPACE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*")
PLACEHOLDER = re.compile(r"\{([0-9])\}")
# Names the header must not define: C++ keywords and what the header itself uses.
RESERVED = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t
class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false float for friend
goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private
protected public register reinterpret_cast requires return short signed sizeof static static_assert
static_cast struct switch template this thread_local throw true try typedef typeid typename union
unsigned using virtual void volatile wchar_t while xor xor_eq std pulse LocalText Tr Format
""".split())


class StringsError(ValueError):
    def __init__(self, message, line=None):
        super().__init__(f"{SOURCE_NAME}:{line}: {message}" if line else message)
        self.line = line


def _unescape(text, line):
    out = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        if i + 1 >= len(text):
            raise StringsError("dangling backslash (write \\\\ for a backslash)", line)
        nxt = text[i + 1]
        mapped = {"t": "\t", "n": "\n", "\\": "\\"}.get(nxt)
        if mapped is None:
            raise StringsError(f"unknown escape \\{nxt} (use \\t, \\n or \\\\)", line)
        out.append(mapped)
        i += 2
    return "".join(out)


def parse(text):
    """Return [{key, zh, tw, en, args, line}] or raise StringsError."""
    if text.startswith("\ufeff"):
        text = text[1:]
    entries = []
    seen = {}
    header_allowed = True
    for number, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        cells = raw.split("\t")
        if header_allowed and cells[0].strip().lower() == "key":
            header_allowed = False
            if tuple(c.strip() for c in cells) != HEADER:
                raise StringsError("header row must be: key<TAB>zh-CN<TAB>zh-TW<TAB>en", number)
            continue
        header_allowed = False
        if len(cells) > 4:
            raise StringsError("too many columns (expected key, zh-CN, zh-TW, en; write \\t for a tab)", number)
        cells += [""] * (4 - len(cells))
        key = cells[0].strip()
        if not KEY.fullmatch(key):
            raise StringsError(f"invalid key '{key}' (a C++ identifier, at most 64 characters)", number)
        if key in RESERVED:
            raise StringsError(f"key '{key}' is reserved", number)
        if key in seen:
            raise StringsError(f"duplicate key '{key}' (first on line {seen[key]})", number)
        zh, tw, en = (_unescape(cell, number) for cell in cells[1:])
        if not zh:
            raise StringsError(f"'{key}' needs Simplified Chinese text", number)
        for value in (zh, tw, en):
            if len(value.encode("utf-8")) > MAX_TEXT_BYTES:
                raise StringsError(f"'{key}' is longer than {MAX_TEXT_BYTES} bytes", number)
            if "\0" in value:
                raise StringsError(f"'{key}' contains a NUL character", number)
        args = {int(n) for n in PLACEHOLDER.findall(zh)}
        for label, value in (("zh-TW", tw), ("en", en)):
            if value and {int(n) for n in PLACEHOLDER.findall(value)} != args:
                raise StringsError(f"'{key}' {label} uses different {{N}} arguments than zh-CN", number)
        if args and args != set(range(max(args) + 1)):
            raise StringsError(f"'{key}' arguments must be numbered from {{0}} without gaps", number)
        seen[key] = number
        entries.append({"key": key, "zh": zh, "tw": tw, "en": en,
                        "args": (max(args) + 1) if args else 0, "line": number})
        if len(entries) > MAX_ENTRIES:
            raise StringsError(f"more than {MAX_ENTRIES} strings", number)
    return entries


def _literal(value):
    """A C++ narrow string literal holding the UTF-8 bytes of value."""
    out = ['"']
    for byte in value.encode("utf-8"):
        ch = chr(byte)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "?":
            out.append("\\?")  # no trigraphs
        elif 0x20 <= byte < 0x7f:
            out.append(ch)
        else:
            out.append(f"\\{byte:03o}")  # octal: never swallows a following digit
    out.append('"')
    return "".join(out)


def _comment(value):
    # One readable line per string. A backslash at the end of a // comment would
    # splice the next line into it, so comments show backslashes as U+FF3C.
    shown = value.replace("\\", "\uff3c").replace("\n", " ").replace("\t", " ").replace("\r", " ")
    return shown if len(shown) <= 120 else shown[:117] + "..."


def generate(text, namespace="strings", source_name=SOURCE_NAME):
    if not NAMESPACE.fullmatch(namespace) or any(part in RESERVED for part in namespace.split("::")):
        raise StringsError(f"invalid namespace '{namespace}'")
    entries = parse(text)
    # Line endings do not matter (a CRLF checkout of the same file is not stale).
    digest = hashlib.sha256(text.lstrip("\ufeff").replace("\r\n", "\n").encode("utf-8")).hexdigest()
    lines = [
        f"// Generated from {source_name} by pulse_ext strings. Do not edit; edit {source_name} instead.",
        f"// source sha256: {digest}",
        "#pragma once",
        "#include <pulse/app.hpp>",
        "#include <string_view>",
        "",
        f"namespace {namespace} {{",
    ]
    for entry in entries:
        lines.append(f"// {_comment(entry['zh'])}")
        zh, tw, en = (_literal(entry[k]) for k in ("zh", "tw", "en"))
        if not entry["args"]:
            lines.append(f"inline ::pulse::sdk::LocalText {entry['key']}() {{ return ::pulse::sdk::Tr({zh}, {tw}, {en}); }}")
            continue
        params = ", ".join(f"std::string_view a{i}" for i in range(entry["args"]))
        args = ", ".join(f"a{i}" for i in range(entry["args"]))
        lines.append(f"inline ::pulse::sdk::LocalText {entry['key']}({params}) {{")
        lines.append(f"    return ::pulse::sdk::Tr(::pulse::sdk::Format({zh}, {{{args}}}),")
        lines.append(f"                            ::pulse::sdk::Format({tw}, {{{args}}}),")
        lines.append(f"                            ::pulse::sdk::Format({en}, {{{args}}}));")
        lines.append("}")
    lines.append(f"}}  // namespace {namespace}")
    lines.append("")
    return "\n".join(lines), entries


def _read_source(source):
    source = Path(source)
    if not source.is_file():
        raise StringsError(f"{source} does not exist")
    data = source.read_bytes()
    if len(data) > MAX_BYTES:
        raise StringsError(f"{source.name} is larger than {MAX_BYTES} bytes")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StringsError(f"{source.name} is not UTF-8 (byte {exc.start})") from exc


def resolve(path, output=None):
    """A project folder or a .tsv file -> (source, output)."""
    path = Path(path)
    source = path / SOURCE_NAME if path.is_dir() else path
    target = Path(output) if output else source.with_name(OUTPUT_NAME)
    return source, target


def sync(path, output=None, namespace="strings", check=False):
    """Generate (or with check=True only compare) the header. Returns a report dict."""
    source, target = resolve(path, output)
    header, entries = generate(_read_source(source), namespace, source.name)
    current = target.read_bytes().decode("utf-8", errors="replace") if target.is_file() else None
    up_to_date = current == header
    if not check and not up_to_date:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(header.encode("utf-8"))
    return {
        "source": str(source),
        "output": str(target),
        "namespace": namespace,
        "keys": len(entries),
        "withArguments": sum(1 for e in entries if e["args"]),
        "upToDate": up_to_date,
        "written": not check and not up_to_date,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="project folder (uses strings.tsv) or a .tsv file")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--namespace", default="strings")
    parser.add_argument("--check", action="store_true", help="fail when the header is missing or stale")
    args = parser.parse_args(argv)
    try:
        report = sync(args.path, args.output, args.namespace, args.check)
    except StringsError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False))
        return 2
    ok = report["upToDate"] or not args.check
    print(json.dumps({"ok": ok, **report}, ensure_ascii=False, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
