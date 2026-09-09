#include "context_menu_controller.h"

#include "../ipc/ctx_menu_util.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {

namespace {

ShellMenuEntry ComEntry(const ops::ShellMenuItem& item, int command = 0) {
    ShellMenuEntry entry;
    entry.command = command;
    entry.text = item.text;
    entry.enabled = item.enabled;
    entry.verb = item.verb;
    entry.from_com = true;
    entry.clsid = item.clsid;
    entry.handler = item.handler;
    return entry;
}

} // namespace

void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries) {
    if (entries.empty()) return;
    constexpr size_t kSectionCap = 48;
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    std::unordered_set<std::wstring> seen;
    for (const auto& it : items)
        if (!it.text.empty()) seen.insert(lower(it.text));

    size_t added = 0;
    for (const auto& e : entries) {
        if (added >= kSectionCap) break;
        const bool header = !e.children.empty();
        if (e.text.empty() || (e.command == 0 && !header)) continue;
        const std::wstring key = lower(e.text);
        if (!seen.insert(key).second) continue;
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
    std::vector<ShellMenuEntry> compress_top;
    struct Ranked {
        ShellMenuEntry entry;
        int rank = 4;
    };
    std::vector<Ranked> kept;
    kept.reserve(entries.size());
    int mru_kept = 0;
    const int mru_cap = (std::max)(0, prefs.open_with_mru);
    int vendor_index = -1;

    const bool compress_on = prefs.ItemEnabled(ipc::CompressCatalogKey(),
        ipc::CtxMenuCategory::Software, true);

    for (const auto& e : entries) {
        if (e.text.empty()) continue;
        if (!e.clsid.empty() && !prefs.HandlerEnabled(e.clsid)) continue;
        const bool flyout = !e.children.empty();
        if (!flyout && ipc::IsCompressTopLevel(e.text)) {
            if (compress_on) compress_top.push_back(e);
            continue;
        }
        const auto cat = ipc::ClassifyExplorerItem(e.verb, e.text, flyout);
        const std::wstring key = !e.clsid.empty()
            ? ipc::HandlerCatalogKey(e.clsid)
            : ipc::CatalogKey(e.text, flyout);
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
            if (!dup) dest.children.push_back(std::move(row));
        }
        compress_top.clear();
    };

    if (!compress_top.empty() && vendor_index >= 0) {
        append_compress_children(kept[static_cast<size_t>(vendor_index)].entry);
    } else if (!compress_top.empty()) {
        ShellMenuEntry group;
        group.text = ipc::CompressFlyoutText();
        group.from_com = true;
        append_compress_children(group);
        if (!group.children.empty())
            kept.insert(kept.begin(), { std::move(group), 0 });
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

std::wstring ContextMenuController::CacheKey(bool background,
                                             const std::wstring& extension) {
    if (background) return L":bg";
    return extension.empty() ? L":file" : extension;
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
    if (token_ != 0 && background_ == background && paths_ == paths) {
        if (prefetch_static) prefetch_static(extension);
        return;
    }
    Close();
    uint32_t token = 0;
    if (operations_.query)
        token = operations_.query(paths, hwnd, background, extended,
                                  prefs.DisabledHandlerClsids());
    paths_ = std::move(paths);
    background_ = background;
    extension_ = std::move(extension);
    if (background && extension_.empty()) extension_ = ipc::kBackgroundVerbKey;
    token_ = token;
    com_ready_ = false;
    com_items_.clear();
    query_started_at_ = token == 0 ? 0 : GetTickCount64();
    static_verbs_.clear();
    if (const auto found = static_cache_.find(extension_); found != static_cache_.end())
        static_verbs_ = found->second;
    if (prefetch_static) prefetch_static(extension_);
}

void ContextMenuController::Close() {
    const uint32_t live = token_;
    token_ = 0;
    paths_.clear();
    background_ = false;
    com_ready_ = false;
    com_items_.clear();
    static_verbs_.clear();
    extension_.clear();
    query_started_at_ = 0;
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

std::vector<ui::FluentMenuItem> ContextMenuController::BuildDisplay(
    ContextMenuPrefs& prefs, const std::vector<ui::FluentMenuItem>& base_items,
    bool& prefs_changed) const {
    std::vector<ui::FluentMenuItem> display = base_items;
    AppendShellSection(display, ApplyExplorerPrefs(
        prefs, ComposeEntries(prefs, prefs_changed)));
    ui::FluentMenuItem manage;
    manage.command = CmdSettingsContextMenu;
    manage.text = L"管理右键项…";
    manage.glyph = L"\xE713";
    if (!display.empty()) display.back().separator_after = true;
    display.push_back(std::move(manage));
    return display;
}

bool ContextMenuController::ExecuteShellCommand(
    int command,
    const std::function<void()>& wait_until_ready,
    const std::function<void()>& refresh_folder) {
    if (command >= CmdShellComBase) {
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
        if (live == 0 && com_ready_) live = clicked;
        if (token_ && (live != 0 || !text.empty() || !verb.empty())) {
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
        if (index < static_verbs_.size()) {
            const StaticVerb* verb = &static_verbs_[index];
            if (child > 0) {
                const size_t ci = static_cast<size_t>(child - 1);
                if (ci < verb->children.size()) verb = &verb->children[ci];
                else verb = nullptr;
            }
            constexpr size_t kMaxTargets = 16;
            if (verb) {
                for (size_t i = 0; i < paths_.size() && i < kMaxTargets; ++i) {
                    if (!verb->app_path.empty()) {
                        if (operations_.open_with_app)
                            operations_.open_with_app(verb->app_path, paths_[i]);
                    } else if (!verb->command.empty()) {
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
    if (extension.empty() || static_cache_.contains(extension) ||
        static_pending_.contains(extension)) {
        return false;
    }
    static_pending_.insert(extension);
    return true;
}

void ContextMenuController::MergeStaticCache(
    std::unordered_map<std::wstring, std::vector<StaticVerb>> entries) {
    for (auto& [extension, verbs] : entries) {
        if (!static_cache_.contains(extension)) {
            static_cache_.emplace(std::move(extension), std::move(verbs));
        }
    }
}

bool ContextMenuController::CompleteStaticVerbs(const std::wstring& extension,
                                                std::vector<StaticVerb> verbs) {
    static_pending_.erase(extension);
    static_cache_[extension] = std::move(verbs);
    if (extension_ != extension) return false;
    static_verbs_ = static_cache_[extension];
    return true;
}

void ContextMenuController::InvalidateCaches() {
    static_cache_.clear();
    com_cache_.clear();
    static_pending_.clear();
}

bool ContextMenuController::SeedComItemsFromCache() {
    if (com_ready_ || !com_items_.empty()) return false;
    const auto found = com_cache_.find(CacheKey(background_, extension_));
    if (found == com_cache_.end() || found->second.empty()) return false;
    com_items_ = found->second;
    return true;
}

ContextMenuController::QueryCompletion ContextMenuController::CompleteComQuery(
    uint32_t token, std::vector<ops::ShellMenuItem> items, uint64_t completed_at,
    bool partial) {
    QueryCompletion result;
    if (token == 0 || token != token_) return result;
    com_items_ = std::move(items);
    result.accepted = true;
    result.partial = partial;
    result.cache_key = CacheKey(background_, extension_);
    com_cache_[result.cache_key] = com_items_;
    if (!partial) {
        com_ready_ = true;
        if (query_started_at_ != 0 && completed_at >= query_started_at_) {
            result.elapsed_ms = static_cast<uint32_t>(std::min<uint64_t>(
                completed_at - query_started_at_, UINT32_MAX));
        }
    }
    return result;
}

std::vector<ShellMenuEntry> ContextMenuController::ComposeEntries(
    ContextMenuPrefs& prefs, bool& prefs_changed) const {
    std::vector<ShellMenuEntry> entries;
    entries.reserve(static_verbs_.size() + com_items_.size());
    for (size_t i = 0; i < static_verbs_.size() && i < static_cast<size_t>(ipc::kMaxStaticVerbParents);
         ++i) {
        const auto& verb = static_verbs_[i];
        ShellMenuEntry entry;
        entry.text = verb.display;
        entry.verb = verb.verb;
        if (!verb.children.empty()) {
            entry.command = 0;
            const size_t n = (std::min)(verb.children.size(),
                                        static_cast<size_t>(ipc::kMaxSubmenuChildren));
            for (size_t j = 0; j < n; ++j) {
                ShellMenuEntry child;
                child.command = CmdShellStaticBase +
                    static_cast<int>(i) * ipc::kStaticVerbStride + static_cast<int>(j) + 1;
                child.text = verb.children[j].display;
                child.verb = verb.children[j].verb;
                entry.children.push_back(std::move(child));
            }
        } else {
            entry.command = CmdShellStaticBase +
                static_cast<int>(i) * ipc::kStaticVerbStride;
        }
        entries.push_back(std::move(entry));
    }
    for (size_t i = 0; i < com_items_.size(); ++i) {
        const auto& item = com_items_[i];
        if (item.child) continue;
        if (item.has_children) {
            ShellMenuEntry parent = ComEntry(item);
            for (size_t j = i + 1; j < com_items_.size() && com_items_[j].child; ++j) {
                if (com_items_[j].id > 0x7FFF) continue;
                parent.children.push_back(ComEntry(com_items_[j],
                    CmdShellComBase + static_cast<int>(com_items_[j].id)));
            }
            if (!parent.children.empty()) entries.push_back(std::move(parent));
            continue;
        }
        if (item.id > 0x7FFF) continue;
        entries.push_back(ComEntry(item, CmdShellComBase + static_cast<int>(item.id)));
    }

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
            ipc::ToLowerVerb(entry.text) == ipc::ToLowerVerb(ipc::CompressFlyoutText())) {
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
                                 category, true))
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
    if (!verb.empty()) {
        for (const auto& item : com_items_) {
            if (!item.has_children && item.id != 0 && item.verb == verb) return item.id;
        }
    }
    if (text.empty()) return 0;
    for (const auto& item : com_items_) {
        if (!item.has_children && item.id != 0 && item.text == text) return item.id;
    }
    return 0;
}

void ContextMenuController::OpenMenu(std::vector<ui::FluentMenuItem> base_items) {
    base_items_ = std::move(base_items);
    menu_com_items_ = com_items_;
    menu_open_ = true;
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
