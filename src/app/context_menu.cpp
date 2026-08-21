// context_menu.cpp — See context_menu.h.
#include "context_menu.h"
#include "../fs/fs_enum.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string_view>
#include <unordered_set>

namespace pulse::app {

namespace {

// Segoe Fluent Icons codepoints (same font strategy as ui_renderer.cpp).
constexpr const wchar_t* kGlyphOpen = L"\xE8E5";
constexpr const wchar_t* kGlyphCut = L"\xE8C6";
constexpr const wchar_t* kGlyphCopy = L"\xE8C8";
constexpr const wchar_t* kGlyphPaste = L"\xE77F";
constexpr const wchar_t* kGlyphDelete = L"\xE74D";
constexpr const wchar_t* kGlyphRename = L"\xE8AC";
constexpr const wchar_t* kGlyphProperties = L"\xE946";
constexpr const wchar_t* kGlyphTerminal = L"\xE756";
constexpr const wchar_t* kGlyphLink = L"\xE71B";
constexpr const wchar_t* kGlyphUndo = L"\xE7A7";
constexpr const wchar_t* kGlyphNewFolder = L"\xE8F4";
constexpr const wchar_t* kGlyphNewFile = L"\xE8A5";
constexpr const wchar_t* kGlyphSplit = L"\xE8A9";
constexpr const wchar_t* kGlyphLayout = L"\xE8A9";
constexpr const wchar_t* kGlyphFolder = L"\xE8B7";
constexpr const wchar_t* kGlyphOpenInNewTab = L"\xE8A7";
constexpr const wchar_t* kGlyphSearch = L"\xE721";
constexpr const wchar_t* kGlyphTag = L"\xE8EC";
constexpr const wchar_t* kGlyphSettings = L"\xE713";

ui::FluentMenuItem Item(int cmd, const wchar_t* text, const wchar_t* glyph,
                        const wchar_t* shortcut = nullptr, bool enabled = true) {
    ui::FluentMenuItem it;
    it.command = cmd;
    it.text = text;
    it.glyph = glyph;
    if (shortcut) it.shortcut = shortcut;
    it.enabled = enabled;
    return it;
}

ui::FluentMenuItem UndoItem(bool can_undo, const std::wstring& undo_label) {
    auto it = Item(CmdUndo,
        can_undo && !undo_label.empty() ? undo_label.c_str() : L"撤销",
        kGlyphUndo, L"Ctrl+Z", can_undo);
    it.separator_after = false;
    return it;
}

} // namespace

std::vector<ui::FluentMenuItem> BuildItemMenu(bool can_undo, const std::wstring& undo_label,
                                              bool folder) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdOpen, L"打开", kGlyphOpen));

    // 剪切/复制/删除/重命名 live on their shortcuts; collapse them into one
    // icon-button row (like the tag swatch strip) to make room for the
    // Explorer verbs merged below.
    ui::FluentMenuItem strip;
    strip.command = CmdNone; // row itself does nothing; buttons carry commands
    strip.enabled = true;
    strip.quick_swatches = {
        { CmdCut,    {}, false, false, kGlyphCut },
        { CmdCopy,   {}, false, false, kGlyphCopy },
        { CmdDelete, {}, false, false, kGlyphDelete },
        { CmdRename, {}, false, false, kGlyphRename },
    };
    strip.separator_after = true;
    items.push_back(std::move(strip));

    if (folder)
        items.push_back(Item(CmdOpenInNewTab, L"在新标签打开", kGlyphOpenInNewTab));
    items.push_back(Item(CmdCopyPath, L"复制路径", kGlyphLink, L"Ctrl+Shift+C"));
    items.push_back(Item(CmdOpenTerminal, L"在此处打开终端", kGlyphTerminal));
    items.push_back(Item(CmdProperties, L"属性", kGlyphProperties, L"Alt+Enter"));
    items.back().separator_after = true;
    items.push_back(Item(CmdPinWorkspace, L"钉为工作区", kGlyphFolder));
    items.push_back(Item(CmdPinNetwork, L"钉为网络位置", kGlyphLink));
    items.back().separator_after = true;
    items.push_back(Item(CmdTags, L"标签…", kGlyphTag));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries) {
    if (entries.empty()) return;
    constexpr size_t kSectionCap = 48;
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    std::vector<std::wstring> seen;
    for (const auto& it : items)
        if (!it.text.empty()) seen.push_back(lower(it.text));

    size_t added = 0;
    for (const auto& e : entries) {
        if (added >= kSectionCap) break;
        const bool header = !e.children.empty();
        if (e.text.empty() || (e.command == 0 && !header)) continue;
        const std::wstring key = lower(e.text);
        bool dup = false;
        for (const auto& s : seen)
            if (s == key) { dup = true; break; }
        if (dup) continue;
        seen.push_back(key);
        if (added == 0 && !items.empty()) items.back().separator_after = true;
        ui::FluentMenuItem row;
        row.command = e.command;
        row.text = e.text;
        row.enabled = e.enabled;
        // Software-owned submenu: keep the hierarchy as a one-level flyout.
        for (const auto& c : e.children) {
            if (c.text.empty() || c.command == 0) continue;
            ui::FluentMenuItem child;
            child.command = c.command;
            child.text = c.text;
            child.enabled = c.enabled;
            row.children.push_back(std::move(child));
        }
        if (header && row.children.empty()) continue;
        items.push_back(std::move(row));
        ++added;
    }
}

