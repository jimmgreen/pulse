// shell_icons.cpp
#include "shell_icons.h"
#include "../common/path_utils.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wincodec.h>
#include <cwctype>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse::ui {

namespace {

// Shell handlers can expose an icon per path (not just per extension). Keep
// those maps bounded because a long scroll through executable/link files
// otherwise retains every path seen during the session.
constexpr size_t kExactIndexLimit = 4096;
constexpr size_t kBitmapCacheLimit = 128;
// Marks a cache entry whose icon already carries the link overlay. The list id
// and image index occupy the low 32 bits, so the sign bit is free.
constexpr uint64_t kLinkIconBit = 1ull << 63;

std::wstring LowerExt(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

constexpr size_t kReadyPixelsLimit = 256;

uint64_t IconKey(int list_id, int index) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(list_id)) << 32) |
        static_cast<uint32_t>(index);
}

// Worker thread: image-list icon -> 32bpp premultiplied BGRA pixels. The
// thread owns its own image-list pointers and WIC factory.
bool ConvertIconPixels(uint64_t key, std::unordered_map<int, IImageList*>& lists,
                       ComPtr<IWICImagingFactory>& wic, UINT& width, UINT& height,
                       std::vector<uint8_t>& data) {
    const int list_id = static_cast<int>(static_cast<uint32_t>(key >> 32));
    const int index = static_cast<int>(static_cast<uint32_t>(key & 0xffffffffu));
    IImageList* list = nullptr;
    if (const auto it = lists.find(list_id); it != lists.end()) {
        list = it->second;
    } else {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        if (FAILED(SHGetImageList(list_id, IID_IImageList, reinterpret_cast<void**>(&list)))) list = nullptr;
        lists.emplace(list_id, list);
    }
    if (!list) return false;
    if (!wic.get() && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(&wic)))) return false;
    if (!wic.get()) return false;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) return false;
    ComPtr<IWICBitmap> source;
    const HRESULT hr = wic->CreateBitmapFromHICON(icon, &source);
    DestroyIcon(icon);
    if (FAILED(hr) || !source.get()) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) || !converter.get()) return false;
    if (FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) return false;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > 1024 || height > 1024) return false;
    data.resize(static_cast<size_t>(width) * height * 4u);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(data.size()),
                                           data.data()));
}

// SHGetIconOverlayIndex hands back an overlay slot, not an image index, so the
// link badge has no image of its own to look up. The only way to get it is to
// let the image list composite the overlay while drawing the base icon.
int LinkOverlaySlot() {
    static const int slot = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    return slot;
}

// 0..255 alpha -> 0..65535, then a rounded high-multiply back to 8 bits.
void PremultiplyBgra(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t alpha = pixels[i + 3];
        if (alpha == 0xFF) continue;
        const uint32_t scale = alpha * 0x0101u;
        for (size_t channel = 0; channel < 3; ++channel)
            pixels[i + channel] = static_cast<uint8_t>(
                (pixels[i + channel] * scale + 0x8080u) >> 16);
    }
}

} // namespace

ShellIconCache::ShellIconCache() = default;

ShellIconCache::~ShellIconCache() {
    Reset();
}

void ShellIconCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (dc_ == dc) return;
    dc_ = dc;
    bitmaps_.clear();
}

void ShellIconCache::SetScale(float scale) {
    if (std::abs(scale_ - scale) <= 0.001f) return;
    scale_ = scale;
}

void ShellIconCache::SetNotifyWindow(HWND hwnd) {
    hwnd_ = hwnd;
    if (hwnd) {
        GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        GenericIndex(L"", false, FILE_ATTRIBUTE_NORMAL);
    }
}

void ShellIconCache::Reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    bitmaps_.clear();
    generic_index_.clear();
    for (auto& [_, list] : image_lists_)
        if (list) list->Release();
    image_lists_.clear();
    wic_.reset();
    dc_ = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    exact_index_.clear();
    retry_after_.clear();
    last_used_.clear();
    queued_.clear();
    while (!queue_.empty()) queue_.pop();
    convert_queue_.clear();
    convert_pending_.clear();
    ready_pixels_.clear();
}

int ShellIconCache::ImageListId(float desired_pixels) noexcept {
    if (desired_pixels <= 16.0f) return SHIL_SMALL;
    if (desired_pixels <= 32.0f) return SHIL_LARGE;
    if (desired_pixels <= 64.0f) return SHIL_EXTRALARGE;
    return SHIL_JUMBO;
}

