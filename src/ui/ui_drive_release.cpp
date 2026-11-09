#include "ui_renderer.h"
#include "quick_preview_window.h"
#include "../common/volume_path.h"
#include <algorithm>
namespace pulse::ui {
std::vector<std::shared_future<void>> MainRenderer::SuspendVolume(const std::wstring& root) {
    if (std::find(suspended_volumes_.begin(), suspended_volumes_.end(), root) == suspended_volumes_.end())
        suspended_volumes_.push_back(root);
    auto pending = hover_video_ ? hover_video_->SuspendVolume(root) : std::vector<std::shared_future<void>>{};
    if (path::OnVolume(hover_video_path_, root)) {
        hover_video_path_.clear();
        hover_video_failed_ = false;
        hover_frame_serial_ = ~0ull;
    }
    return pending;
}
void MainRenderer::ResumeVolume(const std::wstring& root) {
    std::erase(suspended_volumes_, root);
}
std::vector<std::shared_future<void>> QuickPreviewWindow::SuspendVolume(const std::wstring& root) {
    if (std::find(suspended_volumes_.begin(), suspended_volumes_.end(), root) == suspended_volumes_.end())
        suspended_volumes_.push_back(root);
    auto pending = video_.SuspendVolume(root);
    pending.push_back(handler_.SuspendVolume(root));
    pending.push_back(thumbnails_.SuspendVolume(root));
    if (path::OnVolume(item_.path, root)) Close();
    return pending;
}
void QuickPreviewWindow::ResumeVolume(const std::wstring& root) {
    std::erase(suspended_volumes_, root);
    handler_.ResumeVolume(root);
    thumbnails_.ResumeVolume(root);
}
}
