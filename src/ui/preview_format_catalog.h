#pragma once
// Settings > Quick Look: the read-only "supported formats" card.
// The groups are assembled from the preview format tables (preview_extensions.h)
// plus the rich viewers, so the list cannot drift from what Quick Look opens.
#include "../common/windows_compat.h"
#include <d2d1.h>
#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace pulse::ui {

struct WindowViewModel;
enum class PreviewFormatSource { BuiltIn, Pack, PackOrSystem, Plugin };
struct PreviewFormatState {
    PreviewFormatSource source = PreviewFormatSource::BuiltIn;
    bool available = true;
};
PreviewFormatState PreviewFormatSupport(const WindowViewModel& vm, size_t group, size_t index);

struct PreviewFormatGroup {
    std::wstring name;                   // localized
    std::wstring note;                   // localized, may be empty
    std::vector<std::wstring> extensions;  // without the dot, lower case
};

// Built once per language.
const std::vector<PreviewFormatGroup>& PreviewFormatGroups();
size_t PreviewFormatCount();

// System extensions the card reports on. Bit i of the detection mask = row i.
inline constexpr int kPreviewCodecCount = 4;  // HEIF, HEVC, AV1, WebP
struct PreviewCodecInfo {
    std::wstring name, description;
    const wchar_t* store_id;
};
PreviewCodecInfo PreviewCodec(int index);
// Registry package lookup plus Media Foundation decoder enumeration (video).
// Cached; refresh re-runs it (after the user may have installed something).
// The returned mask has kPreviewCodecsDetected set once detection ran.
// Never blocks: detection runs on a worker thread and invalidates `notify`
// when the mask changes, so call it again on the next paint.
inline constexpr unsigned kPreviewCodecsDetected = 0x80000000u;
unsigned DetectPreviewCodecs(bool refresh, HWND notify);
bool OpenPreviewCodecStore(HWND owner, int index);

// Shared by layout and drawing so the measured height matches the paint.
struct PreviewFormatChip { D2D1_RECT_F rect; size_t group; size_t index; };
struct PreviewFormatRow { D2D1_RECT_F name, note, bounds; };
float LayoutPreviewFormats(const D2D1_RECT_F& area, float scale,
                           std::vector<PreviewFormatChip>* chips,
                           std::vector<PreviewFormatRow>* rows,
                           const std::function<float(std::wstring_view, float)>& measure_note = {});

} // namespace pulse::ui
