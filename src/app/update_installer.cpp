#include "../common/app_runtime_paths.h"
#include "update_installer.h"
#include "update_download_cleanup.h"
#include "update_transport.h"
#include "update_stage.h"
#include "../common/runtime_log.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <cwctype>
#include <thread>
#include <vector>

namespace pulse::app {
namespace {
constexpr uint64_t kMaximumInstallerBytes = 512ull * 1024 * 1024;
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Sha256() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};
}

bool VerifyUpdateInstaller(HANDLE file, std::wstring_view expected_hash) {
    if (file == INVALID_HANDLE_VALUE || expected_hash.size() != 64) return false;
    LARGE_INTEGER size{}, start{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaximumInstallerBytes ||
        !SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
    Sha256 sha;
    if (BCryptOpenAlgorithmProvider(&sha.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(sha.algorithm, &sha.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::array<uint8_t, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (!read) break;
        if (BCryptHashData(sha.hash, buffer.data(), read, 0) < 0) return false;
    }
    std::array<uint8_t, 32> digest{};
    if (BCryptFinishHash(sha.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i) {
        if (towlower(expected_hash[i * 2]) != hex[digest[i] >> 4] ||
            towlower(expected_hash[i * 2 + 1]) != hex[digest[i] & 15]) return false;
    }
    return true;
}

DWORD UpdateInstallErrorFromExitCode(DWORD exit_code) {
    if (exit_code == 0) return ERROR_SUCCESS;
    if (exit_code == 2 || exit_code == 5) return ERROR_CANCELLED;
    return ERROR_INSTALL_FAILURE;
}

std::wstring UpdateInstallParameters(std::wstring_view executable) {
    const auto slash = executable.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos || slash == 0 ||
        executable.find_first_of(L"\"\r\n") != std::wstring_view::npos) return {};
    return L"/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PULSEUPDATE=1 /LOG /DIR=\"" +
        std::wstring(executable.substr(0, slash)) + L"\"";
}

struct UpdateInstaller::State {
    const uint64_t operation_id = diagnostics::runtime::NextId();
    const ULONGLONG started = GetTickCount64();
    std::atomic<bool> cancelled{false};
    std::atomic<bool> downloading{true};
    std::atomic<bool> installing{false};
    std::mutex mutex;
    UpdateProgress progress{UpdatePhase::Connecting};
    void SetPhase(UpdatePhase phase) {
        std::lock_guard<std::mutex> lock(mutex);
        if (progress.phase != phase) {
            diagnostics::runtime::Event("update_install_phase", {{"operation", operation_id},
                {"phase", static_cast<uint64_t>(phase)}, {"elapsed_ms", GetTickCount64() - started}});
        }
        progress.phase = phase;
    }
    bool has_result = false;
    bool has_install_result = false;
    DWORD install_error = ERROR_SUCCESS;
    DWORD error = ERROR_SUCCESS;
    std::wstring directory, file;
    UpdateDownloadPackage package;
    FileHandle guard;
    ~State() {
        if (guard.value != INVALID_HANDLE_VALUE) {
            CloseHandle(guard.value);
            guard.value = INVALID_HANDLE_VALUE;
        }
        RemoveUpdateDownloadPackage(package);
    }

    DWORD Download(const UpdateResult& update) {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
            return ERROR_PATH_NOT_FOUND;
        if (!CreateUpdateDownloadPackage(std::wstring(local) + L"\\PulseUpdateDownloads", package))
            return ERROR_ACCESS_DENIED;
        directory = package.directory;
        file = package.file;
        {
            FileHandle output{OpenUpdateDownloadPackage(package, GENERIC_WRITE, 0)};
            if (output.value == INVALID_HANDLE_VALUE) { const DWORD open_error = GetLastError(); return open_error ? open_error : ERROR_INVALID_DATA; }
            DWORD failure = ERROR_SUCCESS;
            UpdateError category = UpdateError::None;
            if (!ReadUpdateWithFallback(update.download_page, kMaximumInstallerBytes, cancelled,
                    [&] {
                        // A fallback truncates the file and resets its denominator/counter together.
                        { std::lock_guard<std::mutex> lock(mutex); progress = {UpdatePhase::Connecting}; }
                        LARGE_INTEGER start{};
                        return SetFilePointerEx(output.value, start, nullptr, FILE_BEGIN) &&
                            SetEndOfFile(output.value);
                    },
                    [&](const void* data, DWORD size) {
                        DWORD written = 0;
                        return WriteFile(output.value, data, size, &written, nullptr) && written == size;
                    }, category, failure,
                    [&](std::wstring_view url, uint64_t maximum, const std::atomic<bool>& stop,
                        const std::function<bool(const void*, DWORD)>& consume, UpdateError& kind, DWORD& error) {
                        const auto transfer_started = GetTickCount64();
                        diagnostics::runtime::Event("update_download_start", {{"operation", operation_id}});
                        const bool received_ok = ReadUpdateResponseWithProgress(url, maximum, stop, consume, kind, error,
                            [&](uint64_t received, uint64_t total) {
                                std::lock_guard<std::mutex> lock(mutex);
                                progress = {UpdatePhase::Downloading, received, total};
                            });
                        diagnostics::runtime::Event("update_download_end", {{"operation", operation_id},
                            {"ok", received_ok}, {"error", static_cast<uint64_t>(kind)}, {"code", error},
                            {"elapsed_ms", GetTickCount64() - transfer_started}});
                        return received_ok;
                    })) return failure;
            SetPhase(UpdatePhase::Verifying);
            if (!FlushFileBuffers(output.value)) return GetLastError();
        }
        if (cancelled) return ERROR_CANCELLED;
        // Deny writes and deletion from verification until the installer has started.
        guard.value = OpenUpdateDownloadPackage(package, GENERIC_READ, FILE_SHARE_READ);
        if (guard.value == INVALID_HANDLE_VALUE) { const DWORD open_error = GetLastError(); return open_error ? open_error : ERROR_INVALID_DATA; }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(guard.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            return ERROR_INVALID_DATA;
        const auto verify_started = GetTickCount64();
        const DWORD verify_error = VerifyUpdateInstaller(guard.value, update.installer_sha256) ? ERROR_SUCCESS : ERROR_CRC;
        diagnostics::runtime::Event("update_verify_end", {{"operation", operation_id},
            {"code", verify_error}, {"elapsed_ms", GetTickCount64() - verify_started}});
        return verify_error;
    }
};

UpdateInstaller::~UpdateInstaller() { Stop(); }
bool UpdateInstaller::downloading() const noexcept { return state_ && state_->downloading; }
bool UpdateInstaller::installing() const noexcept { return state_ && state_->installing; }

UpdateProgress UpdateInstaller::Progress() const {
    if (!state_) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->progress;
}

bool UpdateInstaller::Start(const UpdateResult& update, HWND notify, UINT message) {
    if (Progress().active() || !notify || !message || update.error != UpdateError::None ||
        !update.update_available || !update.download_page.starts_with(L"https://") ||
        update.installer_sha256.size() != 64) return false;
    Stop();
    auto state = std::make_shared<State>();
    state_ = state;
    diagnostics::runtime::Event("update_install_start", {{"operation", state->operation_id}});
    try {
        std::thread([state, update, notify, message] {
            DWORD error = ERROR_GEN_FAILURE;
            try { error = state->Download(update); } catch (...) {}
            diagnostics::runtime::Event("update_prepare_end", {{"operation", state->operation_id},
                {"code", error}, {"cancelled", state->cancelled.load()},
                {"elapsed_ms", GetTickCount64() - state->started}});
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->progress.phase = !error && !state->cancelled ? UpdatePhase::Ready : UpdatePhase::Idle;
                diagnostics::runtime::Event("update_install_phase", {{"operation", state->operation_id},
                    {"phase", static_cast<uint64_t>(state->progress.phase)},
                    {"elapsed_ms", GetTickCount64() - state->started}});
                state->error = error;
                state->downloading = false;
                state->has_result = true;
            }
            if (!state->cancelled) PostMessageW(notify, message, 0, 0);
        }).detach();
    } catch (...) {
        diagnostics::runtime::Event("update_download_thread_failed", {{"operation", state->operation_id}});
        Stop();
        return false;
    }
    return true;
}

bool UpdateInstaller::TakeResult(DWORD& error) {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->has_result) return false;
    state_->has_result = false;
    error = state_->error;
    return true;
}

bool UpdateInstaller::Launch(HWND owner, DWORD& error) {
    error = ERROR_INVALID_STATE;
    if (!state_ || state_->downloading || state_->installing || state_->error || state_->guard.value == INVALID_HANDLE_VALUE) return false;
    const auto executable = app_runtime_paths::StableEntryPath();
    const std::wstring parameters = UpdateInstallParameters(executable);
    if (parameters.empty()) {
        error = ERROR_BAD_PATHNAME;
        diagnostics::runtime::Event("update_launch_parameters_failed", {{"operation", state_->operation_id}, {"code", error}});
        return false;
    }
    state_->SetPhase(UpdatePhase::Launching);
    const auto launch_started = GetTickCount64();
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    // Let the setup elevate its worker while retaining the original user's
    // token for post-install launch. Explicit runas makes Pulse elevated too.
    execute.lpVerb = L"open";
    execute.lpFile = state_->file.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        error = GetLastError();
        diagnostics::runtime::Event("update_launch_end", {{"operation", state_->operation_id},
            {"code", error}, {"elapsed_ms", GetTickCount64() - launch_started}});
        state_->SetPhase(UpdatePhase::Idle);
        return false;
    }
    diagnostics::runtime::Event("update_launch_end", {{"operation", state_->operation_id},
        {"code", ERROR_SUCCESS}, {"has_process", execute.hProcess != nullptr},
        {"elapsed_ms", GetTickCount64() - launch_started}});
    if (execute.hProcess) {
        auto state = state_;
        state->installing = true;
        state->SetPhase(UpdatePhase::Installing);
        try {
            std::thread([state, process = execute.hProcess] {
                const auto install_started = GetTickCount64();
                DWORD exit_code = 0;
                DWORD failure = ERROR_SUCCESS;
                bool has_exit_code = false;
                if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0 ||
                    !GetExitCodeProcess(process, &exit_code)) {
                    failure = GetLastError();
                    if (!failure) failure = ERROR_GEN_FAILURE;
                } else {
                    has_exit_code = true;
                    failure = UpdateInstallErrorFromExitCode(exit_code);
                }
                CloseHandle(process);
                diagnostics::runtime::Event("update_install_end", {{"operation", state->operation_id},
                    {"code", failure}, {"exit_code", exit_code}, {"has_exit_code", has_exit_code},
                    {"elapsed_ms", GetTickCount64() - install_started}});
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->progress.phase = UpdatePhase::Idle;
                    state->install_error = failure;
                    state->installing = false;
                    state->has_install_result = true;
                }
            }).detach();
        } catch (...) {
            diagnostics::runtime::Event("update_install_monitor_failed", {{"operation", state->operation_id}});
            CloseHandle(execute.hProcess);
            state->installing = false;
            state->SetPhase(UpdatePhase::Idle);
        }
    } else state_->SetPhase(UpdatePhase::Idle);
    error = ERROR_SUCCESS;
    return true;
}

void UpdateInstaller::WaitForOperations() {
    if (state_ && !state_->downloading && !state_->installing && !state_->error &&
        state_->guard.value != INVALID_HANDLE_VALUE) state_->SetPhase(UpdatePhase::WaitingOperations);
}

bool UpdateInstaller::TakeInstallResult(DWORD& error) {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->has_install_result) return false;
    state_->has_install_result = false;
    error = state_->install_error;
    return true;
}

void UpdateInstaller::Stop() {
    if (state_) {
        diagnostics::runtime::Event("update_install_release", {{"operation", state_->operation_id},
            {"downloading", state_->downloading.load()}, {"installing", state_->installing.load()},
            {"elapsed_ms", GetTickCount64() - state_->started}});
        state_->cancelled = true;
    }
    state_.reset();
}
}
