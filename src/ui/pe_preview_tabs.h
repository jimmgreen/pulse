#pragma once
#include <array>
#include <string>
#include <string_view>
#include <cwctype>

namespace pulse::ui {
// Interpret only the native PE report; other text previews keep their original view.
struct PePreviewTabs {
    bool active = false;
    std::array<std::wstring, 5> pages;
    std::wstring source;

    void Update(const std::wstring& path, const std::wstring& report) {
        if (source == report && active) return;
        *this = {};
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) return;
        auto ext = path.substr(dot);
        for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (ext != L".dll" && ext != L".exe" && ext != L".sys" && ext != L".ocx" &&
            ext != L".cpl" && ext != L".scr" && ext != L".efi" && ext != L".pyd") return;
        if (!report.starts_with(L"Portable Executable (PE32") ||
            report.find(L"\nArchitecture: ") == std::wstring::npos) return;
        source = report;
        active = true;
        int section = 0;
        std::wstring overview;
        std::wstring warnings;
        for (size_t start = 0; start < report.size();) {
            const auto end = report.find(L'\n', start);
            auto line = report.substr(start, end == std::wstring::npos ? end : end - start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.starts_with(L"Sections (")) section = 1;
            else if (line == L"DLL Dependencies") section = 2;
            else if (line == L"Version information") section = 0;
            else if (line == L"Imports" || line == L"Delay imports") section = 3;
            else if (line == L"Exports") section = 4;
            if (line == L"[Preview truncated]" || line.starts_with(L"Some metadata is invalid")) {
                warnings += line + L"\n";
            }
            if (section == 2) {
                const auto token = line.find(L'[');
                if (token != std::wstring::npos) {
                    const wchar_t* icon = line.compare(token, 4, L"[OK]") == 0 ? L"\u2713 " :
                        line.compare(token, 9, L"[MISSING]") == 0 ? L"\u2717 " :
                        (line.compare(token, 9, L"[WARNING]") == 0 || line.compare(token, 7, L"[LIMIT]") == 0) ? L"\u26A0 " : L"\u2022 ";
                    line.insert(token, icon);
                }
            }
            pages[section] += line + L"\n";
            if (section == 0 && pages[1].empty() && pages[2].empty()) overview += line + L"\n";
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        pages[1] = overview + pages[1];
        for (auto& page : pages) {
            if (page.empty()) page = L"No information available in this preview.\n";
            if (!warnings.empty()) page = warnings + L"\n" + page;
        }
    }
};
} // namespace pulse::ui
