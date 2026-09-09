# One-shot splitter for app_main.cpp / ui_renderer.cpp. Not part of the product.
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "src" / "app"
UI = ROOT / "src" / "ui"

def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines(keepends=True)

def slice_lines(lines: list[str], start: int, end: int) -> str:
    return "".join(lines[start - 1 : end])

def strip_static(text: str) -> str:
    out = []
    for line in text.splitlines(True):
        if line.startswith("static "):
            line = line[len("static ") :]
        elif line.startswith("template<typename Fn>\n") or line.startswith("template<typename Fn>\r"):
            pass
        out.append(line)
    return "".join(out)

def strip_definition_defaults(text: str) -> str:
    replacements = [
        (
            "ui::WindowViewModel BuildVm(AppState& s, bool probe_details = true)",
            "ui::WindowViewModel BuildVm(AppState& s, bool probe_details)",
        ),
        (
            "app::PlaceItemKind kind = app::PlaceItemKind::Unknown)",
            "app::PlaceItemKind kind)",
        ),
        (
            "ui::PaneViewModel* out = nullptr)",
            "ui::PaneViewModel* out)",
        ),
        (
            "bool include_descendants = false)",
            "bool include_descendants)",
        ),
    ]
    for old, new in replacements:
        text = text.replace(old, new)
    return text

def extract_decls(text: str) -> list[str]:
    """Top-level function declarations from a cpp snippet (no static, keep defaults)."""
    decls: list[str] = []
    i = 0
    n = len(text)
    depth = 0
    while i < n:
        if depth == 0:
            m = re.match(
                r"(?:template<[^>]+>\s*)?(?:static\s+)?(?:inline\s+)?"
                r"(?:[\w:<>*&,\s]+?)\s+\w+\s*\([^;{}]*\)\s*(?:const\s*)?(?:\{|;)",
                text[i:],
                re.S,
            )
            # Fallback: scan line-based
        i += 1
        if text[i - 1] == "{":
            depth += 1
        elif text[i - 1] == "}":
            depth = max(0, depth - 1)
    # More reliable: line-based brace matcher
    lines = text.splitlines()
    depth = 0
    buf: list[str] = []
    collecting = False
    for line in lines:
        stripped = line.strip()
        if not collecting and depth == 0:
            if stripped.startswith("template<"):
                collecting = True
                buf = [line]
                depth += line.count("{") - line.count("}")
                if "{" in line and depth == 0 and stripped.endswith(";"):
                    collecting = False
                    buf = []
                continue
            if (
                stripped.startswith("static ")
                or re.match(
                    r"^(?:const\s+)?(?:[\w:<>*&]+(?:\s+[\w:<>*&]+)*)\s+\w+\s*\(",
                    stripped,
                )
            ) and not stripped.startswith("static HANDLE") and not stripped.startswith("static constexpr"):
                if stripped.startswith("struct ") or stripped.startswith("enum "):
                    depth += line.count("{") - line.count("}")
                    continue
                collecting = True
                buf = [line]
                depth += line.count("{") - line.count("}")
                if stripped.endswith(";") and "{" not in line:
                    collecting = False
                    buf = []
                    depth = 0
                elif "{" in line:
                    sig = "\n".join(buf)
                    sig = re.sub(r"\s*\{.*", "", sig, flags=re.S)
                    sig = sig.replace("static ", "", 1) if sig.lstrip().startswith("static ") else sig
                    decls.append(sig.strip() + ";")
                    collecting = False
                    buf = []
                continue
        if collecting:
            buf.append(line)
            depth += line.count("{") - line.count("}")
            if "{" in line:
                sig = "\n".join(buf)
                sig = re.sub(r"\s*\{.*", "", sig, flags=re.S)
                sig = re.sub(r"^static ", "", sig.lstrip(), count=1)
                # undo lstrip of first line indent — keep readable
                first = buf[0]
                indent = first[: len(first) - len(first.lstrip())]
                decls.append(sig.strip() + ";")
                collecting = False
                buf = []
            elif stripped.endswith(";") and depth <= 0:
                collecting = False
                buf = []
                depth = 0
        else:
            depth += line.count("{") - line.count("}")
            if depth < 0:
                depth = 0
    # Dedup while preserving order
    seen = set()
    out = []
    for d in decls:
        key = re.sub(r"\s+", " ", d)
        if key in seen:
            continue
        seen.add(key)
        out.append(d)
    return out


