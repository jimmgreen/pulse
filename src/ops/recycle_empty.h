// recycle_empty.h - Empty the current user's Recycle Bin by deleting its $R
// payloads and $I records directly.
//
// SHEmptyRecycleBinW routes every file through the Shell. On a 12,000-file
// bin it took 7.6-11.4 s here, with 6-10 s of CPU in the caller plus another
// 6-8 s in explorer.exe, and it can neither report progress nor be cancelled.
// Deleting the same payloads on 8 threads took 2.3-2.8 s with no Explorer
// work. Header-only so tests can run it against isolated fixture folders.
#pragma once

#include "../common/current_user_security.h"
#include "../common/recycle_index.h"
#include "../common/worker_thread.h"
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::recycle_empty {

struct Progress {
    uint64_t total_items = 0;    // items the Recycle Bin shows ($I with a payload)
    uint64_t removed_items = 0;
    uint64_t total_bytes = 0;    // sizes recorded in the $I files
    uint64_t deleted_bytes = 0;
    uint64_t deleted_files = 0;
    std::wstring current;        // original name of an item being deleted
};

struct Result {
    uint64_t removed_items = 0;
    uint64_t deleted_files = 0;
    uint64_t deleted_bytes = 0;
    uint64_t failed_entries = 0; // files or folders that could not be deleted
    DWORD first_error = ERROR_SUCCESS;
    bool cancelled = false;
};

using CancelFn = std::function<bool()>;
using ProgressFn = std::function<void(const Progress&)>;

// "\\?\X:\$Recycle.Bin\<SID>" on every local volume. Payloads are often deeper
// than MAX_PATH (node_modules, build trees).
inline std::vector<std::wstring> CurrentUserBinFolders() {
    std::vector<std::wstring> out;
    const std::wstring sid = CurrentUserSidString();
    if (sid.empty()) return out; // Never fall back to other users' folders.
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0) continue;
        const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        out.push_back(std::wstring(L"\\\\?\\") + root + L"$Recycle.Bin\\" + sid);
    }
    return out;
}

// Explorer's Recycle Bin views and the desktop icon refresh from these.
inline void NotifyShell() {
    PIDLIST_ABSOLUTE bin = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin))) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, bin, nullptr);
        CoTaskMemFree(bin);
    }
    // Documented, but missing from some SDK headers.
    using UpdateIcon = void(WINAPI*)();
    if (HMODULE shell = GetModuleHandleW(L"shell32.dll")) {
        if (FARPROC proc = GetProcAddress(shell, "SHUpdateRecycleBinIcon")) {
            UpdateIcon update = nullptr;
            static_assert(sizeof(update) == sizeof(proc));
            memcpy(&update, &proc, sizeof(update));
            update();
        }
    }
}

