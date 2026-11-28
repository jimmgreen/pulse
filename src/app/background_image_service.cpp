// background_image_service.cpp — see background_image_service.h.
#include "background_image_service.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <system_error>
#include <utility>

namespace pulse::app {
namespace {

constexpr size_t kRetainedCacheFiles = 8;
constexpr size_t kMaxFolderEntries = 20000;
constexpr int kMaxFolderDepth = 8;
constexpr size_t kWriteChunk = 1u << 20;

bool EqualsExtension(std::wstring_view extension, std::wstring_view candidate) {
    if (extension.size() != candidate.size()) return false;
    for (size_t i = 0; i < extension.size(); ++i) {
        wchar_t character = extension[i];
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
        if (character != candidate[i]) return false;
    }
    return true;
}

bool IsImageExtension(std::wstring_view extension) {
    static constexpr std::wstring_view kExtensions[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".tif", L".tiff"};
    for (const auto candidate : kExtensions) {
        if (EqualsExtension(extension, candidate)) return true;
    }
    return false;
}

bool IsCacheExtension(std::wstring_view extension) {
    static constexpr std::wstring_view kExtensions[] = {
        L".png", L".jpg", L".gif", L".bmp", L".webp", L".tif"};
    for (const auto candidate : kExtensions) {
        if (EqualsExtension(extension, candidate)) return true;
    }
    return false;
}

std::filesystem::path DefaultCacheDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!required) return {};
    std::wstring value(static_cast<size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
    if (!written || written >= required) return {};
    value.resize(static_cast<size_t>(written));
    if (value.empty()) return {};
    std::filesystem::path directory(value);
    directory /= L"Pulse";
    directory /= L"wallpaper-cache";
    return directory;
}

uint64_t HashUrl(std::wstring_view url) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t character : url) {
        const uint16_t code = static_cast<uint16_t>(character);
        hash ^= static_cast<uint8_t>(code & 0xFF);
        hash *= 1099511628211ull;
        hash ^= static_cast<uint8_t>((code >> 8) & 0xFF);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring CacheFileName(std::wstring_view url, BackgroundImageFormat format) {
    wchar_t name[32]{};
    swprintf_s(name, L"bg-%016llX.%s", static_cast<unsigned long long>(HashUrl(url)),
        BackgroundImageExtension(format));
    return name;
}

void PruneCache(const std::filesystem::path& directory, const std::filesystem::path& keep) {
    std::error_code error;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
    for (std::filesystem::directory_iterator iterator(directory, error), end; iterator != end;
         iterator.increment(error)) {
        if (error) break;
        std::error_code entry_error;
        if (!iterator->is_regular_file(entry_error) || entry_error) continue;
        const std::wstring name = iterator->path().filename().wstring();
        if (name.size() < 4 || name.compare(0, 3, L"bg-") != 0) continue;
        if (!IsCacheExtension(iterator->path().extension().wstring())) continue;
        const auto write_time = iterator->last_write_time(entry_error);
        if (entry_error) continue;
        entries.emplace_back(write_time, iterator->path());
    }
    if (entries.size() <= kRetainedCacheFiles) return;
    std::sort(entries.begin(), entries.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    const std::wstring keep_path = keep.wstring();
    size_t kept = 0;
    for (const auto& entry : entries) {
        if (kept < kRetainedCacheFiles || entry.second.wstring() == keep_path) {
            ++kept;
            continue;
        }
        std::error_code remove_error;
        std::filesystem::remove(entry.second, remove_error);
    }
}

bool WriteCacheFile(const std::filesystem::path& directory, const std::vector<uint8_t>& body,
                    BackgroundImageFormat format, std::wstring_view url,
                    std::wstring& out_path, std::wstring& error) {
    if (body.empty() || format == BackgroundImageFormat::Unknown || directory.empty()) {
        error = L"wallpaper cache directory is unavailable";
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(directory, directory_error);
    if (directory_error) {
        error = L"cannot create the wallpaper cache directory";
        return false;
    }
    const std::filesystem::path final_path = directory / CacheFileName(url, format);
    std::filesystem::path temporary = final_path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot create a wallpaper cache file";
        return false;
    }
    bool written = true;
    size_t offset = 0;
    while (offset < body.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(body.size() - offset, kWriteChunk));
        DWORD completed = 0;
        if (!WriteFile(file, body.data() + offset, chunk, &completed, nullptr) ||
            completed != chunk) {
            written = false;
            break;
        }
        offset += static_cast<size_t>(completed);
    }
    if (written) written = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        error = L"cannot write a wallpaper cache file";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"cannot publish a wallpaper cache file";
        return false;
    }
    out_path = final_path.wstring();
    PruneCache(directory, final_path);
    return true;
}

