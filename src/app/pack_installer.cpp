#include "pack_installer.h"
#include "update_installer.h"
#include "update_transport.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <cwchar>
#include <vector>

namespace pulse::app {
namespace {

constexpr uint64_t kMaximumPackFileBytes = 256ull * 1024 * 1024;

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    explicit FileHandle(HANDLE h) : value(h) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { Close(); }
    void Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    bool ok() const { return value != INVALID_HANDLE_VALUE; }
};

// The Compression API lives in cabinet.dll; load it from System32 on demand
// so Pulse keeps starting where it is missing.
class Lzms {
public:
    Lzms() {
        module_ = LoadLibraryExW(L"cabinet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return;
        create_ = reinterpret_cast<CreateFn>(GetProcAddress(module_, "CreateDecompressor"));
        decompress_ = reinterpret_cast<DecompressFn>(GetProcAddress(module_, "Decompress"));
        close_ = reinterpret_cast<CloseFn>(GetProcAddress(module_, "CloseDecompressor"));
        constexpr DWORD kCompressAlgorithmLzms = 5;
        if (create_ && decompress_ && close_ && !create_(kCompressAlgorithmLzms, nullptr, &handle_))
            handle_ = nullptr;
    }
    ~Lzms() {
        if (handle_) close_(handle_);
        if (module_) FreeLibrary(module_);
    }
    bool ready() const { return handle_ != nullptr; }
    bool Decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        SIZE_T written = 0;
        if (!decompress_(handle_, in.data(), in.size(), out.data(), out.size(), &written)) return false;
        return written == out.size();
    }

private:
    using CreateFn = BOOL(WINAPI*)(DWORD, void*, void**);
    using DecompressFn = BOOL(WINAPI*)(void*, const void*, SIZE_T, void*, SIZE_T, SIZE_T*);
    using CloseFn = BOOL(WINAPI*)(void*);
    HMODULE module_ = nullptr;
    CreateFn create_ = nullptr;
    DecompressFn decompress_ = nullptr;
    CloseFn close_ = nullptr;
    void* handle_ = nullptr;
};

// A single path component made of safe characters: catalog values end up in
// file names, so never let one climb out of the pack directory.
bool SafeComponent(const wchar_t* text) {
    if (!text || !*text || !wcscmp(text, L".") || !wcscmp(text, L"..")) return false;
    for (const wchar_t* p = text; *p; ++p) {
        const wchar_t c = *p;
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                        (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-' || c == L'_';
        if (!ok) return false;
    }
    return true;
}

bool ValidRelease(const PackRelease& release) {
    if (!release.files || !release.file_count || !SafeComponent(release.key) ||
        !SafeComponent(release.version) || !release.base_url ||
        wcsncmp(release.base_url, L"https://", 8) != 0) return false;
    const size_t url = wcslen(release.base_url);
    if (!url || release.base_url[url - 1] != L'/') return false;
    for (size_t i = 0; i < release.file_count; ++i) {
        const PackFile& f = release.files[i];
        if (f.download_name && !SafeComponent(f.download_name)) return false;
        if (!SafeComponent(f.name) || !f.sha256 || wcslen(f.sha256) != 64 || !f.packed_sha256 ||
            wcslen(f.packed_sha256) != 64 || !f.size || f.size > kMaximumPackFileBytes ||
            !f.packed_size || f.packed_size > kMaximumPackFileBytes) return false;
    }
    return true;
}

bool ReadWhole(HANDLE file, std::vector<uint8_t>& out) {
    LARGE_INTEGER size{}, start{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaximumPackFileBytes ||
        !SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
        if (!ReadFile(file, out.data() + done, chunk, &read, nullptr) || !read) return false;
        done += read;
    }
    return true;
}

bool WriteWhole(HANDLE file, const std::vector<uint8_t>& data) {
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
        if (!WriteFile(file, data.data() + done, chunk, &wrote, nullptr) || !wrote) return false;
        done += wrote;
    }
    return FlushFileBuffers(file) != FALSE;
}

DWORD Fail(DWORD error) { return error ? error : ERROR_INSTALL_FAILURE; }

// Downloads, verifies and decompresses one file into `directory`.
DWORD InstallFile(const PackRelease& release, const PackFile& file, const std::wstring& directory,
                  Lzms& lzms, PackInstallProgress& progress, uint64_t done_before,
                  const UpdateResponseReader& read, PackFailure& failure) {
    failure = PackFailure::Disk;
    const std::wstring packed_path = directory + L"\\" + file.name + L".lzms";
    const std::wstring final_path = directory + L"\\" + file.name;
    {
        FileHandle packed(CreateFileW(packed_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (!packed.ok()) return Fail(GetLastError());
        uint64_t received = 0;
        auto reset = [&] {
            LARGE_INTEGER zero{};
            received = 0;
            progress.received.store(done_before, std::memory_order_relaxed);
            return SetFilePointerEx(packed.value, zero, nullptr, FILE_BEGIN) && SetEndOfFile(packed.value);
        };
        auto consume = [&](const void* data, DWORD size) {
            if (received + size > file.packed_size) return false;
            DWORD wrote = 0;
            if (!WriteFile(packed.value, data, size, &wrote, nullptr) || wrote != size) return false;
            received += size;
            progress.received.store(done_before + received, std::memory_order_relaxed);
            return true;
        };
        const std::wstring download_name = file.download_name ? file.download_name : std::wstring(file.name) + L".lzms";
        const std::wstring url = std::wstring(release.base_url) + download_name;
        UpdateError category = UpdateError::None;
        DWORD error = ERROR_SUCCESS;
        // A source whose file has the wrong size or hash is a source to skip
        // (a mirror with another build), not a reason to give up.
        auto verify = [&] {
            return received == file.packed_size && VerifyUpdateInstaller(packed.value, file.packed_sha256);
        };
        if (!ReadUpdateWithFallback(url, file.packed_size, progress.cancelled, reset, consume, category, error, read,
                                    verify)) {
            if (progress.cancelled.load()) return ERROR_CANCELLED;
            failure = category == UpdateError::LocalIo ? PackFailure::Disk
                    : category == UpdateError::ResponseTooLarge || category == UpdateError::Mismatch ? PackFailure::Mismatch
                    : category == UpdateError::Network || category == UpdateError::HttpStatus ? PackFailure::Network
                    : PackFailure::Other;
            return Fail(error);
        }
        std::vector<uint8_t> in, out;
        if (!ReadWhole(packed.value, in)) return Fail(GetLastError());
        packed.Close();
        DeleteFileW(packed_path.c_str());
        if (progress.cancelled.load()) return ERROR_CANCELLED;
        out.resize(static_cast<size_t>(file.size));
        if (!lzms.Decompress(in, out)) { failure = PackFailure::Mismatch; return ERROR_INVALID_DATA; }
        in.clear();
        in.shrink_to_fit();
        FileHandle plain(CreateFileW(final_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!plain.ok() || !WriteWhole(plain.value, out)) return Fail(GetLastError());
        // Hash what is on disk, not what was in memory.
        if (!VerifyUpdateInstaller(plain.value, file.sha256)) { failure = PackFailure::Mismatch; return ERROR_INVALID_DATA; }
    }
    return ERROR_SUCCESS;
}

// Takes one file from the installed version when it already matches the
// pinned size and hash. The copy is hashed again on disk, like a download;
// anything doubtful falls back to downloading.
bool ReuseInstalledFile(const PackFile& file, const std::wstring& installed, const std::wstring& directory) {
    const std::wstring source = installed + L"\\" + file.name;
    const std::wstring target = directory + L"\\" + file.name;
    {
        FileHandle in(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER size{};
        if (!in.ok() || !GetFileSizeEx(in.value, &size) || static_cast<uint64_t>(size.QuadPart) != file.size ||
            !VerifyUpdateInstaller(in.value, file.sha256)) return false;
    }
    if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) return false;
    {
        FileHandle copy(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (copy.ok() && VerifyUpdateInstaller(copy.value, file.sha256)) return true;
    }
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(target.c_str());
    return false;
}

// Old version directories and leftovers; files still in use stay until next time.
void RemoveOtherVersions(const std::wstring& base, const std::wstring& keep) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(data.cFileName, L".") ||
            !wcscmp(data.cFileName, L"..") || !_wcsicmp(data.cFileName, keep.c_str())) continue;
        RemovePackTree(base + L"\\" + data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

} // namespace

bool RemovePackTree(const std::wstring& directory) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((directory + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, 0);
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            const std::wstring child = directory + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                ok = RemovePackTree(child) && ok;
            } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ok = RemoveDirectoryW(child.c_str()) && ok;   // a junction: never follow it
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ok = DeleteFileW(child.c_str()) && ok;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return RemoveDirectoryW(directory.c_str()) && ok;
}

PackInstallOutcome InstallPack(const PackRelease& release, const std::wstring& root,
                               PackInstallProgress& progress, DWORD& error,
                               const UpdateResponseReader& read, bool reuse_installed) {
    error = ERROR_SUCCESS;
    progress.failure.store(static_cast<uint32_t>(PackFailure::None), std::memory_order_relaxed);
    // Failures without a better category are the packs folder's (Disk).
    auto failed = [&](DWORD code, PackFailure why = PackFailure::Disk) {
        error = code;
        if (code != ERROR_CANCELLED) progress.failure.store(static_cast<uint32_t>(why), std::memory_order_relaxed);
        return code == ERROR_CANCELLED ? PackInstallOutcome::Cancelled : PackInstallOutcome::Failed;
    };
    if (root.empty() || !ValidRelease(release)) return failed(ERROR_INVALID_PARAMETER, PackFailure::Other);
    uint64_t total = 0;
    for (size_t i = 0; i < release.file_count; ++i) total += release.files[i].packed_size;
    progress.total.store(total, std::memory_order_relaxed);
    progress.received.store(0, std::memory_order_relaxed);

    Lzms lzms;
    if (!lzms.ready()) return failed(ERROR_NOT_SUPPORTED, PackFailure::Other);
    const std::wstring base = root + L"\\" + release.key;
    const std::wstring version = release.version;
    const std::wstring staging = base + L"\\" + version + L".partial";
    const std::wstring target = base + L"\\" + version;
    if (!packs::CreateDirectoryChain(base)) return failed(Fail(GetLastError()));
    RemovePackTree(staging);
    if (!CreateDirectoryW(staging.c_str(), nullptr)) return failed(Fail(GetLastError()));
    // An update starts from the files of the version installed now.
    std::wstring installed_directory = reuse_installed ? packs::InstalledPackDirectory(base) : std::wstring();
    if (!_wcsicmp(installed_directory.c_str(), staging.c_str())) installed_directory.clear();

    uint64_t done = 0;
    size_t reused = 0;
    for (size_t i = 0; i < release.file_count; ++i) {
        DWORD code = progress.cancelled.load() ? ERROR_CANCELLED : ERROR_SUCCESS;
        PackFailure why = PackFailure::Other;
        if (code == ERROR_SUCCESS && !installed_directory.empty() &&
            ReuseInstalledFile(release.files[i], installed_directory, staging)) {
            ++reused;
        } else if (code == ERROR_SUCCESS) {
            code = InstallFile(release, release.files[i], staging, lzms, progress, done, read, why);
        }
        if (code != ERROR_SUCCESS) {
            RemovePackTree(staging);
            diagnostics::runtime::Event("pack_install_failed",
                                        {{"error", code}, {"file", i}, {"kind", static_cast<uint64_t>(why)}});
            return failed(code, why);
        }
        done += release.files[i].packed_size;
        progress.received.store(done, std::memory_order_relaxed);
    }
    if (reused) diagnostics::runtime::Event("pack_install_reused", {{"files", reused}});
    // Reinstalling the same version replaces it; a running preview may still
    // hold the old files, in which case the old copy stays and we fail cleanly.
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && !RemovePackTree(target)) {
        RemovePackTree(staging);
        return failed(ERROR_SHARING_VIOLATION);
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0)) {
        const DWORD code = Fail(GetLastError());
        RemovePackTree(staging);
        return failed(code);
    }
    const std::wstring installed = L"{\"version\":\"" + version + L"\",\"dir\":\"" + version + L"\"}\n";
    if (!WriteUtf8FileAtomic(base + L"\\installed.json", installed)) return failed(Fail(GetLastError()));
    RemoveOtherVersions(base, version);
    return PackInstallOutcome::Installed;
}

PackInstaller::~PackInstaller() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}