IImageList* ShellIconCache::EnsureImageList(int id) {
    if (const auto cached = image_lists_.find(id); cached != image_lists_.end())
        return cached->second;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(id, IID_IImageList, reinterpret_cast<void**>(&list))) || !list)
        return nullptr;
    image_lists_.emplace(id, list);
    return list;
}

bool ShellIconCache::EnsureWic() {
    if (wic_.get()) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic_))) && wic_.get();
}

bool ShellIconCache::NeedsExactIcon(const std::wstring& name, bool is_dir,
                                    const std::wstring& path) {
    (void)name;
    (void)is_dir;
    // Folder customizations and registered icon handlers are path-dependent.
    return !path.empty();
}

int ShellIconCache::GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs) {
    std::wstring key = is_dir ? L"<dir>" : LowerExt(name);
    if (key.empty()) key = L"<file>";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto cached = generic_index_.find(key); cached != generic_index_.end())
            return cached->second;
    }
    (void)attrs;
    RequestExact(std::wstring(1, L'\x1f') + L"GEN:" + key);
    return -1;
}

void ShellIconCache::RequestExact(const std::wstring& path) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exact_index_.contains(path)) {
            last_used_[path] = ++access_clock_;
            return;
        }
        if (queued_.contains(path)) return;
        if (queue_.size() >= kExactIndexLimit) {
            queued_.erase(queue_.front());
            queue_.pop();
        }
        if (const auto retry = retry_after_.find(path);
            retry != retry_after_.end() && GetTickCount64() < retry->second) return;
        queued_.insert(path);
        queue_.push(path);
        StartWorkerLocked();
    }
    cv_.notify_one();
}

void ShellIconCache::StartWorkerLocked() {
    if (running_) return;
    running_ = true;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this] { WorkerLoop(); });
}

void ShellIconCache::WorkerLoop() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::unordered_map<int, IImageList*> worker_lists;
    ComPtr<IWICImagingFactory> worker_wic;
    for (;;) {
        std::wstring path;
        uint64_t convert_key = 0;
        bool convert = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !running_ || !queue_.empty() || !convert_queue_.empty(); });
            if (!running_) break;
            if (!queue_.empty()) {
                // Index lookups first: they are cheap and unlock conversions.
                path = std::move(queue_.front());
                queue_.pop();
            } else {
                convert_key = convert_queue_.front();
                convert_queue_.pop_front();
                convert = true;
            }
        }
        if (convert) {
            IconPixels pixels;
            const bool ok = ConvertIconPixels(convert_key, worker_lists, worker_wic,
                                              pixels.width, pixels.height, pixels.data);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                convert_pending_.erase(convert_key);
                if (ok) {
                    if (ready_pixels_.size() >= kReadyPixelsLimit) ready_pixels_.erase(ready_pixels_.begin());
                    ready_pixels_[convert_key] = std::move(pixels);
                }
            }
            if (ok) {
                if (const HWND hwnd = hwnd_.load()) InvalidateRect(hwnd, nullptr, FALSE);
            }
            continue;
        }
        SHFILEINFOW info{};
        int index = -1;
        const std::wstring prefix = std::wstring(1, L'\x1f') + L"GEN:";
        if (path.starts_with(prefix)) {
            const std::wstring key = path.substr(prefix.size());
            const bool dir = key == L"<dir>";
            const std::wstring query = dir ? L"dummy" : (key == L"<file>" ? L"dummy" : L"dummy" + key);
            DWORD useAttrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (SHGetFileInfoW(query.c_str(), useAttrs, &info, sizeof(info),
                               SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES))
                index = info.iIcon;
            std::lock_guard<std::mutex> lock(mutex_);
            if (index >= 0) {
                if (generic_index_.size() >= 512) {
                    const auto victim = std::find_if(generic_index_.begin(), generic_index_.end(),
                        [](const auto& item) { return item.first != L"<dir>" && item.first != L"<file>"; });
                    if (victim != generic_index_.end()) generic_index_.erase(victim);
                }
                generic_index_[key] = index;
            }
        } else {
            const UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
            const std::wstring query = ShellPath(path);
            if (SHGetFileInfoW(query.c_str(), 0, &info, sizeof(info), flags)) {
                index = info.iIcon;
                if (info.hIcon) DestroyIcon(info.hIcon);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queued_.erase(path);
            if (index >= 0) {
                retry_after_.erase(path);
                if (!path.starts_with(prefix)) {
                    if (exact_index_.size() >= kExactIndexLimit) {
                        const auto oldest = std::min_element(last_used_.begin(), last_used_.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; });
                        if (oldest != last_used_.end()) {
                            exact_index_.erase(oldest->first);
                            last_used_.erase(oldest);
                        }
                    }
                    exact_index_[path] = index;
                    last_used_[path] = ++access_clock_;
                }
            } else {
                if (retry_after_.size() >= kExactIndexLimit) retry_after_.erase(retry_after_.begin());
                retry_after_[path] = GetTickCount64() + 2000;
            }
        }
        if (const HWND hwnd = hwnd_.load()) InvalidateRect(hwnd, nullptr, FALSE);
    }
    for (auto& [_, list] : worker_lists)
        if (list) list->Release();
    worker_wic.reset();
    if (SUCCEEDED(com_hr)) CoUninitialize();
}