APP_CPP_INCLUDES = r'''#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;
'''

UI_CPP_INCLUDES = r'''#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {
'''


def write_app_state(lines: list[str]) -> None:
    wm = slice_lines(lines, 92, 112)
    structs = slice_lines(lines, 114, 539)
    # Drop g_shell globals from the struct block
    structs = re.sub(
        r"\nstatic HANDLE g_shell_watch_stop = nullptr;\nstatic HANDLE g_shell_watch_thread = nullptr;\n",
        "\n",
        structs,
    )
    header = r'''// app_state.h — Window process state, shot request, and WM_APP message ids.
#pragma once

#include "../ui/ui_compositor.h"
#include "../ui/ui_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/bloom_accent_picker.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "app_model.h"
#include "app_worker.h"
#include "places.h"
#include "details_meta.h"
#include "context_menu_prefs.h"
#include "context_menu_controller.h"
#include "shell_verbs.h"
#include "app_prefs.h"
#include "saved_search.h"
#include "settings_controller.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "tab_controller.h"
#include "update_checker.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include "../ops/ops_manager.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse {

'''
    header += wm
    header += "\n"
    header += r'''enum class OmnibarMode { Path, Command, Project };

struct TrayDeckEntry {
    int batch = -1;
    int sub = -1;
    const app::TrayItem* item = nullptr;
};

'''
    header += structs
    header += r'''
inline AppState* GetAppState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

inline std::wstring ClipboardPath(const std::wstring& p) {
    if (p.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + p.substr(8);
    if (p.starts_with(L"\\\\?\\")) return p.substr(4);
    return p;
}

} // namespace pulse
'''
    (APP / "app_state.h").write_text(header, encoding="utf-8")


INLINE_RUNTIME = r'''
inline app::LayoutTab& LiveLayout(AppState& s) {
    s.window_tabs.EnsureDefault();
    return *s.window_tabs.Active();
}

inline const app::LayoutTab* LiveLayout(const AppState& s) {
    return s.window_tabs.Active();
}

inline std::vector<std::unique_ptr<app::Pane>>& Panes(AppState& s) {
    return LiveLayout(s).panes;
}

inline std::unique_ptr<app::SplitContainer>& Root(AppState& s) {
    return LiveLayout(s).root;
}

inline const std::unique_ptr<app::SplitContainer>& Root(const AppState& s) {
    static const std::unique_ptr<app::SplitContainer> empty;
    const app::LayoutTab* tab = LiveLayout(s);
    return tab ? tab->root : empty;
}

inline app::LayoutPreset& LayoutOf(AppState& s) {
    return LiveLayout(s).layout;
}

template<typename Fn>
inline void ForEachPane(AppState& s, Fn&& fn) {
    for (auto& owned : s.window_tabs.items) {
        if (!owned) continue;
        for (auto& pane : owned->panes) {
            if (pane) fn(*pane);
        }
    }
}

inline app::Tab* ActiveTab(AppState& s) {
    return s.pane ? s.pane->ActiveTab() : nullptr;
}
'''


def wrap_cpp(name: str, body: str) -> str:
    body = strip_static(body)
    body = strip_definition_defaults(body)
    return f"// {name} — extracted from app_main.cpp.\n{APP_CPP_INCLUDES}\nnamespace pulse {{\n{body}\n}} // namespace pulse\n"


def prepare_handler(body: str) -> str:
    body = body.replace("if (!s) break;", "if (!s) return DefWindowProcW(hwnd, msg, wParam, lParam);")
    # Trailing switch-style break at function scope: last line `        break;`
    lines = body.splitlines(True)
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == "break;":
            indent = lines[i][: len(lines[i]) - len(lines[i].lstrip())]
            lines[i] = f"{indent}return DefWindowProcW(hwnd, msg, wParam, lParam);\n"
            break
    return "".join(lines)


