#include "context_menu_controller.h"
#include "../common/context_diagnostics.h"
#include "../common/localization.h"

#include "../ipc/ctx_menu_util.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {

namespace {

// The submenu each row sits in: children follow their parent row.
std::vector<std::wstring> ParentKeys(const std::vector<ops::ShellMenuItem>& items) {
    std::vector<std::wstring> parents(items.size());
    std::vector<std::wstring> path;
    for (size_t i = 0; i < items.size(); ++i) {
        const size_t depth = items[i].depth ? items[i].depth : items[i].child ? 1 : 0;
        if (depth < path.size()) path.resize(depth);
        if (!path.empty()) parents[i] = path.back();
        path.push_back(parents[i] + items[i].clsid + L'\x1F' + items[i].text + L'\x1E');
    }
    return parents;
}

// A cached row and a live row are the same command: same handler, same
// submenu, and the same verb (or, without one, the same label).
bool SameComCommand(const ops::ShellMenuItem& cached, const ops::ShellMenuItem& live) {
    if (cached.child != live.child || cached.depth != live.depth || cached.has_children != live.has_children ||
        cached.clsid != live.clsid)
        return false;
    if (!cached.verb.empty() || !live.verb.empty()) return cached.verb == live.verb;
    return cached.text == live.text;
}

// A partial answer laid over rows painted from the cache: matched rows take
// the live row (its host id, label, state), the others stay as painted, and
// live rows the cache did not have wait for the complete answer.
std::vector<ops::ShellMenuItem> OverlayLiveRows(std::vector<ops::ShellMenuItem> predicted,
                                                const std::vector<ops::ShellMenuItem>& live) {
    const auto predicted_parents = ParentKeys(predicted);
    const auto live_parents = ParentKeys(live);
    std::vector<bool> used(live.size(), false);
    for (size_t i = 0; i < predicted.size(); ++i) {
        for (size_t j = 0; j < live.size(); ++j) {
            if (used[j] || live_parents[j] != predicted_parents[i] ||
                !SameComCommand(predicted[i], live[j]))
                continue;
            used[j] = true;
            predicted[i] = live[j];
            break;
        }
    }
    return predicted;
}

bool SameStaticVerbs(const std::vector<StaticVerb>& a, const std::vector<StaticVerb>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        // Icons resolve on the live pass only (seeded rows have none), so a
        // row that just got its icon counts as changed and repaints.
        if (a[i].verb != b[i].verb || a[i].display != b[i].display ||
            a[i].app_path != b[i].app_path || a[i].command != b[i].command ||
            a[i].mnemonic != b[i].mnemonic || a[i].icon_pixels != b[i].icon_pixels ||
            !SameStaticVerbs(a[i].children, b[i].children))
            return false;
    }
    return true;
}

// Stored rows against a new answer: host ids differ every session and do not
// count, everything painted does.
bool SameLayoutRows(const std::vector<ops::ShellMenuItem>& a,
                    const std::vector<ops::ShellMenuItem>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if ((a[i].id == 0) != (b[i].id == 0) || a[i].enabled != b[i].enabled ||
            a[i].separator_after != b[i].separator_after ||
            a[i].has_children != b[i].has_children || a[i].child != b[i].child || a[i].depth != b[i].depth ||
            a[i].mnemonic != b[i].mnemonic || a[i].verb != b[i].verb ||
            a[i].text != b[i].text || a[i].clsid != b[i].clsid || a[i].handler != b[i].handler)
            return false;
    }
    return true;
}

ShellMenuEntry ComEntry(const ops::ShellMenuItem& item, int command = 0) {
    ShellMenuEntry entry;
    entry.command = command;
    entry.text = item.text;
    entry.mnemonic = item.mnemonic;
    entry.enabled = item.enabled;
    entry.verb = item.verb;
    entry.from_com = true;
    entry.clsid = item.clsid;
    entry.handler = item.handler;
    entry.icon = item.icon;
    return entry;
}

ui::FluentMenuItem ShellDisplayEntry(const ShellMenuEntry& entry) {
    ui::FluentMenuItem row;
    row.command = entry.command;
    row.text = entry.text;
    row.mnemonic = entry.mnemonic;
    row.enabled = entry.enabled;
    row.icon = entry.icon;
    for (const auto& child : entry.children) {
        if (child.text.empty() || (child.command == 0 && child.children.empty())) continue;
        auto converted = ShellDisplayEntry(child);
        if (child.children.empty() || !converted.children.empty()) row.children.push_back(std::move(converted));
    }
    return row;
}

} // namespace

void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries) {
    context_diagnostics::Batch diagnostic(L"append-shell");
    if (entries.empty()) return;
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    std::unordered_set<std::wstring> seen;
    for (const auto& it : items)
        if (!it.text.empty()) seen.insert(lower(it.text));

    size_t added = 0;
    for (const auto& e : entries) {
        const bool header = !e.children.empty();
        if (e.text.empty() || (e.command == 0 && !header)) { diagnostic.Row(L"drop-invalid", e.text); continue; }
        const std::wstring key = lower(e.text);
        if (!seen.insert(key).second) { diagnostic.Row(L"drop-duplicate-label", e.text); continue; }
        if (added == 0 && !items.empty()) items.back().separator_after = true;
        ui::FluentMenuItem row = ShellDisplayEntry(e);
        if (header && row.children.empty()) continue;
        items.push_back(std::move(row));
        ++added;
    }
}

