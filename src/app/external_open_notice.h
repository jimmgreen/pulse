#pragma once
#include <cstdint>
#include <limits>
#include <string>

namespace pulse::app {
struct ExternalOpenNoticeCounts {
    uint32_t timed_out = 0;
    uint32_t failed = 0;
    explicit operator bool() const { return timed_out || failed; }
};
// UI-thread state only: retain no request identities, paths or retry payloads.
struct ExternalOpenNotice {
    uint32_t pending = 0;
    uint32_t failed = 0;
    void Clear() { pending = 0; failed = 0; }
    void TimedOut(bool exiting) {
        if (exiting) { Clear(); return; }
        Increment(pending);
    }
    void Failed(bool exiting) {
        if (exiting) { Clear(); return; }
        Increment(failed);
    }
    ExternalOpenNoticeCounts Take(bool visible, bool busy, bool existing_toast, bool exiting) {
        if (exiting) { Clear(); return {}; }
        if (!visible || busy || existing_toast) return {};
        const ExternalOpenNoticeCounts count{pending, failed};
        Clear();
        return count;
    }
private:
    static void Increment(uint32_t& count) {
        if (count != (std::numeric_limits<uint32_t>::max)()) ++count;
    }
};
inline std::wstring FormatExternalOpenNotice(std::wstring text, ExternalOpenNoticeCounts count) {
    const auto replace = [&](const wchar_t* marker, uint32_t amount) {
        const std::wstring value = std::to_wstring(amount);
        for (size_t index = 0; (index = text.find(marker, index)) != std::wstring::npos; index += value.size())
            text.replace(index, 3, value);
    };
    replace(L"{t}", count.timed_out);
    replace(L"{f}", count.failed);
    replace(L"{n}", count.timed_out ? count.timed_out : count.failed);
    return text;
}

}
