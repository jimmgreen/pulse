#include "route.h"
#include <shellapi.h>
#include <cwctype>
#include <vector>
namespace explorer_probe {
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
bool Resolve(const wchar_t* application, const wchar_t* command, DWORD flags, Request& out) {
    out = {};
    // Deliberately narrow: the prototype does not change debugging, suspended,
    // token, protected-process or unrecognised creation contracts.
    constexpr DWORD allowed = CREATE_UNICODE_ENVIRONMENT;
    if ((flags & ~allowed) || !command || !*command) return false;
    int count = 0;
    wchar_t** args = CommandLineToArgvW(command, &count);
    if (!args) return false;
    std::vector<std::wstring> values;
    for (int i = 0; i < count; ++i) values.emplace_back(args[i]);
    LocalFree(args);
    if (values.size() != 2) return false;
    const std::wstring image = application && *application ? application : values[0];
    // Basename matching is not identity: a cwd/PATH impostor stays untouched.
    if (_wcsicmp(image.c_str(), SystemExplorer().c_str())) return false;
    std::wstring value = values[1];
    if (value.size() >= 8 && !_wcsnicmp(value.c_str(), L"/select,", 8)) {
        out.select = true; value.erase(0, 8);
    }
    if (value.size() >= 8 && !_wcsnicmp(value.c_str(), L"file:///", 8)) {
        if (value.find_first_of(L"?#") != std::wstring::npos) return false;
        std::wstring decoded;
        if (!DecodeUrl(std::wstring_view(value).substr(8), decoded)) return false;
        value = std::move(decoded);
    }
    for (auto& ch : value) if (ch == L'/') ch = L'\\';
    if (value.size() < 3 || !((value[0] >= L'A' && value[0] <= L'Z') ||
        (value[0] >= L'a' && value[0] <= L'z')) || value[1] != L':' || value[2] != L'\\') return false;
    if (value.find_first_of(L"\"<>|?*\r\n") != std::wstring::npos || value.find(L':', 2) != std::wstring::npos) return false;
    // Explorer interprets commas in an unencoded operand as additional syntax.
    // URL-encoded commas have already become an unambiguous single operand.
    const auto raw = values[1].substr(out.select ? 8 : 0);
    if (raw.find(L',') != std::wstring::npos) return false;
    out.path = std::move(value);
    return true;
}
}
