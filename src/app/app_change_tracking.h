#pragma once
#include "../index/change_tracking_client.h"
#include "../ui/ui_renderer.h"
#include <unordered_map>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
struct ChangeDetailPending {
    app::Tab* tab = nullptr;
    std::wstring path;
    uint64_t generation = 0;
    bool append = false;
};
struct ChangeTrackingUi {
    index::ChangeTrackingClient client;
    std::unordered_map<std::wstring, index::ChangeSummary> summaries;
    std::vector<std::wstring> visible_paths, requested_paths;
    std::unordered_map<uint32_t, ChangeDetailPending> details;
    uint32_t next_request = 1, summary_request = 0;
    bool summary_pending = false;
    uint64_t last_query = 0, last_detail_refresh = 0;
    bool enabled = false;
    int days = 7;
    ui::ChangePopover popover;
    std::wstring hover_path;
    uint64_t hover_deadline = 0;
};
void StartChangeTracking(AppState& s);
bool TickChangeTracking(AppState& s);
void ReceiveChangeTracking(AppState& s, uint32_t request, bool details);
void ApplyChangeSummaries(AppState& s, uint32_t request, const index::ChangeResponse& result);
void ApplyChangeDetails(AppState& s, uint32_t request, const index::ChangeResponse& result);
void FillChangePane(AppState& s, app::Tab& tab, ui::PaneViewModel& pane,
                    const D2D1_RECT_F& bounds);
void FillChangePopover(AppState& s, ui::WindowViewModel& vm);
void OpenChangeView(AppState& s, const std::wstring& root);
void LoadChangeView(AppState& s, app::Tab& tab, const std::wstring& root, bool append = false, bool refresh = false);
bool HandleChangeClick(AppState& s, const ui::HitTestResult& hit, POINT point);
void UpdateChangeHover(AppState& s, const ui::HitTestResult& hit, POINT point);
}
