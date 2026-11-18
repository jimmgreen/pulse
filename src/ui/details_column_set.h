// details_column_set.h — Which optional Details columns are shown (#26, #36).
//
// One global choice (AppPrefs::details_columns), edited from the column
// header's right-click menu. Bit = 1 << MainRenderer::ColumnKind value; Name
// is always shown and Path belongs to search views, so neither has a bit.
// Status (cloud files) is chosen here too, but only cloud folders draw it.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::ui {

inline constexpr uint32_t kDetailsColumnModified = 1u << 2;
inline constexpr uint32_t kDetailsColumnType = 1u << 3;
inline constexpr uint32_t kDetailsColumnSize = 1u << 4;
inline constexpr uint32_t kDetailsColumnCreated = 1u << 5;
inline constexpr uint32_t kDetailsColumnAccessed = 1u << 6;
inline constexpr uint32_t kDetailsColumnStatus = 1u << 7;

inline constexpr uint32_t kDetailsColumnsDefault =
    kDetailsColumnModified | kDetailsColumnType | kDetailsColumnSize | kDetailsColumnStatus;
inline constexpr uint32_t kDetailsColumnsAll = kDetailsColumnsDefault |
    kDetailsColumnCreated | kDetailsColumnAccessed;

constexpr uint32_t NormalizeDetailsColumns(uint32_t mask) noexcept {
    return mask & kDetailsColumnsAll;
}

// Manual widths (DIP) of the folder-view metadata columns, by slot:
// 0 modified, 1 type, 2 size, 3 created, 4 accessed. 0 = automatic.
using DetailsColumnWidths = std::array<float, 5>;

// Display order of the optional columns (AppPrefs::details_column_order),
// as MainRenderer::ColumnKind values 2..7. Name (and Path in search views)
// always lead; hidden columns keep their place for when they come back.
using DetailsColumnOrder = std::array<uint8_t, 6>;
inline constexpr uint8_t kDetailsOrderFirstKind = 2;   // ColumnKind::Date
inline constexpr uint8_t kDetailsOrderLastKind = 7;    // ColumnKind::Status
// The classic order: status after the name (as in Explorer), then modified,
// created, accessed, type, size.
inline constexpr DetailsColumnOrder kDetailsColumnOrderDefault{7, 2, 5, 6, 3, 4};

constexpr bool IsOrderableDetailsColumn(uint32_t kind) noexcept {
    return kind >= kDetailsOrderFirstKind && kind <= kDetailsOrderLastKind;
}

// Unknown or repeated entries are dropped; missing columns are appended in
// the default order, so any input yields a permutation.
constexpr DetailsColumnOrder NormalizeDetailsColumnOrder(const DetailsColumnOrder& order) noexcept {
    DetailsColumnOrder out{};
    size_t n = 0;
    uint32_t seen = 0;
    auto take = [&](uint8_t kind) {
        if (!IsOrderableDetailsColumn(kind) || (seen & (1u << kind))) return;
        seen |= 1u << kind;
        out[n++] = kind;
    };
    for (uint8_t kind : order) take(kind);
    for (uint8_t kind : kDetailsColumnOrderDefault) take(kind);
    return out;
}

// Moves `kind` next to `anchor` (before it, or after it with after = true).
// Anchoring to visible neighbours keeps hidden columns where they were.
constexpr DetailsColumnOrder MoveDetailsColumn(const DetailsColumnOrder& order, uint8_t kind,
                                               uint8_t anchor, bool after) noexcept {
    DetailsColumnOrder in = NormalizeDetailsColumnOrder(order);
    if (kind == anchor || !IsOrderableDetailsColumn(kind) || !IsOrderableDetailsColumn(anchor)) return in;
    DetailsColumnOrder out{};
    size_t n = 0;
    for (uint8_t k : in) {
        if (k == kind) continue;
        if (k == anchor && !after) out[n++] = kind;
        out[n++] = k;
        if (k == anchor && after) out[n++] = kind;
    }
    return out;
}

// Stored as "7,2,5,6,3,4".
inline std::wstring FormatDetailsColumnOrder(const DetailsColumnOrder& order) {
    std::wstring out;
    for (uint8_t kind : NormalizeDetailsColumnOrder(order)) {
        if (!out.empty()) out += L',';
        out += static_cast<wchar_t>(L'0' + kind);
    }
    return out;
}

inline DetailsColumnOrder ParseDetailsColumnOrder(std::wstring_view text) {
    DetailsColumnOrder order{};
    size_t n = 0;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9' && n < order.size()) order[n++] = static_cast<uint8_t>(c - L'0');
    }
    for (; n < order.size(); ++n) order[n] = 0;   // filled from the default
    return NormalizeDetailsColumnOrder(order);
}

} // namespace pulse::ui
