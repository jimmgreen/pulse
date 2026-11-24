#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <winternl.h>
namespace pulse::ops::native_delete {
#ifdef PULSE_NATIVE_DELETE_TEST
inline bool fail_next_enumeration = false;
inline bool deny_first_enumeration = false;
inline bool force_disposition_unsupported = false;
inline std::atomic<size_t> disposition_probes{0};
inline void (*after_mutation)() = nullptr;
#endif
// FILE_DISPOSITION_INFO_EX is spelled out because the Windows 8.1 build
// targets headers older than it.
constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosixSemantics = 0x2;
constexpr DWORD kDispositionIgnoreReadonly = 0x10;


inline std::wstring NativePath(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

inline std::wstring JoinChild(const std::wstring& dir, const wchar_t* name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

// ERROR_SUCCESS, or why the item is still there.
inline DWORD DeleteNativeItem(const std::wstring& path, DWORD attributes, bool& disposition_supported) {
    const std::wstring api_path = NativePath(path);
    if (disposition_supported) {
        HANDLE handle = CreateFileW(api_path.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        struct { DWORD flags; } info{kDispositionDelete | kDispositionPosixSemantics | kDispositionIgnoreReadonly};
        BOOL deleted = FALSE;
#ifdef PULSE_NATIVE_DELETE_TEST
        ++disposition_probes;
        if (force_disposition_unsupported) SetLastError(ERROR_NOT_SUPPORTED);
        else
#endif
        deleted = SetFileInformationByHandle(handle, kFileDispositionInfoEx, &info, sizeof(info));
        const DWORD code = deleted ? ERROR_SUCCESS : GetLastError();
        CloseHandle(handle);
        if (deleted) return ERROR_SUCCESS;
        // Cache unsupported capability only for this root traversal.
        // Every subsequent root probes its own backend again.
        if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED &&
            code != ERROR_INVALID_FUNCTION)
            return code;
        disposition_supported = false;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(api_path.c_str())
                                                                 : DeleteFileW(api_path.c_str());
    if (removed) return ERROR_SUCCESS;
    const DWORD code = GetLastError();
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes);
    return code;
}

struct NativeDeleteOutcome {
    bool done = false;       // the source no longer exists
    bool mutated = false;    // something under it was deleted
    bool cancelled = false;
    bool enumeration_failed = false;
    DWORD error = ERROR_SUCCESS;  // why the item it stopped at is still there
};

// Streaming postorder: one enumeration handle and path per active depth.
// A parent is removed only after enumeration and every visited child succeed.
template <typename Progress>
NativeDeleteOutcome DeleteNative(const std::wstring& source, std::atomic<bool>& cancel, Progress&& progress,
                                 unsigned workers = 1) {
    NativeDeleteOutcome outcome;
    bool disposition_supported = true;
    if (source.size() <= 3 || source.starts_with(L"::") || source.find(L'/') != std::wstring::npos)
        return outcome;
    const DWORD attributes = GetFileAttributesW(NativePath(source).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return outcome;
    struct Frame {
        std::wstring path;
        DWORD attributes;
        HANDLE find = INVALID_HANDLE_VALUE;
        HANDLE guard = INVALID_HANDLE_VALUE;
        WIN32_FIND_DATAW data{};
        bool opened = false;
        bool advance = false;
    };
    std::vector<Frame> stack;
    struct Cleanup {
        std::vector<Frame>& frames;
        ~Cleanup() {
            for (auto& f : frames) {
                if (f.find != INVALID_HANDLE_VALUE) FindClose(f.find);
                if (f.guard != INVALID_HANDLE_VALUE) CloseHandle(f.guard);
            }
        }
    } cleanup{stack};
    stack.push_back({source, attributes});
    struct Ready { std::wstring path; DWORD attributes; };
    std::vector<Ready> ready;
    if (workers > 1) ready.reserve(32);
    // These atomics and the callable outlive the one reusable work object.
    // Every drain waits before changing the batch or resetting its state.
    std::atomic<size_t> next{0};
    std::atomic<DWORD> failure{ERROR_SUCCESS};
    std::atomic<bool> changed{false};
    std::atomic<bool> supported{true};
    auto work = [&]() {
        while (!cancel.load() && failure.load() == ERROR_SUCCESS) {
            const size_t index = next.fetch_add(1);
            if (index >= ready.size()) break;
            bool capability = supported.load();
            const DWORD code = DeleteNativeItem(ready[index].path, ready[index].attributes, capability);
            if (!capability) supported.store(false);
            if (code == ERROR_SUCCESS) changed.store(true);
            else if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                DWORD expected = ERROR_SUCCESS;
                failure.compare_exchange_strong(expected, code);
            }
        }
    };
    struct WorkOwner {
        PTP_WORK value = nullptr;
        bool pending = false;
        ~WorkOwner() {
            if (!value) return;
            if (pending) WaitForThreadpoolWorkCallbacks(value, FALSE);
            CloseThreadpoolWork(value);
        }
    } pool_work;
    bool creation_attempted = false;
    const auto drain = [&]() {
        if (ready.empty()) return true;
        next = 0;
        failure = ERROR_SUCCESS;
        changed = false;
        supported = disposition_supported;
        const size_t count = (std::min)(ready.size(),
            static_cast<size_t>((std::min)(workers, 8u)));
        if (count > 1) {
            if (!creation_attempted) {
                creation_attempted = true;
                const auto callback = [](PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) {
                    (*static_cast<decltype(work)*>(context))();
                };
                pool_work.value = CreateThreadpoolWork(callback,
                    const_cast<void*>(static_cast<const void*>(&work)), nullptr);
            }
            if (pool_work.value) {
                pool_work.pending = true;
                for (size_t i = 1; i < count; ++i) SubmitThreadpoolWork(pool_work.value);
            }
        }
        work();
        if (pool_work.pending) {
            WaitForThreadpoolWorkCallbacks(pool_work.value, FALSE);
            pool_work.pending = false;
        }
        disposition_supported = supported.load();
        outcome.mutated = outcome.mutated || changed.load();
        outcome.cancelled = cancel.load();
        outcome.error = failure.load();
        ready.clear();
        if (changed.load()) progress(source, -1.0f);
        outcome.cancelled = cancel.load();
        return !outcome.cancelled && outcome.error == ERROR_SUCCESS;
    };
    ULONGLONG last_report = 0;
    while (!stack.empty()) {
        if (cancel.load()) { outcome.cancelled = true; return outcome; }
        auto& frame = stack.back();
        if (!frame.opened && (frame.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            // Deny rename/replacement while enumerating this directory and its children.
            frame.guard = CreateFileW(NativePath(frame.path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (frame.guard == INVALID_HANDLE_VALUE) { outcome.error = GetLastError(); return outcome; }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(frame.guard, &info)) {
                outcome.error = GetLastError();
                return outcome;
            }
            frame.attributes = info.dwFileAttributes;
        }
        const bool directory = (frame.attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                               !(frame.attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (directory) {
            if (!frame.opened) {
                frame.opened = true;
                const auto pattern = JoinChild(NativePath(frame.path), L"*");
#ifdef PULSE_NATIVE_DELETE_TEST
                if (deny_first_enumeration) {
                    deny_first_enumeration = false;
                    SetLastError(ERROR_ACCESS_DENIED);
                } else
#endif
                frame.find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &frame.data,
                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (frame.find == INVALID_HANDLE_VALUE) {
                    const DWORD code = GetLastError();
                    if (code != ERROR_FILE_NOT_FOUND) { outcome.enumeration_failed = outcome.mutated; outcome.error = code; return outcome; }
                }
            } else if (frame.advance && frame.find != INVALID_HANDLE_VALUE) {
                frame.advance = false;
                bool next_ok = false;
#ifdef PULSE_NATIVE_DELETE_TEST
                if (fail_next_enumeration) {
                    fail_next_enumeration = false;
                    SetLastError(ERROR_READ_FAULT);
                } else
#endif
                { next_ok = FindNextFileW(frame.find, &frame.data) != FALSE; }
                if (!next_ok) {
                    const DWORD code = GetLastError();
                    FindClose(frame.find);
                    frame.find = INVALID_HANDLE_VALUE;
                    if (code != ERROR_NO_MORE_FILES) { outcome.enumeration_failed = true; outcome.error = code; return outcome; }
                }
            }
            if (frame.find != INVALID_HANDLE_VALUE) {
                frame.advance = true;
                const wchar_t* name = frame.data.cFileName;
                if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
                Frame child{JoinChild(frame.path, name), frame.data.dwFileAttributes};
                if (workers > 1 && !(child.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                    ready.push_back({std::move(child.path), child.attributes});
                    if (ready.size() == 32 && !drain()) return outcome;
                    continue;
                }
                if (!drain()) return outcome;
                stack.push_back(std::move(child));
                continue;
            }
        }
        if (!drain()) return outcome;
        if (cancel.load()) { outcome.cancelled = true; return outcome; }
        if (frame.guard != INVALID_HANDLE_VALUE) {
            CloseHandle(frame.guard);
            frame.guard = INVALID_HANDLE_VALUE;
        }
        const DWORD code = DeleteNativeItem(frame.path, frame.attributes, disposition_supported);
        if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            outcome.error = code;
            return outcome;
        }
        if (code == ERROR_SUCCESS) {
            outcome.mutated = true;
#ifdef PULSE_NATIVE_DELETE_TEST
            if (after_mutation) after_mutation();
#endif
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_report >= 50) {
            last_report = now;
            progress(frame.path, -1.0f);
        }
        stack.pop_back();
    }
    outcome.done = true;
    return outcome;
}
// A folder tree on a local disk, several folders at once. Deleting is mostly
// waiting on the file system's metadata reads and log writes; a few requests
// in flight let the disk serve them in a better order. Each worker lists a
// folder through its guard handle (which keeps the folder from being renamed
// or replaced) and deletes the items in it relative to that handle, so no
// path is parsed per file. A folder goes once its listing is done and its
// subfolders are gone. `progress` is called only on the calling thread.
// Same outcome contract as DeleteNative; a source that is not a plain folder,
// or a system without the native calls, goes to DeleteNative.
template <typename Progress>
NativeDeleteOutcome DeleteNativeParallel(const std::wstring& source, std::atomic<bool>& cancel,
                                         Progress&& progress, unsigned threads = 4) {
    using OpenFile = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
    using SetInformationFile = LONG(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    using StatusToError = ULONG(NTAPI*)(LONG);
    static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    static const auto open_file = ntdll ? reinterpret_cast<OpenFile>(GetProcAddress(ntdll, "NtOpenFile")) : nullptr;
    static const auto set_information = ntdll
        ? reinterpret_cast<SetInformationFile>(GetProcAddress(ntdll, "NtSetInformationFile")) : nullptr;
    static const auto status_to_error = ntdll
        ? reinterpret_cast<StatusToError>(GetProcAddress(ntdll, "RtlNtStatusToDosError")) : nullptr;
    if (source.size() <= 3 || source.starts_with(L"::") || source.find(L'/') != std::wstring::npos)
        return {};
    const DWORD root_attributes = GetFileAttributesW(NativePath(source).c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES) return {};
    if (threads < 2 || !open_file || !set_information || !status_to_error ||
        !(root_attributes & FILE_ATTRIBUTE_DIRECTORY) || (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return DeleteNative(source, cancel, progress, 1);

    struct Folder {
        std::wstring path;
        Folder* parent = nullptr;
        DWORD attributes = FILE_ATTRIBUTE_DIRECTORY;
        std::atomic<int> pending{1};  // its own listing, plus each subfolder not yet gone
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Folder*> queue;
    std::vector<std::unique_ptr<Folder>> folders;
    size_t listing = 0;
    bool finished = false;
    std::atomic<bool> stop{false};
    std::atomic<bool> mutated{false};
    std::atomic<bool> enumeration_failed{false};
    std::atomic<bool> root_gone{false};
    std::atomic<DWORD> failure{ERROR_SUCCESS};
    std::atomic<size_t> deleted{0};
    // Learned once per root, as DeleteNative does: FAT/exFAT and older Windows
    // have no POSIX delete, and probing it again for every file costs a call each.
    std::atomic<bool> posix{true};
    const auto fail = [&](DWORD code) {
        DWORD expected = ERROR_SUCCESS;
        failure.compare_exchange_strong(expected, code);
        stop.store(true);
        changed.notify_all();
    };
    const auto mutation = [&] {
        mutated.store(true);
        ++deleted;
#ifdef PULSE_NATIVE_DELETE_TEST
        std::lock_guard lock(mutex);
        if (after_mutation) after_mutation();
#endif
    };
    // The folder's listing or one subfolder is done; the last one removes it.
    const auto release = [&](Folder* folder) {
        while (folder && --folder->pending == 0) {
            if (stop.load() || cancel.load()) return;
            bool capability = posix.load();
            const DWORD code = DeleteNativeItem(folder->path, folder->attributes, capability);
            if (!capability) posix.store(false);
            if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                fail(code);
                return;
            }
            if (code == ERROR_SUCCESS) mutation();
            if (!folder->parent) root_gone.store(true);
            folder = folder->parent;
        }
    };
    // ERROR_SUCCESS, or why the item is still there.
    const auto delete_child = [&](HANDLE directory, const Folder& folder, const wchar_t* name, USHORT bytes,
                                  DWORD attributes) -> DWORD {
        bool capability = false;
        if (!posix.load())
            return DeleteNativeItem(JoinChild(folder.path, std::wstring(name, bytes / sizeof(wchar_t)).c_str()),
                                    attributes, capability);
        UNICODE_STRING relative{bytes, bytes, const_cast<wchar_t*>(name)};
        OBJECT_ATTRIBUTES object{};
        // Exact case: the name is the listing's own, and a case-sensitive folder may hold both spellings.
        InitializeObjectAttributes(&object, &relative, 0, directory, nullptr);
        IO_STATUS_BLOCK io{};
        HANDLE item = nullptr;
        constexpr ULONG kOpenReparsePoint = 0x00200000, kOpenForBackupIntent = 0x00004000, kNonDirectory = 0x00000040;
        const bool link = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        LONG status = open_file(&item, DELETE | SYNCHRONIZE, &object, &io,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            kOpenReparsePoint | kOpenForBackupIntent | (link ? 0 : kNonDirectory));
        if (status < 0) return status_to_error(status);
        struct { DWORD flags; } info{kDispositionDelete | kDispositionPosixSemantics | kDispositionIgnoreReadonly};
        constexpr ULONG kFileDispositionInformationEx = 64;
#ifdef PULSE_NATIVE_DELETE_TEST
        ++disposition_probes;
        if (force_disposition_unsupported) status = static_cast<LONG>(0xC000000DL);  // STATUS_INVALID_PARAMETER
        else
#endif
        status = set_information(item, &io, &info, sizeof(info), kFileDispositionInformationEx);
        CloseHandle(item);
        if (status >= 0) return ERROR_SUCCESS;
        const DWORD code = status_to_error(status);
        // POSIX delete not understood (older Windows, some file systems): the
        // classic delete, read-only cleared and restored as DeleteNative does.
        if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_FUNCTION)
            return code;
        posix.store(false);
        return DeleteNativeItem(JoinChild(folder.path, std::wstring(name, bytes / sizeof(wchar_t)).c_str()),
                                attributes, capability);
    };
    const auto list_and_delete = [&](Folder& folder, std::vector<Folder*>& found) {
        HANDLE directory = CreateFileW(NativePath(folder.path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return;
            enumeration_failed.store(mutated.load());
            fail(code);
            return;
        }
        struct Close { HANDLE h; ~Close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } close{directory};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(directory, &info)) { fail(GetLastError()); return; }
        // Turned into a link since it was listed: the link goes, never what it points to.
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(directory);
            close.h = INVALID_HANDLE_VALUE;
            bool capability = true;
            const DWORD code = DeleteNativeItem(folder.path, info.dwFileAttributes, capability);
            if (code == ERROR_SUCCESS) mutation();
            else if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) fail(code);
            if (!folder.parent) root_gone.store(code == ERROR_SUCCESS);
            folder.pending.store(-1);  // never removed again as a folder
            return;
        }
        alignas(8) unsigned char buffer[16 * 1024];
        FILE_INFO_BY_HANDLE_CLASS page = FileIdBothDirectoryRestartInfo;
        for (bool first = true;; first = false) {
            if (stop.load() || cancel.load()) return;
            BOOL listed = FALSE;
#ifdef PULSE_NATIVE_DELETE_TEST
            if (first && deny_first_enumeration) {
                deny_first_enumeration = false;
                SetLastError(ERROR_ACCESS_DENIED);
            } else if (!first && fail_next_enumeration) {
                fail_next_enumeration = false;
                SetLastError(ERROR_READ_FAULT);
            } else
#endif
            listed = GetFileInformationByHandleEx(directory, page, buffer, sizeof(buffer));
            (void)first;
            if (!listed) {
                const DWORD code = GetLastError();
                if (code == ERROR_NO_MORE_FILES || code == ERROR_FILE_NOT_FOUND) return;
                enumeration_failed.store(true);
                fail(code);
                return;
            }
            page = FileIdBothDirectoryInfo;
            for (auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer);;
                 entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                     reinterpret_cast<const unsigned char*>(entry) + entry->NextEntryOffset)) {
                const USHORT bytes = static_cast<USHORT>(entry->FileNameLength);
                const wchar_t* name = entry->FileName;
                const bool dots = (bytes == 2 && name[0] == L'.') || (bytes == 4 && name[0] == L'.' && name[1] == L'.');
                if (!dots) {
                    if (stop.load() || cancel.load()) return;
                    const DWORD attributes = entry->FileAttributes;
                    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        auto child = std::make_unique<Folder>();
                        child->path = JoinChild(folder.path, std::wstring(name, bytes / sizeof(wchar_t)).c_str());
                        child->parent = &folder;
                        child->attributes = attributes;
                        ++folder.pending;
                        found.push_back(child.get());
                        std::lock_guard lock(mutex);
                        folders.push_back(std::move(child));
                    } else {
                        const DWORD code = delete_child(directory, folder, name, bytes, attributes);
                        if (code == ERROR_SUCCESS) mutation();
                        else if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) { fail(code); return; }
                    }
                }
                if (!entry->NextEntryOffset) break;
            }
        }
    };
    const auto worker = [&] {
        for (;;) {
            Folder* folder = nullptr;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return finished || stop.load() || cancel.load() || !queue.empty(); });
                if (finished || stop.load() || cancel.load()) return;
                folder = queue.front();
                queue.pop_front();
                ++listing;
            }
            std::vector<Folder*> found;
            list_and_delete(*folder, found);
            {
                std::lock_guard lock(mutex);
                // Depth first: few folders are open or pending at once.
                for (auto it = found.rbegin(); it != found.rend(); ++it) queue.push_front(*it);
                --listing;
                if (queue.empty() && listing == 0) finished = true;
            }
            changed.notify_all();
            release(folder->pending.load() >= 0 ? folder : folder->parent);
        }
    };
    {
        auto root = std::make_unique<Folder>();
        root->path = source;
        root->attributes = root_attributes;
        queue.push_back(root.get());
        folders.push_back(std::move(root));
    }
    std::vector<std::thread> pool;
    try {
        for (unsigned i = 0; i < (std::min)(threads, 8u); ++i) pool.emplace_back(worker);
    } catch (...) {
        if (pool.empty()) return DeleteNative(source, cancel, progress, 1);
    }
    const auto join = [&] {
        stop.store(true);
        changed.notify_all();
        for (auto& thread : pool) if (thread.joinable()) thread.join();
    };
    size_t reported = 0;
    try {
        for (;;) {
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(50),
                                 [&] { return finished || stop.load() || cancel.load(); });
                if (finished || stop.load() || cancel.load()) break;
            }
            if (const size_t now = deleted.load(); now != reported) {
                reported = now;
                progress(source, -1.0f);
            }
        }
    } catch (...) {
        join();  // workers stop where they are; nothing more is deleted
        throw;
    }
    changed.notify_all();
    for (auto& thread : pool) thread.join();
    NativeDeleteOutcome outcome;
    outcome.mutated = mutated.load();
    outcome.error = failure.load();
    outcome.enumeration_failed = enumeration_failed.load();
    outcome.cancelled = cancel.load() && !root_gone.load();
    outcome.done = root_gone.load() && outcome.error == ERROR_SUCCESS;
    if (outcome.mutated) progress(source, -1.0f);
    return outcome;
}
}