namespace detail {
#ifdef PULSE_RESOURCE_REUSE_TEST
inline size_t metadata_entry_limit = 262144;
inline size_t file_capacity_growths = 0;
inline bool reuse_scratch = true;
#endif

// Preserve whole-item enumeration before mutation, including on resource
// exhaustion. Account both paths and container nodes before adding them.
struct MetadataBudget {
    static constexpr size_t kMaxBytes = 64 * 1024 * 1024;
    static constexpr size_t kMaxEntries = 262144;
    size_t bytes = 0, entries = 0;
    bool Admit(size_t characters, size_t overhead = 128) {
        if (characters > (kMaxBytes - overhead) / sizeof(wchar_t)) return false;
        const size_t cost = characters * sizeof(wchar_t) + overhead;
        size_t limit = kMaxEntries;
#ifdef PULSE_RESOURCE_REUSE_TEST
        limit = metadata_entry_limit;
#endif
        if (entries >= limit || cost > kMaxBytes - bytes) return false;
        bytes += cost;
        ++entries;
        return true;
    }
};

struct Item {
    std::wstring index;   // $I path; empty for a payload without one
    std::wstring payload; // $R path; empty for an orphan $I
    std::wstring name;    // original file name, for progress
    std::vector<std::wstring> folders; // payload folders, each before its children
    uint64_t files = 0;
    uint64_t left = 0;    // files not yet attempted in the current batch
    uint64_t failed = 0;
};

struct File {
    std::wstring path;
    uint64_t bytes = 0;
    size_t item = 0;
};

inline bool IsDots(const wchar_t* name) {
    return name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

inline uint64_t SizeOf(const WIN32_FIND_DATAW& data) {
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// Pairs $I and $R entries by their shared suffix. desktop.ini and anything
// else in the folder is left alone.
inline DWORD ListBinFolder(const std::wstring& folder, std::vector<Item>& items, MetadataBudget& budget) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    struct Close { HANDLE handle; ~Close() { FindClose(handle); } } close{find};
    std::map<std::wstring, size_t> by_suffix;
    do {
        const std::wstring name = data.cFileName;
        if (name.size() < 3 || name[0] != L'$') continue;
        const bool index = name[1] == L'I' || name[1] == L'i';
        const bool payload = name[1] == L'R' || name[1] == L'r';
        if (!index && !payload) continue;
        if (index && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!budget.Admit(folder.size() + name.size() * 3 + 1)) return ERROR_NOT_ENOUGH_MEMORY;
        std::wstring suffix = name.substr(2);
        CharUpperBuffW(suffix.data(), static_cast<DWORD>(suffix.size()));
        auto [it, added] = by_suffix.emplace(suffix, items.size());
        if (added) items.emplace_back();
        Item& item = items[it->second];
        (index ? item.index : item.payload) = folder + L"\\" + name;
        if (payload && item.name.empty()) item.name = name;
    } while (FindNextFileW(find, &data));
    const DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}

// Lists a payload without following links: a junction or symbolic link is
// deleted as itself, never through its target.
inline DWORD ListPayload(size_t item_index, Item& item, std::vector<File>& files,
                        const std::function<bool()>& stop, MetadataBudget& budget) {
    WIN32_FILE_ATTRIBUTE_DATA root{};
    if (!GetFileAttributesExW(item.payload.c_str(), GetFileExInfoStandard, &root)) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    if (!budget.Admit(item.payload.size() * 2)) return ERROR_NOT_ENOUGH_MEMORY;
    if (!(root.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
#ifdef PULSE_RESOURCE_REUSE_TEST
        if (files.size() == files.capacity()) ++file_capacity_growths;
#endif
        files.push_back({item.payload,
            (static_cast<uint64_t>(root.nFileSizeHigh) << 32) | root.nFileSizeLow, item_index});
        ++item.files;
        return ERROR_SUCCESS;
    }
    item.folders.push_back(item.payload);
    if (root.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_SUCCESS;
    std::vector<std::wstring> pending{item.payload};
    while (!pending.empty() && !stop()) {
        const std::wstring folder = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) continue;
            return error;
        }
        struct Close { HANDLE handle; ~Close() { FindClose(handle); } } close{find};
        do {
            if (stop()) return ERROR_OPERATION_ABORTED;
            if (IsDots(data.cFileName)) continue;
            if (!budget.Admit((folder.size() + wcslen(data.cFileName) + 1) * 2))
                return ERROR_NOT_ENOUGH_MEMORY;
            std::wstring child = folder + L"\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                item.folders.push_back(child);
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    pending.push_back(std::move(child));
            } else {
#ifdef PULSE_RESOURCE_REUSE_TEST
                if (files.size() == files.capacity()) ++file_capacity_growths;
#endif
                files.push_back({std::move(child), SizeOf(data), item_index});
                ++item.files;
            }
        } while (FindNextFileW(find, &data));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) return error;
    }
    return ERROR_SUCCESS;
}

inline DWORD DeleteOneFile(const std::wstring& path) {
    if (DeleteFileW(path.c_str())) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (error == ERROR_ACCESS_DENIED && SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        if (DeleteFileW(path.c_str())) return ERROR_SUCCESS;
        error = GetLastError();
    }
    return error;
}

inline DWORD RemoveOneFolder(const std::wstring& path) {
    if (RemoveDirectoryW(path.c_str())) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (error == ERROR_ACCESS_DENIED && SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_DIRECTORY)) {
        if (RemoveDirectoryW(path.c_str())) return ERROR_SUCCESS;
        error = GetLastError();
    }
    return error;
}

} // namespace detail

