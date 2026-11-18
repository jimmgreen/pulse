#pragma once

#include "FluentTokens.h"

namespace pulse::ui {
namespace text_contrast_detail {
inline thread_local const bool* current = nullptr;
}

inline bool TextOptimizationBaselineForTest() noexcept {
#ifdef PULSE_WITH_SELFTEST
    static const bool baseline = [] {
        wchar_t value[4]{};
        return GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0 &&
            GetEnvironmentVariableW(L"PULSE_TEST_TEXT_BASELINE", value, ARRAYSIZE(value)) == 1 && value[0] == L'1';
    }();
    return baseline;
#else
    return false;
#endif
}

// One system setting snapshot per synchronous frame. Outside a frame callers
// still query Windows, and nested draws restore their caller's snapshot.
class TextContrastScope {
public:
    explicit TextContrastScope(bool high_contrast, bool active = true) noexcept
        : value_(high_contrast), previous_(text_contrast_detail::current) {
        if (active) text_contrast_detail::current = &value_;
    }
    ~TextContrastScope() { text_contrast_detail::current = previous_; }
    TextContrastScope(const TextContrastScope&) = delete;
    TextContrastScope& operator=(const TextContrastScope&) = delete;
private:
    bool value_;
    const bool* previous_;
};

inline bool TextHighContrast() noexcept {
    return text_contrast_detail::current ? *text_contrast_detail::current : IsHighContrast();
}
} // namespace pulse::ui
