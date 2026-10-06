#include "audio_meta_ui.h"
#include "app_state.h"
#include "../common/preview_extensions.h"
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"
#include "../ui/view_layout.h"

namespace pulse {
namespace {
std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    return !parent.empty() && parent.back() == L'\\' ? parent + name : parent + L"\\" + name;
}

uint64_t ToTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
} // namespace

void FillAudioMeta(AppState& s, ui::WindowViewModel& vm) {
    const uint32_t wanted = s.renderer.DetailsColumnsMask() & ui::kDetailsColumnAudio;
    for (auto& slot : vm.pane_slots) {
        auto& pane = slot.pane;
        pane.audio_meta_labels.clear();
        // Only the details columns show these, and search results are listed
        // from the index, which carries no audio properties.
        if (wanted == 0 || pane.view_mode != ui::ViewMode::Details || pane.loading ||
            pane.content_results || pane.is_search || pane.is_recycle || !pane.is_file_system ||
            !pane.snapshot || pane.path.empty() || fs::IsVirtualPath(pane.path)) continue;
        const auto list = s.renderer.PaneListRect(pane, slot.rect);
        ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x, pane.scroll_y,
                              s.scale, s.renderer.ListRowHeightDip(pane, list), pane.Groups());
        const auto [first, last] = layout.VisibleRange();
        for (int i = std::max(0, first); i <= last; ++i) {
            const int source = pane.SourceIndex(i);
            if (source < 0 || static_cast<size_t>(source) >= pane.snapshot->size()) continue;
            const auto& entry = (*pane.snapshot)[static_cast<size_t>(source)];
            if (entry.is_dir || entry.drive_type != 0) continue;
            const std::wstring path = entry.full_path.empty()
                ? ChildPath(pane.path, entry.name) : entry.full_path;
            if (fs::IsVirtualPath(path)) continue;
            const size_t dot = path.find_last_of(L'.');
            if (dot == std::wstring::npos) continue;
            std::wstring extension = path.substr(dot);
            for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
            if (!preview::IsAudioExtension(extension)) continue;
            ui::AudioMetaValues values;
            if (!s.renderer.RequestAudioMeta(path, entry.attrs, pane.view_generation,
                                             ToTicks(entry.mtime), entry.size, values))
                continue;
            pane.audio_meta_labels.emplace(source, std::move(values));
        }
    }
    for (const auto& slot : vm.pane_slots) if (slot.focused)
        vm.pane.audio_meta_labels = slot.pane.audio_meta_labels;
}
}
