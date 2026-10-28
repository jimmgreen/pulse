#include "../app/app_internal.h"
#include "../app/address_path_open.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool ScrollLast(FluentMenu& menu) {
        menu.ScrollTo(menu.MaxScrollPx());
        return menu.scroll_y_ > 0 && menu.model_.RowTopPx(menu.model_.Count() - 1) - menu.scroll_y_ < menu.ViewportPx();
    }
    static bool Snapshot(FluentMenu& menu, const std::wstring& path) {
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < menu.model_.Count(); ++i) items.push_back(*menu.model_.At(i));
        return menu.SaveDebugSnapshot(path.c_str(), std::move(items), 0);
    }
    static bool Ready(FluentMenu& menu, int count) { return count > 0 ? menu.model_.Count() == count && menu.model_.At(0)->enabled : menu.model_.Count() == 1 && menu.model_.At(0)->text == l10n::Get(count == 0 ? l10n::StringId::EmptyFolder : l10n::StringId::DirectoryUnavailable); }
    static bool Compact(FluentMenu& menu, bool many) {
        std::cout << "menu width=" << menu.model_.WidthPx() << " body=" << menu.BodyHeightPx()
                  << " surface=" << menu.surf_.w << "x" << menu.surf_.h << " rows=" << menu.model_.Count() << std::endl;
        return menu.model_.WidthPx() >= 180 * menu.scale_ && menu.model_.WidthPx() <= 320 * menu.scale_ + 1 &&
            (!many || (menu.MaxScrollPx() > 0 && menu.BodyHeightPx() <= menu.model_.RowTopPx(10) + 4 * menu.scale_));
    }
    static bool Choose(FluentMenu& menu, const std::wstring& name) {
        for (int i = 0; i < menu.model_.Count(); ++i) {
            const auto* item = menu.model_.At(i);
            if (item->enabled && item->text == name) {
                menu.result_ = menu.InvokeRow(i);
                menu.Dismiss();
                return true;
            }
        }
        return false;
    }
};
}
namespace {
std::function<void()> menu_step;
void CALLBACK MenuTick(HWND, UINT, UINT_PTR, DWORD) { if (menu_step) menu_step(); }
}

