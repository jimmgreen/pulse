#ifdef PULSE_WITH_SELFTEST
#include "operation_toast_test.h"
#include "app_internal.h"
#include "../common/localization.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace pulse::app {
namespace {
LRESULT CALLBACK ToastOwner(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state && state->notification_toast.HandleMessage(hwnd, message, wp, lp)) return 0;
    return DefWindowProcW(hwnd, message, wp, lp);
}
void PumpToast() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
}

bool RunOperationToastTest() {
    const auto output = std::filesystem::absolute(L"bench_data/operation-toast");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    bool ok = log.good();
    const auto check = [&](bool value, const char* label) {
        log << (value ? "[PASS] " : "[FAIL] ") << label << std::endl;
        ok &= value;
        return value;
    };
    const auto fixture = output / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    if (!check(std::filesystem::create_directory(fixture), "isolated fixture created")) return false;
    const auto source = fixture / L"source.txt";
    const auto target = fixture / L"existing.txt";
    std::ofstream(source) << "source";
    std::ofstream(target) << "existing";
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->places.persist = false;
    state->appPrefs.persist = false;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = ToastOwner;
    wc.lpszClassName = L"PulseOperationToastTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_POPUP,
        -30000, -30000, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    state->hwnd = hwnd;
    if (check(hwnd && state->compositor.Init(hwnd), "hidden owner and graphics initialized")) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(fixture.wstring());
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        fs::DirEntry entry;
        entry.name = L"source.txt";
        entry.size = 6;
        GetSystemTimeAsFileTime(&entry.mtime);
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
        tab->selected_index = 0;
        tab->loading = false;
        state->operationWindow = std::make_unique<ui::FileOperationWindow>();
        check(state->operationWindow->Create(hwnd, {}), "operation window initialized");
        state->ops.Start([] {});
        struct Variant { const wchar_t* name; const wchar_t* language; bool dark; float scale; int width; int height; };
        for (const auto& variant : { Variant{L"light", L"zh-CN", false, 1.0f, 1000, 700},
                                    Variant{L"dark-narrow-150", L"zh-CN", true, 1.5f, 640, 540},
                                    Variant{L"english", L"en-US", false, 1.0f, 800, 600} }) {
            l10n::SetLanguage(variant.language);
            check(l10n::Get(l10n::StringId::RenameTargetExists) ==
                (std::wstring_view(variant.language) == L"zh-CN"
                    ? L"此文件夹中已存在同名项目，请换一个名称。"
                    : L"An item with this name already exists in this folder. Choose a different name."),
                "rename collision explanation is available in the selected language");
            state->darkMode = variant.dark;
            state->scale = variant.scale;
            SetWindowPos(hwnd, nullptr, 0, 0, variant.width, variant.height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            state->compositor.Resize(variant.width, variant.height);
            state->compositor.RecreateTextFormats(variant.scale);
            state->renderer.SetScale(variant.scale);
            const auto previous_id = state->ops.Status().task_id;
            ops::OpRequest request;
            request.type = ops::OpType::Rename;
            request.sources = {source.wstring()};
            request.new_name = L"existing.txt";
            state->ops.Submit(std::move(request));
            const auto start = GetTickCount64();
            while (GetTickCount64() - start < 5000) {
                const auto status = state->ops.Status();
                if (status.task_id != previous_id && !status.active) break;
                Sleep(10);
            }
            check(state->ops.Status().phase == ops::OpPhase::Failed, "real rename collision is rejected");
            const HWND focus = GetFocus();
            UpdateOperationWindow(*state, false);
            check(state->notification_toast.IsVisible() && IsWindowEnabled(hwnd) && GetFocus() == focus &&
                !state->operationWindow->IsVisible(), "rename error shows a toast without a modal window or focus change");
            auto* dc = state->compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0.0f));
            const auto theme = ui::MakeTheme(variant.dark, state->accentColor);
            state->renderer.Render(BuildVm(*state, false),
                D2D1::RectF(0, 0, static_cast<float>(variant.width), static_cast<float>(variant.height)), theme);
            state->notification_toast.Draw(state->compositor, theme, variant.scale, false);
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((output / (std::wstring(variant.name) + L".png")).c_str()),
                "operation toast screenshot saved");
            const auto bounds = state->notification_toast.Bounds();
            check(bounds.left >= 16 * variant.scale && bounds.right <= variant.width - 16 * variant.scale &&
                bounds.top >= 48 * variant.scale && bounds.top <= 64 * variant.scale && bounds.bottom <= variant.height &&
                std::abs(bounds.left + bounds.right - variant.width) < 1.0f,
                "error toast sits at the top center and fits narrow high-DPI windows");
            SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            UpdateOperationWindow(*state, false);
            check(!state->notification_toast.IsVisible(), "Escape dismisses error and repeated status does not reopen it");
        }
        state->notification_toast.ShowError(hwnd, L"Error", L"Transient error");
        const auto start = GetTickCount64();
        while (state->notification_toast.IsVisible() && GetTickCount64() - start < 7500) {
            PumpToast();
            Sleep(10);
        }
        check(!state->notification_toast.IsVisible(), "error toast dismisses automatically after six seconds");
        state->ops.Stop();
        check(std::filesystem::exists(source) && std::filesystem::exists(target) &&
            std::filesystem::file_size(source) == 6 && std::filesystem::file_size(target) == 8,
            "collision preserves both files and their contents");
        state->operationWindow.reset();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    if (hwnd) DestroyWindow(hwnd);
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    l10n::SetLanguage(L"zh-CN");
    std::filesystem::remove(source);
    std::filesystem::remove(target);
    check(std::filesystem::remove(fixture), "isolated fixture cleaned");
    return ok;
}
}
#endif
