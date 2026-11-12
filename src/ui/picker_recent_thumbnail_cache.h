#pragma once

#include "folder_picker_dialog.h"
#include "thumbnail_cache.h"
#include <algorithm>

namespace pulse::ui {

// The same isolated preview-host pipeline as the file list, with independent
// frame interests so drawing the list cannot retire the recent-card requests.
class PickerRecentThumbnailCache {
public:
    void Begin(HWND window, ID2D1DeviceContext* dc) {
        if (window_ != window) {
            if (window_) cache_.RemoveNotifyWindow(window_);
            window_ = window;
            if (window_) cache_.AddNotifyWindow(window_);
        }
        cache_.SetDeviceContext(dc);
        dc_ = dc;
        cache_.AdvanceFrame(window_);
    }

    bool Draw(const PickerRecentFile& file, const D2D1_RECT_F& bounds) {
        if (!dc_ || !file.attributes || (file.attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;
        const float edge = std::max(bounds.right - bounds.left, bounds.bottom - bounds.top);
        const uint32_t pixels = edge <= 128.0f ? 128 : edge <= 256.0f ? 256 : 512;
        return cache_.DrawWallThumbnail(dc_, bounds, file.path, file.attributes, pixels,
            1, file.modified, file.size, 1.0f, nullptr) == PreviewDrawResult::Bitmap;
    }

    bool End() {
        cache_.EndInterestFrame();
        return cache_.TakeUploadFrameRequest();
    }

    uint64_t RetryDeadline() { return cache_.WindowRetryDeadline(window_); }

    void Detach() {
        if (window_) cache_.RemoveNotifyWindow(window_);
        window_ = nullptr;
        dc_ = nullptr;
        cache_.SetDeviceContext(nullptr);
    }

private:
    ThumbnailCache cache_{4ull * 1024 * 1024, 24};
    HWND window_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
};

} // namespace pulse::ui
