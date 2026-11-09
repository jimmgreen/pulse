#include "../common/app_runtime_paths.h"
#include "about_info.h"

#include "../common/localization.h"
#include "pulse_release_notes.h"
#include "pulse_version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace pulse::app {
namespace {

std::wstring Utf8ToWide(const char* data, size_t size) {
    if (!data || !size) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), out.data(), n);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == 0xFEFF)) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) --b;
    return s.substr(a, b - a);
}

// Minimal Markdown to plain text: list items become bullets, headings and
// emphasis/code markers are dropped, [text](url) keeps the text.
std::wstring StripInline(const std::wstring& line) {
    std::wstring out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (c == L'*' && i + 1 < line.size() && line[i + 1] == L'*') { ++i; continue; }
        if (c == L'`') continue;
        if (c == L'[') {
            const size_t close = line.find(L"](", i);
            const size_t end = close == std::wstring::npos ? close : line.find(L')', close);
            if (end != std::wstring::npos) {
                out.append(line, i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        out.push_back(c);
    }
    return Trim(out);
}

ui::ReleaseNoteView ParseNote(const wchar_t* version, const std::wstring& text) {
    ui::ReleaseNoteView note;
    note.version = version;
    note.current = note.version == PULSE_VERSION_STRING;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.rfind(L"![", 0) == 0 || line.rfind(L"<!--", 0) == 0) continue;
        bool bullet = false;
        bool heading = false;
        if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'+') &&
            line[1] == L' ') {
            bullet = true;
            line = line.substr(2);
        } else {
            size_t hashes = 0;
            while (hashes < line.size() && line[hashes] == L'#') ++hashes;
            if (hashes) line = line.substr(hashes);
            heading = hashes > 0;
        }
        // Before StripInline, which reads "[tag](url)" as a link.
        std::wstring tag_label;
        const ui::ReleaseTag tag = bullet ? ui::TakeReleaseTag(line, tag_label) : ui::ReleaseTag::None;
        line = StripInline(line);
        if (line.empty()) continue;
        note.lines.push_back(std::move(line));
        note.bullets.push_back(bullet);
        note.headings.push_back(heading);
        note.tags.push_back(tag);
        note.tag_labels.push_back(std::move(tag_label));
    }
    return note;
}

std::wstring BuildTimeText() {
    // PULSE_BUILD_ID is "YYYYMMDDTHHMMSSZ-<hash>" in UTC; show local time.
    const std::wstring id = PULSE_BUILD_ID;
    SYSTEMTIME utc{};
    if (id.size() < 16 || id[8] != L'T' || id[15] != L'Z' ||
        swscanf_s(id.c_str(), L"%4hu%2hu%2huT%2hu%2hu%2hu", &utc.wYear, &utc.wMonth, &utc.wDay,
                  &utc.wHour, &utc.wMinute, &utc.wSecond) != 6)
        return {};
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t text[32]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute);
    return text;
}

const std::wstring& WindowsText() {
    static const std::wstring text = [] {
        wchar_t product[128]{}, display[64]{}, build[32]{};
        DWORD ubr = 0;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                          KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof(product);
            RegGetValueW(key, nullptr, L"ProductName", RRF_RT_REG_SZ, nullptr, product, &size);
            size = sizeof(display);
            if (RegGetValueW(key, nullptr, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS) {
                size = sizeof(display);
                RegGetValueW(key, nullptr, L"ReleaseId", RRF_RT_REG_SZ, nullptr, display, &size);
            }
            size = sizeof(build);
            RegGetValueW(key, nullptr, L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &size);
            size = sizeof(ubr);
            RegGetValueW(key, nullptr, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
            RegCloseKey(key);
        }
        std::wstring name = *product ? product : L"Windows";
        // ProductName still says "Windows 10" on Windows 11.
        if (wcstoul(build, nullptr, 10) >= 22000) {
            const size_t at = name.find(L"Windows 10");
            if (at != std::wstring::npos) name.replace(at, 10, L"Windows 11");
        }
        if (*display) name += L" " + std::wstring(display);
        if (*build) {
            name += L" (" + std::wstring(build);
            if (ubr) name += L"." + std::to_wstring(ubr);
            name += L")";
        }
        return name;
    }();
    return text;
}

std::wstring InstallDirectory() {
    return app_runtime_paths::InstallRoot();
}

constexpr const wchar_t* kArchitecture =
#if defined(_M_ARM64)
    L"ARM64";
#elif defined(_M_X64)
    L"x64";
#else
    L"x86";
#endif

} // namespace

