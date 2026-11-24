// native_recycle.h - Direct NTFS/local-volume Recycle Bin move ($I v2 + $R).
//
// ITransferSource::RecycleItem walks large trees (e.g. 158k-file node_modules)
// through the Shell (~45 s) and then synchronously purges expired Recycle Bin
// items via single-threaded Shell deletion (+55 s after the folder already
// vanished from disk). Moving directly into <Drive>:\$Recycle.Bin\<SID> with
// a standard Windows v2 $I record reduces the same operation to a single fast
// FindFirstFileExW measurement (~2-3 s, live progress, instant cancel) plus an
// O(1) same-volume MoveFileExW (<1 ms), while preserving 100% Explorer and
// Pulse Undo/Restore compatibility. Excess Recycle Bin quota is compacted
// asynchronously on a low-priority background thread so the foreground delete
// never hangs after the source has already moved.
#pragma once

#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/recycle_index.h"
#include "../common/worker_thread.h"
#include "recycle_empty.h"

#include <windows.h>
#include <sherrors.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace pulse::ops::native_recycle {

enum class FallbackReason : uint32_t {
    None = 0,
    UnsupportedPath = 1,
    InsideRecycleBin = 2,
    DifferentVolume = 3,
    NotFixedDrive = 4,
    NoUserSid = 5,
    BinNotInitialized = 6,
    ReparseOrCloudRoot = 7,
    BinIndexCreateFailed = 8,
    MoveFallback = 9
};

struct ScanProgress {
    uint64_t scanned_items = 0;
    uint64_t scanned_bytes = 0;
    bool moving = false;
};

using ScanProgressFn = std::function<void(const ScanProgress&)>;

struct RecycleMetrics {
    bool used_native = false;
    FallbackReason fallback = FallbackReason::None;
    DWORD fallback_win32_error = 0;
    uint64_t scanned_items = 0;
    uint64_t scanned_bytes = 0;
    uint64_t scan_ms = 0;
    uint64_t index_write_ms = 0;
    uint64_t move_ms = 0;
    uint64_t notify_ms = 0;
    uint64_t quota_check_ms = 0;
    uint64_t bin_used_bytes = 0;
    uint64_t bin_max_bytes = 0;
    uint32_t quota_victims_queued = 0;
    uint64_t quota_victim_bytes = 0;
};

struct NativeRecycleResult {
    bool handled = false;       // true = succeeded or definitive error; false = fall back to Shell
    HRESULT hr = S_OK;
    bool cancelled = false;
    bool mutated = false;
    bool is_directory = false;
    std::wstring recycled_payload; // $R path on success
    std::wstring bin_dir;          // X:\$Recycle.Bin\<SID>
    RecycleMetrics metrics;
};

struct QuotaCompactionEvent {
    uint64_t elapsed_ms = 0;
    uint64_t freed_bytes = 0;
    uint32_t victims_removed = 0;
    uint32_t victims_failed = 0;
    uint64_t files_deleted = 0;
    DWORD first_error = ERROR_SUCCESS;
    bool stopped = false;
};

using QuotaCompactionObserver = std::function<void(const QuotaCompactionEvent&)>;

struct QuotaVictim {
    std::wstring index_path;
    std::wstring payload_path;
    uint64_t size_bytes = 0;
    uint64_t deleted_ft = 0;
};