// Empties the given bin folders. Each item's payload is deleted completely
// before its $I record, so an interrupted or failed item stays visible in the
// Recycle Bin instead of leaving hidden payload behind. Cancelling stops
// before the next file; untouched items remain intact.
inline Result EmptyFolders(const std::vector<std::wstring>& bin_folders, const CancelFn& cancelled,
                           const ProgressFn& progress, unsigned threads = 0) {
    using namespace detail;
    // Batch target; the hard metadata budget is enforced inside enumeration.
    constexpr size_t kBatchFiles = 20000;
    Result result;
    std::atomic<bool> stop{false};
    const auto should_stop = [&] {
        if (!stop.load() && cancelled && cancelled()) stop.store(true);
        return stop.load();
    };
    Progress state;
    const auto report = [&] { if (progress) progress(state); };

    std::vector<Item> items;
    MetadataBudget item_budget;
    for (const auto& folder : bin_folders) {
        const DWORD error = ListBinFolder(folder, items, item_budget);
        if (error != ERROR_SUCCESS) { result.first_error = error; result.failed_entries = 1; return result; }
    }
    for (auto& item : items) {
        if (item.index.empty()) continue;
        recycle::IndexRecord record;
        if (recycle::ReadIndex(item.index, record)) {
            const size_t slash = record.original_path.find_last_of(L"\\/");
            const size_t offset = slash == std::wstring::npos ? 0 : slash + 1;
            if (!item_budget.Admit(record.original_path.size() - offset)) {
                result.first_error = ERROR_NOT_ENOUGH_MEMORY; result.failed_entries = 1; return result;
            }
            item.name = record.original_path.substr(offset);
            if (!item.payload.empty()) state.total_bytes += record.size;
        }
        if (!item.payload.empty()) ++state.total_items;
    }
    report();
    if (threads == 0) threads = (std::clamp)(std::thread::hardware_concurrency(), 2u, 8u);

    std::atomic<uint64_t> deleted_files{0}, deleted_bytes{0};
    std::atomic<DWORD> first_error{ERROR_SUCCESS};
    const auto note_error = [&](DWORD error) {
        DWORD none = ERROR_SUCCESS;
        first_error.compare_exchange_strong(none, error);
    };
    size_t begin = 0;
    std::vector<File> scratch;
    while (begin < items.size() && !should_stop()) {
#ifdef PULSE_RESOURCE_REUSE_TEST
        std::vector<File> fresh;
        auto& files = reuse_scratch ? scratch : fresh;
#else
        auto& files = scratch;
#endif
        // Keep only a bounded array between batches; clear releases all paths.
        if (files.capacity() > 2 * 1024 * 1024 / sizeof(File)) std::vector<File>().swap(files);
        else files.clear();
        MetadataBudget payload_budget;
        size_t end = begin;
        while (end < items.size() && (end == begin || files.size() < kBatchFiles)) {
            const size_t previous = files.size();
            const DWORD error = items[end].payload.empty() ? ERROR_SUCCESS :
                ListPayload(end, items[end], files, should_stop, payload_budget);
            if (error != ERROR_SUCCESS && error != ERROR_OPERATION_ABORTED) {
                files.resize(previous);
                items[end].files = 0;
                std::vector<std::wstring>().swap(items[end].folders);
                ++items[end].failed;
                note_error(error);
            }
            items[end].left = items[end].files;
            ++end;
            if (should_stop()) break;
        }
        if (should_stop()) break; // A partly listed batch is left untouched.

        // The disk, not the CPU, is the limit: a few threads delete in parallel.
        std::atomic<size_t> next{0};
        std::atomic<unsigned> running{0};
        std::mutex completion_mutex;
        std::condition_variable completed;
        std::mutex counts;
        const unsigned workers = static_cast<unsigned>(
            (std::min<size_t>)(threads, files.size()));
        std::vector<std::thread> pool;
        pool.reserve(workers);
        struct JoinWorkers {
            std::vector<std::thread>& threads;
            std::atomic<bool>& stop;
            ~JoinWorkers() {
                for (auto& thread : threads) if (thread.joinable()) {
                    stop = true;
                    thread.join();
                }
            }
        } join_workers{pool, stop};
        const auto work = [&](bool coordinator) {
                for (size_t i = next++; i < files.size() && !stop.load(); i = next++) {
                    if (coordinator && should_stop()) break;
                    const File& file = files[i];
                    const DWORD error = DeleteOneFile(file.path);
                    if (error == ERROR_SUCCESS) {
                        ++deleted_files;
                        deleted_bytes += file.bytes;
                    } else {
                        note_error(error);
                    }
                    std::lock_guard lock(counts);
                    Item& item = items[file.item];
                    --item.left;
                    if (error != ERROR_SUCCESS) ++item.failed;
                }
                {
                    std::lock_guard lock(completion_mutex);
                    --running;
                }
                completed.notify_one();
        };
        for (unsigned t = 0; t < workers; ++t) {
            ++running;
            try {
                pool.push_back(StartWorkerThread([&] { work(false); }));
            } catch (...) {
                // Continue from the shared cursor; already deleted files must
                // never be replayed. Existing workers remain owned until join.
                work(true);
                break;
            }
        }
        for (;;) {
            const bool finished = running.load() == 0;
            should_stop();
            state.deleted_files = deleted_files.load();
            state.deleted_bytes = deleted_bytes.load();
            const size_t at = (std::min)(next.load(), files.size());
            if (at < files.size()) state.current = items[files[at].item].name;
            report();
            if (finished) break;
            std::unique_lock lock(completion_mutex);
            completed.wait_for(lock, std::chrono::milliseconds(100), [&] { return running.load() == 0; });
        }
        for (auto& thread : pool) thread.join();

        // Folders deepest first, then the $I record once its payload is gone.
        for (size_t i = begin; i < end; ++i) {
            Item& item = items[i];
            if (item.left == 0 && item.failed == 0) {
                for (auto it = item.folders.rbegin(); it != item.folders.rend(); ++it) {
                    const DWORD error = RemoveOneFolder(*it);
                    if (error != ERROR_SUCCESS) { ++item.failed; note_error(error); }
                }
            }
            const bool payload_gone = item.payload.empty() ||
                GetFileAttributesW(item.payload.c_str()) == INVALID_FILE_ATTRIBUTES;
            if (item.left == 0 && item.failed == 0 && payload_gone) {
                const DWORD error = item.index.empty() ? ERROR_SUCCESS : DeleteOneFile(item.index);
                if (error != ERROR_SUCCESS) { ++item.failed; note_error(error); }
                else if (!item.index.empty() && !item.payload.empty()) ++state.removed_items;
            }
            result.failed_entries += item.failed;
            std::vector<std::wstring>().swap(item.folders);
            std::wstring().swap(item.index);
            std::wstring().swap(item.payload);
            std::wstring().swap(item.name);
        }
        report();
        begin = end;
    }
    result.removed_items = state.removed_items;
    result.deleted_files = deleted_files.load();
    result.deleted_bytes = deleted_bytes.load();
    result.first_error = first_error.load();
    result.cancelled = stop.load();
    state.deleted_files = result.deleted_files;
    state.deleted_bytes = result.deleted_bytes;
    state.current.clear();
    report();
    return result;
}

