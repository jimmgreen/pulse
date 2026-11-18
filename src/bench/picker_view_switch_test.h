#pragma once
#include "picker_view_switch_test_api.h"
#include "../ui/ui_view_model.h"
#include <algorithm>

int RunPickerViewSwitchTest() {
    using namespace pulse::ui;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failed = 0;
    {
        Fixture fixture(true);
        const std::vector<unsigned char> pixels(96 * 96 * 4, 150);
        BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = info.biHeight = 96;
        info.biPlanes = 1; info.biBitCount = 32; info.biSizeImage = static_cast<DWORD>(pixels.size());
        for (int i = 0; i < 60; ++i) {
            std::ofstream out(fixture.root / (L"image-" + std::to_wstring(i) + L".bmp"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(&file), sizeof(file));
            out.write(reinterpret_cast<const char*>(&info), sizeof(info));
            out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        }
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        for (const auto mode : {PickerMode::File, PickerMode::Save}) {
            FolderPickerSpec spec; spec.mode = mode; spec.initial_path = fixture.root.wstring();
            spec.persist_layout = false;
            auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                PickerViewSwitchTest query;
                if (!Until([&] {
                    query = {};
                    return Send(hwnd, kPickerViewSwitchTestMessage, 0, reinterpret_cast<LPARAM>(&query)) && query.ready;
                }, done)) return false;
                const auto switch_view = [&](bool timer, ViewMode view) {
                    PickerViewSwitchTest request; request.begin = true; request.timer = timer;
                    request.mode = static_cast<int>(view);
                    return Send(hwnd, kPickerViewSwitchTestMessage, 0, reinterpret_cast<LPARAM>(&request));
                };
                // Populate actual image thumbnails before comparing identical warm switches.
                if (!switch_view(false, ViewMode::MediumIcons)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1800));
                for (bool timer : {true, false}) {
                    for (auto view : {ViewMode::Details, ViewMode::MediumIcons}) {
                        if (!switch_view(timer, view)) return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(850));
                        query = {};
                        if (!Send(hwnd, kPickerViewSwitchTestMessage, 0, reinterpret_cast<LPARAM>(&query))) return false;
                        std::vector<double> gaps;
                        for (size_t i = 1; i < query.count; ++i) {
                            const double elapsed = 1000.0 * (query.frames[i] - query.frames[0]) / frequency.QuadPart;
                            if (elapsed > 500.0) break;
                            gaps.push_back(1000.0 * (query.frames[i] - query.frames[i - 1]) / frequency.QuadPart);
                        }
                        std::sort(gaps.begin(), gaps.end());
                        const double median = gaps.empty() ? 0 : gaps[gaps.size() / 2];
                        std::printf("[VIEW-SWITCH] mode=%d target=%d clock=%s frames=%zu median_ms=%.2f p95_ms=%.2f\n",
                            static_cast<int>(mode), static_cast<int>(view), timer ? "legacy-timer" : "compositor",
                            gaps.size() + 1, median, gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100]);
                        if (gaps.size() < 5 || (!timer && median > 25.0)) ++failed;
                    }
                }
                // A switch interrupted by another switch must still close cleanly.
                if (!switch_view(false, ViewMode::Details) || !switch_view(false, ViewMode::MediumIcons)) return false;
                return PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
            });
            if (!outcome.driven || outcome.status != FilePickerStatus::Cancelled) ++failed;
        }
    }
    CoUninitialize();
    std::printf("[%s] actual open/save picker view switches (%d failures)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? 1 : 0;
}
