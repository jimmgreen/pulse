#pragma once
#include <string>
#include <string_view>

namespace pulse::dialog_launcher {
// Keep identity/security checks on canonical paths. Only the process-facing
// spelling of ordinary short DOS paths is changed; extended-only names stay so.
inline std::wstring ProcessLaunchPath(std::wstring_view canonical) {
    if (!canonical.starts_with(L"\\\\?\\") || canonical.size() < 7) return std::wstring(canonical);
    const auto dos = canonical.substr(4);
    if (dos.size() >= 260 || !((dos[0] >= L'A' && dos[0] <= L'Z') || (dos[0] >= L'a' && dos[0] <= L'z')) ||
        dos[1] != L':' || dos[2] != L'\\') return std::wstring(canonical);
    for (size_t start = 3; start < dos.size();) {
        const auto end = dos.find(L'\\', start);
        const auto part = dos.substr(start, end == dos.npos ? dos.size() - start : end - start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ' || part.find_first_of(L"/:*?\"<>|") != part.npos)
            return std::wstring(canonical);
        std::wstring stem(part.substr(0, part.find(L'.')));
        for (auto& c : stem) { if (c < 32) return std::wstring(canonical); if (c >= L'a' && c <= L'z') c -= L'a' - L'A'; }
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CLOCK$" ||
            stem.starts_with(L"COM") || stem.starts_with(L"LPT")) return std::wstring(canonical);
        if (end == dos.npos) break;
        start = end + 1;
    }
    return std::wstring(dos);
}
}
