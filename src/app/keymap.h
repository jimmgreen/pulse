// keymap.h — the keyboard shortcuts the user can change (Settings > 快捷键).
//
// One table (KeyActionInfo) names every action, its group and default keys;
// shortcut help, the settings page and the input layer all read it. A Keymap
// holds the keys in effect; prefs keep only what differs from the defaults
// (AppPrefs::shortcuts, one "Id=Ctrl+Shift+C|F8" line per changed action).
//
// The input layer does not touch HandleKeyDown's chain: Translate() turns a
// key the user assigned into the action's default key (which the existing
// handlers already know), and swallows a default key that no longer belongs
// to its action. Conflict checks (CheckChord / CheckRange) are pure; the
// RegisterHotKey probe checks availability for system-wide registration only.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Modifier bits as RegisterHotKey takes them (MOD_ALT / MOD_CONTROL / ...),
// so the global search hotkey prefs use the same values.
constexpr uint8_t kKeyAlt = 0x1;
constexpr uint8_t kKeyCtrl = 0x2;
constexpr uint8_t kKeyShift = 0x4;
constexpr uint8_t kKeyWin = 0x8;

struct KeyChord {
    uint8_t mods = 0;
    uint16_t vk = 0;
    bool empty() const noexcept { return vk == 0; }
    friend bool operator==(KeyChord a, KeyChord b) noexcept { return a.mods == b.mods && a.vk == b.vk; }
    friend bool operator!=(KeyChord a, KeyChord b) noexcept { return !(a == b); }
};

enum class KeyGroup : uint8_t { Global, Navigate, Files, Select, Search, Tabs, Windows, Count };

enum class KeyKind : uint8_t {
    Normal,  // up to kMaxKeyBindings chords
    Fixed,   // shown with a lock, never rebound (Enter, arrows)
    Range,   // modifiers + digits first..last; only the modifiers change
    Global,  // the system-wide hotkey (AppPrefs::global_search_*), not translated
};

enum class KeyAction : uint8_t {
    GlobalSearch,
    Open, Back, Forward, Up, GoToPath, Refresh, MoveFocus,
    QuickPreview, Copy, Cut, Paste, CopyPath, StageToTray, Undo, Rename, BatchRename, NewFolder, GroupIntoFolder,
    DissolveFolder, Delete,
    PermanentDelete, Properties, ContextMenu, ToggleTag, ClearTags, TagPanel,
    SelectAll, InvertSelection, SelectPattern,
    Search, CommandBar, Project, Filter, AdvancedSearch,
    NewTab, CloseTab, NextTab, TabNumber, Layout, ViewMode, NextPane, MarkTarget, CopyToTarget,
    MoveToTarget, Sidebar, FavoritesBar, Maximize,
    NewWindow, CloseWindow, TabToWindow, ReopenClosed, SwitchWindow, Overview, MergeWindows,
    AiAssistant, AiHistory, ShortcutHelp,
    ArrangeSideBySide, ArrangeStacked, ArrangeColumns, ArrangeTile, ArrangeMain, ArrangeCascade,
    Count
};
constexpr int kKeyActionCount = static_cast<int>(KeyAction::Count);
constexpr size_t kMaxKeyBindings = 3;

struct KeyActionInfo {
    KeyAction action;
    const char* id;          // prefs key, stable
    KeyGroup group;
    KeyKind kind;
    const wchar_t* zh;
    const wchar_t* en;
    std::array<KeyChord, kMaxKeyBindings> defaults;  // Normal / Fixed / Global
    uint8_t range_mods;      // Range: default modifiers
    uint8_t range_first;     // Range: first and last digit
    uint8_t range_last;
    // Ctrl+Tab and the window switcher: Shift steps backwards, so the key is
    // matched with or without Shift and may not contain Shift itself.
    bool shift_reverses;
};

const KeyActionInfo& ActionInfo(KeyAction action) noexcept;
const wchar_t* ActionName(KeyAction action);  // localized
const wchar_t* GroupName(KeyGroup group);     // localized
KeyAction ActionFromId(std::string_view id) noexcept;  // Count when unknown

