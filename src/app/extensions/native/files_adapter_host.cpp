#include "../../app_runtime.h"
#include "../resource_contract.h"
#include "../resource_service.h"
#include "files_adapter.h"
namespace pulse::extensions::native {
namespace {
FilesBackend Backend(AppState &owner) {
    const HWND window = owner.hwnd;
    FilesBackend backend;
    backend.live = [window](const CallerBinding &caller) {
        return IsWindow(window) && caller.Owner().key == reinterpret_cast<uintptr_t>(window) &&
               ValidateCallerBinding(caller);
    };
    backend.dispatch = [window](const CallerBinding &caller, const json::Value &input,
                                FilesBackend::Completion done) -> FilesBackend::Cancel {
        auto *state =
            IsWindow(window) ? reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
        const auto lease = IssueCaller(caller);
        if (!state || !lease || caller.Owner().key != reinterpret_cast<uintptr_t>(window)) {
            done(ResourceFailure("invalid_caller"));
            return {};
        }
        // Temporary internal conversion seam: existing service remains authoritative
        // for opaque references, native confirmation and task ownership.
        return ResourceService::Call(*state, *lease, input, std::move(done));
    };
    return backend;
}
} // namespace
FilesAdapter::FilesAdapter(AppState &owner, CallerBinding caller) : FilesAdapter(std::move(caller), Backend(owner)) {}
} // namespace pulse::extensions::native
