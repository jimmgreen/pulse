#include "../app/context_menu.h"
#include "../ui/fluent_menu.h"
#include "../shell_host/ctx_menu_icon.h"
#include <filesystem>
#include <cstdio>
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool NewFlyoutRefresh(FluentMenu& menu, const std::filesystem::path& output) {
        FluentMenuItem header; header.text = L"新建";
        for (const auto* title : {L"文件夹", L"文本文档", L"Markdown 文档", L"正在读取系统新建项…"}) {
            FluentMenuItem row; row.text = title; row.command = 8000 + static_cast<int>(header.children.size());
            row.enabled = header.children.size() != 3; header.children.push_back(std::move(row));
        }
        auto next = header; next.children.back().text = L"系统模板"; next.children.back().enabled = true;
        next.children.back().command = 8010;
        if (!menu.SaveDebugSnapshot((output / L"new-loading.png").c_str(), {header})) return false;
        menu.open_ = true;
        menu.OpenSubmenu(0);
        const bool opened = menu.first_sub_.parent_row == 0 && IsWindowVisible(menu.first_sub_.hwnd);
        const bool rejected = !menu.ReplaceItems({next}) && menu.model_.At(0)->children.back().command == 8003;
        menu.HideSubWindow();
        const bool accepted = menu.ReplaceItems({next}) && menu.model_.At(0)->children.back().command == 8010;
        menu.open_ = false;
        std::printf("[%s] real flyout open=%d replacement_rejected=%d close_then_accepted=%d\n",
            opened && rejected && accepted ? "PASS" : "FAIL", opened, rejected, accepted);
        return opened && rejected && accepted;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto desktop_name = L"PulseMenuReview-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 6;
    const auto output = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(output);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseContextMenuVisualFixture";
    RegisterClassW(&wc);
    HWND owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 700, 400,
        nullptr, nullptr, wc.hInstance, nullptr);
    int failures = 0;
    {
        pulse::ui::Compositor compositor;
        if (!owner || !compositor.Init(owner)) return 3;
        using pulse::app::ShellMenuEntry;
        auto entry = [](int command, const wchar_t* text, const wchar_t* verb) {
            ShellMenuEntry e; e.command = command; e.text = text; e.verb = verb; return e;
        };
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 16; bi.bmiHeader.biHeight = -16;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return 4;
        auto* pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
            pixels[y * 16 + x] = (x >= 2 && x < 14 && y >= 2 && y < 14) ?
                ((x < 8) ? 0xff387ae8u : 0xffe89b38u) : 0;
        HMENU native = CreatePopupMenu(); AppendMenuW(native, MF_STRING, 1, L"Provider fixture");
        SetMenuItemBitmaps(native, 0, MF_BYPOSITION, bitmap, nullptr);
        const auto provider_icon = pulse::shell::MenuItemIcon(native, 0);
        DestroyMenu(native); DeleteObject(bitmap);
        if (!provider_icon || !provider_icon->Valid()) return 5;
        auto archive = entry(0, L"压缩", L"compress");
        archive.children = {entry(5101, L"添加到压缩包…", L"compress"),
            entry(5102, L"解压到当前文件夹", L"extract")};
        auto more = entry(0, L"更多压缩操作", L"");
        more.children = {entry(5103, L"测试压缩包", L""), entry(5104, L"查看压缩包属性", L"properties")};
        archive.children.push_back(more);
        std::vector<ShellMenuEntry> shell{archive, entry(5105, L"在记事本中编辑", L"edit"),
            entry(5106, L"通过 QQ 发送到", L"sendto"), entry(5107, L"授予访问权限", L"sharing"),
            entry(5108, L"属性", L"properties")};
        // This is a native provider bitmap fixture, not a production fallback.
        shell[0].icon = provider_icon;
        shell[0].children[0].icon = provider_icon;
        shell[0].children[2].icon = provider_icon;
        shell[0].children[2].children[0].icon = provider_icon;
        std::vector<pulse::ui::FluentMenuItem> rows;
        pulse::app::AppendShellSection(rows, shell);
        for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
            compositor.RecreateTextFormats(scale);
            pulse::ui::FluentMenu menu;
            if (!menu.Create(owner, &compositor, scale)) { ++failures; continue; }
            menu.SetTheme(dark, D2D1::ColorF(0.2f, 0.4f, 0.8f));
            auto save = [&](const wchar_t* level, const auto& items) {
                const auto file = output / (std::wstring(level) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark.png" : L"-light.png"));
                const bool ok = menu.SaveDebugSnapshot(file.c_str(), items, 0);
                std::printf("[%s] renderer %ls scale=%d theme=%s\n", ok ? "PASS" : "FAIL", level,
                    static_cast<int>(scale * 100), dark ? "dark" : "light");
                failures += !ok;
            };
            if (scale == 1.0f && !dark && !pulse::ui::FluentMenuTestPeer::NewFlyoutRefresh(menu, output)) ++failures;
            save(L"root", rows);
            save(L"submenu", rows[0].children);
            save(L"third", rows[0].children[2].children);
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    return failures ? 1 : 0;
}
