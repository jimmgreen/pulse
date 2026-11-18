// tag_rules.cpp — see tag_rules.h.
#include "tag_rules.h"
#include "tag_rule_roots.h"

#include "app_commands.h"
#include "app_messages.h"
#include "session.h"
#include "tag_view.h"
#include "../common/display_path.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace pulse {
namespace {

struct TagRule {
    std::wstring folder;    // plain path
    std::wstring pattern;   // ".pdf": the extension; else the name contains it; empty: any file
    std::wstring tag_id;
    bool automatic = false; // false: ask (the default)
};

// UI thread only.
std::vector<TagRule> g_rules;
bool g_loaded = false;
// Offered by the last snack (tag id → paths), taken by 加上.
std::vector<std::pair<std::wstring, std::vector<std::wstring>>> g_suggested;

// Watcher threads → UI.
std::mutex g_hits_mutex;
std::vector<std::wstring> g_hits;
std::atomic<HWND> g_target{nullptr};

struct Watch {
    std::wstring folder;
    std::thread thread;
    HANDLE stop = nullptr;
    std::atomic_bool armed{false};
    ~Watch() { if (stop) CloseHandle(stop); }
};
std::vector<std::unique_ptr<Watch>> g_watches;

std::wstring RulesPath() { return app::GetPulseDataDir() + L"\\tag_rules.json"; }

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

bool ContainsText(std::wstring_view text, std::wstring_view part) {
    if (part.empty()) return true;
    return FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, text.data(),
                           static_cast<int>(text.size()), part.data(), static_cast<int>(part.size()), nullptr,
                           nullptr, nullptr, 0) >= 0;
}

// path is inside folder (any depth).
bool Inside(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() <= folder.size() || !SameText(std::wstring_view(path).substr(0, folder.size()), folder))
        return false;
    return folder.back() == L'\\' || path[folder.size()] == L'\\';
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool Matches(const TagRule& rule, const std::wstring& path) {
    if (!Inside(path, rule.folder)) return false;
    const std::wstring leaf = Leaf(path);
    if (rule.pattern.empty()) return true;
    if (rule.pattern.front() == L'.') {
        return leaf.size() > rule.pattern.size() &&
               SameText(std::wstring_view(leaf).substr(leaf.size() - rule.pattern.size()), rule.pattern);
    }
    return ContainsText(leaf, rule.pattern);
}

// Downloads in progress and Office lock files.
bool Transient(const std::wstring& leaf) {
    if (leaf.starts_with(L"~$") || leaf.starts_with(L".~")) return true;
    static constexpr std::wstring_view kExts[] = {L".tmp", L".crdownload", L".part", L".partial", L".download", L".opdownload"};
    for (auto ext : kExts)
        if (leaf.size() > ext.size() && SameText(std::wstring_view(leaf).substr(leaf.size() - ext.size()), ext))
            return true;
    return false;
}

std::wstring NormalizePattern(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    if (text.starts_with(L"*.")) text.erase(0, 1);
    return text;
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::wstring text;
    if (!ReadUtf8File(RulesPath(), text)) return;
    json::ForEachElement(json::ExtractArray(text, L"rules"), [](const std::wstring& raw) {
        TagRule rule;
        rule.folder = json::ExtractString(raw, L"folder");
        rule.pattern = json::ExtractString(raw, L"pattern");
        rule.tag_id = json::ExtractString(raw, L"tag");
        rule.automatic = json::ExtractBool(raw, L"auto");
        if (!rule.folder.empty() && !rule.tag_id.empty()) g_rules.push_back(std::move(rule));
    });
}

void Save() {
    std::wstring out = L"{\n  \"rules\": [";
    for (size_t i = 0; i < g_rules.size(); ++i) {
        const TagRule& rule = g_rules[i];
        out += i ? L",\n    {\"folder\": \"" : L"\n    {\"folder\": \"";
        json::Escape(rule.folder, out);
        out += L"\", \"pattern\": \"";
        json::Escape(rule.pattern, out);
        out += L"\", \"tag\": \"";
        json::Escape(rule.tag_id, out);
        out += rule.automatic ? L"\", \"auto\": true}" : L"\", \"auto\": false}";
    }
    out += L"\n  ]\n}\n";
    WriteUtf8FileAtomic(RulesPath(), out);
}

bool PublishHit(const std::wstring& path, HANDLE stop) {
    for (;;) {
        bool post = false;
        {
            std::lock_guard lock(g_hits_mutex);
            post = g_hits.empty();
            const auto result = tag_rules_detail::EnqueueHit(g_hits, path);
            if (result == tag_rules_detail::EnqueueResult::Duplicate) return true;
            // At most 1024 Windows paths, including while the UI is busy.
            if (result == tag_rules_detail::EnqueueResult::Added) {
                if (HWND target = g_target.load(); post && target) PostMessageW(target, WM_TAG_RULE_HIT, 0, 0);
                return true;
            }
        }
        if (WaitForSingleObject(stop, 20) != WAIT_TIMEOUT) return false;
    }
}

