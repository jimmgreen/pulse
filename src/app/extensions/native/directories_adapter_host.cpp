#include "../../app_runtime.h"
#include "../../app_navigation.h"
#include "directories_adapter.h"
#include <algorithm>

namespace pulse::extensions::native {
namespace {
DirectoriesBackend Backend(AppState &owner) {
    const HWND window = owner.hwnd;
    DirectoriesBackend backend;
    backend.live = [window](const CallerBinding &caller) {
        return IsWindow(window) && caller.Owner().key == reinterpret_cast<uintptr_t>(window) && ValidateCallerBinding(caller);
    };
    backend.open_item = [window](const CallerBinding &caller, const std::wstring &path, bool folder) -> PulseStatus {
        if (!IsWindow(window) || caller.Owner().key != reinterpret_cast<uintptr_t>(window) || !ValidateCallerBinding(caller))
            return PULSE_ERROR_STATE;
        auto *state = reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!state) return PULSE_ERROR_STATE;
        if (folder) NavigateTo(*state, path);
        else OpenPath(*state, path);
        return PULSE_OK;
    };
    return backend;
}
} // namespace
DirectoriesAdapter::DirectoriesAdapter(AppState &owner, CallerBinding caller)
    : DirectoriesAdapter(std::move(caller), Backend(owner)) {}
} // namespace pulse::extensions::native
