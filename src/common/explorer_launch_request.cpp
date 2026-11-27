#include "explorer_launch_request.h"
#include <shellapi.h>
#include <cwctype>
#include <memory>
#include <vector>
namespace pulse::explorer_launch {
namespace {
int Hex(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}
bool DecodeUrl(std::wstring_view raw, std::wstring& decoded) {
    decoded.clear();
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != L'%') { decoded += raw[i++]; continue; }
        std::string bytes;
        while (i < raw.size() && raw[i] == L'%') {
            if (i + 2 >= raw.size() || Hex(raw[i + 1]) < 0 || Hex(raw[i + 2]) < 0) return false;
            const char byte = static_cast<char>((Hex(raw[i + 1]) << 4) | Hex(raw[i + 2]));
            if (!byte) return false;
            bytes += byte; i += 3;
        }
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (!size) return false;
        std::wstring part(static_cast<size_t>(size), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), part.data(), size)) return false;
        decoded += part;
    }
    return true;
}
bool RawArguments(std::wstring_view command, std::vector<std::wstring_view>& out) {
    size_t at = 0;
    while (at < command.size()) {
        while (at < command.size() && (command[at] == L' ' || command[at] == L'\t')) ++at;
        if (at == command.size()) break;
        const size_t begin = at;
        bool quoted = false;
        size_t slashes = 0;
        for (; at < command.size(); ++at) {
            const wchar_t ch = command[at];
            if (!quoted && (ch == L' ' || ch == L'\t')) break;
            if (ch == L'"' && !(slashes % 2)) quoted = !quoted;
            slashes = ch == L'\\' ? slashes + 1 : 0;
        }
        if (quoted) return false;
        out.push_back(command.substr(begin, at - begin));
    }
    return true;
}
bool DriveAbsolute(std::wstring_view path) {
    return path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
}
bool ValidPathText(std::wstring_view path) {
    if (path.find_first_of(L"\"<>|?*") != path.npos || path.find(L':', 2) != path.npos) return false;
    for (const auto ch : path) if (ch < 32) return false;
    return true;
}
}
std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::wstring SystemExplorer() {
    wchar_t path[32768]{};
    const UINT size = GetWindowsDirectoryW(path, ARRAYSIZE(path));
    return size && size < ARRAYSIZE(path) ? std::wstring(path, size) + L"\\explorer.exe" : L"";
}
browse::Match Classify(const wchar_t* application, const wchar_t* command, DWORD flags,
                       const wchar_t* working_directory, browse::Request& out) {
    using browse::Match;
    out = {};
    // Electron adds CREATE_NO_WINDOW even to visible GUI execFile calls.
    // Both Explorer and our bridge are GUI-subsystem executables; preserve this
    // no-console flag unchanged, without admitting debugging/suspended launches.
    constexpr DWORD allowed = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (!command || !*command) return Match::Unrelated;
    int count = 0;
    wchar_t** args = CommandLineToArgvW(command, &count);
    if (!args) return application && *application &&
        !_wcsicmp(application, SystemExplorer().c_str()) ? Match::Unsupported : Match::Unrelated;
    const auto free_args = [](wchar_t** value) { LocalFree(value); };
    const std::unique_ptr<wchar_t*, decltype(free_args)> owned_args(args, free_args);
    std::vector<std::wstring> values;
    for (int i = 0; i < count; ++i) values.emplace_back(args[i]);
    if (values.empty()) return Match::Unrelated;
    const std::wstring image = application && *application ? application : values[0];
    // Basename matching is not identity: a cwd/PATH impostor stays untouched.
    const auto explorer = SystemExplorer();
    if (explorer.empty() || _wcsicmp(image.c_str(), explorer.c_str())) return Match::Unrelated;
    // Explorer without a browsing operand can initialize the desktop shell.
    if (values.size() == 1) return Match::Unrelated;
    if (flags & ~allowed) return Match::Unsupported;
    std::vector<std::wstring_view> raw;
    if (!RawArguments(command, raw) || raw.size() != values.size()) return Match::Unsupported;
    bool select = false;
    size_t operand = 1;
    std::wstring value = values[1];
    if (value.size() >= 8 && !_wcsnicmp(value.c_str(), L"/select,", 8)) {
        select = true; value.erase(0, 8);
        if (value.empty() && values.size() == 3) { operand = 2; value = values[2]; }
    }
    if (values.size() != operand + 1 || value.empty()) return Match::Unsupported;
    const bool combined = select && operand == 1;
    const auto raw_operand = combined && raw[operand].size() >= 8 &&
        !_wcsnicmp(raw[operand].data(), L"/select,", 8) ? raw[operand].substr(8) : raw[operand];
    // Commas are literal only inside a separately quoted path operand. Quoting
    // the entire /select,token does not disambiguate Explorer's comma grammar.
    if (value.find(L',') != value.npos && raw_operand != Quote(value)) return Match::Unsupported;
    if (value.front() == L'/' || value.front() == L'-' || value.starts_with(L"::") ||
        !_wcsnicmp(value.c_str(), L"shell:", 6)) return Match::Unsupported;
    bool url = false;
    if (value.size() >= 8 && !_wcsnicmp(value.c_str(), L"file:///", 8)) {
        url = true;
        if (value.find_first_of(L"?#") != std::wstring::npos) return Match::Unsupported;
        std::wstring decoded;
        if (!DecodeUrl(std::wstring_view(value).substr(8), decoded)) return Match::Unsupported;
        value = std::move(decoded);
    } else if (!_wcsnicmp(value.c_str(), L"file:", 5)) {
        return Match::Unsupported;
    }
    for (auto& ch : value) if (ch == L'/') ch = L'\\';
    if (value.empty() || value.starts_with(L"\\\\") || !ValidPathText(value)) return Match::Unsupported;
    if (!DriveAbsolute(value)) {
        if (url || value.find(L':') != value.npos || !working_directory || !*working_directory)
            return Match::Unsupported;
        std::wstring cwd = working_directory;
        for (auto& ch : cwd) if (ch == L'/') ch = L'\\';
        if (!DriveAbsolute(cwd) || !ValidPathText(cwd)) return Match::Unsupported;
        value = value.front() == L'\\' ? cwd.substr(0, 2) + value : cwd + L"\\" + value;
        const DWORD size = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
        if (!size) return Match::Unsupported;
        std::wstring absolute(size, L'\0');
        const DWORD length = GetFullPathNameW(value.c_str(), size, absolute.data(), nullptr);
        if (!length || length >= size) return Match::Unsupported;
        absolute.resize(length); value = std::move(absolute);
    }
    out.intent = select ? browse::Intent::Reveal : browse::Intent::OpenFolder;
    out.path = std::move(value);
    return Match::Supported;
}
bool Resolve(const wchar_t* application, const wchar_t* command, DWORD flags, Request& out) {
    out = {};
    browse::Request request;
    if (Classify(application, command, flags, nullptr, request) != browse::Match::Supported) return false;
    out.select = request.intent == browse::Intent::Reveal;
    out.path = std::move(request.path);
    return true;
}
}
