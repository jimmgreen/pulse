#pragma once
#include "quick_preview_sizing.h"
#include "preview_resource_budget.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ui {
// One bounded worker and one replaceable request; no window pointers or posted
// heap payloads survive a close. Cancellation never joins slow filesystem I/O.
class QuickPreviewSizeProbe {
public:
    struct Result {
        uint64_t generation = 0;
        std::wstring path;
        uint32_t width = 0, height = 0;
    };
    using Probe = std::function<void(Result&)>;
    explicit QuickPreviewSizeProbe(Probe probe = ReadHeader) : probe_(std::move(probe)) {}
    ~QuickPreviewSizeProbe() {
        if (!state_ || !worker_.joinable()) return;
        { std::lock_guard lock(state_->mutex); state_->stop = true; state_->pending.reset(); }
        state_->wake.notify_one();
        CancelSynchronousIo(worker_.native_handle());
        worker_.detach();
    }
    void Cancel() {
        if (!state_ || !worker_.joinable()) return;
        { std::lock_guard lock(state_->mutex); ++state_->serial; state_->pending.reset(); state_->result.reset(); }
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
                for (;;) {
                    Result request; uint64_t serial = 0;
                    {
                        std::unique_lock lock(state->mutex);
                        state->wake.wait(lock, [&] { return state->stop || state->pending.has_value(); });
                        if (state->stop) return;
                        request = std::move(*state->pending); state->pending.reset(); serial = state->serial;
                    }
                    try { probe(request); } catch (...) { request.width = request.height = 0; }
                    std::lock_guard lock(state->mutex);
                    if (state->stop) return;
                    if (serial == state->serial) state->result = std::move(request);
                }
            });
            } catch (...) { return false; }
            state_ = std::move(state);
        }
        { std::lock_guard lock(state_->mutex); state_->pending = Result{generation, path}; }
        state_->wake.notify_one();
        return true;
    }
    std::optional<Result> Take(uint64_t generation, const std::wstring& path) {
        if (!state_) return {};
        std::lock_guard lock(state_->mutex);
        auto result = std::move(state_->result); state_->result.reset();
        if (result && result->generation == generation && result->path == path) return result;
        return {};
    }
private:
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false;
        uint64_t serial = 0;
        std::optional<Result> pending, result;
    };
    static void ReadHeader(Result& result) {
        std::wstring path = result.path;
        if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
        if (path.rfind(L"\\\\", 0) == 0 || path.rfind(L"UNC\\", 0) == 0) return;
        if (path.size() >= 2 && path[1] == L':') {
            const wchar_t root[4] = {path[0], L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_REMOTE) return;
        }
        HANDLE file = CreateFileW(result.path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        uint8_t head[128 * 1024];
        DWORD read = 0;
        const BOOL ok = ReadFile(file, head, sizeof(head), &read, nullptr);
        CloseHandle(file);
        if (ok) ProbeImageSize(head, read, result.width, result.height);
    }
    Probe probe_;
    std::shared_ptr<State> state_;
    std::thread worker_;
};
} // namespace pulse::ui