std::vector<ShellMenuEntry> ApplyExplorerPrefs(const ContextMenuPrefs& prefs,
                                               const std::vector<ShellMenuEntry>& entries) {
    bool has_compress_flyout = false;
    for (const auto& e : entries) {
        if (!e.children.empty() && ipc::IsCompressVendorFlyout(e.text)) {
            has_compress_flyout = true;
            break;
        }
    }

    struct Ranked {
        ShellMenuEntry entry;
        int rank = 4;
    };
    std::vector<Ranked> kept;
    kept.reserve(entries.size());
    int mru_kept = 0;
    const int mru_cap = (std::max)(0, prefs.open_with_mru);

    for (const auto& e : entries) {
        if (e.text.empty()) continue;
        const bool flyout = !e.children.empty();
        const auto cat = ipc::ClassifyExplorerItem(e.verb, e.text, flyout);
        const std::wstring key = ipc::CatalogKey(e.text, flyout);
        const bool explicit_on = prefs.item_enabled.count(key) && prefs.item_enabled.at(key);
        if (has_compress_flyout && !flyout && ipc::IsCompressTopLevel(e.text) && !explicit_on)
            continue;
        if (!prefs.ItemEnabled(key, cat, e.from_com)) continue;
        if (cat == ipc::CtxMenuCategory::OpenWith && ipc::IsOpenWithMruText(e.text)) {
            if (mru_kept >= mru_cap) continue;
            ++mru_kept;
        }
        int rank = 4;
        if (flyout) rank = 0;
        else if (cat == ipc::CtxMenuCategory::Software) rank = 1;
        else if (cat == ipc::CtxMenuCategory::OpenWith) rank = 2;
        else if (cat == ipc::CtxMenuCategory::Print) rank = 3;
        kept.push_back({ e, rank });
    }

    std::stable_sort(kept.begin(), kept.end(),
                     [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });

    std::vector<ShellMenuEntry> out;
    const size_t cap = static_cast<size_t>((std::max)(1, prefs.explorer_cap));
    out.reserve((std::min)(kept.size(), cap));
    for (auto& row : kept) {
        if (out.size() >= cap) break;
        out.push_back(std::move(row.entry));
    }
    return out;
}

std::vector<ui::FluentMenuItem> BuildBackgroundMenu(bool can_paste, bool can_undo,
                                                    const std::wstring& undo_label) {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdNewFolder, L"新建文件夹", kGlyphNewFolder, L"F7"));
    items.push_back(Item(CmdNewTextFile, L"新建文本文档", kGlyphNewFile));
    items.back().separator_after = true;
    items.push_back(Item(CmdPaste, L"粘贴", kGlyphPaste, L"Ctrl+V", can_paste));
    items.push_back(Item(CmdCopyPath, L"复制路径", kGlyphLink, L"Ctrl+Shift+C"));
    items.back().separator_after = true;
    items.push_back(Item(CmdOpenTerminal, L"在此处打开终端", kGlyphTerminal));
    items.back().separator_after = true;
    items.push_back(Item(CmdPinWorkspace, L"钉为工作区", kGlyphFolder));
    items.push_back(Item(CmdPinNetwork, L"钉为网络位置", kGlyphLink));
    items.back().separator_after = true;
    items.push_back(UndoItem(can_undo, undo_label));
    return items;
}

std::vector<ui::FluentMenuItem> BuildNewMenu() {
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdNewFolder, L"文件夹", kGlyphNewFolder, L"F7"));
    items.push_back(Item(CmdNewTextFile, L"文本文档", kGlyphNewFile));
    return items;
}

