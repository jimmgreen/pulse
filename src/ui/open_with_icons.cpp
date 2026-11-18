// open_with_icons.cpp
#include "open_with_icons.h"
#include "../common/preview_extensions.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cwctype>

namespace pulse::ui {

namespace {

constexpr size_t kEntryLimit = 256;
constexpr size_t kQueueLimit = 64;
constexpr size_t kReadyLimit = 256;
constexpr size_t kReadyBytes = 4 * 1024 * 1024;

std::wstring LowerExtension(const std::wstring& name) {
    const size_t slash = name.find_last_of(L"\\/");
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ||
        dot + 1 >= name.size())
        return L"";
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

// Icons are decoded at a few fixed edges so every view size reuses them.
uint32_t BucketPixels(uint32_t pixels) noexcept {
    static constexpr std::array<uint32_t, 8> kBuckets{16, 20, 24, 32, 40, 48, 64, 96};
    for (const uint32_t bucket : kBuckets)
        if (pixels <= bucket) return bucket;
    return kBuckets.back();
}

std::wstring QueryAssociation(ASSOCSTR what, const std::wstring& extension) {
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD chars = ARRAYSIZE(buffer);
    // IGNOREUNKNOWN: types nobody registered fail instead of reporting the
    // "pick an app" handler, which would put the same icon on everything.
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN | ASSOCF_NOTRUNCATE, what,
                                 extension.c_str(), nullptr, buffer, &chars)))
        return L"";
    return buffer;
}

bool EndsWithInsensitive(const std::wstring& text, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

bool CopyPremultiplied(IWICImagingFactory* wic, IWICBitmapSource* source,
                       UINT& width, UINT& height, std::vector<uint8_t>& data) {
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) || !converter.get()) return false;
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) return false;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > 256 || height > 256) return false;
    data.resize(static_cast<size_t>(width) * height * 4u);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4u,
                                           static_cast<UINT>(data.size()), data.data()));
}

// Store apps report a package logo ("@{Package?ms-resource://...}") that
// resolves to a scaled PNG on disk.
bool LoadImageFile(IWICImagingFactory* wic, const std::wstring& file, uint32_t edge,
                   UINT& width, UINT& height, std::vector<uint8_t>& data) {
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder)) ||
        !decoder.get()) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame.get()) return false;
    UINT source_w = 0, source_h = 0;
    if (FAILED(frame->GetSize(&source_w, &source_h)) || !source_w || !source_h) return false;
    const float fit = static_cast<float>(edge) / static_cast<float>((std::max)(source_w, source_h));
    const UINT target_w = (std::max)(1u, static_cast<UINT>(source_w * fit + 0.5f));
    const UINT target_h = (std::max)(1u, static_cast<UINT>(source_h * fit + 0.5f));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) || !scaler.get() ||
        FAILED(scaler->Initialize(frame.get(), target_w, target_h,
                                  WICBitmapInterpolationModeHighQualityCubic))) return false;
    return CopyPremultiplied(wic, scaler.get(), width, height, data);
}

bool LoadIconResource(IWICImagingFactory* wic, const std::wstring& file, int index,
                      uint32_t edge, UINT& width, UINT& height, std::vector<uint8_t>& data) {
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, MAKELONG(edge, 0))) ||
        !icon) return false;
    ComPtr<IWICBitmap> bitmap;
    const HRESULT hr = wic->CreateBitmapFromHICON(icon, &bitmap);
    DestroyIcon(icon);
    if (FAILED(hr) || !bitmap.get()) return false;
    return CopyPremultiplied(wic, bitmap.get(), width, height, data);
}

// "C:\...\app.exe,-101" or "%ProgramFiles%\app.exe" -> expanded file + index.
bool ParseIconLocation(const std::wstring& reference, std::wstring& file, int& index) {
    wchar_t buffer[MAX_PATH * 2]{};
    if (wcsncpy_s(buffer, reference.c_str(), _TRUNCATE) != 0 && !buffer[0]) return false;
    index = PathParseIconLocationW(buffer);
    PathUnquoteSpacesW(buffer);
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(buffer, expanded, ARRAYSIZE(expanded));
    file = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : buffer;
    return !file.empty();
}