def main_app(lines: list[str]) -> dict[str, str]:
    write_app_state(lines)

    ranges = {
        "app_runtime.cpp": [
            (547, 568),  # PrefetchDetailsMeta
            (609, 688),  # RememberLayoutFocus .. BindCurrentLayout
            (700, 727),  # ResolveOpenFolderPath .. PostWorkerResult
            (733, 770),  # FocusedPaneRect .. ListRect
            (797, 838),  # scrollbar geom + RememberPath
            (840, 1032),  # FillPaneSlots
            (1054, 1997),  # tray/recycle/details/BuildVm/tooltip/paths/tags
            (2075, 2093),  # SyncSavedSearchSidebar
        ],
        "app_ops_ui.cpp": [
            (2005, 2055),
            (2096, 2144),
            (5035, 5057),
            (5417, 5432),
            (5652, 5670),
            (6156, 6244),
        ],
        "app_commands.cpp": [
            (2146, 3006),
            (4655, 5014),
            (5059, 5064),
            (5207, 5374),
            (6136, 6154),
        ],
        "app_input.cpp": [
            (1034, 1052),
            (3008, 3580),
        ],
        "app_navigation.cpp": [
            (3582, 4653),
            (5016, 5033),
            (5068, 5205),
            (5376, 5415),
        ],
        "app_hosted_edit.cpp": [
            (5434, 5650),
            (5672, 5814),
            (5915, 6134),
        ],
        "app_main_keep": [
            (5816, 5913),  # metrics + Render
            (6246, 9959),  # WndProc + WinMain — trimmed later
        ],
    }

    # ClipboardPath definition is 1999-2003; excluded (inline in header).
    # LiveLayout/Panes/Root/LayoutOf/ForEachPane/ActiveTab excluded (inline).

    extracted: dict[str, str] = {}
    used = set()
    for dest, spans in ranges.items():
        if dest == "app_main_keep":
            continue
        parts = []
        for a, b in spans:
            parts.append(slice_lines(lines, a, b))
            used.update(range(a, b + 1))
        extracted[dest] = "".join(parts)
        if dest == "app_runtime.cpp":
            extracted[dest] = re.sub(
                r"\nstruct TrayDeckEntry \{.*?\};\n",
                "\n",
                extracted[dest],
                count=1,
                flags=re.S,
            )

    # Handlers from WndProc
    handlers = [
        ("HandleMouseMove", 6926, 7828),
        ("HandleMouseLeave", 7830, 7849),
        ("HandleLButtonDown", 7851, 8331),
        ("HandleLButtonDblClk", 8333, 8362),
        ("HandleLButtonUp", 8364, 8626),
        ("HandleCaptureChanged", 8628, 8677),
        ("HandleRButtonDown", 8680, 8717),
        ("HandleRButtonUp", 8719, 8802),
        ("HandleMouseWheel", 8804, 8916),
        ("HandleKeyDown", 8925, 9085),
    ]
    handler_src = []
    handler_decls = []
    for name, a, b in handlers:
        used.update(range(a, b + 1))
        raw = slice_lines(lines, a, b)
        # Drop `case WM_*: {` first line and last `    }`
        raw_lines = raw.splitlines(True)
        # first line is `    case WM_...: {`
        body = "".join(raw_lines[1:-1]) if len(raw_lines) >= 2 else raw
        body = prepare_handler(body)
        handler_src.append(
            f"LRESULT {name}(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {{\n{body}}}\n"
        )
        handler_decls.append(
            f"LRESULT {name}(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);"
        )
    extracted["app_input.cpp"] += "\n" + "\n".join(handler_src)

    # g_shell in commands
    extracted["app_commands.cpp"] = (
        "HANDLE g_shell_watch_stop = nullptr;\nHANDLE g_shell_watch_thread = nullptr;\n\n"
        + extracted["app_commands.cpp"]
    )

    files_body = {}
    for dest, body in extracted.items():
        files_body[dest] = wrap_cpp(dest, body)
        (APP / dest).write_text(files_body[dest], encoding="utf-8")

    # Domain headers from decls
    def header_for(name: str, extra: str, body: str) -> str:
        decls = extract_decls(strip_static(body) if "g_shell" not in body[:80] else body)
        # Drop HANDLE globals from decls
        decls = [d for d in decls if "g_shell" not in d and "HANDLE g_" not in d]
        text = extra + "\n".join(decls) + "\n"
        return text

    runtime_h = (
        "// app_runtime.h — Shared layout accessors and window-model helpers.\n"
        "#pragma once\n#include \"app_state.h\"\n\nnamespace pulse {\n"
        + INLINE_RUNTIME
        + "\n"
        + "\n".join(extract_decls(extracted["app_runtime.cpp"]))
        + "\n} // namespace pulse\n"
    )
    (APP / "app_runtime.h").write_text(runtime_h, encoding="utf-8")

    def domain_header(fname: str, comment: str, body_key: str, extra_decls: list[str] | None = None) -> None:
        extra = extra_decls or []
        decls = extra + [d for d in extract_decls(extracted[body_key])
                         if not d.startswith("LRESULT Handle")]
        extra_inc = "#include <commctrl.h>\n" if fname == "app_hosted_edit.h" else ""
        text = (
            f"// {fname} — {comment}\n#pragma once\n#include \"app_runtime.h\"\n{extra_inc}\n"
            "namespace pulse {\n"
            + "\n".join(decls)
            + "\n} // namespace pulse\n"
        )
        (APP / fname).write_text(text, encoding="utf-8")

    domain_header("app_navigation.h", "Navigation, watches, search, and virtual places.", "app_navigation.cpp")
    domain_header("app_ops_ui.h", "Delete/tray/paste/conflict/recycle operations UI.", "app_ops_ui.cpp")
    domain_header("app_commands.h", "Menus, omnibar, view/split, tags, settings chrome.", "app_commands.cpp")
    domain_header("app_hosted_edit.h", "Address/filter/rename overlay editors.", "app_hosted_edit.cpp")
    domain_header(
        "app_input.h",
        "Pointer, keyboard, marquee, and drag/drop input.",
        "app_input.cpp",
        extra_decls=handler_decls,
    )

    internal = r'''// app_internal.h — Aggregate declarations for Pulse window TUs.
#pragma once
#include "app_state.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_input.h"
'''
    (APP / "app_internal.h").write_text(internal, encoding="utf-8")

    return {"used": used, "handlers": handlers}


