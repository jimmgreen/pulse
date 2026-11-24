// folder_alias.h — 别名: a name the user gives a file or folder for display.
// The item itself keeps its real name; copying the path, dragging it out and
// editing the address still use the real path. An alias is also an address
// bar shortcut: typing it (or "alias\sub\path") jumps to the folder.
//
// Header-only on purpose: the renderer reads it while building rows, and
// every target that compiles the renderer gets it without new sources.
// Stored in <Pulse data dir>\aliases.json, keyed by full path.
#pragma once
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/async_file_writer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::app {

// How aliased items are labelled: 别名 + 原名 (default), 只显示别名, 显示原名.
enum class AliasDisplay : int { Both = 0, AliasOnly = 1, Real = 2 };

struct AliasEntry {
    std::wstring path;    // as given (original case)
    std::wstring alias;
};

class AliasStore {
public:
    static AliasStore& Instance() {
        static AliasStore store;
        return store;
    }

    // Reads `file` (a missing file is an empty store). Called once at start.
    void Load(const std::wstring& file) {
        GlobalAsyncFileWriter().Flush();
        std::wstring text;
        std::unordered_map<std::wstring, AliasEntry> loaded;
        int display = 0;
        bool mark = false;
        if (ReadUtf8File(file, text)) {
            display = json::ExtractInt(text, L"display", 0);
            mark = json::ExtractBool(text, L"mark", false);
            if (display < 0 || display > 2) display = 0;
            const std::wstring object = json::ExtractObject(text, L"aliases");
            json::ForEachMember(object, [&](const std::wstring& key, const std::wstring& raw) {
                if (raw.size() < 2 || raw.front() != L'"') return;
                size_t pos = 1;
                std::wstring alias = json::UnescapeString(raw, pos);
                alias = Trimmed(alias);
                if (key.empty() || alias.empty()) return;
                loaded[Key(key)] = AliasEntry{key, alias};
            });
        }
        std::lock_guard<std::mutex> lock(mu_);
        file_ = file;
        map_ = std::move(loaded);
        display_.store(display, std::memory_order_relaxed);
        mark_.store(mark, std::memory_order_relaxed);
        count_.store(map_.size(), std::memory_order_relaxed);
        revision_.fetch_add(1, std::memory_order_relaxed);
    }

    bool Empty() const { return count_.load(std::memory_order_relaxed) == 0; }
    // Bumped on every change, so cached rows know to look again.
    uint64_t Revision() const { return revision_.load(std::memory_order_relaxed); }

    AliasDisplay Display() const { return static_cast<AliasDisplay>(display_.load(std::memory_order_relaxed)); }
    void SetDisplay(AliasDisplay mode) {
        std::wstring file;
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (display_.load(std::memory_order_relaxed) == static_cast<int>(mode)) return;
            display_.store(static_cast<int>(mode), std::memory_order_relaxed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            file = file_;
            json = SerializeLocked();
        }
        if (!file.empty()) WriteUtf8FileAtomic(file, json);
    }
    // The small mark beside an alias (list rows, sidebar, breadcrumb, address
    // suggestions). Off by default; 设置 > 通用 turns it on.
    bool ShowMark() const { return mark_.load(std::memory_order_relaxed); }
    void SetShowMark(bool on) {
        std::wstring file;
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (mark_.load(std::memory_order_relaxed) == on) return;
            mark_.store(on, std::memory_order_relaxed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            file = file_;
            json = SerializeLocked();
        }
        if (!file.empty()) WriteUtf8FileAtomic(file, json);
    }
    // The label to show for `path` under the display mode: its alias, or
    // empty when it has none or 显示原名 is on.
    std::wstring Shown(std::wstring_view path) const {
        if (Display() == AliasDisplay::Real) return {};
        return Find(path);
    }

    // The alias of `path`, or empty.
    std::wstring Find(std::wstring_view path) const {
        if (path.empty() || Empty()) return {};
        const std::wstring key = Key(path);
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = map_.find(key);
        return it == map_.end() ? std::wstring() : it->second.alias;
    }

    // Sets (or, with an empty alias, clears) the alias of `path` and saves.
    // False when nothing changed.
    bool Set(const std::wstring& path, std::wstring_view alias_in) {
        if (path.empty()) return false;
        const std::wstring alias = Trimmed(alias_in);
        const std::wstring key = Key(path);
        std::wstring file;
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = map_.find(key);
            if (alias.empty()) {
                if (it == map_.end()) return false;
                map_.erase(it);
            } else {
                if (it != map_.end() && it->second.alias == alias) return false;
                map_[key] = AliasEntry{Normalized(path), alias};
            }
            count_.store(map_.size(), std::memory_order_relaxed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            file = file_;
            json = SerializeLocked();
        }
        if (!file.empty()) {
            GlobalAsyncFileWriter().Flush();
            WriteUtf8FileAtomic(file, json);
        }
        return true;
    }

    // A rename or move took `from` to `to`: its alias, and those of anything
    // under it, follow.
    void Move(const std::wstring& from, const std::wstring& to) {
        if (from.empty() || to.empty() || Empty()) return;
        const std::wstring from_key = Key(from);
        std::wstring file;
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::vector<AliasEntry> moved;
            for (auto it = map_.begin(); it != map_.end();) {
                const std::wstring& k = it->first;
                const bool same = k == from_key;
                const bool under = k.size() > from_key.size() && k.compare(0, from_key.size(), from_key) == 0 &&
                                   (k[from_key.size()] == L'\\' || from_key.back() == L'\\');
                if (!same && !under) { ++it; continue; }
                AliasEntry e = it->second;
                e.path = Normalized(to) + e.path.substr(std::min(e.path.size(), Normalized(from).size()));
                moved.push_back(std::move(e));
                it = map_.erase(it);
            }
            if (moved.empty()) return;
            for (auto& e : moved) map_[Key(e.path)] = e;
            revision_.fetch_add(1, std::memory_order_relaxed);
            file = file_;
            json = SerializeLocked();
        }
        if (!file.empty()) GlobalAsyncFileWriter().Submit(std::move(file), std::move(json));
    }

    // 导出: writes every alias to `file` in the store's own format.
    bool ExportTo(const std::wstring& file) const {
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            json = SerializeLocked();
        }
        return WriteUtf8FileAtomic(file, json);
    }
    // 导入: merges the aliases in `file` (an export or another aliases.json);
    // an imported alias replaces the current one of the same item. Returns
    // how many changed, or -1 when the file has no aliases object.
    int ImportFrom(const std::wstring& file) {
        std::wstring text;
        if (!ReadUtf8File(file, text)) return -1;
        const std::wstring object = json::ExtractObject(text, L"aliases");
        if (object.empty()) return -1;
        std::vector<AliasEntry> incoming;
        json::ForEachMember(object, [&](const std::wstring& key, const std::wstring& raw) {
            if (raw.size() < 2 || raw.front() != L'"') return;
            size_t pos = 1;
            std::wstring alias = Trimmed(json::UnescapeString(raw, pos));
            if (!key.empty() && !alias.empty()) incoming.push_back(AliasEntry{Normalized(key), alias});
        });
        int changed = 0;
        std::wstring target;
        std::wstring json;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& e : incoming) {
                auto& slot = map_[Key(e.path)];
                if (slot.alias == e.alias && slot.path == e.path) continue;
                slot = std::move(e);
                ++changed;
            }
            if (changed == 0) return 0;
            count_.store(map_.size(), std::memory_order_relaxed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            target = file_;
            json = SerializeLocked();
        }
        if (!target.empty()) WriteUtf8FileAtomic(target, json);
        return changed;
    }

    std::vector<AliasEntry> All() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<AliasEntry> out;
        out.reserve(map_.size());
        for (const auto& [k, e] : map_) out.push_back(e);
        return out;
    }

    // Address bar: "alias" or "alias\rest" (case-insensitive) → the aliased
    // folder joined with rest. Only folders that exist are jump targets.
    bool ResolveTyped(std::wstring_view text, std::wstring& path) const {
        std::wstring t = Trimmed(text);
        while (!t.empty() && t.front() == L'"') t.erase(t.begin());
        while (!t.empty() && t.back() == L'"') t.pop_back();
        if (t.empty() || Empty()) return false;
        if (t.size() >= 2 && t[1] == L':') return false;       // C:\...
        if (t.starts_with(L"\\\\") || t.starts_with(L"%")) return false;
        const size_t cut = t.find_first_of(L"\\/");
        const std::wstring head = Trimmed(std::wstring_view(t).substr(0, cut));
        std::wstring rest = cut == std::wstring::npos ? std::wstring() : t.substr(cut);
        for (auto& c : rest) if (c == L'/') c = L'\\';
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (head.empty()) return false;
        const std::wstring folded = Lower(head);
        std::wstring target;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [k, e] : map_) {
                if (Lower(e.alias) != folded) continue;
                const DWORD attrs = GetFileAttributesW(e.path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
                target = e.path;
                break;
            }
        }
        if (target.empty()) return false;
        if (!rest.empty()) {
            if (!target.empty() && target.back() == L'\\') target.pop_back();
            target += rest;
        }
        path = std::move(target);
        return true;
    }

    // Filter matching on an alias: substring, then — when the app installed
    // it — pinyin (full or initials, "fbb" finds 发布包). The hook keeps this
    // header free of the pinyin tables for targets that don't link them.
    using PinyinMatcher = bool (*)(std::wstring_view name, std::wstring_view folded_query);
    static PinyinMatcher& PinyinHook() {
        static PinyinMatcher hook = nullptr;
        return hook;
    }
    static bool AliasMatches(std::wstring_view alias, std::wstring_view folded_query) {
        if (alias.empty() || folded_query.empty()) return false;
        if (Lower(alias).find(folded_query) != std::wstring::npos) return true;
        const PinyinMatcher hook = PinyinHook();
        return hook && hook(alias, folded_query);
    }

    // Omnibar suggestions for `query`: aliased folders, best first — the
    // alias itself, then an alias starting with it, its pinyin, an alias
    // containing it, and last a real folder name containing it.
    std::vector<AliasEntry> Suggest(std::wstring_view query, size_t limit) const {
        std::vector<AliasEntry> out;
        for (auto& [rank, e] : SuggestRanked(query, limit)) out.push_back(std::move(e));
        return out;
    }

    // Suggest, keeping how each matched: 0 exact, 1 prefix, 2 pinyin,
    // 3 alias contains, 4 real folder name contains.
    std::vector<std::pair<int, AliasEntry>> SuggestRanked(std::wstring_view query, size_t limit) const {
        const std::wstring q = Lower(Trimmed(query));
        if (q.empty() || Empty()) return {};
        std::vector<std::pair<int, AliasEntry>> ranked;
        for (const AliasEntry& e : All()) {
            const std::wstring a = Lower(e.alias);
            const size_t slash = e.path.find_last_of(L'\\');
            const std::wstring base = Lower(slash == std::wstring::npos ? e.path : e.path.substr(slash + 1));
            int rank = -1;
            if (a == q) rank = 0;
            else if (a.starts_with(q)) rank = 1;
            else if (PinyinHook() && PinyinHook()(e.alias, q)) rank = 2;
            else if (a.find(q) != std::wstring::npos) rank = 3;
            else if (base.find(q) != std::wstring::npos) rank = 4;
            if (rank < 0) continue;
            const DWORD attrs = GetFileAttributesW(e.path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
            ranked.emplace_back(rank, e);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& l, const auto& r) {
            return l.first != r.first ? l.first < r.first : l.second.alias < r.second.alias;
        });
        if (ranked.size() > limit) ranked.resize(limit);
        return ranked;
    }

    static std::wstring Lower(std::wstring_view s) {
        std::wstring out(s);
        if (!out.empty())
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.c_str(), static_cast<int>(out.size()),
                          out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
        return out;
    }

