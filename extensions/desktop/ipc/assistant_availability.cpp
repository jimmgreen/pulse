#include "ipc/assistant_availability.h"

#include "../../../src/common/json_value.h"

#include <iterator>
#include <utility>

namespace pulse::desktop::ai {
namespace {

bool InstallDefault(HANDLE pulse_process) {
    wchar_t exe[32768];
    DWORD length = static_cast<DWORD>(std::size(exe));
    if (pulse_process) {
        if (!QueryFullProcessImageNameW(pulse_process, 0, exe, &length)) return true;
    } else {
        length = GetModuleFileNameW(nullptr, exe, length);
    }
    if (length == 0 || length >= std::size(exe)) return true;
    std::wstring ini(exe, length);
    const auto slash = ini.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    ini.resize(slash + 1);
    ini += L"setup-defaults.ini";
    return GetPrivateProfileIntW(L"features", L"ai_assistant", 1, ini.c_str()) != 0;
}

} // namespace

AssistantAvailability::AssistantAvailability(HANDLE pulse_process, std::wstring ai_dir) {
    if (ai_dir.empty()) {
        default_enabled_ = InstallDefault(pulse_process);
        enabled_ = default_enabled_;
        wchar_t root[32768];
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
        if (length == 0 || length >= std::size(root)) return;
        ai_dir = std::wstring(root, length) + L"\\Pulse\\ai";
    }
    path_ = std::move(ai_dir) + L"\\ai.json";
}

bool AssistantAvailability::Enabled() {
    if (path_.empty()) return true;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            file_read_ = false;
            enabled_ = default_enabled_;
        }
        return enabled_;
    }
    if (file_read_ && CompareFileTime(&last_write_, &attributes.ftLastWriteTime) == 0 &&
        size_.HighPart == attributes.nFileSizeHigh && size_.LowPart == attributes.nFileSizeLow) {
        return enabled_;
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (size.QuadPart > json::ParseLimits{}.max_bytes) return enabled_;
    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return enabled_;
    FILETIME write_time{};
    DWORD count = 0;
    const bool read = GetFileTime(file, nullptr, nullptr, &write_time) &&
        ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &count, nullptr);
    CloseHandle(file);
    if (!read || count != raw.size() || CompareFileTime(&attributes.ftLastWriteTime, &write_time) != 0) return enabled_;

    std::string_view utf8(raw);
    if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);
    json::Value root;
    if (!json::Parse(utf8, root) || !root.is_object()) return enabled_;
    last_write_ = write_time;
    size_ = size;
    file_read_ = true;
    enabled_ = root.get_bool("enabled", true);
    return enabled_;
}

} // namespace pulse::desktop::ai