std::vector<ui::FluentMenuItem> ReconcileCtxDisplay(
    const std::vector<ui::FluentMenuItem>& shown, const std::vector<ui::FluentMenuItem>& fresh,
    CtxReconcileStats* stats, bool append_late) {
    auto lower = [](const std::wstring& text) {
        std::wstring out = text;
        for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
        return out;
    };
    std::vector<std::wstring> fresh_keys;
    fresh_keys.reserve(fresh.size());
    for (const auto& row : fresh) fresh_keys.push_back(lower(row.text));
    std::vector<char> used(fresh.size(), 0);
    CtxReconcileStats result;
    std::vector<ui::FluentMenuItem> out = shown;
    for (size_t i = 0; i < out.size(); ++i) {
        const bool separator = shown[i].separator_after;
        const std::wstring key = lower(shown[i].text);
        size_t match = fresh.size();
        if (!key.empty()) {
            for (size_t j = 0; j < fresh.size(); ++j) {
                if (!used[j] && fresh_keys[j] == key) { match = j; break; }
            }
        }
        if (match < fresh.size()) {
            used[match] = 1;
            out[i] = fresh[match];
        } else if (!key.empty()) {
            // Predicted, but this selection does not have it: keep the place.
            out[i].enabled = false;
            for (auto& child : out[i].children) child.enabled = false;
            ++result.greyed;
        }
        out[i].separator_after = separator;
    }
    const size_t shown_count = out.size();
    for (size_t j = 0; j < fresh.size(); ++j) {
        if (used[j] || fresh[j].text.empty()) continue;
        if (!append_late) {
            ++result.withheld;
            continue;
        }
        ui::FluentMenuItem row = fresh[j];
        row.separator_after = false;
        if (result.late == 0 && shown_count > 0) out[shown_count - 1].separator_after = true;
        out.push_back(std::move(row));
        ++result.late;
    }
    context_diagnostics::Batch diagnostic(L"reconcile");
    diagnostic.Number(L"greyed", result.greyed);
    diagnostic.Number(L"withheld", result.withheld);
    diagnostic.Number(L"late", result.late);
    for (const auto& row : out) diagnostic.Row(row.enabled ? L"candidate-enabled" : L"candidate-disabled", row.text);
    if (stats) *stats = result;
    return out;
}

std::vector<ShellMenuEntry> ApplyExplorerPrefs(const ContextMenuPrefs& prefs,
                                               const std::vector<ShellMenuEntry>& entries) {
    context_diagnostics::Batch diagnostic(L"preferences");
    std::vector<ShellMenuEntry> compress_top;
    struct Ranked {
        ShellMenuEntry entry;
        int rank = 4;
    };
    std::vector<Ranked> kept;
    kept.reserve(entries.size());
    int mru_kept = 0;
    const int mru_cap = prefs.open_with_mru;
    int vendor_index = -1;

    const bool compress_on = prefs.RowEnabled(ipc::CompressCatalogKey(),
        ipc::CtxMenuCategory::Software, true);

    for (const auto& e : entries) {
        diagnostic.Row(e.from_com ? L"input-com" : L"input-static", e.text);
        if (e.text.empty()) { diagnostic.Row(L"drop-empty"); continue; }
        if (!e.clsid.empty() && !prefs.HandlerEnabled(e.clsid)) { diagnostic.Row(L"drop-handler-disabled", e.clsid); continue; }
        const bool flyout = !e.children.empty();
        if (!flyout && ipc::IsCompressTopLevel(e.text)) {
            diagnostic.Row(compress_on ? L"regroup-compression" : L"drop-compression-preference", e.text);
            if (compress_on) compress_top.push_back(e);
            continue;
        }
        const auto cat = ipc::ClassifyExplorerItem(e.verb, e.text, flyout);
        const std::wstring key = !e.clsid.empty()
            ? ipc::HandlerCatalogKey(e.clsid)
            : ipc::CatalogKey(e.text, flyout);
        diagnostic.Number(L"category", static_cast<unsigned>(cat));
        if (!prefs.RowEnabled(key, cat, e.from_com)) {
            diagnostic.Row(prefs.item_enabled.contains(key) ? L"drop-item-override" : L"drop-category-or-com-preference", e.text);
            continue;
        }
        if (cat == ipc::CtxMenuCategory::OpenWith && ipc::IsOpenWithMruText(e.text)) {
            if (mru_cap >= 0 && mru_kept >= mru_cap) { diagnostic.Row(L"drop-mru-cap", e.text); continue; }
            ++mru_kept;
        }
        // Explorer's order: vendor flyouts, then software verbs, 打开方式, 打印,
        // and 发送到 / image / system verbs last. Ranking by category instead of
        // "is a flyout" keeps the 发送到 submenu from jumping above the software
        // section now that the share group defaults to on.
        int rank = 4;
        if (cat == ipc::CtxMenuCategory::Software) rank = flyout ? 0 : 1;
        else if (cat == ipc::CtxMenuCategory::OpenWith) rank = 2;
        else if (cat == ipc::CtxMenuCategory::Print) rank = 3;
        if (flyout && ipc::IsCompressVendorFlyout(e.text))
            vendor_index = static_cast<int>(kept.size());
        kept.push_back({ e, rank });
    }

    auto append_compress_children = [&](ShellMenuEntry& dest) {
        for (auto& row : compress_top) {
            if (dest.children.size() >= static_cast<size_t>(ipc::kMaxSubmenuChildren)) break;
            bool dup = false;
            for (const auto& c : dest.children)
                if (ipc::ToLowerVerb(c.text) == ipc::ToLowerVerb(row.text)) {
                    dup = true;
                    break;
                }
            if (dup) diagnostic.Row(L"drop-compression-duplicate", row.text);
            if (!dup) dest.children.push_back(std::move(row));
        }
        compress_top.clear();
    };

    if (!compress_top.empty() && vendor_index >= 0) {
        append_compress_children(kept[static_cast<size_t>(vendor_index)].entry);
    } else if (!compress_top.empty()) {
        ShellMenuEntry group;
        group.text = l10n::Cn(ipc::CompressFlyoutText());
        group.from_com = true;
        append_compress_children(group);
        if (!group.children.empty())
            kept.insert(kept.begin(), { std::move(group), 0 });
    }

    std::stable_sort(kept.begin(), kept.end(),
                     [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });

    // No row cap: a long menu is shortened by hiding rows (管理右键项), never
    // by cutting or folding them into a 「更多」 flyout.
    std::vector<ShellMenuEntry> out;
    out.reserve(kept.size());
    for (auto& row : kept) out.push_back(std::move(row.entry));
    return out;
}

