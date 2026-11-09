#include "context_adapter.h"
#include "../../app_runtime.h"
#include "../../../common/display_path.h"
#include <algorithm>
#include <stdexcept>
#include <filesystem>
namespace pulse::extensions::native {
namespace {
std::string Utf8(const std::wstring &text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("invalid_context_path");
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
const char *ViewMode(ui::ViewMode mode) {
    switch (mode) {
    case ui::ViewMode::ExtraLargeIcons: return "extraLargeIcons";
    case ui::ViewMode::LargeIcons: return "largeIcons";
    case ui::ViewMode::MediumIcons: return "mediumIcons";
    case ui::ViewMode::SmallIcons: return "smallIcons";
    case ui::ViewMode::List: return "list";
    case ui::ViewMode::Details: return "details";
    case ui::ViewMode::Tiles: return "tiles";
    case ui::ViewMode::Content: return "content";
    }
    return "";
}
ContextBackend Backend(AppState &owner) {
    const auto window = owner.hwnd;
    ContextBackend backend;
    backend.live = [window](const CallerBinding &caller) {
        return IsWindow(window) && caller.Owner().key == reinterpret_cast<uintptr_t>(window) && ValidateCallerBinding(caller);
    };
    backend.capture = [window] {
        ContextObservation value;
        auto *state = IsWindow(window) ? reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
        const auto *tab = state ? ActiveTab(*state) : nullptr;
        if (!tab) return value;
        value.directory = Utf8(path::FriendlyPathText(tab->current_path));
        value.view_mode = ViewMode(tab->view_mode);
        value.tab_key = reinterpret_cast<uintptr_t>(tab);
        value.snapshot_key = reinterpret_cast<uintptr_t>(tab->snapshot.get());
        value.selection_epoch = tab->selection_revision;
        value.loading = tab->loading;
        if (value.loading) return value;
        value.selection_count = static_cast<uint64_t>(tab->SelectedCount());
        if (value.selection_count != 1 || tab->content_results) return value;
        int selected = -1;
        if (tab->all_selected && tab->CountBound() == 1) selected = 0;
        else if (tab->selected.size() == 1) selected = *tab->selected.begin();
        if (selected >= 0 && static_cast<size_t>(selected) < tab->EntryCount() && tab->IsSelected(selected))
            value.selection_name = Utf8(tab->EntryAt(static_cast<size_t>(selected)).name);
        return value;
    };
    backend.capture_selection = [window](ContextSelection &value) -> PulseStatus {
        auto *state = IsWindow(window) ? reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
        const auto *tab = state ? ActiveTab(*state) : nullptr;
        if (!tab || tab->loading || !tab->snapshot || tab->snapshot_path != tab->current_path) return PULSE_ERROR_STATE;
        if (tab->content_results || tab->search_entries || !std::filesystem::path(tab->current_path).is_absolute())
            return PULSE_ERROR_UNAVAILABLE;
        if (tab->SelectedCount() > 4096) return PULSE_ERROR_UNAVAILABLE;
        value.directory = Utf8(tab->current_path);
        size_t bytes = value.directory.size();
        const auto indices = tab->SelectedIndices();
        if (indices.size() > 4096) return PULSE_ERROR_UNAVAILABLE;
        for (const int index : indices) {
            if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount() || !tab->IsSelected(index)) return PULSE_ERROR_STATE;
            const auto &entry = tab->EntryAt(static_cast<size_t>(index));
            const auto full_path = EntryFullPath(*tab, index);
            if (!std::filesystem::path(full_path).is_absolute()) return PULSE_ERROR_UNAVAILABLE;
            ContextSelectionEntry item{Utf8(full_path), Utf8(entry.name), entry.is_dir};
            bytes += item.path.size() + item.name.size();
            if (bytes > 4 * 1024 * 1024) return PULSE_ERROR_UNAVAILABLE;
            value.entries.push_back(std::move(item));
        }
        return PULSE_OK;
    };
    return backend;
}
}
ContextAdapter::ContextAdapter(AppState &owner, CallerBinding caller)
    : ContextAdapter(std::move(caller), Backend(owner)) {}
}
