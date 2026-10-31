#pragma once
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::index {
// The pipe owns the lease; disconnect revokes it even if the engine has not
// reached its safe point yet. Only the engine worker edits active resources.
struct VolumePause {
    std::vector<std::wstring> ids, roots;
    std::atomic<bool> active{true};
    std::promise<void> released;
    std::shared_future<void> ready = released.get_future().share();
    bool acknowledged = false;
};
}
