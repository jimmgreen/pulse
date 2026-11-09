#include "audit_persistence.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::extensions {
namespace {
class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }
  private:
    HANDLE value_;
};
bool SafeFile(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
           info.nNumberOfLinks == 1;
}
bool Identifier(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}
bool FileOperation(std::string_view value) {
    constexpr std::string_view allowed[] = {
        "files.deleteCurrentSelection", "files.transfer", "files.rename.commit"};
    return std::find(std::begin(allowed), std::end(allowed), value) != std::end(allowed);
}
// Existing log segments remain readable after narrowing new audit admission.
bool Operation(std::string_view value) {
    constexpr std::string_view legacy[] = {
        "navigation.back", "navigation.forward", "navigation.up", "view.refresh", "view.setMode",
        "workspace.setLayout", "files.copySelectionToOtherPane", "files.moveSelectionToOtherPane",
        "workflow.run", "workflow.cancel", "selection.selectAll", "selection.invert", "selection.clear",
        "view.setSort", "tabs.new", "tabs.duplicate", "tabs.closeActive", "tabs.activate", "panes.focus",
        "context.get", "files.list", "files.stat", "operations.get", "operations.cancel", "settings.get",
        "settings.set", "audit.list", "package.trust", "runtime.invoke", "files.prepareRename", "files.commitRename"};
    return FileOperation(value) || std::find(std::begin(legacy), std::end(legacy), value) != std::end(legacy);
}
bool Outcome(std::string_view value) {
    constexpr std::string_view allowed[] = {
        "accepted", "submitted", "ok", "pending", "cancelled", "dispatched", "rejected", "pendingConfirmation", "invalid_request",
        "invalid_caller", "wrong_owner", "not_authorized", "unavailable", "timeout", "stale_reference",
        "reference_budget", "not_found", "invalid_test_profile", "internal_error", "unsafe_path", "io_error",
        "corrupt_data", "version_conflict", "settings_limit", "invalid_argument", "busy", "not_supported",
        "permission_denied", "request_budget", "limit_exceeded", "version_exhausted", "revoked",
        "unsupported_context", "invalid_name", "plan_budget", "stale_plan", "submit_failed", "conflict", "source_changed", "task_budget", "directory_unavailable", "rename_conflict", "no_changes"};
    return std::find(std::begin(allowed), std::end(allowed), value) != std::end(allowed);
}
uint64_t UtcMs() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return (value.QuadPart - 116444736000000000ull) / 10000;
}
bool Digits(std::string_view value) {
    return !value.empty() && value.size() <= 16 &&
           std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool ValidEvent(const json::Value& event) {
    if (!event.is_object() || event.size() != 5) return false;
    for (const auto* key : {"packageId", "operation", "outcome", "utcMs", "sessionId"}) {
        const auto* value = event.find(key);
        if (!value || !value->is_string()) return false;
    }
    const auto session = event.get_string("sessionId");
    return Identifier(event.get_string("packageId")) && Operation(event.get_string("operation")) &&
           Outcome(event.get_string("outcome")) && Digits(event.get_string("utcMs")) && session.size() == 32 &&
           std::all_of(session.begin(), session.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::wstring Extended(const std::wstring& path) { return L"\\\\?\\" + path; }
struct Directory {
    std::wstring path;
    std::vector<Handle> pins;
    std::string error = "io_error";
    bool Open(const std::wstring& input, bool create) {
        if (input.size() < 3 || input[1] != L':' || (input[2] != L'\\' && input[2] != L'/') ||
            input.find(L'\0') != std::wstring::npos || input.find(L':', 2) != std::wstring::npos) {
            error = "invalid_root"; return false;
        }
        const std::filesystem::path original(input);
        for (const auto& part : original) {
            if (part == L".." || part == L".") { error = "unsafe_path"; return false; }
        }
        path = original.lexically_normal().wstring();
        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        if (GetDriveTypeW(path.substr(0, 3).c_str()) != DRIVE_FIXED) {
            error = "local_fixed_drive_required"; return false;
        }
        size_t end = 3;
        for (;;) {
            const auto prefix = path.substr(0, end);
            if (create && end > 3 && !CreateDirectoryW(Extended(prefix).c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) return false;
            Handle pin(CreateFileW(Extended(prefix).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!pin) return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(pin.Get(), &info)) return false;
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                error = "unsafe_path"; return false;
            }
            pins.push_back(std::move(pin));
            if (end == path.size()) return true;
            const auto next = path.find(L'\\', end + (path[end] == L'\\' ? 1 : 0));
            end = next == std::wstring::npos ? path.size() : next;
        }
    }
    std::wstring File(std::wstring_view name) const { return Extended(path + L"\\" + std::wstring(name)); }
    std::wstring FinalPath() const {
        if (pins.empty()) return {};
        const DWORD needed = GetFinalPathNameByHandleW(pins.back().Get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!needed) return {};
        std::wstring result(needed, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(pins.back().Get(), result.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= needed) return {};
        result.resize(length);
        while (!result.empty() && result.back() == L'\\') result.pop_back();
        return result;
    }
};
class Lock {
  public:
    explicit Lock(const Directory& directory, bool create = true) : file_(CreateFileW(directory.File(L"audit.lock").c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        if (!file_) return;
        LARGE_INTEGER size{};
        if (!SafeFile(file_.Get()) || !GetFileSizeEx(file_.Get(), &size) || size.QuadPart != 0) {
            error = "unsafe_lock"; return;
        }
        acquired_ = LockFileEx(file_.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                               0, 1, 0, &position_) != FALSE;
        if (!acquired_) error = "store_busy";
    }
    ~Lock() { if (acquired_) UnlockFileEx(file_.Get(), 0, 1, 0, &position_); }
    explicit operator bool() const { return acquired_; }
    std::string error = "io_error";
  private:
    Handle file_;
    OVERLAPPED position_{};
    bool acquired_ = false;
};
bool ReadFileBytes(const std::wstring& path, size_t limit, std::string& bytes, uint64_t& modified,
                   std::string& error) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) { bytes.clear(); modified = 0; return true; }
        error = "io_error"; return false;
    }
    LARGE_INTEGER size{};
    if (!SafeFile(file.Get())) { error = "unsafe_path"; return false; }
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > limit) {
        error = "disk_budget_exceeded"; return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        error = "io_error"; return false;
    }
    FILETIME time{};
    if (!GetFileTime(file.Get(), nullptr, nullptr, &time)) { error = "io_error"; return false; }
    modified = (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    return true;
}
bool ParseLines(const std::string& bytes, json::Value& result) {
    size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        if (end == std::string::npos || end - begin > 1024) return false;
        json::Value event;
        if (!json::Parse(std::string_view(bytes).substr(begin, end - begin), event, nullptr, {1024, 4}) || !ValidEvent(event)) return false;
        result.push(std::move(event));
        begin = end + 1;
    }
    return true;
}
std::wstring Segment(size_t index) { return L"audit-" + std::to_wstring(index) + L".jsonl"; }
bool Inventory(const Directory& directory, std::string& error) {
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW(directory.File(L"*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) { error = "io_error"; return false; }
    bool valid = true;
    do {
        const std::wstring name(item.cFileName);
        if (name == L"." || name == L"..") continue;
        bool known = name == L"audit.lock" || name == L"audit.pending" || name == L"audit-status.json";
        for (size_t i = 0; i < kAuditSegments; ++i) known |= name == Segment(i);
        if (!known || (item.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            valid = false; error = "unexpected_or_unsafe_entry"; break;
        }
    } while (FindNextFileW(search, &item));
    if (valid && GetLastError() != ERROR_NO_MORE_FILES) { valid = false; error = "io_error"; }
    FindClose(search);
    return valid;
}
bool Commit(const Directory& directory, const std::wstring& target, const std::string& bytes, std::string& error) {
    Handle file(CreateFileW(directory.File(L"audit.pending").c_str(), GENERIC_WRITE | DELETE,
                            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!file) { error = GetLastError() == ERROR_FILE_EXISTS ? "interrupted_commit" : "io_error"; return false; }
    bool committed = false;
    DWORD written = 0;
    if (SafeFile(file.Get()) && WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file.Get())) {
        const auto path = directory.File(target);
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + path.size() * sizeof(wchar_t), 0);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = TRUE;
        rename->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        std::copy(path.begin(), path.end(), rename->FileName);
        committed = SetFileInformationByHandle(file.Get(), FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) != FALSE;
    }
    if (!committed) {
        FILE_DISPOSITION_INFO remove{TRUE};
        SetFileInformationByHandle(file.Get(), FileDispositionInfo, &remove, sizeof(remove));
        error = "io_error";
    }
    return committed;
}
json::Value StatusJson(const AuditPersistenceStatus& status) {
    auto value = json::Value::Object();
    value.set("accepted", std::to_string(status.accepted)); value.set("persisted", std::to_string(status.persisted));
    value.set("dropped", std::to_string(status.dropped)); value.set("failures", std::to_string(status.failures));
    value.set("lastError", status.last_error);
    return value;
}
bool ValidStatus(const std::string& bytes, json::Value& output) {
    output = json::Value::Object();
    if (bytes.empty()) return true;
    auto value = json::Value::Object();
    if (!json::Parse(bytes, value, nullptr, {4096, 3}) || !value.is_object() || value.size() != 5) return false;
    for (const auto* key : {"accepted", "persisted", "dropped", "failures"})
        if (!Digits(value.get_string(key))) return false;
    const auto error = value.get_string("lastError");
    constexpr std::string_view known[] = {"", "stopped", "queue_full", "invalid_event", "invalid_root", "unsafe_path",
        "local_fixed_drive_required", "io_error", "unsafe_lock", "store_busy", "disk_budget_exceeded",
        "unexpected_or_unsafe_entry", "interrupted_commit", "corrupt_data", "corrupt_status", "worker_exception",
        "session_id_unavailable"};
    if (!value.find("lastError") || !value.find("lastError")->is_string() ||
        std::find(std::begin(known), std::end(known), error) == std::end(known)) return false;
    output = std::move(value);
    return true;
}
bool Load(const Directory& directory, std::array<std::string, kAuditSegments>& bytes,
          std::array<uint64_t, kAuditSegments>& times, json::Value& records, json::Value& status,
          std::string& error) {
    if (!Inventory(directory, error)) return false;
    // A crash leftover is evidence, never silently deleted or replayed.
    const DWORD pending = GetFileAttributesW(directory.File(L"audit.pending").c_str());
    if (pending != INVALID_FILE_ATTRIBUTES) { error = "interrupted_commit"; return false; }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) { error = "io_error"; return false; }
    for (size_t i = 0; i < kAuditSegments; ++i) {
        if (!ReadFileBytes(directory.File(Segment(i)), kAuditSegmentBytes, bytes[i], times[i], error)) return false;
        if (!ParseLines(bytes[i], records)) { error = "corrupt_data"; return false; }
    }
    std::string summary;
    uint64_t ignored = 0;
    if (!ReadFileBytes(directory.File(L"audit-status.json"), 4096, summary, ignored, error)) return false;
    if (!ValidStatus(summary, status)) { error = "corrupt_status"; return false; }
    return true;
}
json::Value Reply(json::Value records, json::Value status, const std::string& error) {
    auto value = json::Value::Object();
    value.set("ok", error.empty()); value.set("partial", !error.empty() && records.size() != 0);
    value.set("records", std::move(records)); value.set("lastWriterStatus", std::move(status));
    value.set("error", error); value.set("grantsAuthority", false);
    auto budget = json::Value::Object();
    budget.set("segmentBytes", static_cast<uint64_t>(kAuditSegmentBytes));
    budget.set("segments", static_cast<uint64_t>(kAuditSegments));
    budget.set("maximumDiskBytes", static_cast<uint64_t>(kAuditMaximumDiskBytes));
    budget.set("queueRecords", static_cast<uint64_t>(kAuditQueueRecords));
    value.set("retention", std::move(budget));
    return value;
}
std::unique_ptr<AuditPersistence> host_audit;
AuditPersistenceStatus unavailable_status;
}

struct AuditPersistence::Impl {
    explicit Impl(std::wstring directory) : root(std::move(directory)) {
        std::array<unsigned char, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            status.last_error = "session_id_unavailable"; status.failures = 1; stopping = true; return;
        }
        for (auto byte : random) { session += "0123456789abcdef"[byte >> 4]; session += "0123456789abcdef"[byte & 15]; }
        worker = std::thread([this] { Run(); });
    }
    void Run() {
        for (;;) {
            std::deque<std::string> batch;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                batch.swap(queue);
                if (batch.empty() && stopping) break;
            }
            std::string error;
            uint64_t committed = 0;
            try {
                Directory directory;
                if (!directory.Open(root, true)) error = directory.error;
                else {
                    Lock file_lock(directory);
                    if (!file_lock) error = file_lock.error;
                    else {
                        std::array<std::string, kAuditSegments> bytes;
                        std::array<uint64_t, kAuditSegments> times{};
                        auto records = json::Value::Array(), old_status = json::Value::Object();
                        if (Load(directory, bytes, times, records, old_status, error)) {
                            size_t slot = static_cast<size_t>(std::max_element(times.begin(), times.end()) - times.begin());
                            uint64_t staged = 0;
                            for (const auto& line : batch) {
                                if (bytes[slot].size() + line.size() > kAuditSegmentBytes) {
                                    if (staged) {
                                        if (!Commit(directory, Segment(slot), bytes[slot], error)) break;
                                        committed += staged; staged = 0;
                                    }
                                    slot = (slot + 1) % kAuditSegments; bytes[slot].clear();
                                }
                                bytes[slot] += line;
                                ++staged;
                            }
                            if (error.empty() && staged && Commit(directory, Segment(slot), bytes[slot], error)) committed += staged;
                            AuditPersistenceStatus snapshot;
                            { std::lock_guard lock(mutex); snapshot = status; }
                            snapshot.persisted += committed;
                            snapshot.dropped += batch.size() - committed;
                            if (!error.empty()) { ++snapshot.failures; snapshot.last_error = error; }
                            if (error.empty()) Commit(directory, L"audit-status.json", json::Serialize(StatusJson(snapshot)), error);
                        }
                    }
                }
            } catch (...) { error = "worker_exception"; }
            {
                std::lock_guard lock(mutex);
                status.persisted += committed; status.dropped += batch.size() - committed;
                if (!error.empty()) { ++status.failures; status.last_error = error; }
            }
            if (!error.empty()) {
                const std::string message = "Pulse extension audit: " + error + "\n";
                OutputDebugStringA(message.c_str());
            }
        }
        // Capture admission drops after the last batch as well. This is worker-only;
        // corrupt/locked storage is never repaired or waited upon during shutdown.
        std::string error;
        try {
            Directory directory;
            if (!directory.Open(root, true)) error = directory.error;
            else {
                Lock lock(directory);
                if (!lock) error = lock.error;
                else {
                    std::array<std::string, kAuditSegments> bytes;
                    std::array<uint64_t, kAuditSegments> times{};
                    auto records = json::Value::Array(), old = json::Value::Object();
                    if (Load(directory, bytes, times, records, old, error)) {
                        AuditPersistenceStatus snapshot;
                        { std::lock_guard guard(mutex); snapshot = status; }
                        Commit(directory, L"audit-status.json", json::Serialize(StatusJson(snapshot)), error);
                    }
                }
            }
        } catch (...) { error = "worker_exception"; }
        if (!error.empty()) {
            std::lock_guard lock(mutex);
            ++status.failures; status.last_error = error;
            const std::string message = "Pulse extension audit shutdown: " + error + "\n";
            OutputDebugStringA(message.c_str());
        }
    }
    std::wstring root;
    std::string session;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::thread worker;
    AuditPersistenceStatus status;
    bool stopping = false;
};
AuditPersistence::AuditPersistence(std::wstring root) : impl_(std::make_unique<Impl>(std::move(root))) {}
AuditPersistence::~AuditPersistence() { Stop(); }
bool AuditPersistence::Enqueue(std::string_view package, std::string_view operation, std::string_view outcome) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->queue.size() >= kAuditQueueRecords || !Identifier(package) || !FileOperation(operation) || !Outcome(outcome)) {
        ++impl_->status.dropped;
        impl_->status.last_error = impl_->stopping ? "stopped" : impl_->queue.size() >= kAuditQueueRecords ? "queue_full" : "invalid_event";
        return false;
    }
    auto event = json::Value::Object();
    event.set("packageId", package); event.set("operation", operation); event.set("outcome", outcome);
    event.set("utcMs", std::to_string(UtcMs())); event.set("sessionId", impl_->session);
    impl_->queue.push_back(json::Serialize(event) + "\n"); ++impl_->status.accepted;
    impl_->wake.notify_one(); return true;
}
AuditPersistenceStatus AuditPersistence::Status() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->status; result.queued = impl_->queue.size(); return result;
}
void AuditPersistence::Stop() {
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; }
    impl_->wake.notify_one();
    if (impl_->worker.joinable()) impl_->worker.join();
}
json::Value ReadPersistentAudit(const std::wstring& root) {
    auto records = json::Value::Array(), status = json::Value::Object();
    std::string error;
    try {
        Directory directory;
        if (!directory.Open(root, false)) return Reply(std::move(records), std::move(status), directory.error);
        Lock lock(directory, false);
        if (!lock) return Reply(std::move(records), std::move(status), lock.error);
        std::array<std::string, kAuditSegments> bytes;
        std::array<uint64_t, kAuditSegments> times{};
        Load(directory, bytes, times, records, status, error);
    } catch (...) { error = "read_exception"; }
    return Reply(std::move(records), std::move(status), error);
}
json::Value ExportPersistentAudit(const std::wstring& root, const std::wstring& destination) {
    auto result = ReadPersistentAudit(root);
    if (!result.get_bool("ok")) return result;
    try {
        const std::filesystem::path path(destination);
        Directory parent;
        if (!parent.Open(path.parent_path().wstring(), false) || path.filename().empty())
            return Reply(json::Value::Array(), json::Value::Object(), "invalid_destination");
        Directory source_directory;
        if (!source_directory.Open(root, false))
            return Reply(json::Value::Array(), json::Value::Object(), "invalid_destination");
        const auto source = source_directory.FinalPath();
        const auto destination_parent = parent.FinalPath();
        if (source.empty() || destination_parent.empty())
            return Reply(json::Value::Array(), json::Value::Object(), "invalid_destination");
        const auto full_destination = destination_parent + L"\\" + path.filename().wstring();
        if (full_destination.size() > source.size() && full_destination[source.size()] == L'\\' &&
            CompareStringOrdinal(full_destination.c_str(), static_cast<int>(source.size()), source.c_str(),
                                 static_cast<int>(source.size()), TRUE) == CSTR_EQUAL)
            return Reply(json::Value::Array(), json::Value::Object(), "destination_inside_audit_root");
        const auto name = path.filename().wstring();
        if (name.find_first_of(L":\\/") != std::wstring::npos || name == L"." || name == L"..")
            return Reply(json::Value::Array(), json::Value::Object(), "invalid_destination");
        // CREATE_NEW never overwrites existing/source objects. No inherited handles.
        Handle file(CreateFileW(parent.File(name).c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
                                CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!file) return Reply(json::Value::Array(), json::Value::Object(), "destination_exists_or_unavailable");
        const auto bytes = json::Serialize(result) + "\n";
        DWORD written = 0;
        if (!SafeFile(file.Get()) || !WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size() || !FlushFileBuffers(file.Get())) {
            FILE_DISPOSITION_INFO remove{TRUE}; SetFileInformationByHandle(file.Get(), FileDispositionInfo, &remove, sizeof(remove));
            return Reply(json::Value::Array(), json::Value::Object(), "export_failed");
        }
        result.set("exported", true);
    } catch (...) { return Reply(json::Value::Array(), json::Value::Object(), "invalid_destination"); }
    return result;
}
void StartPersistentExtensionAudit(std::wstring root) {
    StopPersistentExtensionAudit();
    try { host_audit = std::make_unique<AuditPersistence>(std::move(root)); }
    catch (...) { SetPersistentExtensionAuditStartupFailure(false); }
}
void StopPersistentExtensionAudit() { host_audit.reset(); unavailable_status = {}; }
void SetPersistentExtensionAuditStartupFailure(bool missing_root) {
    unavailable_status = {};
    unavailable_status.failures = 1;
    unavailable_status.last_error = missing_root ? "data_root_unavailable" : "worker_start_failed";
    OutputDebugStringA(missing_root ? "Pulse extension audit: data_root_unavailable\n" :
                                    "Pulse extension audit: worker_start_failed\n");
}
void PersistExtensionAudit(std::string_view package, std::string_view operation, std::string_view outcome) {
    if (host_audit) host_audit->Enqueue(package, operation, outcome);
}
AuditPersistenceStatus PersistentExtensionAuditStatus() { return host_audit ? host_audit->Status() : unavailable_status; }
}
