#include "file_dialog_client_state.h"
#include <cwctype>
#include <vector>
#include <string_view>

namespace pulse::shell_bridge {
namespace {
bool ValidDirectory(std::wstring_view path) noexcept {
    if (path.empty() || path.size() >= 32768) return false;
    bool unc = false;
    if (path.starts_with(L"\\\\?\\UNC\\")) { path.remove_prefix(8); unc = true; }
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    else if (path.starts_with(L"\\\\")) { path.remove_prefix(2); unc = true; }
    size_t start = 0;
    if (!unc) {
        if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' || path[2] != L'\\') return false;
        start = 3;
    }
    size_t components = 0;
    for (size_t i = start; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\') {
            const wchar_t ch = path[i];
            if (ch < 32 || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
                ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') return false;
            continue;
        }
        const auto part = path.substr(start, i - start);
        if (part.empty()) {
            if (i != path.size()) return false;
        } else {
            if (part == L"." || part == L"..") return false;
            ++components;
        }
        start = i + 1;
    }
    return !unc || components >= 2;
}
struct KeyHandle {
    HKEY value = nullptr;
    ~KeyHandle() { if (value) RegCloseKey(value); }
};
std::wstring ProcessIdentity() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    for (auto& ch : path) ch = static_cast<wchar_t>(std::towlower(ch));
    return path;
}
}
FileDialogClientState::FileDialogClientState()
    : FileDialogClientState(ProcessIdentity(), L"Software\\Pulse\\FileDialogClients") {}
FileDialogClientState::FileDialogClientState(std::wstring app_identity, std::wstring registry_root) {
    if (app_identity.empty() || registry_root.empty()) return;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    app_key_ = std::move(registry_root);
    size_t column = 0;
    for (const wchar_t ch : app_identity) {
        if (column == 0) app_key_ += L'\\';
        for (int shift = 12; shift >= 0; shift -= 4) app_key_ += digits[(ch >> shift) & 15];
        column = (column + 4) % 240;
    }
    app_key_ += L"\\Clients";
}
std::wstring FileDialogClientState::Key(REFGUID guid) const {
    if (app_key_.empty()) return {};
    wchar_t text[40]{};
    swprintf_s(text, L"\\%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return app_key_ + text;
}
std::optional<std::wstring> FileDialogClientState::Read(REFGUID guid) const noexcept try {
    const auto key = Key(guid);
    if (key.empty()) return std::nullopt;
    KeyHandle handle;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle.value) != ERROR_SUCCESS) return std::nullopt;
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(handle.value, L"Directory", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < 2 * sizeof(wchar_t) || bytes > 32768 * sizeof(wchar_t) || bytes % sizeof(wchar_t)) return std::nullopt;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    DWORD actual = bytes;
    if (RegQueryValueExW(handle.value, L"Directory", nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &actual) != ERROR_SUCCESS ||
        type != REG_SZ || actual != bytes || buffer.back() != L'\0') return std::nullopt;
    const std::wstring directory(buffer.data(), buffer.size() - 1);
    if (!ValidDirectory(directory)) return std::nullopt;
    return directory;
} catch (...) { return std::nullopt; }
bool FileDialogClientState::Write(REFGUID guid, const std::wstring& directory) const noexcept try {
    const auto key = Key(guid);
    if (key.empty() || !ValidDirectory(directory)) return false;
    KeyHandle handle;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle.value, nullptr) != ERROR_SUCCESS) return false;
    return RegSetValueExW(handle.value, L"Directory", 0, REG_SZ, reinterpret_cast<const BYTE*>(directory.c_str()),
        static_cast<DWORD>((directory.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
} catch (...) { return false; }
bool FileDialogClientState::Clear(REFGUID guid) const noexcept try {
    const auto key = Key(guid);
    if (key.empty()) return false;
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
} catch (...) { return false; }
}