// Default chords of an action. The window switcher's key is the one left of
// 1 (scan code 0x29), whatever the layout calls it, so it is resolved here.
std::vector<KeyChord> DefaultBindings(KeyAction action);
uint16_t SwitcherVirtualKey() noexcept;

// "Ctrl+Shift+C" <-> chord. Keys: A-Z, 0-9, F1-F24, named keys (Space,
// Enter, Tab, Backspace, Delete, Insert, Home, End, PgUp, PgDn, Left, Right,
// Up, Down, Esc, Apps) and #XX for any other virtual-key code.
std::wstring FormatChord(KeyChord chord);
bool ParseChord(std::wstring_view text, KeyChord& chord);
// What the keycaps show: the layout's name for punctuation, arrows as glyphs.
std::wstring KeyCapText(uint16_t vk);
std::vector<std::wstring> ChordCaps(KeyChord chord);  // modifiers first, then the key
std::wstring ChordDisplay(KeyChord chord);            // caps joined with "+"
std::wstring ModsDisplay(uint8_t mods);               // "Ctrl+Alt"

// The modifiers held right now (GetKeyState) plus `vk`.
KeyChord CurrentChord(UINT vk);
bool IsModifierKey(UINT vk) noexcept;

class Keymap {
public:
    Keymap();
    static Keymap FromPrefs(const std::vector<std::wstring>& lines);
    std::vector<std::wstring> ToPrefs() const;  // changed actions only

    const std::vector<KeyChord>& Bindings(KeyAction action) const;
    uint8_t RangeMods(KeyAction action) const;
    void SetBindings(KeyAction action, std::vector<KeyChord> chords);
    void SetRangeMods(KeyAction action, uint8_t mods);
    // The global hotkey lives in AppPrefs::global_search_*; set it here so
    // the conflict checks see it (disabled: no chord).
    void SetGlobalHotkey(KeyChord chord, bool enabled);
    KeyChord GlobalHotkey() const noexcept { return global_; }
    bool GlobalHotkeyEnabled() const noexcept { return global_enabled_; }

    void Reset(KeyAction action);
    void ResetAll();
    bool IsDefault(KeyAction action) const;
    bool AllDefault() const;

    // Expanded chords of a range action with the given modifiers.
    std::vector<KeyChord> RangeChords(KeyAction action, uint8_t mods) const;
    // Every chord in effect for `action` (ranges expanded, global included
    // while enabled).
    std::vector<KeyChord> EffectiveChords(KeyAction action) const;

    struct Owner {
        KeyAction action = KeyAction::Count;
        int digit = -1;        // range actions: the digit
        bool reverse = false;  // shift_reverses action reached with Shift
        bool found() const noexcept { return action != KeyAction::Count; }
    };
    // Which action `chord` reaches now; `except` is skipped. Fixed keys count.
    Owner Find(KeyChord chord, KeyAction except = KeyAction::Count) const;

    struct Translation {
        enum Kind : uint8_t { Pass, Rewrite, Swallow } kind = Pass;
        KeyChord target{};
        KeyAction action = KeyAction::Count;
    };
    // A pressed chord as the existing handlers should see it.
    Translation Translate(KeyChord pressed) const;

private:
    std::array<std::vector<KeyChord>, kKeyActionCount> bind_{};
    std::array<uint8_t, kKeyActionCount> range_mods_{};
    KeyChord global_{};
    bool global_enabled_ = false;
};

// The keymap every window uses (UI thread). Set after prefs load and on change.
const Keymap& ActiveKeymap();
void SetActiveKeymap(Keymap keymap);

// Workspace digits are a fallback; configured actions and retired defaults win.
int WorkspaceShortcutIndex(const Keymap& keymap, KeyChord chord);

// Shown keys. Menus, tooltips and hints are written with the default keys:
// ShownShortcut("Ctrl+Shift+C") is the key that now runs that action ("" when
// it has none), RemapShortcutText does that for every key in a sentence, and
// CustomizedKeysText gives the shortcut sheet's "Ctrl Shift C / F7" (empty
// while every action listed still has its default keys).
std::wstring ShownShortcut(std::wstring_view default_chord);
std::wstring RemapShortcutText(std::wstring_view text);
std::wstring CustomizedKeysText(const std::vector<KeyAction>& actions);

