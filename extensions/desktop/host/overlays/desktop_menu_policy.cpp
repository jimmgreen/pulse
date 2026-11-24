#include "host/overlays/desktop_menu_policy.h"
#include "../../../../src/common/utf8_file.h"
#include <shlobj.h>
#include <chrono>
#include <filesystem>

namespace pulse::desktop::overlays {

DesktopMenuPolicyCache::~DesktopMenuPolicyCache() { Stop(); }

void DesktopMenuPolicyCache::Start(std::wstring path) {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this, path = std::move(path)](std::stop_token stop) mutable {
        if (path.empty()) {
            wchar_t local[MAX_PATH]{};
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return;
            path = std::wstring(local) + L"\\Pulse\\context_menu.json";
        }
        FILETIME loaded{};
        FILETIME language_loaded{};
        bool have_loaded = false;
        bool have_language = false;
        const auto app_path = (std::filesystem::path(path).parent_path() / L"app.json").wstring();
        while (!stop.stop_requested()) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) != FALSE;
            const DWORD error = exists ? ERROR_SUCCESS : GetLastError();
            if (!exists && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
                std::lock_guard lock(mutex_);
                const auto language = policy_.language;
                policy_ = {};
                policy_.language = language;
                have_loaded = false;
            } else if (exists && (!have_loaded || CompareFileTime(&loaded, &info.ftLastWriteTime) != 0)) {
                std::wstring text;
                DesktopMenuPolicy next;
                if (ReadUtf8File(path, text) && DesktopMenuPolicy::Parse(text, next)) {
                    std::lock_guard lock(mutex_);
                    next.language = policy_.language;
                    policy_ = std::move(next);
                    loaded = info.ftLastWriteTime;
                    have_loaded = true;
                }
            }
            if (GetFileAttributesExW(app_path.c_str(), GetFileExInfoStandard, &info) &&
                (!have_language || CompareFileTime(&language_loaded, &info.ftLastWriteTime) != 0)) {
                std::wstring text;
                if (ReadUtf8File(app_path, text) && json::ValidConfigObject(text)) {
                    auto language = json::ExtractString(text, L"language", L"system");
                    if (language != L"en-US" && language != L"zh-CN" && language != L"zh-TW") language = L"system";
                    std::lock_guard lock(mutex_);
                    policy_.language = std::move(language);
                    language_loaded = info.ftLastWriteTime;
                    have_language = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void DesktopMenuPolicyCache::Stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

DesktopMenuPolicy DesktopMenuPolicyCache::Snapshot() const {
    std::lock_guard lock(mutex_);
    return policy_;
}

} // namespace pulse::desktop::overlays
