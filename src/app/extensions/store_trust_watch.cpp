#include "store_trust_watch.h"
#include "package_signature.h"
#include <windows.h>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <stdexcept>

namespace pulse::extensions {
namespace {
struct Handles {
    std::vector<HANDLE> values;
    ~Handles() { for (auto value : values) CloseHandle(value); }
};
bool ReadPolicy(const std::wstring& root, std::string& bytes) {
    const std::filesystem::path path(root);
    if (!path.is_absolute() || path.root_name().wstring().size() != 2) return false;
    Handles pins;
    std::vector<std::filesystem::path> ancestors;
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        ancestors.push_back(ancestor);
        if (ancestor == ancestor.root_path()) break;
    }
    for (auto at = ancestors.rbegin(); at != ancestors.rend(); ++at) {
        const auto& ancestor = *at;
        HANDLE handle = CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        pins.values.push_back(handle);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    HANDLE file = CreateFileW((path / L"signature-policy.json").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    pins.values.push_back(file);
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 65536) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
}
}
struct StoreTrustWatch::Impl {
    Impl(std::wstring directory, uint64_t epoch, std::vector<StoreTrustSnapshot> values, PolicyReader read)
        : root(std::move(directory)), generation(epoch), snapshots(std::move(values)), revoked(snapshots.size(), false),
          reader(read ? std::move(read) : PolicyReader{ReadPolicy}) {}
    void Run() {
        for (;;) {
            StoreTrustUpdate result{generation, ++check_sequence, {}, {}};
            std::string policy;
            bool read = false;
            for (int attempt = 0; attempt < 3 && !read; ++attempt) {
                policy.clear();
                { std::lock_guard lock(mutex); if (stopping) return; }
                try { read = reader(root, policy); } catch (...) {}
                if (!read && attempt != 2) {
                    std::unique_lock lock(mutex);
                    if (wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping; })) return;
                }
            }
            for (size_t i = 0; i < snapshots.size(); ++i) {
                { std::lock_guard lock(mutex); if (stopping) return; }
                if (revoked[i]) continue;
                std::string ignored;
                bool trusted = false;
                try { trusted = read && VerifyPackageSignature(snapshots[i].manifest, snapshots[i].signature, policy, ignored); }
                catch (...) {}
                if (!trusted) { revoked[i] = true; result.revoked.push_back(i); }
            }
            if (!result.revoked.empty()) result.error = read ? "trust_revoked" : "trust_policy_unavailable";
            std::unique_lock lock(mutex);
            if (stopping) return;
            if (!pending) pending = std::move(result);
            else {
                pending->check_sequence = result.check_sequence;
                pending->revoked.insert(pending->revoked.end(), result.revoked.begin(), result.revoked.end());
                if (!result.error.empty()) pending->error = result.error;
            }
            if (wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; })) return;
        }
    }
    std::wstring root;
    uint64_t generation;
    uint64_t check_sequence = 0;
    std::vector<StoreTrustSnapshot> snapshots;
    std::vector<bool> revoked;
    PolicyReader reader;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool finished = false;
    std::optional<StoreTrustUpdate> pending;
};
StoreTrustWatch::StoreTrustWatch(std::wstring root, uint64_t generation, std::vector<StoreTrustSnapshot> snapshots, PolicyReader reader) {
    // UI-thread lifecycle; a stuck prior worker must not accumulate on repeated startups.
    static std::weak_ptr<Impl> previous;
    if (!previous.expired() || snapshots.size() > 32) throw std::runtime_error("trust_worker_unavailable");
    impl_ = std::make_shared<Impl>(std::move(root), generation, std::move(snapshots), std::move(reader));
    previous = impl_;
    worker_ = std::thread([state = impl_] {
        try { state->Run(); } catch (...) {
            std::lock_guard lock(state->mutex);
            if (!state->stopping) {
                StoreTrustUpdate failure{state->generation, ++state->check_sequence, {}, "trust_worker_failed"};
                for (size_t i = 0; i < state->snapshots.size(); ++i) failure.revoked.push_back(i);
                state->pending = std::move(failure);
            }
        }
        { std::lock_guard lock(state->mutex); state->finished = true; }
        state->wake.notify_all();
    });
}
StoreTrustWatch::~StoreTrustWatch() {
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; }
    impl_->wake.notify_all();
    CancelSynchronousIo(worker_.native_handle());
    std::unique_lock lock(impl_->mutex);
    const bool done = impl_->wake.wait_for(lock, std::chrono::milliseconds(200), [this] { return impl_->finished; });
    lock.unlock();
    if (done) worker_.join();
    else worker_.detach(); // Private shared state only; never calls runtime or UI after shutdown.
}
std::optional<StoreTrustUpdate> StoreTrustWatch::TakeUpdate() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->pending, std::nullopt);
}
}
