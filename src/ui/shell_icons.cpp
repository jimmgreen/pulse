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
constexpr size_t kExactIndexLimit = 4096;
constexpr size_t kBitmapCacheLimit = 128;

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
        if (!running_) {
            running_ = true;
            if (worker_.joinable()) worker_.join();
            worker_ = std::thread([this] { WorkerLoop(); });
        }
    }
    cv_.notify_one();
}

void ShellIconCache::WorkerLoop() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_) break;
            path = std::move(queue_.front());
            queue_.pop();
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
    if (bitmaps_.size() >= kBitmapCacheLimit) bitmaps_.erase(bitmaps_.begin());
    bitmaps_[key] = std::move(bitmap);
    return raw;
}

bool ShellIconCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                          const std::wstring& path, const std::wstring& name,
                          bool is_dir, DWORD attrs) {
    if (!dc) return false;
    const float desired = std::max(dest.right - dest.left, dest.bottom - dest.top);
    ID2D1Bitmap* bitmap = BitmapFor(path, name, is_dir, attrs, desired);
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
        if (it != exact_index_.end()) {
            index = it->second;
            last_used_[path] = ++access_clock_;
        }
    }
    if (index < 0 && exact) RequestExact(path);
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    const int list_id = ImageListId(desired_dips);
    if (auto* bitmap = BitmapForIndex(index, list_id)) return bitmap;
    return BitmapForIndex(GenericIndex(L"", is_dir, attrs), list_id);
}
} // namespace pulse::ui