std::wstring ContextMenuController::CacheKey(bool background,
                                             const std::wstring& extension, bool multi) {
    if (background) return L":bg";
    std::wstring key = extension.empty() ? L":file" : extension;
    if (multi) key += L"|m";
    return key;
}

void ContextMenuController::StartQuery(
    ContextMenuPrefs& prefs, HWND hwnd,
    std::vector<std::wstring> paths, bool background, std::wstring extension,
    bool extended,
    const std::function<std::wstring(const std::wstring&)>& normalize_path,
    const std::function<void(const std::wstring&)>& prefetch_static) {
    if (paths.empty()) return;
    if (normalize_path) {
        for (auto& path : paths) path = normalize_path(path);
    }
    auto disabled_handlers = prefs.DisabledHandlerClsids();
    std::sort(disabled_handlers.begin(), disabled_handlers.end());
    if (token_ != 0 && background_ == background && paths_ == paths &&
        extended_ == extended && disabled_handlers_ == disabled_handlers) {
        if (prefetch_static) prefetch_static(extension);
        return;
    }
    Close();
    if (draining_.token != 0 && draining_.token != token_ &&
        GetTickCount64() - draining_.since > kDrainMs) {
        if (operations_.close) operations_.close(draining_.token);
        draining_ = {};
    }
    uint32_t token = 0;
    if (operations_.query)
        token = operations_.query(paths, hwnd, background, extended,
                                  disabled_handlers);
    paths_ = std::move(paths);
    background_ = background;
    extended_ = extended;
    disabled_handlers_ = std::move(disabled_handlers);
    extension_ = std::move(extension);
    if (background && extension_.empty()) extension_ = ipc::kBackgroundVerbKey;
    names_ = CtxNamesFor(paths_, background_);
    context_diagnostics::Batch diagnostic(L"start-query");
    diagnostic.Number(L"token", token);
    diagnostic.Number(L"selection-count", paths.size());
    token_ = token;
    com_ready_ = false;
    com_predicted_ = false;
    com_items_.clear();
    query_started_at_ = token == 0 ? 0 : GetTickCount64();
    static_verbs_.clear();
    if (const auto found = static_cache_.find(extension_); found != static_cache_.end())
        static_verbs_ = found->second;
    if (token_ != 0) SeedComItemsFromCache();
    if (prefetch_static) prefetch_static(extension_);
}

void ContextMenuController::Close() {
    const uint32_t live = token_;
    const bool drain = live != 0 && !com_ready_ && query_started_at_ != 0;
    Draining next;
    if (drain) {
        next = { live, CacheKey(background_, extension_, paths_.size() > 1), names_,
                 cache_generation_, GetTickCount64() };
    }
    display_snapshot_valid_ = false;
    menu_com_items_.clear();
    menu_static_verbs_.clear();
    token_ = 0;
    paths_.clear();
    background_ = false;
    extended_ = false;
    disabled_handlers_.clear();
    com_ready_ = false;
    com_predicted_ = false;
    com_items_.clear();
    static_verbs_.clear();
    extension_.clear();
    names_ = {};
    query_started_at_ = 0;
    if (drain) {
        if (draining_.token != 0 && operations_.close) operations_.close(draining_.token);
        draining_ = std::move(next);
        return;
    }
    if (live && operations_.close) operations_.close(live);
}

void ContextMenuController::WaitUntilReady(
    HWND hwnd, const std::function<void()>& pump_messages, uint32_t timeout_ms) {
    if (!hwnd || token_ == 0 || com_ready_) return;
    if (pump_messages) pump_messages();
    if (com_ready_) return;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!com_ready_) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                  static_cast<DWORD>(deadline - now), QS_ALLINPUT);
        if (pump_messages) pump_messages();
    }
}

namespace {
// Pulse's own 新建 flyout (the toolbar New menu's rows) is in the base menu.
bool HasPulseNewMenu(const std::vector<ui::FluentMenuItem>& base_items) {
    for (const auto& row : base_items) {
        for (const auto& child : row.children) {
            if (child.command == CmdNewMarkdownFile ||
                (child.command >= CmdShellNewBase && child.command < CmdShellNewBase + kMaxShellNewItems))
                return true;
        }
    }
    return false;
}

// Explorer's 新建 submenu (Directory\Background\shellex\ContextMenuHandlers\New).
bool IsNativeNewEntry(const ShellMenuEntry& entry) {
    return _wcsicmp(entry.clsid.c_str(), L"{D969A300-E7FF-11d0-A93B-00A0C90F2719}") == 0;
}
} // namespace