// ---- conflict checks ------------------------------------------------------

enum class ChordLevel : uint8_t { Ok, Info, Warn, Block };

enum class ChordIssue : uint8_t {
    None,
    Incomplete,     // only modifiers so far
    WinKey,         // Win combinations are Windows' own (allowed for the global hotkey)
    Reserved,       // Alt+F4, Alt+Tab, Ctrl+Esc, ...
    PlainKey,       // a letter, digit or punctuation without Ctrl/Alt: type-ahead
    NeedsCtrl,      // the window switcher cycles while Ctrl is held
    ShiftReserved,  // Shift steps backwards for this action
    Same,           // already this action's key
    Taken,          // unavailable for global registration (RegisterHotKey: 1409)
    DuplicateFixed, // a fixed key or a numbered group: cannot be replaced
    Duplicate,      // another action's key: can be replaced
    Ime,            // the Chinese IME uses it
    TextEdit,       // fine, but text boxes keep it for editing
    RangeNeedsMod,  // ranges need Ctrl or Alt
    RangeSame,      // the range's current modifiers
    RangeBad,       // some digits do not work
};

struct ShortcutEnv {
    std::vector<KeyChord> taken;  // chords unavailable for global registration
    bool chinese_ime = false;     // a Chinese input method is installed
    // 已关闭，不再提示: IME warnings and chords unavailable for global registration (the
    // list and the sidebar badge leave them out; recording still warns).
    std::vector<KeyChord> ime_ignored;
    bool IsTaken(KeyChord chord) const noexcept;
    bool ImeApplies(KeyChord chord) const noexcept;
};

struct ChordVerdict {
    ChordIssue issue = ChordIssue::None;
    ChordLevel level = ChordLevel::Ok;
    Keymap::Owner other{};  // Duplicate / DuplicateFixed
    bool can_save() const noexcept { return level != ChordLevel::Block; }
};

// The Chinese IME's own use of a chord (微软拼音 defaults), or nullptr.
const wchar_t* ImeConflictName(KeyChord chord);
const wchar_t* ReservedName(KeyChord chord);  // nullptr when not reserved
bool IsTextEditChord(KeyChord chord) noexcept;

// Recording a key for `action` (Normal or Global).
ChordVerdict CheckChord(const Keymap& keymap, KeyAction action, KeyChord chord, const ShortcutEnv& env);

struct RangeDigitStatus {
    KeyChord chord{};
    ChordLevel level = ChordLevel::Ok;
    ChordIssue issue = ChordIssue::None;
    Keymap::Owner other{};
};
struct RangeVerdict {
    ChordIssue issue = ChordIssue::None;
    ChordLevel level = ChordLevel::Ok;
    std::vector<RangeDigitStatus> digits;
    int bad = 0;
};
RangeVerdict CheckRange(const Keymap& keymap, KeyAction action, uint8_t mods, const ShortcutEnv& env);

// A key in effect that does not work as expected: Taken (red) or Ime (amber).
struct BindingIssue {
    KeyAction action = KeyAction::Count;
    KeyChord chord{};
    ChordIssue issue = ChordIssue::None;
    ChordLevel level = ChordLevel::Ok;
};
std::vector<BindingIssue> CollectIssues(const Keymap& keymap, const ShortcutEnv& env);

// Up to `count` free chords for `action` near its default key.
std::vector<KeyChord> SuggestChords(const Keymap& keymap, KeyAction action, const ShortcutEnv& env,
                                    size_t count = 3);

// Chords worth probing: every key in effect, the defaults and the suggestion
// candidates. Pulse's own global hotkey is left out (it is ours).
std::vector<KeyChord> ProbeCandidates(const Keymap& keymap);
// RegisterHotKey each chord on the calling thread; returns the ones that
// fail with ERROR_HOTKEY_ALREADY_REGISTERED. Cheap; run off the UI thread
// for the full list.
std::vector<KeyChord> ProbeTakenChords(const std::vector<KeyChord>& chords);
bool ChineseImeInstalled();

} // namespace pulse::app