ComPtr<ID2D1Bitmap> ShellIconCache::BitmapFromIcon(HICON icon) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (!icon || !dc_ || !EnsureWic()) return bitmap;
    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wic_->CreateBitmapFromHICON(icon, &wicBitmap)) || !wicBitmap.get()) {
        return bitmap;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(&converter)) || !converter.get()) {
        return bitmap;
    }
    if (FAILED(converter->Initialize(wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return bitmap;
    }
    dc_->CreateBitmapFromWicBitmap(converter.get(), nullptr, &bitmap);
    return bitmap;
}

int ShellIconCache::IconIndex(const std::wstring& path, const std::wstring& name,
                              bool is_dir, DWORD attrs, bool touch) {
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir, path) && !path.empty();
    if (exact) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = exact_index_.find(path); it != exact_index_.end()) {
            index = it->second;
            if (touch) last_used_[path] = ++access_clock_;
        }
    }
    if (index < 0 && exact) RequestExact(path);
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    return index;
}

ID2D1Bitmap* ShellIconCache::BitmapForIndex(int index, int list_id) {
    if (index < 0 || !dc_) return nullptr;
    IImageList* image_list = EnsureImageList(list_id);
    if (!image_list) return nullptr;
    const uint64_t key = IconKey(list_id, index);
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.get();
    if (ID2D1Bitmap* ready = UploadReady(key)) return ready;
    HICON icon = nullptr;
    if (FAILED(image_list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bitmap = BitmapFromIcon(icon);
    DestroyIcon(icon);
    return StoreBitmap(key, std::move(bitmap));
}

ID2D1Bitmap* ShellIconCache::StoreBitmap(uint64_t key, ComPtr<ID2D1Bitmap> bitmap) {
    if (!bitmap.get()) return nullptr;
    ID2D1Bitmap* raw = bitmap.get();
    if (bitmaps_.size() >= kBitmapCacheLimit) bitmaps_.erase(bitmaps_.begin());
    bitmaps_[key] = std::move(bitmap);
    return raw;
}

ID2D1Bitmap* ShellIconCache::UploadReady(uint64_t key) {
    if (!dc_) return nullptr;
    IconPixels pixels;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = ready_pixels_.find(key);
        if (it == ready_pixels_.end()) return nullptr;
        pixels = std::move(it->second);
        ready_pixels_.erase(it);
    }
    ComPtr<ID2D1Bitmap> bitmap;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.data.data(),
                                 pixels.width * 4u, props, &bitmap))) return nullptr;
    return StoreBitmap(key, std::move(bitmap));
}

void ShellIconCache::Prefetch(const std::wstring& path, const std::wstring& name,
                              bool is_dir, DWORD attrs, float desired_dips) {
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir, path) && !path.empty();
    if (exact) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = exact_index_.find(path); it != exact_index_.end()) index = it->second;
    }
    if (index < 0 && exact) RequestExact(path);  // converted on a later call
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    if (index < 0) return;
    const uint64_t key = IconKey(ImageListId(desired_dips), index);
    if (bitmaps_.contains(key)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_pixels_.contains(key) || convert_pending_.contains(key)) return;
        convert_pending_.insert(key);
        convert_queue_.push_back(key);
        StartWorkerLocked();
    }
    cv_.notify_one();
}

bool ShellIconCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                          const std::wstring& path, const std::wstring& name,
                          bool is_dir, DWORD attrs, bool link_overlay) {
    if (!dc) return false;
    const float desired = std::max(dest.right - dest.left, dest.bottom - dest.top);
    ID2D1Bitmap* bitmap = link_overlay
        ? LinkBitmapFor(path, name, is_dir, attrs, desired)
        : BitmapFor(path, name, is_dir, attrs, desired);
    if (!bitmap) return false;
    dc->DrawBitmap(bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   nullptr, nullptr);
    return true;
}