std::vector<ui::FluentMenuItem> ContextMenuController::BuildDisplay(
    ContextMenuPrefs& prefs, const std::vector<ui::FluentMenuItem>& base_items,
    bool& prefs_changed) const {
    context_diagnostics::Batch diagnostic(L"build-display");
    diagnostic.Number(L"token", token_);
    diagnostic.Row(com_predicted_ ? L"cache-predicted" : L"live-layout");
    std::vector<ui::FluentMenuItem> display = base_items;
    auto entries = ComposeEntries(prefs, prefs_changed);
    // The same entries already sit in Pulse's 新建 flyout; one 新建 is enough.
    if (HasPulseNewMenu(base_items)) {
        for (const auto& entry : entries) if (IsNativeNewEntry(entry)) diagnostic.Row(L"drop-native-new-duplicate", entry.text);
        std::erase_if(entries, IsNativeNewEntry);
    }
    AppendShellSection(display, ApplyExplorerPrefs(prefs, std::move(entries)));
    ui::FluentMenuItem manage;
    manage.command = CmdSettingsContextMenu;
    manage.text = pulse::l10n::Get(pulse::l10n::StringId::ManageContext);
    manage.glyph = L"\xE713";
    if (!display.empty()) display.back().separator_after = true;
    display.push_back(std::move(manage));
    for (const auto& row : display) {
        diagnostic.Row(row.enabled ? L"candidate-enabled" : L"candidate-disabled", row.text);
        for (const auto& child : row.children) diagnostic.Row(child.enabled ? L"candidate-child-enabled" : L"candidate-child-disabled", child.text);
    }
    return display;
}

bool ContextMenuController::AnswersComplete() const {
    const bool com_late = token_ != 0 && !com_ready_;
    const bool static_late = static_pending_.contains(extension_) &&
                             !static_cache_.contains(extension_);
    return !com_late && !static_late;
}

bool ContextMenuController::MayGrowAfterOpen() const {
    if (static_pending_.contains(extension_) && !static_cache_.contains(extension_)) return true;
    if (token_ == 0 || com_ready_) return false;
    if (!com_predicted_) return true;
    const CtxHandlerStats* stats = ComHandlerStats(CacheKey(background_, extension_, paths_.size() > 1));
    if (!stats || stats->empty()) return true;
    return std::any_of(stats->begin(), stats->end(),
                       [](const CtxHandlerStat& stat) { return !stat.done; });
}

std::vector<ui::FluentMenuItem> ContextMenuController::BuildOpenDisplay(
    ContextMenuPrefs& prefs, const std::vector<ui::FluentMenuItem>& base_items,
    bool& prefs_changed) {
    auto display = BuildDisplay(prefs, base_items, prefs_changed);
    reconcile_stats_ = {};
    pending_stats_ = {};
    may_grow_at_open_ = MayGrowAfterOpen();
    late_since_ = 0;
    shown_ = display;
    return display;
}

std::vector<ui::FluentMenuItem> ContextMenuController::ReconcileDisplay(
    ContextMenuPrefs& prefs, bool& prefs_changed, uint64_t now) {
    if (shown_.empty()) return {};  // not a menu BuildOpenDisplay made
    const auto fresh = BuildDisplay(prefs, base_items_, prefs_changed);
    auto out = ReconcileCtxDisplay(shown_, fresh, &pending_stats_);
    if (pending_stats_.late == 0 || AnswersComplete()) {
        late_since_ = 0;
        return out;
    }
    if (late_since_ == 0) late_since_ = now;
    if (now - late_since_ >= kLateBatchMs) {
        late_since_ = 0;
        return out;
    }
    // More answers are coming: patch the shown rows, keep the late ones for
    // a single growth step.
    return ReconcileCtxDisplay(shown_, fresh, &pending_stats_, false);
}

void ContextMenuController::CommitDisplay(std::vector<ui::FluentMenuItem> shown) {
    context_diagnostics::Batch diagnostic(L"commit-display");
    diagnostic.Number(L"token", token_);
    diagnostic.Number(L"rows", shown.size());
    for (const auto& row : shown) {
        diagnostic.Row(row.enabled ? L"committed-enabled" : L"committed-disabled", row.text);
        for (const auto& child : row.children) diagnostic.Row(child.enabled ? L"committed-child-enabled" : L"committed-child-disabled", child.text);
    }
    shown_ = std::move(shown);
    // Appended rows are shown rows from now on: late adds up, greyed is the
    // current state.
    reconcile_stats_.late += pending_stats_.late;
    reconcile_stats_.greyed = pending_stats_.greyed;
    // Still waiting for their growth step: dropped if the menu closes first.
    const uint32_t withheld = pending_stats_.withheld;
    pending_stats_ = {};
    pending_stats_.withheld = withheld;
    NotePatchedDisplay();
    if (timing_active_) {
        timing_.late = reconcile_stats_.late;
        timing_.greyed = reconcile_stats_.greyed;
    }
}