void ScanBackgroundFolder(const std::wstring& root, std::vector<std::wstring>& files,
                          std::wstring& error) {
    files.clear();
    error.clear();
    if (root.empty()) {
        error = L"empty folder";
        return;
    }
    std::error_code directory_error;
    const std::filesystem::path directory(root);
    if (!std::filesystem::is_directory(directory, directory_error) || directory_error) {
        error = L"folder is not accessible";
        return;
    }
    std::filesystem::recursive_directory_iterator iterator(directory,
        std::filesystem::directory_options::skip_permission_denied, directory_error);
    const std::filesystem::recursive_directory_iterator end;
    if (directory_error) {
        error = L"folder is not accessible";
        return;
    }
    size_t entries = 0;
    for (; iterator != end; iterator.increment(directory_error)) {
        if (directory_error) {
            directory_error.clear();
            continue;
        }
        if (++entries > kMaxFolderEntries) break;
        if (iterator.depth() >= kMaxFolderDepth) iterator.disable_recursion_pending();
        std::error_code file_error;
        if (!iterator->is_regular_file(file_error) || file_error) continue;
        if (!IsImageExtension(iterator->path().extension().wstring())) continue;
        files.push_back(iterator->path().wstring());
    }
}

std::wstring UrlFromBody(const std::vector<uint8_t>& body) {
    if (body.empty()) return {};
    size_t offset = 0;
    if (body.size() >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) offset = 3;
    const std::string_view text(reinterpret_cast<const char*>(body.data() + offset),
        body.size() - offset);
    std::wstring value = Utf8ToWide(text);
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring NetworkErrorText(uint32_t error) {
    if (error == ERROR_TIMEOUT) return L"network timeout";
    if (error == ERROR_FILE_TOO_LARGE) return L"response is too large";
    return L"network error " + std::to_wstring(error);
}

}  // namespace

void BackgroundShuffleBag::Reset(size_t count, std::mt19937& rng) {
    if (count == 0) {
        Clear();
        return;
    }
    order_.resize(count);
    for (size_t i = 0; i < count; ++i) order_[i] = i;
    std::shuffle(order_.begin(), order_.end(), rng);
    cursor_ = 0;
    if (last_ >= count) has_last_ = false;
    if (has_last_ && count > 1 && order_[0] == last_) std::swap(order_[0], order_[1]);
}

void BackgroundShuffleBag::Clear() noexcept {
    order_.clear();
    cursor_ = 0;
    has_last_ = false;
    last_ = 0;
}

size_t BackgroundShuffleBag::Take(std::mt19937& rng) {
    if (order_.empty()) return 0;
    if (cursor_ >= order_.size()) Reset(order_.size(), rng);
    const size_t value = order_[cursor_++];
    last_ = value;
    has_last_ = true;
    return value;
}

bool IsCurrentBackgroundGeneration(uint64_t captured, uint64_t current) noexcept {
    return captured == current;
}

