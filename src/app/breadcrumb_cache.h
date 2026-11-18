#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::app {
using BreadcrumbFolders = std::vector<std::pair<std::wstring, std::wstring>>;
struct BreadcrumbJob {
    std::wstring key;
    std::mutex mutex;
    bool ready = false, failed = false;
    BreadcrumbFolders folders;
};

// Process-wide provider ownership, independent of a popup's subscription.
// Cached successes require both a short TTL and a caller-validated directory revision.
class BreadcrumbCache {
public:
    struct Acquisition { std::shared_ptr<BreadcrumbJob> job; bool start = false; };
    static std::wstring Key(std::wstring path) {
        // Keep case: NTFS directories can opt into case-sensitive names.
        for (auto& ch : path) if (ch == L'/') ch = L'\\';
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        return path;
    }
    Acquisition Acquire(const std::wstring& path) {
        std::lock_guard lock(mutex_);
        const auto key = Key(path);
        if (auto it = active_.find(key); it != active_.end()) return {it->second, false};
        auto job = std::make_shared<BreadcrumbJob>();
        job->key = key;
        active_.emplace(key, job);
        return {std::move(job), true};
    }
    void Retire(const std::wstring& path, const std::shared_ptr<BreadcrumbJob>& job) {
        (void)path;
        std::lock_guard lock(mutex_);
        auto it = active_.find(job->key);
        if (it != active_.end() && it->second == job) active_.erase(it);
    }
    bool Load(const std::wstring& path, uint64_t revision, uint64_t now, BreadcrumbFolders& folders) {
        std::lock_guard lock(mutex_);
        auto it = completed_.find(Key(path));
        if (it == completed_.end()) return false;
        if (now < it->second.completed || now - it->second.completed > 500 || it->second.revision != revision) {
            bytes_ -= it->second.bytes; completed_.erase(it); return false;
        }
        folders = it->second.folders;
        it->second.used = ++clock_;
        return true;
    }
    void Store(const std::wstring& path, uint64_t revision, uint64_t now, const BreadcrumbFolders& folders) {
        const auto key = Key(path);
        size_t bytes = key.size() * sizeof(wchar_t);
        for (const auto& [name, child] : folders) bytes += (name.size() + child.size()) * sizeof(wchar_t) + sizeof(folders[0]);
        std::lock_guard lock(mutex_);
        Erase(key);
        if (bytes > kBytes) return;
        while (!completed_.empty() && (completed_.size() >= kEntries || bytes_ + bytes > kBytes)) {
            auto oldest = std::min_element(completed_.begin(), completed_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            bytes_ -= oldest->second.bytes; completed_.erase(oldest);
        }
        completed_.emplace(key, Entry{folders, revision, now, ++clock_, bytes});
        bytes_ += bytes;
    }
    void Invalidate(const std::wstring& path) { std::lock_guard lock(mutex_); Erase(Key(path)); }
    std::pair<size_t, size_t> Size() { std::lock_guard lock(mutex_); return {completed_.size(), bytes_}; }
private:
    void Erase(const std::wstring& key) {
        auto it = completed_.find(key);
        if (it != completed_.end()) { bytes_ -= it->second.bytes; completed_.erase(it); }
    }
    struct Entry { BreadcrumbFolders folders; uint64_t revision, completed, used; size_t bytes; };
    static constexpr size_t kEntries = 32, kBytes = 2 * 1024 * 1024;
    std::mutex mutex_;
    std::unordered_map<std::wstring, std::shared_ptr<BreadcrumbJob>> active_;
    std::unordered_map<std::wstring, Entry> completed_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
};
}