bool ContextMenuController::ExecuteShellCommand(
    int command,
    const std::function<void()>& wait_until_ready,
    const std::function<void()>& refresh_folder) {
    if (command >= CmdShellComBase && command <= CmdShellComLast) {
        const uint32_t clicked = static_cast<uint32_t>(command - CmdShellComBase);
        std::wstring text, verb;
        if (const auto* snapshot = FindComItemIn(menu_com_items_, clicked)) {
            text = snapshot->text;
            verb = snapshot->verb;
        } else if (const auto* live_item = FindComItem(clicked)) {
            text = live_item->text;
            verb = live_item->verb;
        }
        if (!com_ready_ && wait_until_ready) wait_until_ready();
        uint32_t live = FindLiveComId(verb, text);
        // A recycled session-local ID cannot stand in for a missing action.
        if (token_ && (live != 0 || (!com_ready_ && (!text.empty() || !verb.empty())))) {
            if (operations_.invoke) operations_.invoke(token_, live, verb, text);
            token_ = 0;
        }
        Close();
        if (refresh_folder) refresh_folder();
        return true;
    }
    if (command >= CmdShellStaticBase && command < CmdShellComBase) {
        const int packed = command - CmdShellStaticBase;
        const size_t index = static_cast<size_t>(packed / ipc::kStaticVerbStride);
        const int child = packed % ipc::kStaticVerbStride;
        const auto& displayed = display_snapshot_valid_ ? menu_static_verbs_ : static_verbs_;
        if (index < displayed.size()) {
            const StaticVerb* verb = &displayed[index];
            const auto same = [](const StaticVerb& a, const StaticVerb& b) {
                return a.verb == b.verb && a.command == b.command &&
                       a.app_path == b.app_path && a.display == b.display;
            };
            const auto live = std::find_if(static_verbs_.begin(), static_verbs_.end(),
                [&](const StaticVerb& candidate) { return same(*verb, candidate); });
            auto descendant = [](auto&& self, const StaticVerb& parent, int& ordinal) -> const StaticVerb* {
                for (const auto& candidate : parent.children) {
                    if (--ordinal == 0) return &candidate;
                    if (const auto* found = self(self, candidate, ordinal)) return found;
                }
                return nullptr;
            };
            if (child > 0) {
                int ordinal = child;
                verb = descendant(descendant, *verb, ordinal);
            }
            auto contains = [&](auto&& self, const StaticVerb& parent) -> bool {
                for (const auto& candidate : parent.children)
                    if (same(*verb, candidate) || self(self, candidate)) return true;
                return false;
            };
            // Never reinterpret a displayed command after an asynchronous refresh.
            if (live == static_verbs_.end()) verb = nullptr;
            else if (verb && child > 0 && !contains(contains, *live)) verb = nullptr;
            if (verb && !verb->children.empty()) verb = nullptr;
            constexpr size_t kMaxTargets = 16;
            if (verb) {
                for (size_t i = 0; i < paths_.size() && i < kMaxTargets; ++i) {
                    if (!verb->app_path.empty()) {
                        if (operations_.open_with_app)
                            operations_.open_with_app(verb->app_path, paths_[i]);
                    } else if (!verb->command.empty() && !ShellOwnsStaticVerb(verb->verb)) {
                        if (operations_.execute_command)
                            operations_.execute_command(verb->command, paths_[i]);
                    } else if (operations_.execute_verb) {
                        operations_.execute_verb(paths_[i], verb->verb);
                    }
                    if (verb->verb == L"openas") break;
                }
            }
        }
        Close();
        if (refresh_folder) refresh_folder();
        return true;
    }
    Close();
    return false;
}

bool ContextMenuController::RequestStaticPrefetch(const std::wstring& extension) {
    if (extension.empty() || static_pending_.contains(extension)) return false;
    const auto cached = static_cache_.find(extension);
    // Rows seeded from the on-disk cache still get one live pass: the cache can
    // be older than the enumeration code (that is how 打开方式 went missing).
    if (cached != static_cache_.end() && !static_seeded_.contains(extension)) return false;
    static_pending_.insert(extension);
    return true;
}

bool ContextMenuController::RequestStaticRevalidate(const std::wstring& extension, uint64_t now) {
    if (extension.empty() || static_pending_.contains(extension) ||
        !static_cache_.contains(extension))
        return false;
    const auto read = static_read_at_.find(extension);
    if (read != static_read_at_.end() && now - read->second < kStaticRevalidateMs) return false;
    static_read_at_[extension] = now;
    static_pending_.insert(extension);
    return true;
}

void ContextMenuController::MergeStaticCache(
    std::unordered_map<std::wstring, std::vector<StaticVerb>> entries) {
    for (auto& [extension, verbs] : entries) {
        if (static_cache_.contains(extension)) continue;
        // Rows with empty display text (written by a build without resources)
        // would be dropped by the menu anyway; drop them here and let the live
        // enumeration replace the rest.
        verbs.erase(std::remove_if(verbs.begin(), verbs.end(),
                                   [](const StaticVerb& v) { return v.display.empty(); }),
                    verbs.end());
        static_cache_.emplace(extension, std::move(verbs));
        static_seeded_.insert(std::move(extension));
    }
}

bool ContextMenuController::CompleteStaticVerbs(const std::wstring& extension,
                                                std::vector<StaticVerb> verbs,
                                                uint32_t generation) {
    // Read before the registry changed: it must neither fill the fresh cache
    // nor clear a newer request for the same extension.
    if (generation != cache_generation_) return false;
    static_pending_.erase(extension);
    static_seeded_.erase(extension);
    auto& cached = static_cache_[extension];
    const bool same = SameStaticVerbs(cached, verbs);
    if (!same) cached = std::move(verbs);
    if (extension_ != extension) return false;
    if (same && SameStaticVerbs(static_verbs_, cached)) return false;
    static_verbs_ = cached;
    return true;
}

void ContextMenuController::InvalidateCaches() {
    ++cache_generation_;
    if (draining_.token != 0 && operations_.close) operations_.close(draining_.token);
    draining_ = {};
    static_cache_.clear();
    static_seeded_.clear();
    com_cache_.clear();
    com_layout_loaded_ = false;
    com_layout_dirty_ = false;
    static_pending_.clear();
    static_read_at_.clear();
}