BackgroundImageService::BackgroundImageService() : cache_root_(DefaultCacheDirectory()) {
    std::random_device device;
    const uint64_t random_seed =
        (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
    rng_.seed(static_cast<uint32_t>(random_seed ^ GetTickCount64()));
    worker_ = std::thread(&BackgroundImageService::WorkerMain, this);
}

BackgroundImageService::~BackgroundImageService() {
    Shutdown();
}

bool BackgroundImageService::SameContent(const BackgroundImageConfig& left,
                                         const BackgroundImageConfig& right) {
    return left.source == right.source && left.image == right.image &&
           left.folder == right.folder && left.url == right.url &&
           left.api_pointer == right.api_pointer;
}

bool BackgroundImageService::SetConfig(BackgroundImageConfig config) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    if (config_ == config) return false;
    if (SameContent(config_, config)) {
        // Only the switch schedule moved: keep the in-flight request, the folder
        // scan, and the published picture; re-arm so the new interval applies.
        config_ = std::move(config);
        ResetScheduleLocked();
        return false;
    }
    config_ = std::move(config);
    ResetScheduleLocked();
    folder_ready_ = false;
    folder_scanned_.clear();
    folder_files_.clear();
    bag_.Clear();
    pending_.reset();
    ++generation_;
    snapshot_.error.clear();

    switch (config_.source) {
        case BackgroundSource::Image:
            snapshot_.busy = false;
            if (config_.image.empty()) {
                ClearPathLocked();
            } else {
                PublishLocked(config_.image, false);
            }
            return true;
        case BackgroundSource::Folder:
            if (config_.folder.empty()) {
                snapshot_.busy = false;
                FailLocked(L"no folder configured");
                return true;
            }
            break;
        case BackgroundSource::Url:
        case BackgroundSource::Api:
            if (config_.url.empty()) {
                snapshot_.busy = false;
                FailLocked(L"no URL configured");
                return true;
            }
            break;
    }

    Job job;
    job.kind = config_.source == BackgroundSource::Folder ? JobKind::Folder : JobKind::Network;
    job.source = config_.source;
    job.generation = generation_.load(std::memory_order_relaxed);
    job.folder = config_.folder;
    job.url = config_.url;
    job.pointer = config_.api_pointer;
    pending_ = std::move(job);
    snapshot_.busy = true;
    snapshot_.error.clear();
    cv_.notify_one();
    return true;
}

BackgroundImageSnapshot BackgroundImageService::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void BackgroundImageService::Tick(uint64_t now_ms, bool navigated) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    bool trigger = false;
    switch (config_.switch_mode) {
        case BackgroundSwitchMode::Manual:
            break;
        case BackgroundSwitchMode::OnStartup:
            if (!startup_consumed_) {
                startup_consumed_ = true;
                trigger = true;
            }
            break;
        case BackgroundSwitchMode::Timed: {
            const uint64_t interval =
                static_cast<uint64_t>(std::clamp(config_.interval_minutes, 1, 1440)) * 60000ull;
            if (!timed_armed_) {
                timed_armed_ = true;
                timed_due_ms_ = now_ms + interval;
            } else if (now_ms >= timed_due_ms_) {
                timed_due_ms_ = now_ms + interval;
                trigger = true;
            }
            break;
        }
        case BackgroundSwitchMode::OnNavigate:
            if (navigated) trigger = true;
            break;
    }
    // An in-flight request swallows every trigger, but startup is consumed by
    // its first tick so a slow initial scan cannot cause a second fetch later.
    if (trigger && !snapshot_.busy) TriggerLocked();
}

bool BackgroundImageService::Next() {
    std::lock_guard lock(mutex_);
    if (stopping_ || snapshot_.busy) return false;
    return TriggerLocked();
}

void BackgroundImageService::Clear() {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    config_ = BackgroundImageConfig{};
    folder_ready_ = false;
    folder_scanned_.clear();
    folder_files_.clear();
    bag_.Clear();
    pending_.reset();
    ++generation_;
    snapshot_.path.clear();
    snapshot_.network = false;
    snapshot_.busy = false;
    snapshot_.error.clear();
    ++snapshot_.version;
    startup_consumed_ = true;
    timed_armed_ = false;
    timed_due_ms_ = 0;
}

void BackgroundImageService::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        pending_.reset();
        ++generation_;
        snapshot_.busy = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t BackgroundImageService::NetworkRequestsForTesting() const {
    return network_requests_.load(std::memory_order_relaxed);
}