ID2D1Bitmap* ShellIconCache::BitmapFor(const std::wstring& path, const std::wstring& name,
                                       bool is_dir, DWORD attrs, float desired_dips) {
    const int index = IconIndex(path, name, is_dir, attrs, true);
    const int list_id = ImageListId(desired_dips);
    if (auto* bitmap = BitmapForIndex(index, list_id)) return bitmap;
    return BitmapForIndex(GenericIndex(L"", is_dir, attrs), list_id);
}

ID2D1Bitmap* ShellIconCache::LinkBitmapFor(const std::wstring& path, const std::wstring& name,
                                           bool is_dir, DWORD attrs, float desired_dips) {
    const int index = IconIndex(path, name, is_dir, attrs, true);
    const int list_id = ImageListId(desired_dips);
    if (auto* bitmap = LinkBitmapForIndex(index, list_id)) return bitmap;
    return LinkBitmapForIndex(GenericIndex(L"", is_dir, attrs), list_id);
}

// A link icon is the ordinary icon with the Shell's link badge composited on
// top, which only the image list can do (see LinkOverlaySlot). The composited
// surface is uploaded and cached like any other icon, under the link key.
ID2D1Bitmap* ShellIconCache::LinkBitmapForIndex(int index, int list_id) {
    if (index < 0 || !dc_) return nullptr;
    IImageList* image_list = EnsureImageList(list_id);
    if (!image_list) return nullptr;
    const int overlay_slot = LinkOverlaySlot();
    if (overlay_slot < 0) return nullptr;
    const uint64_t key = IconKey(list_id, index) | kLinkIconBit;
    if (const auto it = bitmaps_.find(key); it != bitmaps_.end()) return it->second.get();
    int width = 0, height = 0;
    if (FAILED(image_list->GetIconSize(&width, &height)) || width <= 0 || height <= 0)
        return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down, like the rest of the pipeline
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dib || !memory) {
        if (dib) DeleteObject(dib);
        if (memory) DeleteDC(memory);
        return nullptr;
    }
    const HGDIOBJ previous = SelectObject(memory, dib);
    memset(pixels, 0, static_cast<size_t>(width) * height * 4u);
    const BOOL drawn = ImageList_Draw(reinterpret_cast<HIMAGELIST>(image_list), index, memory,
                                      0, 0, ILD_TRANSPARENT | INDEXTOOVERLAYMASK(overlay_slot));
    SelectObject(memory, previous);
    DeleteDC(memory);
    std::vector<uint8_t> composited;
    if (drawn)
        composited.assign(static_cast<uint8_t*>(pixels),
                          static_cast<uint8_t*>(pixels) +
                              static_cast<size_t>(width) * height * 4u);
    DeleteObject(dib);
    if (!drawn) return nullptr;
    PremultiplyBgra(composited);
    ComPtr<ID2D1Bitmap> bitmap;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(dc_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width),
                                                static_cast<UINT32>(height)),
                                    composited.data(), static_cast<UINT32>(width) * 4u, props,
                                    &bitmap)))
        return StoreBitmap(key, std::move(bitmap));
    return nullptr;
}

ID2D1Bitmap* ShellIconCache::CachedBitmapFor(const std::wstring& path, const std::wstring& name,
                                             bool is_dir, DWORD attrs, float desired_dips) {
    (void)attrs;  // generic icons are keyed by extension only (see GenericIndex)
    int exact = -1;
    if (NeedsExactIcon(name, is_dir, path) && !path.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = exact_index_.find(path); it != exact_index_.end()) exact = it->second;
    }
    int generic = -1;
    {
        std::wstring key = is_dir ? L"<dir>" : LowerExt(name);
        if (key.empty()) key = L"<file>";
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = generic_index_.find(key); it != generic_index_.end()) generic = it->second;
    }
    // Requested size first, then larger sizes (scale down cleanly), then smaller.
    const int wanted = ImageListId(desired_dips);
    static constexpr int kOrder[] = { SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO };
    int at = 0;
    for (int i = 0; i < 4; ++i) if (kOrder[i] == wanted) at = i;
    int lists[4];
    int n = 0;
    lists[n++] = wanted;
    for (int i = at + 1; i < 4; ++i) lists[n++] = kOrder[i];
    for (int i = at - 1; i >= 0; --i) lists[n++] = kOrder[i];
    for (const int index : { exact, generic }) {
        if (index < 0) continue;
        for (int i = 0; i < n; ++i) {
            const uint64_t key = IconKey(lists[i], index);
            if (const auto it = bitmaps_.find(key); it != bitmaps_.end() && it->second.get())
                return it->second.get();
            if (ID2D1Bitmap* ready = UploadReady(key)) return ready;
        }
    }
    return nullptr;
}
} // namespace pulse::ui