bool ContextMenuController::SeedComItemsFromCache() {
    if (com_ready_ || !com_items_.empty()) return false;
    const auto found = com_cache_.find(CacheKey(background_, extension_, paths_.size() > 1));
    if (found == com_cache_.end() || found->second.rows.empty()) return false;
    found->second.used = ++com_use_clock_;
    com_items_ = found->second.rows;
    uint32_t next = kPredictedIdFirst;
    for (auto& item : com_items_) {
        item.text = ExpandCtxLabel(item.text, names_);
        if (item.id == 0) continue;
        item.id = next <= kPredictedIdLast ? next++ : 0;
    }
    com_predicted_ = true;
    return true;
}

void ContextMenuController::StoreLayout(const std::wstring& key,
                                        const std::vector<ops::ShellMenuItem>& items,
                                        const CtxNames& names,
                                        const std::vector<ops::ShellMenuHandlerStatus>& handlers) {
    std::vector<ops::ShellMenuItem> rows = items;
    for (auto& row : rows) row.text = TemplateCtxLabel(row.text, names);
    auto& stored = com_cache_[key];
    if (!handlers.empty()) {
        rows = MergeCtxHandlerShards(stored.rows, rows, handlers);
        if (MergeCtxHandlerStats(stored.handlers, handlers)) com_layout_dirty_ = true;
    }
    if (!SameLayoutRows(stored.rows, rows)) {
        stored.rows = std::move(rows);
        com_layout_dirty_ = true;
    }
    stored.used = ++com_use_clock_;
}

size_t ContextMenuController::MergeComLayout(CtxLayoutMap layout) {
    size_t added = 0;
    for (auto& [key, entry] : layout) {
        if (entry.rows.empty() || com_cache_.contains(key)) continue;
        com_cache_.emplace(key, ComLayout{ std::move(entry.rows), 0, std::move(entry.handlers) });
        ++added;
    }
    com_layout_loaded_ = true;
    // The query already running when the layout landed may paint from it.
    if (token_ != 0 && !com_ready_ && com_items_.empty() && !menu_open_)
        SeedComItemsFromCache();
    return added;
}

CtxLayoutMap ContextMenuController::ComLayoutSnapshot() const {
    std::vector<std::pair<uint64_t, const std::wstring*>> order;
    order.reserve(com_cache_.size());
    for (const auto& [key, layout] : com_cache_)
        if (!layout.rows.empty()) order.emplace_back(layout.used, &key);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (order.size() > kCtxLayoutMaxKeys) order.resize(kCtxLayoutMaxKeys);
    CtxLayoutMap out;
    out.reserve(order.size());
    for (const auto& entry : order) {
        const auto& layout = com_cache_.at(*entry.second);
        out.emplace(*entry.second, CtxLayoutEntry{ layout.rows, layout.handlers });
    }
    return out;
}

const CtxHandlerStats* ContextMenuController::ComHandlerStats(const std::wstring& key) const {
    const auto found = com_cache_.find(key);
    return found == com_cache_.end() ? nullptr : &found->second.handlers;
}

std::vector<std::pair<std::wstring, uint32_t>> ContextMenuController::ComHandlerTimes() const {
    std::vector<std::pair<std::wstring, uint32_t>> out;
    for (const auto& [key, layout] : com_cache_) {
        for (const CtxHandlerStat& stat : layout.handlers) {
            if (stat.clsid.empty()) continue;
            const auto found = std::find_if(out.begin(), out.end(), [&](const auto& timed) {
                return timed.first.size() == stat.clsid.size() &&
                       CompareStringOrdinal(timed.first.c_str(), static_cast<int>(timed.first.size()),
                                            stat.clsid.c_str(), static_cast<int>(stat.clsid.size()),
                                            TRUE) == CSTR_EQUAL;
            });
            if (found == out.end()) out.emplace_back(stat.clsid, stat.elapsed_ms);
            else found->second = (std::max)(found->second, stat.elapsed_ms);
        }
    }
    return out;
}

std::map<std::wstring, std::vector<std::wstring>> ContextMenuController::ComHandlerLabels() const {
    std::map<std::wstring, std::vector<std::wstring>> out;
    // Sorted keys keep the order of the rows stable between calls.
    std::vector<const std::wstring*> keys;
    for (const auto& [key, layout] : com_cache_) keys.push_back(&key);
    std::sort(keys.begin(), keys.end(), [](const std::wstring* a, const std::wstring* b) { return *a < *b; });
    const CtxNames placeholder{L"\u2026", L"\u2026", L"\u2026"};
    for (const std::wstring* key : keys) {
        for (const ops::ShellMenuItem& row : com_cache_.at(*key).rows) {
            if (row.child || row.clsid.empty() || row.text.empty()) continue;
            std::wstring text = ExpandCtxLabel(row.text, placeholder);
            text.erase(std::remove(text.begin(), text.end(), L'&'), text.end());
            if (text.empty()) continue;
            auto& list = out[row.clsid];
            if (std::find(list.begin(), list.end(), text) == list.end()) list.push_back(std::move(text));
        }
    }
    return out;
}