void BackgroundImageService::SetFetchForTesting(BackgroundFetchFunction fetch) {
    std::lock_guard lock(mutex_);
    fetch_ = std::move(fetch);
}

void BackgroundImageService::SetCacheRootForTesting(std::filesystem::path root) {
    std::lock_guard lock(mutex_);
    cache_root_ = std::move(root);
}

void BackgroundImageService::WorkerMain() {
    for (;;) {
        Job job;
        BackgroundFetchFunction fetch;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            job = std::move(*pending_);
            pending_.reset();
            fetch = fetch_;
        }
        if (!IsCurrentBackgroundGeneration(job.generation,
                generation_.load(std::memory_order_acquire))) {
            continue;
        }
        try {
            if (job.kind == JobKind::Folder) {
                ExecuteFolder(job);
            } else {
                ExecuteNetwork(job, fetch);
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (IsCurrentBackgroundGeneration(job.generation,
                    generation_.load(std::memory_order_relaxed))) {
                FailLocked(L"background image update failed");
                snapshot_.busy = false;
            }
            continue;
        }
        std::lock_guard lock(mutex_);
        if (IsCurrentBackgroundGeneration(job.generation,
                generation_.load(std::memory_order_relaxed))) {
            snapshot_.busy = false;
        }
    }
}

void BackgroundImageService::ExecuteFolder(const Job& job) {
    {
        std::lock_guard lock(mutex_);
        if (!IsCurrentBackgroundGeneration(job.generation,
                generation_.load(std::memory_order_relaxed))) {
            return;
        }
        if (folder_ready_ && folder_scanned_ == job.folder && !folder_files_.empty()) {
            PublishLocked(folder_files_[bag_.Take(rng_)], false);
            snapshot_.busy = false;
            return;
        }
    }

    std::vector<std::wstring> files;
    std::wstring error;
    ScanBackgroundFolder(job.folder, files, error);

    std::lock_guard lock(mutex_);
    if (!IsCurrentBackgroundGeneration(job.generation,
            generation_.load(std::memory_order_relaxed))) {
        return;
    }
    if (files.empty()) {
        FailLocked(error.empty() ? L"empty folder" : error);
        snapshot_.busy = false;
        return;
    }
    folder_scanned_ = job.folder;
    folder_files_ = std::move(files);
    folder_ready_ = true;
    bag_.Reset(folder_files_.size(), rng_);
    PublishLocked(folder_files_[bag_.Take(rng_)], false);
    snapshot_.busy = false;
}

