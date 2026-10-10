#include "default_file_manager.h"
#include <windows.h>
#include <cwctype>

namespace pulse::app {
namespace {
// Quotes, surrounding spaces, a trailing separator and a "shell:" prefix are ignored.
std::wstring_view ShellArgumentName(std::wstring_view raw) {
    auto equals = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() &&
            CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    while (!raw.empty() && (raw.front() == L'"' || iswspace(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && (raw.back() == L'"' || raw.back() == L'\\' || iswspace(raw.back())))
        raw.remove_suffix(1);
    constexpr std::wstring_view shell = L"shell:";
    if (raw.size() > shell.size() && equals(raw.substr(0, shell.size()), shell))
        raw.remove_prefix(shell.size());
    return raw;
}
} // namespace

bool IsRecycleBinArgument(std::wstring_view raw) {
    const std::wstring_view name = ShellArgumentName(raw);
    auto equals = [&](std::wstring_view b) {
        return name.size() == b.size() &&
            CompareStringOrdinal(name.data(), static_cast<int>(name.size()), b.data(),
                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    return equals(kRecycleBinParsingName) || equals(L"RecycleBinFolder");
}

bool IsThisPcArgument(std::wstring_view raw) {
    auto equals = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() &&
            CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    while (!raw.empty() && (raw.front() == L'"' || iswspace(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && (raw.back() == L'"' || raw.back() == L'\\' || iswspace(raw.back())))
        raw.remove_suffix(1);
    constexpr std::wstring_view shell = L"shell:";
    if (raw.size() > shell.size() && equals(raw.substr(0, shell.size()), shell))
        raw.remove_prefix(shell.size());
    return equals(raw, kThisPcParsingName) || equals(raw, L"MyComputerFolder");
}
}