std::vector<ui::FluentMenuItem> BuildSplitMenu(int current_preset) {
    auto mark = [&](int cmd, const wchar_t* text, const wchar_t* shortcut) {
        auto it = Item(cmd, text, kGlyphSplit, shortcut);
        if (cmd - CmdLayoutSingle == current_preset)
            it.text = std::wstring(L"● ") + text;
        return it;
    };
    std::vector<ui::FluentMenuItem> items;
    items.push_back(mark(CmdLayoutSingle, L"单栏", L"Ctrl+1"));
    items.push_back(mark(CmdLayoutTwoVertical, L"左右分栏", L"Ctrl+2"));
    items.push_back(mark(CmdLayoutTwoHorizontal, L"上下分栏", nullptr));
    items.push_back(mark(CmdLayoutThree, L"三栏", L"Ctrl+3"));
    items.push_back(mark(CmdLayoutFourGrid, L"四宫格", L"Ctrl+4"));
    items.back().separator_after = true;
    items.push_back(Item(CmdCopyToTarget, L"复制到目标栏", kGlyphCopy, L"Ctrl+Alt+C"));
    items.push_back(Item(CmdMoveToTarget, L"移动到目标栏", kGlyphCut, L"Ctrl+Alt+X"));
    return items;
}

std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode, bool details_panel) {
    static constexpr const wchar_t* labels[] = {
        L"超大图标", L"大图标", L"中图标", L"小图标",
        L"列表", L"详细信息", L"平铺", L"内容"
    };
    static constexpr const wchar_t* glyphs[] = {
        L"\xE7F4", L"\xE7F4", L"\xE7F4", L"\xECA5",
        L"\xEA37", L"\xE8A5", L"\xECA5", L"\xE8FD"
    };
    std::vector<ui::FluentMenuItem> items;
    items.reserve(9);
    for (int i = 0; i < 8; ++i) {
        auto row = Item(CmdViewBase + i, labels[i], glyphs[i]);
        if (i == 0) row.glyph_scale = 1.16f;
        else if (i == 1) row.glyph_scale = 1.0f;
        else if (i == 2) row.glyph_scale = 0.82f;
        row.radio = ui::ViewModeIndex(current_mode) == i;
        items.push_back(std::move(row));
    }
    items.back().separator_after = true;
    auto panel = Item(CmdDetailsPanel, L"详细信息面板", L"\xE700");
    panel.checked = details_panel;
    items.push_back(std::move(panel));
    return items;
}

static std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

static std::wstring FolderTitle(const std::wstring& path) {
    if (path.empty()) return L"This PC";
    std::wstring shown = DisplayPath(path);
    std::wstring_view v = shown;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    if (pos != std::wstring_view::npos && pos + 1 < v.size())
        return std::wstring(v.substr(pos + 1));
    return std::wstring(v);
}

static bool ContainsI(std::wstring hay, std::wstring needle) {
    for (auto& c : hay) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
    return needle.empty() || hay.find(needle) != std::wstring::npos;
}

static void TrimInPlace(std::wstring& s) {
    const auto start = s.find_first_not_of(L" \t");
    if (start == std::wstring::npos) {
        s.clear();
        return;
    }
    const auto end = s.find_last_not_of(L" \t");
    s = s.substr(start, end - start + 1);
}

OmnibarQuery ParseOmnibarQuery(const std::wstring& query, bool project_only) {
    OmnibarQuery q;
    q.needle = query;
    if (project_only) {
        q.kind = OmnibarQuery::Kind::Project;
        TrimInPlace(q.needle);
        return q;
    }
    if (!q.needle.empty()) {
        const wchar_t c = q.needle[0];
        if (c == L'>') {
            q.kind = OmnibarQuery::Kind::Command;
            q.prefix = L'>';
            q.needle.erase(0, 1);
        } else if (c == L'?' || c == L'/') {
            q.kind = OmnibarQuery::Kind::Search;
            q.prefix = c;
            q.needle.erase(0, 1);
        }
    }
    TrimInPlace(q.needle);
    return q;
}