private:
    static std::wstring Trimmed(std::wstring_view s) {
        auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000 || c == L'\r' || c == L'\n'; };
        while (!s.empty() && space(s.front())) s.remove_prefix(1);
        while (!s.empty() && space(s.back())) s.remove_suffix(1);
        return std::wstring(s);
    }
    // Trailing separators go, except on a drive root ("C:\").
    static std::wstring Normalized(std::wstring_view path) {
        // "\\?\C:\x" and "\\?\UNC\srv\x" name the same items as "C:\x" and "\\srv\x".
        if (path.starts_with(L"\\\\?\\UNC\\")) {
            std::wstring p = L"\\\\" + std::wstring(path.substr(8));
            return Normalized(p);
        }
        if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
        std::wstring p(path);
        for (auto& c : p) if (c == L'/') c = L'\\';
        while (p.size() > 3 && p.back() == L'\\') p.pop_back();
        return p;
    }
    static std::wstring Key(std::wstring_view path) { return Lower(Normalized(path)); }

    std::wstring SerializeLocked() const {
        std::wstring out = L"{\n  \"version\": 1,\n  \"display\": " +
                           std::to_wstring(display_.load(std::memory_order_relaxed)) + L",\n  \"mark\": " +
                           (mark_.load(std::memory_order_relaxed) ? L"true" : L"false") + L",\n  \"aliases\": {";
        bool first = true;
        for (const auto& [k, e] : map_) {
            out += first ? L"\n    \"" : L",\n    \"";
            first = false;
            json::Escape(e.path, out);
            out += L"\": \"";
            json::Escape(e.alias, out);
            out += L"\"";
        }
        out += first ? L"}\n}\n" : L"\n  }\n}\n";
        return out;
    }

    mutable std::mutex mu_;
    std::wstring file_;
    std::unordered_map<std::wstring, AliasEntry> map_;
    std::atomic<size_t> count_{0};
    std::atomic<int> display_{0};
    std::atomic<bool> mark_{false};
    std::atomic<uint64_t> revision_{1};
};

} // namespace pulse::app