namespace detail {

constexpr DWORD kCloudOrReparseMask =
    FILE_ATTRIBUTE_REPARSE_POINT |
    FILE_ATTRIBUTE_OFFLINE |
    0x00040000u | // FILE_ATTRIBUTE_RECALL_ON_OPEN
    0x00080000u | // FILE_ATTRIBUTE_PINNED
    0x00100000u | // FILE_ATTRIBUTE_UNPINNED
    0x00400000u;  // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS

inline std::wstring NormalizePlainPath(std::wstring_view raw) {
    std::wstring plain = pulse::path::StripExtendedPathPrefix(raw);
    for (auto& ch : plain) {
        if (ch == L'/') ch = L'\\';
    }
    while (plain.size() > 3 && plain.back() == L'\\') plain.pop_back();
    return plain;
}

inline std::wstring ExtendedPath(const std::wstring& plain) {
    if (plain.starts_with(L"\\\\?\\")) return plain;
    if (plain.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + plain.substr(2);
    return L"\\\\?\\" + plain;
}

inline std::wstring UpperFold(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(towupper(ch));
    return value;
}

inline bool IsLocalDrivePath(const std::wstring& plain) {
    return plain.size() > 3 &&
        ((plain[0] >= L'A' && plain[0] <= L'Z') || (plain[0] >= L'a' && plain[0] <= L'z')) &&
        plain[1] == L':' && plain[2] == L'\\';
}

inline bool IsInsideRecycleBinPath(const std::wstring& plain) {
    if (plain.size() < 15) return false;
    const std::wstring prefix = UpperFold(plain.substr(2, 13));
    if (prefix != L"\\$RECYCLE.BIN") return false;
    return plain.size() == 15 || plain[15] == L'\\';
}

inline std::wstring ExtractExtension(const std::wstring& plain) {
    const size_t slash = plain.find_last_of(L'\\');
    const std::wstring_view leaf = slash == std::wstring::npos
        ? std::wstring_view(plain)
        : std::wstring_view(plain).substr(slash + 1);
    const size_t dot = leaf.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= leaf.size()) return {};
    const std::wstring_view ext = leaf.substr(dot);
    if (ext.size() > 16) return {};
    for (wchar_t ch : ext) {
        if (ch == L' ' || ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
            return {};
    }
    return std::wstring(ext);
}

inline std::wstring GenerateSixCharTag() {
    static constexpr wchar_t kAlphabet[] = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static std::atomic<uint64_t> counter{0};
    uint64_t seed = 0;
    using RtlGenRandomFn = BOOLEAN(WINAPI*)(PVOID, ULONG);
    if (HMODULE advapi = GetModuleHandleW(L"advapi32.dll")) {
        if (FARPROC proc = GetProcAddress(advapi, "SystemFunction036")) {
            RtlGenRandomFn gen = nullptr;
            static_assert(sizeof(gen) == sizeof(proc));
            memcpy(&gen, &proc, sizeof(gen));
            gen(&seed, sizeof(seed));
        }
    }
    if (seed == 0) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        seed = static_cast<uint64_t>(qpc.QuadPart) ^
            (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
            (static_cast<uint64_t>(GetCurrentThreadId()) << 16);
    }
    seed ^= ++counter * 0xBF58476D1CE4E5B9ull;
    std::wstring tag(6, L'0');
    for (size_t i = 0; i < 6; ++i) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        tag[i] = kAlphabet[(seed * 0x2545F4914F6CDD1Dull) % 36u];
    }
    return tag;
}

inline void NotifyShellPath(LONG event_id, const wchar_t* path) {
    if (!path || !*path) return;
    using ChangeNotifyFn = void(WINAPI*)(LONG, UINT, LPCVOID, LPCVOID);
    if (HMODULE shell = GetModuleHandleW(L"shell32.dll")) {
        if (FARPROC proc = GetProcAddress(shell, "SHChangeNotify")) {
            ChangeNotifyFn notify = nullptr;
            static_assert(sizeof(notify) == sizeof(proc));
            memcpy(&notify, &proc, sizeof(notify));
            notify(event_id, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path, nullptr);
        }
    }
}

inline void NotifyShellRecycleBin() {
    HMODULE shell = GetModuleHandleW(L"shell32.dll");
    if (!shell) return;
    using GetKnownIdListFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PIDLIST_ABSOLUTE*);
    using ChangeNotifyFn = void(WINAPI*)(LONG, UINT, LPCVOID, LPCVOID);
    using FreeIdListFn = void(WINAPI*)(PIDLIST_ABSOLUTE);
    using UpdateIconFn = void(WINAPI*)();
    if (FARPROC get_id = GetProcAddress(shell, "SHGetKnownFolderIDList")) {
        if (FARPROC notify_proc = GetProcAddress(shell, "SHChangeNotify")) {
            if (FARPROC free_proc = GetProcAddress(shell, "ILFree")) {
                GetKnownIdListFn get_fn = nullptr;
                ChangeNotifyFn notify_fn = nullptr;
                FreeIdListFn free_fn = nullptr;
                memcpy(&get_fn, &get_id, sizeof(get_fn));
                memcpy(&notify_fn, &notify_proc, sizeof(notify_fn));
                memcpy(&free_fn, &free_proc, sizeof(free_fn));
                PIDLIST_ABSOLUTE bin = nullptr;
                if (SUCCEEDED(get_fn(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)) && bin) {
                    notify_fn(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, bin, nullptr);
                    free_fn(bin);
                }
            }
        }
    }
    if (FARPROC proc = GetProcAddress(shell, "SHUpdateRecycleBinIcon")) {
        UpdateIconFn update = nullptr;
        static_assert(sizeof(update) == sizeof(proc));
        memcpy(&update, &proc, sizeof(update));
        update();
    }
}

