// shell_icons.h — Windows Shell image-list icons for file rows.
#pragma once
#include "ui_compositor.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <unordered_set>

struct IImageList;
struct IWICImagingFactory;

namespace pulse::ui {

class ShellIconCache {
public:
    ShellIconCache();
    ~ShellIconCache();
    ShellIconCache(const ShellIconCache&) = delete;
    ShellIconCache& operator=(const ShellIconCache&) = delete;

    void SetDeviceContext(ID2D1DeviceContext2* dc);
    void SetScale(float scale);
    void SetNotifyWindow(HWND hwnd);
    void Reset();

    // Returns false when the caller should draw the glyph fallback.
    bool Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
              const std::wstring& path, const std::wstring& name,
              bool is_dir, DWORD attrs);

    // Icon bitmap at (about) desired_dips, or nullptr while unresolved —
    // caller draws the glyph fallback. Lets owners apply their own opacity
    // and transforms (tray card deck) instead of the fixed Draw path.
    ID2D1Bitmap* BitmapFor(const std::wstring& path, const std::wstring& name,
                           bool is_dir, DWORD attrs, float desired_dips);

private:
    static int ImageListId(float desired_pixels) noexcept;
    IImageList* EnsureImageList(int list_id);
    bool EnsureWic();
    int GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs);
    void RequestExact(const std::wstring& path);
    ID2D1Bitmap* BitmapForIndex(int index, int list_id);
    ComPtr<ID2D1Bitmap> BitmapFromIcon(HICON icon);
    void WorkerLoop();
    static bool NeedsExactIcon(const std::wstring& name, bool is_dir,
                               const std::wstring& path);

    ID2D1DeviceContext2* dc_ = nullptr;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    std::unordered_map<int, IImageList*> image_lists_;
    ComPtr<IWICImagingFactory> wic_;
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::unordered_map<std::wstring, int> generic_index_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::wstring> queue_;
    std::unordered_set<std::wstring> queued_;
    std::unordered_map<std::wstring, int> exact_index_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace pulse::ui