void BackgroundImageService::ExecuteNetwork(const Job& job, const BackgroundFetchFunction& fetch) {
    const auto current = [&] {
        return IsCurrentBackgroundGeneration(job.generation,
            generation_.load(std::memory_order_acquire));
    };
    const auto fail = [&](std::wstring error) {
        std::lock_guard lock(mutex_);
        if (!IsCurrentBackgroundGeneration(job.generation,
                generation_.load(std::memory_order_relaxed))) {
            return;
        }
        FailLocked(error);
        snapshot_.busy = false;
    };

    if (!fetch) {
        fail(L"network transport is unavailable");
        return;
    }
    if (!current()) return;

    const auto request = [&](std::wstring_view url, size_t max_bytes,
                             BackgroundFetchResult& result) {
        BackgroundFetchRequest item;
        item.url = std::wstring(url);
        item.max_bytes = max_bytes;
        network_requests_.fetch_add(1, std::memory_order_relaxed);
        return fetch(item, result) && result.ok;
    };

    BackgroundFetchResult first;
    if (!request(job.url,
            job.source == BackgroundSource::Api ? kBackgroundMaxJsonBytes
                                                : kBackgroundMaxImageBytes,
            first)) {
        fail(NetworkErrorText(first.error));
        return;
    }
    if (!current()) return;

    std::vector<uint8_t> body;
    std::wstring final_url;
    if (job.source == BackgroundSource::Api) {
        std::wstring image_url;
        if (job.pointer.empty()) {
            image_url = UrlFromBody(first.body);
        } else {
            if (first.body.empty()) {
                fail(L"empty JSON response");
                return;
            }
            const std::string_view text(reinterpret_cast<const char*>(first.body.data()),
                first.body.size());
            std::string value;
            switch (JsonPointerString(text, job.pointer, value)) {
                case JsonPointerStatus::Found:
                    break;
                case JsonPointerStatus::InvalidJson:
                    fail(L"JSON response is invalid");
                    return;
                case JsonPointerStatus::NotFound:
                    fail(L"JSON pointer was not found");
                    return;
                case JsonPointerStatus::NotString:
                    fail(L"JSON pointer did not resolve to a string");
                    return;
            }
            image_url = Utf8ToWide(value);
        }
        if (!IsHttpUrl(image_url)) {
            fail(L"image URL must start with http:// or https://");
            return;
        }
        if (!current()) return;
        BackgroundFetchResult second;
        if (!request(image_url, kBackgroundMaxImageBytes, second)) {
            fail(NetworkErrorText(second.error));
            return;
        }
        body = std::move(second.body);
        final_url = std::move(image_url);
    } else {
        body = std::move(first.body);
        final_url = job.url;
    }
    if (!current()) return;

    if (body.empty()) {
        fail(L"empty response");
        return;
    }
    const BackgroundImageFormat format = SniffBackgroundImage(body.data(), body.size());
    if (format == BackgroundImageFormat::Unknown) {
        fail(L"response is not an image");
        return;
    }

    std::filesystem::path cache_root;
    {
        std::lock_guard lock(mutex_);
        if (!IsCurrentBackgroundGeneration(job.generation,
                generation_.load(std::memory_order_relaxed))) {
            return;
        }
        cache_root = cache_root_;
    }
    if (cache_root.empty()) {
        fail(L"wallpaper cache directory is unavailable");
        return;
    }

    std::wstring cached_path;
    std::wstring cache_error;
    if (!WriteCacheFile(cache_root, body, format, final_url, cached_path, cache_error)) {
        fail(cache_error);
        return;
    }

    std::lock_guard lock(mutex_);
    if (!IsCurrentBackgroundGeneration(job.generation,
            generation_.load(std::memory_order_relaxed))) {
        return;
    }
    PublishLocked(std::move(cached_path), true);
    snapshot_.busy = false;
}

bool BackgroundImageService::TriggerLocked() {
    switch (config_.source) {
        case BackgroundSource::Folder:
            if (config_.folder.empty()) return false;
            return PostLocked(JobKind::Folder);
        case BackgroundSource::Url:
        case BackgroundSource::Api:
            if (config_.url.empty()) return false;
            return PostLocked(JobKind::Network);
        case BackgroundSource::Image:
        default:
            return false;
    }
}

bool BackgroundImageService::PostLocked(JobKind kind) {
    if (stopping_ || snapshot_.busy) return false;
    Job job;
    job.kind = kind;
    job.source = config_.source;
    job.generation = ++generation_;
    job.folder = config_.folder;
    job.url = config_.url;
    job.pointer = config_.api_pointer;
    pending_ = std::move(job);
    snapshot_.busy = true;
    snapshot_.error.clear();
    cv_.notify_one();
    return true;
}

void BackgroundImageService::PublishLocked(std::wstring path, bool network) {
    snapshot_.path = std::move(path);
    snapshot_.network = network;
    snapshot_.error.clear();
    ++snapshot_.version;
}

void BackgroundImageService::ClearPathLocked() {
    snapshot_.path.clear();
    snapshot_.network = false;
    snapshot_.error.clear();
    ++snapshot_.version;
}

void BackgroundImageService::FailLocked(const std::wstring& error) {
    snapshot_.error = error;
}

void BackgroundImageService::ResetScheduleLocked() {
    // Startup is an application event, not a per-configuration one: changing the
    // fit or opacity must not fetch another image in OnStartup mode.
    timed_armed_ = false;
    timed_due_ms_ = 0;
}

}  // namespace pulse::app
