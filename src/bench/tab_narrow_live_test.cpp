#include "../app/app_internal.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <filesystem>
#include <wrl/client.h>
#include <set>
#include <cmath>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int);
namespace pulse::ui {
struct TitleLayoutTestAccess {
    static auto Geometry(MainRenderer& renderer, const WindowViewModel& vm, float width) {
        return renderer.ComputeTabStrip(vm, width);
    }
};
}
namespace {
bool CapturePresentedFrame(pulse::ui::Compositor& compositor, const std::wstring& path) {
    auto* source = compositor.HeldFrame();
    if (!source) return false;
    const auto size = source->GetPixelSize();
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> readback;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        source->GetPixelFormat());
    HRESULT result = compositor.Dc()->CreateBitmap(size, nullptr, 0, &properties, &readback);
    if (SUCCEEDED(result)) result = readback->CopyFromBitmap(nullptr, source, nullptr);
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(result) || FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
        mapped.pitch, mapped.pitch * size.height, mapped.bits, &bitmap);
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    readback->Unmap();
    return SUCCEEDED(result);
}
int RunTabProbe(pulse::AppState& s, FILE* report) {
    using namespace pulse;
    wchar_t output[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_TAB_OUTPUT", output, ARRAYSIZE(output));
    std::filesystem::create_directories(output);
    auto vm = BuildVm(s, false);
    vm.tabs.clear();
    vm.tab_groups.clear();
    vm.tab_layout = 0;
    vm.show_pinned_tab_names = false;
    for (int i = 0; i < 12; ++i) {
        ui::TabView tab;
        tab.title = i == 5 ? L"Desktop" : L"Documents and projects";
        tab.active = i == 5;
        tab.id = static_cast<uintptr_t>(i + 1);
        vm.tabs.push_back(tab);
    }
    constexpr float scale = 1.5f;
    s.renderer.SetScale(scale);
    s.renderer.SetVerticalTabs(false);
    s.compositor.RecreateTextFormats(scale);
    const auto theme = ui::MakeTheme(s.darkMode, s.accentColor);
    int failures = 0;
    for (float dpi : {1.f, 1.5f, 2.f}) {
        s.renderer.SetScale(dpi);
        s.compositor.RecreateTextFormats(dpi);
        auto crowded = vm;
        while (crowded.tabs.size() < 40) {
            auto tab = crowded.tabs.back(); tab.id = crowded.tabs.size() + 1;
            crowded.tabs.push_back(tab);
        }
        const float width = 1100 * dpi;
        auto geometry = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const float x = (geometry.view_left + geometry.view_right) * .5f;
        const bool consumed = s.renderer.ScrollTabStrip(crowded, width, x, 20 * dpi, -1);
        auto after = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const bool moved = consumed && after.scroll > geometry.scroll && after.hidden_indices.empty();
        s.renderer.ScrollTabStrip(crowded, width, x, 20 * dpi, -1000);
        after = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const auto bounds = D2D1::RectF(0, 0, width, 800 * dpi);
        auto arrows = [&] (const auto& strip, bool left, bool right) {
            if (dpi == 1.5f && left != right) {
                s.compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(800 * dpi));
                s.renderer.UpdateTabLayout(crowded, width);
                for (int frame = 0; frame < 45; ++frame) {
                    auto* dc = s.compositor.Dc(); dc->BeginDraw();
                    dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
                    s.renderer.Render(crowded, bounds, theme);
                    if (FAILED(dc->EndDraw())) return false;
                    if (frame == 44) s.compositor.CaptureNextFrame();
                    if (FAILED(s.compositor.Present())) return false;
                    Sleep(16);
                }
                const auto path = std::filesystem::path(output) / (left ? L"endpoint-last-150dpi.png" : L"endpoint-first-150dpi.png");
                if (!CapturePresentedFrame(s.compositor, path.wstring())) return false;
            }
            auto l = s.renderer.HitTest(crowded, bounds, strip.view_left - strip.arrow_w * .5f, 20 * dpi);
            auto r = s.renderer.HitTest(crowded, bounds, strip.view_right + strip.arrow_w * .5f, 20 * dpi);
            return (l.region == ui::HitTestResult::TabScroll && l.index == -1) == left &&
                (r.region == ui::HitTestResult::TabScroll && r.index == 1) == right;
        };
        const bool end_arrows = arrows(after, true, false);
        const bool end = after.scroll == after.max_scroll && after.item_rects.back().right <= after.view_right + .1f;
        s.renderer.ScrollTabStrip(crowded, width, x, 20 * dpi, 1000);
        after = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const bool start_arrows = arrows(after, false, true);
        s.renderer.ScrollTabStrip(crowded, width, x, 20 * dpi, -1);
        auto middle = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const bool middle_arrows = arrows(middle, true, true);
        const bool stable = middle.view_left == after.view_left && middle.view_right == after.view_right;
        fprintf(report, "[%s] endpoint arrows dpi=%.1f start=%d middle=%d end=%d stable=%d\n",
            start_arrows && middle_arrows && end_arrows && stable ? "PASS" : "FAIL", dpi,
            start_arrows, middle_arrows, end_arrows, stable);
        failures += !(start_arrows && middle_arrows && end_arrows && stable);
        const bool start = after.scroll == 0;
        crowded.tabs[5].active = false; crowded.tabs.back().active = true;
        after = ui::TitleLayoutTestAccess::Geometry(s.renderer, crowded, width);
        const bool reveal = after.item_rects.back().left >= after.view_left && after.item_rects.back().right <= after.view_right + .1f;
        const bool outside = !s.renderer.ScrollTabStrip(crowded, width, 0, 20 * dpi, -1);
        bool readable = true;
        for (size_t i = 0; i < crowded.tabs.size(); ++i)
            readable &= after.Width(crowded, i) >= 96 * dpi && !after.icon_only[i];
        const bool ok = moved && end && start && reveal && outside && readable;
        fprintf(report, "[%s] wheel dpi=%.1f moved=%d end=%d start=%d active_reveal=%d outside=%d readable=%d\n", ok ? "PASS" : "FAIL", dpi, moved, end, start, reveal, outside, readable);
        failures += !ok;
    }
    s.renderer.SetScale(scale);
    s.compositor.RecreateTextFormats(scale);
    // The complete title area crosses both sides of the label-width threshold.
    for (int width : {1680, 1848, 2016, 2200}) {
        constexpr int height = 1050;
        s.compositor.Resize(width, height);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
        s.renderer.UpdateTabLayout(vm, static_cast<float>(width));
        // Settle the production tab springs without dispatching app paints.
        for (int frame = 0; frame < 80; ++frame) {
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
            s.renderer.Render(vm, bounds, theme);
            const HRESULT draw = dc->EndDraw();
            if (FAILED(draw)) { ++failures; break; }
            if (frame == 79) s.compositor.CaptureNextFrame();
            if (FAILED(s.compositor.Present())) { ++failures; break; }
            Sleep(16);
        }
        int first = -1, last = -1, plus = -1;
        std::set<int> tabs;
        for (int x = 0; x < width; ++x) {
            const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), 24.f);
            if (hit.region == ui::HitTestResult::Tab || hit.region == ui::HitTestResult::TabClose) {
                if (first < 0) first = x;
                last = x;
                tabs.insert(hit.index);
            }
            if (hit.region == ui::HitTestResult::TabNew && plus < 0) plus = x;
        }
        // MakeTitleChrome reserves 264 DIP for right controls, then 64 DIP drag
        // area and 38 DIP new-tab area. The final inter-tab gap adds 2 DIP.
        const float expected_plus = static_cast<float>(width) - (264.f + 64.f + 36.f) * scale;
        const auto geometry = ui::TitleLayoutTestAccess::Geometry(s.renderer, vm, static_cast<float>(width));
        bool readable = true;
        for (size_t i = 0; i < vm.tabs.size(); ++i) readable &= geometry.Width(vm, i) >= 96 * scale && !geometry.icon_only[i];
        const bool filled = readable && plus >= 0 && (geometry.overflow || (tabs.size() == 12 && std::abs(plus - expected_plus) <= 3.f));
        const auto path = std::filesystem::path(output) / (L"tabs-" + std::to_wstring(width) + L"-150dpi.png");
        const bool captured = CapturePresentedFrame(s.compositor, path.wstring());
        fprintf(report, "[%s] width=%d scale=1.5 visible=%zu first=%d last=%d plus=%d expected_plus=%.1f capture=%d\n",
                filled && captured ? "PASS" : "FAIL", width, tabs.size(), first, last, plus, expected_plus, captured);
        failures += !filled || !captured;
    }
    return failures ? 1 : 0;
}
HWND target = nullptr;
HHOOK hook = nullptr;
int result = 2;
ULONGLONG started = 0;
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    auto* state = target ? pulse::GetAppState(target) : nullptr;
    if (!state || !state->startupComplete || !IsWindowVisible(target)) {
        if (GetTickCount64() - started > 30000) { KillTimer(nullptr, timer); PostQuitMessage(2); }
        return;
    }
    KillTimer(nullptr, timer);
    wchar_t report[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_TAB_REPORT", report, ARRAYSIZE(report));
    FILE* file = nullptr;
    if (_wfopen_s(&file, report, L"w") == 0 && file) {
        result = RunTabProbe(*state, file);
        std::fclose(file);
    }
    DestroyWindow(target);
    PostQuitMessage(result);
}
LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !target) {
        const auto* message = reinterpret_cast<CWPSTRUCT*>(lp);
        wchar_t name[128]{}; GetClassNameW(message->hwnd, name, ARRAYSIZE(name));
        if (wcscmp(name, L"PulseMainWindow") == 0) target = message->hwnd;
    }
    return CallNextHookEx(hook, code, wp, lp);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || wcscmp(argv[1], L"--test-instance") != 0) return 2;
    wchar_t profile[32768]{}, report[32768]{};
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", profile, ARRAYSIZE(profile)) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_TAB_REPORT", report, ARRAYSIZE(report))) return 2;
    std::filesystem::create_directories(profile);
    hook = SetWindowsHookExW(WH_CALLWNDPROC, Observe, nullptr, GetCurrentThreadId());
    if (!hook) return 2;
    started = GetTickCount64();
    SetTimer(nullptr, 0, 100, Probe);
    wchar_t args[] = L"--test-instance";
    wWinMain(GetModuleHandleW(nullptr), nullptr, args, SW_SHOWNORMAL);
    UnhookWindowsHookEx(hook);
    std::printf("TAB_NARROW_LIVE_EXIT=%d\n", result);
    return result;
}




