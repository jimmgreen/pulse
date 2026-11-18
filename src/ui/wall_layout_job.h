#pragma once
#include "view_layout.h"
#include <atomic>
#include <algorithm>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::ui {
inline float WallAnchorScroll(const WallRows& before, const WallRows& after, float scroll) {
    if (!before.RowCount() || !after.RowCount()) return scroll;
    const int old_row = std::max(0, before.RowAtY(scroll));
    const int item = before.row_first[static_cast<size_t>(old_row)];
    const int new_row = after.RowOf(item);
    return new_row < 0 ? scroll : std::max(0.0f, scroll +
        after.row_top[static_cast<size_t>(new_row)] - before.row_top[static_cast<size_t>(old_row)]);
}
// One running calculation and one replaceable request; workers own no UI objects.
class WallLayoutJob {
public:
    using Prepare = std::function<const std::vector<float>*(const std::atomic<uint64_t>&, uint64_t)>;
private:
    struct Request {
        Prepare prepare;
        float width, row, scale;
        uint64_t generation;
        std::function<void()> committed;
    };
    struct State {
        std::mutex mutex;
        std::optional<Request> pending;
        std::shared_ptr<const WallRows> ready;
        std::atomic<uint64_t> generation{0};
        bool running = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    ~WallLayoutJob() { Cancel(); }
    void Cancel() {
        std::lock_guard lock(state_->mutex);
        ++state_->generation;
        state_->pending.reset();
        state_->ready.reset();
    }
    void RequestRows(std::vector<float> aspects, float width, float row, float scale) {
        auto values = std::make_shared<const std::vector<float>>(std::move(aspects));
        RequestPrepared([values](const std::atomic<uint64_t>&, uint64_t) { return values.get(); }, width, row, scale);
    }
    void RequestPrepared(Prepare prepare, float width, float row, float scale, std::function<void()> committed = {}, bool preserve_ready = false) {
        const auto state = state_;
        std::lock_guard lock(state->mutex);
        const uint64_t generation = ++state->generation;
        if (!preserve_ready) state->ready.reset();
        state->pending = Request{std::move(prepare), width, row, scale, generation, std::move(committed)};
        if (state->running) return;
        state->running = true;
        try {
            std::thread([state] {
                for (;;) {
                    std::optional<Request> request;
                    {
                        std::lock_guard guard(state->mutex);
                        request = std::move(state->pending);
                        state->pending.reset();
                        if (!request) {
                            state->running = false;
                            return;
                        }
                    }
                    try {
                        const auto* aspects = request->prepare(state->generation, request->generation);
                        if (!aspects || state->generation != request->generation) continue;
                        auto rows = std::make_shared<const WallRows>(BuildWallRows(*aspects,
                            request->width, request->row, request->scale, &state->generation, request->generation));
                        std::lock_guard guard(state->mutex);
                        if (state->generation == request->generation) {
                            state->ready = std::move(rows);
                            if (request->committed) request->committed();
                        }
                    } catch (...) {
                        // Keep the published layout. A changed input can retry.
                    }
                }
            }).detach();
        } catch (...) {
            state->running = false;
            state->pending.reset();
        }
    }
    bool Busy() {
        std::lock_guard lock(state_->mutex);
        return state_->running || bool(state_->ready);
    }
    std::shared_ptr<const WallRows> TakeReady() {
        std::lock_guard lock(state_->mutex);
        return std::exchange(state_->ready, {});
    }
};
}
