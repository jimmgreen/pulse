#include "picker_archive_session.h"
#include "../../extensions/7zip/worker_client.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

namespace pulse::ui {
namespace {
namespace disk = std::filesystem;
bool Cancelled(const std::function<bool()>& callback) { return callback && callback(); }
DWORD RuntimeError(const std::wstring& executable) {
    if (executable.empty()) return ERROR_MOD_NOT_FOUND;
    if (!disk::path(executable).is_absolute()) return ERROR_INVALID_NAME;
    const DWORD attributes = GetFileAttributesW(executable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return attributes & FILE_ATTRIBUTE_DIRECTORY ? ERROR_BAD_EXE_FORMAT : ERROR_SUCCESS;
}
struct Less {
    bool operator()(std::wstring_view a, std::wstring_view b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && !Less{}(a, b) && !Less{}(b, a);
}
std::wstring Normalize(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}
DWORD Error(engine::Status status) {
    switch (status) {
    case engine::Status::Ok: return ERROR_SUCCESS;
    case engine::Status::Cancelled: return ERROR_CANCELLED;
    case engine::Status::NeedPassword: case engine::Status::WrongPassword: return ERROR_PASSWORD_RESTRICTION;
    case engine::Status::Unsupported: return ERROR_NOT_SUPPORTED;
    case engine::Status::LimitReached: return ERROR_FILE_TOO_LARGE;
    case engine::Status::InvalidInput: return ERROR_INVALID_NAME;
    case engine::Status::WorkerTimedOut: return ERROR_TIMEOUT;
    case engine::Status::WorkerMemoryLimit: return ERROR_NOT_ENOUGH_MEMORY;
    case engine::Status::InvalidArchive: return ERROR_INVALID_DATA;
    default: return ERROR_READ_FAULT;
    }
}
struct FileLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info{};
    ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    DWORD Open(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        if (!GetFileInformationByHandle(handle, &info)) return GetLastError();
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
        return ERROR_SUCCESS;
    }
};
bool Same(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
struct Item { engine::Entry entry; bool explicit_entry = false; };
struct Snapshot {
    BY_HANDLE_FILE_INFORMATION identity{};
    std::map<std::wstring, Item, Less> items;
};
DWORD BuildSnapshot(engine::Listing listing, Snapshot& snapshot) {
    if (listing.status != engine::Status::Ok) return Error(listing.status);
    if (listing.truncated) return ERROR_FILE_TOO_LARGE;
    const auto budget = engine::PreviewBudget();
    size_t path_bytes = 0;
    auto within_budget = [&](const std::wstring& name, bool inserted) {
        if (inserted) path_bytes += name.size() * sizeof(wchar_t);
        return snapshot.items.size() <= budget.max_entries && path_bytes <= budget.max_path_bytes;
    };
    for (auto& entry : listing.entries) {
        std::replace(entry.path.begin(), entry.path.end(), L'/', L'\\');
        if (entry.dir && !entry.path.empty() && entry.path.back() == L'\\') entry.path.pop_back();
        if (!engine::SafeRelativePath(entry.path) || entry.link ||
            (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_INVALID_DATA;
        auto [it, inserted] = snapshot.items.try_emplace(entry.path, Item{entry, true});
        if (!within_budget(entry.path, inserted)) return ERROR_FILE_TOO_LARGE;
        if (!inserted) {
            if (it->first != entry.path || it->second.explicit_entry || !entry.dir || !it->second.entry.dir)
                return ERROR_INVALID_DATA;
            it->second = {entry, true};
        }
        size_t slash = entry.path.find(L'\\');
        while (slash != std::wstring::npos) {
            const auto parent = entry.path.substr(0, slash);
            engine::Entry directory;
            directory.path = parent;
            directory.dir = true;
            directory.attributes = FILE_ATTRIBUTE_DIRECTORY;
            auto [ancestor, added] = snapshot.items.try_emplace(parent, Item{directory, false});
            if (!within_budget(parent, added)) return ERROR_FILE_TOO_LARGE;
            if (!added && (ancestor->first != parent || !ancestor->second.entry.dir)) return ERROR_INVALID_DATA;
            slash = entry.path.find(L'\\', slash + 1);
        }
    }
    return ERROR_SUCCESS;
}
void RemoveOwned(const std::wstring& root) noexcept {
    try {
        std::error_code error;
        for (disk::recursive_directory_iterator it(root, disk::directory_options::none, error), end;
             !error && it != end; it.increment(error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                it.disable_recursion_pending();
                continue;
            }
            // Extracted metadata may include read-only; reset only owned regular entries.
            if (attributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(it->path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        }
        disk::remove_all(root, error);
    } catch (...) {}
}
struct CleanupContext { std::wstring root; HMODULE module = nullptr; };
void CALLBACK CleanupWork(PTP_CALLBACK_INSTANCE instance, void* context) {
    std::unique_ptr<CleanupContext> cleanup(static_cast<CleanupContext*>(context));
    if (cleanup->module) FreeLibraryWhenCallbackReturns(instance, cleanup->module);
    RemoveOwned(cleanup->root);
}
DWORD MakeStaging(std::wstring root, std::wstring& created) {
    if (root.empty()) {
        PWSTR local = nullptr;
        const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local);
        if (FAILED(hr)) return ERROR_PATH_NOT_FOUND;
        root = std::wstring(local) + L"\\Pulse\\PickerExtracted";
        CoTaskMemFree(local);
    }
    const disk::path base(root);
    if (!base.is_absolute()) return ERROR_INVALID_NAME;
    disk::path walk;
    for (const auto& part : base) {
        walk /= part;
        if (walk == base.root_name()) continue;
        const DWORD attributes = GetFileAttributesW(walk.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
        } else if (!CreateDirectoryW(walk.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return GetLastError();
        }
    }
    static std::atomic_uint64_t sequence{0};
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto leaf = L"delivery-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
        const auto path = (base / leaf).wstring();
        if (CreateDirectoryW(path.c_str(), nullptr)) { created = path; return ERROR_SUCCESS; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
    }
    return ERROR_ALREADY_EXISTS;
}
} // namespace

struct PickerArchiveDelivery::State {
    std::mutex mutex;
    std::wstring root;
};
PickerArchiveDelivery::PickerArchiveDelivery(std::wstring root) : state_(std::make_unique<State>()) {
    state_->root = std::move(root);
}
PickerArchiveDelivery::~PickerArchiveDelivery() {
    if (state_->root.empty()) return;
    auto* cleanup = new (std::nothrow) CleanupContext{std::move(state_->root), nullptr};
    if (!cleanup) return;
    // A picker DLL may be unloaded as soon as its window closes. Hold its code
    // until the pool callback has returned, without blocking the closing UI.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&CleanupWork), &cleanup->module)) {
        delete cleanup;
        return;
    }
    if (!TrySubmitThreadpoolCallback(CleanupWork, cleanup, nullptr)) {
        FreeLibrary(cleanup->module);
        delete cleanup;
    }
}
void PickerArchiveDelivery::Commit() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->root.clear();
}
void PickerArchiveDelivery::Cleanup() noexcept {
    std::wstring root;
    { std::lock_guard lock(state_->mutex); root.swap(state_->root); }
    if (!root.empty()) RemoveOwned(root);
}

struct PickerArchiveSession::Impl {
    std::function<std::wstring()> runtime;
    std::wstring staging;
    mutable std::mutex mutex;
    std::map<std::wstring, std::shared_ptr<const Snapshot>, Less> archives;
    DWORD Get(const std::wstring& archive, FileLock& file, std::shared_ptr<const Snapshot>& snapshot,
              const std::function<bool()>& cancelled) {
        if (Cancelled(cancelled)) return ERROR_CANCELLED;
        DWORD error = file.Open(archive);
        if (error) return error;
        {
            std::lock_guard lock(mutex);
            const auto found = archives.find(archive);
            if (found == archives.end()) return ERROR_PATH_NOT_FOUND;
            if (found->second && Same(found->second->identity, file.info)) {
                snapshot = found->second;
                return ERROR_SUCCESS;
            }
        }
        const auto executable = runtime ? runtime() : std::wstring{};
        error = RuntimeError(executable);
        if (error) return error;
        worker_client::ScopedWorkerExecutable worker(executable);
        auto next = std::make_shared<Snapshot>();
        next->identity = file.info;
        error = BuildSnapshot(engine::List(archive, cancelled, {}, engine::PreviewBudget().max_entries), *next);
        if (error) return error;
        if (Cancelled(cancelled)) return ERROR_CANCELLED;
        { std::lock_guard lock(mutex); archives[archive] = next; }
        snapshot = std::move(next);
        return ERROR_SUCCESS;
    }
};
PickerArchiveSession::PickerArchiveSession(std::function<std::wstring()> runtime_provider, std::wstring staging_root)
    : impl_(std::make_unique<Impl>()) {
    impl_->runtime = std::move(runtime_provider);
    impl_->staging = std::move(staging_root);
}
PickerArchiveSession::~PickerArchiveSession() = default;
void PickerArchiveSession::Register(std::wstring archive_path) {
    archive_path = Normalize(std::move(archive_path));
    if (archive_path.empty()) return;
    std::lock_guard lock(impl_->mutex);
    impl_->archives.try_emplace(std::move(archive_path), nullptr);
}
std::wstring PickerArchiveSession::ArchiveFor(std::wstring_view path) const {
    const auto normalized = Normalize(std::wstring(path));
    std::wstring result;
    std::lock_guard lock(impl_->mutex);
    for (const auto& [archive, unused] : impl_->archives) {
        if (archive.size() > result.size() && normalized.size() >= archive.size() &&
            Equal(std::wstring_view(normalized).substr(0, archive.size()), archive) &&
            (normalized.size() == archive.size() || normalized[archive.size()] == L'\\')) result = archive;
    }
    return result;
}
bool PickerArchiveSession::Contains(std::wstring_view path) const { return !ArchiveFor(path).empty(); }
DWORD PickerArchiveSession::List(const std::wstring& path, std::vector<fs::DirEntry>& rows,
                                 const std::function<bool()>& cancelled) {
    rows.clear();
    const auto normalized = Normalize(path);
    const auto archive = ArchiveFor(normalized);
    if (archive.empty()) return ERROR_PATH_NOT_FOUND;
    FileLock file;
    std::shared_ptr<const Snapshot> snapshot;
    DWORD error = impl_->Get(archive, file, snapshot, cancelled);
    if (error) return error;
    const auto relative = normalized.size() == archive.size() ? std::wstring{} : normalized.substr(archive.size() + 1);
    if (!relative.empty()) {
        const auto parent = snapshot->items.find(relative);
        if (parent == snapshot->items.end() || !parent->second.entry.dir) return ERROR_PATH_NOT_FOUND;
    }
    std::vector<fs::DirEntry> result;
    const auto prefix = relative.empty() ? std::wstring{} : relative + L"\\";
    for (const auto& [name, item] : snapshot->items) {
        if (Cancelled(cancelled)) return ERROR_CANCELLED;
        if (name.size() <= prefix.size() || !Equal(std::wstring_view(name).substr(0, prefix.size()), prefix)) continue;
        const auto leaf = name.substr(prefix.size());
        if (leaf.find(L'\\') != std::wstring::npos) continue;
        fs::DirEntry row;
        row.name = leaf;
        row.size = item.entry.size;
        row.is_dir = item.entry.dir;
        row.attrs = item.entry.attributes;
        if (row.is_dir) row.attrs |= FILE_ATTRIBUTE_DIRECTORY;
        row.mtime.dwLowDateTime = static_cast<DWORD>(item.entry.modified);
        row.mtime.dwHighDateTime = static_cast<DWORD>(item.entry.modified >> 32);
        result.push_back(std::move(row));
    }
    rows = std::move(result);
    return ERROR_SUCCESS;
}
PickerArchiveMaterialization PickerArchiveSession::Materialize(const std::vector<std::wstring>& virtual_paths,
                                                               const std::function<bool()>& cancelled) {
    PickerArchiveMaterialization result;
    auto fail = [&](DWORD error) {
        result.error = error;
        result.paths.clear();
        if (result.lease) result.lease->Cleanup();
        result.lease.reset();
        return result;
    };
    if (Cancelled(cancelled)) return fail(ERROR_CANCELLED);
    if (virtual_paths.empty()) return fail(ERROR_INVALID_PARAMETER);
    const auto budget = engine::DefaultExtractBudget();
    if (virtual_paths.size() > budget.max_entries) return fail(ERROR_FILE_TOO_LARGE);
    struct Group {
        std::unique_ptr<FileLock> file;
        std::shared_ptr<const Snapshot> snapshot;
        std::vector<std::wstring> names;
        std::wstring destination;
    };
    std::map<std::wstring, Group, Less> groups;
    std::vector<std::pair<std::wstring, std::wstring>> order;
    std::set<std::wstring, Less> unique;
    uint64_t total = 0;
    for (const auto& path : virtual_paths) {
        if (Cancelled(cancelled)) return fail(ERROR_CANCELLED);
        const auto normalized = Normalize(path);
        const auto archive = ArchiveFor(normalized);
        if (archive.empty() || normalized.size() <= archive.size() || !unique.insert(normalized).second)
            return fail(ERROR_INVALID_NAME);
        const auto relative = normalized.substr(archive.size() + 1);
        auto& group = groups[archive];
        if (!group.file) {
            group.file = std::make_unique<FileLock>();
            const DWORD error = impl_->Get(archive, *group.file, group.snapshot, cancelled);
            if (error) return fail(error);
        }
        const auto entry = group.snapshot->items.find(relative);
        if (entry == group.snapshot->items.end()) return fail(ERROR_FILE_NOT_FOUND);
        if (entry->second.entry.dir) return fail(ERROR_DIRECTORY);
        if (entry->second.entry.size > budget.max_single_bytes || entry->second.entry.size > budget.max_total_bytes - total)
            return fail(ERROR_FILE_TOO_LARGE);
        total += entry->second.entry.size;
        group.names.push_back(entry->first);
        order.emplace_back(archive, entry->first);
    }
    std::wstring root;
    DWORD error = MakeStaging(impl_->staging, root);
    if (error) return fail(error);
    result.lease = std::shared_ptr<PickerArchiveDelivery>(new PickerArchiveDelivery(root));
    const auto executable = impl_->runtime ? impl_->runtime() : std::wstring{};
    error = RuntimeError(executable);
    if (error) return fail(error);
    worker_client::ScopedWorkerExecutable worker(executable);
    size_t index = 0;
    for (auto& [archive, group] : groups) {
        if (Cancelled(cancelled)) return fail(ERROR_CANCELLED);
        group.destination = root + L"\\" + std::to_wstring(index++);
        if (!CreateDirectoryW(group.destination.c_str(), nullptr)) return fail(GetLastError());
        auto extracted = engine::ExtractSelected(archive, group.names, L"", group.destination, cancelled, {}, {}, budget);
        if (extracted.status != engine::Status::Ok) return fail(Error(extracted.status));
        if (extracted.files != group.names.size() || extracted.skipped_links) return fail(ERROR_INVALID_DATA);
    }
    for (const auto& [archive, relative] : order) {
        if (Cancelled(cancelled)) return fail(ERROR_CANCELLED);
        const auto& group = groups.at(archive);
        const auto output = group.destination + L"\\" + relative;
        {
            FileLock file;
            error = file.Open(output);
            if (!error) {
                const auto size = (static_cast<uint64_t>(file.info.nFileSizeHigh) << 32) | file.info.nFileSizeLow;
                if (size != group.snapshot->items.at(relative).entry.size) error = ERROR_INVALID_DATA;
            }
        }
        if (error) return fail(error);
        result.paths.push_back(output);
    }
    return result;
}
} // namespace pulse::ui
