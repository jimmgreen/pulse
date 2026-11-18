#include "development_reload.h"
#include "package_revision.h"
#include <windows.h>
#include <mutex>
#include <thread>

namespace pulse::extensions {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ReadResult {
    std::string bytes;
    std::string error;
    bool cancelled = false;
};
ReadResult Read(const std::wstring& path, HANDLE stopped) {
    ReadResult result;
    if (WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0) { result.cancelled = true; return result; }
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK) {
        result.error = "Cannot open the development manifest.";
        return result;
    }
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) { result.error = "Cannot create the development read event."; return result; }
    OVERLAPPED overlap{};
    overlap.hEvent = event.value;
    result.bytes.resize(kMaxManifestBytes + 1);
    DWORD count = 0;
    if (!ReadFile(file.value, result.bytes.data(), static_cast<DWORD>(result.bytes.size()), &count, &overlap)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            const HANDLE waits[] = {stopped, event.value};
            const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 2000);
            if (waited != WAIT_OBJECT_0 + 1) {
                CancelIoEx(file.value, &overlap);
                GetOverlappedResult(file.value, &overlap, &count, TRUE);
                result.cancelled = waited == WAIT_OBJECT_0;
                result.error = result.cancelled ? "" : "Development manifest read timed out.";
                return result;
            }
            if (!GetOverlappedResult(file.value, &overlap, &count, FALSE) && GetLastError() != ERROR_HANDLE_EOF) {
                result.error = "Cannot read the development manifest.";
                return result;
            }
        } else if (error != ERROR_HANDLE_EOF) {
            result.error = "Cannot read the development manifest.";
            return result;
        }
    }
    result.bytes.resize(count);
    return result;
}
}

struct DevelopmentReload::Impl {
    mutable std::mutex mutex;
    std::thread worker;
    HANDLE stopped = nullptr;
    std::wstring path;
    std::wstring diagnostic;
    std::string previous;
    uint64_t generation = 0;
    uint64_t revision = 0;
    uint64_t applied_revision = 0;
    std::string status = "active";
    std::string error;
    bool enabled = false;
    bool dirty = true;
    uint64_t write_sequence = 0;
    std::optional<DevelopmentUpdate> pending;

    json::Value Snapshot() const {
        auto value = json::Value::Object();
        value.set("schemaVersion", 1);
        value.set("manifest", json::Utf8(path));
        value.set("generation", generation);
        value.set("revision", revision);
        value.set("appliedRevision", applied_revision);
        value.set("status", status);
        value.set("enabled", enabled);
        value.set("error", error);
        return value;
    }
    void Publish() {
        std::string bytes;
        {
            std::lock_guard lock(mutex);
            if (!dirty) return;
            bytes = json::Serialize(Snapshot()) + "\n";
            dirty = false;
        }
        const std::wstring temporary = diagnostic + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(generation) + L"." + std::to_wstring(++write_sequence) + L".tmp";
        bool written = false;
        bool created = false;
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.value != INVALID_HANDLE_VALUE) {
                created = true;
                DWORD count = 0;
                written = WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
            }
        }
        if (written && MoveFileExW(temporary.c_str(), diagnostic.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        if (created) DeleteFileW(temporary.c_str());
        std::lock_guard lock(mutex);
        dirty = true;
    }
    void Run() {
        Publish();
        while (WaitForSingleObject(stopped, 250) == WAIT_TIMEOUT) {
            auto read = Read(path, stopped);
            if (read.cancelled || WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0) break;
            const std::string fingerprint = read.error.empty() ? "bytes:" + read.bytes : "error:" + read.error;
            if (fingerprint != previous) {
                previous = fingerprint;
                ManifestResult parsed;
                if (read.error.empty()) parsed = ParseManifest(read.bytes);
                const auto hash = parsed.manifest ? PackageRevision(read.bytes) : std::string{};
                const bool identity_failed = parsed.manifest && hash.empty();
                if (identity_failed) { parsed.manifest.reset(); parsed.error = "Cannot hash validated manifest."; }
                std::lock_guard lock(mutex);
                ++revision;
                error = read.error.empty() ? parsed.error : read.error;
                status = !read.error.empty() ? "read_failed" : parsed ? "pending" : "invalid";
                pending = DevelopmentUpdate{generation, revision, std::move(parsed.manifest), hash, identity_failed};
                dirty = true;
            }
            Publish();
        }
        {
            std::lock_guard lock(mutex);
            pending.reset();
            status = "stopped";
            dirty = true;
        }
        Publish();
    }
};

DevelopmentReload::DevelopmentReload() : impl_(std::make_unique<Impl>()) {}
DevelopmentReload::~DevelopmentReload() { Stop(); }
bool DevelopmentReload::Start(uint64_t generation, std::wstring path, std::wstring diagnostic,
    std::string initial_bytes, bool enabled) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->generation = generation;
    impl_->path = std::move(path);
    impl_->diagnostic = std::move(diagnostic);
    impl_->previous = "bytes:" + initial_bytes;
    impl_->enabled = enabled;
    impl_->stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stopped) return false;
    try { impl_->worker = std::thread([state = impl_.get()] { state->Run(); }); }
    catch (...) { CloseHandle(impl_->stopped); impl_->stopped = nullptr; return false; }
    return true;
}
void DevelopmentReload::Stop() {
    if (impl_->stopped) SetEvent(impl_->stopped);
    if (impl_->worker.joinable()) {
        CancelSynchronousIo(impl_->worker.native_handle());
        impl_->worker.join();
    }
    if (impl_->stopped) { CloseHandle(impl_->stopped); impl_->stopped = nullptr; }
}
std::optional<DevelopmentUpdate> DevelopmentReload::TakeUpdate() {
    std::lock_guard lock(impl_->mutex);
    auto update = std::move(impl_->pending);
    impl_->pending.reset();
    return update;
}
bool DevelopmentReload::Commit(const DevelopmentUpdate& update, bool enabled, const std::function<void()>& swap) {
    std::lock_guard lock(impl_->mutex);
    if (update.generation != impl_->generation || update.revision != impl_->revision ||
        update.revision <= impl_->applied_revision || !update.manifest ||
        !impl_->stopped || WaitForSingleObject(impl_->stopped, 0) == WAIT_OBJECT_0 || !swap) return false;
    swap();
    impl_->applied_revision = update.revision;
    impl_->enabled = enabled;
    impl_->status = "active";
    impl_->error.clear();
    impl_->dirty = true;
    return true;
}
void DevelopmentReload::SetEnabled(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->enabled == enabled) return;
    impl_->enabled = enabled;
    impl_->dirty = true;
}
json::Value DevelopmentReload::Status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->Snapshot();
}
}
