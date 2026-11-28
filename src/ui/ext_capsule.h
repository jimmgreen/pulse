// ext_capsule.h - inline filename extension capsules populated by plugins
// (pulse.extcapsules). Header-only and free of D2D/USER32 dependencies so both
// the UI renderer and headless SDK/harness tests share the exact same lookup.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pulse::ui::ext_capsule {

struct Style {
    float corner_radius_dip = 4.0f;
    float padding_x_dip = 5.0f;
    float padding_y_dip = 1.0f;
    float border_width_dip = 1.0f;
    float gap_dip = 4.0f;
    float font_size_delta_dip = -1.0f;
    uint32_t font_weight = 600u;
};

struct Rule {
    uint32_t light_bg = 0;      // 0xAARRGGBB (0xFF alpha if high byte is 0)
    uint32_t light_text = 0;    // 0xAARRGGBB
    uint32_t light_border = 0;  // 0xAARRGGBB; 0 = no border
    uint32_t dark_bg = 0;
    uint32_t dark_text = 0;
    uint32_t dark_border = 0;
    bool uppercase = false;
    bool strip_dot = false;
    Style style;
};

struct Table {
    std::unordered_map<std::wstring, Rule> by_ext;  // lowercase ASCII ext without leading dot
};

struct Match {
    std::wstring_view stem;
    std::wstring_view raw_ext;  // includes leading dot, e.g. L".psd"
    std::wstring label;         // rendered text inside capsule, e.g. L".psd" or L"PSD"
    uint32_t bg_argb = 0;
    uint32_t text_argb = 0;
    uint32_t border_argb = 0;   // 0 = no border
    Style style;
};

inline uint32_t NormalizeOpaqueArgb(uint32_t color) noexcept {
    if (color == 0) return 0;
    return (color & 0xFF000000u) == 0 ? (0xFF000000u | color) : color;
}

class Store {
public:
    static Store& Instance() {
        static Store store;
        return store;
    }

    uint64_t Revision() const noexcept {
        return revision_.load(std::memory_order_acquire);
    }

    bool Empty() const noexcept {
        return empty_.load(std::memory_order_acquire);
    }

    std::shared_ptr<const Table> Snapshot() const {
        if (empty_.load(std::memory_order_acquire)) return {};
        std::lock_guard lock(mutex_);
        return table_;
    }

    void Replace(std::shared_ptr<const Table> next) {
        const bool is_empty = !next || next->by_ext.empty();
        {
            std::lock_guard lock(mutex_);
            table_ = is_empty ? nullptr : std::move(next);
        }
        empty_.store(is_empty, std::memory_order_release);
        revision_.fetch_add(1, std::memory_order_release);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Table> table_;
    std::atomic<bool> empty_{true};
    std::atomic<uint64_t> revision_{1};
};

inline uint64_t Revision() noexcept { return Store::Instance().Revision(); }

inline bool HasAnyRules() noexcept { return !Store::Instance().Empty(); }

inline std::optional<Match> MatchFileName(std::wstring_view name, bool dark_theme,
                                          const Table* explicit_table = nullptr) {
    std::shared_ptr<const Table> owned;
    const Table* table = explicit_table;
    if (!table) {
        if (Store::Instance().Empty()) return std::nullopt;
        owned = Store::Instance().Snapshot();
        table = owned.get();
        if (!table || table->by_ext.empty()) return std::nullopt;
    }
    const size_t dot = name.find_last_of(L'.');
    // Ignore extensionless names, dotfiles (".gitignore"), and trailing dots.
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= name.size()) return std::nullopt;
    const size_t ext_len = name.size() - dot - 1;
    if (ext_len == 0 || ext_len > 24) return std::nullopt;

    wchar_t lower_buf[25];
    for (size_t i = 0; i < ext_len; ++i) {
        const wchar_t ch = name[dot + 1 + i];
        if (ch >= L'A' && ch <= L'Z') lower_buf[i] = static_cast<wchar_t>(ch - L'A' + L'a');
        else if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'-') lower_buf[i] = ch;
        else return std::nullopt;
    }
    lower_buf[ext_len] = L'\0';

    const std::wstring key(lower_buf, ext_len);
    const auto found = table->by_ext.find(key);
    if (found == table->by_ext.end()) return std::nullopt;

    const Rule& rule = found->second;
    Match out;
    out.stem = name.substr(0, dot);
    out.raw_ext = name.substr(dot);
    out.style = rule.style;
    if (dark_theme) {
        out.bg_argb = NormalizeOpaqueArgb(rule.dark_bg);
        out.text_argb = NormalizeOpaqueArgb(rule.dark_text);
        out.border_argb = NormalizeOpaqueArgb(rule.dark_border);
    } else {
        out.bg_argb = NormalizeOpaqueArgb(rule.light_bg);
        out.text_argb = NormalizeOpaqueArgb(rule.light_text);
        out.border_argb = NormalizeOpaqueArgb(rule.light_border);
    }
    if (!rule.strip_dot) out.label.push_back(L'.');
    for (size_t i = 0; i < ext_len; ++i) {
        wchar_t ch = lower_buf[i];
        if (rule.uppercase && ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        out.label.push_back(ch);
    }
    return out;
}

}  // namespace pulse::ui::ext_capsule