bool LooksLikeFilesystemPath(const std::wstring& text) {
    std::wstring t = text;
    TrimInPlace(t);
    if (t.size() >= 2) {
        const wchar_t drive = t[0];
        const bool letter = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
        if (letter && t[1] == L':') return true;
        if (t[0] == L'\\' && t[1] == L'\\') return true;
    }
    return t.find(L'\\') != std::wstring::npos;
}

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::vector<std::wstring>& recent_paths) {
    return BuildCommandPalette(L"", recent_paths, {}, false);
}

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::wstring& query,
                                                    const std::vector<std::wstring>& recent_paths,
                                                    const std::vector<index::Hit>& hits,
                                                    bool project_only,
                                                    size_t total,
                                                    const std::wstring& current_path) {
    std::vector<ui::FluentMenuItem> items;
    const OmnibarQuery parsed = ParseOmnibarQuery(query, project_only);
    const std::wstring& needle = parsed.needle;
    const bool command_mode = parsed.kind == OmnibarQuery::Kind::Command;
    const bool search_mode = parsed.kind == OmnibarQuery::Kind::Search;
    const bool path_like = parsed.kind == OmnibarQuery::Kind::Mixed &&
                           LooksLikeFilesystemPath(needle);
    auto add_cmd = [&](int cmd, const wchar_t* text, const wchar_t* glyph, const wchar_t* shortcut) {
        if (!ContainsI(text, needle)) return;
        items.push_back(Item(cmd, text, glyph, shortcut));
    };
    if (!project_only && !search_mode && !path_like) {
        add_cmd(CmdLayoutTwoVertical, L"左右分栏", kGlyphSplit, L"Ctrl+2");
        add_cmd(CmdLayoutFourGrid, L"四宫格", kGlyphLayout, L"Ctrl+4");
        add_cmd(CmdCopyToTarget, L"复制所选到目标栏", kGlyphCopy, L"Ctrl+Alt+C");
        add_cmd(CmdNewFolder, L"新建文件夹", kGlyphNewFolder, L"F7");
        add_cmd(CmdPinWorkspace, L"钉为工作区", kGlyphFolder, nullptr);
        add_cmd(CmdCopyPath, L"复制路径", kGlyphLink, L"Ctrl+Shift+C");
        add_cmd(CmdInstallFullIndex, L"启用全盘索引（安装服务）", kGlyphSearch, nullptr);
        add_cmd(CmdSettings, L"设置", kGlyphSettings, nullptr);
    }
    std::unordered_set<std::wstring> seen_paths;
    if (!command_mode && (!hits.empty() || total > 0)) {
        if (!items.empty()) items.back().separator_after = true;
        for (size_t i = 0; i < hits.size() && i < 48; ++i) {
            items.push_back(Item(CmdIndexBase + static_cast<int>(i),
                hits[i].name.c_str(), hits[i].is_dir ? kGlyphFolder : kGlyphNewFile));
            items.back().text = hits[i].name;
            items.back().shortcut = DisplayPath(hits[i].path);
            const std::wstring n = fs::NormalizePath(hits[i].path);
            if (!n.empty()) seen_paths.insert(n);
        }
        if (!needle.empty() && !project_only) {
            if (!items.empty()) items.back().separator_after = true;
            wchar_t count[64];
            swprintf_s(count, L"%zu 项", total);
            items.push_back(Item(CmdSearchAll, L"在列表中显示全部结果", kGlyphSearch, count));
        }
    }

    const std::wstring current_n = current_path.empty() ? L"" : fs::NormalizePath(current_path);
    constexpr size_t kMaxRecent = 6;
    size_t recent_added = 0;
    bool recent_sep = false;
    for (size_t i = 0; i < recent_paths.size() && recent_added < kMaxRecent; ++i) {
        const std::wstring n = fs::NormalizePath(recent_paths[i]);
        if (n.empty() || !seen_paths.insert(n).second) continue;
        if (!current_n.empty() && n == current_n) continue;
        const std::wstring title = FolderTitle(recent_paths[i]);
        if (!ContainsI(title, needle) && !ContainsI(recent_paths[i], needle) &&
            !ContainsI(DisplayPath(recent_paths[i]), needle))
            continue;
        if (!recent_sep) {
            if (!items.empty()) items.back().separator_after = true;
            recent_sep = true;
        }
        items.push_back(Item(CmdRecentBase + static_cast<int>(i), title.c_str(), kGlyphFolder));
        items.back().text = title;
        items.back().badge_text = L"历史路径";
        ++recent_added;
    }
    return items;
}

std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext) {
    auto exists = [&](const std::wstring& name) {
        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += name;
        return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    std::wstring candidate = base + ext;
    for (int i = 2; exists(candidate); ++i) {
        candidate = base + L" (" + std::to_wstring(i) + L")" + ext;
    }
    return candidate;
}

} // namespace pulse::app