ContextMenuController::QueryCompletion ContextMenuController::CompleteComQuery(
    uint32_t token, std::vector<ops::ShellMenuItem> items, uint64_t completed_at,
    bool partial, std::vector<ops::ShellMenuHandlerStatus> handlers) {
    QueryCompletion result;
    if (token != 0 && token == draining_.token && token != token_) {
        if (!partial) {
            if (draining_.generation == cache_generation_)
                StoreLayout(draining_.key, items, draining_.names, handlers);
            draining_ = {};
        }
        return result;
    }
    if (token == 0 || token != token_) return result;
    context_diagnostics::Batch diagnostic(L"complete-com");
    diagnostic.Number(L"token", token);
    diagnostic.Number(L"partial", partial);
    diagnostic.Number(L"rows", items.size());
    diagnostic.Number(L"predicted", com_predicted_);
    result.accepted = true;
    result.partial = partial;
    result.cache_key = CacheKey(background_, extension_, paths_.size() > 1);
    // Without handler states a partial snapshot only holds the handlers that
    // finished inside the host's fast budget, so it must feed this session
    // and nothing else: caching an incomplete list made packaged verbs
    // (WinRAR and friends) show up on one right-click and vanish on the next.
    // With them the unfinished handlers keep their stored rows, so partial
    // answers are stored too.
    const bool shards = !handlers.empty();
    if (shards) {
        StoreLayout(result.cache_key, items, names_, handlers);
        if (partial) {
            com_items_ = MergeCtxHandlerShards(com_items_, items, handlers);
        } else {
            com_items_ = std::move(items);
            com_predicted_ = false;
        }
    } else if (partial && com_predicted_) {
        com_items_ = OverlayLiveRows(std::move(com_items_), items);
    } else {
        com_items_ = std::move(items);
        com_predicted_ = false;
    }
    if (!partial) {
        if (!shards) StoreLayout(result.cache_key, com_items_, names_);
        com_ready_ = true;
        if (timing_active_ && menu_open_ && completed_at >= opened_at_) {
            timing_.live_after_open_ms = static_cast<int32_t>(std::min<uint64_t>(
                completed_at - opened_at_, INT32_MAX));
        }
        if (query_started_at_ != 0 && completed_at >= query_started_at_) {
            result.elapsed_ms = static_cast<uint32_t>(std::min<uint64_t>(
                completed_at - query_started_at_, UINT32_MAX));
        }
    }
    return result;
}

std::vector<ShellMenuEntry> ContextMenuController::ComposeEntries(
    ContextMenuPrefs& prefs, bool& prefs_changed) const {
    context_diagnostics::Batch diagnostic(L"compose-input");
    for (const auto& row : static_verbs_) diagnostic.Row(L"static", row.display);
    for (const auto& row : com_items_) { diagnostic.Row(row.child ? L"com-child" : L"com", row.text); diagnostic.Row(L"clsid", row.clsid); }
    std::vector<ShellMenuEntry> entries;
    entries.reserve(static_verbs_.size() + com_items_.size());
    // Explorer's own 打开方式 flyout (recommended apps + 选择其他应用) replaces
    // the static 打开方式… row once the host delivered it with its apps; until
    // then the static row is the instant fallback.
    bool com_open_with = false;
    for (size_t i = 0; i + 1 < com_items_.size() && !com_open_with; ++i) {
        com_open_with = !com_items_[i].child && com_items_[i].has_children &&
                        com_items_[i + 1].child &&
                        ipc::IsOpenWithSubmenuVerb(com_items_[i].verb);
    }
    for (size_t i = 0; i < static_verbs_.size() && i < static_cast<size_t>(ipc::kMaxStaticVerbParents);
         ++i) {
        const auto& verb = static_verbs_[i];
        if (com_open_with && ipc::ToLowerVerb(verb.verb) == L"openas") { diagnostic.Row(L"drop-static-openas-replaced-by-com", verb.display); continue; }
        ShellMenuEntry entry;
        entry.text = verb.display;
        entry.mnemonic = verb.mnemonic;
        entry.verb = verb.verb;
        entry.icon = verb.icon_pixels;
        int ordinal = 0;
        auto children = [&](auto&& self, const std::vector<StaticVerb>& source) -> std::vector<ShellMenuEntry> {
            std::vector<ShellMenuEntry> rows;
            for (const auto& child : source) {
                if (ordinal >= ipc::kMaxSubmenuChildren) break;
                const int slot = ++ordinal;
                ShellMenuEntry row;
                row.text = child.display;
                row.mnemonic = child.mnemonic;
                row.verb = child.verb;
                row.icon = child.icon_pixels;
                row.children = self(self, child.children);
                if (!child.children.empty() && row.children.empty()) continue;
                if (row.children.empty()) row.command = CmdShellStaticBase +
                    static_cast<int>(i) * ipc::kStaticVerbStride + slot;
                rows.push_back(std::move(row));
            }
            return rows;
        };
        entry.children = children(children, verb.children);
        if (verb.children.empty()) entry.command = CmdShellStaticBase +
            static_cast<int>(i) * ipc::kStaticVerbStride;
        else if (entry.children.empty()) continue;
        entries.push_back(std::move(entry));
    }
    auto row_depth = [](const ops::ShellMenuItem& row) -> size_t {
        return row.depth ? row.depth : row.child ? 1 : 0;
    };
    auto compose = [&](auto&& self, size_t& index, size_t depth) -> std::vector<ShellMenuEntry> {
        std::vector<ShellMenuEntry> rows;
        while (index < com_items_.size() && row_depth(com_items_[index]) >= depth) {
            const auto& item = com_items_[index++];
            if (row_depth(item) != depth) continue;
            ShellMenuEntry entry = ComEntry(item, item.has_children ? 0 :
                CmdShellComBase + static_cast<int>(item.id));
            if (item.has_children) {
                entry.children = self(self, index, depth + 1);
                if (depth == 0 && ipc::IsOpenWithSubmenuVerb(item.verb)) entry.from_com = false;
                if (entry.children.empty()) continue;
            } else if (item.id == 0 || item.id > 0x7FFF) continue;
            rows.push_back(std::move(entry));
        }
        return rows;
    };
    size_t com_index = 0;
    auto com_entries = compose(compose, com_index, 0);
    for (auto& entry : com_entries) entries.push_back(std::move(entry));

    prefs_changed = false;
    std::unordered_set<std::wstring> recorded_handlers;
    for (const auto& entry : entries) {
        const bool flyout = !entry.children.empty();
        if (!flyout && ipc::IsCompressTopLevel(entry.text)) {
            if (prefs.RecordSeen(ipc::CompressCatalogKey(), ipc::CompressCatalogText(),
                                 false, ipc::CtxMenuCategory::Software, entry.from_com))
                prefs_changed = true;
            continue;
        }
        if (flyout && entry.clsid.empty() &&
            ipc::ToLowerVerb(entry.text) == ipc::ToLowerVerb(l10n::Cn(ipc::CompressFlyoutText()))) {
            if (prefs.RecordSeen(ipc::CompressCatalogKey(), ipc::CompressCatalogText(),
                                 true, ipc::CtxMenuCategory::Software, true))
                prefs_changed = true;
            continue;
        }
        if (!entry.clsid.empty()) {
            if (!recorded_handlers.insert(ipc::ToLowerVerb(entry.clsid)).second)
                continue;
            const std::wstring name = !entry.text.empty() ? entry.text : entry.handler;
            const auto category = ipc::ClassifyExplorerItem(entry.verb, entry.text, flyout);
            if (prefs.RecordSeen(ipc::HandlerCatalogKey(entry.clsid), name, flyout,
                                 category, entry.from_com))
                prefs_changed = true;
            continue;
        }
        const auto category = ipc::ClassifyExplorerItem(entry.verb, entry.text, flyout);
        if (prefs.RecordSeen(ipc::CatalogKey(entry.text, flyout), entry.text, flyout,
                             category, entry.from_com)) {
            prefs_changed = true;
        }
    }
    return entries;
}

