#pragma once
#include "../common/path_utils.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::app {
// Only the latest editor in a pane owns selection. An unrelated selection or
// navigation invalidates it; an automatic relisting can carry its revision on.
struct InlineRenameSelection {
    uint64_t task = 0, view = 0, selection = 0;
    std::wstring destination;

    bool Current(uint64_t generation, uint64_t revision) const {
        return task && view == generation && selection == revision;
    }
    bool Complete(uint64_t id, uint64_t generation, std::wstring_view actual) {
        if (!task || task != id || view != generation || actual.empty()) return false;
        destination = actual;
        return true;
    }
    void Finish(uint64_t id) {
        if (task == id && destination.empty()) *this = {};
    }
    void Carry(uint64_t generation, uint64_t before, uint64_t after) {
        if (!Current(generation, before)) { *this = {}; return; }
        selection = after;
    }

    // A duplicate basename in another branch of a tree/search must not win.
    template<class Matches, class Full, class Visible>
    int Find(size_t count, Matches&& matches, Full&& full, Visible&& visible) const {
        if (destination.empty()) return -1;
        const auto actual = path::StripExtendedPathPrefix(destination);
        const auto leaf = std::wstring_view(actual).substr(actual.find_last_of(L"\\/") + 1);
        for (size_t i = 0; i < count; ++i) {
            if (matches(i, leaf) && visible(i) &&
                path::EqualInsensitive(path::StripExtendedPathPrefix(full(i)), actual))
                return static_cast<int>(i);
        }
        return -1;
    }
};

// The hint is speculative; the worker always resolves the on-disk collision.
// Do not walk an arbitrarily large listing on each keypress.
inline constexpr size_t kRenameHintRowLimit = 1024;
template<class Predict>
std::wstring InlineRenameHint(size_t rows, const std::wstring& trimmed, Predict&& predict) {
    return rows <= kRenameHintRowLimit ? predict() : trimmed;
}
} // namespace pulse::app
