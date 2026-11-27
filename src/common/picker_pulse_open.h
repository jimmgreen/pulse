#pragma once

#include "picker_user_storage.h"
#include "shell_open_entry.h"
#include <filesystem>
#include <memory>
#include <optional>

namespace pulse::picker_pulse_open {
inline constexpr std::wstring_view kFolder = L"pulse:picker-folder:";
inline constexpr std::wstring_view kFile = L"pulse:picker-file:";
struct Request { std::wstring path; bool file = false; };

inline std::optional<Request> Parse(std::wstring_view intent) {
    const bool file = intent.starts_with(kFile);
    const auto prefix = file ? kFile : kFolder;
    if (!intent.starts_with(prefix) || intent.size() >= 32768) return {};
    picker_user_storage::Path path;
    if (!picker_user_storage::Parse(intent.substr(prefix.size()), path)) return {};
    return Request{std::move(path.value), file};
}

inline std::wstring Intent(const std::wstring& path, bool file) {
    const auto intent = std::wstring(file ? kFile : kFolder) + path;
    const auto parsed = Parse(intent);
    return parsed ? std::wstring(file ? kFile : kFolder) + parsed->path : std::wstring();
}

// Bind to the picker module's bundle, never the intercepted application's
// executable or its working directory/PATH. Thin bundles launch the stable entry.
inline std::wstring Executable(const std::wstring& module_path) {
    namespace fs = std::filesystem;
    const fs::path image(module_path);
    if (!image.is_absolute()) return {};
    auto root = image.parent_path();
    const auto family = root.parent_path().filename().wstring();
    if (_wcsicmp(family.c_str(), L"DialogBridge") == 0 || _wcsicmp(family.c_str(), L"AppRuntime") == 0)
        root = root.parent_path().parent_path();
    return (root / L"pulse.exe").wstring();
}

struct LaunchWork { HMODULE module = nullptr; std::wstring intent; };
inline DWORD WINAPI LaunchWorker(void* context) {
    HMODULE module = nullptr;
    DWORD error = ERROR_INVALID_PARAMETER;
    {
        std::unique_ptr<LaunchWork> work(static_cast<LaunchWork*>(context));
        module = work->module;
        try {
            std::wstring path(32768, L'\0');
            const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (count && count < path.size()) {
                path.resize(count);
                const auto exe = Executable(path);
                auto command = shell_open_entry::Quote(exe) + L" " + shell_open_entry::Quote(work->intent);
                STARTUPINFOW startup{}; startup.cb = sizeof(startup);
                PROCESS_INFORMATION process{};
                if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                    CloseHandle(process.hThread); CloseHandle(process.hProcess);
                    error = ERROR_SUCCESS;
                } else error = GetLastError();
            } else error = GetLastError();
        } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    }
    // A hosted picker may close while this launch is running. Keep its module
    // loaded until the worker has stopped executing its code.
    FreeLibraryAndExitThread(module, error);
}

inline bool Launch(const std::wstring& path, bool file) {
    auto work = std::make_unique<LaunchWork>();
    work->intent = Intent(path, file);
    if (work->intent.empty()) return false;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&LaunchWorker), &work->module)) return false;
    const HANDLE thread = CreateThread(nullptr, 0, LaunchWorker, work.get(), 0, nullptr);
    if (!thread) { FreeLibrary(work->module); return false; }
    work.release(); CloseHandle(thread); return true;
}
}