// Confirm the original current-user bin roots only. Do not infer emptiness from
// the Shell's item count: orphaned or inaccessible payloads may be omitted there.
inline DWORD ConfirmEmptyFolders(const std::vector<std::wstring>& folders) {
    for (const auto& folder : folders) {
        const DWORD attributes = GetFileAttributesW(folder.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)))
            return ERROR_ACCESS_DENIED;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
            // A missing bin on an online volume is empty; a disconnected or
            // unreadable volume is unknown and must not clear a prior failure.
            std::vector<wchar_t> volume(32768);
            if (!GetVolumePathNameW(folder.c_str(), volume.data(), static_cast<DWORD>(volume.size())) ||
                !GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0))
                return GetLastError();
            continue;
        }
        struct Close { HANDLE value; ~Close() { FindClose(value); } } close{find};
        do {
            const auto* name = data.cFileName;
            if (name[0] == L'$' && name[1] && name[2] &&
                (name[1] == L'I' || name[1] == L'i' || name[1] == L'R' || name[1] == L'r'))
                return ERROR_DIR_NOT_EMPTY;
        } while (FindNextFileW(find, &data));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) return error;
    }
    return ERROR_SUCCESS;
}

inline HRESULT FinalizeResult(const Result& native, HRESULT shell_result,
                              const std::vector<std::wstring>& folders) {
    if (native.cancelled) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (FAILED(shell_result)) return shell_result;
    if (!native.failed_entries) return shell_result;
    if (folders.empty()) return HRESULT_FROM_WIN32(native.first_error ? native.first_error : ERROR_GEN_FAILURE);
    const DWORD remaining = ConfirmEmptyFolders(folders);
    if (remaining == ERROR_SUCCESS) return S_OK;
    return HRESULT_FROM_WIN32(remaining == ERROR_DIR_NOT_EMPTY && native.first_error
        ? native.first_error : remaining);
}

} // namespace pulse::recycle_empty
