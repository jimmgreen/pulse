#include "keymap.h"

#include "../common/localization.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace pulse::app {
namespace {

constexpr uint8_t A = kKeyAlt, C = kKeyCtrl, S = kKeyShift;

constexpr KeyChord K(uint8_t mods, uint16_t vk) { return KeyChord{mods, vk}; }
constexpr std::array<KeyChord, kMaxKeyBindings> D(KeyChord a = {}, KeyChord b = {}, KeyChord c = {}) {
    return {a, b, c};
}

using G = KeyGroup;
using N = KeyKind;
using X = KeyAction;

// Same order as KeyAction. Every key here is one HandleKeyDown (app_input.cpp),
// ExplorerShortcut, WM_SYSKEYDOWN (app_main.cpp), the window switcher or the
// overview already handles; shortcut help lists the same rows.
constexpr KeyActionInfo kActions[] = {
    {X::GlobalSearch, "GlobalSearch", G::Global, N::Global, L"呼出全局搜索", L"Open global search", D(K(A, VK_SPACE)), 0, 0, 0, false},
    {X::Open, "Open", G::Navigate, N::Fixed, L"打开", L"Open", D(K(0, VK_RETURN)), 0, 0, 0, false},
    {X::Back, "Back", G::Navigate, N::Normal, L"后退", L"Back", D(K(0, VK_BACK), K(A, VK_LEFT)), 0, 0, 0, false},
    {X::Forward, "Forward", G::Navigate, N::Normal, L"前进", L"Forward", D(K(A, VK_RIGHT)), 0, 0, 0, false},
    {X::Up, "Up", G::Navigate, N::Normal, L"上一级", L"Up one level", D(K(A, VK_UP)), 0, 0, 0, false},
    {X::GoToPath, "GoToPath", G::Navigate, N::Normal, L"转到地址栏", L"Go to the address bar", D(K(C, 'L'), K(A, 'D'), K(0, VK_F4)), 0, 0, 0, false},
    {X::Refresh, "Refresh", G::Navigate, N::Normal, L"刷新", L"Refresh", D(K(0, VK_F5)), 0, 0, 0, false},
    {X::MoveFocus, "MoveFocus", G::Navigate, N::Fixed, L"移动焦点", L"Move the focus", D(K(0, VK_UP), K(0, VK_NEXT), K(0, VK_HOME)), 0, 0, 0, false},
    {X::QuickPreview, "QuickPreview", G::Files, N::Normal, L"快速预览", L"Quick Look", D(K(0, VK_SPACE)), 0, 0, 0, false},
    {X::Copy, "Copy", G::Files, N::Normal, L"复制", L"Copy", D(K(C, 'C')), 0, 0, 0, false},
    {X::Cut, "Cut", G::Files, N::Normal, L"剪切", L"Cut", D(K(C, 'X')), 0, 0, 0, false},
    {X::Paste, "Paste", G::Files, N::Normal, L"粘贴", L"Paste", D(K(C, 'V')), 0, 0, 0, false},
    {X::CopyPath, "CopyPath", G::Files, N::Normal, L"复制路径", L"Copy path", D(K(C | S, 'C')), 0, 0, 0, false},
    {X::StageToTray, "StageToTray", G::Files, N::Normal, L"加入暂存区", L"Add to staging tray", D(K(C | S, 'S')), 0, 0, 0, false},
    {X::Undo, "Undo", G::Files, N::Normal, L"撤销", L"Undo", D(K(C, 'Z')), 0, 0, 0, false},
    {X::Rename, "Rename", G::Files, N::Normal, L"重命名", L"Rename", D(K(0, VK_F2)), 0, 0, 0, false},
    {X::BatchRename, "BatchRename", G::Files, N::Normal, L"批量重命名", L"Batch rename", D(K(C | S, 'R')), 0, 0, 0, false},
    {X::NewFolder, "NewFolder", G::Files, N::Normal, L"新建文件夹", L"New folder", D(K(C | S, 'N'), K(0, VK_F7)), 0, 0, 0, false},
    {X::GroupIntoFolder, "GroupIntoFolder", G::Files, N::Normal, L"合并到文件夹", L"New folder with selection", D(K(C, 'G')), 0, 0, 0, false},
    {X::DissolveFolder, "DissolveFolder", G::Files, N::Normal, L"解散文件夹", L"Ungroup folder", D(K(C | S, 'G')), 0, 0, 0, false},
    {X::Delete, "Delete", G::Files, N::Normal, L"删除", L"Delete", D(K(0, VK_DELETE), K(C, 'D')), 0, 0, 0, false},
    {X::PermanentDelete, "PermanentDelete", G::Files, N::Normal, L"永久删除", L"Delete permanently", D(K(S, VK_DELETE), K(C | S, 'D')), 0, 0, 0, false},
    {X::Properties, "Properties", G::Files, N::Normal, L"属性", L"Properties", D(K(A, VK_RETURN)), 0, 0, 0, false},
    {X::ContextMenu, "ContextMenu", G::Files, N::Normal, L"打开右键菜单", L"Open the context menu", D(K(S, VK_F10), K(0, VK_APPS)), 0, 0, 0, false},
    {X::ToggleTag, "ToggleTag", G::Files, N::Range, L"切换第 N 个常用标签", L"Toggle favorite tag N", D(), A, 1, 9, false},
    {X::ClearTags, "ClearTags", G::Files, N::Normal, L"清除所有标签", L"Clear all tags", D(K(A, '0')), 0, 0, 0, false},
    {X::TagPanel, "TagPanel", G::Files, N::Normal, L"标签面板", L"Tag panel", D(K(A, 'T')), 0, 0, 0, false},
    {X::SelectAll, "SelectAll", G::Select, N::Normal, L"全选", L"Select all", D(K(C, 'A')), 0, 0, 0, false},
    {X::InvertSelection, "InvertSelection", G::Select, N::Normal, L"反选", L"Invert selection", D(K(C, 'I')), 0, 0, 0, false},
    {X::SelectPattern, "SelectPattern", G::Select, N::Normal, L"按规则选择", L"Select by pattern", D(K(C | S, 'A')), 0, 0, 0, false},
    {X::Search, "Search", G::Search, N::Normal, L"搜索", L"Search", D(K(C, 'K')), 0, 0, 0, false},
    {X::CommandBar, "CommandBar", G::Search, N::Normal, L"命令栏", L"Command bar", D(K(C | S, 'K')), 0, 0, 0, false},
    {X::Project, "Project", G::Search, N::Normal, L"项目", L"Projects", D(K(C, 'P')), 0, 0, 0, false},
    {X::Filter, "Filter", G::Search, N::Normal, L"筛选当前文件夹", L"Filter this folder", D(K(C, 'F'), K(C, 'E'), K(0, VK_F3)), 0, 0, 0, false},
    {X::AdvancedSearch, "AdvancedSearch", G::Search, N::Normal, L"高级搜索", L"Advanced search", D(K(C | S, 'F')), 0, 0, 0, false},
    {X::NewTab, "NewTab", G::Tabs, N::Normal, L"新建标签页", L"New tab", D(K(C, 'T')), 0, 0, 0, false},
    {X::CloseTab, "CloseTab", G::Tabs, N::Normal, L"关闭标签页", L"Close tab", D(K(C, 'W')), 0, 0, 0, false},
    {X::NextTab, "NextTab", G::Tabs, N::Normal, L"下一个标签页", L"Next tab", D(K(C, VK_TAB)), 0, 0, 0, true},
    {X::TabNumber, "TabNumber", G::Tabs, N::Range, L"转到第 N 个标签页", L"Go to tab N", D(), C, 1, 9, false},
    {X::Layout, "Layout", G::Tabs, N::Range, L"分栏布局", L"Pane layout", D(), static_cast<uint8_t>(C | A), 1, 4, false},
    {X::ViewMode, "ViewMode", G::Tabs, N::Range, L"视图模式", L"View mode", D(), static_cast<uint8_t>(C | S), 1, 8, false},
    {X::NextPane, "NextPane", G::Tabs, N::Normal, L"切换到下一栏", L"Next pane", D(K(0, VK_F6)), 0, 0, 0, false},
    {X::MarkTarget, "MarkTarget", G::Tabs, N::Normal, L"设为目标栏", L"Mark as target pane", D(K(C | A, 'D')), 0, 0, 0, false},
    {X::CopyToTarget, "CopyToTarget", G::Tabs, N::Normal, L"复制到目标栏", L"Copy to target pane", D(K(C | A, 'C')), 0, 0, 0, false},
    {X::MoveToTarget, "MoveToTarget", G::Tabs, N::Normal, L"移动到目标栏", L"Move to target pane", D(K(C | A, 'X')), 0, 0, 0, false},
    {X::Sidebar, "Sidebar", G::Tabs, N::Normal, L"折叠侧栏", L"Collapse the sidebar", D(K(C, 'B')), 0, 0, 0, false},
    {X::FavoritesBar, "FavoritesBar", G::Tabs, N::Normal, L"显示收藏栏", L"Show the favorites bar", D(K(C | S, 'B')), 0, 0, 0, false},
    {X::Maximize, "Maximize", G::Tabs, N::Normal, L"最大化", L"Maximize", D(K(0, VK_F11)), 0, 0, 0, false},
    // Windows dispatches this through the configured shell integration.
    {X::NewWindow, "NewWindow", G::Windows, N::Normal, L"新建窗口", L"New window", D(K(C, 'N')), 0, 0, 0, false},
    {X::CloseWindow, "CloseWindow", G::Windows, N::Normal, L"关闭窗口", L"Close window", D(K(C | S, 'W')), 0, 0, 0, false},
    {X::TabToWindow, "TabToWindow", G::Windows, N::Normal, L"标签页移到新窗口", L"Move tab to a new window", D(K(C | S, 'M')), 0, 0, 0, false},
    {X::ReopenClosed, "ReopenClosed", G::Windows, N::Normal, L"重新打开关闭的窗口", L"Reopen what was closed", D(K(C | S, 'T')), 0, 0, 0, false},
    {X::SwitchWindow, "SwitchWindow", G::Windows, N::Normal, L"切换窗口", L"Switch windows", D(K(C, VK_OEM_3)), 0, 0, 0, true},
    {X::Overview, "Overview", G::Windows, N::Normal, L"窗口总览", L"Window overview", D(K(C | S, 'O')), 0, 0, 0, false},
    {X::MergeWindows, "MergeWindows", G::Windows, N::Normal, L"合并所有窗口", L"Merge all windows", D(K(C | S, 'J')), 0, 0, 0, false},
    {X::AiAssistant, "AiAssistant", G::Windows, N::Normal, L"Pulse 助手", L"Pulse Assistant", D(K(C, 'J')), 0, 0, 0, false},
    {X::AiHistory, "AiHistory", G::Windows, N::Normal, L"Pulse 助手历史对话", L"Pulse Assistant chat history", D(K(C, 'H')), 0, 0, 0, false},
    {X::ShortcutHelp, "ShortcutHelp", G::Windows, N::Normal, L"键盘快捷键", L"Keyboard shortcuts", D(K(C, VK_OEM_2)), 0, 0, 0, false},
    {X::ArrangeSideBySide, "ArrangeSideBySide", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
    {X::ArrangeStacked, "ArrangeStacked", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
    {X::ArrangeColumns, "ArrangeColumns", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
    {X::ArrangeTile, "ArrangeTile", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
    {X::ArrangeMain, "ArrangeMain", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
    {X::ArrangeCascade, "ArrangeCascade", G::Windows, N::Normal, L"", L"", D(), 0, 0, 0, false},
};
static_assert(std::size(kActions) == static_cast<size_t>(kKeyActionCount));

constexpr size_t Index(KeyAction action) noexcept { return static_cast<size_t>(action); }

// 移动焦点: arrows, PgUp/PgDn, Home/End, alone or with Shift (extends the selection).
bool IsMoveFocusChord(KeyChord chord) noexcept {
    if (chord.mods != 0 && chord.mods != kKeyShift) return false;
    switch (chord.vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
        return true;
    default:
        return false;
    }
}

struct NamedKey {
    uint16_t vk;
    const wchar_t* name;
};
constexpr NamedKey kNamedKeys[] = {
    {VK_SPACE, L"Space"}, {VK_RETURN, L"Enter"}, {VK_TAB, L"Tab"}, {VK_BACK, L"Backspace"},
    {VK_DELETE, L"Delete"}, {VK_INSERT, L"Insert"}, {VK_HOME, L"Home"}, {VK_END, L"End"},
    {VK_PRIOR, L"PgUp"}, {VK_NEXT, L"PgDn"}, {VK_LEFT, L"Left"}, {VK_RIGHT, L"Right"},
    {VK_UP, L"Up"}, {VK_DOWN, L"Down"}, {VK_ESCAPE, L"Esc"}, {VK_APPS, L"Apps"},
    {VK_PAUSE, L"Pause"}, {VK_SNAPSHOT, L"PrtSc"},
};

const wchar_t* NamedKeyText(uint16_t vk) noexcept {
    for (const auto& key : kNamedKeys)
        if (key.vk == vk) return key.name;
    return nullptr;
}

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

bool Contains(const std::vector<KeyChord>& chords, KeyChord chord) noexcept {
    return std::find(chords.begin(), chords.end(), chord) != chords.end();
}

void PushUnique(std::vector<KeyChord>& chords, KeyChord chord) {
    if (!chord.empty() && !Contains(chords, chord)) chords.push_back(chord);
}

std::string Narrow(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

bool IsDigitKey(uint16_t vk) noexcept { return vk >= '0' && vk <= '9'; }

Keymap g_active;

} // namespace

const KeyActionInfo& ActionInfo(KeyAction action) noexcept {
    const size_t i = Index(action);
    return kActions[i < std::size(kActions) ? i : 0];
}

const wchar_t* ActionName(KeyAction action) {
    const auto& info = ActionInfo(action);
    switch (info.action) {
    case KeyAction::ArrangeSideBySide: return l10n::Get(l10n::StringId::ArrangeSideBySide).c_str();
    case KeyAction::ArrangeStacked: return l10n::Get(l10n::StringId::ArrangeStacked).c_str();
    case KeyAction::ArrangeColumns: return l10n::Get(l10n::StringId::ArrangeColumns).c_str();
    case KeyAction::ArrangeTile: return l10n::Get(l10n::StringId::ArrangeTile).c_str();
    case KeyAction::ArrangeMain: return l10n::Get(l10n::StringId::ArrangeMain).c_str();
    case KeyAction::ArrangeCascade: return l10n::Get(l10n::StringId::ArrangeCascade).c_str();
    default: break;
    }
    return l10n::Pick(info.zh, info.en);
}

const wchar_t* GroupName(KeyGroup group) {
    switch (group) {
    case KeyGroup::Global: return l10n::Get(l10n::StringId::SystemWide).c_str();
    case KeyGroup::Navigate: return l10n::Get(l10n::StringId::Navigation).c_str();
    case KeyGroup::Files: return l10n::Get(l10n::StringId::Files).c_str();
    case KeyGroup::Select: return l10n::Get(l10n::StringId::Selection).c_str();
    case KeyGroup::Search: return l10n::Get(l10n::StringId::AdvSearchRun).c_str();
    case KeyGroup::Tabs: return l10n::Get(l10n::StringId::HelpGroupTabs).c_str();
    case KeyGroup::Windows: return l10n::Get(l10n::StringId::HelpGroupWindows).c_str();
    default: return L"";
    }
}

KeyAction ActionFromId(std::string_view id) noexcept {
    for (const auto& info : kActions)
        if (id == info.id) return info.action;
    return KeyAction::Count;
}

uint16_t SwitcherVirtualKey() noexcept {
    const UINT vk = MapVirtualKeyW(0x29, MAPVK_VSC_TO_VK);
    return static_cast<uint16_t>(vk ? vk : VK_OEM_3);
}

std::vector<KeyChord> DefaultBindings(KeyAction action) {
    const auto& info = ActionInfo(action);
    std::vector<KeyChord> out;
    if (info.kind == KeyKind::Range) return out;
    if (action == KeyAction::SwitchWindow) {
        out.push_back(K(C, SwitcherVirtualKey()));
        return out;
    }
    for (const KeyChord chord : info.defaults)
        if (!chord.empty()) out.push_back(chord);
    return out;
}

bool IsModifierKey(UINT vk) noexcept {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

KeyChord CurrentChord(UINT vk) {
    uint8_t mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kKeyCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= kKeyAlt;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= kKeyShift;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= kKeyWin;
    return KeyChord{mods, static_cast<uint16_t>(IsModifierKey(vk) ? 0 : vk)};
}

std::wstring FormatChord(KeyChord chord) {
    std::wstring out;
    if (chord.mods & kKeyCtrl) out += L"Ctrl+";
    if (chord.mods & kKeyAlt) out += L"Alt+";
    if (chord.mods & kKeyShift) out += L"Shift+";
    if (chord.mods & kKeyWin) out += L"Win+";
    const uint16_t vk = chord.vk;
    if ((vk >= 'A' && vk <= 'Z') || IsDigitKey(vk)) {
        out.push_back(static_cast<wchar_t>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        out += L"F" + std::to_wstring(vk - VK_F1 + 1);
    } else if (const wchar_t* name = NamedKeyText(vk)) {
        out += name;
    } else {
        wchar_t hex[8]{};
        swprintf_s(hex, L"#%02X", static_cast<unsigned>(vk & 0xFF));
        out += hex;
    }
    return out;
}

bool ParseChord(std::wstring_view text, KeyChord& chord) {
    KeyChord out{};
    while (!text.empty()) {
        const size_t plus = text.find(L'+', 1);  // "Ctrl++"-style keys are not used
        const std::wstring_view part = text.substr(0, plus);
        text = plus == std::wstring_view::npos ? std::wstring_view{} : text.substr(plus + 1);
        if (part.empty()) return false;
        if (EqualsNoCase(part, L"Ctrl")) { out.mods |= kKeyCtrl; continue; }
        if (EqualsNoCase(part, L"Alt")) { out.mods |= kKeyAlt; continue; }
        if (EqualsNoCase(part, L"Shift")) { out.mods |= kKeyShift; continue; }
        if (EqualsNoCase(part, L"Win")) { out.mods |= kKeyWin; continue; }
        if (out.vk != 0 || !text.empty()) return false;  // the key comes last, once
        if (part.size() == 1 && ((std::towupper(part[0]) >= L'A' && std::towupper(part[0]) <= L'Z') ||
                                 (part[0] >= L'0' && part[0] <= L'9'))) {
            out.vk = static_cast<uint16_t>(std::towupper(part[0]));
        } else if ((part[0] == L'F' || part[0] == L'f') && part.size() <= 3 && part.size() >= 2) {
            int n = 0;
            for (size_t i = 1; i < part.size(); ++i) {
                if (part[i] < L'0' || part[i] > L'9') return false;
                n = n * 10 + (part[i] - L'0');
            }
            if (n < 1 || n > 24) return false;
            out.vk = static_cast<uint16_t>(VK_F1 + n - 1);
        } else if (part[0] == L'#' && part.size() == 3) {
            const unsigned long v = wcstoul(std::wstring(part.substr(1)).c_str(), nullptr, 16);
            if (v == 0 || v > 0xFE) return false;
            out.vk = static_cast<uint16_t>(v);
        } else {
            for (const auto& key : kNamedKeys)
                if (EqualsNoCase(part, key.name)) out.vk = key.vk;
            if (out.vk == 0) return false;
        }
    }
    if (out.vk == 0 || IsModifierKey(out.vk)) return false;
    chord = out;
    return true;
}

std::wstring KeyCapText(uint16_t vk) {
    switch (vk) {
    case VK_LEFT: return L"\x2190";
    case VK_RIGHT: return L"\x2192";
    case VK_UP: return L"\x2191";
    case VK_DOWN: return L"\x2193";
    default: break;
    }
    if ((vk >= 'A' && vk <= 'Z') || IsDigitKey(vk)) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (const wchar_t* name = NamedKeyText(vk)) return name;
    // Punctuation: the character this layout puts on the key (` on US, ^ on German).
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;  // high bit: dead key
    if (ch >= 0x21) return std::wstring(1, static_cast<wchar_t>(ch));
    wchar_t name[32]{};
    if (const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 32) > 0)
        return name;
    return FormatChord(KeyChord{0, vk});
}

std::wstring ModsDisplay(uint8_t mods) {
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += L'+';
        out += part;
    };
    if (mods & kKeyCtrl) add(L"Ctrl");
    if (mods & kKeyAlt) add(L"Alt");
    if (mods & kKeyShift) add(L"Shift");
    if (mods & kKeyWin) add(L"Win");
    return out;
}

std::vector<std::wstring> ChordCaps(KeyChord chord) {
    std::vector<std::wstring> caps;
    if (chord.mods & kKeyCtrl) caps.emplace_back(L"Ctrl");
    if (chord.mods & kKeyAlt) caps.emplace_back(L"Alt");
    if (chord.mods & kKeyShift) caps.emplace_back(L"Shift");
    if (chord.mods & kKeyWin) caps.emplace_back(L"Win");
    if (chord.vk) caps.push_back(KeyCapText(chord.vk));
    return caps;
}

std::wstring ChordDisplay(KeyChord chord) {
    std::wstring out;
    for (const auto& cap : ChordCaps(chord)) {
        if (!out.empty()) out += L'+';
        out += cap;
    }
    return out;
}

// ---- Keymap -----------------------------------------------------------------

Keymap::Keymap() { ResetAll(); }

void Keymap::ResetAll() {
    for (int i = 0; i < kKeyActionCount; ++i) Reset(static_cast<KeyAction>(i));
}

void Keymap::Reset(KeyAction action) {
    const size_t i = Index(action);
    if (i >= bind_.size()) return;
    if (action == KeyAction::GlobalSearch) return;  // AppPrefs::global_search_* own it
    bind_[i] = DefaultBindings(action);
    range_mods_[i] = ActionInfo(action).range_mods;
}

const std::vector<KeyChord>& Keymap::Bindings(KeyAction action) const {
    static const std::vector<KeyChord> none;
    const size_t i = Index(action);
    return i < bind_.size() ? bind_[i] : none;
}

uint8_t Keymap::RangeMods(KeyAction action) const {
    const size_t i = Index(action);
    return i < range_mods_.size() ? range_mods_[i] : 0;
}

void Keymap::SetBindings(KeyAction action, std::vector<KeyChord> chords) {
    const size_t i = Index(action);
    const auto& info = ActionInfo(action);
    if (i >= bind_.size() || info.kind != KeyKind::Normal) return;
    std::vector<KeyChord> unique;
    for (const KeyChord chord : chords)
        if (unique.size() < kMaxKeyBindings) PushUnique(unique, chord);
    bind_[i] = std::move(unique);
}

void Keymap::SetRangeMods(KeyAction action, uint8_t mods) {
    const size_t i = Index(action);
    if (i >= range_mods_.size() || ActionInfo(action).kind != KeyKind::Range) return;
    range_mods_[i] = static_cast<uint8_t>(mods & (kKeyCtrl | kKeyAlt | kKeyShift));
}

void Keymap::SetGlobalHotkey(KeyChord chord, bool enabled) {
    global_ = chord;
    global_enabled_ = enabled && !chord.empty();
    bind_[Index(KeyAction::GlobalSearch)] = global_enabled_ ? std::vector<KeyChord>{chord} : std::vector<KeyChord>{};
}

bool Keymap::IsDefault(KeyAction action) const {
    const auto& info = ActionInfo(action);
    if (info.kind == KeyKind::Global) {
        const KeyChord def = info.defaults[0];
        return global_ == def || global_.empty();
    }
    if (info.kind == KeyKind::Range) return RangeMods(action) == info.range_mods;
    return Bindings(action) == DefaultBindings(action);
}

bool Keymap::AllDefault() const {
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        if (ActionInfo(action).kind == KeyKind::Global) continue;
        if (!IsDefault(action)) return false;
    }
    return true;
}

Keymap Keymap::FromPrefs(const std::vector<std::wstring>& lines) {
    Keymap map;
    for (const auto& line : lines) {
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        const KeyAction action = ActionFromId(Narrow(std::wstring_view(line).substr(0, eq)));
        if (action == KeyAction::Count) continue;  // an action this build does not know
        const auto& info = ActionInfo(action);
        const std::wstring_view value = std::wstring_view(line).substr(eq + 1);
        if (info.kind == KeyKind::Range) {
            KeyChord probe{};
            // "Ctrl+Alt" is stored without a key: parse it with a dummy digit.
            if (ParseChord(std::wstring(value) + L"+1", probe) && (probe.mods & (kKeyCtrl | kKeyAlt)))
                map.SetRangeMods(action, probe.mods);
            continue;
        }
        if (info.kind != KeyKind::Normal) continue;
        std::vector<KeyChord> chords;
        bool ok = true;
        size_t start = 0;
        while (start <= value.size() && !value.empty()) {
            const size_t bar = value.find(L'|', start);
            const std::wstring_view part = value.substr(start, bar == std::wstring_view::npos ? std::wstring_view::npos : bar - start);
            KeyChord chord{};
            if (!part.empty()) {
                if (ParseChord(part, chord)) chords.push_back(chord);
                else ok = false;
            }
            if (bar == std::wstring_view::npos) break;
            start = bar + 1;
        }
        if (ok) map.SetBindings(action, std::move(chords));  // "Id=" leaves it without a key
    }
    return map;
}

std::vector<std::wstring> Keymap::ToPrefs() const {
    std::vector<std::wstring> out;
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        const auto& info = ActionInfo(action);
        if (info.kind == KeyKind::Fixed || info.kind == KeyKind::Global || IsDefault(action)) continue;
        std::wstring line;
        for (const char* p = info.id; *p; ++p) line.push_back(static_cast<wchar_t>(*p));
        line += L'=';
        if (info.kind == KeyKind::Range) {
            line += ModsDisplay(RangeMods(action));
        } else {
            const auto& chords = Bindings(action);
            for (size_t k = 0; k < chords.size(); ++k) {
                if (k) line += L'|';
                line += FormatChord(chords[k]);
            }
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<KeyChord> Keymap::RangeChords(KeyAction action, uint8_t mods) const {
    const auto& info = ActionInfo(action);
    std::vector<KeyChord> out;
    if (info.kind != KeyKind::Range) return out;
    for (int d = info.range_first; d <= info.range_last; ++d)
        out.push_back(KeyChord{mods, static_cast<uint16_t>('0' + d)});
    return out;
}

std::vector<KeyChord> Keymap::EffectiveChords(KeyAction action) const {
    const auto& info = ActionInfo(action);
    if (info.kind == KeyKind::Range) return RangeChords(action, RangeMods(action));
    return Bindings(action);
}

Keymap::Owner Keymap::Find(KeyChord chord, KeyAction except) const {
    Owner owner;
    if (chord.empty()) return owner;
    if (except != KeyAction::MoveFocus && IsMoveFocusChord(chord)) {
        owner.action = KeyAction::MoveFocus;
        return owner;
    }
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        if (action == except || action == KeyAction::MoveFocus) continue;
        const auto& info = ActionInfo(action);
        if (info.kind == KeyKind::Range) {
            const uint8_t mods = RangeMods(action);
            if (chord.mods == mods && IsDigitKey(chord.vk) &&
                chord.vk - '0' >= info.range_first && chord.vk - '0' <= info.range_last) {
                owner.action = action;
                owner.digit = chord.vk - '0';
                return owner;
            }
            continue;
        }
        if (info.kind == KeyKind::Global && !global_enabled_) continue;
        for (const KeyChord bound : Bindings(action)) {
            if (bound == chord) {
                owner.action = action;
                return owner;
            }
            if (info.shift_reverses && !(chord.mods & kKeyAlt) && bound.vk == chord.vk &&
                bound.mods == (chord.mods & ~kKeyShift) && (chord.mods & kKeyShift)) {
                owner.action = action;
                owner.reverse = true;
                return owner;
            }
        }
    }
    return owner;
}

Keymap::Translation Keymap::Translate(KeyChord pressed) const {
    Translation t;
    if (pressed.empty()) return t;
    const Owner owner = Find(pressed);
    if (owner.found()) {
        const auto& info = ActionInfo(owner.action);
        t.action = owner.action;
        if (info.kind == KeyKind::Fixed || info.kind == KeyKind::Global) return t;
        if (info.kind == KeyKind::Range) {
            t.target = KeyChord{info.range_mods, pressed.vk};
        } else {
            const std::vector<KeyChord> defaults = DefaultBindings(owner.action);
            if (defaults.empty()) return t;
            for (const KeyChord def : defaults) {
                if (def == pressed) return t;  // a default key still in place
                if (owner.reverse && def.vk == pressed.vk && def.mods == (pressed.mods & ~kKeyShift)) return t;
            }
            t.target = defaults.front();
            if (owner.reverse) t.target.mods |= kKeyShift;
        }
        if (t.target == pressed) return t;
        t.kind = Translation::Rewrite;
        return t;
    }
    // Not the user's key for anything: still a default key of an action that
    // moved elsewhere? Then it must not run that action any more.
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        const auto& info = ActionInfo(action);
        if (info.kind == KeyKind::Fixed || info.kind == KeyKind::Global) continue;
        if (info.kind == KeyKind::Range) {
            if (pressed.mods == info.range_mods && IsDigitKey(pressed.vk) &&
                pressed.vk - '0' >= info.range_first && pressed.vk - '0' <= info.range_last) {
                t.kind = Translation::Swallow;
                t.action = action;
                return t;
            }
            continue;
        }
        for (const KeyChord def : DefaultBindings(action)) {
            const bool hit = def == pressed ||
                (info.shift_reverses && (pressed.mods & kKeyShift) && def.vk == pressed.vk &&
                 def.mods == (pressed.mods & ~kKeyShift));
            if (hit) {
                t.kind = Translation::Swallow;
                t.action = action;
                return t;
            }
        }
    }
    return t;
}

const Keymap& ActiveKeymap() { return g_active; }

int WorkspaceShortcutIndex(const Keymap& keymap, KeyChord chord) {
    if (chord.mods != (kKeyCtrl | kKeyAlt) || chord.vk < '1' || chord.vk > '9') return -1;
    const auto route = keymap.Translate(chord);
    if (route.kind != Keymap::Translation::Pass || route.action != KeyAction::Count) return -1;
    return chord.vk - '1';
}
void SetActiveKeymap(Keymap keymap) { g_active = std::move(keymap); }

// ---- conflict checks --------------------------------------------------------

bool ShortcutEnv::IsTaken(KeyChord chord) const noexcept { return Contains(taken, chord); }

bool ShortcutEnv::ImeApplies(KeyChord chord) const noexcept {
    return chinese_ime && ImeConflictName(chord) != nullptr && !Contains(ime_ignored, chord);
}

const wchar_t* ImeConflictName(KeyChord chord) {
    // 微软拼音's default keys (设置 > 时间和语言 > 微软拼音 > 按键).
    if (chord == K(C | S, 'F')) return l10n::Get(l10n::StringId::SimplifiedTraditional).c_str();
    if (chord == K(C, VK_SPACE)) return l10n::Get(l10n::StringId::ChineseEnglishInput).c_str();
    if (chord == K(S, VK_SPACE)) return l10n::Get(l10n::StringId::FullHalfWidth).c_str();
    if (chord == K(C, VK_OEM_PERIOD)) return l10n::Get(l10n::StringId::ChineseEnglishPunctuation).c_str();
    return nullptr;
}

const wchar_t* ReservedName(KeyChord chord) {
    if (chord == K(A, VK_F4)) return l10n::Get(l10n::StringId::CloseWindow).c_str();
    if (chord == K(A, VK_TAB) || chord == K(A | S, VK_TAB)) return l10n::Get(l10n::StringId::SwitchApps).c_str();
    if (chord == K(A, VK_ESCAPE)) return l10n::Get(l10n::StringId::SwitchApps).c_str();
    if (chord == K(C, VK_ESCAPE)) return l10n::Get(l10n::StringId::StartMenu).c_str();
    if (chord == K(C | S, VK_ESCAPE)) return l10n::Get(l10n::StringId::TaskManager).c_str();
    if (chord == K(A, VK_SPACE)) return l10n::Get(l10n::StringId::WindowMenu).c_str();
    if (chord == K(C | A, VK_DELETE)) return l10n::Get(l10n::StringId::SecurityOptions).c_str();
    if (chord.mods == 0 && (chord.vk == VK_ESCAPE || chord.vk == VK_RETURN))
        return l10n::Get(l10n::StringId::CancelOpen).c_str();
    if (chord.vk == VK_F12 && chord.mods == 0) return l10n::Get(l10n::StringId::Debugger).c_str();
    return nullptr;
}

bool IsTextEditChord(KeyChord chord) noexcept {
    if (chord.mods == kKeyCtrl)
        return chord.vk == 'A' || chord.vk == 'C' || chord.vk == 'V' || chord.vk == 'X' || chord.vk == 'Z' ||
               chord.vk == 'Y' || chord.vk == VK_LEFT || chord.vk == VK_RIGHT || chord.vk == VK_BACK ||
               chord.vk == VK_DELETE;
    if (chord.mods == 0 || chord.mods == kKeyShift)
        return chord.vk == VK_HOME || chord.vk == VK_END || chord.vk == VK_DELETE || chord.vk == VK_BACK;
    return false;
}

ChordVerdict CheckChord(const Keymap& keymap, KeyAction action, KeyChord chord, const ShortcutEnv& env) {
    ChordVerdict v;
    auto set = [&](ChordIssue issue, ChordLevel level) {
        v.issue = issue;
        v.level = level;
        return v;
    };
    const auto& info = ActionInfo(action);
    const bool global = info.kind == KeyKind::Global;
    if (chord.empty()) return set(ChordIssue::Incomplete, ChordLevel::Info);
    if ((chord.mods & kKeyWin) && !global) return set(ChordIssue::WinKey, ChordLevel::Block);
    if (const wchar_t* reserved = ReservedName(chord)) {
        (void)reserved;
        // The global hotkey may take Alt+Space (it is the default) but not the rest.
        const bool allowed = global && chord == K(A, VK_SPACE);
        if (!allowed) return set(ChordIssue::Reserved, ChordLevel::Block);
    }
    const bool plain = (chord.mods & ~kKeyShift) == 0;
    const bool printable = (chord.vk >= 'A' && chord.vk <= 'Z') || IsDigitKey(chord.vk) ||
                           (chord.vk >= VK_OEM_1 && chord.vk <= VK_OEM_3) ||
                           (chord.vk >= VK_OEM_4 && chord.vk <= VK_OEM_8) || chord.vk == VK_OEM_102;
    if (plain && printable) return set(ChordIssue::PlainKey, ChordLevel::Block);
    if (global && plain) return set(ChordIssue::PlainKey, ChordLevel::Block);
    if (info.shift_reverses) {
        if (action == KeyAction::SwitchWindow && (!(chord.mods & kKeyCtrl) || (chord.mods & kKeyAlt)))
            return set(ChordIssue::NeedsCtrl, ChordLevel::Block);
        if (chord.mods & kKeyShift) return set(ChordIssue::ShiftReserved, ChordLevel::Block);
    }
    if (Contains(keymap.Bindings(action), chord)) return set(ChordIssue::Same, ChordLevel::Info);
    const bool own_global = global && keymap.GlobalHotkeyEnabled() && keymap.GlobalHotkey() == chord;
    if (global && env.IsTaken(chord) && !own_global) return set(ChordIssue::Taken, ChordLevel::Block);
    const Keymap::Owner other = keymap.Find(chord, action);
    if (other.found()) {
        v.other = other;
        const auto kind = ActionInfo(other.action).kind;
        if (kind == KeyKind::Fixed || kind == KeyKind::Range || other.reverse)
            return set(ChordIssue::DuplicateFixed, ChordLevel::Block);
        return set(ChordIssue::Duplicate, ChordLevel::Warn);
    }
    if (env.ImeApplies(chord)) return set(ChordIssue::Ime, ChordLevel::Warn);
    if (IsTextEditChord(chord)) return set(ChordIssue::TextEdit, ChordLevel::Info);
    return set(ChordIssue::None, ChordLevel::Ok);
}

RangeVerdict CheckRange(const Keymap& keymap, KeyAction action, uint8_t mods, const ShortcutEnv& env) {
    RangeVerdict v;
    if (ActionInfo(action).kind != KeyKind::Range) return v;
    if (!(mods & (kKeyCtrl | kKeyAlt))) {
        v.issue = ChordIssue::RangeNeedsMod;
        v.level = ChordLevel::Block;
        return v;
    }
    for (const KeyChord chord : keymap.RangeChords(action, mods)) {
        RangeDigitStatus d;
        d.chord = chord;
        if (const auto other = keymap.Find(chord, action); other.found()) {
            d.issue = ChordIssue::DuplicateFixed;
            d.level = ChordLevel::Block;
            d.other = other;
        } else if (env.ImeApplies(chord)) {
            d.issue = ChordIssue::Ime;
            d.level = ChordLevel::Warn;
        }
        if (d.level == ChordLevel::Block) ++v.bad;
        v.digits.push_back(d);
    }
    if (mods == keymap.RangeMods(action)) {
        v.issue = ChordIssue::RangeSame;
        v.level = ChordLevel::Info;
    } else if (v.bad > 0) {
        v.issue = ChordIssue::RangeBad;
        v.level = ChordLevel::Block;
    }
    return v;
}

std::vector<BindingIssue> CollectIssues(const Keymap& keymap, const ShortcutEnv& env) {
    std::vector<BindingIssue> out;
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        const auto& info = ActionInfo(action);
        if (info.kind == KeyKind::Fixed) continue;
        if (info.kind == KeyKind::Global) continue;  // its own registration reports a failure
        for (const KeyChord chord : keymap.EffectiveChords(action)) {
            // Global registration availability does not prove local input is blocked.
            if (env.ImeApplies(chord)) out.push_back({action, chord, ChordIssue::Ime, ChordLevel::Warn});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const BindingIssue& a, const BindingIssue& b) {
        return a.level > b.level;  // red first
    });
    return out;
}

namespace {
std::vector<KeyChord> SuggestionPool(const Keymap& keymap, KeyAction action) {
    std::vector<KeyChord> pool;
    const auto& info = ActionInfo(action);
    if (info.kind == KeyKind::Global) {
        for (const KeyChord chord : {K(C | A, VK_SPACE), K(C | S, VK_SPACE), K(kKeyWin | S, 'F'),
                                      K(C | A, 'F'), K(A | S, VK_SPACE)})
            pool.push_back(chord);
        return pool;
    }
    std::vector<KeyChord> base = DefaultBindings(action);
    for (const KeyChord chord : keymap.Bindings(action)) PushUnique(base, chord);
    for (const KeyChord chord : base) {
        if (action == KeyAction::SwitchWindow) break;
        const uint16_t vk = chord.vk;
        const bool letter = vk >= 'A' && vk <= 'Z';
        if (letter || (vk >= VK_F1 && vk <= VK_F24)) {
            PushUnique(pool, K(C | A | S, vk));
            PushUnique(pool, K(C | S, vk));
            PushUnique(pool, K(A | S, vk));
            PushUnique(pool, K(C | A, vk));
        }
    }
    if (action == KeyAction::SwitchWindow) {
        for (const uint16_t vk : {static_cast<uint16_t>(VK_OEM_5), static_cast<uint16_t>(VK_OEM_7),
                                  static_cast<uint16_t>('Q'), static_cast<uint16_t>(VK_OEM_1)})
            PushUnique(pool, K(C, vk));
        return pool;
    }
    // Neighbouring letters of the default (C -> Y/U like the prototype) and free function keys.
    for (const uint16_t vk : {static_cast<uint16_t>('Y'), static_cast<uint16_t>('U'),
                              static_cast<uint16_t>('G'), static_cast<uint16_t>('J')})
        PushUnique(pool, K(C | S, vk));
    for (const uint16_t vk : {static_cast<uint16_t>(VK_F8), static_cast<uint16_t>(VK_F9),
                              static_cast<uint16_t>(VK_F12)}) {
        PushUnique(pool, K(0, vk));
        PushUnique(pool, K(S, vk));
        PushUnique(pool, K(C, vk));
    }
    return pool;
}
} // namespace

std::vector<KeyChord> SuggestChords(const Keymap& keymap, KeyAction action, const ShortcutEnv& env, size_t count) {
    std::vector<KeyChord> out;
    for (const KeyChord chord : SuggestionPool(keymap, action)) {
        if (out.size() >= count) break;
        const ChordVerdict v = CheckChord(keymap, action, chord, env);
        if (v.level == ChordLevel::Ok) out.push_back(chord);
    }
    return out;
}

std::vector<KeyChord> ProbeCandidates(const Keymap& keymap) {
    std::vector<KeyChord> out;
    // Probe only keys intended for system-wide registration. Local shortcuts
    // are delivered through window messages and do not need RegisterHotKey.
    for (const KeyChord chord : SuggestionPool(keymap, KeyAction::GlobalSearch)) PushUnique(out, chord);
    if (keymap.GlobalHotkeyEnabled()) {
        const KeyChord own = keymap.GlobalHotkey();
        out.erase(std::remove(out.begin(), out.end(), own), out.end());
    }
    return out;
}

std::vector<KeyChord> ProbeTakenChords(const std::vector<KeyChord>& chords) {
    std::vector<KeyChord> taken;
    constexpr int kProbeId = 0x5C1F;  // thread hotkey: no window, unregistered at once
    for (const KeyChord chord : chords) {
        if (chord.empty()) continue;
        if (RegisterHotKey(nullptr, kProbeId, chord.mods | MOD_NOREPEAT, chord.vk)) {
            UnregisterHotKey(nullptr, kProbeId);
        } else if (GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED) {
            taken.push_back(chord);
        }
    }
    return taken;
}

bool ChineseImeInstalled() {
    HKL layouts[64]{};
    const int n = GetKeyboardLayoutList(static_cast<int>(std::size(layouts)), layouts);
    for (int i = 0; i < n; ++i) {
        const LANGID lang = LOWORD(reinterpret_cast<UINT_PTR>(layouts[i]));
        if (PRIMARYLANGID(lang) == LANG_CHINESE) return true;
    }
    return false;
}

// ---- shown keys -----------------------------------------------------------
// Menus, tooltips, hints and the shortcut sheet are written with the default
// keys; these put the user's keys in their place.

namespace {

// ParseChord, plus a trailing punctuation key as this layout types it
// ("Ctrl+/", "Ctrl+`").
bool ParseShownChord(std::wstring_view text, KeyChord& chord) {
    if (ParseChord(text, chord)) return true;
    const size_t plus = text.rfind(L'+');
    if (plus == std::wstring_view::npos || plus + 2 != text.size()) return false;
    const wchar_t ch = text.back();
    if (ch < 0x21 || ch > 0x7E || std::iswalnum(ch)) return false;
    const SHORT scan = VkKeyScanW(ch);
    if (scan == -1 || (scan & 0xFF00) != 0) return false;  // needs Shift/AltGr here
    std::wstring mods(text.substr(0, plus));
    KeyChord base{};
    if (!ParseChord(mods + L"+A", base)) return false;
    chord = KeyChord{base.mods, static_cast<uint16_t>(scan & 0xFF)};
    return true;
}

bool HasChord(const std::vector<KeyChord>& list, KeyChord chord) {
    return std::find(list.begin(), list.end(), chord) != list.end();
}

std::wstring SpacedCaps(std::wstring text) {
    std::replace(text.begin(), text.end(), L'+', L' ');
    return text;
}

} // namespace

std::wstring ShownShortcut(std::wstring_view default_chord) {
    KeyChord chord{};
    if (!ParseShownChord(default_chord, chord)) return std::wstring(default_chord);
    const Keymap& map = ActiveKeymap();
    for (int i = 0; i < kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        const auto& info = ActionInfo(action);
        if (info.kind == KeyKind::Range) {
            if (chord.mods != info.range_mods || !IsDigitKey(chord.vk)) continue;
            const int digit = chord.vk - L'0';
            if (digit < info.range_first || digit > info.range_last) continue;
            const uint8_t mods = map.RangeMods(action);
            if (mods == info.range_mods) break;
            return ChordDisplay(KeyChord{mods, chord.vk});
        }
        if (info.kind != KeyKind::Normal || !HasChord(DefaultBindings(action), chord)) continue;
        const auto& now = map.Bindings(action);
        if (HasChord(now, chord)) break;      // the default key still works
        return now.empty() ? std::wstring{} : ChordDisplay(now.front());
    }
    return std::wstring(default_chord);
}

std::wstring RemapShortcutText(std::wstring_view text) {
    auto ascii_alnum = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
    };
    std::wstring out;
    size_t i = 0;
    while (i < text.size()) {
        if (!ascii_alnum(text[i]) || (i > 0 && ascii_alnum(text[i - 1]))) {
            out.push_back(text[i++]);
            continue;
        }
        // A token: letters, digits and "+", plus one punctuation key after "+".
        size_t end = i;
        while (end < text.size()) {
            const wchar_t c = text[end];
            if (ascii_alnum(c) || c == L'+') { ++end; continue; }
            if (end > i && text[end - 1] == L'+' && c >= 0x21 && c <= 0x7E) { ++end; break; }
            break;
        }
        const std::wstring_view token = text.substr(i, end - i);
        const bool chord_like = token.find(L'+') != std::wstring_view::npos ||
            (token.size() >= 2 && token.size() <= 3 && (token[0] == L'F') && std::iswdigit(token[1]));
        std::wstring shown = chord_like ? ShownShortcut(token) : std::wstring{};
        out += chord_like && !shown.empty() ? shown : std::wstring(token);
        i = end;
    }
    return out;
}

std::wstring CustomizedKeysText(const std::vector<KeyAction>& actions) {
    const Keymap& map = ActiveKeymap();
    bool changed = false;
    for (const auto action : actions) changed = changed || !map.IsDefault(action);
    if (!changed) return {};
    std::wstring out;
    for (const auto action : actions) {
        const auto& info = ActionInfo(action);
        std::wstring part;
        if (info.kind == KeyKind::Range) {
            const uint8_t mods = map.RangeMods(action);
            if (mods) {
                part = SpacedCaps(ModsDisplay(mods)) + L' ' + std::to_wstring(info.range_first) +
                       L'\x2013' + std::to_wstring(info.range_last);
            }
        } else {
            for (const auto chord : map.Bindings(action)) {
                if (!part.empty()) part += L" / ";
                part += SpacedCaps(ChordDisplay(chord));
            }
        }
        if (part.empty()) continue;
        if (!out.empty()) out += L"  \x00B7  ";
        out += part;
    }
    return out.empty() ? std::wstring(l10n::Get(l10n::StringId::NotSet).c_str()) : out;
}

} // namespace pulse::app