inline bool WriteAll(HANDLE handle, const BYTE* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(size - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, data + offset, chunk, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

inline bool DirectoryMoveAccessDeniedIsInUse(const std::wstring& ext_source, const std::wstring& plain_source) {
    // On NTFS, MoveFileExW on a directory containing an open handle returns
    // ERROR_ACCESS_DENIED rather than ERROR_SHARING_VIOLATION. Verify whether
    // the caller actually holds DELETE on the directory and FILE_DELETE_CHILD
    // on its parent so locked trees trigger in-use/Restart-Manager handling
    // instead of a spurious UAC prompt.
    HANDLE dir = CreateFileW(ext_source.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return false;
    CloseHandle(dir);

    const size_t slash = plain_source.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 2) return false;
    const std::wstring parent = slash == 2 ? plain_source.substr(0, 3) : plain_source.substr(0, slash);
    HANDLE parent_handle = CreateFileW(ExtendedPath(parent).c_str(), FILE_DELETE_CHILD,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(parent_handle);
    return true;
}

inline bool DeleteBatchParallel(const std::vector<std::wstring>& files, const std::atomic<bool>& stop,
                                uint64_t& deleted_files, DWORD& first_error) {
    if (files.empty()) return true;
    const unsigned workers = files.size() < 64
        ? 1u
        : (std::clamp)(std::thread::hardware_concurrency() / 2u, 2u, 4u);
    if (workers <= 1u) {
        bool ok = true;
        for (const auto& file : files) {
            if (stop.load(std::memory_order_relaxed)) return false;
            const DWORD err = pulse::recycle_empty::detail::DeleteOneFile(file);
            if (err == ERROR_SUCCESS) ++deleted_files;
            else {
                ok = false;
                if (first_error == ERROR_SUCCESS) first_error = err;
            }
        }
        return ok;
    }

    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    std::atomic<DWORD> err_out{ERROR_SUCCESS};
    std::atomic<bool> failed{false};
    auto run = [&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) break;
            const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= files.size()) break;
            const DWORD err = pulse::recycle_empty::detail::DeleteOneFile(files[idx]);
            if (err == ERROR_SUCCESS) {
                done.fetch_add(1, std::memory_order_relaxed);
            } else {
                failed.store(true, std::memory_order_relaxed);
                DWORD expected = ERROR_SUCCESS;
                err_out.compare_exchange_strong(expected, err);
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        try {
            pool.push_back(StartWorkerThread(run));
        } catch (...) {
            run();
            break;
        }
    }
    for (auto& t : pool) if (t.joinable()) t.join();
    deleted_files += done.load(std::memory_order_relaxed);
    if (const DWORD err = err_out.load(); err != ERROR_SUCCESS && first_error == ERROR_SUCCESS)
        first_error = err;
    return !failed.load() && !stop.load();
}

// Deletes one recycled $R payload (file or arbitrarily deep directory tree)
// in bounded memory chunks, removing its $I record ONLY after the payload is
// completely gone so interrupted compaction never leaves hidden orphan data.
inline bool PurgeOneVictim(const QuotaVictim& victim, const std::atomic<bool>& stop,
                           uint64_t& files_deleted, DWORD& first_error) {
    const std::wstring ext_payload = ExtendedPath(victim.payload_path);
    const std::wstring ext_index = ExtendedPath(victim.index_path);
    WIN32_FILE_ATTRIBUTE_DATA root{};
    if (!GetFileAttributesExW(ext_payload.c_str(), GetFileExInfoStandard, &root)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return pulse::recycle_empty::detail::DeleteOneFile(ext_index) == ERROR_SUCCESS;
        }
        if (first_error == ERROR_SUCCESS) first_error = err;
        return false;
    }

    bool ok = true;
    if (!(root.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        const DWORD err = pulse::recycle_empty::detail::DeleteOneFile(ext_payload);
        if (err == ERROR_SUCCESS) ++files_deleted;
        else {
            ok = false;
            if (first_error == ERROR_SUCCESS) first_error = err;
        }
    } else if (root.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        const DWORD err = pulse::recycle_empty::detail::RemoveOneFolder(ext_payload);
        if (err != ERROR_SUCCESS) {
            ok = false;
            if (first_error == ERROR_SUCCESS) first_error = err;
        }
    } else {
        std::vector<std::wstring> pending{ext_payload};
        std::vector<std::wstring> folders{ext_payload};
        std::vector<std::wstring> file_batch;
        file_batch.reserve(4096);
        constexpr size_t kBatchLimit = 4096;
        while (!pending.empty() && !stop.load(std::memory_order_relaxed)) {
            const std::wstring dir = std::move(pending.back());
            pending.pop_back();
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data,
                                           FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                const DWORD err = GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    ok = false;
                    if (first_error == ERROR_SUCCESS) first_error = err;
                }
                continue;
            }
            struct Close { HANDLE h; ~Close() { FindClose(h); } } close{find};
            do {
                if (stop.load(std::memory_order_relaxed)) break;
                if (pulse::recycle_empty::detail::IsDots(data.cFileName)) continue;
                std::wstring child = dir + L"\\" + data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    folders.push_back(child);
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        pending.push_back(std::move(child));
                } else {
                    file_batch.push_back(std::move(child));
                    if (file_batch.size() >= kBatchLimit) {
                        if (!DeleteBatchParallel(file_batch, stop, files_deleted, first_error)) ok = false;
                        file_batch.clear();
                    }
                }
            } while (FindNextFileW(find, &data));
        }
        if (!file_batch.empty() && !stop.load(std::memory_order_relaxed)) {
            if (!DeleteBatchParallel(file_batch, stop, files_deleted, first_error)) ok = false;
            file_batch.clear();
        }
        if (ok && !stop.load(std::memory_order_relaxed)) {
            for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
                if (stop.load(std::memory_order_relaxed)) { ok = false; break; }
                const DWORD err = pulse::recycle_empty::detail::RemoveOneFolder(*it);
                if (err != ERROR_SUCCESS) {
                    ok = false;
                    if (first_error == ERROR_SUCCESS) first_error = err;
                }
            }
        }
    }

    if (!ok || stop.load(std::memory_order_relaxed)) return false;
    if (GetFileAttributesW(ext_payload.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD idx_err = pulse::recycle_empty::detail::DeleteOneFile(ext_index);
    if (idx_err != ERROR_SUCCESS) {
        if (first_error == ERROR_SUCCESS) first_error = idx_err;
        return false;
    }
    return true;
}

class BackgroundQuotaCompactor {
public:
    static BackgroundQuotaCompactor& Instance() {
        static BackgroundQuotaCompactor instance;
        return instance;
    }

    void SetEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
        if (!enabled) {
            queue_.clear();
            queued_payloads_.clear();
        }
    }

    bool IsEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

    void SetObserver(QuotaCompactionObserver observer) {
        std::lock_guard<std::mutex> lock(mutex_);
        observer_ = std::move(observer);
    }

    uint32_t Enqueue(std::vector<QuotaVictim> victims) {
        if (victims.empty()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || shutting_down_) return 0;
        uint32_t added = 0;
        for (auto& v : victims) {
            const std::wstring key = UpperFold(v.payload_path);
            if (!queued_payloads_.insert(key).second) continue;
            queue_.push_back(std::move(v));
            ++added;
        }
        if (added == 0) return 0;
        if (!worker_.joinable()) {
            stop_.store(false);
            try {
                worker_ = StartWorkerThread([this] { WorkerLoop(); });
            } catch (...) {
                queue_.clear();
                queued_payloads_.clear();
                return 0;
            }
        }
        cv_.notify_one();
        return added;
    }

    void Shutdown() {
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutting_down_ = true;
            enabled_ = false;
            stop_.store(true);
            queue_.clear();
            queued_payloads_.clear();
            to_join = std::move(worker_);
        }
        cv_.notify_all();
        if (to_join.joinable()) to_join.join();
    }

    ~BackgroundQuotaCompactor() {
        Shutdown();
    }