const ops::ShellMenuItem* ContextMenuController::FindComItem(uint32_t id) const {
    return FindComItemIn(com_items_, id);
}

const ops::ShellMenuItem* ContextMenuController::FindComItemIn(
    const std::vector<ops::ShellMenuItem>& items, uint32_t id) const {
    for (const auto& item : items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

uint32_t ContextMenuController::FindLiveComId(const std::wstring& text) const {
    return FindLiveComId({}, text);
}

uint32_t ContextMenuController::FindLiveComId(const std::wstring& verb,
                                              const std::wstring& text) const {
    // A row still painted from the cache has no host id yet.
    const auto live_id = [&](const ops::ShellMenuItem& item) {
        return !item.has_children && item.id != 0 &&
               !(com_predicted_ && item.id >= kPredictedIdFirst && item.id <= kPredictedIdLast);
    };
    if (!verb.empty()) {
        for (const auto& item : com_items_) {
            if (live_id(item) && item.verb == verb) return item.id;
        }
    }
    if (text.empty()) return 0;
    for (const auto& item : com_items_) {
        if (live_id(item) && item.text == text) return item.id;
    }
    return 0;
}

void ContextMenuController::OpenMenu(std::vector<ui::FluentMenuItem> base_items) {
    base_items_ = std::move(base_items);
    menu_com_items_ = com_items_;
    menu_static_verbs_ = static_verbs_;
    display_snapshot_valid_ = true;
    menu_open_ = true;
}

void ContextMenuController::BeginTiming(uint32_t input_to_open_ms, size_t rows,
                                        bool seeded, uint64_t now) {
    timing_ = {};
    timing_.key = extension_;
    timing_.background = background_;
    timing_.input_to_open_ms = input_to_open_ms;
    timing_.rows_open = static_cast<uint32_t>(rows);
    timing_.seeded = seeded;
    timing_.live_at_open = com_ready_;
    timing_.may_grow = may_grow_at_open_;
    display_rows_ = rows;
    opened_at_ = now;
    timing_active_ = true;
}

void ContextMenuController::NoteDisplayUpdate(size_t rows, uint64_t now) {
    if (!timing_active_) return;
    ++timing_.refreshes;
    if (rows != display_rows_) ++timing_.grew;
    display_rows_ = rows;
    constexpr uint32_t kTraceEntries = 24;
    if (timing_.refreshes <= kTraceEntries) {
        if (!timing_.trace.empty()) timing_.trace += ' ';
        timing_.trace += std::to_string(now >= opened_at_ ? now - opened_at_ : 0) + ':' +
                         std::to_string(rows);
    }
}

bool ContextMenuController::TakeTiming(bool shell_command, CtxMenuTiming& out) {
    if (!timing_active_) return false;
    timing_active_ = false;
    timing_.rows_final = static_cast<uint32_t>(display_rows_);
    // Late rows of an answer the open menu could not take (a visible flyout
    // was changing): the stored layout has them for the next menu.
    timing_.dropped = pending_stats_.late + pending_stats_.withheld;
    context_diagnostics::Batch diagnostic(L"menu-final-timing");
    diagnostic.Number(L"token", token_);
    diagnostic.Number(L"rows", timing_.rows_final);
    diagnostic.Number(L"late", reconcile_stats_.late);
    diagnostic.Number(L"greyed", reconcile_stats_.greyed);
    diagnostic.Number(L"dropped", timing_.dropped);
    timing_.shell_command = shell_command;
    out = std::move(timing_);
    timing_ = {};
    return true;
}

void ContextMenuController::ScheduleFolderRefresh(uint64_t now) noexcept {
    refresh_at_ = now + 400;
    refresh_again_at_ = now + 1600;
}

int ContextMenuController::ConsumeDueRefreshes(uint64_t now) noexcept {
    int due = 0;
    if (refresh_at_ != 0 && now >= refresh_at_) {
        refresh_at_ = 0;
        ++due;
    }
    if (refresh_again_at_ != 0 && now >= refresh_again_at_) {
        refresh_again_at_ = 0;
        ++due;
    }
    return due;
}

} // namespace pulse::app
