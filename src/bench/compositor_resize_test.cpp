// compositor_resize_test.cpp - A swap chain that cannot be resized must not
// leave the window without a render target. When the monitor Pulse was on
// went away, Windows moved the window to another one, ResizeBuffers failed
// with DXGI_ERROR_INVALID_CALL, and every later frame drew into nothing: a
// white window until Pulse was restarted.
#include "../ui/ui_compositor.h"
#include "../ui/typography.h"
#include "../ui/text_contrast_scope.h"
#include <thread>

#include <cstdio>

using pulse::ui::ComPtr;
using pulse::ui::Compositor;

namespace {
int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

bool HasTarget(Compositor& compositor) {
    ComPtr<ID2D1Image> target;
    compositor.Dc()->GetTarget(&target);
    return target.get() != nullptr;
}

bool DrawFrame(Compositor& compositor) {
    ID2D1DeviceContext* dc = compositor.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.2f, 0.4f, 0.6f, 1.0f));
    if (FAILED(dc->EndDraw())) return false;
    compositor.Present();
    return !compositor.NeedsRecovery();
}
}  // namespace

int wmain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                L"Pulse compositor resize test", WS_POPUP | WS_VISIBLE,
                                -32000, -32000, 320, 240, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    const bool ready = hwnd && compositor.Init(hwnd);
    Check(ready, "initialize an off-screen compositor");
    if (ready) {
        Check(HasTarget(compositor) && DrawFrame(compositor), "the first frame draws");
        const bool system_contrast = pulse::ui::IsHighContrast();
        {
            pulse::ui::TextContrastScope scope(true);
            Check(pulse::ui::TextHighContrast() && !compositor.LumaTextEnabled() && !compositor.CustomEditEnabled(),
                  "high contrast frame keeps the native text fallback");
            {
                pulse::ui::TextContrastScope nested(false);
                Check(!pulse::ui::TextHighContrast(), "nested frame uses its own contrast snapshot");
            }
            Check(pulse::ui::TextHighContrast(), "nested frame restores outer contrast snapshot");
            {
                pulse::ui::TextContrastScope inactive(false, false);
                Check(pulse::ui::TextHighContrast(), "inactive test scope leaves its caller unchanged");
            }

            bool other_thread = !system_contrast;
            std::thread([&] { other_thread = pulse::ui::TextHighContrast(); }).join();
            Check(other_thread == system_contrast, "contrast snapshot is confined to its drawing thread");
        }
        Check(pulse::ui::TextHighContrast() == pulse::ui::IsHighContrast(),
              "outside the frame contrast is queried from the system again");
        compositor.RecreateTextFormats(1.0f);
        const auto generation = pulse::ui::typography::Generation();
        const float width = pulse::ui::typography::MeasureLine(&compositor, compositor.TextFormat(), L"Sample file 123");
        {
            Compositor auxiliary;
            const bool auxiliary_ready = auxiliary.Init(hwnd);
            Check(auxiliary_ready, "initialize auxiliary compositor on the shared device");
            if (auxiliary_ready) {
                auxiliary.RecreateTextFormats(1.0f);
                Check(pulse::ui::typography::Generation() == generation,
                      "initial auxiliary formats preserve main-window measurements");
                auxiliary.RecreateTextFormats(1.5f);
                Check(pulse::ui::typography::Generation() != generation,
                      "replacement formats still invalidate measurements");
                const float scaled = pulse::ui::typography::MeasureLine(&auxiliary, auxiliary.TextFormat(), L"Sample file 123");
                Check(scaled > width * 1.4f && scaled < width * 1.6f,
                      "DPI replacement measures at the new scale");
            }
        }


        bool kept = true;
        for (int i = 0; i < 40 && kept; ++i) {
            compositor.Resize(320 + (i % 7) * 37, 240 + (i % 5) * 29);
            kept = !compositor.NeedsRecovery() && HasTarget(compositor) && DrawFrame(compositor);
        }
        Check(kept, "forty resizes in a row, each after a drawn frame, keep a render target");

        // Hold the back buffer through the D2D target, as a reference that
        // is still alive does: ResizeBuffers answers DXGI_ERROR_INVALID_CALL.
        ComPtr<ID2D1Image> stale;
        compositor.Dc()->GetTarget(&stale);
        SetWindowPos(hwnd, nullptr, 0, 0, 400, 300, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        compositor.Resize(400, 300);
        Check(compositor.NeedsRecovery(), "a failed resize asks for a rebuild instead of drawing into nothing");
        stale.reset();
        Check(compositor.Recover(), "the rebuild succeeds");
        RECT client{};
        GetClientRect(hwnd, &client);
        Check(HasTarget(compositor) && compositor.Width() == client.right &&
                  compositor.Height() == client.bottom,
              "after the rebuild the target has the window's new size");
        Check(DrawFrame(compositor), "frames draw again");
        compositor.Shutdown();
    }
    if (hwnd) DestroyWindow(hwnd);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