// Mostly light pixels over a lot of transparency: a white Store glyph meant
// for a colored tile. It disappears on light thumbnails without a plate.
bool NeedsPlate(const std::vector<uint8_t>& data) {
    uint64_t opaque = 0, light = 0;
    const size_t count = data.size() / 4;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t a = data[i * 4 + 3];
        if (a < 128) continue;
        ++opaque;
        // Premultiplied: compare against alpha.
        const uint32_t b = data[i * 4], g = data[i * 4 + 1], r = data[i * 4 + 2];
        if (r * 100 >= a * 88u && g * 100 >= a * 88u && b * 100 >= a * 88u) ++light;
    }
    return count > 0 && opaque > 0 && opaque * 10 < count * 7 && light * 10 >= opaque * 9;
}

bool ResolvePhotoshopIcon(IWICImagingFactory* wic, uint32_t edge,
                          UINT& width, UINT& height, std::vector<uint8_t>& data) {
    auto load = [&](std::wstring file) {
        if (file.empty()) return false;
        wchar_t expanded[32768]{};
        const DWORD count = ExpandEnvironmentStringsW(file.c_str(), expanded, ARRAYSIZE(expanded));
        if (!count || count > ARRAYSIZE(expanded)) return false;
        file = expanded;
        PathUnquoteSpacesW(file.data());
        file.resize(wcslen(file.c_str()));
        if (_wcsicmp(PathFindFileNameW(file.c_str()), L"Photoshop.exe") != 0) return false;
        return LoadIconResource(wic, file, 0, edge, width, height, data);
    };
    // A document family badge identifies its editor, not a generic viewer
    // selected independently for .psd and .psb in Windows.
    for (const auto* extension : {L".psd", L".psb"})
        if (load(QueryAssociation(ASSOCSTR_EXECUTABLE, extension))) return true;
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Photoshop.exe",
                    0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) continue;
            wchar_t file[32768]{};
            DWORD bytes = sizeof(file);
            const auto result = RegGetValueW(key, nullptr, nullptr,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, file, &bytes);
            RegCloseKey(key);
            if (result == ERROR_SUCCESS && load(file)) return true;
        }
    }
    return false;
}

bool ResolveIcon(IWICImagingFactory* wic, const std::wstring& extension, uint32_t edge,
                 UINT& width, UINT& height, std::vector<uint8_t>& data) {
    if (preview::IsPsdExtension(extension))
        return ResolvePhotoshopIcon(wic, edge, width, height, data);
    std::wstring reference = QueryAssociation(ASSOCSTR_APPICONREFERENCE, extension);
    if (!reference.empty() && reference.front() == L'@') {
        wchar_t file[MAX_PATH * 2]{};
        if (SUCCEEDED(SHLoadIndirectString(reference.c_str(), file, ARRAYSIZE(file), nullptr)) &&
            file[0] && LoadImageFile(wic, file, edge, width, height, data))
            return true;
        reference.clear();
    }
    std::wstring file;
    int index = 0;
    if (!reference.empty() && ParseIconLocation(reference, file, index) &&
        LoadIconResource(wic, file, index, edge, width, height, data))
        return true;
    // Classic Win32 programs: the executable's first icon.
    file = QueryAssociation(ASSOCSTR_EXECUTABLE, extension);
    if (file.empty()) return false;
    // Hosts rather than programs: their icon says nothing about the app.
    if (EndsWithInsensitive(file, L"\\rundll32.exe") || EndsWithInsensitive(file, L"\\OpenWith.exe") ||
        EndsWithInsensitive(file, L"\\explorer.exe") || EndsWithInsensitive(file, L"\\cmd.exe"))
        return false;
    return LoadIconResource(wic, file, 0, edge, width, height, data);
}

} // namespace

OpenWithIconCache::~OpenWithIconCache() {
    Reset();
}

void OpenWithIconCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    // Another window's context on the same device keeps every icon.
    if (!device_binding_.Bind(dc)) return;
    dc_ = device_binding_.Dc();
    entries_.clear();
}

void OpenWithIconCache::Reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        queue_.clear();
        pending_.clear();
        ready_.clear();
        ready_order_.clear();
        ready_bytes_ = 0;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    entries_.clear();
    device_binding_.Bind(nullptr);
    dc_ = nullptr;
}

void OpenWithIconCache::InvalidateAssociations() {
    epoch_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        pending_.clear();
        ready_.clear();
        ready_order_.clear();
        ready_bytes_ = 0;
    }
    // Keep displayed icons until their replacements are ready. Registry
    // notifications can repeat even when the actual default app is unchanged.
}

void OpenWithIconCache::StartWorkerLocked() {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    worker_ = std::thread([this] { WorkerLoop(); });
}

