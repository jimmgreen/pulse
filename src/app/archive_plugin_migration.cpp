#include "archive_plugin_migration.h"
#include <windows.h>
#include <string>
#include <thread>
#include <system_error>

namespace pulse {
namespace {
constexpr wchar_t kTargetProperty[] = L"Pulse.ArchivePluginMigration.Target";
int target_cookie;
}

UINT ArchivePluginMigrationNoticeMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.ArchivePluginMigration.Notice.v1");
    return message;
}

bool IsArchivePluginMigrationNoticeTarget(HWND window, WPARAM token) {
    return token == reinterpret_cast<WPARAM>(&target_cookie) &&
        GetPropW(window, kTargetProperty) == &target_cookie;
}

void StartArchivePluginMigrationNotice(HWND window) {
    const UINT message = ArchivePluginMigrationNoticeMessage();
    if (!message || !SetPropW(window, kTargetProperty, &target_cookie)) return;
    try {
        std::thread([window, message] {
            if (TakeArchivePluginMigrationNotice()) {
                // The receiver validates the property too: recycled HWND values
                // cannot display a notice intended for the original window.
                PostMessageW(window, message, reinterpret_cast<WPARAM>(&target_cookie), 0);
            }
        }).detach();
    } catch (const std::system_error&) {
        RemovePropW(window, kTargetProperty);
    }
}

bool TakeArchivePluginMigrationNotice() {
    wchar_t local_app_data[32768];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, 32768);
    if (!count || count >= 32768) return false;

    const std::wstring directory = std::wstring(local_app_data, count) + L"\\Pulse\\packs\\archive\\";
    const DWORD attributes = GetFileAttributesW((directory + L"installed.json").c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;

    // CREATE_NEW arbitrates between windows/processes without a read/write race.
    // A failed creation leaves no in-memory completion flag, allowing a retry.
    HANDLE marker = CreateFileW((directory + L"plugin-migration-notice-v1.seen").c_str(),
        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) return false;
    CloseHandle(marker);
    return true;
}
}
