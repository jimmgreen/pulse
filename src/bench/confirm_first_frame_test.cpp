#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/ui_motion.h"
#include "../ui/window_helpers.h"
#include <dwmapi.h>
#include "confirm_caption_trace.h"

#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
using namespace pulse::ui;
using Clock = std::chrono::steady_clock;
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
double Ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Pixels {
    UINT width = 0, height = 0;
    std::vector<unsigned char> data;
};
Pixels ReadPng(const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Pixels p;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&p.width, &p.height))) return {};
    p.data.resize(static_cast<size_t>(p.width) * p.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, p.width * 4,
        static_cast<UINT>(p.data.size()), p.data.data()))) return {};
    return p;
}
void CompareContent(const std::wstring& early, const std::wstring& settled) {
    const auto a = ReadPng(early), b = ReadPng(settled);
    const bool valid = !a.data.empty() && a.width == b.width && a.height == b.height;
    Check(valid, "first and settled native PNGs decode with identical dimensions");
    if (!valid) return;
    size_t changed = 0, contrast = 0;
    // Include the complete title, message, item glyph/path and footer buttons.
    // Only the outer edge is excluded; an empty title must not escape the check.
    const UINT left = 12;
    const UINT top = 12;
    for (UINT y = top; y + 12 < a.height; ++y) {
        for (UINT x = left; x + 20 < a.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.width + x) * 4;
            int delta = 0;
            for (size_t c = 0; c < 3; ++c) delta += std::abs(int(a.data[i+c]) - int(b.data[i+c]));
            if (delta > 6) ++changed;
            if (x > left && std::abs(int(b.data[i]) - int(b.data[i-4])) > 20) ++contrast;
        }
    }
    std::printf("[PIXELS] changed=%zu settled_edges=%zu size=%ux%u\n",
                changed, contrast, a.width, a.height);
    Check(contrast > 100, "settled content is not an empty uniform surface");
    Check(changed == 0, "title message item and buttons are complete at first capture");
}
struct Run {
    std::wstring first, settled;
    bool capture = false;
    unsigned paints = 0, timers = 0;
    double opened_ms = 0, first_capture_ms = 0;
    Clock::time_point start;
};
LRESULT CALLBACK Observe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                         UINT_PTR id, DWORD_PTR data) {
    auto& run = *reinterpret_cast<Run*>(data);
    if (msg == WM_PAINT) ++run.paints;
    if (msg == WM_TIMER && wp == 0x43464D54) ++run.timers;
    if (msg == WM_TIMER && wp == 0x54455354) {
        KillTimer(hwnd, wp);
        if (run.capture) Check(SaveConfirmDialogFrame(hwnd, run.settled), "save settled native frame");
        PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Observe, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
ConfirmDialogSpec Spec() {
    ConfirmDialogSpec s;
    s.title = L"删除";
    s.message = L"确认将此文件移到回收站吗？";
    s.confirm_text = L"删除";
    s.cancel_text = L"取消";
    s.items = {L"C:\\PulseTest\\删除确认测试.txt"};
    return s;
}
void VisualRun(const std::filesystem::path& out, bool capture, int index) {
    auto spec = Spec();
    if (index >= 2 && capture) {
        spec.items = {L"\\\\server\\share\\" + std::wstring(130, L'长') + L"\\测试文件.txt"};
        spec.danger = true;
    }
    Run run;
    run.capture = capture;
    run.first = (out / (std::to_wstring(index) + L"-first.png")).wstring();
    run.settled = (out / (std::to_wstring(index) + L"-settled.png")).wstring();
    spec.on_open = [&](HWND hwnd) {
        if (!hwnd) return;
        run.opened_ms = Ms(run.start);
        Check(SetWindowSubclass(hwnd, Observe, 1, reinterpret_cast<DWORD_PTR>(&run)) != FALSE,
              "observe actual confirmation window");
        if (capture) {
            Check(SaveConfirmDialogFrame(hwnd, run.first), "save first native frame without message pumping");
            run.first_capture_ms = Ms(run.start);
        }
        Check(SetTimer(hwnd, 0x54455354, 750, nullptr) != 0, "arm bounded observation");
    };
    run.start = Clock::now();
    const auto choice = ShowConfirmDialogEx(nullptr, spec, index % 2 != 0, D2D1::ColorF(0x0078D4));
    Check(choice == ConfirmChoice::Cancel, "Escape cancels after observation");
    std::printf("[METRIC] sample=%d capture=%d open_ms=%.3f capture_ready_ms=%.3f paints=%u animation_timers=%u\n",
        index, capture ? 1 : 0, run.opened_ms, run.first_capture_ms, run.paints, run.timers);
    if (capture) CompareContent(run.first, run.settled);
    else Check(run.timers == 0, "idle confirmation does not schedule entrance redraws");
}
void InputChecks() {
    const auto accent = D2D1::ColorF(0x0078D4);
    for (int mode = 0; mode < 5; ++mode) {
        auto spec = Spec();
        if (mode == 1) { spec.danger = true; spec.default_choice = ConfirmChoice::Cancel; }
        if (mode == 2) { spec.confirm_again_text = L"确认删除"; spec.confirm_again_note = L"再次确认"; }
        if (mode == 3) spec.secondary_text = L"跳过";
        spec.on_open = [mode](HWND hwnd) {
            if (!hwnd) return;
            if (mode == 2) {
                SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
                Check(IsWindow(hwnd) != FALSE, "first Enter only arms double confirmation");
            }
            if (mode == 3) SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 0);
            PostMessageW(hwnd, WM_KEYDOWN, mode == 4 ? VK_ESCAPE : VK_RETURN, 0);
        };
        const auto expected = mode == 1 || mode == 4 ? ConfirmChoice::Cancel
                            : mode == 3 ? ConfirmChoice::Secondary : ConfirmChoice::Confirm;
        Check(ShowConfirmDialogEx(nullptr, spec, false, accent) == expected, "keyboard choice contract");
    }
    auto spec = Spec();
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(cancel != nullptr, "create borrowed cancellation event");
    if (cancel) {
        spec.cancel_event = cancel;
        spec.on_open = [cancel](HWND hwnd) { if (hwnd) SetEvent(cancel); };
        Check(ShowConfirmDialogEx(nullptr, spec, false, accent) == ConfirmChoice::Cancel, "external cancellation closes dialog");
        CloseHandle(cancel);
    }
}
#include "confirm_caption_accent_test.inc"
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    const bool capture = std::wstring(argv[2]) == L"visual";
    const std::wstring name = L"PulseConfirmTest-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 3;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 4;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::printf("[INPUT] real HWND isolated desktop, animations=%d; no file operations\n",
                pulse::ui::motion::SystemAnimationsEnabled() ? 1 : 0);
    if (std::wstring_view(argv[2]) == L"caption") {
        CaptionChecks(out);
        InputChecks();
    } else {
        for (int i = 0; i < 4; ++i) VisualRun(out, capture, i);
        if (capture) InputChecks();
    }
    std::printf("RESULT checks=%d failures=%d\n", checks, failures);
    CoUninitialize();
    CloseDesktop(desktop);
    return failures ? 1 : 0;
}
