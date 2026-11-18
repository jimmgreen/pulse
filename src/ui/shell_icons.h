// shell_icons.h — Windows Shell image-list icons for file rows.
#pragma once
#include "ui_compositor.h"
#include "d2d_device_binding.h"
#include "icon_artwork_bounds.h"
#include "repaint_targets.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <unordered_set>
#include <vector>
#include <deque>
#include <optional>

namespace pulse::ui {

class ShellIconCache {
public:
    ShellIconCache();
    ~ShellIconCache();
    ShellIconCache(const ShellIconCache&) = delete;
    ShellIconCache& operator=(const ShellIconCache&) = delete;

    // Loads the shell's icon cache on a short-lived worker thread. The first
    // system image list icon otherwise initializes it on the UI thread in
    // the middle of the first frame (about 100 ms).
    static void PrewarmSystemImageList();

    void SetDeviceContext(ID2D1DeviceContext* dc);
    void SetScale(float scale);
    // Set replaces every window that repaints when icons land; Add/Remove let
    // the windows sharing this cache (RenderCaches) register independently.
    void SetNotifyWindow(HWND hwnd);
    void AddNotifyWindow(HWND hwnd);
    void RemoveNotifyWindow(HWND hwnd);
    void Reset();

    // Returns false while native icons are loading or the device is unavailable.
    // opacity lets callers fade the icon (hidden entries, cut items).
    bool Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
              const std::wstring& path, const std::wstring& name,
              bool is_dir, DWORD attrs, float opacity = 1.0f);

    // Icon bitmap at (about) desired_dips, or nullptr while unresolved —
    // caller draws its fallback. Extraction/conversion runs on the worker.
    // Lets owners apply their own opacity
    // and transforms (tray card deck) instead of the fixed Draw path.
    ID2D1Bitmap* BitmapFor(const std::wstring& path, const std::wstring& name,
                           bool is_dir, DWORD attrs, float desired_dips);

    // Unlike BitmapFor, does not request missing icons and can reuse other sizes: the
    // best bitmap already converted (requested size first, then the nearest
    // other size, then the type's generic icon), or nullptr. For animation
    // frames, where a synchronous HICON -> D2D conversion (1-7 ms each for
    // per-file icons) would stall the glide.
    ID2D1Bitmap* CachedBitmapFor(const std::wstring& path, const std::wstring& name,
                                 bool is_dir, DWORD attrs, float desired_dips);

    // The path's own icon only, never a generic fallback: nullptr while the
    // lookup is pending or when it failed (the caller keeps its own artwork).
    // Also takes the program icons of 网盘 entries: "shell:::{CLSID}" (a This
    // PC launcher) and "::{CLSID}" (a navigation-pane root) resolve to the
    // icon the program registered for that CLSID, as Explorer shows it.
    ID2D1Bitmap* ExactBitmapFor(const std::wstring& path, float desired_dips);

    // Matches CachedBitmapFor's selected bitmap. Unknown/empty artwork uses
    // the full normalized rectangle; this only inspects cached metadata.
    D2D1_RECT_F CachedArtworkBounds(const std::wstring& path, const std::wstring& name,
                                   bool is_dir, DWORD attrs, float desired_dips);

    // For the same bitmap: nonzero when the icon has no image of the size
    // asked for, only a smaller one of this many pixels a side, which is drawn
    // at most twice that (IconDrawSide) instead of blurred across the cell.
    uint32_t CachedNativeSide(const std::wstring& path, const std::wstring& name,
                              bool is_dir, DWORD attrs, float desired_dips);

    // Queues the icon BitmapFor would return for a background conversion
    // (image-list extraction + WIC pixel conversion on the worker thread);
    // the UI thread later only uploads the finished pixels. Cheap to call
    // every frame; the window is invalidated when a conversion lands.
    void Prefetch(const std::wstring& path, const std::wstring& name,
                  bool is_dir, DWORD attrs, float desired_dips);

private:
    friend struct ShellIconCacheTestAccess;
    static int ImageListId(float desired_pixels) noexcept;
    int GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs);
    void RequestExact(const std::wstring& path);
    ID2D1Bitmap* BitmapForIndex(int index, int list_id);
    void RequestConversion(uint64_t key);
    // Worker-converted pixels for key, uploaded to a D2D bitmap and cached.
    ID2D1Bitmap* UploadReady(uint64_t key);
    ID2D1Bitmap* StoreBitmap(uint64_t key, ComPtr<ID2D1Bitmap> bitmap,
                            IconArtworkBounds bounds = {});
    std::optional<uint64_t> CachedBitmapKey(const std::wstring& path,
        const std::wstring& name, bool is_dir, float desired_dips, bool upload_ready);
    void StartWorkerLocked();
    void WorkerLoop();
    static bool NeedsExactIcon(const std::wstring& name, bool is_dir,
                               const std::wstring& path);

    D2DDeviceBinding device_binding_;
    ID2D1DeviceContext* dc_ = nullptr;  // device_binding_'s private context
    RepaintTargets repaint_;
    float scale_ = 1.0f;
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::unordered_map<uint64_t, IconArtworkBounds> artwork_bounds_;
    std::unordered_map<std::wstring, int> generic_index_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::wstring> queue_;
    std::unordered_set<std::wstring> queued_;
    std::unordered_map<std::wstring, int> exact_index_;
    std::unordered_map<std::wstring, ULONGLONG> retry_after_;
    std::unordered_map<std::wstring, uint64_t> last_used_;
    // Background icon conversions, keyed like bitmaps_ (list id << 32 | index).
    struct IconPixels {
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> data;  // 32bpp premultiplied BGRA
        IconArtworkBounds bounds;
    };
    std::deque<uint64_t> convert_queue_;
    std::unordered_set<uint64_t> convert_pending_;
    std::unordered_map<uint64_t, IconPixels> ready_pixels_;
    uint64_t access_clock_ = 0;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace pulse::ui
