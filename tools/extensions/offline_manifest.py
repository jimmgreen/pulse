"""Host-free offline manifest & PE64 ABI contract validator (Python stdlib only).

Matches src/app/extensions/manifest.cpp and schemas/extensions/manifest.schema.json
byte-for-byte so pulse_ext.py and native_author_mcp.py work 100% offline without pulse.exe.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import struct

import package_store

MAX_MANIFEST_BYTES = 65536
MAX_JSON_DEPTH = 40
MAX_ID_BYTES = 64
MAX_LABEL_BYTES = 128
MAX_RESOURCES = 15
MAX_CONTRIBUTED_COMMANDS = 32
MAX_CONTRIBUTED_ICONS = 32
MAX_ICON_LAYERS = 32
SDK_PREVIEW_VERSION = 3

KNOWN_RESOURCES: tuple[str, ...] = (
    "context.read",
    "files.readCurrentDirectory",
    "operations.own",
    "settings.own",
    "audit.own",
    "files.renameCurrentDirectory",
    "files.readDirectory",
    "files.watchDirectory",
    "files.open",
    "context.events",
    "files.transferCurrentSelection",
    "files.deleteCurrentSelection",
    "files.transferToDirectory",
    "files.transferFromDirectory",
    "files.transferDirectoryTree",
)

REMOVED_DECLARATIVE_FIELDS: tuple[str, ...] = (
    "runtime",
    "commands",
    "toolbar",
    "palette",
    "workflows",
    "panel",
)

ALLOWED_ROOT_FIELDS = frozenset({"apiVersion", "id", "name", "resources", "native", "contributes"})
ALLOWED_NATIVE_FIELDS = frozenset({"version", "architecture", "library", "sha256", "files"})
ALLOWED_SIDECAR_FIELDS = frozenset({"path", "sha256"})
ALLOWED_CONTRIBUTES_FIELDS = frozenset({"icons", "commands"})
ALLOWED_ICON_FIELDS = frozenset({"id", "viewBox", "layers"})
ALLOWED_ICON_LAYER_FIELDS = frozenset({"path", "stroke", "fill", "closed", "color", "strokeWidth", "opacity"})
ALLOWED_COMMAND_FIELDS = frozenset({
    "id",
    "title",
    "detail",
    "group",
    "groupIcon",
    "submenu",
    "primary",
    "separatorBefore",
    "separatorAfter",
    "hidden",
    "checked",
    "icon",
    "kinds",
    "minSelection",
    "maxSelection",
    "extensions",
    "excludeExtensions",
    "shortcut",
})
ALLOWED_LOCALIZED_FIELDS = frozenset({"zh", "en", "tw"})
ALLOWED_SHORTCUT_FIELDS = frozenset({"key", "ctrl", "shift", "alt"})

BUILTIN_ICON_NAMES = frozenset({
    "default", "archive", "extract", "search", "box", "extract-to", "add-archive", "test",
    "hash", "gear", "folder", "file", "chevron-right", "chevron-down", "chevron-up", "more",
    "check", "shield", "badge", "lock", "eye", "copy", "cut", "rename", "trash", "properties",
    "open", "open-with", "path", "undo", "close", "plus", "minus", "square", "back", "forward",
    "up", "refresh", "image", "camera", "film", "text-search", "sparkle", "puzzle", "palette",
    "list-view", "tabs", "power", "duplicate", "cube", "lines", "keyboard", "info", "warn",
    "sidebar", "panes", "contrast", "download", "cloud-off", "drag", "sort", "filter",
    "new-folder", "star", "pc", "disk", "chevron-left", "key",
    # Canonical aliases:
    "compress", "settings", "delete", "link", "add", "remove", "video", "extension", "warning",
})

_ID_RE = re.compile(r"^[A-Za-z0-9._-]+$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_EXT_RE = re.compile(r"^\.[a-z0-9._-]+$")
_COLOR_RE = re.compile(r"^(?:inherit|accent|success|warning|danger|#[0-9A-Fa-f]{6}|#[0-9A-Fa-f]{8})$")


def _strict_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    out: dict[str, object] = {}
    for key, val in pairs:
        if key in out:
            raise ValueError("Duplicate JSON member")
        out[key] = val
    return out


def _check_depth(value: object, current: int = 0) -> bool:
    if current > MAX_JSON_DEPTH:
        return False
    if isinstance(value, dict):
        if current + 1 > MAX_JSON_DEPTH:
            return False
        return all(_check_depth(v, current + 1) for v in value.values())
    if isinstance(value, list):
        if current + 1 > MAX_JSON_DEPTH:
            return False
        return all(_check_depth(v, current + 1) for v in value)
    return True


def _is_strict_int(value: object, minimum: int, maximum: int) -> bool:
    if isinstance(value, bool) or not isinstance(value, int):
        return False
    return minimum <= value <= maximum


def _valid_text(value: object, limit_bytes: int, allow_empty: bool = False) -> bool:
    if not isinstance(value, str):
        return False
    encoded = value.encode("utf-8")
    if len(encoded) == 0:
        return allow_empty
    if len(encoded) > limit_bytes:
        return False
    visible = False
    for b in encoded:
        if b < 0x20 or b == 0x7F:
            return False
        if b != 0x20:
            visible = True
    return visible


def _valid_id(value: object, limit_bytes: int = MAX_ID_BYTES) -> bool:
    return _valid_text(value, limit_bytes, allow_empty=False) and bool(_ID_RE.fullmatch(str(value)))


def _valid_library_name(value: object) -> bool:
    if not _valid_text(value, 128, allow_empty=False):
        return False
    s = str(value)
    if len(s) <= 4 or not s.endswith(".dll"):
        return False
    if any(ch in s for ch in '<>:"/\\|?*'):
        return False
    if not (s[0].isascii() and s[0].isalnum()):
        return False
    if not _ID_RE.fullmatch(s):
        return False
    stem = s.split(".", 1)[0].rstrip(" .").upper()
    if not stem:
        return False
    if stem in ("CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"):
        return False
    if stem.startswith(("COM", "LPT")):
        suffix = stem[3:]
        if (len(suffix) == 1 and "1" <= suffix <= "9") or suffix in ("\u00b9", "\u00b2", "\u00b3"):
            return False
    return True


def _valid_sidecar_name(value: object) -> bool:
    if not _valid_text(value, 128, allow_empty=False):
        return False
    s = str(value)
    if s.endswith(".") or not (s[0].isascii() and s[0].isalnum()) or not _ID_RE.fullmatch(s):
        return False
    lower = s.lower()
    if lower in ("manifest.json", "package.json"):
        return False
    stem = lower.split(".", 1)[0]
    if stem in ("con", "prn", "aux", "nul"):
        return False
    if len(stem) == 4 and stem[:3] in ("com", "lpt") and "1" <= stem[3] <= "9":
        return False
    return True


def _valid_localized_or_plain(value: object, limit_bytes: int, allow_empty: bool) -> bool:
    if value is None:
        return allow_empty
    if isinstance(value, str):
        return _valid_text(value, limit_bytes, allow_empty=allow_empty)
    if not isinstance(value, dict) or not set(value.keys()).issubset(ALLOWED_LOCALIZED_FIELDS):
        return False
    if "zh" not in value or "en" not in value:
        return False
    if not _valid_text(value["zh"], limit_bytes, allow_empty=allow_empty):
        return False
    if not _valid_text(value["en"], limit_bytes, allow_empty=allow_empty):
        return False
    if "tw" in value and not _valid_text(value["tw"], limit_bytes, allow_empty=allow_empty):
        return False
    return True


def _is_strict_number(value: object, minimum: float, maximum: float) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    f = float(value)
    return minimum <= f <= maximum


def _valid_svg_path(data: str) -> bool:
    if not data or len(data.encode("utf-8")) > 2048:
        return False
    for ch in data:
        code = ord(ch)
        if ch in "\t\n\r":
            continue
        if code < 0x20 or code > 0x7E:
            return False
    n = len(data)
    i = 0

    def skip() -> None:
        nonlocal i
        while i < n and data[i] in " ,\t\n\r":
            i += 1

    def read_num() -> bool:
        nonlocal i
        skip()
        if i >= n:
            return False
        start = i
        if data[i] in "+-":
            i += 1
        digits = False
        dot = False
        while i < n and (data[i].isdigit() or (data[i] == "." and not dot)):
            if data[i] == ".":
                dot = True
            else:
                digits = True
            i += 1
        if not digits:
            i = start
            return False
        if i < n and data[i] in "eE":
            exp = i + 1
            if exp < n and data[exp] in "+-":
                exp += 1
            if exp >= n or not data[exp].isdigit():
                return False
            i = exp
            while i < n and data[i].isdigit():
                i += 1
        try:
            v = float(data[start:i])
        except ValueError:
            return False
        return abs(v) <= 16384.0

    def read_flag() -> bool:
        nonlocal i
        skip()
        if i >= n or data[i] not in ("0", "1"):
            return False
        i += 1
        return True

    cmd = ""
    has_cmd = False
    segments = 0
    while True:
        skip()
        if i >= n:
            break
        if data[i].isalpha() and data[i].isascii():
            cmd = data[i]
            i += 1
            if not has_cmd and cmd not in ("M", "m"):
                return False
        elif not cmd:
            return False
        has_cmd = True
        segments += 1
        if segments > 512:
            return False
        kind = cmd.upper()
        if kind == "M":
            if not (read_num() and read_num()):
                return False
            cmd = "l" if cmd.islower() else "L"
        elif kind == "L":
            if not (read_num() and read_num()):
                return False
        elif kind in ("H", "V"):
            if not read_num():
                return False
        elif kind == "C":
            if not all(read_num() for _ in range(6)):
                return False
        elif kind == "S":
            if not all(read_num() for _ in range(4)):
                return False
        elif kind == "A":
            if not (read_num() and read_num() and read_num() and read_flag() and read_flag() and read_num() and read_num()):
                return False
        elif kind == "Z":
            cmd = ""
        else:
            return False
    return has_cmd


def _parse_shortcut_vk(key: object) -> int | None:
    if not isinstance(key, str):
        return None
    if len(key) == 1 and (("A" <= key <= "Z") or ("0" <= key <= "9")):
        return ord(key)
    if 2 <= len(key) <= 3 and key[0] == "F" and key[1] != "0" and key[1:].isdigit():
        num = int(key[1:])
        if 1 <= num <= 24:
            return 0x70 + num - 1
    return None


def validate_manifest_bytes(data: bytes, path_str: str = "<memory>") -> dict:
    if len(data) > MAX_MANIFEST_BYTES:
        return {
            "valid": False,
            "error": "Manifest exceeds the 65536-byte limit",
            "code": "too_large",
            "path": path_str,
        }
    if data.startswith(b"\xef\xbb\xbf"):
        return {
            "valid": False,
            "error": "UTF-8 BOM is not permitted in manifest JSON",
            "code": "invalid_manifest",
            "path": path_str,
        }
    try:
        text = data.decode("utf-8", errors="strict")
        # Reject unpaired surrogates if any slipped in via JSON \uD800 escapes after loads.
        root = json.loads(
            text,
            object_pairs_hook=_strict_pairs,
            parse_constant=lambda val: (_ for _ in ()).throw(ValueError(f"Non-standard JSON constant: {val}")),
        )
        json.dumps(root, ensure_ascii=False).encode("utf-8", errors="strict")
    except (UnicodeError, ValueError) as exc:
        return {
            "valid": False,
            "error": f"Invalid strict UTF-8 JSON: {exc}",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if not _check_depth(root, 0):
        return {
            "valid": False,
            "error": "JSON nesting depth exceeds maximum limit (40)",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if not isinstance(root, dict):
        return {
            "valid": False,
            "error": "Manifest must be a JSON object",
            "code": "invalid_manifest",
            "path": path_str,
        }

    for removed in REMOVED_DECLARATIVE_FIELDS:
        if removed in root:
            return {
                "valid": False,
                "error": f"{removed} is no longer supported; use a native C++ DLL package",
                "code": "invalid_manifest",
                "path": path_str,
            }

    if not set(root.keys()).issubset(ALLOWED_ROOT_FIELDS):
        return {
            "valid": False,
            "error": "Manifest must be an object containing only apiVersion, id, name, resources, native and contributes",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if not _is_strict_int(root.get("apiVersion"), 1, 1):
        return {
            "valid": False,
            "error": "apiVersion must be 1",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if not _valid_id(root.get("id"), MAX_ID_BYTES):
        return {
            "valid": False,
            "error": "Invalid manifest id (1-64 ASCII letters, digits, ._-)",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if not _valid_text(root.get("name"), MAX_LABEL_BYTES, allow_empty=False):
        return {
            "valid": False,
            "error": "Invalid manifest name (1-128 UTF-8 bytes, no controls)",
            "code": "invalid_manifest",
            "path": path_str,
        }

    if "resources" in root:
        resources = root["resources"]
        if not isinstance(resources, list) or len(resources) > MAX_RESOURCES:
            return {
                "valid": False,
                "error": "resources must be an array of at most 15 resource permissions",
                "code": "invalid_manifest",
                "path": path_str,
            }
        seen_res: set[str] = set()
        for idx, res in enumerate(resources):
            if not isinstance(res, str) or res not in KNOWN_RESOURCES:
                return {
                    "valid": False,
                    "error": f"resources[{idx}]: unknown resource permission",
                    "code": "invalid_manifest",
                    "path": path_str,
                }
            if res in seen_res:
                return {
                    "valid": False,
                    "error": "resources contains a duplicate permission",
                    "code": "invalid_manifest",
                    "path": path_str,
                }
            seen_res.add(res)

    if "contributes" in root:
        contributes = root["contributes"]
        if not isinstance(contributes, dict) or not set(contributes.keys()).issubset(ALLOWED_CONTRIBUTES_FIELDS):
            return {
                "valid": False,
                "error": "contributes: expected only icons and commands",
                "code": "invalid_manifest",
                "path": path_str,
            }
        declared_icons: set[str] = set()
        manifest_id_str = str(root["id"])
        if "icons" in contributes:
            icons_list = contributes["icons"]
            if not isinstance(icons_list, list) or len(icons_list) > MAX_CONTRIBUTED_ICONS:
                return {
                    "valid": False,
                    "error": "contributes.icons: expected an array of at most 32 icons",
                    "code": "invalid_manifest",
                    "path": path_str,
                }
            for idx, icon_item in enumerate(icons_list):
                if not isinstance(icon_item, dict) or not set(icon_item.keys()).issubset(ALLOWED_ICON_FIELDS):
                    return {
                        "valid": False,
                        "error": f"contributes.icons[{idx}]: unexpected or malformed field",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                iid = icon_item.get("id")
                if not _valid_id(iid, MAX_ID_BYTES) or str(iid) in declared_icons:
                    return {
                        "valid": False,
                        "error": f"contributes.icons[{idx}]: invalid or duplicate icon id",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                declared_icons.add(str(iid))
                if "viewBox" in icon_item and not _is_strict_number(icon_item["viewBox"], 4.0, 1024.0):
                    return {
                        "valid": False,
                        "error": "contributes.icons: viewBox must be a number in [4, 1024]",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                layers = icon_item.get("layers")
                if not isinstance(layers, list) or not (1 <= len(layers) <= MAX_ICON_LAYERS):
                    return {
                        "valid": False,
                        "error": "contributes.icons: layers must contain 1..32 layers",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                total_path_bytes = 0
                for raw_layer in layers:
                    if not isinstance(raw_layer, dict) or not set(raw_layer.keys()).issubset(ALLOWED_ICON_LAYER_FIELDS):
                        return {
                            "valid": False,
                            "error": "contributes.icons: unexpected layer field",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    p = raw_layer.get("path")
                    if not _valid_text(p, 2048, allow_empty=False) or not _valid_svg_path(str(p)):
                        return {
                            "valid": False,
                            "error": "contributes.icons: invalid SVG path syntax",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    total_path_bytes += len(str(p).encode("utf-8"))
                    if total_path_bytes > 8192:
                        return {
                            "valid": False,
                            "error": "contributes.icons: icon path data exceeds 8192 bytes",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    if ("stroke" in raw_layer and type(raw_layer["stroke"]) is not bool) or (
                        "fill" in raw_layer and type(raw_layer["fill"]) is not bool
                    ):
                        return {
                            "valid": False,
                            "error": "contributes.icons: stroke and fill must be boolean",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    do_stroke = raw_layer["stroke"] if "stroke" in raw_layer else (False if "fill" in raw_layer else True)
                    do_fill = raw_layer["fill"] if "fill" in raw_layer else False
                    if not do_stroke and not do_fill:
                        return {
                            "valid": False,
                            "error": "contributes.icons: layer must enable stroke and/or fill",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    if "closed" in raw_layer and type(raw_layer["closed"]) is not bool:
                        return {
                            "valid": False,
                            "error": "contributes.icons: closed must be boolean",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    if "color" in raw_layer:
                        col = raw_layer["color"]
                        if not isinstance(col, str) or not _COLOR_RE.fullmatch(col) or (
                            len(col) == 9 and col[1:3] == "00"
                        ):
                            return {
                                "valid": False,
                                "error": "contributes.icons: unknown color role or hex color",
                                "code": "invalid_manifest",
                                "path": path_str,
                            }
                    if "strokeWidth" in raw_layer and not _is_strict_number(raw_layer["strokeWidth"], 0.25, 8.0):
                        return {
                            "valid": False,
                            "error": "contributes.icons: strokeWidth must be in [0.25, 8.0]",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    if "opacity" in raw_layer and not _is_strict_number(raw_layer["opacity"], 0.01, 1.0):
                        return {
                            "valid": False,
                            "error": "contributes.icons: opacity must be in [0.01, 1.0]",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }

        def _valid_manifest_icon_ref(name_obj: object) -> bool:
            if not isinstance(name_obj, str) or not name_obj:
                return False
            if name_obj in declared_icons or name_obj in BUILTIN_ICON_NAMES:
                return True
            prefix = manifest_id_str + "/"
            if name_obj.startswith(prefix) and name_obj[len(prefix):] in declared_icons:
                return True
            return False

        if "commands" in contributes:
            commands = contributes["commands"]
            if not isinstance(commands, list) or len(commands) > MAX_CONTRIBUTED_COMMANDS:
                return {
                    "valid": False,
                    "error": "contributes.commands: expected an array of at most 32 commands",
                    "code": "invalid_manifest",
                    "path": path_str,
                }
            seen_cmd_ids: set[str] = set()
            seen_shortcuts: set[tuple[int, int]] = set()
            for idx, item in enumerate(commands):
                if not isinstance(item, dict) or not set(item.keys()).issubset(ALLOWED_COMMAND_FIELDS):
                    return {
                        "valid": False,
                        "error": f"contributes.commands[{idx}]: unexpected or malformed field",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                cid = item.get("id")
                if not _valid_id(cid, MAX_ID_BYTES) or str(cid) in seen_cmd_ids:
                    return {
                        "valid": False,
                        "error": f"contributes.commands[{idx}]: invalid or duplicate command id",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                seen_cmd_ids.add(str(cid))

                if not _valid_localized_or_plain(item.get("title"), MAX_LABEL_BYTES, allow_empty=False):
                    return {
                        "valid": False,
                        "error": f"contributes.commands[{idx}]: invalid title",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                if "detail" in item and not _valid_localized_or_plain(item.get("detail"), 160, allow_empty=True):
                    return {
                        "valid": False,
                        "error": f"contributes.commands[{idx}]: invalid detail",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                if "group" in item and not _valid_localized_or_plain(item.get("group"), 32, allow_empty=True):
                    return {
                        "valid": False,
                        "error": f"contributes.commands[{idx}]: invalid group",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                for bool_flag in ("submenu", "primary", "separatorBefore", "separatorAfter", "hidden", "checked"):
                    if bool_flag in item and type(item[bool_flag]) is not bool:
                        return {
                            "valid": False,
                            "error": f"contributes.commands: {bool_flag} must be boolean",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                if "icon" in item and not _valid_manifest_icon_ref(item["icon"]):
                    return {
                        "valid": False,
                        "error": "contributes.commands: unknown icon",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                if "groupIcon" in item and not _valid_manifest_icon_ref(item["groupIcon"]):
                    return {
                        "valid": False,
                        "error": "contributes.commands: unknown groupIcon",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                kinds = item.get("kinds")
                if not isinstance(kinds, list) or not (1 <= len(kinds) <= 3):
                    return {
                        "valid": False,
                        "error": "contributes.commands: kinds must contain files, directories, and/or background",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                seen_kinds: set[str] = set()
                for k in kinds:
                    if not isinstance(k, str) or k not in ("files", "directories", "background") or k in seen_kinds:
                        return {
                            "valid": False,
                            "error": "contributes.commands: invalid or duplicate kind",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    seen_kinds.add(k)

                min_sel = item.get("minSelection")
                max_sel = item.get("maxSelection")
                if (not _is_strict_int(min_sel, 0, 4096) or
                        not _is_strict_int(max_sel, 0, 4096) or
                        int(min_sel) > int(max_sel)):  # type: ignore[arg-type]
                    return {
                        "valid": False,
                        "error": "contributes.commands: invalid minSelection/maxSelection range",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                if seen_kinds == {"background"} and int(min_sel) > 0:  # type: ignore[arg-type]
                    return {
                        "valid": False,
                        "error": "contributes.commands: background-only command requires minSelection 0",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }

                if "excludeExtensions" in item and type(item["excludeExtensions"]) is not bool:
                    return {
                        "valid": False,
                        "error": "contributes.commands: excludeExtensions must be boolean",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                if "extensions" in item:
                    exts = item["extensions"]
                    if not isinstance(exts, list) or len(exts) > 32:
                        return {
                            "valid": False,
                            "error": "contributes.commands: extensions must be an array of at most 32 extensions",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    seen_exts: set[str] = set()
                    for ext in exts:
                        if (not isinstance(ext, str) or
                                not (2 <= len(ext.encode("utf-8")) <= 32) or
                                not _EXT_RE.fullmatch(ext) or
                                ext in seen_exts):
                            return {
                                "valid": False,
                                "error": "contributes.commands: invalid or duplicate lowercase dotted extension",
                                "code": "invalid_manifest",
                                "path": path_str,
                            }
                        seen_exts.add(ext)

                if "shortcut" in item:
                    sc = item["shortcut"]
                    if not isinstance(sc, dict) or not set(sc.keys()).issubset(ALLOWED_SHORTCUT_FIELDS):
                        return {
                            "valid": False,
                            "error": "contributes.commands: invalid shortcut fields",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    vk = _parse_shortcut_vk(sc.get("key"))
                    if vk is None:
                        return {
                            "valid": False,
                            "error": "contributes.commands: invalid shortcut key",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    mods = 0
                    for mod_name, bit in (("ctrl", 1), ("shift", 2), ("alt", 4)):
                        if mod_name in sc:
                            if type(sc[mod_name]) is not bool:
                                return {
                                    "valid": False,
                                    "error": "contributes.commands: shortcut modifiers must be boolean",
                                    "code": "invalid_manifest",
                                    "path": path_str,
                                }
                            if sc[mod_name]:
                                mods |= bit
                    if vk < 0x70 and (mods & (1 | 4)) == 0:
                        return {
                            "valid": False,
                            "error": "contributes.commands: non-function shortcut requires ctrl or alt",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    if (vk, mods) in seen_shortcuts:
                        return {
                            "valid": False,
                            "error": "contributes.commands: duplicate shortcut",
                            "code": "invalid_manifest",
                            "path": path_str,
                        }
                    seen_shortcuts.add((vk, mods))

    if "native" in root:
        native = root["native"]
        if not isinstance(native, dict) or not set(native.keys()).issubset(ALLOWED_NATIVE_FIELDS):
            return {
                "valid": False,
                "error": "native: expected only version, architecture, library, sha256 and files",
                "code": "invalid_manifest",
                "path": path_str,
            }
        if not _is_strict_int(native.get("version"), 1, 1):
            return {
                "valid": False,
                "error": "native.version: expected 1",
                "code": "invalid_manifest",
                "path": path_str,
            }
        if native.get("architecture") != "windows-x64":
            return {
                "valid": False,
                "error": "native.architecture: expected windows-x64",
                "code": "invalid_manifest",
                "path": path_str,
            }
        lib = native.get("library")
        if not _valid_library_name(lib):
            return {
                "valid": False,
                "error": "native.library: expected a non-reserved single filename ending in .dll",
                "code": "invalid_manifest",
                "path": path_str,
            }
        sha = native.get("sha256")
        if not isinstance(sha, str) or not _SHA256_RE.fullmatch(sha):
            return {
                "valid": False,
                "error": "native.sha256: expected 64 lowercase hexadecimal characters",
                "code": "invalid_manifest",
                "path": path_str,
            }
        if "files" in native:
            files = native["files"]
            if not isinstance(files, list) or not (1 <= len(files) <= 63):
                return {
                    "valid": False,
                    "error": "native.files: expected 1-63 sidecar files; omit files for a single DLL",
                    "code": "invalid_manifest",
                    "path": path_str,
                }
            seen_names = {str(lib).lower()}
            for entry in files:
                if not isinstance(entry, dict) or set(entry.keys()) != ALLOWED_SIDECAR_FIELDS:
                    return {
                        "valid": False,
                        "error": "native.files: expected path and sha256",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                fpath = entry.get("path")
                fsha = entry.get("sha256")
                if not _valid_sidecar_name(fpath) or not isinstance(fsha, str) or not _SHA256_RE.fullmatch(fsha):
                    return {
                        "valid": False,
                        "error": "native.files: expected a non-reserved flat ASCII path and lowercase SHA-256",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                lower = str(fpath).lower()
                if lower in seen_names:
                    return {
                        "valid": False,
                        "error": "native.files: duplicate filename (case-insensitive)",
                        "code": "invalid_manifest",
                        "path": path_str,
                    }
                seen_names.add(lower)

    return {
        "valid": True,
        "error": "",
        "code": "",
        "path": path_str,
    }


def validate_manifest_file(path: Path) -> dict:
    abs_path = Path(os.path.abspath(path))
    path_str = str(abs_path)
    try:
        for ancestor in (abs_path, *abs_path.parents):
            package_store.ordinary(ancestor)
    except package_store.StoreError as exc:
        return {"valid": False, "error": str(exc), "code": "invalid_path", "path": path_str}
    if not abs_path.is_file():
        return {"valid": False, "error": "Manifest file was not found", "code": "read_failed", "path": path_str}
    try:
        with abs_path.open("rb") as stream:
            data = stream.read(MAX_MANIFEST_BYTES + 1)
    except OSError as exc:
        return {"valid": False, "error": str(exc), "code": "read_failed", "path": path_str}
    return validate_manifest_bytes(data, path_str)


def offline_api_contract(sdk_root: Path | None = None) -> dict:
    root = Path(os.path.abspath(sdk_root)) if sdk_root else Path(__file__).resolve().parents[2]
    catalog = None
    for candidate in (root / "sdk/native/api-catalog.json", root / "api-catalog.json"):
        if candidate.is_file():
            try:
                catalog = json.loads(candidate.read_text(encoding="utf-8"))
                break
            except (OSError, ValueError):
                pass
    sdk_preview = catalog.get("sdkPreview", SDK_PREVIEW_VERSION) if isinstance(catalog, dict) else SDK_PREVIEW_VERSION
    return {
        "apiVersion": 1,
        "offline": True,
        "manifestSchema": {
            "type": "object",
            "properties": {
                "resources": {
                    "items": {"enum": list(KNOWN_RESOURCES)},
                }
            },
        },
        "limits": {
            "manifestBytes": MAX_MANIFEST_BYTES,
            "idBytes": MAX_ID_BYTES,
            "labelBytes": MAX_LABEL_BYTES,
        },
        "capabilities": ["native.plugin"],
        "bindings": ["context.directory"],
        "developmentNotes": ["offline-sdk-contract"],
        "developerFeatures": ["nativePackages", "isolatedDevReload"],
        "nativeSdk": {
            "catalog": catalog or {"sdkPreview": sdk_preview, "abiVersion": 1},
        },
    }


def inspect_pe64_binary(data: bytes) -> dict:
    """Static PE32+ (Windows x64) inspector for exports and DLL imports."""
    sha256 = package_store.validate_native_binary(data)
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    coff_offset = pe_offset + 4
    _, num_sections, _, _, _, opt_size, characteristics = struct.unpack_from("<HHIIIHH", data, coff_offset)
    opt_offset = coff_offset + 20
    magic = struct.unpack_from("<H", data, opt_offset)[0]
    if magic != 0x20B:
        raise package_store.StoreError("invalid_native_binary", "Expected PE32+ (x64) optional header")

    is_dll = bool(characteristics & 0x2000)
    num_rva = struct.unpack_from("<I", data, opt_offset + 108)[0]
    dirs_offset = opt_offset + 112

    sections: list[tuple[int, int, int, int]] = []
    sec_table = opt_offset + opt_size
    for i in range(num_sections):
        base = sec_table + i * 40
        if base + 40 > len(data):
            break
        vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, base + 8)
        sections.append((rva, max(vsize, raw_size), raw_ptr, raw_size))

    def rva_to_offset(rva: int) -> int | None:
        if rva == 0:
            return None
        for sec_rva, sec_span, raw_ptr, raw_size in sections:
            if sec_rva <= rva < sec_rva + sec_span:
                delta = rva - sec_rva
                if delta < raw_size and raw_ptr + delta < len(data):
                    return raw_ptr + delta
                return None
        return None

    def read_cstr(offset: int | None, limit: int = 256) -> str | None:
        if offset is None or offset < 0 or offset >= len(data):
            return None
        end = data.find(b"\x00", offset, min(len(data), offset + limit))
        if end < 0:
            return None
        return data[offset:end].decode("ascii", errors="replace")

    exports: list[str] = []
    if num_rva >= 1 and dirs_offset + 8 <= len(data):
        exp_rva, exp_size = struct.unpack_from("<II", data, dirs_offset)
        exp_off = rva_to_offset(exp_rva)
        if exp_off is not None and exp_size >= 40 and exp_off + 40 <= len(data):
            num_names = struct.unpack_from("<I", data, exp_off + 24)[0]
            names_rva = struct.unpack_from("<I", data, exp_off + 32)[0]
            names_off = rva_to_offset(names_rva)
            if names_off is not None and num_names <= 16384:
                for idx in range(num_names):
                    pos = names_off + idx * 4
                    if pos + 4 > len(data):
                        break
                    name_rva = struct.unpack_from("<I", data, pos)[0]
                    sym = read_cstr(rva_to_offset(name_rva))
                    if sym:
                        exports.append(sym)

    imports: list[str] = []
    if num_rva >= 2 and dirs_offset + 16 <= len(data):
        imp_rva, imp_size = struct.unpack_from("<II", data, dirs_offset + 8)
        imp_off = rva_to_offset(imp_rva)
        if imp_off is not None and imp_size >= 20:
            cursor = imp_off
            for _ in range(512):
                if cursor + 20 > len(data):
                    break
                orig_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, cursor)
                if orig_thunk == 0 and name_rva == 0 and first_thunk == 0:
                    break
                dll_name = read_cstr(rva_to_offset(name_rva))
                if dll_name:
                    imports.append(dll_name)
                cursor += 20

    gui_imports = [
        dll for dll in imports
        if dll.lower() in ("user32.dll", "gdi32.dll", "shell32.dll")
    ]
    return {
        "sha256": sha256,
        "architecture": "windows-x64",
        "isDll": is_dll,
        "exports": sorted(set(exports)),
        "hasEntryV1": "PulsePluginEntryV1" in exports,
        "hasSdkPreview": "PulsePluginSdkPreview" in exports,
        "imports": imports,
        "guiImports": gui_imports,
    }
