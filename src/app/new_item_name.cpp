#include "context_menu.h"
#include "../common/path_utils.h"

namespace pulse::app {
std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext) {
    return UniqueChildName(dir, base, ext, {});
}
std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext, const std::function<bool()>& cancelled) {
    auto exists = [&](const std::wstring& name) {
        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += name;
        return pulse::path::Exists(full);
    };
    std::wstring candidate = base + ext;
    for (int i = 2;; ++i) {
        if (cancelled && cancelled()) return {};
        const bool occupied = exists(candidate);
        if (cancelled && cancelled()) return {};
        if (!occupied) return candidate;
        candidate = base + L" (" + std::to_wstring(i) + L")" + ext;
    }
}
}
