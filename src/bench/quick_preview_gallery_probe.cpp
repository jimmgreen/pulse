// Live probe for Quick Look's photo viewer (quick_preview_gallery.cpp).
// PULSE_TEST_GALLERY_DIR: a folder of pictures and videos (read only).
// PULSE_TEST_DATA_DIR: where captures and gallery-probe.log go.
// Runs through RunQuickPreviewAuditTest on a private desktop.
#include "../ui/quick_preview_window.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include <objbase.h>
#include <commctrl.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ui {

struct QuickPreviewGalleryProbe {
    struct File {
        std::wstring path, name;
        DWORD attrs = 0;
        uint64_t modified = 0, size = 0;
        int kind = 0;
    };
    static inline std::vector<File> files;
    static inline QuickPreviewWindow* window = nullptr;
    static inline int jumps = 0;
    static inline int last_lparam = 0;
    static inline int failures = 0;
    static inline std::filesystem::path out;

    static bool Check(bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(stdout);
        failures += !ok;
        return ok;
    }
    static void Pump(unsigned ms) {
        const auto end = GetTickCount64() + ms;
        do {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        } while (GetTickCount64() < end);
    }
    static QuickPreviewItem ItemAt(int row) {
        QuickPreviewItem item;
        const File& f = files[static_cast<size_t>(row)];
        item.path = f.path;
        item.name = f.name;
        item.attrs = f.attrs;
        item.modified = f.modified;
        item.size = f.size;
        item.position = row + 1;
        item.count = static_cast<int>(files.size());
        auto gallery = std::make_shared<QuickPreviewGallery>();
        for (const File& file : files) gallery->kinds.push_back(static_cast<uint8_t>(file.kind));
        gallery->current = row;
        gallery->first = (std::max)(0, row - 80);
        const int last = (std::min)(static_cast<int>(files.size()) - 1, row + 80);
        for (int r = gallery->first; r <= last; ++r) {
            QuickPreviewGalleryEntry e;
            e.path = files[static_cast<size_t>(r)].path;
            e.attrs = files[static_cast<size_t>(r)].attrs;
            e.modified = files[static_cast<size_t>(r)].modified;
            e.size = files[static_cast<size_t>(r)].size;
            gallery->entries.push_back(std::move(e));
        }
        item.gallery = std::move(gallery);
        return item;
    }
    static int Row(const QuickPreviewWindow& q) {
        for (size_t i = 0; i < files.size(); ++i)
            if (files[i].path == q.item_.path) return static_cast<int>(i);
        return -1;
    }
    // The app's side of WM_QUICK_PREVIEW_NAVIGATE (absolute when lParam > 0).
    static LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_APP + 201 && window) {
            ++jumps;
            last_lparam = static_cast<int>(lparam);
            const int current = Row(*window);
            const int count = static_cast<int>(files.size());
            int target = lparam > 0 ? static_cast<int>(lparam) - 1 : current + (static_cast<int>(wparam) < 0 ? -1 : 1);
            if (target >= 0 && target < count && target != current) window->Update(ItemAt(target));
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    static bool Save(QuickPreviewWindow& q, const wchar_t* name) {
        q.compositor_.CaptureNextFrame();
        q.Render();
        ID2D1DeviceContext* dc = q.compositor_.Dc();
        ID2D1Bitmap1* bitmap = q.compositor_.HeldFrame();
        if (!dc || !bitmap) return false;
        ComPtr<ID2D1Bitmap1> readable;
        auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                             bitmap->GetPixelFormat());
        const auto size = bitmap->GetPixelSize();
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr)))
            return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        // Opaque copy (the window's frame may carry alpha).
        std::vector<BYTE> pixels(static_cast<size_t>(size.width) * size.height * 4);
        for (UINT y = 0; y < size.height; ++y) {
            BYTE* dst = pixels.data() + static_cast<size_t>(y) * size.width * 4;
            std::memcpy(dst, mapped.bits + y * mapped.pitch, size.width * 4);
            for (UINT x = 0; x < size.width; ++x) dst[x * 4 + 3] = 255;
        }
        readable->Unmap();
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        const std::wstring path = (out / name).wstring();
        return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
               SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
               SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(size.width, size.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
               SUCCEEDED(frame->WritePixels(size.height, size.width * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    static void Resize(QuickPreviewWindow& q, int w, int h) {
        SetWindowPos(q.hwnd_, nullptr, 40, 40, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(250);
    }
    static bool WaitPicture(QuickPreviewWindow& q, unsigned ms = 8000) {
        const auto end = GetTickCount64() + ms;
        while (GetTickCount64() < end) {
            Pump(40);
            if (q.native_kind_ == QuickPreviewWindow::NativeKind::Bitmap && q.decoded_w_ > 0) break;
        }
        Pump(900);   // filmstrip thumbnails and the ambient backdrop
        return q.native_kind_ == QuickPreviewWindow::NativeKind::Bitmap && q.decoded_w_ > 0;
    }
    static bool WaitPlaying(QuickPreviewWindow& q, unsigned ms = 10000) {
        const auto end = GetTickCount64() + ms;
        while (GetTickCount64() < end) {
            Pump(50);
            if (q.video_.active() && q.video_.Snapshot().playing) return true;
        }
        return false;
    }
    static POINT Center(const D2D1_RECT_F& r) {
        return POINT{static_cast<LONG>((r.left + r.right) * 0.5f), static_cast<LONG>((r.top + r.bottom) * 0.5f)};
    }
    // No pump: on the private desktop the real cursor is elsewhere, so the
    // WM_MOUSELEAVE that TrackMouseEvent queues would clear the hover at once.
    static void Move(QuickPreviewWindow& q, POINT p) {
        SendMessageW(q.hwnd_, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
    }
    static void Click(QuickPreviewWindow& q, POINT p) {
        Move(q, p);
        SendMessageW(q.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        SendMessageW(q.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
        Pump(30);
    }
    static void Key(QuickPreviewWindow& q, UINT vk) {
        SendMessageW(q.hwnd_, WM_KEYDOWN, vk, 1);
        SendMessageW(q.hwnd_, WM_KEYUP, vk, (1u << 31) | (1u << 30) | 1);
        Pump(30);
    }
    static bool Intersects(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom &&
               a.right > a.left && b.right > b.left;
    }
    static double RenderMs(QuickPreviewWindow& q, int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) q.Render();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    }
    static int FirstOfKind(int kind, int from = 0) {
        for (int i = from; i < static_cast<int>(files.size()); ++i)
            if (files[static_cast<size_t>(i)].kind == kind) return i;
        return -1;
    }

    static bool Run(const std::wstring& dir) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            File f;
            f.path = entry.path().wstring();
            f.name = entry.path().filename().wstring();
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &data)) continue;
            f.attrs = data.dwFileAttributes;
            f.size = (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
            f.modified = (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
            f.kind = QuickPreviewMediaKind(f.name);
            files.push_back(std::move(f));
        }
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        int pictures = 0, videos = 0;
        for (const File& f : files) { pictures += f.kind == 1; videos += f.kind == 2; }
        std::printf("fixture files=%zu pictures=%d videos=%d\n", files.size(), pictures, videos);
        if (!Check(pictures >= 3 && videos >= 1, "fixture has pictures and a video")) return false;

        OleInitialize(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW wc{};
        wc.lpfnWndProc = OwnerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseGalleryProbeOwner";
        RegisterClassW(&wc);
        HWND owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!Check(owner != nullptr, "owner window")) return false;
        {
            QuickPreviewWindow q;
            window = &q;
            Check(q.Initialize(owner, WM_APP + 201, WM_APP + 202), "initialize Quick Look");
            q.options_.immersive = true;
            // Start on a landscape picture in the middle of the listing.
            int start = FirstOfKind(1, static_cast<int>(files.size()) / 3);
            if (start < 0) start = FirstOfKind(1);
            q.Show(ItemAt(start), true, WindowEffect::None, false);
            Check(WaitPicture(q), "picture decoded");
            RECT frame0{};
            GetWindowRect(q.hwnd_, &frame0);
            std::printf("scale=%.2f size=%ux%u\n", q.scale_, q.compositor_.Width(), q.compositor_.Height());
            Check(q.Immersive() && q.GalleryActive(), "photo viewer active with a gallery");
            Check(q.FooterHeight() == 0.0f, "no footer in the photo viewer");
            Check(q.gallery_filter_count_ == 3, "filter pill shows all / pictures / videos");
            Save(q, L"01_photo.png");
            Check(!q.gallery_tiles_.empty(), "filmstrip tiles laid out");
            std::printf("counter=%ls tiles=%zu\n", q.header_.counter_text.c_str(), q.gallery_tiles_.size());
            const D2D1_RECT_F content = q.ContentRect();
            Check(!Intersects(content, q.gallery_panel_), "picture area clear of the filmstrip");
            std::printf("render_ms immersive=%.2f\n", RenderMs(q, 30));
            {
                const float keep = q.gallery_alpha_;
                q.gallery_alpha_ = 0.0f;   // backdrop and picture only
                std::printf("render_ms immersive_without_chrome=%.2f\n", RenderMs(q, 30));
                q.gallery_alpha_ = keep;
                q.Render();   // lay the filmstrip out again
            }

            // Hover a neighbouring tile, then click it.
            int neighbour = -1;
            D2D1_RECT_F neighbour_rect{};
            for (const auto& tile : q.gallery_tiles_)
                if (tile.row > Row(q)) { neighbour = tile.row; neighbour_rect = tile.rect; break; }
            if (Check(neighbour >= 0, "a tile after the current one")) {
                Move(q, Center(neighbour_rect));
                Check(q.gallery_hover_ == 100 + neighbour, "tile hover");
                Save(q, L"02_hover.png");
                const int before = jumps;
                Click(q, Center(neighbour_rect));
                Check(jumps == before + 1 && last_lparam == neighbour + 1, "tile click posts an absolute jump");
                Pump(120);
                Save(q, L"03a_glide.png");
                if (files[static_cast<size_t>(neighbour)].kind == 1) WaitPicture(q); else Pump(1500);
                Check(Row(q) == neighbour, "tile click shows that file");
                RECT frame1{};
                GetWindowRect(q.hwnd_, &frame1);
                Check(EqualRect(&frame0, &frame1) != FALSE, "window keeps its frame while stepping");
                Save(q, L"03b_jumped.png");
            }
            // Filter: pictures only, then → several times never lands on a video.
            for (int i = 0; i < q.gallery_filter_count_; ++i)
                if (q.gallery_filter_ids_[i] == 1) Click(q, Center(q.gallery_filter_rects_[i]));
            Pump(200);
            Check(q.gallery_filter_ == 1, "pictures filter selected");
            if (files[static_cast<size_t>(Row(q))].kind != 1) WaitPicture(q);
            bool only_pictures = true;
            for (int i = 0; i < 6; ++i) {
                Key(q, VK_RIGHT);
                Pump(350);
                only_pictures &= files[static_cast<size_t>(Row(q))].kind == 1;
            }
            Check(only_pictures, "→ with the pictures filter skips videos and other files");
            Check(q.gallery_alpha_ > 0.95f, "stepping with the keys keeps the filmstrip shown");
            WaitPicture(q, 4000);
            Save(q, L"04_filter_pictures.png");
            std::printf("counter=%ls\n", q.header_.counter_text.c_str());
            // Videos filter jumps to a video; the filmstrip folds while it plays.
            for (int i = 0; i < q.gallery_filter_count_; ++i)
                if (q.gallery_filter_ids_[i] == 2) Click(q, Center(q.gallery_filter_rects_[i]));
            Pump(300);
            Check(q.gallery_filter_ == 2 && files[static_cast<size_t>(Row(q))].kind == 2, "videos filter jumps to a video");
            const bool playing = WaitPlaying(q);
            Check(playing, "video plays");
            Pump(200);
            Save(q, L"05a_video_start.png");
            Pump(700);
            Check(q.gallery_open_ < 0.05f, "filmstrip folded while playing");
            Save(q, L"05b_video_playing.png");
            Check(!Intersects(q.PlaybackRect(), q.GalleryPanelRect()) || q.gallery_open_ < 0.05f,
                  "playback bar clear of the filmstrip");
            q.video_.Play(false);
            Pump(700);
            Check(q.gallery_open_ > 0.95f, "filmstrip back when paused");
            Save(q, L"05c_video_paused.png");
            std::printf("playback=%.0f,%.0f-%.0f,%.0f panel=%.0f,%.0f-%.0f,%.0f\n", q.PlaybackRect().left, q.PlaybackRect().top,
                        q.PlaybackRect().right, q.PlaybackRect().bottom, q.GalleryPanelRect().left, q.GalleryPanelRect().top,
                        q.GalleryPanelRect().right, q.GalleryPanelRect().bottom);
            Check(!Intersects(q.PlaybackRect(), q.GalleryPanelRect()), "paused: playback bar above the filmstrip");
            // Back to all, a picture; then rest the pointer for 3 s.
            for (int i = 0; i < q.gallery_filter_count_; ++i)
                if (q.gallery_filter_ids_[i] == 0) Click(q, Center(q.gallery_filter_rects_[i]));
            Click(q, Center(q.gallery_edges_[0].right > 0 ? q.gallery_edges_[0] : q.gallery_edges_[1]));
            Pump(200);
            for (int i = 0; i < 4 && files[static_cast<size_t>(Row(q))].kind != 1; ++i) { Key(q, VK_LEFT); Pump(300); }
            WaitPicture(q, 4000);
            Move(q, POINT{static_cast<LONG>(q.compositor_.Width() / 2), static_cast<LONG>(q.compositor_.Height() / 2)});
            Pump(3300);
            Check(q.gallery_alpha_ < 0.05f, "controls fade after 2.5 s still");
            Save(q, L"06_idle.png");
            Move(q, POINT{static_cast<LONG>(q.compositor_.Width() / 2) + 9, static_cast<LONG>(q.compositor_.Height() / 2)});
            Pump(300);
            Check(q.gallery_alpha_ > 0.95f, "controls return on a move");
            // Portrait / PNG picture.
            int png = -1;
            for (int i = 0; i < static_cast<int>(files.size()); ++i)
                if (files[static_cast<size_t>(i)].kind == 1 && _wcsicmp(std::filesystem::path(files[static_cast<size_t>(i)].name).extension().c_str(), L".png") == 0) { png = i; break; }
            if (png >= 0) {
                q.Update(ItemAt(png));
                WaitPicture(q);
                Save(q, L"07_png.png");
            }
            // Small window: no filmstrip.
            Resize(q, 520, 400);
            Pump(300);
            Check(!q.GalleryStripWanted(), "small window drops the filmstrip");
            Save(q, L"08_small.png");
            Resize(q, 1600, 1000);
            // Classic layout via the menu command.
            q.ToggleImmersive();
            Pump(400);
            Check(!q.Immersive() && q.FooterHeight() > 0.0f, "classic layout keeps its footer");
            Save(q, L"09_classic.png");
            std::printf("render_ms classic=%.2f\n", RenderMs(q, 30));
            q.ToggleImmersive();
            Pump(300);
            // Light theme still dark in the viewer.
            q.Close();
            Pump(400);
            q.Show(ItemAt(start), false, WindowEffect::None, false);
            WaitPicture(q);
            Save(q, L"10_light_theme.png");
            q.Close();
            Pump(300);
            window = nullptr;
        }
        DestroyWindow(owner);
        OleUninitialize();
        std::printf("gallery_probe failures=%d\n", failures);
        return failures == 0;
    }
};

}  // namespace pulse::ui

int RunQuickPreviewGalleryProbe() {
    std::vector<wchar_t> dir(32768), data(32768);
    if (!GetEnvironmentVariableW(L"PULSE_TEST_GALLERY_DIR", dir.data(), static_cast<DWORD>(dir.size())) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data.data(), static_cast<DWORD>(data.size())))
        return 2;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    pulse::ui::QuickPreviewGalleryProbe::out = data.data();
    bool ok = false;
    if (GetEnvironmentVariableW(L"PULSE_TEST_GALLERY_DESKTOP", nullptr, 0)) {
        ok = pulse::ui::QuickPreviewGalleryProbe::Run(dir.data());
    } else {
        const auto name = L"PulseGalleryProbe-" + std::to_wstring(GetCurrentProcessId());
        HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) return 2;
        std::thread worker([&] { if (SetThreadDesktop(desktop)) ok = pulse::ui::QuickPreviewGalleryProbe::Run(dir.data()); });
        worker.join();
        CloseDesktop(desktop);
    }
    return ok ? 0 : 1;
}