private:
    void WorkerLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::vector<QuotaVictim> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
                if (stop_.load()) break;
                batch.reserve(queue_.size());
                while (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }
            if (batch.empty()) continue;
            const ULONGLONG started = GetTickCount64();
            QuotaCompactionEvent ev;
            for (const auto& victim : batch) {
                if (stop_.load()) { ev.stopped = true; break; }
                if (PurgeOneVictim(victim, stop_, ev.files_deleted, ev.first_error)) {
                    ++ev.victims_removed;
                    ev.freed_bytes += victim.size_bytes;
                } else if (stop_.load()) {
                    ev.stopped = true;
                    break;
                } else {
                    ++ev.victims_failed;
                }
            }
            ev.elapsed_ms = GetTickCount64() - started;
            if (ev.victims_removed > 0) NotifyShellRecycleBin();
            QuotaCompactionObserver obs;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& victim : batch) queued_payloads_.erase(UpperFold(victim.payload_path));
                obs = observer_;
            }
            if (obs) {
                try { obs(ev); } catch (...) {}
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QuotaVictim> queue_;
    std::unordered_set<std::wstring> queued_payloads_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    bool enabled_ = false;
    bool shutting_down_ = false;
    QuotaCompactionObserver observer_;
};

} // namespace detail

inline void EnableBackgroundQuotaCompaction(bool enabled) {
    detail::BackgroundQuotaCompactor::Instance().SetEnabled(enabled);
}

