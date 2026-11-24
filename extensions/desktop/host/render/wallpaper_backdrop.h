// wallpaper_backdrop.h - 1/4-resolution WIC/preset wallpaper generator, vibrant accent extractor,
// O(1) 3-pass separable box blur baker (Acrylic 28px & Mica 90px), procedural 96x96 noise,
// and non-blocking <= 6 MB hard-capped LRU thumbnail & Shell icon cache.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "host/model/desktop_model.h"
#include "host/render/liquid_glass_effect.h"
#include "host/render/text_contrast.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::desktop::render {

uint32_t ExtractVibrantAccentArgb(const uint8_t* pbgra, uint32_t width, uint32_t height, uint32_t stride) noexcept;

class WallpaperBackdrop {
public:
    static constexpr size_t kMaxCacheBytes = 3u * 1024u * 1024u; // 3 MB hard LRU cap (holds 200+ right-sized icons)

    WallpaperBackdrop() = default;
    ~WallpaperBackdrop();
    WallpaperBackdrop(const WallpaperBackdrop&) = delete;
    WallpaperBackdrop& operator=(const WallpaperBackdrop&) = delete;

    static std::wstring QueryCurrentWallpaperPath();
    static DESKTOP_WALLPAPER_POSITION QueryWallpaperPosition();
    static uint32_t PresetAccentArgb(model::WallpaperPreset preset, bool dark_mode) noexcept;

    // Crisp vector/gradient renderers matching desktop_pack_mockup.html (zero persistent VRAM).
    static void DrawPresetScene(ID2D1Factory* factory, ID2D1RenderTarget* rt,
                                const D2D1_RECT_F& rect, model::WallpaperPreset preset, bool dark_mode);
    static void DrawPhotoTileScene(ID2D1Factory* factory, ID2D1RenderTarget* rt,
                                   const D2D1_RECT_F& rect, int scene_index, bool dark_mode);
    static bool RenderPresetWallpaperFile(const std::wstring& png_path,
                                          uint32_t width_px, uint32_t height_px,
                                          model::WallpaperPreset preset, bool dark_mode);

    // Decodes live Windows wallpaper (or synthesizes preset Bloom/Dusk/Forest/Photo) at (screen_w/4, screen_h/4),
    // computes vibrant accent color, bakes Acrylic & Mica separable box-blur bitmaps + 96x96 noise brush,
    // and retains one bounded CPU wallpaper snapshot for cross-device menus.
    bool LoadForScreen(ID2D1DeviceContext* dc, uint32_t screen_width, uint32_t screen_height,
                       model::WallpaperPreset preset, bool dark_mode = true,
                       const std::wstring& override_wallpaper_path = {});

    bool LoadForScreen(ID2D1DeviceContext* dc, uint32_t screen_width, uint32_t screen_height,
                       const std::wstring& override_wallpaper_path = {}) {
        return LoadForScreen(dc, screen_width, screen_height, model::WallpaperPreset::System, true,
                             override_wallpaper_path);
    }

    void ReleaseDeviceResources() noexcept;
    void ShutdownAsyncWorker() noexcept;

    void SetUseVectorIconsOnly(bool vector_only) noexcept { vector_icons_only_ = vector_only; }
    bool UseVectorIconsOnly() const noexcept { return vector_icons_only_; }
    void SetAsyncReadyCallback(std::function<void()> cb);

    uint32_t QuarterWidth() const noexcept { return quarter_w_; }
    uint32_t QuarterHeight() const noexcept { return quarter_h_; }
    uint32_t ScreenWidth() const noexcept { return screen_w_; }
    uint32_t ScreenHeight() const noexcept { return screen_h_; }
    uint32_t VibrantAccentArgb() const noexcept { return vibrant_accent_argb_; }
    // Mean relative luminance (0..1) of the wallpaper under a rectangle in desktop DIPs, or -1 when unknown.
    // Read from the quarter-resolution CPU snapshot, so it costs a few hundred pixel reads.
    float AverageLuminance(const D2D1_RECT_F& dip_rect) const noexcept;
    // Raw cached wallpaper colors, at most 256 reads; no IO, allocation or GPU readback.
    text_contrast::Samples SampleColors(const D2D1_RECT_F& dip_rect) const noexcept;
    const std::wstring& WallpaperPath() const noexcept { return wallpaper_path_; }
    model::WallpaperPreset ActivePreset() const noexcept { return active_preset_; }
    bool PresetDarkMode() const noexcept { return preset_dark_mode_; }