bool CompensateRoot(const std::wstring& root, HANDLE stop) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator cursor(root,
        std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && cursor != end && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        const auto path = cursor->path().wstring();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) cursor.disable_recursion_pending();
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && !PublishHit(path, stop)) return false;
        }
        cursor.increment(error);
    }
    return !error && cursor == end;
}

void WatchFolder(Watch* watch, HANDLE stop) {
    HANDLE dir = CreateFileW(watch->folder.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> storage(8 * 1024);   // DWORD-aligned, as the API wants
    BYTE* buffer = reinterpret_cast<BYTE*>(storage.data());
    const DWORD buffer_bytes = static_cast<DWORD>(storage.size() * sizeof(DWORD));
    bool dirty = false;
    while (done) {
        OVERLAPPED ov{};
        ov.hEvent = done;
        ResetEvent(done);
        if (!ReadDirectoryChangesW(dir, buffer, buffer_bytes, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &ov,
                                   nullptr))
            break;
        HANDLE waits[2] = {stop, done};
        watch->armed = true;
        DWORD bytes = 0;
        // Arm before reconciliation so changes made during the scan remain
        // covered. Failed scans retain the dirty state and retry cancellably.
        while (dirty && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
            dirty = !CompensateRoot(watch->folder, stop);
            if (dirty && WaitForSingleObject(stop, 250) != WAIT_TIMEOUT) break;
        }
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(dir, &ov);
            GetOverlappedResult(dir, &ov, &bytes, TRUE);
            break;
        }
        if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) break;
        if (bytes == 0) {
            // Kernel overflow makes this root dirty. Stream reconciliation
            // with backpressure instead of materializing the entire subtree.
            dirty = true;
            continue;
        }
        std::vector<std::wstring> found;
        for (size_t offset = 0;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                std::wstring path = watch->folder;
                if (path.back() != L'\\') path += L'\\';
                path.append(info->FileName, info->FileNameLength / sizeof(wchar_t));
                found.push_back(std::move(path));
            }
            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
        if (found.empty()) continue;
        for (const auto& path : found) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !PublishHit(path, stop)) break;
        }
    }
    if (done) CloseHandle(done);
    CloseHandle(dir);
}

void Restart() {
    StartTagRules(g_target.load());
}

std::wstring Formatted(l10n::StringId id, const std::wstring& a, const std::wstring& b = {}) {
    wchar_t text[512]{};
    swprintf_s(text, l10n::Get(id).c_str(), a.c_str(), b.c_str());
    return text;
}

std::wstring RuleLabel(const TagRule& rule) {
    const std::wstring folder = Leaf(rule.folder).empty() ? rule.folder : Leaf(rule.folder);
    const std::wstring what = rule.pattern.empty() ? l10n::Get(l10n::StringId::TagRuleAnyFile) : rule.pattern;
    return folder + L" · " + what + L" · " +
           l10n::Get(rule.automatic ? l10n::StringId::TagRuleAutoShort : l10n::StringId::TagRuleAskShort);
}

} // namespace

void StartTagRules(HWND target) {
    g_target = target;
    Load();
    // One watcher per folder (a folder inside another watched one is covered).
    std::vector<std::wstring> folders;
    for (const auto& rule : g_rules) {
        folders.push_back(rule.folder);
    }
    folders = tag_rules_detail::MinimalRoots(std::move(folders));
    std::erase_if(g_watches, [&](auto& watch) {
        if (std::any_of(folders.begin(), folders.end(), [&](const auto& folder) {
                return SameText(folder, watch->folder);
            })) return false;
        SetEvent(watch->stop);
        if (watch->thread.joinable()) watch->thread.join();
        return true;
    });
    for (const auto& folder : folders) {
        if (std::any_of(g_watches.begin(), g_watches.end(), [&](const auto& watch) {
                return SameText(folder, watch->folder);
            })) continue;
        auto watch = std::make_unique<Watch>();
        watch->folder = folder;
        watch->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!watch->stop) continue;
        g_watches.push_back(std::move(watch));
        auto& added = g_watches.back();
        try { added->thread = std::thread(WatchFolder, added.get(), added->stop); }
        catch (...) { g_watches.pop_back(); }
    }
}

void StopTagRules() {
    for (auto& watch : g_watches) SetEvent(watch->stop);
    for (auto& watch : g_watches)
        if (watch->thread.joinable()) watch->thread.join();
    g_watches.clear();
}

