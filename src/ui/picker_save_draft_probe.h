#pragma once

#include "preview_resource_budget.h"
#include "../fs/fs_enum.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace pulse::ui {
// UI-thread owned. One worker, one replaceable request, and no HWND/payload
// lifetime coupling. A retired slow worker keeps its budget lease until exit.
class PickerSaveDraftProbe {
public:
    struct Result {
        uint64_t generation = 0;
        std::wstring path;
        DWORD attributes = INVALID_FILE_ATTRIBUTES;
        DWORD error = ERROR_SUCCESS;
        bool space_known = false;
        uint64_t free_bytes = 0;
        DWORD parent_attributes = INVALID_FILE_ATTRIBUTES;
    };
    using Probe = std::function<void(Result&)>;
    explicit PickerSaveDraftProbe(Probe probe = ReadAttributes) : probe_(std::move(probe)) {}
    ~PickerSaveDraftProbe() {
        if (!state_ || !worker_.joinable()) return;
        {
            std::lock_guard lock(state_->mutex);
            state_->stop = true;
            state_->pending.reset();
            state_->result.reset();
        }
        state_->wake.notify_one();
        CancelSynchronousIo(worker_.native_handle());
        worker_.detach();
    }
    void Cancel() {
        if (!state_ || !worker_.joinable()) return;
        {
            std::lock_guard lock(state_->mutex);
            ++state_->serial;
            state_->pending.reset();
            state_->result.reset();
        }
        CancelSynchronousIo(worker_.native_handle());
    }
    bool Start(uint64_t generation, const std::wstring& path) {
        Cancel();
        if (!state_) {
            static auto* budget = new PreviewResourceBudget(4);
            auto lease = budget->TryAcquire();
            if (!lease) return false;
            auto state = std::make_shared<State>();
            try {
                worker_ = std::thread([state, probe = probe_, lease = std::move(lease)] {
                    (void)lease;
                    ThreadErrorMode mode;
                    for (;;) {
                        Result request;
                        uint64_t serial = 0;
                        {
                            std::unique_lock lock(state->mutex);
                            state->wake.wait(lock, [&] { return state->stop || state->pending.has_value(); });
                            if (state->stop) return;
                            request = std::move(*state->pending);
                            state->pending.reset();
                            serial = state->serial;
                        }
                        try { probe(request); }
                        catch (...) {
                            request.attributes = INVALID_FILE_ATTRIBUTES;
                            request.error = ERROR_GEN_FAILURE;
                            request.space_known = false;
                            request.free_bytes = 0;
                            request.parent_attributes = INVALID_FILE_ATTRIBUTES;
                        }
                        std::lock_guard lock(state->mutex);
                        if (state->stop) return;
                        if (serial == state->serial) state->result = std::move(request);
                    }
                });
            } catch (...) { return false; }
            state_ = std::move(state);
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->pending = Result{generation, path};
        }
        state_->wake.notify_one();
        return true;
    }
    std::optional<Result> Take(uint64_t generation, const std::wstring& path) {
        if (!state_) return {};
        std::lock_guard lock(state_->mutex);
        auto result = std::move(state_->result);
        state_->result.reset();
        if (result && result->generation == generation && result->path == path) return result;
        return {};
    }
private:
    struct ThreadErrorMode {
        DWORD previous = 0;
        bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
        ~ThreadErrorMode() { if (changed) SetThreadErrorMode(previous, nullptr); }
    };
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false;
        uint64_t serial = 0;
        std::optional<Result> pending, result;
    };
    static void ReadAttributes(Result& result) {
        result.attributes = INVALID_FILE_ATTRIBUTES;
        result.error = ERROR_SUCCESS;
        result.space_known = false;
        result.free_bytes = 0;
        result.parent_attributes = INVALID_FILE_ATTRIBUTES;
        const auto path = fs::NormalizePath(result.path);
        if (path.empty()) { result.error = result.path.empty() ? ERROR_INVALID_NAME : GetLastError(); return; }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) result.attributes = data.dwFileAttributes;
        else result.error = GetLastError();
        auto parent = fs::ParentPath(path);
        if (GetFileAttributesExW(parent.c_str(), GetFileExInfoStandard, &data)) result.parent_attributes = data.dwFileAttributes;
        // Directory READONLY is not a write-permission check. Metadata queries
        // neither open the target's contents nor request placeholder hydration.
        if (result.parent_attributes != INVALID_FILE_ATTRIBUTES && (result.parent_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!parent.ends_with(L'\\')) parent += L'\\';
            ULARGE_INTEGER available{};
            if (GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr)) {
                result.space_known = true;
                result.free_bytes = available.QuadPart;
            }
        }
    }
    Probe probe_;
    std::shared_ptr<State> state_;
    std::thread worker_;
};
} // namespace pulse::ui
