#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::context_diagnostics {
// Opt-in is sampled once per process; restarting makes capture reproducible.
inline bool Enabled() {
    static const bool enabled = [] {
        try {
        wchar_t exe[32768]{};
        const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
        if (!n || n >= 32768) return false;
        std::wstring path(exe, n);
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        path.resize(slash + 1);
        path += L"pulse-context-diagnostics.flag";
        const DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        } catch (...) { return false; }
    }();
    return enabled;
}
inline std::wstring Safe(std::wstring_view value) {
    // Labels can contain selected filenames. Suppress path-like fields entirely.
    if (value.find(L'\\') != value.npos || value.find(L'/') != value.npos)
        return L"[path-like label omitted]";
    std::wstring out(value.substr(0, 160));
    for (auto& ch : out) if (ch < L' ' || ch == 0x7f) ch = L' ';
    return out;
}
class Batch {
public:
    explicit Batch(const wchar_t* stage) noexcept : enabled_(Enabled()) {
        try {
        if (enabled_) data_ = L"tick=" + std::to_wstring(GetTickCount64()) + L" pid=" +
            std::to_wstring(GetCurrentProcessId()) + L" stage=" + stage + L"\r\n";
        } catch (...) { enabled_ = false; }
    }
    void Row(const wchar_t* reason, std::wstring_view label = {}) noexcept {
        try {
        if (enabled_ && data_.size() < 60000)
            data_ += std::wstring(reason) + L" label=" + Safe(label) + L"\r\n";
        } catch (...) { enabled_ = false; }
    }
    void Number(const wchar_t* name, unsigned long long value) noexcept {
        if (!enabled_) return;
        try { Row(name, std::to_wstring(value)); } catch (...) { enabled_ = false; }
    }
    ~Batch() noexcept {
        try {
        if (!enabled_ || data_.empty()) return;
        wchar_t local[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!n || n >= 32768) return;
        std::wstring path = std::wstring(local) + L"\\Pulse";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\pulse_context_ui.log";
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) &&
            (attr.nFileSizeHigh || attr.nFileSizeLow >= 1024 * 1024))
            MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, data_.data(), static_cast<int>(data_.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return;
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, data_.data(), static_cast<int>(data_.size()), utf8.data(), bytes, nullptr, nullptr);
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
        } catch (...) {}
    }
private:
    bool enabled_;
    std::wstring data_;
};
}