void RetargetTagRules(HWND target) {
    if (target) g_target = target;
}

void HandleTagRuleHits(AppState& s) {
    std::vector<std::wstring> hits;
    {
        std::lock_guard lock(g_hits_mutex);
        hits.swap(g_hits);
    }
    std::unordered_set<std::wstring> seen;
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> automatic;
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> asked;
    auto add = [](auto& list, const std::wstring& tag, const std::wstring& path) {
        for (auto& [id, paths] : list)
            if (id == tag) {
                if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
                return;
            }
        list.emplace_back(tag, std::vector<std::wstring>{path});
    };
    for (const auto& path : hits) {
        if (!seen.insert(path).second || Transient(Leaf(path))) continue;
        // Revalidate at consumption: queued paths may have been deleted or
        // replaced since the producer checked them.
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        for (const auto& rule : g_rules) {
            const int index = s.places.FindTagIndex(rule.tag_id);
            if (index < 0 || !Matches(rule, path) || s.places.PathHasTag(path, index)) continue;
            add(rule.automatic ? automatic : asked, rule.tag_id, path);
        }
    }
    // 自动加上: tagged now, with the drop's 已给 N 项加上「…」 · 撤销.
    for (const auto& [id, paths] : automatic) ApplyDropTags(s, {id}, paths);
    if (asked.empty() || !s.hwnd) return;
    // Ask: one snack for everything still waiting (an unanswered offer grows).
    for (const auto& [id, paths] : asked)
        for (const auto& path : paths) add(g_suggested, id, path);
    const app::ColorTag* first = s.places.FindTag(g_suggested.front().first);
    if (!first) return;
    std::unordered_set<std::wstring> files;
    for (const auto& [id, paths] : g_suggested) files.insert(paths.begin(), paths.end());
    std::wstring text;
    if (files.size() == 1) {
        text = Formatted(l10n::StringId::TagRuleSuggestOne, Leaf(*files.begin()), first->name);
    } else {
        wchar_t buffer[512]{};
        swprintf_s(buffer, l10n::Get(l10n::StringId::TagRuleSuggestMany).c_str(), static_cast<int>(files.size()),
                   first->name.c_str());
        text = buffer;
    }
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, text, l10n::Get(l10n::StringId::TagRulesMenu),
                                   {ui::SnackAction{l10n::Get(l10n::StringId::TagRuleAccept), WM_TAG_RULE_ACCEPT}});
}

void AcceptTagRuleSuggestion(AppState& s) {
    auto suggested = std::move(g_suggested);
    g_suggested.clear();
    for (const auto& [id, paths] : suggested)
        if (s.places.FindTag(id)) ApplyDropTags(s, {id}, paths);
}

void AppendTagRulesMenu(const app::ColorTag& tag, std::vector<ui::FluentMenuItem>& items,
                        int new_command, int rule_base) {
    Load();
    ui::FluentMenuItem menu;
    menu.text = l10n::Get(l10n::StringId::TagRulesMenu);
    menu.glyph = L"\xE945";   // lightning bolt
    int n = 0;
    for (const auto& rule : g_rules) {
        if (rule.tag_id != tag.id) continue;
        ui::FluentMenuItem item;
        item.command = rule_base + n++;
        item.text = RuleLabel(rule);
        item.glyph = L"\xE711";   // ✕: click removes it
        menu.children.push_back(std::move(item));
        if (n >= 32) break;
    }
    if (!menu.children.empty()) menu.children.back().separator_after = true;
    ui::FluentMenuItem create;
    create.command = new_command;
    create.text = l10n::Get(l10n::StringId::TagRuleNew);
    create.glyph = L"\xE710";
    menu.children.push_back(std::move(create));
    items.push_back(std::move(menu));
}

