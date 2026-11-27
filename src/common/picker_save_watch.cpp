#include "picker_save_watch.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

namespace pulse::picker_save {
namespace {
using Clock = std::chrono::steady_clock;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

DWORD Excluded(DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_REPARSE_TAG_INVALID;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return ERROR_DIRECTORY;
    // These bits include cloud placeholders; do not open a file to hydrate it.
    constexpr DWORD recall_on_open = 0x00040000;
    constexpr DWORD recall_on_data_access = 0x00400000;
    if (attributes & (FILE_ATTRIBUTE_OFFLINE | recall_on_open | recall_on_data_access))
        return ERROR_FILE_OFFLINE;
    return ERROR_SUCCESS;
}

DWORD ReadStamp(const std::wstring& path, DWORD sharing, Stamp& stamp) {
    stamp = {};
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return ERROR_INVALID_NAME;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    if (const DWORD error = Excluded(attributes)) return error;
    // Attribute-only opens do not enforce the writer-sharing check on Windows.
    // The final probe requests read access but never reads file contents.
    const DWORD access = sharing == FILE_SHARE_READ ? GENERIC_READ : FILE_READ_ATTRIBUTES;
    Handle file{CreateFileW(path.c_str(), access, sharing, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) return GetLastError();
    if (const DWORD error = Excluded(info.dwFileAttributes)) return error;
    Stamp result;
    result.exists = true;
    result.attributes = info.dwFileAttributes;
    result.size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.write_time = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    FILE_ID_INFO identity{};
    if (GetFileInformationByHandleEx(file.value, FileIdInfo, &identity, sizeof(identity))) {
        result.volume = identity.VolumeSerialNumber;
        std::memcpy(result.file_id.data(), identity.FileId.Identifier, result.file_id.size());
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
            error != ERROR_INVALID_FUNCTION) return error;
        result.volume = info.dwVolumeSerialNumber;
        const uint64_t legacy_id = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        std::memcpy(result.file_id.data(), &legacy_id, sizeof(legacy_id));
    }
    stamp = result;
    return ERROR_SUCCESS;
}

bool Same(const Stamp& a, const Stamp& b) {
    return a.exists == b.exists && a.volume == b.volume && a.file_id == b.file_id &&
        a.size == b.size && a.write_time == b.write_time;
}

std::wstring PathKey(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return {};
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (normalized.starts_with(L"\\\\?\\UNC\\")) normalized = L"\\\\" + normalized.substr(8);
    else if (normalized.starts_with(L"\\\\?\\")) normalized.erase(0, 4);
    const DWORD needed = GetFullPathNameW(normalized.c_str(), 0, nullptr, nullptr);
    if (!needed || needed > 32768) return {};
    std::wstring full(needed, L'\0');
    const DWORD length = GetFullPathNameW(normalized.c_str(), needed, full.data(), nullptr);
    if (!length || length >= needed) return {};
    full.resize(length);
    return full;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
} // namespace

DWORD Capture(const std::wstring& path, Stamp& stamp) {
    return ReadStamp(path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, stamp);
}

struct Observer::State {
    struct Pending {
        Request request;
        std::wstring key;
        uint64_t generation = 0;
        Clock::time_point expires;
        Clock::time_point stable_since;
        Stamp last;
        bool observed = false;
    };
    explicit State(Config value) : config(value) {}
    Config config;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Pending> pending;
    std::vector<Completion> ready;
    uint64_t generation = 0;
    bool stopping = false;
    HMODULE module = nullptr;
};

Observer::Observer() : Observer(Config{}) {}

Observer::Observer(Config config) {
    config.quiet = (std::max)(config.quiet, std::chrono::milliseconds{1});
    config.deadline = (std::max)(config.deadline, std::chrono::milliseconds{1});
    config.poll = (std::max)(config.poll, std::chrono::milliseconds{1});
    config.capacity = (std::min)(config.capacity, size_t{256});
    auto state = std::make_shared<State>(config);
    auto argument = std::make_unique<std::shared_ptr<State>>(state);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Observer::Worker), &state->module)) return;
    HANDLE thread = CreateThread(nullptr, 0, &Observer::Worker, argument.get(), 0, nullptr);
    if (!thread) { FreeLibrary(state->module); return; }
    argument.release();
    CloseHandle(thread);
    state_ = std::move(state);
}