    ID2D1Bitmap1* QuarterWallpaperBitmap() const;
    ID2D1Bitmap1* AcrylicBlurBitmap() const;
    ID2D1Bitmap1* MicaBlurBitmap() const;
    // Lazy GPU sharp (<= 1600px long edge, zero persistent CPU bytes) & silky frost sources for Liquid Glass.
    ID2D1Bitmap1* GlassSharpBitmap(ID2D1DeviceContext* dc, bool allow_sync_decode,
                                   uint32_t& out_w, uint32_t& out_h);
    ID2D1Bitmap1* GlassFrostBitmap(ID2D1DeviceContext* dc, uint32_t& out_w, uint32_t& out_h) const;
    void ReleaseGlassSource() const noexcept;
    float OriginX() const noexcept { return origin_x_px_; }
    float OriginY() const noexcept { return origin_y_px_; }
    // Maps the backdrop brushes into a DIP scene whose origin sits origin px into the monitor.
    void SetDipMapping(float dip_scale, float origin_x_px, float origin_y_px) noexcept;
    ID2D1BitmapBrush1* AcrylicBrush() const;
    ID2D1BitmapBrush1* MicaBrush() const;
    ID2D1ImageBrush* SampledMicaBrush(ID2D1DeviceContext* dc, bool alternate, bool dark,
                                     uint8_t opacity = 78);
    void SetMonitorScreenOrigin(POINT origin) noexcept { monitor_screen_origin_ = origin; }
    // Optional 1/2-res composite snapshot of the desktop scene (wallpaper + underlying cards)
    // directly behind an opening popup menu so Liquid Glass menus refract underlying cards too (G10).
    void SetMenuUnderlaySnapshot(std::vector<uint8_t> pbgra, uint32_t width, uint32_t height,
                                 RECT screen_rect_px) const noexcept;
    bool DrawMenuMaterial(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_px,
                          POINT popup_screen_px, float radius_px, model::CardMaterial material,
                          bool dark, uint8_t opacity, uint8_t dispersion_level = 1,
                          const model::BeautifySettings* glass_cfg = nullptr) const;
    void ReleaseMenuMaterialCache() const noexcept;
    ID2D1BitmapBrush1* MenuNoiseBrush(ID2D1DeviceContext* dc) const;
    uint64_t MenuUploadCount() const noexcept { return menu_uploads_; }
    // Explicit retained bitmap allocations; effect graphs die after baking.
    size_t MenuCacheBytes() const noexcept {
        const size_t bytes = static_cast<size_t>(quarter_w_) * quarter_h_ * 4u;
        const size_t snap_bytes = static_cast<size_t>(menu_underlay_w_) * menu_underlay_h_ * 4u;
        return (menu_wallpaper_ ? bytes : 0) + (menu_material_brush_ ? bytes : 0) +
               (menu_frost_bitmap_ ? bytes : 0) +
               (menu_underlay_bitmap_ ? snap_bytes : 0) +
               (menu_underlay_frost_ ? snap_bytes / 4u : 0) +
               (menu_noise_bitmap_ ? 128u * 128u * 4u : 0u);
    }
    struct MaterialCacheStats {
        uint64_t card_builds = 0, menu_builds = 0, menu_uploads = 0;
        size_t cpu_wallpaper_bytes = 0, card_bitmap_bytes = 0, menu_bitmap_bytes = 0;
        size_t glass_bitmap_bytes = 0;
    };
    MaterialCacheStats MaterialStats() const noexcept {
        size_t card_bytes = 0;
        for (const auto& brush : sampled_mica_brushes_)
            if (brush) card_bytes += static_cast<size_t>(quarter_w_) * quarter_h_ * 4u;
        const size_t glass_bytes =
            (glass_sharp_ ? static_cast<size_t>(glass_sharp_w_) * glass_sharp_h_ * 4u : 0u) +
            (glass_frost_ ? static_cast<size_t>(quarter_w_) * quarter_h_ * 4u : 0u);
        return {card_material_builds_, menu_material_builds_, menu_uploads_, wallpaper_pixels_.size(),
                card_bytes, MenuCacheBytes(), glass_bytes};
    }
    ID2D1BitmapBrush1* NoiseBrush() const noexcept { return noise_brush_.Get(); }
    ID2D1BitmapBrush1* EnsureNoiseBrush(ID2D1DeviceContext* dc);

    // Non-blocking bounded LRU thumbnail & Shell icon cache (<= 6 MB strict byte cap).
    // Uploads any completed background decodes, checks O(1) LRU cache, and enqueues cache misses
    // onto the background STA worker so the UI thread never blocks on disk or Shell COM.
    void DrainCompletedUploads(ID2D1DeviceContext* dc);
    // `path` may also be a Shell namespace item ("::{CLSID}", e.g. This PC, Recycle Bin): Explorer's own icon.
    // `key_salt` separates states of one item whose icon changes (an empty or full Recycle Bin).
    ID2D1Bitmap1* GetItemBitmap(ID2D1DeviceContext* dc, const std::wstring& path,
                                bool is_folder, uint32_t target_px = 64, std::wstring_view key_salt = {});
    ID2D1Bitmap1* GetPhotoTileBitmap(ID2D1DeviceContext* dc, const std::wstring& image_path,
                                     uint32_t max_w = 424, uint32_t max_h = 208);
    // Uses only an already-uploaded photo's bounded CPU thumbnail, with centered Cover crop.
    text_contrast::PhotoSamples SamplePhotoColors(const std::wstring& image_path,
        const D2D1_RECT_F& tile_rect, const D2D1_RECT_F& text_rect,
        uint32_t max_w = 424, uint32_t max_h = 208) const;
    ID2D1Bitmap1* CacheRgbaBitmap(ID2D1DeviceContext* dc, const std::wstring& cache_key,
                                  const uint8_t* pbgra_pixels, uint32_t width, uint32_t height);