bool HandleTagRulesCommand(AppState& s, const std::wstring& tag_id, int command, int new_command,
                           int rule_base, POINT screen_pt) {
    if (command >= rule_base && command < rule_base + 32) {
        int n = command - rule_base;
        for (auto it = g_rules.begin(); it != g_rules.end(); ++it) {
            if (it->tag_id != tag_id || n-- != 0) continue;
            g_rules.erase(it);
            Save();
            Restart();
            if (s.hwnd)
                s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
                                               l10n::Get(l10n::StringId::TagRuleRemoved), std::wstring{}, {});
            return true;
        }
        return true;
    }
    if (command != new_command) return false;
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag || !EnsureMenu(s)) return true;
    const std::wstring tag_name = tag->name;
    TagRule rule;
    rule.tag_id = tag_id;
    if (!PickFolder(s, rule.folder, l10n::Get(l10n::StringId::TagRulePickFolder).c_str(), L"tag_rule") || rule.folder.empty())
        return true;
    rule.folder = pulse::path::FriendlyPathText(rule.folder);
    // One step for both: type the name part or .ext, pick 提示 / 自动.
    constexpr int kAsk = 31950;
    constexpr int kAuto = 31951;
    auto rebuild = [](const std::wstring& query) {
        const std::wstring pattern = NormalizePattern(query);
        const std::wstring what = pattern.empty() ? l10n::Get(l10n::StringId::TagRuleAnyFile) : pattern;
        std::vector<ui::FluentMenuItem> list;
        ui::FluentMenuItem ask;
        ask.command = kAsk;
        ask.text = what + L" · " + l10n::Get(l10n::StringId::TagRuleAskMode);
        ask.glyph = L"\xE946";
        list.push_back(std::move(ask));
        ui::FluentMenuItem automatic;
        automatic.command = kAuto;
        automatic.text = what + L" · " + l10n::Get(l10n::StringId::TagRuleAutoMode);
        automatic.glyph = L"\xE945";
        list.push_back(std::move(automatic));
        return list;
    };
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TagRulePatternHint));
    s.menu->SetFilterMinWidth(320.0f);
    const int picked = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
    const std::wstring query = s.menu->LastFilterQuery();
    const bool committed = s.menu->LastFilterCommitted();
    s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    if (picked != kAsk && picked != kAuto && !(picked == app::CmdNone && committed)) return true;
    rule.pattern = NormalizePattern(query);
    rule.automatic = picked == kAuto;
    const std::wstring label = RuleLabel(rule);
    g_rules.push_back(std::move(rule));
    Save();
    Restart();
    if (s.hwnd)
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
                                       Formatted(l10n::StringId::TagRuleCreated, label, tag_name), std::wstring{}, {});
    return true;
}
int RunTagRuleReuseTest() {
    // Dedicated command-line process only: never load or save user rules.
    StopTagRules();
    g_loaded = true;
    g_rules.clear();
    g_target = nullptr;
    const auto root = std::filesystem::absolute(L"bench_data").wstring() +
        L"\\tag_rule_reuse_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::error_code error;
    std::filesystem::create_directories(root + L"\\child", error);
    if (error) return 1;
    bool ok = true;
    g_rules.push_back({root + L"\\child", {}, L"fixture", false});
    StartTagRules(nullptr);
    g_rules.push_back({root, {}, L"fixture", false});
    Restart();
    ok = ok && g_watches.size() == 1 && SameText(g_watches.front()->folder, root);
    const auto retained = g_watches.front()->thread.get_id();
    g_rules.push_back({root, L".txt", L"second-fixture", false});
    Restart();
    ok = ok && g_watches.size() == 1 && g_watches.front()->thread.get_id() == retained;
    g_rules.erase(g_rules.begin() + 1, g_rules.end());
    Restart();
    ok = ok && g_watches.size() == 1 && SameText(g_watches.front()->folder, root + L"\\child");
    // Allow the native watch to arm, then force producer backpressure.
    const auto arm_deadline = GetTickCount64() + 2000;
    while (!g_watches.front()->armed && GetTickCount64() < arm_deadline) Sleep(1);
    ok = ok && g_watches.front()->armed;
    std::vector<std::wstring> files;
    for (int i = 0; i < 1200; ++i) {
        auto path = root + L"\\child\\file" + std::to_wstring(i) + L".txt";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { ok = false; break; }
        CloseHandle(file);
        files.push_back(std::move(path));
    }
    std::unordered_set<std::wstring> seen;
    const auto drain = [&] {
        std::lock_guard lock(g_hits_mutex);
        ok = ok && g_hits.size() <= 1024;
        seen.insert(g_hits.begin(), g_hits.end());
        g_hits.clear();
    };
    const auto until = GetTickCount64() + 10000;
    while (seen.size() < files.size() && GetTickCount64() < until) { drain(); Sleep(5); }
    ok = ok && seen.size() == files.size();
    StopTagRules();
    drain();
    seen.clear();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic_bool complete{false};
    bool reconciled = false;
    std::thread scan([&] { reconciled = CompensateRoot(root, stop); complete = true; });
    while (!complete) { drain(); Sleep(1); }
    scan.join();
    drain();
    ok = ok && reconciled && seen.size() == files.size();
    CloseHandle(stop);
    for (const auto& file : files) DeleteFileW(file.c_str());
    RemoveDirectoryW((root + L"\\child").c_str());
    RemoveDirectoryW(root.c_str());
    g_rules.clear();
    std::printf("[%s] Tag watcher root diff, retained thread, 1200 notifications, bounded reconciliation\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
} // namespace pulse
