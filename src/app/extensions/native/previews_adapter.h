#pragma once
#include "../../../../sdk/native/include/pulse/preview.h"
#include <windows.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../ui_tree.h"
namespace pulse::extensions::native {
inline constexpr size_t kMaxSessionPreviewWorkers = 2;
struct PreviewEntry {
    std::wstring path;
    uint64_t size = 0, packed = 0, modified = 0;
    bool directory = false, encrypted = false;
};
struct PreviewResult {
    uint32_t kind = 0;
    std::shared_ptr<const UiTree> model;
    std::wstring format;
    std::shared_ptr<const std::wstring> archive_text;
    bool incomplete = false;
    std::vector<PreviewEntry> entries;
    uint32_t width = 0, height = 0, stride = 0;
    std::vector<uint8_t> pixels;
};
enum class PreviewStatus { Unavailable, Pending, Ready, Failed };
struct PreviewResponse {
    PreviewStatus status = PreviewStatus::Unavailable;
    std::shared_ptr<const PreviewResult> result;
    PulseStatus error = PULSE_ERROR_UNAVAILABLE;
};
class PreviewsAdapter {
public:
    PreviewsAdapter(HWND owner, std::function<bool()> live,
                    std::function<void()> wake = {}, std::string package_id = {});
    ~PreviewsAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    void Poll() noexcept;
    bool CallbackActive() const noexcept;
    size_t ActiveWorkerCount() const noexcept;
    size_t QueuedCount() const noexcept;
    static PreviewResponse Request(HWND owner, const std::wstring& path, uint64_t modified,
        uint64_t size, uint32_t pixels, uint64_t surface, HWND notify = nullptr);
    static bool Available(HWND owner, const std::wstring& path);
    static void Cancel(HWND owner, uint64_t surface);
    // Grid thumbnails from providers registered with PULSE_PREVIEW_PROVIDER_THUMBNAIL.
    // Any thread (the thumbnail decode lanes); runs the provider on the calling thread
    // within the session's worker budget. NotHandled: no such provider. Busy: no worker
    // slot within ~2 s. Failed: the provider produced no usable bitmap.
    enum class ThumbnailStatus { NotHandled, Ready, Busy, Failed };
    struct ThumbnailBitmap {
        std::vector<uint8_t> pixels;  // premultiplied BGRA, top-down, width * 4 per row
        uint32_t width = 0, height = 0;
    };
    static ThumbnailStatus Thumbnail(const std::wstring& path, uint64_t modified, uint64_t size,
        uint32_t pixels, const std::function<bool()>& cancelled, ThumbnailBitmap& out) noexcept;
    static bool ThumbnailAvailable(const std::wstring& path) noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}