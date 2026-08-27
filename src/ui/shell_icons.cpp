// shell_icons.cpp
#include "shell_icons.h"
#include "../common/path_utils.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wincodec.h>
#include <cwctype>
#include <algorithm>
#include <cmath>

namespace pulse::ui {

namespace {

// Shell handlers can expose an icon per path (not just per extension). Keep
// those maps bounded because a long scroll through executable/link files
// otherwise retains every path seen during the session.
constexpr size_t kExactIndexLimit = 256;
constexpr size_t kGenericIndexLimit = 64;
constexpr size_t kBitmapCacheLimit = 128;

std::wstring LowerExt(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsPerFileIconExt(const std::wstring& ext) {
    return ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".cur" ||
           ext == L".dll" || ext == L".scr" || ext == L".cpl" || ext == L".msc" ||
           ext == L".url" || ext == L".appref-ms";
}

std::wstring PathLeaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

} // namespace

ShellIconCache::ShellIconCache() = default;

ShellIconCache::~ShellIconCache() {
    Reset();
}

void ShellIconCache::SetDeviceContext(ID2D1DeviceContext2* dc) {
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
    queued_.clear();
    while (!queue_.empty()) queue_.pop();
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
    (void)is_dir;
    // Presentation may treat a resolved folder shortcut as a directory, which
    // used to skip the .lnk lookup and draw a blank document / generic folder.
    return IsPerFileIconExt(LowerExt(name)) ||
           IsPerFileIconExt(LowerExt(PathLeaf(path)));
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
        if (exact_index_.contains(path) || queued_.contains(path)) return;
        queued_.insert(path);
        queue_.push(path);
        if (!running_) {
            running_ = true;
            if (worker_.joinable()) worker_.join();
            worker_ = std::thread([this] { WorkerLoop(); });
        }
    }
    cv_.notify_one();
}

void ShellIconCache::WorkerLoop() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_ && queue_.empty()) break;
            path = std::move(queue_.front());
            queue_.pop();
            queued_.erase(path);
        }
        SHFILEINFOW info{};
        int index = -1;
        const std::wstring prefix = std::wstring(1, L'\x1f') + L"GEN:";
        if (path.starts_with(prefix)) {
            const std::wstring key = path.substr(prefix.size());
            const bool dir = key == L"<dir>";
            const std::wstring query = dir ? L"dummy" : (key == L"<file>" ? L".file" : key);
            DWORD useAttrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (SHGetFileInfoW(query.c_str(), useAttrs, &info, sizeof(info),
                               SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES))
                index = info.iIcon;
            std::lock_guard<std::mutex> lock(mutex_);
            if (generic_index_.size() >= kGenericIndexLimit) generic_index_.clear();
            generic_index_[key] = index;
            if (exact_index_.size() >= kExactIndexLimit) exact_index_.clear();
            exact_index_[path] = index;
        } else {
            const UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
            const std::wstring query = ShellPath(path);
            if (SHGetFileInfoW(query.c_str(), 0, &info, sizeof(info), flags)) {
                index = info.iIcon;
                if (info.hIcon) DestroyIcon(info.hIcon);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (exact_index_.size() >= kExactIndexLimit) exact_index_.clear();
            exact_index_[path] = index;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    CoUninitialize();
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

ID2D1Bitmap* ShellIconCache::BitmapForIndex(int index, int list_id) {
    if (index < 0 || !dc_) return nullptr;
    IImageList* image_list = EnsureImageList(list_id);
    if (!image_list) return nullptr;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(list_id)) << 32) |
        static_cast<uint32_t>(index);
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.get();
    HICON icon = nullptr;
    if (FAILED(image_list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bitmap = BitmapFromIcon(icon);
    DestroyIcon(icon);
    if (!bitmap.get()) return nullptr;
    ID2D1Bitmap* raw = bitmap.get();
    if (bitmaps_.size() >= kBitmapCacheLimit) bitmaps_.clear();
    bitmaps_[key] = std::move(bitmap);
    return raw;
}

bool ShellIconCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                          const std::wstring& path, const std::wstring& name,
                          bool is_dir, DWORD attrs) {
    if (!dc) return false;
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir, path) && !path.empty();
    if (exact) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = exact_index_.find(path);
        if (it != exact_index_.end()) index = it->second;
    }
    if (index < 0 && exact) RequestExact(path);
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    const float desired = std::max(dest.right - dest.left, dest.bottom - dest.top);
    ID2D1Bitmap* bitmap = BitmapForIndex(index, ImageListId(desired));
    if (!bitmap) return false;
    dc->DrawBitmap(bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   nullptr, nullptr);
    return true;
}


ID2D1Bitmap* ShellIconCache::BitmapFor(const std::wstring& path, const std::wstring& name,
                                       bool is_dir, DWORD attrs, float desired_dips) {
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir, path) && !path.empty();
    if (exact) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = exact_index_.find(path);
        if (it != exact_index_.end()) index = it->second;
    }
    if (index < 0 && exact) RequestExact(path);
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    return BitmapForIndex(index, ImageListId(desired_dips));
}
} // namespace pulse::ui