static std::vector<ui::ReleaseNoteView> LoadReleaseNotes(bool english) {
    std::vector<ui::ReleaseNoteView> out;
    HMODULE module = GetModuleHandleW(nullptr);
    const auto find = [module](int id) {
        return FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    };
    for (int i = 0; kPulseReleaseNoteVersions[i]; ++i) {
        const int id = PULSE_RELEASE_NOTE_RESOURCE_BASE + i;
        HRSRC res = english ? find(id + PULSE_RELEASE_NOTE_ENGLISH_OFFSET) : nullptr;
        if (!res) res = find(id);
        if (!res) continue;
        HGLOBAL handle = LoadResource(module, res);
        const DWORD size = SizeofResource(module, res);
        const char* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
        if (!data || !size) continue;
        auto note = ParseNote(kPulseReleaseNoteVersions[i], Utf8ToWide(data, size));
        if (!note.lines.empty()) out.push_back(std::move(note));
    }
    return out;
}

const std::vector<ui::ReleaseNoteView>& EmbeddedReleaseNotes() {
    static const std::vector<ui::ReleaseNoteView> chinese = LoadReleaseNotes(false);
    static const std::vector<ui::ReleaseNoteView> english = LoadReleaseNotes(true);
    switch (l10n::effective_language()) {
    case l10n::Language::EnUS:
        return english;
    case l10n::Language::ZhTW: {
        // Release notes ship in Simplified Chinese and English only.
        static const std::vector<ui::ReleaseNoteView> traditional = [] {
            auto notes = chinese;
            for (auto& note : notes)
                for (auto& line : note.lines) line = l10n::ToTraditional(line);
            for (auto& note : notes)
                for (auto& label : note.tag_labels)
                    if (!label.empty()) label = l10n::ToTraditional(label);
            return notes;
        }();
        return traditional;
    }
    default:
        return chinese;
    }
}

std::vector<AboutRow> BuildAboutRows(bool index_service, bool index_installed, float scale) {
    using I = l10n::StringId;
    std::vector<AboutRow> rows;
    rows.emplace_back(l10n::Get(I::AboutVersion),
                      std::wstring(PULSE_VERSION_STRING) + L" \u00B7 " + kArchitecture);
    const std::wstring built = BuildTimeText();
    if (!built.empty()) rows.emplace_back(l10n::Get(I::AboutBuilt), built);
    rows.emplace_back(l10n::Get(I::AboutBuildId), PULSE_BUILD_ID);
    rows.emplace_back(l10n::Get(I::AboutOs), WindowsText());
    rows.emplace_back(l10n::Get(I::AboutLocation), InstallDirectory());
    rows.emplace_back(l10n::Get(I::AboutIndex),
                      l10n::Get(index_service ? I::AboutIndexService
                                : index_installed ? I::AboutIndexWaiting : I::AboutIndexUser));
    wchar_t display[128]{};
    swprintf_s(display, l10n::Get(I::AboutDisplayFormat).c_str(),
               static_cast<int>(std::lround(scale * 100.0f)));
    rows.emplace_back(l10n::Get(I::AboutDisplay), display);
    return rows;
}

std::wstring AboutRowsText(const std::vector<AboutRow>& rows) {
    std::wstring text = L"Pulse\r\n";
    for (const auto& [label, value] : rows) text += label + L": " + value + L"\r\n";
    return text;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(memory)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

} // namespace pulse::app
