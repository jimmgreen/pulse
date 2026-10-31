#include "shortcut_settings.h"

#include "../common/localization.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace pulse::app {

namespace {

constexpr UINT_PTR kToastTimer = 0x5C7A;
constexpr ULONGLONG kToastMs = 6000;

// The mockup's order: 全局, 文件操作, 导航, 搜索, 选择, 标签页与布局, 窗口.
constexpr KeyGroup kGroupOrder[] = {KeyGroup::Global, KeyGroup::Files, KeyGroup::Navigate, KeyGroup::Search,
                                    KeyGroup::Select, KeyGroup::Tabs, KeyGroup::Windows};

void CALLBACK ToastTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring Quote(std::wstring_view text) {
    return std::wstring(l10n::Get(l10n::StringId::Text2874).c_str()) + std::wstring(text) + l10n::Get(l10n::StringId::Text2875).c_str();
}

uint8_t Level(ChordLevel level) { return static_cast<uint8_t>(level); }

bool Contains(const std::vector<KeyChord>& chords, KeyChord chord) {
    return std::find(chords.begin(), chords.end(), chord) != chords.end();
}

std::wstring RangeLabel(const KeyActionInfo& info) {
    return KeyCapText(static_cast<uint16_t>('0' + info.range_first)) + L"–" +
           KeyCapText(static_cast<uint16_t>('0' + info.range_last));
}

std::wstring IssueShort(ChordIssue issue) {
    switch (issue) {
    case ChordIssue::Taken: return l10n::Get(l10n::StringId::HeldByAnotherProgram).c_str();
    case ChordIssue::Ime: return l10n::Get(l10n::StringId::TheIMEMayTakeIt).c_str();
    case ChordIssue::DuplicateFixed:
    case ChordIssue::Duplicate: return l10n::Get(l10n::StringId::UsedByAnotherAction).c_str();
    case ChordIssue::Reserved: return l10n::Get(l10n::StringId::ReservedByWindows).c_str();
    default: return l10n::Get(l10n::StringId::NotAvailable).c_str();
    }
}

} // namespace

ShortcutSettings::ShortcutSettings() {
    map_.SetGlobalHotkey(ActionInfo(KeyAction::GlobalSearch).defaults[0], false);
}

ShortcutSettings::~ShortcutSettings() {
    if (worker_.joinable()) worker_.join();
}

void ShortcutSettings::SetNotifyWindow(HWND hwnd) {
    std::lock_guard lock(mutex_);
    notify_ = hwnd;
}

void ShortcutSettings::SetEnvForTesting(ShortcutEnv env) {
    env_ = std::move(env);
    probed_once_ = true;
    dirty_ = true;
}

void ShortcutSettings::Sync(const std::vector<std::wstring>& lines, KeyChord global, bool global_enabled) {
    if (synced_ && lines == synced_lines_ && global == synced_global_ && global_enabled == synced_global_enabled_) return;
    synced_ = true;
    synced_lines_ = lines;
    synced_global_ = global;
    synced_global_enabled_ = global_enabled;
    map_ = Keymap::FromPrefs(lines);
    map_.SetGlobalHotkey(global, global_enabled);
    env_.ime_ignored.clear();
    const std::wstring prefix = std::wstring(kImeIgnoredId) + L"=";
    for (const auto& line : lines) {
        if (line.rfind(prefix, 0) != 0) continue;
        std::wstring_view rest(line);
        rest.remove_prefix(prefix.size());
        while (!rest.empty()) {
            const size_t bar = rest.find(L'|');
            KeyChord chord{};
            if (ParseChord(rest.substr(0, bar), chord)) env_.ime_ignored.push_back(chord);
            if (bar == std::wstring_view::npos) break;
            rest.remove_prefix(bar + 1);
        }
    }
    dirty_ = true;
}

