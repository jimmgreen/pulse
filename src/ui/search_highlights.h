#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

namespace pulse::ui {
struct SearchHighlight { uint32_t start, length; bool current; };

// A synchronous, borrowed view. Search offsets are sorted and share one query
// length; existing callers may still supply explicit highlights without copying.
class SearchHighlights {
public:
    SearchHighlights() = default;
    SearchHighlights(std::span<const uint32_t> offsets, uint32_t length, size_t current)
        : offsets_(offsets), length_(length), current_(current) {}
    SearchHighlights(const std::vector<SearchHighlight>& highlights) : explicit_(highlights) {}
    bool empty() const noexcept { return (offsets_.empty() || length_ == 0) && explicit_.empty(); }
    struct Iterator {
        const SearchHighlights* owner;
        size_t index, limit;
        uint64_t first, last;
        SearchHighlight operator*() const { return owner->At(index); }
        Iterator& operator++() { ++index; Skip(); return *this; }
        bool operator!=(const Iterator& other) const { return index != other.index; }
        void Skip() {
            // Explicit legacy highlights need not have equal lengths or sorted
            // ends. Preserve their original order while checking intersections.
            if (owner->explicit_.empty()) return;
            while (index < limit) {
                const auto h = owner->At(index);
                if (h.length && uint64_t{h.start} < last && uint64_t{h.start} + h.length > first) break;
                ++index;
            }
        }
    };
    struct Range {
        Iterator first, last;
        Iterator begin() const { return first; }
        Iterator end() const { return last; }
    };
    Range Intersecting(uint64_t first, uint64_t last) const {
        size_t begin = 0, end = 0;
        if (first < last && !empty()) {
            if (!explicit_.empty()) end = explicit_.size();
            else {
                const auto a = std::lower_bound(offsets_.begin(), offsets_.end(), first,
                    [&](uint32_t offset, uint64_t value) { return uint64_t{offset} + length_ <= value; });
                const auto b = std::lower_bound(a, offsets_.end(), last,
                    [](uint32_t offset, uint64_t value) { return uint64_t{offset} < value; });
                begin = static_cast<size_t>(a - offsets_.begin());
                end = static_cast<size_t>(b - offsets_.begin());
            }
        }
        Iterator a{this, begin, end, first, last}; a.Skip();
        return {a, {this, end, end, first, last}};
    }
private:
    SearchHighlight At(size_t index) const {
        return explicit_.empty() ? SearchHighlight{offsets_[index], length_, index == current_} : explicit_[index];
    }
    std::span<const uint32_t> offsets_;
    std::span<const SearchHighlight> explicit_;
    uint32_t length_ = 0;
    size_t current_ = 0;
};
} // namespace pulse::ui