OpenWithIconCache::Badge OpenWithIconCache::BadgeFor(const std::wstring& name, uint32_t pixels) {
    const std::wstring extension = LowerExtension(name);
    if (!dc_ || extension.empty() || extension == L".lnk" || extension == L".url") return {};
    if (preview::IsImageExtension(extension) || preview::IsVectorExtension(extension) ||
        preview::IsFamilyExtension(preview::PreviewFamily::MetaFile, extension) ||
        preview::IsOneOf(extension, {L".apng", L".jxl", L".raw", L".dng", L".cr2", L".cr3",
            L".nef", L".nrw", L".arw", L".sr2", L".orf", L".rw2", L".raf", L".pef"})) return {};
    const uint32_t bucket = BucketPixels(pixels);
    const std::wstring family = preview::IsPsdExtension(extension) ? L".psd" : extension;
    const std::wstring key = family + L"|" + std::to_wstring(bucket);
    const wchar_t* format_label = extension == L".psd" ? L"PSD" : extension == L".psb" ? L"PSB" : nullptr;
    Badge previous;
    if (const auto it = entries_.find(key); it != entries_.end()) {
        previous = {it->second.bitmap.get(), it->second.needs_plate,
                    it->second.missing ? format_label : nullptr};
        if (it->second.epoch == epoch_.load(std::memory_order_relaxed)) return previous;
    }
    Pixels ready;
    bool have_ready = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto it = ready_.find(key); it != ready_.end()) {
            ready_bytes_ -= it->second.data.size();
            ready = std::move(it->second);
            ready_.erase(it);
            std::erase(ready_order_, key);
            have_ready = true;
        } else if (!pending_.contains(key)) {
            if (queue_.size() >= kQueueLimit) {
                pending_.erase(queue_.front().key);
                queue_.pop_front();
            }
            pending_.insert(key);
            queue_.push_back({key, family, bucket, epoch_.load(std::memory_order_relaxed)});
            StartWorkerLocked();
            cv_.notify_one();
        }
    }
    if (!have_ready) return previous;
    if (!entries_.contains(key) && entries_.size() >= kEntryLimit) entries_.erase(entries_.begin());
    Entry replacement;
    replacement.epoch = epoch_.load(std::memory_order_relaxed);
    if (ready.data.empty()) {
        replacement.missing = true;
        entries_[key] = std::move(replacement);
        return {nullptr, false, format_label};
    }
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(ready.width, ready.height), ready.data.data(),
                                 ready.width * 4u, props, &replacement.bitmap)) || !replacement.bitmap.get()) {
        return previous;
    }
    replacement.needs_plate = ready.needs_plate;
    Entry& entry = entries_[key];
    entry = std::move(replacement);
    return {entry.bitmap.get(), entry.needs_plate};
}

void OpenWithIconCache::PublishReadyLocked(const std::wstring& key, Pixels pixels) {
    if (const auto found = ready_.find(key); found != ready_.end()) {
        ready_bytes_ -= found->second.data.size();
        ready_.erase(found);
        std::erase(ready_order_, key);
    }
    if (pixels.data.size() > kReadyBytes) return;
    while (!ready_order_.empty() && (ready_.size() >= kReadyLimit ||
            ready_bytes_ + pixels.data.size() > kReadyBytes)) {
        const auto found = ready_.find(ready_order_.front());
        if (found != ready_.end()) {
            ready_bytes_ -= found->second.data.size();
            ready_.erase(found);
        }
        ready_order_.pop_front();
    }
    ready_bytes_ += pixels.data.size();
    ready_order_.push_back(key);
    ready_[key] = std::move(pixels);
}

void OpenWithIconCache::WorkerLoop() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !running_ || !queue_.empty(); });
            if (!running_) break;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        Pixels pixels;
        if (wic.get() && ResolveIcon(wic.get(), request.extension, request.pixels,
                                     pixels.width, pixels.height, pixels.data))
            pixels.needs_plate = NeedsPlate(pixels.data);
        else
            pixels.data.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request.epoch != epoch_.load(std::memory_order_relaxed)) continue;
            pending_.erase(request.key);
            if (!running_) continue;
            PublishReadyLocked(request.key, std::move(pixels));
        }
        repaint_.InvalidateAll();
    }
    wic.reset();
    if (SUCCEEDED(com_hr)) CoUninitialize();
}

} // namespace pulse::ui