def rewrite_app_main(lines: list[str], used: set[int], handlers) -> None:
    # Keep prologue includes, drop types, drop extracted functions, thin WndProc.
    keep_prefix = []
    # lines 1-88 includes + pragma + using
    keep_prefix.append("".join(lines[0:88]))
    keep_prefix.append('#include "app_internal.h"\n')
    keep_prefix.append("#include <commctrl.h>\n\n")
    keep_prefix.append("using namespace pulse;\n\n")

    # Keep Render + metrics (5816-5913) and everything from WndProcImpl except extracted cases
    metrics = slice_lines(lines, 5816, 5913)
    metrics = strip_static(metrics)

    wnd = slice_lines(lines, 6246, len(lines))
    wnd = strip_static(wnd)

    replacements = [
        ("HandleMouseMove", "WM_MOUSEMOVE"),
        ("HandleMouseLeave", "WM_MOUSELEAVE"),
        ("HandleLButtonDown", "WM_LBUTTONDOWN"),
        ("HandleLButtonDblClk", "WM_LBUTTONDBLCLK"),
        ("HandleLButtonUp", "WM_LBUTTONUP"),
        ("HandleCaptureChanged", "WM_CAPTURECHANGED"),
        ("HandleRButtonDown", "WM_RBUTTONDOWN"),
        ("HandleRButtonUp", "WM_RBUTTONUP"),
        ("HandleMouseWheel", "WM_MOUSEWHEEL"),
        ("HandleKeyDown", "WM_KEYDOWN"),
    ]
    for name, msg in replacements:
        braced = rf"    case {msg}: \{{.*?\n    \}}\n"
        repl = f"    case {msg}:\n        return {name}(s, hwnd, msg, wParam, lParam);\n"
        wnd, n = re.subn(braced, repl, wnd, count=1, flags=re.S)
        if n == 0:
            unbraced = rf"    case {msg}:\n(?:.*?\n)*?        return 0;\n"
            wnd, n = re.subn(unbraced, repl, wnd, count=1, flags=re.S)
        if n != 1:
            raise SystemExit(f"failed to replace {msg} ({n})")

    # WM_CAPTURECHANGED originally had no extra braces around some versions —
    # already handled if it used `case WM_CAPTURECHANGED:` without brace.
    # If replacement failed we'd have exited.

    out = (
        "// app_main.cpp — Pulse UI process entry point, window, shot mode.\n"
        + keep_prefix[0]
        + keep_prefix[1]
        + keep_prefix[2]
        + keep_prefix[3]
        + metrics
        + "\n"
        + wnd
    )
    # Original file already starts with the comment; keep_prefix[0] includes it. Strip duplicate.
    if out.startswith("// app_main.cpp"):
        # keep_prefix[0] already has the first comment line
        out = keep_prefix[0] + keep_prefix[1] + keep_prefix[2] + keep_prefix[3] + metrics + "\n" + wnd
        # Insert include after original includes: keep_prefix[0] is lines 1-88 which already
        # has using namespace at 88. We added another using. Remove original using at end of prefix.
        pass

    # Fix double using / missing include placement: rewrite more carefully.
    prefix_lines = lines[:88]
    # drop `using namespace pulse;` if present as last
    while prefix_lines and prefix_lines[-1].strip() in ("", "using namespace pulse;"):
        if prefix_lines[-1].strip() == "using namespace pulse;":
            prefix_lines.pop()
            continue
        prefix_lines.pop()
    text = "".join(prefix_lines)
    text += '#include "app_internal.h"\n'
    text += "#include <commctrl.h>\n\n"
    text += "using namespace pulse;\n\n"
    text += metrics
    text += "\n"
    text += wnd
    (APP / "app_main.cpp").write_text(text, encoding="utf-8")