inline void SetQuotaCompactionObserver(QuotaCompactionObserver observer) {
    detail::BackgroundQuotaCompactor::Instance().SetObserver(std::move(observer));
}

inline void ShutdownBackgroundQuotaCompaction() {
    detail::BackgroundQuotaCompactor::Instance().Shutdown();
}

// Builds a standard Windows Vista+/10/11 v2 $I index file compatible with
// Explorer and pulse::recycle::ParseIndex / RestoreSelectedRecyclePayload.
inline std::vector<BYTE> BuildIndexV2(std::wstring_view original_path, uint64_t size_bytes,
                                      FILETIME deleted_utc) {
    const std::wstring plain = detail::NormalizePlainPath(original_path);
    if (plain.empty() || plain.size() + 1 > pulse::recycle::kMaxPathUnits) return {};
    if (!pulse::recycle::SupportedAbsolutePath(plain)) return {};
    const uint32_t units = static_cast<uint32_t>(plain.size() + 1);
    std::vector<BYTE> out(28 + static_cast<size_t>(units) * 2, 0);
    const auto put_le = [&](size_t offset, uint64_t value, size_t count) {
        for (size_t i = 0; i < count; ++i)
            out[offset + i] = static_cast<BYTE>((value >> (8 * i)) & 0xFFu);
    };
    put_le(0, 2u, 8);
    put_le(8, size_bytes, 8);
    put_le(16, deleted_utc.dwLowDateTime, 4);
    put_le(20, deleted_utc.dwHighDateTime, 4);
    put_le(24, units, 4);
    for (size_t i = 0; i < plain.size(); ++i) {
        const uint16_t ch = static_cast<uint16_t>(plain[i]);
        out[28 + i * 2] = static_cast<BYTE>(ch & 0xFFu);
        out[28 + i * 2 + 1] = static_cast<BYTE>((ch >> 8) & 0xFFu);
    }
    return out;
}

