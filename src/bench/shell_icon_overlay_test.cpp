// Isolated exact-query fixture plus a real private Windows IImageList.
// No COM registration, associations, user files or Shell image lists change.
#include "../ui/shell_icons.h"
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <cstdio>

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
std::atomic<UINT> exact_flags{0};
std::atomic<DWORD> query_thread{0};
int base_index = 0;
int link_slot = 0;
constexpr UINT kLockSlot = 14;
constexpr UINT kOtherSlot = 13;
DWORD_PTR QueryIconFixture(LPCWSTR path, DWORD attrs, SHFILEINFOW* info, UINT size, UINT flags) {
    if (!(flags & SHGFI_PIDL) && path && std::wstring_view(path).starts_with(L"PULSE_OVERLAY_FIXTURE_")) {
        const std::wstring_view name(path);
        UINT overlay = name.find(L"encrypted") != name.npos ? kLockSlot :
            name.find(L"other") != name.npos ? kOtherSlot :
            name.find(L"shortcut") != name.npos ? static_cast<UINT>(link_slot) : 0;
        exact_flags = flags;
        query_thread = GetCurrentThreadId();
        info->iIcon = base_index;
        if ((flags & (SHGFI_ICON | SHGFI_OVERLAYINDEX)) == (SHGFI_ICON | SHGFI_OVERLAYINDEX))
            info->iIcon |= static_cast<int>(overlay << 24);
        info->hIcon = nullptr;
        return 1;
    }
    return SHGetFileInfoW(path, attrs, info, size, flags);
}
}

// Only this test translation unit redirects the query boundary. All cache,
// worker, key decoding and WIC conversion code below is production source.
#define SHGetFileInfoW QueryIconFixture
#ifndef PULSE_SHELL_ICONS_SOURCE
#define PULSE_SHELL_ICONS_SOURCE "../ui/shell_icons.cpp"
#endif
#include PULSE_SHELL_ICONS_SOURCE
#undef SHGetFileInfoW

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool Wait(ShellIconCache& cache) {
        const auto end = GetTickCount64() + 10000;
        while (GetTickCount64() < end) {
            { std::lock_guard lock(cache.mutex_); if (cache.queued_.empty()) return true; }
            Sleep(2);
        }
        return false;
    }
    static int Exact(ShellIconCache& cache, const std::wstring& path) {
        cache.RequestExact(path);
        if (!Wait(cache)) return -1;
        std::lock_guard lock(cache.mutex_);
        const auto found = cache.exact_index_.find(path);
        return found == cache.exact_index_.end() ? -1 : found->second;
    }
    static bool Invalidate(ShellIconCache& cache) {
        const auto generation = cache.association_generation_;
        cache.InvalidateAssociations();
        return cache.exact_index_.empty() && cache.queued_.empty() && cache.convert_pending_.empty() &&
            cache.ready_pixels_.empty() && cache.association_generation_ == generation + 1;
    }
};
}

namespace {
int failures = 0;
void Check(bool pass, const char* message) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", message);
    failures += !pass;
}

HBITMAP Image(UINT side, bool lock, UINT base_extent = 0) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(side), -static_cast<LONG>(side),
        1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap || !raw) return bitmap;
    auto* pixels = static_cast<uint32_t*>(raw);
    std::fill_n(pixels, static_cast<size_t>(side) * side, 0u);
    for (UINT y = 2; y < side - 2; ++y)
        for (UINT x = 2; x < side - 2; ++x) {
            if (!lock && (!base_extent || (x < base_extent - 2 && y < base_extent - 2)))
                pixels[y * side + x] = 0xff2460d8u;
            else if (lock && x >= side / 2 && y >= side * 5 / 8)
                pixels[y * side + x] = 0xffffc21au;
        }
    return bitmap;
}

bool SavePng(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& pixels) {
    using namespace pulse::ui;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    auto format = GUID_WICPixelFormat32bppBGRA;
    return !pixels.empty() &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), const_cast<BYTE*>(pixels.data()))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