Observer::~Observer() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pending.clear();
        state_->ready.clear();
    }
    state_->wake.notify_one();
    // A remote filesystem operation may still be in progress. Its worker owns
    // both the state and a module reference; destruction never waits for I/O.
}

bool Observer::Submit(Request request) {
    if (!state_ || !request.request_id) return false;
    const auto key = PathKey(request.path);
    if (key.empty()) return false;
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return false;
    for (const auto& item : state_->pending)
        if (item.request.request_id == request.request_id) return false;
    for (const auto& item : state_->ready)
        if (item.request.request_id == request.request_id) return false;
    const auto pending_same = [&](const State::Pending& item) { return SamePath(item.key, key); };
    const auto ready_same = [&](const Completion& item) { return SamePath(PathKey(item.request.path), key); };
    const auto replaced = std::count_if(state_->pending.begin(), state_->pending.end(), pending_same) +
        std::count_if(state_->ready.begin(), state_->ready.end(), ready_same);
    if (state_->pending.size() + state_->ready.size() - static_cast<size_t>(replaced) >= state_->config.capacity)
        return false;
    State::Pending item;
    item.request = std::move(request);
    item.key = key;
    item.generation = ++state_->generation;
    item.expires = Clock::now() + state_->config.deadline;
    std::erase_if(state_->pending, pending_same);
    std::erase_if(state_->ready, ready_same);
    state_->pending.push_back(std::move(item));
    state_->wake.notify_one();
    return true;
}

void Observer::Cancel(uint64_t request_id) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    std::erase_if(state_->pending, [&](const State::Pending& item) { return item.request.request_id == request_id; });
    std::erase_if(state_->ready, [&](const Completion& item) { return item.request.request_id == request_id; });
}

std::vector<Completion> Observer::Drain() {
    std::vector<Completion> result;
    if (!state_) return result;
    std::lock_guard lock(state_->mutex);
    result.swap(state_->ready);
    return result;
}

DWORD WINAPI Observer::Worker(void* argument) {
    HMODULE module = nullptr;
    {
        std::unique_ptr<std::shared_ptr<State>> holder(static_cast<std::shared_ptr<State>*>(argument));
        auto state = *holder;
        holder.reset();
        module = state->module;
        try {
            for (;;) {
                std::vector<State::Pending> batch;
                {
                    std::unique_lock lock(state->mutex);
                    if (state->pending.empty())
                        state->wake.wait(lock, [&] { return state->stopping || !state->pending.empty(); });
                    if (state->stopping) break;
                    batch = state->pending;
                }
                for (auto& item : batch) {
                    Stamp current;
                    bool complete = false;
                    const auto now = Clock::now();
                    if (now < item.expires) {
                        const DWORD error = Capture(item.request.path, current);
                        if (error || !current.exists || Same(current, item.request.before)) {
                            item.observed = false;
                        } else if (!item.observed || !Same(current, item.last)) {
                            item.last = current;
                            item.observed = true;
                            item.stable_since = Clock::now();
                        } else if (now - item.stable_since >= state->config.quiet) {
                            Stamp verified;
                            // Excluding FILE_SHARE_WRITE and FILE_SHARE_DELETE
                            // rejects open writers and replacement in progress.
                            const DWORD error_verify = ReadStamp(item.request.path, FILE_SHARE_READ, verified);
                            complete = !error_verify && verified.exists && Same(verified, current);
                            if (complete) current = verified;
                            else item.observed = false;
                        }
                    }
                    std::lock_guard lock(state->mutex);
                    if (state->stopping) break;
                    auto live = std::find_if(state->pending.begin(), state->pending.end(),
                        [&](const State::Pending& entry) { return entry.generation == item.generation; });
                    if (live == state->pending.end()) continue;
                    if (Clock::now() >= item.expires) state->pending.erase(live);
                    else if (complete) {
                        state->ready.push_back({std::move(item.request), current});
                        state->pending.erase(live);
                    } else *live = std::move(item);
                }
                std::unique_lock lock(state->mutex);
                if (state->stopping) break;
                state->wake.wait_for(lock, state->config.poll);
            }
        } catch (...) {
            std::lock_guard lock(state->mutex);
            state->stopping = true;
            state->pending.clear();
        }
    }
    FreeLibraryAndExitThread(module, 0);
}

} // namespace pulse::picker_save