std::vector<std::wstring> ShortcutSettings::PrefsLines() const {
    std::vector<std::wstring> lines = map_.ToPrefs();
    if (!env_.ime_ignored.empty()) {
        std::wstring line = std::wstring(kImeIgnoredId) + L"=";
        for (size_t i = 0; i < env_.ime_ignored.size(); ++i) {
            if (i) line += L"|";
            line += FormatChord(env_.ime_ignored[i]);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// ---- probe ------------------------------------------------------------------

void ShortcutSettings::EnsureProbed() {
    if (!probed_once_) StartProbe();
}

void ShortcutSettings::StartProbe() {
    if (probing_.load()) return;
    if (worker_.joinable()) worker_.join();  // finished: probing_ is false
    probed_once_ = true;
    env_.chinese_ime = ChineseImeInstalled();
    std::vector<KeyChord> candidates = ProbeCandidates(map_);
    probing_ = true;
    dirty_ = true;
    worker_ = std::thread([this, candidates = std::move(candidates)]() {
        std::vector<KeyChord> taken = ProbeTakenChords(candidates);
        HWND notify = nullptr;
        {
            std::lock_guard lock(mutex_);
            probe_result_ = std::move(taken);
            probe_ready_ = true;
            notify = notify_;
        }
        probing_ = false;
        if (notify) InvalidateRect(notify, nullptr, FALSE);
    });
}

void ShortcutSettings::TakeProbeResult() {
    std::lock_guard lock(mutex_);
    if (!probe_ready_) return;
    probe_ready_ = false;
    env_.taken = std::move(probe_result_);
    probe_result_.clear();
    dirty_ = true;
}

// ---- view -------------------------------------------------------------------

const BindingIssue* ShortcutSettings::IssueFor(KeyAction action, KeyChord chord) const {
    for (const auto& issue : issues_)
        if (issue.action == action && issue.chord == chord) return &issue;
    return nullptr;
}

std::vector<KeyChord> ShortcutSettings::DisplayChords(KeyAction action) const {
    if (ActionInfo(action).kind == KeyKind::Global) return {map_.GlobalHotkey()};
    return map_.Bindings(action);
}

int ShortcutSettings::IssueCount() {
    View();
    return static_cast<int>(issues_.size());
}

std::shared_ptr<const ui::ShortcutsView> ShortcutSettings::View() {
    TakeProbeResult();
    if (view_probing_ != probing_.load()) dirty_ = true;
    // An open popover replaces the toast (the toast would cover the popover's buttons).
    if (!toast_.empty() && (GetTickCount64() >= toast_until_ || pop_ != ui::ShortcutPopView::None)) {
        toast_.clear();
        dirty_ = true;
    }
    if (dirty_ || !view_) Rebuild();
    return view_;
}

void ShortcutSettings::Rebuild() {
    dirty_ = false;
    view_probing_ = probing_.load();
    auto v = std::make_shared<ui::ShortcutsView>();
    issues_ = CollectIssues(map_, env_);
    v->probing = view_probing_;
    v->ime_installed = env_.chinese_ime;
    v->filter = filter_;
    v->query = query_;
    v->search_focused = search_focused_;
    v->finding = finding_;
    if (!find_chord_.empty()) v->find_caps = ChordCaps(find_chord_);
    const std::wstring needle = Lower(query_);
    const Keymap::Owner find_owner = find_chord_.empty() ? Keymap::Owner{} : map_.Find(find_chord_);
    for (const KeyGroup group : kGroupOrder) {
        ui::ShortcutGroupView gv;
        gv.title = GroupName(group);
        if (group == KeyGroup::Tabs) gv.title = l10n::Get(l10n::StringId::TabsAndLayout).c_str();
        for (int i = 0; i < kKeyActionCount; ++i) {
            const auto action = static_cast<KeyAction>(i);
            const auto& info = ActionInfo(action);
            if (info.group != group) continue;
            ui::ShortcutRowView row;
            row.action = i;
            row.name = ActionName(action);
            row.fixed = info.kind == KeyKind::Fixed;
            row.range = info.kind == KeyKind::Range;
            row.global = info.kind == KeyKind::Global;
            row.global_off = row.global && !map_.GlobalHotkeyEnabled();
            row.modified = !row.fixed && !map_.IsDefault(action);
            if (row.range) {
                ui::ShortcutBindingView b;
                b.caps = ChordCaps(KeyChord{map_.RangeMods(action), 0});
                for (const auto& issue : issues_)
                    if (issue.action == action) b.level = (std::max)(b.level, Level(issue.level));
                row.range_label = RangeLabel(info);
                row.level = b.level;
                row.bindings.push_back(std::move(b));
            } else {
                for (const KeyChord chord : DisplayChords(action)) {
                    if (chord.empty()) continue;
                    ui::ShortcutBindingView b;
                    b.caps = ChordCaps(chord);
                    if (const auto* issue = IssueFor(action, chord)) {
                        b.level = Level(issue->level);
                        b.tip = IssueShort(issue->issue);
                    }
                    row.level = (std::max)(row.level, b.level);
                    row.bindings.push_back(std::move(b));
                }
            }
            row.can_add = info.kind == KeyKind::Normal && map_.Bindings(action).size() < kMaxKeyBindings;
            if (!find_chord_.empty())
                row.found = find_owner.action == action || Contains(map_.EffectiveChords(action), find_chord_);
            ++v->counts[0];
            if (row.level >= 2) ++v->counts[1];
            if (row.modified) ++v->counts[2];
            if (filter_ == 1 && row.level < 2) continue;
            if (filter_ == 2 && !row.modified) continue;
            if (!find_chord_.empty() && !row.found) continue;
            if (!needle.empty()) {
                bool match = Lower(row.name).find(needle) != std::wstring::npos ||
                             Lower(info.zh).find(needle) != std::wstring::npos ||
                             Lower(info.en).find(needle) != std::wstring::npos;
                for (const KeyChord chord : map_.EffectiveChords(action))
                    match = match || Lower(ChordDisplay(chord)).find(needle) != std::wstring::npos ||
                            Lower(FormatChord(chord)).find(needle) != std::wstring::npos;
                if (!match) continue;
            }
            if (row.level >= 2) {
                ++gv.issues;
                gv.level = (std::max)(gv.level, row.level);
            }
            gv.rows.push_back(std::move(row));
        }
        if (!gv.rows.empty()) v->groups.push_back(std::move(gv));
    }
    for (const auto& issue : issues_) {
        ui::ShortcutChipView chip;
        chip.action = static_cast<int>(issue.action);
        chip.text = std::wstring(ActionName(issue.action)) + L" · " + ChordDisplay(issue.chord);
        chip.level = Level(issue.level);
        (issue.level == ChordLevel::Block ? v->red : v->amber) += 1;
        v->chips.push_back(std::move(chip));
    }
    v->any_modified = !map_.AllDefault();
    BuildPop(v->pop);
    v->toast = toast_;
    v->toast_undo = toast_undo_ && undo_.has_value();
    view_ = std::move(v);
}

ChordVerdict ShortcutSettings::RecordVerdict() const {
    ChordVerdict verdict = CheckChord(map_, pop_action_, rec_chord_, env_);
    // The global hotkey is Pulse's own registration: it never reaches the
    // window, so it cannot be shared or replaced from here.
    if (verdict.issue == ChordIssue::Duplicate && verdict.other.action == KeyAction::GlobalSearch) {
        verdict.issue = ChordIssue::DuplicateFixed;
        verdict.level = ChordLevel::Block;
    }
    return verdict;
}

void ShortcutSettings::BuildPop(ui::ShortcutPopView& p) {
    using P = ui::ShortcutPopView;
    p = {};
    p.kind = pop_;
    if (pop_ == P::None) return;
    pop_suggestions_.clear();
    auto suggest = [&](KeyAction action) {
        pop_suggestions_ = SuggestChords(map_, action, env_, 3);
        for (const KeyChord chord : pop_suggestions_) p.suggestions.push_back(ChordDisplay(chord));
    };
    if (pop_ == P::More) {
        p.title = l10n::Get(l10n::StringId::More).c_str();
        p.more_items = {l10n::Get(l10n::StringId::CopySettingsToTheClipboard).c_str(),
                        l10n::Get(l10n::StringId::ImportFromTheClipboard).c_str(),
                        l10n::Get(l10n::StringId::ResetAllToDefaults).c_str()};
        return;
    }
    if (pop_ == P::Tester) {
        p.title = l10n::Get(l10n::StringId::KeyTester).c_str();
        p.subtitle = l10n::Get(l10n::StringId::PressAnyKeysToSeeWhatPulseDoesEscCloses).c_str();
        p.waiting = test_chord_.empty();
        if (p.waiting) {
            if (rec_held_) p.caps = ChordCaps(KeyChord{rec_held_, 0});
            return;
        }
        p.caps = ChordCaps(test_chord_);
        const std::wstring keys = ChordDisplay(test_chord_);
        const Keymap::Owner owner = map_.Find(test_chord_);
        if (owner.found()) {
            std::wstring name = ActionName(owner.action);
            if (owner.digit >= 0) name += L" " + std::to_wstring(owner.digit);
            p.verdict = l10n::Get(l10n::StringId::Got).c_str() + keys + L" → " + Quote(name);
            if (env_.ImeApplies(test_chord_)) {
                p.level = 2;
                p.detail = std::wstring(l10n::Get(l10n::StringId::NoteMicrosoftPinyinUsesItFor).c_str()) +
                           Quote(ImeConflictName(test_chord_) ? ImeConflictName(test_chord_) : L"") +
                           l10n::Get(l10n::StringId::ItMayBeTakenWhileTheIMEIsOn).c_str();
            } else if (owner.reverse) {
                p.detail = l10n::Get(l10n::StringId::WithShiftStepsBackwards).c_str();
            }
        } else if (const wchar_t* reserved = ReservedName(test_chord_)) {
            p.level = 1;
            p.verdict = keys + l10n::Get(l10n::StringId::IsReservedByWindows).c_str();
            p.detail = std::wstring(l10n::Get(l10n::StringId::WindowsUsesItFor).c_str()) + Quote(reserved);
        } else {
            p.level = 1;
            p.verdict = l10n::Get(l10n::StringId::NoActionUses).c_str() + keys;
            p.detail = l10n::Get(l10n::StringId::YouCanAssignItToAnyAction).c_str();
        }
        return;
    }
    const auto& info = ActionInfo(pop_action_);
    p.action = static_cast<int>(pop_action_);
    p.slot = pop_slot_;
    p.title = ActionName(pop_action_);
    p.can_reset = info.kind != KeyKind::Global && !map_.IsDefault(pop_action_);
    if (pop_ == P::Range) {
        p.subtitle = std::wstring(l10n::Get(l10n::StringId::PickTheModifiersDigits).c_str()) + RangeLabel(info) +
                     l10n::Get(l10n::StringId::Stay).c_str();
        p.range_mods = range_mods_;
        const RangeVerdict rv = CheckRange(map_, pop_action_, range_mods_, env_);
        for (const auto& d : rv.digits) {
            p.digit_labels.push_back(KeyCapText(d.chord.vk));
            p.digit_levels.push_back(Level(d.level));
        }
        p.level = Level(rv.level);
        const std::wstring mods = ModsDisplay(range_mods_);
        switch (rv.issue) {
        case ChordIssue::RangeNeedsMod:
            p.verdict = l10n::Get(l10n::StringId::NeedsCtrlOrAlt).c_str();
            p.detail = l10n::Get(l10n::StringId::WithShiftAloneOrNoModifierDigitsAreTypedAndJumpToNames).c_str();
            break;
        case ChordIssue::RangeSame:
            p.verdict = l10n::Get(l10n::StringId::ThisIsTheCurrentSetting).c_str();
            p.detail = l10n::Get(l10n::StringId::NothingToChange).c_str();
            break;
        case ChordIssue::RangeBad: {
            p.verdict = std::to_wstring(rv.bad) + l10n::Get(l10n::StringId::CombinationsDoNotWork).c_str();
            int listed = 0;
            for (const auto& d : rv.digits) {
                if (d.level < ChordLevel::Warn || listed >= 2) continue;
                if (listed++) p.detail += l10n::Get(l10n::StringId::Text2855).c_str();
                p.detail += ChordDisplay(d.chord) + l10n::Get(l10n::StringId::Text2903).c_str() +
                            (d.other.found() ? Quote(ActionName(d.other.action)) : IssueShort(d.issue));
            }
            break;
        }
        default:
            p.verdict = mods + L"+" + RangeLabel(info) + l10n::Get(l10n::StringId::AllAvailable).c_str();
            break;
        }
        p.save_label = l10n::Get(l10n::StringId::Save).c_str();
        p.can_save = rv.level != ChordLevel::Block && rv.issue != ChordIssue::RangeSame;
        return;
    }
    const std::vector<KeyChord> chords = DisplayChords(pop_action_);
    const KeyChord current = pop_slot_ >= 0 && pop_slot_ < static_cast<int>(chords.size())
        ? chords[static_cast<size_t>(pop_slot_)] : KeyChord{};
    if (pop_ == P::Info) {
        const BindingIssue* issue = IssueFor(pop_action_, current);
        p.subtitle = l10n::Get(l10n::StringId::CurrentKey).c_str() + ChordDisplay(current);
        p.can_record = true;
        p.level = issue ? Level(issue->level) : 0;
        if (issue && issue->issue == ChordIssue::Taken) {
            p.verdict = ChordDisplay(current) + l10n::Get(l10n::StringId::IsHeldByAnotherProgram).c_str();
            p.detail = l10n::Get(l10n::StringId::AnotherProgramRegisteredItAsAHotkeySoPulseNeverReceivesItPic).c_str();
            p.can_ignore = true;
        } else if (issue && issue->issue == ChordIssue::Ime) {
            const wchar_t* use = ImeConflictName(current);
            p.verdict = std::wstring(l10n::Get(l10n::StringId::MicrosoftPinyinUsesItFor).c_str()) + Quote(use ? use : L"");
            p.detail = l10n::Get(l10n::StringId::WhileTheChineseIMEIsOnItTakesThisKeyFirstSwitchTheIMEHotkeyO).c_str();
            p.steps = {l10n::Get(l10n::StringId::OpenSettingsTimeLanguageLanguageRegion).c_str(),
                       l10n::Get(l10n::StringId::ChineseSimplifiedLanguageOptionsMicrosoftPinyinKeys).c_str(),
                       std::wstring(l10n::Get(l10n::StringId::UnderHotkeysSwitchOff).c_str()) + Quote(use ? use : L"")};
            p.can_ignore = true;
        } else {
            p.verdict = l10n::Get(l10n::StringId::ThisKeyWorks).c_str();
        }
        suggest(pop_action_);
        return;
    }
    // Record.
    p.subtitle = l10n::Get(l10n::StringId::HoldModifiersThenPressALetterDigitOrFunctionKeyEnterSavesEsc).c_str();
    p.waiting = rec_chord_.empty();
    p.save_label = l10n::Get(l10n::StringId::Save).c_str();
    p.can_remove = info.kind == KeyKind::Normal && pop_slot_ >= 0 && pop_slot_ < static_cast<int>(map_.Bindings(pop_action_).size());
    if (p.waiting) {
        if (rec_held_) p.caps = ChordCaps(KeyChord{rec_held_, 0});
        return;
    }
    p.caps = ChordCaps(rec_chord_);
    const ChordVerdict verdict = RecordVerdict();
    // A slot replaced by another key of the same action is no change either.
    const bool same = verdict.issue == ChordIssue::Same || rec_chord_ == current;
    p.level = Level(verdict.level);
    const std::wstring keys = ChordDisplay(rec_chord_);
    switch (same ? ChordIssue::Same : verdict.issue) {
    case ChordIssue::None:
        p.verdict = keys + l10n::Get(l10n::StringId::IsFree).c_str();
        p.detail = l10n::Get(l10n::StringId::ReadyToSave).c_str();
        break;
    case ChordIssue::TextEdit:
        p.verdict = keys + l10n::Get(l10n::StringId::IsFreeTextBoxesKeepIt).c_str();
        p.detail = l10n::Get(l10n::StringId::WhileTheAddressBarOrAFilterBoxHasFocusTheKeyEditsTextFirst).c_str();
        break;
    case ChordIssue::Ime: {
        const wchar_t* use = ImeConflictName(rec_chord_);
        p.verdict = std::wstring(l10n::Get(l10n::StringId::MicrosoftPinyinUsesItFor).c_str()) + Quote(use ? use : L"");
        p.detail = l10n::Get(l10n::StringId::YouCanSaveItButTheIMEMayTakeItFirst).c_str();
        break;
    }
    case ChordIssue::Duplicate: {
        const std::wstring other = ActionName(verdict.other.action);
        p.verdict = l10n::Get(l10n::StringId::AssignedTo).c_str() + Quote(other);
        p.detail = map_.Bindings(verdict.other.action).size() > 1
            ? l10n::Get(l10n::StringId::AfterReplacing).c_str() + Quote(other) + l10n::Get(l10n::StringId::KeepsItsOtherKeys).c_str()
            : l10n::Get(l10n::StringId::AfterReplacing).c_str() + Quote(other) + l10n::Get(l10n::StringId::HasNoKey).c_str();
        p.save_label = l10n::Get(l10n::StringId::ReplaceAndSave).c_str();
        break;
    }
    case ChordIssue::DuplicateFixed:
        p.verdict = keys + l10n::Get(l10n::StringId::IsUsedBy).c_str() +
                    Quote(verdict.other.found() ? ActionName(verdict.other.action) : L"") + l10n::Get(l10n::StringId::Text2927).c_str();
        p.detail = verdict.other.action == KeyAction::GlobalSearch
            ? l10n::Get(l10n::StringId::ItIsTheSystemWideHotkeyAndCannotBeShared).c_str()
            : l10n::Get(l10n::StringId::ThatIsAFixedKeyOrANumberedGroupItCannotBeReplaced).c_str();
        break;
    case ChordIssue::Taken:
        p.verdict = keys + l10n::Get(l10n::StringId::IsHeldByAnotherProgram).c_str();
        p.detail = l10n::Get(l10n::StringId::AnotherProgramRegisteredItSoPulseNeverReceivesIt).c_str();
        break;
    case ChordIssue::Reserved: {
        const wchar_t* use = ReservedName(rec_chord_);
        p.verdict = keys + l10n::Get(l10n::StringId::IsReservedByWindows).c_str();
        p.detail = std::wstring(l10n::Get(l10n::StringId::WindowsUsesItFor).c_str()) + Quote(use ? use : L"");
        break;
    }
    case ChordIssue::PlainKey:
        p.verdict = info.kind == KeyKind::Global ? l10n::Get(l10n::StringId::ASystemWideHotkeyNeedsCtrlAltOrWin).c_str()
                                                 : l10n::Get(l10n::StringId::ASingleLetterOrDigitCannotBeAShortcut).c_str();
        p.detail = l10n::Get(l10n::StringId::TheyJumpToFileNamesAddCtrlOrAlt).c_str();
        break;
    case ChordIssue::WinKey:
        p.verdict = l10n::Get(l10n::StringId::InAppShortcutsCannotUseWin).c_str();
        p.detail = l10n::Get(l10n::StringId::WindowsHandlesWinCombinationsTheSystemWideHotkeyCanUseThem).c_str();
        break;
    case ChordIssue::NeedsCtrl:
        p.verdict = l10n::Get(l10n::StringId::NeedsCtrl).c_str();
        p.detail = l10n::Get(l10n::StringId::TheSwitcherCyclesWhileCtrlIsHeld).c_str();
        break;
    case ChordIssue::ShiftReserved:
        p.verdict = l10n::Get(l10n::StringId::CannotContainShift).c_str();
        p.detail = l10n::Get(l10n::StringId::ShiftStepsBackwards).c_str();
        break;
    case ChordIssue::Same:
        p.level = 1;
        p.verdict = l10n::Get(l10n::StringId::AlreadyThisActionSKey).c_str();
        p.detail = l10n::Get(l10n::StringId::NothingToChange).c_str();
        break;
    default:
        break;
    }
    p.can_save = !same && verdict.can_save();
    if (verdict.level >= ChordLevel::Warn && !same) suggest(pop_action_);
}

// ---- popovers -----------------------------------------------------------------

void ShortcutSettings::ClosePop() {
    pop_ = ui::ShortcutPopView::None;
    pop_action_ = KeyAction::Count;
    pop_slot_ = -1;
    rec_chord_ = {};
    rec_held_ = 0;
    test_chord_ = {};
    dirty_ = true;
}

void ShortcutSettings::CloseTransient() {
    if (pop_ != ui::ShortcutPopView::None || search_focused_ || finding_) {
        ClosePop();
        search_focused_ = false;
        finding_ = false;
    }
}

void ShortcutSettings::OpenRecord(KeyAction action, int slot) {
    ClosePop();
    pop_ = ui::ShortcutPopView::Record;
    pop_action_ = action;
    pop_slot_ = slot;
}

void ShortcutSettings::OpenInfo(KeyAction action, int slot) {
    ClosePop();
    pop_ = ui::ShortcutPopView::Info;
    pop_action_ = action;
    pop_slot_ = slot;
}

void ShortcutSettings::OpenRange(KeyAction action) {
    ClosePop();
    pop_ = ui::ShortcutPopView::Range;
    pop_action_ = action;
    range_mods_ = map_.RangeMods(action);
}

void ShortcutSettings::OpenRow(KeyAction action, int slot) {
    const auto& info = ActionInfo(action);
    if (info.kind == KeyKind::Fixed) return;
    if (info.kind == KeyKind::Range) { OpenRange(action); return; }
    const std::vector<KeyChord> chords = DisplayChords(action);
    const int count = static_cast<int>(chords.size());
    if (info.kind == KeyKind::Global) {
        if (count && IssueFor(action, chords[0])) OpenInfo(action, 0);
        else OpenRecord(action, 0);
        return;
    }
    if (slot == 3) { OpenRecord(action, count); return; }  // the + button
    if (slot >= 0 && slot < count) {
        if (IssueFor(action, chords[static_cast<size_t>(slot)])) OpenInfo(action, slot);
        else OpenRecord(action, slot);
        return;
    }
    // The row itself: explain the first problem, else add (or change) a key.
    for (int i = 0; i < count; ++i)
        if (IssueFor(action, chords[static_cast<size_t>(i)])) { OpenInfo(action, i); return; }
    OpenRecord(action, count < static_cast<int>(kMaxKeyBindings) ? count : 0);
}

// ---- changes ------------------------------------------------------------------

void ShortcutSettings::Remember() {
    undo_ = Snapshot{map_, env_.ime_ignored};
}

void ShortcutSettings::Toast(std::wstring text, bool undo) {
    toast_ = std::move(text);
    toast_undo_ = undo;
    toast_until_ = GetTickCount64() + kToastMs;
    HWND notify = nullptr;
    {
        std::lock_guard lock(mutex_);
        notify = notify_;
    }
    if (notify) SetTimer(notify, kToastTimer, static_cast<UINT>(kToastMs + 100), ToastTimerProc);
    dirty_ = true;
}

void ShortcutSettings::Changed(std::wstring toast, bool undo) {
    changed_ = true;
    Toast(std::move(toast), undo);
}

void ShortcutSettings::ApplyChord(KeyAction action, int slot, KeyChord chord, const ChordVerdict& verdict) {
    Remember();
    std::wstring toast = Quote(ActionName(action)) + l10n::Get(l10n::StringId::IsNow).c_str() + ChordDisplay(chord);
    if (ActionInfo(action).kind == KeyKind::Global) {
        map_.SetGlobalHotkey(chord, synced_global_enabled_);
        global_changed_ = true;
    } else {
        if (verdict.issue == ChordIssue::Duplicate && verdict.other.found()) {
            auto other = map_.Bindings(verdict.other.action);
            other.erase(std::remove(other.begin(), other.end(), chord), other.end());
            map_.SetBindings(verdict.other.action, std::move(other));
            toast += l10n::Get(l10n::StringId::Text2942).c_str() + Quote(ActionName(verdict.other.action)) +
                     l10n::Get(l10n::StringId::NoLongerUsesIt).c_str();
        }
        auto bindings = map_.Bindings(action);
        if (slot >= 0 && slot < static_cast<int>(bindings.size())) bindings[static_cast<size_t>(slot)] = chord;
        else bindings.push_back(chord);
        map_.SetBindings(action, std::move(bindings));
    }
    ClosePop();
    Changed(std::move(toast));
}

void ShortcutSettings::SaveRecord() {
    if (pop_ != ui::ShortcutPopView::Record || rec_chord_.empty()) return;
    const ChordVerdict verdict = RecordVerdict();
    const auto chords = DisplayChords(pop_action_);
    const bool same = verdict.issue == ChordIssue::Same ||
        (pop_slot_ >= 0 && pop_slot_ < static_cast<int>(chords.size()) && chords[static_cast<size_t>(pop_slot_)] == rec_chord_);
    if (same || !verdict.can_save()) return;
    ApplyChord(pop_action_, pop_slot_, rec_chord_, verdict);
}

void ShortcutSettings::SaveRange() {
    if (pop_ != ui::ShortcutPopView::Range) return;
    const RangeVerdict rv = CheckRange(map_, pop_action_, range_mods_, env_);
    if (rv.level == ChordLevel::Block || rv.issue == ChordIssue::RangeSame) return;
    Remember();
    const auto action = pop_action_;
    map_.SetRangeMods(action, range_mods_);
    ClosePop();
    Changed(Quote(ActionName(action)) + l10n::Get(l10n::StringId::IsNow).c_str() + ModsDisplay(map_.RangeMods(action)) + L"+" +
            RangeLabel(ActionInfo(action)));
}

void ShortcutSettings::Undo() {
    if (!undo_) return;
    if (undo_->map.GlobalHotkey() != map_.GlobalHotkey()) global_changed_ = true;
    map_ = undo_->map;
    env_.ime_ignored = undo_->ime_ignored;
    undo_.reset();
    ClosePop();
    Changed(l10n::Get(l10n::StringId::Undone).c_str(), false);
}

void ShortcutSettings::ResetAllKeys() {
    if (map_.AllDefault()) return;
    Remember();
    const KeyChord def = ActionInfo(KeyAction::GlobalSearch).defaults[0];
    map_.ResetAll();
    if (map_.GlobalHotkey() != def) {
        map_.SetGlobalHotkey(def, synced_global_enabled_);
        global_changed_ = true;
    }
    ClosePop();
    Changed(l10n::Get(l10n::StringId::EverythingIsBackToDefaults).c_str());
}

void ShortcutSettings::CopyToClipboard() {
    std::wstring text = L"; Pulse shortcuts\r\n";
    for (const auto& line : PrefsLines()) text += line + L"\r\n";
    for (const char* c = ActionInfo(KeyAction::GlobalSearch).id; *c; ++c) text += static_cast<wchar_t>(*c);
    text += L"=" + FormatChord(map_.GlobalHotkey()) + L"\r\n";
    HWND owner = nullptr;
    {
        std::lock_guard lock(mutex_);
        owner = notify_;
    }
    bool ok = false;
    if (OpenClipboard(owner)) {
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(mem)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
    }
    ClosePop();
    Toast(ok ? l10n::Get(l10n::StringId::ShortcutSettingsCopied).c_str() : l10n::Get(l10n::StringId::TheClipboardIsBusy).c_str(),
          false);
}

void ShortcutSettings::PasteFromClipboard() {
    std::wstring text;
    HWND owner = nullptr;
    {
        std::lock_guard lock(mutex_);
        owner = notify_;
    }
    if (OpenClipboard(owner)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* src = static_cast<const wchar_t*>(GlobalLock(data))) {
                text.assign(src, wcsnlen(src, 64 * 1024));
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    std::vector<std::wstring> lines;
    KeyChord global{};
    bool has_global = false;
    int known = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && std::iswspace(line.back())) line.pop_back();
        while (!line.empty() && std::iswspace(line.front())) line.erase(line.begin());
        const size_t eq = line.find(L'=');
        if (line.empty() || line[0] == L';' || eq == std::wstring::npos) continue;
        std::string id;
        for (size_t i = 0; i < eq; ++i) id += static_cast<char>(line[i] < 0x80 ? line[i] : '?');
        if (ActionFromId(id) == KeyAction::GlobalSearch) {
            has_global = ParseChord(std::wstring_view(line).substr(eq + 1), global);
            known += has_global ? 1 : 0;
            continue;
        }
        if (ActionFromId(id) != KeyAction::Count || line.rfind(kImeIgnoredId, 0) == 0) {
            ++known;
            lines.push_back(std::move(line));
        }
    }
    ClosePop();
    if (!known) {
        Toast(l10n::Get(l10n::StringId::NoShortcutSettingsOnTheClipboard).c_str(), false);
        return;
    }
    Remember();
    const KeyChord old_global = map_.GlobalHotkey();
    map_ = Keymap::FromPrefs(lines);
    map_.SetGlobalHotkey(has_global ? global : old_global, synced_global_enabled_);
    if (map_.GlobalHotkey() != old_global) global_changed_ = true;
    env_.ime_ignored.clear();
    for (const auto& line : lines) {
        if (line.rfind(kImeIgnoredId, 0) != 0) continue;
        std::wstring_view rest(line);
        rest.remove_prefix((std::min)(rest.size(), wcslen(kImeIgnoredId) + 1));
        while (!rest.empty()) {
            const size_t bar = rest.find(L'|');
            KeyChord chord{};
            if (ParseChord(rest.substr(0, bar), chord)) env_.ime_ignored.push_back(chord);
            if (bar == std::wstring_view::npos) break;
            rest.remove_prefix(bar + 1);
        }
    }
    Changed(l10n::Get(l10n::StringId::ImportedConflictsCheckedAgain).c_str());
}

bool ShortcutSettings::TakeChange(Change& out) {
    if (!changed_) return false;
    changed_ = false;
    out.lines = PrefsLines();
    out.global = map_.GlobalHotkey();
    out.global_changed = global_changed_;
    global_changed_ = false;
    synced_lines_ = out.lines;
    synced_global_ = out.global;
    synced_ = true;
    return true;
}

// ---- input ------------------------------------------------------------------------

void ShortcutSettings::Click(int index) {
    using P = ui::ShortcutPopView;
    if (index != ui::kScSearch && index != ui::kScPopBody) search_focused_ = false;
    dirty_ = true;
    if (index >= ui::kScKeyBase) {
        const int action = (index - ui::kScKeyBase) / 4;
        if (action < kKeyActionCount) OpenRow(static_cast<KeyAction>(action), (index - ui::kScKeyBase) % 4);
        return;
    }
    if (index >= ui::kScRowBase) {
        if (index - ui::kScRowBase < kKeyActionCount) OpenRow(static_cast<KeyAction>(index - ui::kScRowBase), -1);
        return;
    }
    if (index >= ui::kScMoreBase && index < ui::kScMoreBase + static_cast<int>(ui::kScMoreCount)) {
        const int item = index - ui::kScMoreBase;
        if (item == ui::kScMoreCopy) CopyToClipboard();
        else if (item == ui::kScMorePaste) PasteFromClipboard();
        else ResetAllKeys();
        return;
    }
    if (index >= ui::kScPopSuggestBase && index < ui::kScPopSuggestBase + 10) {
        const size_t i = static_cast<size_t>(index - ui::kScPopSuggestBase);
        if (i >= pop_suggestions_.size() || pop_action_ == KeyAction::Count) return;
        const KeyChord chord = pop_suggestions_[i];
        const ChordVerdict verdict = CheckChord(map_, pop_action_, chord, env_);
        if (!verdict.can_save()) return;
        const int count = static_cast<int>(DisplayChords(pop_action_).size());
        ApplyChord(pop_action_, pop_slot_ >= 0 && pop_slot_ < count ? pop_slot_ : count, chord, verdict);
        return;
    }
    if (index >= ui::kScPopModBase && index < ui::kScPopModBase + 3) {
        static constexpr uint8_t kBits[] = {kKeyCtrl, kKeyAlt, kKeyShift};
        range_mods_ = static_cast<uint8_t>(range_mods_ ^ kBits[index - ui::kScPopModBase]);
        return;
    }
    if (index >= ui::kScChipBase && index < ui::kScPopClose) {
        const size_t i = static_cast<size_t>(index - ui::kScChipBase);
        if (i >= issues_.size()) return;
        const BindingIssue issue = issues_[i];
        if (ActionInfo(issue.action).kind == KeyKind::Range) { OpenRange(issue.action); return; }
        const auto chords = DisplayChords(issue.action);
        const auto it = std::find(chords.begin(), chords.end(), issue.chord);
        OpenInfo(issue.action, it == chords.end() ? 0 : static_cast<int>(it - chords.begin()));
        return;
    }
    if (index >= ui::kScFilterBase && index < ui::kScFilterBase + 3) {
        filter_ = index - ui::kScFilterBase;
        ClosePop();
        return;
    }
    switch (index) {
    case ui::kScSearch:
        search_focused_ = true;
        finding_ = false;
        ClosePop();
        break;
    case ui::kScSearchClear:
        query_.clear();
        find_chord_ = {};
        finding_ = false;
        break;
    case ui::kScFind:
        finding_ = !finding_;
        find_chord_ = {};
        if (finding_) query_.clear();
        ClosePop();
        break;
    case ui::kScTester: {
        const bool open = pop_ == P::Tester;
        ClosePop();
        finding_ = false;
        if (!open) pop_ = P::Tester;
        break;
    }
    case ui::kScMore: {
        const bool open = pop_ == P::More;
        ClosePop();
        if (!open) pop_ = P::More;
        break;
    }
    case ui::kScRecheck: StartProbe(); break;
    case ui::kScResetAll: ResetAllKeys(); break;
    case ui::kScToastUndo: Undo(); break;
    case ui::kScBackdrop:
    case ui::kScPopClose: ClosePop(); break;
    case ui::kScPopSave:
        if (pop_ == P::Range) SaveRange();
        else SaveRecord();
        break;
    case ui::kScPopRemove: {
        const auto action = pop_action_;
        if (action == KeyAction::Count || ActionInfo(action).kind != KeyKind::Normal) break;
        auto bindings = map_.Bindings(action);
        if (pop_slot_ < 0 || pop_slot_ >= static_cast<int>(bindings.size())) break;
        Remember();
        const KeyChord removed = bindings[static_cast<size_t>(pop_slot_)];
        bindings.erase(bindings.begin() + pop_slot_);
        map_.SetBindings(action, std::move(bindings));
        ClosePop();
        Changed(Quote(ActionName(action)) + l10n::Get(l10n::StringId::NoLongerUses).c_str() + ChordDisplay(removed));
        break;
    }
    case ui::kScPopReset: {
        const auto action = pop_action_;
        if (action == KeyAction::Count || map_.IsDefault(action)) break;
        Remember();
        map_.Reset(action);
        ClosePop();
        Changed(Quote(ActionName(action)) + l10n::Get(l10n::StringId::IsBackToItsDefault).c_str());
        break;
    }
    case ui::kScPopIgnore: {
        const auto chords = DisplayChords(pop_action_);
        if (pop_slot_ < 0 || pop_slot_ >= static_cast<int>(chords.size())) break;
        const KeyChord chord = chords[static_cast<size_t>(pop_slot_)];
        const bool taken = env_.IsTaken(chord);
        Remember();
        if (!Contains(env_.ime_ignored, chord)) env_.ime_ignored.push_back(chord);
        ClosePop();
        Changed(ChordDisplay(chord) + l10n::Get(taken ? l10n::StringId::NoMoreConflictWarnings
                                                      : l10n::StringId::NoMoreIMEWarnings).c_str());
        break;
    }
    case ui::kScPopRecord: {
        if (pop_action_ == KeyAction::Count) break;
        const auto action = pop_action_;
        const int slot = pop_slot_;
        OpenRecord(action, slot);
        break;
    }
    default: break;
    }
}

bool ShortcutSettings::WantsKeys() const noexcept {
    return pop_ != ui::ShortcutPopView::None || search_focused_ || finding_;
}

void ShortcutSettings::FeedChord(KeyChord chord) {
    dirty_ = true;
    rec_held_ = 0;
    if (finding_) {
        find_chord_ = chord;
        finding_ = false;
        return;
    }
    if (pop_ == ui::ShortcutPopView::Tester) {
        test_chord_ = chord;
        return;
    }
    if (pop_ != ui::ShortcutPopView::Record) return;
    rec_chord_ = chord;
    // One RegisterHotKey for the new chord: cheap enough on the UI thread.
    const bool ours = map_.GlobalHotkeyEnabled() && chord == map_.GlobalHotkey();
    if (ActionInfo(pop_action_).kind == KeyKind::Global && !ours && !env_.IsTaken(chord) && (chord.mods & (kKeyCtrl | kKeyAlt | kKeyWin))) {
        if (!ProbeTakenChords({chord}).empty()) env_.taken.push_back(chord);
    }
}

bool ShortcutSettings::HandleHotkeyChord(KeyChord chord) {
    if (!(finding_ || pop_ == ui::ShortcutPopView::Record || pop_ == ui::ShortcutPopView::Tester)) return false;
    FeedChord(chord);
    return true;
}

bool ShortcutSettings::HandleKey(UINT msg, WPARAM wparam, LPARAM lparam) {
    using P = ui::ShortcutPopView;
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    const bool character = msg == WM_CHAR || msg == WM_SYSCHAR;
    if (!down && !up && !character) return false;
    UINT vk = static_cast<UINT>(wparam);
    if ((down || up) && vk == VK_PROCESSKEY) {
        UINT scan = (static_cast<UINT>(lparam) >> 16) & 0xFF;
        if (lparam & (1 << 24)) scan |= 0xE000;
        if (const UINT mapped = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX)) vk = mapped;
    }
    const bool capturing = finding_ || pop_ == P::Record || pop_ == P::Tester;
    if (capturing) {
        if (character) return true;
        if (IsModifierKey(vk) || vk == VK_LWIN || vk == VK_RWIN) {
            // Modifiers alone: show what is held; a new press starts over.
            if (down && pop_ == P::Record && !rec_chord_.empty()) rec_chord_ = {};
            if (down && pop_ == P::Tester && !test_chord_.empty()) test_chord_ = {};
            rec_held_ = CurrentChord(0).mods;
            if (up) {
                // The key going up is still reported as held by GetKeyState.
                if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) rec_held_ = static_cast<uint8_t>(rec_held_ & ~kKeyCtrl);
                if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) rec_held_ = static_cast<uint8_t>(rec_held_ & ~kKeyAlt);
                if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) rec_held_ = static_cast<uint8_t>(rec_held_ & ~kKeyShift);
                if (vk == VK_LWIN || vk == VK_RWIN) rec_held_ = static_cast<uint8_t>(rec_held_ & ~kKeyWin);
            }
            dirty_ = true;
            return true;
        }
        if (up) return true;  // also keeps Alt / F10 from opening the window menu
        const KeyChord chord = CurrentChord(vk);
        if (chord.mods == 0 && vk == VK_ESCAPE) {
            if (finding_) finding_ = false;
            else ClosePop();
            dirty_ = true;
            return true;
        }
        if (chord.mods == 0 && vk == VK_RETURN && pop_ == P::Record) {
            SaveRecord();
            dirty_ = true;
            return true;
        }
        FeedChord(chord);
        return true;
    }
    if (pop_ != P::None) {
        if (down && vk == VK_ESCAPE && CurrentChord(vk).mods == 0) {
            ClosePop();
            return true;
        }
        if (down && vk == VK_RETURN && CurrentChord(vk).mods == 0 && pop_ == P::Range) {
            SaveRange();
            dirty_ = true;
            return true;
        }
        return false;
    }
    if (search_focused_) {
        if (character) {
            const wchar_t c = static_cast<wchar_t>(wparam);
            if (c >= 0x20 && c != 0x7F && query_.size() < 64) {
                query_ += c;
                dirty_ = true;
            }
            return true;
        }
        if (up) return false;
        const uint8_t mods = CurrentChord(vk).mods;
        if (vk == VK_BACK) {
            if (!query_.empty()) query_.pop_back();
            dirty_ = true;
            return true;
        }
        if (vk == VK_ESCAPE) {
            if (!query_.empty()) query_.clear();
            else search_focused_ = false;
            dirty_ = true;
            return true;
        }
        if (vk == VK_RETURN || vk == VK_TAB) {
            search_focused_ = false;
            dirty_ = true;
            return true;
        }
        if ((mods & kKeyCtrl) && vk == 'V') {
            std::wstring text;
            HWND owner = nullptr;
            {
                std::lock_guard lock(mutex_);
                owner = notify_;
            }
            if (OpenClipboard(owner)) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* src = static_cast<const wchar_t*>(GlobalLock(data))) {
                        text.assign(src, wcsnlen(src, 64));
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            for (const wchar_t c : text) {
                if (c == L'\r' || c == L'\n') break;
                if (c >= 0x20 && query_.size() < 64) query_ += c;
            }
            dirty_ = true;
            return true;
        }
        // Plain keys type; Ctrl / Alt shortcuts still work.
        return (mods & (kKeyCtrl | kKeyAlt)) == 0;
    }
    return false;
}

} // namespace pulse::app