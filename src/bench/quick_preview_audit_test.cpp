#include "../ui/quick_preview_window.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/text_contrast_scope.h"
#include "../ui/typography.h"
#include "../ui/ui_renderer.h"
#include <objbase.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <thread>
#include <psapi.h>
#include <chrono>
#include <wincodec.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
namespace pulse::ui {
struct QuickPreviewAuditTest {
    static bool VideoFeedback(QuickPreviewWindow& q) {
        std::vector<wchar_t> path(32768), output(32768);
        if (!GetEnvironmentVariableW(L"PULSE_TEST_VIDEO_FEEDBACK", path.data(), static_cast<DWORD>(path.size())) ||
            !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.data(), static_cast<DWORD>(output.size()))) return false;
        const auto root = std::filesystem::path(output.data());
        std::ofstream log(root / L"video-feedback.log");
        WIN32_FILE_ATTRIBUTE_DATA data{};
        bool ok = Check(GetFileAttributesExW(path.data(), GetFileExInfoStandard, &data) != FALSE,
            "read original video fixture without modifying it");
        if (!ok) return false;
        QuickPreviewItem item;
        item.path = path.data(); item.name = std::filesystem::path(item.path).filename().wstring();
        item.attrs = data.dwFileAttributes;
        item.size = (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
        item.modified = (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
        VideoPreview::State pending;
        pending.opening = true; pending.missing_decoder = true;
        ok &= Check(!q.CodecCardVisible(pending), "codec probe cannot show failure while a playback attempt is pending");
        q.Show(item, false, WindowEffect::None, false);
        const bool expect_failure = GetEnvironmentVariableW(L"PULSE_TEST_VIDEO_EXPECT_FAILURE", nullptr, 0) != 0;
        const ULONGLONG start = GetTickCount64();
        ULONGLONG settled = 0;
        bool saw_card = false, premature_card = false, card_then_picture = false;
        bool progressed = false, stable = true;
        int64_t first_position = -1;
        D2D1_RECT_F first_rect{};
        std::string previous;
        while (GetTickCount64() - start < 20000) {
            Pump(30);
            q.Render();
            const auto state = q.video_.Snapshot();
            const bool card = q.CodecCardVisible(state);
            premature_card |= card && state.opening;
            saw_card |= card;
            const bool playable = !state.opening && state.ready && !card && SUCCEEDED(state.error);
            card_then_picture |= playable && saw_card;
            const std::string summary = std::to_string(state.opening) + "," + std::to_string(state.ready) + "," +
                std::to_string(state.ffmpeg) + "," + std::to_string(card) + "," + std::to_string(state.error);
            if (summary != previous) {
                log << GetTickCount64() - start << "ms opening,ready,ffmpeg,card,error=" << summary << '\n';
                log.flush(); previous = summary;
            }
            if (playable && !q.fit_pending_) {
                const auto rect = q.VideoFrameRect(state);
                if (!settled) { settled = GetTickCount64(); first_position = state.position; first_rect = rect; }
                else stable &= rect.left == first_rect.left && rect.top == first_rect.top &&
                    rect.right == first_rect.right && rect.bottom == first_rect.bottom;
                progressed |= state.position > first_position + 20000000;
            } else if (!state.opening && (card || FAILED(state.error)) && !settled) settled = GetTickCount64();
            if (settled && GetTickCount64() - settled >= 5000) break;
        }
        const auto state = q.video_.Snapshot();
        ok &= Check(!state.opening && settled, "playback attempts reach a settled outcome");
        ok &= Check(!premature_card && !card_then_picture, "no failure card appears before successful playback");
        if (expect_failure)
            ok &= Check(!state.opening && (q.CodecCardVisible(state) || FAILED(state.error)),
                "unavailable decoder ends in an honest stable failure state");
        else {
            ok &= Check(state.ready && SUCCEEDED(state.error) && !q.CodecCardVisible(state), "original video becomes playable");
            ok &= Check(progressed && stable, "playback advances with stable picture bounds");
        }
        if (!expect_failure && state.ready) {
        HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 320, 180);
        HGDIOBJ old = SelectObject(memory, bitmap);
        unsigned char pixels[320 * 180 * 4]{};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 320; info.bmiHeader.biHeight = -180;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        const auto sample_rect = q.VideoFrameRect(q.video_.Snapshot());
        POINT origin{static_cast<LONG>((sample_rect.left + sample_rect.right) / 2 - 160), static_cast<LONG>((sample_rect.top + sample_rect.bottom) / 2 - 90)};
        ClientToScreen(q.hwnd_, &origin);
        unsigned long long previous_hash = 0;
        ULONGLONG sample_start = GetTickCount64(), changed = sample_start;
        unsigned changes = 0, stalls = 0, black_samples = 0, samples = 0; ULONGLONG longest = 0;
        std::atomic<bool> sampling_done{false};
        std::thread sampler([&] {
        while (GetTickCount64() - sample_start < 8000) {
            Sleep(8);
            BitBlt(memory, 0, 0, 320, 180, screen, origin.x, origin.y, SRCCOPY);
            SelectObject(memory, old);
            GetDIBits(memory, bitmap, 0, 180, pixels, &info, DIB_RGB_COLORS);
            SelectObject(memory, bitmap);
            unsigned long long hash = 1469598103934665603ULL;
            for (unsigned i = 0; i < sizeof(pixels); i += 4) { hash ^= pixels[i]; hash *= 1099511628211ULL; }
            unsigned lit = 0;
            for (unsigned i = 0; i < sizeof(pixels); i += 4) if (pixels[i] > 8 || pixels[i+1] > 8 || pixels[i+2] > 8) ++lit;
            ++samples; if (lit < 100) ++black_samples;
            const auto now = GetTickCount64();
            if (hash != previous_hash) {
                const auto gap = now - changed;
                if (changes && gap > 100) ++stalls;
                longest = (std::max)(longest, gap);
                previous_hash = hash; changed = now; ++changes;
            }
        }
        sampling_done.store(true);
        });
        while (!sampling_done.load()) {
            const auto pump_start = GetTickCount64();
            Pump(8);
            if (GetTickCount64() - pump_start > 100) log << "slow_pump_ms=" << GetTickCount64() - pump_start << '\n';
        }
        sampler.join();
        log << "black_samples=" << black_samples << "/" << samples << " presentation changes=" << changes << " gaps_over_100ms=" << stalls
            << " longest_ms=" << longest << " final_hold_ms=" << GetTickCount64() - changed << '\n';
        log.flush();
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
        }
        SetWindowPos(q.hwnd_, nullptr, 0, 0, 1000, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(100); q.Render();
        RECT resized{}; GetClientRect(q.hwnd_, &resized);
        ok &= Check(q.compositor_.Width() == resized.right && q.compositor_.Height() == resized.bottom,
            "actual window resize still updates presentation dimensions");
        ok &= Check(!l10n::Get(l10n::StringId::CanTShowThisVideoOnThisPC).empty(),
            "production localized preview messages are initialized");
        q.compositor_.CaptureNextFrame(); q.Render();
        q.Render();
        ok &= Check(q.compositor_.HeldFrame() != nullptr,
            "unchanged preview repaint retains presentation buffers and captured frame");
        const auto capture = root / L"video-feedback.png";
        ok &= Check(SaveCapturedPng(q.compositor_.Dc(), q.compositor_.HeldFrame(), capture.c_str()),
            "capture actual preview UI after playback settles");
        log << "final position=" << state.position << " ffmpeg=" << state.ffmpeg << " success=" << ok << '\n';
        q.Close();
        return ok;
    }
    static bool HoverGif(QuickPreviewWindow& q) {
        namespace fs = std::filesystem;
        const auto root = fs::absolute(fs::path(L"bench_data") /
            (L"hover-gif-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        fs::create_directories(root);
        std::vector<unsigned char> gif{'G','I','F','8','9','a',16,0,16,0,0x80,0,0,255,0,0,0,255,0};
        for (unsigned color : {0u, 1u}) {
            const unsigned char prefix[]{0x21,0xf9,4,0,10,0,0,0,0x2c,0,0,0,0,16,0,16,0,0,2};
            gif.insert(gif.end(), std::begin(prefix), std::end(prefix));
            std::vector<unsigned char> data;
            unsigned bits = 0, count = 0;
            const auto code = [&](unsigned value) {
                bits |= value << count; count += 3;
                while (count >= 8) { data.push_back(static_cast<unsigned char>(bits)); bits >>= 8; count -= 8; }
            };
            for (int pixel = 0; pixel < 256; ++pixel) { code(4); code(color); }
            code(5);
            if (count) data.push_back(static_cast<unsigned char>(bits));
            gif.push_back(static_cast<unsigned char>(data.size()));
            gif.insert(gif.end(), data.begin(), data.end()); gif.push_back(0);
        }
        gif.push_back(0x3b);
        const auto file = root / L"two-colors.gif";
        { std::ofstream output(file, std::ios::binary); output.write(reinterpret_cast<const char*>(gif.data()), gif.size()); }
        RenderCaches caches;
        MainRenderer renderer(caches);
        renderer.SetCompositor(&q.compositor_);
        HoverPreviewView view;
        view.visible = true; view.path = file.wstring(); view.name = file.filename().wstring();
        view.size = gif.size(); view.modified = 1; view.attrs = FILE_ATTRIBUTE_ARCHIVE;
        view.owner = q.hwnd_; view.shown_ms = GetTickCount64() - 500;
        view.anchor = D2D1::RectF(10, 10, 300, 40); view.pointer = D2D1::Point2F(30, 20);
        const auto client = D2D1::RectF(0, 0, static_cast<float>(q.compositor_.Width()), static_cast<float>(q.compositor_.Height()));
        const auto draw = [&] {
            auto* dc = q.compositor_.Dc();
            dc->BeginDraw(); dc->Clear(q.CurrentTheme().bg);
            renderer.painter_.BeginFrame(q.CurrentTheme(), false);
            renderer.DrawHoverPreview(view, client, q.CurrentTheme(), false);
            return SUCCEEDED(dc->EndDraw());
        };
        bool seen[2]{};
        bool ok = true;
        const auto until = GetTickCount64() + 8000;
        while (GetTickCount64() < until && !(seen[0] && seen[1])) {
            ok &= draw();
            if (renderer.hover_image_animation_.NextDelay(GetTickCount64()) &&
                !renderer.hover_image_animation_.Waiting()) {
                const auto frame = renderer.hover_image_animation_.Current();
                if (frame < 2 && !seen[frame]) {
                    seen[frame] = true;
                    const auto capture = root / (L"frame-" + std::to_wstring(frame) + L".png");
                    ComPtr<ID2D1Image> target;
                    ComPtr<ID2D1Bitmap1> bitmap;
                    q.compositor_.Dc()->GetTarget(&target);
                    if (target.get()) target->QueryInterface(IID_PPV_ARGS(&bitmap));
                    ok &= SaveCapturedPng(q.compositor_.Dc(), bitmap.get(), capture.c_str(), 100);
                    printf("hover_gif_capture=%ls\n", capture.c_str());
                }
            }
            Sleep(20);
        }
        ok &= Check(seen[0] && seen[1], "real two-frame GIF advances in the visible hover card");
        view.visible = false; ok &= draw();
        ok &= Check(renderer.HoverPreviewFrameMs() == 0 && renderer.hover_image_animation_.NextDelay(GetTickCount64()) == 0,
            "hidden hover card stops animation scheduling");
        return ok;
    }
    static bool PdfSwitch(QuickPreviewWindow& q) {
        namespace fs = std::filesystem;
        const auto root = fs::absolute(fs::path(L"bench_data") /
            (L"pdf-switch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        fs::create_directories(root);
        std::vector<QuickPreviewItem> items;
        for (int n = 0; n < 12; ++n) {
            const auto body = std::to_string((n % 3) * 0.3) + " 0.4 0.8 rg 20 20 180 260 re f\n";
            const auto stream = "<< /Length " + std::to_string(body.size()) + " >>\nstream\n" + body + "endstream";
            const std::vector<std::string> objects{
                "<< /Type /Catalog /Pages 2 0 R >>",
                "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 220 300] /Resources << >> /Contents 4 0 R >>", stream,
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 220] /Resources << >> /Contents 6 0 R >>", stream};
            std::string pdf = "%PDF-1.4\n";
            std::vector<size_t> offsets;
            for (size_t i = 0; i < objects.size(); ++i) {
                offsets.push_back(pdf.size());
                pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
            }
            const auto xref = pdf.size();
            pdf += "xref\n0 7\n0000000000 65535 f \n";
            for (auto offset : offsets) { char line[32]; std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset); pdf += line; }
            pdf += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
            const auto file = root / (L"document-" + std::to_wstring(n) + L".pdf");
            { std::ofstream output(file, std::ios::binary); output << pdf; }
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return false;
            QuickPreviewItem item; item.path = file.wstring(); item.name = file.filename().wstring();
            item.attrs = data.dwFileAttributes;
            item.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            item.modified = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            items.push_back(std::move(item));
        }
        q.Show(items.front(), false, WindowEffect::None, true);
        q.compositor_.NotifyDeviceLost(DXGI_ERROR_DEVICE_REMOVED);
        q.compositor_.dc_.reset(); // Failed recovery can leave this exact state.
        q.Render();
        bool ok = Check(q.compositor_.Dc() && !q.compositor_.NeedsRecovery(),
            "preview recovers when the lost device has no remaining context");
        if (!ok) return false;
        q.compositor_.NotifyDeviceLost(DXGI_ERROR_DEVICE_REMOVED);
        q.compositor_.dc_.reset();
        q.compositor_.hwnd_ = nullptr; // Force one recover attempt to fail without touching the driver.
        q.Render();
        ok &= Check(q.compositor_.NeedsRecovery() && q.preview_deadline_ > GetTickCount64(),
            "failed preview recovery schedules a paced retry");
        q.compositor_.hwnd_ = q.hwnd_;
        Pump(350);
        q.Render();
        ok &= Check(q.compositor_.Dc() && !q.compositor_.NeedsRecovery(),
            "preview retry restores the device without user input");
        const auto wait_ready = [&] {
            const auto until = GetTickCount64() + 8000;
            while (GetTickCount64() < until) {
                Pump(20); q.Render();
                std::lock_guard lock(q.thumbnails_.mutex_);
                for (const auto& [key, item] : q.thumbnails_.items_)
                    if (key.rfind(q.item_.path + L"\n", 0) == 0 && item.bitmap.get() && !item.failed &&
                        item.frame_count == 2 && q.pages_count_ == 2) return true;
            }
            return false;
        };
        for (size_t n = 0; n < items.size(); ++n) {
            q.Update(items[n]);
            ok &= Check(wait_ready(), "consecutive distinct PDF produces current document bitmap");
            if (n % 3 == 0) { q.Close(); Pump(180); q.Show(items[n], n % 2 != 0, WindowEffect::None, true); }
        }
        for (const auto& item : items) q.Update(item);
        ok &= Check(wait_ready() && q.item_.path == items.back().path,
            "rapid PDF cancellation settles on the final current document");
        Pump(300); // Capture settled navigation, not its initial transparent frame.
        q.compositor_.CaptureNextFrame();
        q.Render();
        const auto screenshot = root / L"final-pdf.png";
        ok &= Check(SaveCapturedPng(q.compositor_.Dc(), q.compositor_.HeldFrame(), screenshot.c_str(), 10000),
            "capture final PDF has substantial colored page content after switching and recovery");
        q.compositor_.ReleaseHeldFrame();
        printf("pdf_switch_capture=%ls\n", screenshot.c_str());
        q.Close();
        return ok;
    }
    static bool Check(bool ok,const char* label){printf("[%s] %s\n",ok?"PASS":"FAIL",label);fflush(stdout);return ok;}
    static void Pump(unsigned ms){const auto end=GetTickCount64()+ms;do{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(5);}while(GetTickCount64()<end);}
    static void Store(QuickPreviewWindow& q,uint32_t frame,ThumbnailCache::Item item){
        auto& cache=q.thumbnails_;
        const auto pixels=q.RequestedPixelSize(q.ContentRect());
        const auto key=cache.Key(q.item_.path,pixels,q.item_.modified,q.item_.size,frame);
        std::lock_guard lock(cache.mutex_);
        if(auto it=cache.items_.find(key);it!=cache.items_.end()){cache.lru_.erase(it->second.lru_position);cache.items_.erase(it);}
        cache.pending_.erase(key);cache.queue_.clear();cache.lru_.push_front(key);item.lru_position=cache.lru_.begin();cache.items_[key]=std::move(item);
    }
    static bool SaveCapturedPng(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap, const wchar_t* path,
                                size_t minimum_colored = 0) {
    if (!dc || !bitmap) return false;
    ComPtr<ID2D1Bitmap1> readable;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        bitmap->GetPixelFormat());
    const auto size = bitmap->GetPixelSize();
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, bitmap, nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size.width, size.height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(size.height, mapped.pitch, mapped.pitch * size.height, mapped.bits)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    size_t different = 0, colored = 0;
    for (UINT y = 0; y < size.height; ++y) {
        for (UINT x = 0; x < size.width; ++x) {
            const auto* pixel = mapped.bits + y * mapped.pitch + x * 4;
            different += std::memcmp(pixel, mapped.bits, 3) != 0;
            const auto low = (std::min)({pixel[0], pixel[1], pixel[2]});
            const auto high = (std::max)({pixel[0], pixel[1], pixel[2]});
            colored += high - low > 50;
        }
    }
    printf("captured_pixels width=%u height=%u nonuniform=%zu colored=%zu\n", size.width, size.height, different, colored);
    readable->Unmap();
    return ok && different > 200 && colored >= minimum_colored;
}
    static bool RunFramePerf() {
        const auto memory = [](const char* phase) {
            PROCESS_MEMORY_COUNTERS_EX m{}; m.cb = sizeof(m);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
                printf("preview_memory phase=%s private_commit=%llu working_set=%llu\n", phase,
                    static_cast<unsigned long long>(m.PrivateUsage), static_cast<unsigned long long>(m.WorkingSetSize));
            SYSTEM_INFO info{}; GetSystemInfo(&info);
            std::vector<ULONG_PTR> pages(1 + 2 * m.WorkingSetSize / info.dwPageSize + 256);
            if (K32QueryWorkingSet(GetCurrentProcess(), pages.data(), static_cast<DWORD>(pages.size() * sizeof(ULONG_PTR))) && pages[0] < pages.size()) {
                size_t private_pages = 0;
                for (size_t i = 1; i <= pages[0]; ++i) if (!(pages[i] & 256)) ++private_pages;
                printf("preview_resident phase=%s private_bytes=%llu\n", phase, static_cast<unsigned long long>(private_pages) * info.dwPageSize);
            }
        };
        OleInitialize(nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool ok = owner != nullptr;
        {
            QuickPreviewWindow q;
            ok &= Check(q.Initialize(owner, WM_APP + 201, WM_APP + 202), "initialize isolated frame probe");
            if (!ok) { if (owner) DestroyWindow(owner); OleUninitialize(); return false; }
            q.thumbnails_.running_ = true; // Inject owned payloads, never dispatch providers.
            QuickPreviewItem item;
            item.path = L"C:\\PulseAuditFixtures\\frame.log"; item.name = L"frame.log"; item.modified = 2; item.size = 65536;
            q.Show(item, false, WindowEffect::None, true);
            const bool ascii = GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_ASCII", nullptr, 0) > 0;
            const bool complex = GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_COMPLEX", nullptr, 0) > 0;
            const std::wstring complex_line = L" \xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67\x200D\xD83D\xDC66 \xD83D\xDC69\xD83C\xDFFD\x200D\xD83D\xDCBB \xD83C\xDDE8\xD83C\xDDF3 \x2764\xFE0F a\x0301\x0302\x0327 \xD83D\xDE00\xD83D\xDE03\xD83D\xDE04\n";
            std::wstring body = ascii ? L"ASCII log\n" : L"Mixed \x0627\x0644\x0639\x0631\x0628\x064a\x0629 e\x0301 \x6587\x4ef6 \xD83D\xDE00\n";
            const size_t limit = complex ? 8192 : ipc::kPreviewMaxTextChars;
            for (int line = 0; body.size() + (complex ? 200 : 100) < limit; ++line)
                body += L"2026-10-30 INFO request " + std::to_wstring(line) +
                    (complex ? complex_line : ascii ? L" completed; ASCII text\n" : L" completed; Unicode \x6587\x4ef6 \xD83D\xDE00\n");
            for (int kind = 0; kind < 2; ++kind) {
                ThumbnailCache::Item payload;
                if (kind == 0) { payload.kind = ipc::PreviewContentKind::Text; payload.text = body; }
                else {
                    item.path = L"C:\\PulseAuditFixtures\\frame.xlsx"; item.name = L"frame.xlsx"; q.Update(item);
                    payload.kind = ipc::PreviewContentKind::Table;
                    payload.text = L"PULSETBL\t1\nW\t1\t1\t0\nS\txlsx\tSheet1\t80\t4\t0\t\t0\n";
                    for (int row = 0; row < 80; ++row) payload.text += L"R\tFile " + std::to_wstring(row) + L"\t\x6587\x4ef6\t1024\t2026-10-30\n";
                }
                Store(q, 0, std::move(payload));
                const auto first_start = std::chrono::steady_clock::now(); q.Render();
                printf("preview_first kind=%d wall_ms=%.6f\n", kind,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first_start).count());
                ok &= Check(kind == 0 ? q.native_kind_ == QuickPreviewWindow::NativeKind::Text && q.preview_text_ == body && q.text_layout_.get()
                                     : q.native_kind_ == QuickPreviewWindow::NativeKind::Table && q.table_.HasData(),
                            kind == 0 ? "full protocol text reaches real layout" : "table payload reaches real view");
                if (kind == 0) {
                    LARGE_INTEGER freq{}, a{}, b{}; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&a);
                    for (int n = 0; n < 500; ++n) q.EnsureTextLayout(body, q.text_layout_hex_, q.text_layout_width_, q.text_layout_reading_);
                    QueryPerformanceCounter(&b);
                    printf("entry_compare chars=%zu repeats=500 total_ms=%.6f\n", body.size(), 1000.0 * (b.QuadPart-a.QuadPart)/freq.QuadPart);
                }
                if (kind == 0 && !GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_NO_CAPTURE", nullptr, 0)) {
                    wchar_t data_dir[32768]{};
                    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_dir, ARRAYSIZE(data_dir));
                    const auto snapshot = [&](const wchar_t* name) {
                        ThumbnailCache::Item capture_payload; capture_payload.kind = ipc::PreviewContentKind::Text;
                        capture_payload.text = body; Store(q, 0, std::move(capture_payload));
                        q.Render(); // Let an expensive new layout prepare its cache before comparison.
                        q.compositor_.CaptureNextFrame(); q.Render();
                        ok &= Check(q.native_kind_ == QuickPreviewWindow::NativeKind::Text && q.text_layout_.get(), "snapshot contains actual text at current size");
                        const std::wstring path = std::wstring(data_dir) + L"\\" + name + L".png";
                        ok &= Check(SaveCapturedPng(q.compositor_.Dc(), q.compositor_.HeldFrame(), path.c_str()), "capture nonblank real text preview");
                        q.compositor_.ReleaseHeldFrame();
                    };
                    snapshot(L"text_top");
                    const bool previous = GetEnvironmentVariableW(L"PULSE_TEST_VISIBLE_TEXT_BASELINE", nullptr, 0) > 0;
                    ok &= Check(TextOptimizationBaselineForTest() || ((previous || complex) ? ascii || q.text_commands_.get() : !q.text_commands_.get()),
                        "current text path uses its expected bounded resources");
                    q.text_scroll_ = 319.5f; snapshot(L"text_scroll");
                    q.sel_anchor_ = 400; q.sel_focus_ = 1800; snapshot(L"text_selection");
                    q.sel_anchor_ = q.sel_focus_ = 0;
                    q.dark_ = true; snapshot(L"text_dark"); q.dark_ = false;
                    q.OpenFind(); q.find_query_ = L"Unicode"; q.UpdateFindMatches();
                    snapshot(L"text_find"); q.CloseFind();
                    const float original_scale = q.scale_;
                    q.scale_ = 1.25f; q.RecreateFormats(); snapshot(L"text_scale125");
                    q.text_scroll_ = 70.0f; snapshot(L"text_scale125_scroll");
                    q.scale_ = 1.5f; q.RecreateFormats(); snapshot(L"text_scale150");
                    q.scale_ = original_scale; q.RecreateFormats();
                    RECT original{}; GetWindowRect(q.hwnd_, &original);
                    SetWindowPos(q.hwnd_, nullptr, 0, 0, 960, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    snapshot(L"text_wide");
                    SetWindowPos(q.hwnd_, nullptr, 0, 0, 430, 540, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    snapshot(L"text_minimum");
                    q.text_scroll_ = 1.0e7f; snapshot(L"text_end");
                    SetWindowPos(q.hwnd_, nullptr, 0, 0, original.right-original.left, original.bottom-original.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    q.text_scroll_ = 0;
                }
                for (int warm = 0; warm < 2; ++warm) {
                    const auto start = std::chrono::steady_clock::now(); q.Render();
                    printf("preview_warm kind=%d frame=%d wall_ms=%.6f\n", kind, warm,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
                if (kind == 0) printf("preview_cache active=%d\n", q.text_commands_.get() != nullptr);
                memory(kind == 0 ? "text_warm" : "table_warm");
                for (int round = 0; round < 3; ++round) {
                    std::vector<double> times, wall_times;
                    for (int frame = 0; frame < 12; ++frame) {
                        if (kind == 0) q.text_scroll_ = static_cast<float>((frame * 19) % 1200) +
                            (GetEnvironmentVariableW(L"PULSE_TEST_PREVIEW_FRACTIONAL", nullptr, 0) ? 0.5f : 0.0f);
                        const auto start = std::chrono::steady_clock::now();
                        q.Render(); times.push_back(q.last_render_ms_);
                        wall_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                        if (kind == 0 && frame == 0) printf("preview_parts kind=%d text_ms=%.6f total_ms=%.6f\n", kind, q.last_text_ms_, q.last_render_ms_);
                    }
                    std::sort(times.begin(), times.end());
                    std::sort(wall_times.begin(), wall_times.end());
                    printf("preview_frame kind=%s round=%d median_ms=%.6f p95_ms=%.6f\n", kind == 0 ? "text" : "table", round, times[6], times[11]);
                    printf("preview_wall kind=%s round=%d median_ms=%.6f max_ms=%.6f\n", kind == 0 ? "text" : "table", round, wall_times[6], wall_times[11]);
                    fflush(stdout);
                }
            }
            item.path = L"C:\\PulseAuditFixtures\\small.txt"; item.name = L"small.txt"; q.Update(item);
            ThumbnailCache::Item short_payload; short_payload.kind = ipc::PreviewContentKind::Text;
            short_payload.text = L"Small file \x6587\x4ef6 \xD83D\xDE00"; Store(q, 0, std::move(short_payload)); q.Render();
            ok &= Check(q.native_kind_ == QuickPreviewWindow::NativeKind::Text && q.preview_text_ == L"Small file \x6587\x4ef6 \xD83D\xDE00" && !q.text_commands_.get(),
                        "switching to a short file drops cached long text");
            item.path = L"C:\\PulseAuditFixtures\\source.txt"; item.name = L"source.txt"; q.Update(item);
            ThumbnailCache::Item large_payload; large_payload.kind = ipc::PreviewContentKind::Text;
            large_payload.text.assign(ipc::kPreviewMaxTextChars + 1024, L'x'); Store(q, 0, std::move(large_payload));
            q.Render(); q.last_text_ms_ = 9.0; q.Render();
            ok &= Check(q.preview_text_.size() > ipc::kPreviewMaxTextChars && !q.text_commands_.get(), "oversized source views do not grow a drawing cache");
            q.Close(); q.thumbnails_.running_ = false;
            ok &= Check(!q.text_commands_.get() && !q.text_commands_device_.get() && !q.text_commands_params_.get(), "close releases owned text drawing resources");
            memory("closed");
        }
        memory("destroyed");
        if (owner) DestroyWindow(owner);
        OleUninitialize(); return ok;
    }
    static bool Run(){
        OleInitialize(nullptr);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        struct OleScope { ~OleScope() { OleUninitialize(); } } ole_scope;
        HWND owner=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        bool ok=owner!=nullptr;
        {
            QuickPreviewWindow q;ok &= Check(q.Initialize(owner,WM_APP+201,WM_APP+202),"initialize private Quick Look window");
            if (GetEnvironmentVariableW(L"PULSE_TEST_VIDEO_FEEDBACK", nullptr, 0)) {
                ok &= VideoFeedback(q);
                DestroyWindow(owner);
                return ok;
            }
            if (GetEnvironmentVariableW(L"PULSE_TEST_HOVER_GIF", nullptr, 0)) {
                ok &= HoverGif(q);
                DestroyWindow(owner);
                return ok;
            }
            if (GetEnvironmentVariableW(L"PULSE_TEST_PDF_SWITCH", nullptr, 0)) {
                ok &= PdfSwitch(q);
                DestroyWindow(owner);
                return ok;
            }
            if (GetEnvironmentVariableW(L"PULSE_TEST_CHROME_CACHE", nullptr, 0)) {
                auto* format = q.compositor_.SmallFormat();
                auto first = q.UiLayout(L"中文 😀 long.extension", format, 240, 40);
                const auto created = q.ui_layout_creations_;
                const auto signs = q.ui_sign_creations_;
                for (int n = 0; n < 100; ++n) {
                    auto next = q.UiLayout(L"中文 😀 long.extension", format, 240, 40);
                    ok &= next.get() == first.get();
                }
                ok &= Check(first.get() && q.ui_layout_creations_ == created &&
                    q.ui_layout_hits_ >= 100, "warm chrome creates no layouts");
                for (int n = 0; n < 100; ++n)
                    q.UiLayout(std::to_wstring(n), format, 240, 40);
                ok &= Check(q.ui_layout_cache_.size() <= 64 && q.ui_layout_text_bytes_ <= 128 * 1024 &&
                    q.ui_sign_creations_ == signs, "bounded dynamic text reuses trimming sign");
                auto oversized = q.UiLayout(std::wstring(70000, L'x'), format, 240, 40);
                ok &= Check(q.ui_layout_text_bytes_ <= 128 * 1024, "oversize text bypasses cache");
                typography::InvalidateMeasurements();
                auto refreshed = q.UiLayout(L"中文 😀 long.extension", format, 240, 40);
                ok &= Check(refreshed.get() && refreshed.get() != first.get(), "font generation retires layouts");
                ComPtr<ID2D1SolidColorBrush> a, b;
                q.CreateUiBrush(q.compositor_.Dc(), D2D1::ColorF(0x123456), &a);
                const auto brushes = q.ui_brush_creations_;
                q.CreateUiBrush(q.compositor_.Dc(), D2D1::ColorF(0x123456), &b);
                ok &= Check(a.get() && a.get() == b.get() && q.ui_brush_creations_ == brushes,
                    "warm chrome reuses device brush");
                MainRenderer renderer;
                renderer.SetCompositor(&q.compositor_);
                const auto bounds = D2D1::RectF(0, 0, 180, 60);
                Theme theme{};
                q.compositor_.Dc()->BeginDraw();
                q.decoded_w_ = q.decoded_h_ = 32;
                q.image_backdrop_ = 3;
                q.image_fit_ = false;
                q.DrawImageBackdrop(q.compositor_.Dc(), bounds, bounds);
                ComPtr<ID2D1BitmapBrush> checker;
                checker.p = q.backdrop_brush_.get();
                if (checker.get()) checker->AddRef();
                q.DrawImageBackdrop(q.compositor_.Dc(), bounds, D2D1::RectF(10, 10, 180, 60));
                ok &= Check(checker.get() && q.backdrop_brush_.get() == checker.get(),
                    "checkerboard reuses bitmap brush when image moves");
                q.dark_ = !q.dark_;
                q.DrawImageBackdrop(q.compositor_.Dc(), bounds, bounds);
                ok &= Check(q.backdrop_brush_.get() && q.backdrop_brush_.get() != checker.get(),
                    "checkerboard changes pixels with theme");
                renderer.DrawCenteredIconName(L"中文 😀 file.extension", bounds, D2D1::ColorF(0xffffff),
                    theme, {}, nullptr, 2, nullptr, true);
                auto* grid_layout = renderer.icon_final_layouts_.empty() ? nullptr :
                    renderer.icon_final_layouts_.back().layout.get();
                renderer.DrawCenteredIconName(L"中文 😀 file.extension", bounds, D2D1::ColorF(0xffffff),
                    theme, {}, nullptr, 2, nullptr, false);
                ok &= Check(grid_layout && renderer.icon_final_layouts_.size() == 1 &&
                    renderer.icon_final_layouts_.back().layout.get() == grid_layout &&
                    renderer.icon_final_creations_ == 1 && renderer.icon_final_hits_ == 1,
                    "grid final layout reused with changing extension style");
                ComPtr<IUnknown> effect;
                if (grid_layout) grid_layout->GetDrawingEffect(0, &effect);
                ok &= Check(!effect.get(), "cached grid layout retains no device drawing effect");
                for (int n = 0; n < 4200; ++n) {
                    const auto name = L"缓存边界 " + std::to_wstring(n) + L".extension";
                    renderer.CellTextWidth(name);
                    renderer.DrawCenteredIconName(name, bounds, D2D1::ColorF(0xffffff),
                        theme, {}, nullptr, 2, nullptr, false);
                }
                ok &= Check(renderer.icon_final_layouts_.size() <= 512 &&
                    renderer.icon_final_text_bytes_ <= 512 * 1024,
                    "text caches survive 4200 unique names and bounded eviction");
                renderer.DrawWallTileChrome(bounds, true, false, theme);
                auto* gradient = renderer.wall_gradient_.get();
                renderer.DrawWallTileChrome(D2D1::RectF(20, 20, 200, 80), true, true, theme);
                ok &= Check(gradient && renderer.wall_gradient_.get() == gradient,
                    "photo wall reuses gradient across selected tiles");
                PaneViewModel drives;
                ListEntryView drive;
                drive.drive_total = 1000000000; drive.drive_free = 1000000;
                drive.type_text = L"Local disk";
                drives.entries.push_back(std::move(drive));
                const auto wide = renderer.ThisPcColumns(D2D1::RectF(0, 0, 1000, 500), drives);
                renderer.ThisPcColumns(D2D1::RectF(0, 0, 400, 500), drives);
                ok &= Check(renderer.this_pc_widths_.size() == 1 && wide.count > 1,
                    "This PC resize reuses natural column widths");
                drives.entries[0].drive_free = 999999999;
                renderer.ThisPcColumns(D2D1::RectF(0, 0, 1000, 500), drives);
                ok &= Check(renderer.this_pc_widths_.size() == 2,
                    "This PC capacity refresh invalidates natural widths");
                q.compositor_.Dc()->EndDraw();
                wchar_t capture_root[32768]{};
                const auto capture_relative = L"bench_data\\render-resource-20261101-" + std::to_wstring(GetCurrentProcessId());
                GetFullPathNameW(capture_relative.c_str(), ARRAYSIZE(capture_root), capture_root, nullptr);
                CreateDirectoryW(capture_root, nullptr);
                for (bool dark : {false, true}) {
                    auto* dc = q.compositor_.Dc();
                    ComPtr<ID2D1Image> target_image;
                    ComPtr<ID2D1Bitmap1> target_bitmap;
                    dc->GetTarget(&target_image);
                    if (target_image.get()) target_image->QueryInterface(IID_PPV_ARGS(&target_bitmap));
                    Theme capture_theme{};
                    capture_theme.bg = D2D1::ColorF(dark ? 0x202020 : 0xFFFFFF);
                    capture_theme.text = D2D1::ColorF(dark ? 0xFFFFFF : 0x202020);
                    dc->BeginDraw();
                    dc->Clear(capture_theme.bg);
                    renderer.DrawCenteredIconName(L"中文 😀 narrow-window very-long.extension",
                        D2D1::RectF(8, 8, 168, 68), capture_theme.text, capture_theme, {}, nullptr, 2, nullptr, true);
                    renderer.DrawCenteredIconName(L"回滚后的名称 another-file.tar.gz",
                        D2D1::RectF(8, 85, 168, 145), capture_theme.text, capture_theme, {}, nullptr, 2, nullptr, false);
                    const auto draw_result = dc->EndDraw();
                    const auto file = std::wstring(capture_root) + (dark ? L"\\grid-dark-narrow.png" : L"\\grid-light-narrow.png");
                    ok &= Check(SUCCEEDED(draw_result) && target_bitmap.get() &&
                        SaveCapturedPng(dc, target_bitmap.get(), file.c_str()),
                        dark ? "capture dark narrow grid names" : "capture light narrow grid names");
                    printf("render_capture=%ls\n", file.c_str());
                }
                printf("chrome layouts=%llu hits=%llu signs=%llu brushes=%llu\n",
                    q.ui_layout_creations_, q.ui_layout_hits_, q.ui_sign_creations_, q.ui_brush_creations_);
                q.Close();
                DestroyWindow(owner);
                return ok;
            }
            q.thumbnails_.running_=true;
            QuickPreviewItem item;item.path=L"C:\\PulseAuditFixtures\\workbook.xlsx";item.name=L"workbook.xlsx";item.modified=2;item.size=3;
            q.Show(item,false,WindowEffect::None,true);
            ThumbnailCache::Item workbook;workbook.kind=ipc::PreviewContentKind::Table;
            workbook.text=L"PULSETBL\t1\nW\t2\t1\t0\nS\txlsx\tSheet1\t1\t1\t0\t\t0\nR\tvalue\nS\txlsx\tSheet2\t0\t0\t0\tnot-loaded\t0\n";
            Store(q,0,std::move(workbook));q.Render();
            for (const auto kind : {QuickPreviewWindow::NativeKind::Table, QuickPreviewWindow::NativeKind::Tree}) {
                q.native_kind_=kind;
                const auto closed=q.ContentRect();
                q.OpenFind();
                RECT edit{};GetWindowRect(q.find_edit_,&edit);
                MapWindowPoints(nullptr,q.hwnd_,reinterpret_cast<POINT*>(&edit),2);
                ok &= Check(q.FindBarHeight()>0 && q.ContentRect().top>closed.top &&
                    edit.top>=q.FindBarRect().top && edit.bottom<=q.ContentRect().top,
                    kind==QuickPreviewWindow::NativeKind::Table ? "table search reserves space above content" :
                        "tree search reserves space above content");
                q.CloseFind();
                ok &= Check(q.ContentRect().top==closed.top && !IsWindowVisible(q.find_edit_),
                    "closing structured preview search restores content and hides edit");
            }
            q.native_kind_=QuickPreviewWindow::NativeKind::Table;
            ok &= Check(q.table_.Key(VK_NEXT,false,true)&&q.table_.IsSheetRequestPending(),"request nonfirst worksheet through table input");
            ThumbnailCache::Item failed;failed.failed=true;Store(q,1,std::move(failed));q.Render();
            ok &= Check(q.sheet_request_==1&&!q.table_.IsSheetRequestPending()&&q.table_.SelectedSheetIndex()==1,"production Render terminal failure ends correct sheet request after ResetAnimation");
            q.table_.Key(VK_PRIOR,false,true);q.Render();
            ok &= Check(q.table_.SelectedSheetIndex()==0 && !q.table_.IsSheetRequestPending(),
                "failed worksheet allows returning to the loaded first sheet");
            q.table_.Key(VK_NEXT,false,true);
            ThumbnailCache::Item recovered;recovered.kind=ipc::PreviewContentKind::Table;
            recovered.text=L"PULSETBL\t1\nW\t2\t1\t1\nS\txlsx\tSheet1\t0\t0\t0\tnot-loaded\t0\nS\txlsx\tSheet2\t1\t1\t0\t\t0\nR\trecovered\n";
            Store(q,1,std::move(recovered));q.Render();
            ok &= Check(q.table_.SelectedSheetIndex()==1 && !q.table_.IsSheetRequestPending() &&
                q.preview_text_.find(L"recovered")!=std::wstring::npos,
                "revisiting failed worksheet accepts successful retry through Render");
            item.path=L"C:\\PulseAuditFixtures\\retry.tiff";item.name=L"retry.tiff";q.Update(item);
            ThumbnailCache::Item retry;retry.failed=retry.transient=true;retry.retry_at=GetTickCount64()+150;Store(q,0,std::move(retry));q.Render();
            Pump(350);
            bool queued=false;{std::lock_guard lock(q.thumbnails_.mutex_);queued=!q.thumbnails_.queue_.empty();}
            ok &= Check(queued,"ordinary idle message loop wakes transient preview retry without manual paint");
            item.path=L"C:\\PulseAuditFixtures\\unicode.txt";item.name=L"unicode.txt";q.Update(item);
            const std::wstring unicode=L"A\U0001f600\U00020bb7e\u0301Z \u05d0\u05d1\u05d2";
            for (const float scale : {1.0f,1.5f,2.0f}) {
                q.scale_=scale;
                ThumbnailCache::Item text;text.kind=ipc::PreviewContentKind::Text;text.text=unicode;
                Store(q,0,std::move(text));q.Render();
                bool valid=q.native_kind_==QuickPreviewWindow::NativeKind::Text && q.text_layout_.get();
                bool boundaries[12]{};
                if (valid) {
                    // Exercise actual shaped glyph halves, including RTL, rather than guessed character widths.
                    q.text_layout_->SetFontSize(14.0f*scale,DWRITE_TEXT_RANGE{0,static_cast<UINT32>(unicode.size())});
                    for (UINT32 pos=0;pos<unicode.size();++pos) {
                        FLOAT x=0,y=0;DWRITE_HIT_TEST_METRICS metric{};
                        if (FAILED(q.text_layout_->HitTestTextPosition(pos,FALSE,&x,&y,&metric))) {valid=false;continue;}
                        for (const float part : {0.25f,0.75f}) {
                            uint32_t index=0;
                            const float px=q.TextOriginX()+metric.left+metric.width*part;
                            const float py=q.ContentRect().top+20.0f*scale-q.text_scroll_+metric.top+metric.height*0.5f;
                            if (!q.HitTestText(px,py,index) || index>unicode.size()) {valid=false;continue;}
                            // Forbidden boundaries split emoji, supplementary Han or the combining accent.
                            if (index==2 || index==4 || index==6) valid=false;
                            if (index<std::size(boundaries)) boundaries[index]=true;
                        }
                    }
                }
                ok &= Check(valid && boundaries[1] && boundaries[3] && boundaries[5] && boundaries[7],
                    scale==1.0f?"plain Quick Look Unicode cluster boundaries at 100% layout scale":
                    scale==1.5f?"plain Quick Look Unicode cluster boundaries at 150% layout scale":
                                "plain Quick Look Unicode cluster boundaries at 200% layout scale");
            }
            // --- P4 image chrome: footer chips, info drawer, rotation, backdrop, keyboard focus. ---
            q.scale_=1.0f;QuickPreviewWindow::info_open_=false;QuickPreviewWindow::image_backdrop_=0;
            item.path=L"C:\\PulseAuditFixtures\\photo.png";item.name=L"photo.png";q.Update(item);
            {
                ThumbnailCache::Item photo;photo.kind=ipc::PreviewContentKind::Bitmap;
                photo.w=400;photo.h=200;photo.stride=400*4;photo.pixels.assign(size_t{400}*200*4,0);
                for(size_t i=0;i<photo.pixels.size();i+=4){photo.pixels[i]=0x40;photo.pixels[i+1]=0x80;photo.pixels[i+2]=0xC0;photo.pixels[i+3]=(i/4)%400<200?0xFF:0x80;}
                photo.stats=ComputeImageStats(photo.pixels.data(),photo.w,photo.h,photo.stride);
                Store(q,0,std::move(photo));
            }
            q.Render();
            const auto has=[&q](int id){for(const auto& c:q.footer_controls_)if(c.id==id)return true;return false;};
            ok &= Check(q.native_kind_==QuickPreviewWindow::NativeKind::Bitmap && q.image_item_ &&
                has(QuickPreviewWindow::kFooterZoomOut) && has(QuickPreviewWindow::kFooterZoom) &&
                has(QuickPreviewWindow::kFooterZoomIn) && has(QuickPreviewWindow::kFooterRotate) &&
                has(QuickPreviewWindow::kFooterImageBg) && has(QuickPreviewWindow::kFooterInfo),
                "image footer offers zoom, rotate, backdrop and info controls");
            ok &= Check(q.ImageBackdropWanted(),"translucent picture gets the automatic checkerboard");
            const float full_right=q.ContentRect().right;
            q.ActivateFooterControl(QuickPreviewWindow::kFooterInfo);
            const float drawer=static_cast<float>(q.compositor_.Width())>=570.0f?250.0f:0.0f;
            ok &= Check(QuickPreviewWindow::info_open_ && full_right-q.ContentRect().right==drawer,
                "info drawer takes 250 px from the picture instead of covering it");
            q.Render();
            q.ActivateFooterControl(QuickPreviewWindow::kFooterInfo);
            ok &= Check(!QuickPreviewWindow::info_open_ && q.ContentRect().right==full_right,"closing the info drawer restores the view");
            q.Render();
            q.ActivateFooterControl(QuickPreviewWindow::kFooterRotate);q.Render();
            ok &= Check(q.image_turns_==1 && q.TurnedW()==200.0f && q.TurnedH()==400.0f &&
                !q.footer_parts_.empty() && q.footer_parts_[0]==L"200 \x00D7 400",
                "a quarter turn swaps the shown size in layout and footer");
            for(int i=0;i<3;++i)q.ActivateFooterControl(QuickPreviewWindow::kFooterRotate);
            ok &= Check(q.image_turns_==0,"four quarter turns return upright");
            for(int i=0;i<4;++i)q.ActivateFooterControl(QuickPreviewWindow::kFooterImageBg);
            ok &= Check(QuickPreviewWindow::image_backdrop_==0,"backdrop cycles auto, light, dark, checkerboard");
            q.Render();
            q.footer_focus_=0;
            const bool tabbed=q.CycleFooterFocus(false) && q.footer_focus_==q.footer_controls_.front().id;
            q.CycleFooterFocus(true);
            ok &= Check(tabbed && q.footer_focus_==0,"Tab enters the footer controls and Shift+Tab leaves them");
            // --- P5 performance: a full-size text payload scrolled frame by frame. ---
            item.path=L"C:\\PulseAuditFixtures\\long.log";item.name=L"long.log";q.Update(item);
            {
                std::wstring big;big.reserve(ipc::kPreviewMaxTextChars);
                for(int line=0;big.size()+80<ipc::kPreviewMaxTextChars;++line)
                    big+=L"2026-10-23 09:00:00.000 INFO worker["+std::to_wstring(line%16)+L"] request "+std::to_wstring(line)+L" served in 12 ms\n";
                ThumbnailCache::Item text;text.kind=ipc::PreviewContentKind::Text;text.text=big;text.truncated=true;
                Store(q,0,std::move(text));
            }
            q.Render();
            LARGE_INTEGER freq{},t0{},t1{};QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&t0);
            constexpr int kFrames=60;
            std::vector<double> draws,texts;draws.reserve(kFrames);texts.reserve(kFrames);
            for(int i=0;i<kFrames;++i){q.text_scroll_+=97.0f;q.Render();draws.push_back(q.last_render_ms_);texts.push_back(q.last_text_ms_);}
            QueryPerformanceCounter(&t1);
            const double per_frame=static_cast<double>(t1.QuadPart-t0.QuadPart)*1000.0/static_cast<double>(freq.QuadPart)/kFrames;
            double draw_total=0.0;for(const double d:draws)draw_total+=d;
            const double draw_avg=draw_total/kFrames;
            std::sort(draws.begin(),draws.end());std::sort(texts.begin(),texts.end());
            printf("text scroll: DrawTextPreview %.2f median, %.2f worst\n",texts[kFrames/2],texts.back());
            const double draw_median=draws[kFrames/2],draw_p95=draws[kFrames*95/100];
            printf("text scroll: %.2f ms per presented frame (vsync); building a frame %.2f ms avg, %.2f median, %.2f p95, %.2f worst over %d frames\n",
                   per_frame,draw_avg,draw_median,draw_p95,draws.back(),kFrames);
            // Present waits for vsync, so the budget applies to building the frame.
            ok &= Check(q.native_kind_==QuickPreviewWindow::NativeKind::Text && draw_median<8.0 && draw_p95<16.7,
                "scrolling a full text payload draws well within one 60 Hz frame");
            q.Close();const auto epoch=q.thumbnails_.epoch_.load();
            q.OnImagePackInstalled();q.OnExtraPackInstalled();q.OnMediaPackInstalled();
            ok &= Check(q.thumbnails_.epoch_.load()==epoch+3,"all installed-pack callbacks invalidate hidden Quick Look cache generations");
            q.thumbnails_.running_=false;
        }
        if(owner)DestroyWindow(owner);return ok;
    }
};
}
int RunQuickPreviewAuditTest(){
    if (GetEnvironmentVariableW(L"PULSE_TEST_VIDEO_FEEDBACK", nullptr, 0)) {
        pulse::compat::EnableDpiAwareness();
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        return pulse::ui::QuickPreviewAuditTest::Run() ? 0 : 1;
    }
    const auto name=L"PulseQuickPreviewAudit-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);if(!desktop)return 1;
    bool ok=false;std::thread worker([&]{if(SetThreadDesktop(desktop))ok=pulse::ui::QuickPreviewAuditTest::Run();});worker.join();CloseDesktop(desktop);return ok?0:1;
}

int RunQuickPreviewFramePerf() {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return 2;
    const auto name = L"PulseQuickPreviewFrame-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) return 2;
    bool ok = false;
    std::thread worker([&] { if (SetThreadDesktop(desktop)) ok = pulse::ui::QuickPreviewAuditTest::RunFramePerf(); });
    worker.join(); CloseDesktop(desktop); return ok ? 0 : 1;
}