// Fast single-pass directory measurement using FindFirstFileExW(FindExInfoBasic,
// LARGE_FETCH). Stops immediately on cancellation or once bytes exceed `limit`
// (when limit > 0), and emits throttled ScanProgress callbacks (~50 ms).
// With a limit, a folder that cannot be listed fails the measurement with its
// Win32 error (so does a listing that ends early): its size is unknown, so the
// limit cannot be shown to hold and the bin quota is never passed on a partial
// count. A folder that vanished
// during the walk holds nothing and is skipped.
inline HRESULT MeasureFolderFast(const std::wstring& plain_folder, uint64_t limit_bytes,
                                 std::atomic<bool>& cancel, uint64_t& out_items,
                                 uint64_t& out_bytes, const ScanProgressFn& on_progress = {}) {
    out_items = 0;
    out_bytes = 0;
    std::vector<std::wstring> pending{detail::ExtendedPath(plain_folder)};
    ULONGLONG last_report = 0;
    uint64_t since_check = 0;
    while (!pending.empty()) {
        if (cancel.load(std::memory_order_relaxed)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const std::wstring current = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((current + L"\\*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (limit_bytes == 0 || err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) continue;
            return HRESULT_FROM_WIN32(err != ERROR_SUCCESS ? err : ERROR_ACCESS_DENIED);
        }
        struct Close { HANDLE h; ~Close() { FindClose(h); } } close{find};
        do {
            if ((++since_check & 255u) == 0) {
                if (cancel.load(std::memory_order_relaxed)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                if (on_progress) {
                    const ULONGLONG now = GetTickCount64();
                    if (now - last_report >= 50) {
                        last_report = now;
                        on_progress({out_items, out_bytes, false});
                    }
                }
            }
            if (pulse::recycle_empty::detail::IsDots(data.cFileName)) continue;
            ++out_items;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    pending.push_back(current + L"\\" + data.cFileName);
            } else {
                const uint64_t sz = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                out_bytes = sz > UINT64_MAX - out_bytes ? UINT64_MAX : out_bytes + sz;
                if (limit_bytes > 0 && out_bytes > limit_bytes)
                    return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x7A10); // kRecycleBinTooSmall
            }
        } while (FindNextFileW(find, &data));
        // A listing cut short is a partial count, the same as one never started.
        if (limit_bytes > 0) {
            const DWORD next_error = GetLastError();
            if (next_error != ERROR_NO_MORE_FILES)
                return HRESULT_FROM_WIN32(next_error != ERROR_SUCCESS ? next_error : ERROR_ACCESS_DENIED);
        }
    }
    if (cancel.load(std::memory_order_relaxed)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (on_progress) on_progress({out_items, out_bytes, true});
    return S_OK;
}

// Scans $I headers in `bin_dir` (~1-2 ms) and selects the oldest non-protected
// entries needed to bring usage back under `max_bytes`.
inline std::vector<QuotaVictim> PlanQuotaCompaction(const std::wstring& bin_dir, uint64_t max_bytes,
                                                    const std::unordered_set<std::wstring>& protected_payloads_upper,
                                                    uint64_t& out_used_bytes) {
    out_used_bytes = 0;
    if (bin_dir.empty() || max_bytes == 0) return {};
    WIN32_FIND_DATAW data{};
    const std::wstring ext_bin = detail::ExtendedPath(bin_dir);
    HANDLE find = FindFirstFileExW((ext_bin + L"\\$I*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return {};
    struct Close { HANDLE h; ~Close() { FindClose(h); } } close{find};

    std::vector<QuotaVictim> candidates;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = data.cFileName;
        if (name.size() < 3 || name[0] != L'$' || (name[1] != L'I' && name[1] != L'i')) continue;
        const std::wstring i_path = bin_dir + L"\\" + name;
        std::wstring r_name = name;
        r_name[1] = L'R';
        const std::wstring r_path = bin_dir + L"\\" + r_name;
        if (GetFileAttributesW(detail::ExtendedPath(r_path).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        pulse::recycle::IndexRecord rec;
        if (!pulse::recycle::ReadIndex(detail::ExtendedPath(i_path), rec)) continue;
        out_used_bytes = rec.size > UINT64_MAX - out_used_bytes ? UINT64_MAX : out_used_bytes + rec.size;
        if (protected_payloads_upper.contains(detail::UpperFold(r_path))) continue;
        const uint64_t ft = (static_cast<uint64_t>(rec.deleted.dwHighDateTime) << 32) | rec.deleted.dwLowDateTime;
        candidates.push_back({i_path, r_path, rec.size, ft});
    } while (FindNextFileW(find, &data));

    if (out_used_bytes <= max_bytes) return {};
    std::sort(candidates.begin(), candidates.end(), [](const QuotaVictim& a, const QuotaVictim& b) {
        return a.deleted_ft < b.deleted_ft;
    });
    uint64_t excess = out_used_bytes - max_bytes;
    std::vector<QuotaVictim> victims;
    uint64_t planned = 0;
    for (auto& c : candidates) {
        if (planned >= excess) break;
        planned += c.size_bytes;
        victims.push_back(std::move(c));
    }
    return victims;
}

inline uint32_t QueueQuotaCompaction(const std::wstring& bin_dir, uint64_t max_bytes,
                                     const std::unordered_set<std::wstring>& protected_payloads_upper,
                                     uint64_t& out_used_bytes, uint64_t& out_victim_bytes) {
    out_used_bytes = 0;
    out_victim_bytes = 0;
    if (!detail::BackgroundQuotaCompactor::Instance().IsEnabled()) return 0;
    auto victims = PlanQuotaCompaction(bin_dir, max_bytes, protected_payloads_upper, out_used_bytes);
    if (victims.empty()) return 0;
    for (const auto& v : victims) out_victim_bytes += v.size_bytes;
    return detail::BackgroundQuotaCompactor::Instance().Enqueue(std::move(victims));
}

// Attempts a direct same-volume Recycle Bin move for `source` into `bin_dir_override`
// (empty = current user's <Drive>:\$Recycle.Bin\<SID>).
// Returns result.handled = false when the caller should fall back to Shell RecycleItem.
inline NativeRecycleResult RecycleItemNative(const std::wstring& source, uint64_t max_bytes,
                                             std::atomic<bool>& cancel,
                                             const ScanProgressFn& on_progress = {},
                                             bool emit_shell_notify = true,
                                             const std::wstring& bin_dir_override = {},
                                             const std::wstring& cached_bin_dir = {}) {
    NativeRecycleResult out;
    out.metrics.bin_max_bytes = max_bytes;
    if (cancel.load()) {
        out.handled = true;
        out.cancelled = true;
        out.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return out;
    }

    const std::wstring plain = detail::NormalizePlainPath(source);
    if (!detail::IsLocalDrivePath(plain)) {
        out.metrics.fallback = FallbackReason::UnsupportedPath;
        return out;
    }
    if (bin_dir_override.empty() && detail::IsInsideRecycleBinPath(plain)) {
        out.metrics.fallback = FallbackReason::InsideRecycleBin;
        return out;
    }

    const std::wstring ext_source = detail::ExtendedPath(plain);
    WIN32_FILE_ATTRIBUTE_DATA root_info{};
    if (!GetFileAttributesExW(ext_source.c_str(), GetFileExInfoStandard, &root_info)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            out.handled = true;
            out.hr = HRESULT_FROM_WIN32(err);
            return out;
        }
        out.metrics.fallback = FallbackReason::UnsupportedPath;
        out.metrics.fallback_win32_error = err;
        return out;
    }

    if ((root_info.dwFileAttributes & detail::kCloudOrReparseMask) != 0) {
        out.metrics.fallback = FallbackReason::ReparseOrCloudRoot;
        return out;
    }

    std::wstring bin_dir = bin_dir_override;
    if (bin_dir.empty()) {
        const std::wstring drive_root = plain.substr(0, 3);
        if (!cached_bin_dir.empty() &&
            cached_bin_dir.size() >= 3 &&
            towupper(cached_bin_dir[0]) == towupper(drive_root[0])) {
            bin_dir = cached_bin_dir;
        } else {
            wchar_t vol_root[MAX_PATH]{};
            if (!GetVolumePathNameW(ext_source.c_str(), vol_root, MAX_PATH)) {
                out.metrics.fallback = FallbackReason::DifferentVolume;
                out.metrics.fallback_win32_error = GetLastError();
                return out;
            }
            const std::wstring vol_plain = pulse::path::StripExtendedPathPrefix(vol_root);
            if (vol_plain.size() != 3 || towupper(vol_plain[0]) != towupper(drive_root[0]) ||
                vol_plain[1] != L':' || vol_plain[2] != L'\\') {
                out.metrics.fallback = FallbackReason::DifferentVolume;
                return out;
            }
            if (GetDriveTypeW(drive_root.c_str()) != DRIVE_FIXED) {
                out.metrics.fallback = FallbackReason::NotFixedDrive;
                return out;
            }
            const std::wstring sid = pulse::CurrentUserSidString();
            if (sid.empty()) {
                out.metrics.fallback = FallbackReason::NoUserSid;
                return out;
            }
            bin_dir = drive_root + L"$Recycle.Bin\\" + sid;
        }
        const DWORD bin_attrs = GetFileAttributesW(detail::ExtendedPath(bin_dir).c_str());
        if (bin_attrs == INVALID_FILE_ATTRIBUTES || !(bin_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            out.metrics.fallback = FallbackReason::BinNotInitialized;
            out.metrics.fallback_win32_error = bin_attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
            return out;
        }
    }
    out.bin_dir = bin_dir;
    out.is_directory = (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // 1. Single-pass measurement + quota limit check (zero mutation on failure/cancel).
    const ULONGLONG scan_start = GetTickCount64();
    uint64_t item_bytes = 0;
    uint64_t item_count = 1;
    if (out.is_directory) {
        const HRESULT measure_hr = MeasureFolderFast(plain, max_bytes, cancel, item_count, item_bytes, on_progress);
        out.metrics.scan_ms = GetTickCount64() - scan_start;
        out.metrics.scanned_items = item_count;
        out.metrics.scanned_bytes = item_bytes;
        if (FAILED(measure_hr)) {
            out.handled = true;
            out.hr = measure_hr;
            out.cancelled = (measure_hr == HRESULT_FROM_WIN32(ERROR_CANCELLED));
            out.metrics.used_native = true;
            return out;
        }
    } else {
        item_bytes = (static_cast<uint64_t>(root_info.nFileSizeHigh) << 32) | root_info.nFileSizeLow;
        out.metrics.scanned_items = 1;
        out.metrics.scanned_bytes = item_bytes;
        if (max_bytes > 0 && item_bytes > max_bytes) {
            out.handled = true;
            out.hr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x7A10); // kRecycleBinTooSmall
            out.metrics.used_native = true;
            return out;
        }
    }

    if (cancel.load()) {
        out.handled = true;
        out.cancelled = true;
        out.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        out.metrics.used_native = true;
        return out;
    }

    // 2. Atomically reserve $I v2 index and write metadata.
    const ULONGLONG idx_start = GetTickCount64();
    FILETIME deleted_utc{};
    GetSystemTimeAsFileTime(&deleted_utc);
    const std::vector<BYTE> index_bytes = BuildIndexV2(plain, item_bytes, deleted_utc);
    if (index_bytes.empty()) {
        out.metrics.fallback = FallbackReason::UnsupportedPath;
        return out;
    }

    const std::wstring ext = detail::ExtractExtension(plain);
    std::wstring i_path, r_path, ext_i_path, ext_r_path;
    HANDLE i_handle = INVALID_HANDLE_VALUE;
    DWORD last_create_err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::wstring tag = detail::GenerateSixCharTag();
        i_path = bin_dir + L"\\$I" + tag + ext;
        r_path = bin_dir + L"\\$R" + tag + ext;
        ext_i_path = detail::ExtendedPath(i_path);
        ext_r_path = detail::ExtendedPath(r_path);
        if (GetFileAttributesW(ext_r_path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        i_handle = CreateFileW(ext_i_path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (i_handle != INVALID_HANDLE_VALUE) break;
        last_create_err = GetLastError();
        if (last_create_err == ERROR_FILE_EXISTS || last_create_err == ERROR_ALREADY_EXISTS) continue;
        break;
    }
    if (i_handle == INVALID_HANDLE_VALUE) {
        out.metrics.fallback = FallbackReason::BinIndexCreateFailed;
        out.metrics.fallback_win32_error = last_create_err;
        return out;
    }
    // The record must reach the disk before the rename makes $R visible: the
    // rename is journaled metadata and can outlive a power loss that drops the
    // cached $I data, leaving a payload the bin can neither show nor restore.
    // About 1.3 ms per item on a SATA SSD; a failed flush falls back untouched.
    const bool wrote = detail::WriteAll(i_handle, index_bytes.data(), index_bytes.size()) &&
                       FlushFileBuffers(i_handle);
    const DWORD write_err = wrote ? ERROR_SUCCESS : GetLastError();
    CloseHandle(i_handle);
    out.metrics.index_write_ms = GetTickCount64() - idx_start;
    if (!wrote) {
        DeleteFileW(ext_i_path.c_str());
        out.metrics.fallback = FallbackReason::BinIndexCreateFailed;
        out.metrics.fallback_win32_error = write_err;
        return out;
    }

    // 3. O(1) atomic same-volume rename into $R.
    const ULONGLONG move_start = GetTickCount64();
    const BOOL moved = MoveFileExW(ext_source.c_str(), ext_r_path.c_str(), 0);
    const DWORD move_err = moved ? ERROR_SUCCESS : GetLastError();
    out.metrics.move_ms = GetTickCount64() - move_start;

    if (!moved) {
        DeleteFileW(ext_i_path.c_str());
        if (move_err == ERROR_NOT_SAME_DEVICE || move_err == ERROR_INVALID_PARAMETER) {
            out.metrics.fallback = FallbackReason::MoveFallback;
            out.metrics.fallback_win32_error = move_err;
            return out;
        }
        out.handled = true;
        out.metrics.used_native = true;
        if (out.is_directory && move_err == ERROR_ACCESS_DENIED &&
            detail::DirectoryMoveAccessDeniedIsInUse(ext_source, plain)) {
            out.hr = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
        } else {
            out.hr = HRESULT_FROM_WIN32(move_err);
        }
        return out;
    }

    // 4. Success: emit lightweight non-blocking Shell notifications.
    out.handled = true;
    out.mutated = true;
    out.hr = S_OK;
    out.recycled_payload = r_path;
    out.metrics.used_native = true;

    if (emit_shell_notify) {
        const ULONGLONG notify_start = GetTickCount64();
        detail::NotifyShellPath(out.is_directory ? SHCNE_RMDIR : SHCNE_DELETE, plain.c_str());
        detail::NotifyShellPath(out.is_directory ? SHCNE_MKDIR : SHCNE_CREATE, r_path.c_str());
        out.metrics.notify_ms = GetTickCount64() - notify_start;
    }
    return out;
}

} // namespace pulse::ops::native_recycle