bool PackInstaller::Start(const PackRelease& release, HWND notify, UpdateResponseReader read) {
    if (running()) return false;
    if (worker_.joinable()) worker_.join();
    auto job = std::make_shared<Job>();
    job->running.store(true);
    job_ = job;
    const std::wstring root = packs::PacksRoot();
    worker_ = std::thread([job, release, root, notify, read = std::move(read)] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        // Repaint at most ~8 times a second while bytes arrive.
        std::atomic<bool> pumping{true};
        std::thread ticker([&] {
            while (pumping.load()) {
                Sleep(120);
                if (notify) InvalidateRect(notify, nullptr, FALSE);
            }
        });
        DWORD error = ERROR_SUCCESS;
        const PackInstallOutcome outcome = InstallPack(release, root, job->progress, error, read, true);
        pumping.store(false);
        ticker.join();
        job->error.store(error);
        job->outcome.store(static_cast<uint32_t>(outcome));
        job->running.store(false);
        if (notify) InvalidateRect(notify, nullptr, FALSE);
    });
    return true;
}

void PackInstaller::Cancel() noexcept {
    if (job_) job_->progress.cancelled.store(true);
}

bool PackInstaller::running() const noexcept {
    return job_ && job_->running.load();
}

float PackInstaller::progress() const noexcept {
    if (!job_) return 0.0f;
    const uint64_t total = job_->progress.total.load(std::memory_order_relaxed);
    const uint64_t received = job_->progress.received.load(std::memory_order_relaxed);
    return total ? static_cast<float>(static_cast<double>(received) / static_cast<double>(total)) : 0.0f;
}

PackInstallOutcome PackInstaller::TakeOutcome(DWORD& error, PackFailure* failure) {
    if (!job_ || job_->running.load()) return PackInstallOutcome::None;
    const auto outcome = static_cast<PackInstallOutcome>(job_->outcome.exchange(0));
    error = job_->error.load();
    if (failure) *failure = static_cast<PackFailure>(job_->progress.failure.load());
    return outcome;
}

} // namespace pulse::app
