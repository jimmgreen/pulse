// layout_snapshot.cpp - 布局快照: naming, ordering and pruning of saved layout copies, and putting one back.
#include "desktop_model.h"

#include <algorithm>
#include <cwctype>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace pulse::desktop::model {
namespace {
constexpr std::wstring_view kManualPrefix = L"snap-";
constexpr std::wstring_view kAutoPrefix = L"auto-";
constexpr std::wstring_view kSuffix = L".json";

std::wstring LowerPath(std::wstring_view p) {
    std::wstring out(p);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

bool ReadDigits(std::wstring_view s, size_t at, size_t count, int& out) noexcept {
    if (at + count > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < count; ++i) {
        const wchar_t ch = s[at + i];
        if (ch < L'0' || ch > L'9') return false;
        v = v * 10 + (ch - L'0');
    }
    out = v;
    return true;
}

auto TimeKey(const LayoutSnapshotName& n) noexcept {
    return std::make_tuple(n.year, n.month, n.day, n.hour, n.minute, n.second);
}
} // namespace

std::wstring MakeLayoutSnapshotFileName(const LayoutSnapshotName& n) {
    wchar_t buf[48]{};
    swprintf_s(buf, L"%04d%02d%02d-%02d%02d%02d", std::clamp(n.year, 0, 9999), std::clamp(n.month, 0, 99),
               std::clamp(n.day, 0, 99), std::clamp(n.hour, 0, 99), std::clamp(n.minute, 0, 99),
               std::clamp(n.second, 0, 99));
    return std::wstring(n.automatic ? kAutoPrefix : kManualPrefix) + buf + std::wstring(kSuffix);
}

std::optional<LayoutSnapshotName> ParseLayoutSnapshotFileName(std::wstring_view f) noexcept {
    // prefix(5) + yyyymmdd(8) + '-' + hhmmss(6) + ".json"(5)
    if (f.size() != 5 + 8 + 1 + 6 + 5) return std::nullopt;
    LayoutSnapshotName n;
    if (f.starts_with(kAutoPrefix)) n.automatic = true;
    else if (!f.starts_with(kManualPrefix)) return std::nullopt;
    if (!f.ends_with(kSuffix) || f[13] != L'-') return std::nullopt;
    if (!ReadDigits(f, 5, 4, n.year) || !ReadDigits(f, 9, 2, n.month) || !ReadDigits(f, 11, 2, n.day) ||
        !ReadDigits(f, 14, 2, n.hour) || !ReadDigits(f, 16, 2, n.minute) || !ReadDigits(f, 18, 2, n.second)) {
        return std::nullopt;
    }
    if (n.month < 1 || n.month > 12 || n.day < 1 || n.day > 31 || n.hour > 23 || n.minute > 59 || n.second > 60) {
        return std::nullopt;
    }
    return n;
}

std::vector<std::wstring> SortLayoutSnapshots(std::vector<std::wstring> names) {
    std::vector<std::pair<LayoutSnapshotName, std::wstring>> parsed;
    for (auto& f : names) {
        if (auto n = ParseLayoutSnapshotFileName(f)) parsed.emplace_back(*n, std::move(f));
    }
    std::stable_sort(parsed.begin(), parsed.end(), [](const auto& a, const auto& b) {
        if (TimeKey(a.first) != TimeKey(b.first)) return TimeKey(a.first) > TimeKey(b.first);
        return !a.first.automatic && b.first.automatic;   // a hand-made one first at the same second
    });
    std::vector<std::wstring> out;
    out.reserve(parsed.size());
    for (auto& [n, f] : parsed) out.push_back(std::move(f));
    return out;
}

std::vector<std::wstring> LayoutSnapshotsToPrune(const std::vector<std::wstring>& names,
                                                 size_t keep_manual, size_t keep_auto) {
    std::vector<std::wstring> out;
    size_t manual = 0, automatic = 0;
    for (auto& f : SortLayoutSnapshots(names)) {
        const bool is_auto = ParseLayoutSnapshotFileName(f)->automatic;
        size_t& seen = is_auto ? automatic : manual;
        if (++seen > (is_auto ? keep_auto : keep_manual)) out.push_back(std::move(f));
    }
    return out;
}

bool DailyLayoutSnapshotDue(const std::vector<std::wstring>& names, const LayoutSnapshotName& today) noexcept {
    for (const auto& f : names) {
        const auto n = ParseLayoutSnapshotFileName(f);
        if (n && n->automatic && n->year == today.year && n->month == today.month && n->day == today.day) {
            return false;
        }
    }
    return true;
}

size_t ApplyLayoutSnapshot(const DesktopModelState& snapshot, DesktopModelState& state) {
    state.groups = snapshot.groups;
    state.tiles = snapshot.tiles;
    std::unordered_map<std::wstring, const DesktopItem*> placed;
    placed.reserve(snapshot.items.size());
    for (const auto& it : snapshot.items) placed.emplace(LowerPath(it.path), &it);
    std::unordered_set<std::string> group_ids;
    for (const auto& g : state.groups) group_ids.insert(g.id);
    size_t restored = 0;
    for (auto& it : state.items) {
        if (auto p = placed.find(LowerPath(it.path)); p != placed.end()) {
            it.loose_pos = p->second->loose_pos;
            it.pinned_group_id = p->second->pinned_group_id;
            it.force_loose = p->second->force_loose;
            ++restored;
        } else if (!it.pinned_group_id.empty() && it.pinned_group_id != kAppsTileGroupId &&
                   !group_ids.contains(it.pinned_group_id)) {
            it.pinned_group_id.clear();
        }
    }
    return restored;
}

} // namespace pulse::desktop::model
