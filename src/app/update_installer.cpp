#include "update_installer.h"
#include "update_transport.h"
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

struct UpdateInstaller::State {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> downloading{true};
    std::atomic<bool> installing{false};
    std::mutex mutex;
    bool has_result = false;
    bool has_install_result = false;
    DWORD install_error = ERROR_SUCCESS;
    DWORD error = ERROR_SUCCESS;
    std::wstring directory, file;
    FileHandle guard;
    ~State() {
        if (guard.value != INVALID_HANDLE_VALUE) {
            CloseHandle(guard.value);
            guard.value = INVALID_HANDLE_VALUE;
        }
        if (!file.empty()) DeleteFileW(file.c_str());
        if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    }

    DWORD Download(const UpdateResult& update) {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local)))
            return ERROR_PATH_NOT_FOUND;
        std::array<uint8_t, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return ERROR_GEN_FAILURE;
        std::wstring candidate = std::wstring(local) + L"\\PulseUpdate-";
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto value : random) { candidate += hex[value >> 4]; candidate += hex[value & 15]; }
        if (!CreateDirectoryW(candidate.c_str(), nullptr)) return GetLastError();
        directory = std::move(candidate);
        file = directory + L"\\PulseSetup.exe";
        {
            FileHandle output{CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
            if (output.value == INVALID_HANDLE_VALUE) return GetLastError();
            DWORD failure = ERROR_SUCCESS;
            UpdateError category = UpdateError::None;
            if (!ReadUpdateResponse(update.download_page, kMaximumInstallerBytes, cancelled,
                    [&](const void* data, DWORD size) {
                        DWORD written = 0;
                        return WriteFile(output.value, data, size, &written, nullptr) && written == size;
                    }, category, failure)) return failure;
            if (!FlushFileBuffers(output.value)) return GetLastError();
        }
        if (cancelled) return ERROR_CANCELLED;
        // Deny writes and deletion from verification until the installer has started.
        guard.value = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (guard.value == INVALID_HANDLE_VALUE) return GetLastError();
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(guard.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            return ERROR_INVALID_DATA;
        return VerifyUpdateInstaller(guard.value, update.installer_sha256) ? ERROR_SUCCESS : ERROR_CRC;
    }
};

UpdateInstaller::~UpdateInstaller() { Stop(); }
bool UpdateInstaller::downloading() const noexcept { return state_ && state_->downloading; }
bool UpdateInstaller::installing() const noexcept { return state_ && state_->installing; }

bool UpdateInstaller::Start(const UpdateResult& update, HWND notify, UINT message) {
    if (downloading() || installing() || !notify || !message || update.error != UpdateError::None ||
        !update.update_available || !update.download_page.starts_with(L"https://") ||
        update.installer_sha256.size() != 64) return false;
    Stop();
    auto state = std::make_shared<State>();
    state_ = state;
    try {
        std::thread([state, update, notify, message] {
            DWORD error = ERROR_GEN_FAILURE;
            try { error = state->Download(update); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->error = error;
                state->has_result = true;
            }
            state->downloading = false;
            if (!state->cancelled) PostMessageW(notify, message, 0, 0);
        }).detach();
    } catch (...) {
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
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = state_->file.c_str();
    execute.lpParameters = L"/SP- /NORESTART /LOG";
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) { error = GetLastError(); return false; }
    if (execute.hProcess) {
        auto state = state_;
        state->installing = true;
        try {
            std::thread([state, process = execute.hProcess] {
                DWORD exit_code = 0;
                DWORD failure = ERROR_SUCCESS;
                if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0 ||
                    !GetExitCodeProcess(process, &exit_code)) {
                    failure = GetLastError();
                    if (!failure) failure = ERROR_GEN_FAILURE;
                } else {
                    failure = UpdateInstallErrorFromExitCode(exit_code);
                }
                CloseHandle(process);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->install_error = failure;
                    state->has_install_result = true;
                }
                state->installing = false;
            }).detach();
        } catch (...) { CloseHandle(execute.hProcess); state->installing = false; }
    }
    error = ERROR_SUCCESS;
    return true;
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
    if (state_) state_->cancelled = true;
    state_.reset();
}
}