int main() {
    std::cout << "[STAGE] main" << std::endl;
    using namespace pulse;
    OleInitialize(nullptr);
    std::cout << "[STAGE] OLE ready" << std::endl;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.searchHistory.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 1100, 720, nullptr, nullptr, nullptr, nullptr);
    std::cout << "[STAGE] initialize compositor" << std::endl;
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    std::cout << "[STAGE] compositor ready" << std::endl;
    s.compositor.RecreateTextFormats(1);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    const auto root = std::filesystem::temp_directory_path() /
        (L"Pulse-address-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"Sample Folder");
    const auto file = root / L"Sample File.txt";
    std::ofstream(file) << "address fixture";
    tab->current_path = fs::NormalizePath(root.wstring());
    const auto origin = tab->current_path;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        failures += !ok;
    };
    auto wait_probe = [&] {
        for (int i = 0; i < 300; ++i) {
            if (s.addressPathOpen) {
                std::lock_guard lock(s.addressPathOpen->mutex);
                if (s.addressPathOpen->result) return true;
            }
            Sleep(10);
        }
        return false;
    };
    ShowAddressEditor(s);
    const std::wstring quoted = L" \t\"" + file.wstring() + L"\"\r\n";
    check(AddressNavigationTarget(quoted) == file.wstring(), "quoted pasted path is cleaned");
    SetWindowTextW(s.hwndAddressEdit, quoted.c_str());
    SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_RETURN, 0);
    check(wait_probe(), "Enter starts background file probe");
    check(!s.addressEditing && tab->current_path == origin, "file Enter leaves folder unchanged");
    PollAddressPathOpen(s);
    check(!s.places.recent_items.empty() &&
        s.places.recent_items.front().path == fs::NormalizePath(file.wstring()),
        "file is dispatched through normal open route");
    ShowAddressEditor(s);
    SetWindowTextW(s.hwndAddressEdit, (root.wstring() + L"\\Sample").c_str());
    UpdateAddressSuggestions(s);
    for (int i = 0; i < 300 && s.addressSuggest.size() < 2; ++i) {
        Sleep(10);
        SendMessageW(s.hwndAddressEdit, WM_TIMER, 0xAC31, 0);
    }
    check(s.addressSuggest.size() == 2 && s.addressSuggestSel == -1,
        "file and directory suggestions do not replace pasted input");
    for (size_t i = 0; i < s.addressSuggest.size(); ++i) {
        if (s.addressSuggest[i].path.ends_with(L"\\")) {
            s.addressSuggestSel = static_cast<int>(i);
            SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_TAB, 0);
            wchar_t text[1024]{};
            GetWindowTextW(s.hwndAddressEdit, text, 1024);
            check(std::wstring(text) == (root / L"Sample Folder").wstring() + L"\\",
                "Tab completes directory with separator");
            break;
        }
    }
    HideAddressEditor(s, false);
    OpenAddressPath(s, file.wstring());
    check(wait_probe(), "second probe completed");
    const auto recent_count = s.places.recent_items.size();
    ++tab->view_generation;
    PollAddressPathOpen(s);
    check(s.places.recent_items.size() == recent_count && !s.addressPathOpen->result,
        "navigation change discards old open result");
    const auto vm = BuildVm(s, false);
    const auto bar = s.renderer.AddressBarRect(1100);
    bool arrow_hit = false, segment_hit = false;
    for (float x = bar.left; x < bar.right; x += 1) {
        const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, 1100, 720), x, (bar.top + bar.bottom) / 2);
        arrow_hit |= hit.region == ui::HitTestResult::BreadcrumbArrow && fs::NormalizePath(hit.path) == origin;
        segment_hit |= hit.region == ui::HitTestResult::BreadcrumbSegment && fs::NormalizePath(hit.path) == origin;
    }
    check(arrow_hit && segment_hit, "current folder arrow and name have separate hit targets");
    bool selected = false;
    const auto deadline = GetTickCount64() + 4000;
    menu_step = [&] {
        if (selected) return;
        if (s.menu && s.menu->IsOpen()) {
            selected |= ui::FluentMenuTestPeer::Choose(*s.menu, L"Sample Folder");
            if (!selected && GetTickCount64() > deadline) {
                s.menu->Dismiss();
                menu_step = {};
            }
        }
    };
    const auto timer = SetTimer(nullptr, 0, 50, MenuTick);
    ShowBreadcrumbChildren(s, origin, {100, 100});
    KillTimer(nullptr, timer);
    menu_step = {};
    check(selected && tab->current_path == fs::NormalizePath((root / L"Sample Folder").wstring()),
        "async breadcrumb menu selects and navigates to child directory");
    for (float scale : {1.0f, 1.5f}) {
    s.menu.reset();
    s.scale = scale;
    s.compositor.RecreateTextFormats(scale);
    for (int count : {1, 80, 0, -1}) {
        const auto fixture = root / (L"menu-" + std::to_wstring(count));
        if (count >= 0) std::filesystem::create_directories(fixture);
        for (int i = 0; i < count; ++i)
            std::filesystem::create_directory(fixture / (std::to_wstring(i) + std::wstring(count == 80 ? 120 : 3, L'x')));
        bool ready = false, compact = false;
        const auto start = GetTickCount64();
        menu_step = [&] {
            if (ready || !s.menu || !s.menu->IsOpen()) return;
            if (ui::FluentMenuTestPeer::Ready(*s.menu, count)) {
                ready = true;
                compact = ui::FluentMenuTestPeer::Compact(*s.menu, count > 10);
                std::cout << "data ready ms=" << GetTickCount64() - start << std::endl;
                check(ui::FluentMenuTestPeer::Snapshot(*s.menu, L"build/address-menu-" + std::to_wstring(count) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png"), "save bounded directory menu image");
                if (count > 10) check(ui::FluentMenuTestPeer::ScrollLast(*s.menu), "last directory reachable by scrolling");
                s.menu->Dismiss();
            } else if (GetTickCount64() - start > 4000) { ready = true; s.menu->Dismiss(); }
        };
        const auto sizing_timer = SetTimer(nullptr, 0, 15, MenuTick);
        ShowBreadcrumbChildren(s, fixture.wstring(), {100, 100});
        KillTimer(nullptr, sizing_timer);
        menu_step = {};
        check(ready && compact, count == 1 ? "small directory menu compact" : count == 80 ? "80 long directory names bounded and scrollable" : count == 0 ? "empty menu compact" : "unavailable menu compact");
    }
    }
    if (s.menu) {
        std::vector<ui::FluentMenuItem> items{{1, L"Sample Folder"}, {2, L"Other Folder"}};
        check(s.menu->SaveDebugSnapshot(L"build/address-folders-menu.png", items, 0), "save directory menu image");
    }
    DestroyWindow(s.hwndAddressEdit);
    s.hwndAddressEdit = nullptr;
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::filesystem::remove_all(root);
    state.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
