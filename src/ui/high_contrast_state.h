#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>

namespace pulse::ui {
// SystemParametersInfo can block in USER32/UxTheme. Query at initialization and
// on settings notifications, never for each item drawn. Readers do not lock.
class HighContrastState {
public:
    using Query = bool (*)(bool&) noexcept;
    explicit HighContrastState(Query query) noexcept : query_(query) { Refresh(); }
    bool Read() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    bool Refresh() noexcept {
        std::lock_guard lock(refresh_mutex_);
        bool value = false;
        query_count_.fetch_add(1, std::memory_order_relaxed);
        if (!query_(value)) return false; // Preserve the last successful snapshot.
        enabled_.store(value, std::memory_order_relaxed);
        return true;
    }
    unsigned long long QueryCount() const noexcept { return query_count_.load(std::memory_order_relaxed); }
private:
    Query query_;
    std::atomic<bool> enabled_{false};
    std::atomic<unsigned long long> query_count_{0};
    std::mutex refresh_mutex_;
};

inline bool QuerySystemHighContrast(bool& enabled) noexcept {
    HIGHCONTRASTW hc{sizeof(hc)};
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) return false;
    enabled = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    return true;
}

inline HighContrastState& SystemHighContrastState() noexcept {
    static HighContrastState state(QuerySystemHighContrast);
    return state;
}

inline bool RefreshHighContrast() noexcept { return SystemHighContrastState().Refresh(); }
inline bool IsHighContrast() noexcept { return SystemHighContrastState().Read(); }
} // namespace pulse::ui
