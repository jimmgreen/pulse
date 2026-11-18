#pragma once
#include "runtime.h"
#include <functional>
namespace pulse { struct AppState; }
namespace pulse::extensions {
std::optional<size_t> ResolveResourceEntry(const CallerBinding& caller, const std::string& entry, AppState& owner);
class ResourceService {
public:
    using Completion = std::function<void(json::Value)>;
    using Cancel = std::function<void()>;
    // Host broker entry, UI thread. CallerLease must be minted by this host; JSON never supplies identity.
    // Memory reads complete synchronously. Own-settings I/O completes on the UI thread asynchronously.
    static Cancel Call(AppState& owner, const CallerLease& caller, const json::Value& request, Completion complete);
    static void ReleaseOwner(uintptr_t owner);
};
}