def split_ui(lines: list[str]) -> None:
    # Anonymous namespaces and file-static helpers -> internal header.
    # First anon ns: 30-91, second 93-811, empty layout 3800-3849
    helpers = []
    helpers.append("namespace {\n")
    helpers.append(slice_lines(lines, 31, 90))  # inside first ns without wrappers
    helpers.append("} // namespace\n\n")
    helpers.append("namespace {\n")
    helpers.append(slice_lines(lines, 94, 811))
    helpers.append("} // namespace\n\n")
    helpers.append("namespace {\n")
    helpers.append(slice_lines(lines, 3801, 3848))
    helpers.append("} // namespace\n\n")

    # File-static helpers (outside MainRenderer)
    static_spans = [
        (1150, 1154),  # StepLeftHeaderButton
        (1590, 1598),  # FillRect / FillRoundedRect
        (1611, 1742),  # MeasureTextWidth through MakeBrush
        (1744, 2101),  # TitleChrome through TitleBarCompact
        (4256, 4403),  # FitFileName through LayoutNameTrail
        (4747, 4759),  # ScrollbarMetrics
    ]
    # StepLeftHeaderButton may be longer — read later if compile fails.

    static_bits = [slice_lines(lines, a, b) for a, b in static_spans]
    static_text = "namespace {\n" + strip_static("".join(static_bits)) + "} // namespace\n"

    internal = r'''// ui_renderer_internal.h — Draw + HitTest shared geometry (not a public API).
#pragma once
#include "ui_renderer.h"
#include "../common/localization.h"
#include "typography.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace pulse::ui {

'''
    internal += "".join(helpers)
    internal += static_text
    internal += "\n} // namespace pulse::ui\n"
    (UI / "ui_renderer_internal.h").write_text(internal, encoding="utf-8")

    def method_block(start: int, end: int) -> str:
        return slice_lines(lines, start, end)

    def write_ui_cpp(name: str, comment: str, spans: list[tuple[int, int]]) -> None:
        body = "".join(method_block(a, b) for a, b in spans)
        text = f"// {name} — {comment}\n{UI_CPP_INCLUDES}\n{body}\n}} // namespace pulse::ui\n"
        (UI / name).write_text(text, encoding="utf-8")

    # Remaining in ui_renderer.cpp: ctor through chrome, Render, title/toolbar/status, breadcrumb
    write_ui_cpp(
        "ui_sidebar.cpp",
        "Sidebar and staging-tray deck.",
        [
            (1419, 1442),  # SidebarRect, StagingTrayRect, TrayDeckCapacity
            (2742, 3226),  # DrawSidebar, DrawTrayDeck
            (5593, 5604),  # SidebarMaxScroll
            (5771, 5783),  # TagItemRect
        ],
    )
    write_ui_cpp(
        "ui_pane_list.cpp",
        "Pane, list, empty states, columns, and icons.",
        [
            (843, 1063),  # SVG helpers
            (1111, 1148),
            (1156, 1417),  # pane rects, columns, NameCell, PointInItemName
            (2171, 2190),  # folder/file/entry icons
            (3779, 3798),  # DrawPane start before empty-ns (empty ns moved)
            (3851, 4254),  # empty + single pane + truncated/centered
            (4405, 4760),  # RenameFieldRect, DrawList, ComputeScrollbar leftover
            (4761, 4772),  # DrawScrollbar
            (5606, 5666),  # MaxScroll / item geometry
        ],
    )
    write_ui_cpp(
        "ui_details_panel.cpp",
        "Details panel draw and content height.",
        [
            (1103, 1109),
            (3228, 3777),
        ],
    )
    write_ui_cpp(
        "ui_settings_view.cpp",
        "Settings page layout and painting.",
        [
            (4840, 5591),
        ],
    )
    write_ui_cpp(
        "ui_hit_test.cpp",
        "Hit testing and tab-strip queries.",
        [
            (5741, 6428),
        ],
    )

    # Rebuild ui_renderer.cpp without moved pieces.
    # Keep: includes, namespace, ctor/setters/scale/chrome rects, SplitBreadcrumb,
    # BreadcrumbLayout, DrawTextRect, UpdateBrushes, DrawIcon/Button, Render,
    # DrawTitleBar, DrawToolbar, DrawStatusBar, ComputeTabStrip, LogoBitmap
    keep_spans = [
        (813, 841),    # ctor, SetCompositor, InvalidateTypography
        (1065, 1101),  # notify, scale, sidebar width, ContentRect
        (1444, 1599),  # title/toolbar/address + SplitBreadcrumb + BreadcrumbLayout
        (2103, 2169),  # UpdateBrushes, DrawIconText, DrawButton
        (2192, 2322),  # Render
        (2324, 2740),  # DrawTitleBar, DrawToolbar
        (4774, 4838),  # DrawStatusBar
        (2291, 2322),  # LogoBitmap — overlaps Render? Logo is 2291-2322 before Render 2192
    ]
    # LogoBitmap is 2291-end before Render? Render is 2192, Logo is 2291 — Logo is AFTER Render start?
    # Order in original: Render 2192, DrawTitleBar 2324. LogoBitmap 2291 is inside/before DrawTitleBar.
    # Keep 813-841, 1065-1101, 1444-1599, 2103-2322 (brushes through Logo/end of fps overlay),
    # 2324-2740 title+toolbar, 4774-4838 status, 5668-5739 ComputeTabStrip

    keep = []
    keep.append(slice_lines(lines, 813, 841))
    keep.append(slice_lines(lines, 1065, 1101))
    keep.append(slice_lines(lines, 1444, 1588))
    keep.append(slice_lines(lines, 1600, 1609))  # DrawTextRect
    keep.append(slice_lines(lines, 2103, 2740))  # brushes, icons, buttons, Render, Logo, title, toolbar
    keep.append(slice_lines(lines, 4774, 4838))
    keep.append(slice_lines(lines, 5668, 5739))  # ComputeTabStrip

    text = (
        "// ui_renderer.cpp — Chrome orchestration (title, toolbar, status, render).\n"
        + UI_CPP_INCLUDES
        + "".join(keep)
        + "\n} // namespace pulse::ui\n"
    )
    (UI / "ui_renderer.cpp").write_text(text, encoding="utf-8")


def main() -> None:
    app_lines = read_lines(APP / "app_main.cpp.splitbak")
    info = main_app(app_lines)
    rewrite_app_main(app_lines, info["used"], info["handlers"])
    ui_lines = read_lines(UI / "ui_renderer.cpp.splitbak")
    split_ui(ui_lines)
    print("split complete")
    print("app_main", sum(1 for _ in (APP / "app_main.cpp").open(encoding="utf-8")))
    for p in sorted(APP.glob("app_*.cpp")):
        print(p.name, sum(1 for _ in p.open(encoding="utf-8")))
    for p in sorted(UI.glob("ui_*.cpp")):
        print(p.name, sum(1 for _ in p.open(encoding="utf-8")))


if __name__ == "__main__":
    main()