size_t Yellow(const std::vector<uint8_t>& data) {
    size_t result = 0;
    for (size_t i = 0; i + 3 < data.size(); i += 4)
        result += data[i + 2] > 220 && data[i + 1] > 140 && data[i] < 60 && data[i + 3] > 128;
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    if (argc != 2) return 2;
    const std::filesystem::path output = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(output);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    link_slot = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    std::printf("NATIVE_LINK standard_overlay_slot=%d\n", link_slot);
    Check(link_slot > 0 && link_slot <= 15 && link_slot != static_cast<int>(kLockSlot), "resolve standard link overlay slot without registering handlers");
    for (UINT side : {16u, 32u}) {
        ComPtr<IImageList2> images;
        HRESULT hr = ImageList_CoCreateInstance(CLSID_ImageList, nullptr, IID_PPV_ARGS(&images));
        if (SUCCEEDED(hr)) hr = images->Initialize(side, side, ILC_COLOR32 | ILC_MASK, 4, 4);
        Check(SUCCEEDED(hr), "create isolated native image list");
        if (FAILED(hr)) return 2;
        HBITMAP base = Image(side, false), overlay = Image(side, true);
        int lock_image = 0;
        images->Add(base, nullptr, &base_index);
        images->Add(overlay, nullptr, &lock_image);
        DeleteObject(base); DeleteObject(overlay);
        Check(SUCCEEDED(images->SetOverlayImage(lock_image, kLockSlot)), "register private lock overlay mask");
        images->SetOverlayImage(lock_image, kOtherSlot);
        images->SetOverlayImage(lock_image, link_slot);
        ShellIconCache cache;
        const int plain = ShellIconCacheTestAccess::Exact(cache, L"PULSE_OVERLAY_FIXTURE_plain.pdf");
        const int encrypted = ShellIconCacheTestAccess::Exact(cache, L"PULSE_OVERLAY_FIXTURE_encrypted.pdf");
        const int other = ShellIconCacheTestAccess::Exact(cache, L"PULSE_OVERLAY_FIXTURE_other.pdf");
        const int shortcut = ShellIconCacheTestAccess::Exact(cache, L"PULSE_OVERLAY_FIXTURE_shortcut.lnk");
        const int encrypted_shortcut = ShellIconCacheTestAccess::Exact(cache, L"PULSE_OVERLAY_FIXTURE_encrypted-shortcut.lnk");
        std::printf("OVERLAY side=%u plain=%08x encrypted=%08x other=%08x shortcut=%08x flags=%08x\n",
            side, plain, encrypted, other, shortcut, exact_flags.load());
        Check((exact_flags & (SHGFI_ICON | SHGFI_OVERLAYINDEX | SHGFI_ADDOVERLAYS)) ==
            (SHGFI_ICON | SHGFI_OVERLAYINDEX | SHGFI_ADDOVERLAYS), "exact worker requests the path overlay with required SHGFI_ICON");
        Check(query_thread != GetCurrentThreadId(), "Shell extension query stays off the calling UI thread");
        Check(plain == base_index && encrypted == (base_index | static_cast<int>(kLockSlot << 24)) &&
            other != encrypted && IconKey(SHIL_SMALL, plain) != IconKey(SHIL_SMALL, encrypted),
            "same-extension plain, encrypted and other overlay paths retain different pixel-cache keys");
        Check(shortcut == base_index && encrypted_shortcut == encrypted,
            "only native shortcut arrow is suppressed; third-party lock on a shortcut remains");
        const int list_id = side == 16 ? SHIL_SMALL : SHIL_LARGE;
        std::unordered_map<int, IImageList*> lists{{list_id, images.get()}};
        ComPtr<IWICImagingFactory> wic;
        UINT width = 0, height = 0;
        std::vector<uint8_t> plain_pixels, encrypted_pixels, shortcut_pixels;
        const bool plain_ok = ConvertIconPixels(IconKey(list_id, plain), lists, wic, width, height, plain_pixels);
        const bool encrypted_ok = ConvertIconPixels(IconKey(list_id, encrypted), lists, wic, width, height, encrypted_pixels);
        const bool shortcut_ok = ConvertIconPixels(IconKey(list_id, shortcut), lists, wic, width, height, shortcut_pixels);
        Check(plain_ok && encrypted_ok && shortcut_ok && Yellow(plain_pixels) == 0 && Yellow(encrypted_pixels) > 0 &&
            plain_pixels != encrypted_pixels && plain_pixels == shortcut_pixels,
            "real IImageList GetIcon overlay mask preserves yellow lock pixels without contaminating plain or shortcut artwork");
        std::printf("PIXELS side=%u plain_yellow=%zu encrypted_yellow=%zu\n", side, Yellow(plain_pixels), Yellow(encrypted_pixels));
        Check(SavePng(output / (std::to_wstring(side) + L"-plain.png"), width, height, plain_pixels) &&
            SavePng(output / (std::to_wstring(side) + L"-encrypted.png"), width, height, encrypted_pixels), "save actual native overlay pixel captures");
        Check(ShellIconCacheTestAccess::Invalidate(cache), "association invalidation still clears overlay identities and advances cancellation generation");
    }

    {
        ComPtr<IImageList2> images;
        HRESULT hr = ImageList_CoCreateInstance(CLSID_ImageList, nullptr, IID_PPV_ARGS(&images));
        if (SUCCEEDED(hr)) hr = images->Initialize(256, 256, ILC_COLOR32 | ILC_MASK, 2, 2);
        Check(SUCCEEDED(hr), "create isolated jumbo undersized-icon fixture");
        if (FAILED(hr)) return 2;
        HBITMAP base = Image(256, false, 48), overlay = Image(256, true);
        int base_image = 0, lock_image = 0;
        images->Add(base, nullptr, &base_image);
        images->Add(overlay, nullptr, &lock_image);
        DeleteObject(base); DeleteObject(overlay);
        images->SetOverlayImage(lock_image, kLockSlot);
        std::unordered_map<int, IImageList*> lists{{SHIL_JUMBO, images.get()}};
        ComPtr<IWICImagingFactory> wic;
        UINT plain_w = 0, plain_h = 0, lock_w = 0, lock_h = 0;
        std::vector<uint8_t> plain_pixels, lock_pixels;
        bool plain_ok = ConvertIconPixels(IconKey(SHIL_JUMBO, base_image), lists, wic, plain_w, plain_h, plain_pixels);
        bool lock_ok = ConvertIconPixels(IconKey(SHIL_JUMBO, base_image | static_cast<int>(kLockSlot << 24)),
            lists, wic, lock_w, lock_h, lock_pixels);
        auto plain_bounds = MeasureIconArtwork(plain_pixels, plain_w, plain_h);
        auto lock_bounds = MeasureIconArtwork(lock_pixels, lock_w, lock_h);
        CropUndersizedIcon(plain_pixels, plain_w, plain_h, plain_bounds);
        CropUndersizedIcon(lock_pixels, lock_w, lock_h, lock_bounds);
        Check(plain_ok && plain_w == 48 && plain_h == 48 && plain_bounds.native_side == 48 &&
            IconDrawSide(256, plain_bounds.native_side) == 96,
            "unoverlaid 48px artwork in a jumbo frame retains original crop and 2x limit");
        Check(lock_ok && lock_w == 256 && lock_h == 256 && Yellow(lock_pixels) > 0 && lock_bounds.native_side == 0,
            "jumbo corner overlay is preserved outside the small base artwork rather than cropped away");
        std::printf("JUMBO base_width=%u native_side=%u overlaid_width=%u overlaid_native_side=%u yellow=%zu\n",
            plain_w, plain_bounds.native_side, lock_w, lock_bounds.native_side, Yellow(lock_pixels));
        SavePng(output / L"jumbo-48px-base-cropped.png", plain_w, plain_h, plain_pixels);
        SavePng(output / L"jumbo-48px-base-with-corner-overlay.png", lock_w, lock_h, lock_pixels);
    }

    // A real Shell query on self-owned fixtures, without needing encryption
    // software on this machine or claiming the customer's handler reproduced.
    const auto plain_file = output / L"native-plain.txt";
    { std::ofstream file(plain_file); file << "owned shell icon fixture\n"; }
    const auto shortcut_file = output / L"native-shortcut.lnk";
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persist;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(plain_file.c_str());
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr)) hr = persist->Save(shortcut_file.c_str(), TRUE);
    Check(SUCCEEDED(hr), "create owned native shortcut fixture");
    ShellIconCache native;
    for (const auto& file : {plain_file, shortcut_file}) {
        SHFILEINFOW info{};
        const bool got = SHGetFileInfoW(file.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX |
            SHGFI_SMALLICON | SHGFI_ICON | SHGFI_OVERLAYINDEX | SHGFI_ADDOVERLAYS) != 0;
        if (info.hIcon) DestroyIcon(info.hIcon);
        const int index = ShellIconCacheTestAccess::Exact(native, file.wstring());
        int expected = info.iIcon;
        if ((static_cast<uint32_t>(expected) >> 24) == static_cast<uint32_t>(link_slot)) expected &= 0x00ffffff;
        Check(got && index == expected, "native Windows file/shortcut identity matches Shell after Pulse-owned arrow suppression");
        std::printf("NATIVE path=%ls shell=%08x shell_overlay_slot=%u standard_link_slot=%d pulse=%08x\n",
            file.filename().c_str(), info.iIcon, static_cast<uint32_t>(info.iIcon) >> 24, link_slot, index);
    }
    Check(IconDrawSide(256, 32) == 64 && IconDrawSide(256, 0) == 256,
        "existing undersized native-icon two-times display limit remains unchanged");
    std::printf("SUMMARY failures=%d; customer_encryption_handler=not_available_locally\n", failures);
    CoUninitialize();
    return failures ? 1 : 0;
}
