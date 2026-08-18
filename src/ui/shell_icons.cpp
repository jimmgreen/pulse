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

} // namespace

ShellIconCache::ShellIconCache() = default;

ShellIconCache::~ShellIconCache() {
    Reset();
}

void ShellIconCache::SetDeviceContext(ID2D1DeviceContext2* dc) {
    if (dc_ == dc) return;
    dc_ = dc;
    bitmaps_.clear();
    generic_index_.clear();
}

void ShellIconCache::SetScale(float scale) {
    if (std::abs(scale_ - scale) <= 0.001f) return;
    scale_ = scale;
    bitmaps_.clear();
    if (image_list_) {
        image_list_->Release();
        image_list_ = nullptr;
    }
    image_list_id_ = -1;
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
    if (image_list_) {
        image_list_->Release();
        image_list_ = nullptr;
    }
    image_list_id_ = -1;
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

bool ShellIconCache::EnsureImageList(int id) {
    if (image_list_ && image_list_id_ == id) return true;
    if (image_list_) {
        image_list_->Release();
        image_list_ = nullptr;
    }
    bitmaps_.clear();
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    if (FAILED(SHGetImageList(id, IID_IImageList, reinterpret_cast<void**>(&image_list_)))
        || !image_list_) {
        return false;
    }
    image_list_id_ = id;
    return true;
}

bool ShellIconCache::EnsureWic() {
    if (wic_.get()) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic_))) && wic_.get();
}

bool ShellIconCache::NeedsExactIcon(const std::wstring& name, bool is_dir) {
    if (is_dir) return false;
    const std::wstring ext = LowerExt(name);
    return ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".cur" ||
           ext == L".dll" || ext == L".scr" || ext == L".cpl" || ext == L".msc" ||
           ext == L".url" || ext == L".appref-ms";
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
            generic_index_[key] = index;
            exact_index_[path] = index;
        } else {
            const UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
            const std::wstring query = ShellPath(path);
            if (SHGetFileInfoW(query.c_str(), 0, &info, sizeof(info), flags)) {
                index = info.iIcon;
                if (info.hIcon) DestroyIcon(info.hIcon);
            }
            std::lock_guard<std::mutex> lock(mutex_);
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
    if (index < 0 || !EnsureImageList(list_id) || !dc_) return nullptr;
    auto it = bitmaps_.find(index);
    if (it != bitmaps_.end()) return it->second.get();
    HICON icon = nullptr;
    if (FAILED(image_list_->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bitmap = BitmapFromIcon(icon);
    DestroyIcon(icon);
    if (!bitmap.get()) return nullptr;
    ID2D1Bitmap* raw = bitmap.get();
    bitmaps_[index] = std::move(bitmap);
    return raw;
}

bool ShellIconCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                          const std::wstring& path, const std::wstring& name,
                          bool is_dir, DWORD attrs) {
    if (!dc) return false;
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir) && !path.empty();
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
    const bool exact = NeedsExactIcon(name, is_dir) && !path.empty();
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