    void ClearThumbnailCache() noexcept;
    size_t CachedBytes() const noexcept { return cached_bytes_; }
    size_t CachedEntryCount() const noexcept { return cache_map_.size(); }

private:
    void ApplyBrushTransforms() const noexcept;
    bool EnsureAcrylicBlur() const;
    bool EnsureMicaBlur() const;
    struct DecodeJob {
        std::wstring key;
        std::wstring path;
        bool is_folder = false;
        bool is_photo = false;
        bool shared_type_icon = false;
        uint32_t max_w = 64;
        uint32_t max_h = 64;
        bool is_wallpaper_sharp = false;
    };

    struct DecodedUpload {
        std::wstring key;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pbgra;
        bool failed = false;
    };

    IWICImagingFactory* EnsureWicFactory();
    void EnqueueAsyncDecode(DecodeJob job);
    void AsyncWorkerLoop();

    ID2D1Bitmap1* InsertIntoLru(const std::wstring& key, Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp, size_t byte_cost,
        std::unique_ptr<text_contrast::PhotoGrid> photo = {});
    void EvictToBudget(size_t incoming_bytes) noexcept;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    mutable Microsoft::WRL::ComPtr<ID2D1DeviceContext> owner_dc_;
    std::wstring wallpaper_path_;
    model::WallpaperPreset active_preset_ = model::WallpaperPreset::System;
    bool preset_dark_mode_ = true;
    bool vector_icons_only_ = false;
    float dip_scale_ = 1.0f;
    float origin_x_px_ = 0.0f;
    float origin_y_px_ = 0.0f;
    uint32_t screen_w_ = 0;
    uint32_t screen_h_ = 0;
    uint32_t quarter_w_ = 0;
    uint32_t quarter_h_ = 0;
    uint32_t vibrant_accent_argb_ = 0xFF4CC2FFu;

    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> wallpaper_quarter_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> acrylic_blur_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> mica_blur_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> glass_sharp_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> glass_frost_;
    mutable uint32_t glass_sharp_w_ = 0;
    mutable uint32_t glass_sharp_h_ = 0;
    mutable bool glass_sharp_pending_ = false;
    mutable bool glass_sharp_failed_ = false;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> noise_bitmap_;

    mutable Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> acrylic_brush_;
    mutable Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> mica_brush_;
    Microsoft::WRL::ComPtr<ID2D1ImageBrush> sampled_mica_brushes_[4];
    bool sampled_mica_failed_[4]{};
    uint64_t card_material_builds_ = 0;
    std::vector<uint8_t> wallpaper_pixels_;
    POINT monitor_screen_origin_{};
    mutable Microsoft::WRL::ComPtr<ID2D1DeviceContext> menu_dc_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> menu_wallpaper_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> menu_frost_bitmap_;
    mutable std::vector<uint8_t> menu_underlay_pixels_;
    mutable uint32_t menu_underlay_w_ = 0, menu_underlay_h_ = 0;
    mutable RECT menu_underlay_screen_px_{};
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> menu_underlay_bitmap_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> menu_underlay_frost_;
    mutable LiquidGlassPassPool menu_glass_pool_;
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap1> menu_noise_bitmap_;
    mutable Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> menu_noise_brush_;
    mutable bool menu_noise_failed_ = false;
    mutable Microsoft::WRL::ComPtr<ID2D1ImageBrush> menu_material_brush_;
    mutable uint64_t menu_material_builds_ = 0, menu_uploads_ = 0;
    mutable model::CardMaterial menu_material_ = model::CardMaterial::FollowHost;
    mutable bool menu_dark_ = false;
    mutable bool menu_failed_ = false;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> noise_brush_;

    struct CacheNode {
        std::wstring key;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        size_t bytes = 0;
        std::unique_ptr<text_contrast::PhotoGrid> photo;
    };
    std::list<CacheNode> lru_list_;
    std::unordered_map<std::wstring, std::list<CacheNode>::iterator> cache_map_;
    std::unordered_set<std::wstring> failed_keys_;
    size_t cached_bytes_ = 0;

    std::mutex async_mtx_;
    std::condition_variable async_cv_;
    bool worker_running_ = false;
    bool worker_stop_ = false;
    std::thread worker_thread_;
    std::unordered_set<std::wstring> pending_keys_;
    std::deque<DecodeJob> decode_queue_;
    std::deque<DecodedUpload> completed_uploads_;
    std::function<void()> on_async_ready_;
};

} // namespace pulse::desktop::render
