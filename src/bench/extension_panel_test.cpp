#include "../ui/extension_panel.h"
#include "../common/windows_compat.h"
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <stdexcept>

namespace {
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct OwnerWindow {
    HWND hwnd = nullptr;
    ~OwnerWindow() { if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd); }
};
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
int VisiblePanelWindows() {
    int count = 0;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(window, name, 64);
        if (IsWindowVisible(window) && std::wstring_view(name) == L"Pulse.ExtensionPanel")
            ++*reinterpret_cast<int*>(parameter);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
    return count;
}
}

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    ComScope com;
    if (FAILED(com.result)) {
        std::fprintf(stderr, "[FAIL] COM initialization\n");
        return 1;
    }
    try {
        if (argc > 2) {
            std::fprintf(stderr, "Usage: pulse_extension_panel_test [absolute-output-directory]\n");
            return 2;
        }
        const auto output = argc == 2 ? std::filesystem::path(argv[1]) :
            std::filesystem::absolute(std::filesystem::path("bench_data") / "extension-panel-component");
        if (!output.is_absolute()) {
            std::fprintf(stderr, "Output directory must be absolute.\n");
            return 2;
        }
        std::filesystem::create_directories(output);
        std::ofstream report(output / "report.txt");
        if (!report) return 1;
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
            report.flush();
            if (!ok) ++failures;
            return ok;
        };
        OwnerWindow owner;
        owner.hwnd = CreateWindowExW(0, L"STATIC", L"Extension panel component fixture", WS_OVERLAPPEDWINDOW,
            80, 80, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!check(owner.hwnd != nullptr, "create hidden owner window")) return 1;
        pulse::ui::ExtensionPanel panel;
        pulse::ui::ExtensionPanelSpec spec;
        spec.title = L"Navigation tools";
        spec.description = L"Choose a navigation command for the current folder.";
        spec.buttons = {L"Back", L"Forward", L"Parent folder", L"Refresh"};
        int calls = 0;
        size_t last_index = 99;
        spec.on_command = [&](size_t index) { ++calls; last_index = index; };
        const D2D1_COLOR_F accent{0.0f, 0.47f, 0.84f, 1.0f};
        if (!check(panel.Show(owner.hwnd, spec, false, accent, 1.0f) && panel.IsOpen(),
            "show real modeless panel")) return 1;
        const auto first_script_instance = panel.Instance();
        Pump();
        check(panel.InvokeButton(2) && calls == 1 && last_index == 2, "button dispatch preserves exact index");
        check(!panel.InvokeButton(4) && calls == 1, "out of range button rejects without callback");
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        check(calls == 2 && last_index == 1, "Tab then Enter invokes the second button");
        const auto capture = [&](const wchar_t* prefix) {
            for (const bool dark : {false, true}) {
                for (const int percent : {100, 150, 200}) {
                    const std::wstring name = std::wstring(prefix) + (dark ? L"-dark-" : L"-light-") +
                        std::to_wstring(percent) + L".png";
                    const std::string label = std::string(prefix[0] == L'l' ? "long content " : "standard content ") +
                        (dark ? "dark " : "light ") + std::to_string(percent) + "% snapshot";
                    check(panel.SaveDebugSnapshot((output / name).c_str(), dark, static_cast<float>(percent) / 100.0f), label.c_str());
                }
            }
        };
        capture(L"panel");
        check(calls == 2, "snapshot rendering never invokes commands");
        MONITORINFO monitor{sizeof(monitor)};
        if (check(GetMonitorInfoW(MonitorFromWindow(panel.Hwnd(), MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
            "read panel monitor work area")) {
            RECT suggested{monitor.rcWork.left + 12, monitor.rcWork.top + 12,
                monitor.rcWork.left + 732, monitor.rcWork.top + 612};
            SendMessageW(panel.Hwnd(), WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
            RECT actual{};
            check(GetWindowRect(panel.Hwnd(), &actual) && actual.left == suggested.left && actual.top == suggested.top,
                "DPI transition preserves suggested position");
        }
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
        check(!panel.IsOpen(), "Escape closes the panel");
        check(!panel.InvokeButton(0) && calls == 2, "closed panel cannot dispatch");
        if (!check(panel.Show(owner.hwnd, spec, true, accent, 1.5f) && panel.IsOpen(), "closed panel reopens")) return 1;
        check(panel.Instance() != 0 && panel.Instance() != first_script_instance, "reopened panel cannot accept results for an earlier instance");
        panel.Close();
        auto long_spec = spec;
        long_spec.title = L"Navigation actions with deliberately long descriptive labels";
        long_spec.description.clear();
        while (long_spec.description.size() < 512)
            long_spec.description += L"This description verifies bounded wrapping and comfortable command spacing. ";
        long_spec.description.resize(512);
        long_spec.buttons.clear();
        for (size_t i = 0; i < 8; ++i) {
            std::wstring label = L"Action " + std::to_wstring(i + 1) + L" - ";
            while (label.size() < 128) label += L"long descriptive navigation label ";
            label.resize(128);
            long_spec.buttons.push_back(std::move(label));
        }
        if (!check(panel.Show(owner.hwnd, long_spec, false, accent, 1.0f), "show eight long command labels")) return 1;
        capture(L"long-panel");
        for (int i = 0; i < 7; ++i) SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        check(calls == 3 && last_index == 7, "keyboard reaches and invokes eighth button");
        panel.Close();
        auto field_spec = spec;
        int dock_closed = 0;
        auto dock_spec = spec;
        dock_spec.on_closed = [&] { ++dock_closed; };
        const RECT dock_bounds{300, 20, 620, 440};
        check(panel.ShowDocked(owner.hwnd, dock_bounds, dock_spec, false, accent, 1), "create real child compositor panel");
        check(panel.IsDocked() && GetParent(panel.Hwnd()) == owner.hwnd &&
            (GetWindowLongPtrW(panel.Hwnd(), GWL_STYLE) & WS_CHILD), "docked panel is a native child of its host");
        check(panel.SaveCurrentFrame((output / L"dock-child-light-100.png").c_str()), "real child compositor produces nonempty captured frame");
        const RECT scaled_dock{20, 10, 500, 640};
        panel.UpdateDocked(scaled_dock, true, accent, 1.5f);
        check(panel.SaveCurrentFrame((output / L"dock-child-dark-150.png").c_str()), "child compositor updates bounds theme and DPI");
        check(panel.SaveDebugSnapshot((output / L"dock-debug.png").c_str(), false, 1) && dock_closed == 0,
            "snapshot clone never invokes host close callback");
        check(panel.Undock(false, accent, 1) && !panel.IsDocked() && dock_closed == 1,
            "narrow fallback recreates as floating panel and releases dock once");
        panel.Close();
        check(dock_closed == 2, "floating close retains lifecycle notification");
        using Layout = pulse::ui::ExtensionPanelLayout;
        using Density = pulse::ui::ExtensionPanelDensity;
        check(pulse::ui::ExtensionPanelColumns(Layout::Auto, 4, 480) == 1 &&
            pulse::ui::ExtensionPanelColumns(Layout::Auto, 8, 480) == 2, "automatic layout preserves existing threshold");
        check(pulse::ui::ExtensionPanelColumns(Layout::Stack, 8, 960) == 1 &&
            pulse::ui::ExtensionPanelColumns(Layout::Grid, 4, 420) == 2 &&
            pulse::ui::ExtensionPanelColumns(Layout::Grid, 8, 419) == 1,
            "stack stays single column and grid falls back below 420 dip");
        auto styled_spec = long_spec;
        int styled_calls = 0;
        styled_spec.on_command = [&](size_t index) { if (index == 7) ++styled_calls; };
        styled_spec.layout = Layout::Stack;
        styled_spec.density = Density::Comfortable;
        check(panel.Show(owner.hwnd, styled_spec, false, accent, 1), "show explicit stacked long content");
        capture(L"stack-comfortable");
        RECT comfortable{}; GetWindowRect(panel.Hwnd(), &comfortable);
        styled_spec.density = Density::Compact;
        check(panel.Show(owner.hwnd, styled_spec, true, accent, 1), "show compact stack");
        RECT compact{}; GetWindowRect(panel.Hwnd(), &compact);
        check(compact.bottom - compact.top < comfortable.bottom - comfortable.top, "compact stack reduces bounded spacing and button height");
        capture(L"stack-compact");
        styled_spec.layout = Layout::Grid;
        check(panel.Show(owner.hwnd, styled_spec, false, accent, 1), "show compact grid");
        capture(L"grid-compact");
        SetWindowPos(panel.Hwnd(), nullptr, 0, 0, 320, 240, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < 7; ++i) SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        check(styled_calls == 1, "narrow short compact grid scrolls keyboard focus to eighth command");
        capture(L"grid-compact-narrow");
        panel.Close();
        auto grouped_spec = long_spec;
        grouped_spec.title = L"Grouped workspace commands";
        grouped_spec.description = L"Commands retain their identity when groups reorder them.";
        grouped_spec.groups = {
            {L"First group", L"The last command is intentionally first.", {7, 2}},
            {std::wstring(128, L'G'), std::wstring(512, L'd'), {0, 3, 4}},
            {L"Third group", L"One command.", {6}},
            {L"Final group", L"Keyboard focus scrolls into view.", {1, 5}}
        };
        grouped_spec.primary_button = 7;
        grouped_spec.title_tone = pulse::ui::ExtensionPanelTitleTone::Accent;
        std::vector<size_t> invoked_groups;
        grouped_spec.on_command = [&](size_t index) { invoked_groups.push_back(index); };
        check(panel.Show(owner.hwnd, grouped_spec, false, accent, 1) && invoked_groups.empty(),
            "grouped primary panel never executes implicitly");
        capture(L"groups-auto");
        for (size_t i = 0; i < 8; ++i) {
            SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
            SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        }
        check(invoked_groups == std::vector<size_t>{7, 2, 0, 3, 4, 6, 1, 5},
            "Tab follows visible group order and dispatch preserves original indices");
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        check(invoked_groups.size() == 9 && invoked_groups.back() == 7,
            "Tab wraps from close control to first visual grouped command");
        check(panel.InvokeButton(3) && invoked_groups.back() == 3, "direct invocation keeps original button identity");
        grouped_spec.layout = Layout::Grid;
        grouped_spec.density = Density::Compact;
        check(panel.Show(owner.hwnd, grouped_spec, true, accent, 1), "compact grouped grid opens");
        capture(L"groups-grid-compact");
        const RECT grouped_dock{0, 0, 480, 450};
        check(panel.ShowDocked(owner.hwnd, grouped_dock, grouped_spec, true, accent, 1.5f), "groups render in narrow dock child");
        for (size_t i = 0; i < 7; ++i) SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        check(invoked_groups.back() == 5 && panel.SaveCurrentFrame((output / L"groups-dock-scrolled-dark-150.png").c_str()),
            "narrow grouped dock scrolls last focused button into actual frame");
        check(pulse::ui::ExtensionPanel::SavePreviewSnapshot(owner.hwnd, grouped_spec,
            (output / L"groups-static-disabled.png").c_str(), false, accent, 1), "static preview retains groups and disabled primary styling");
        auto invalid_groups = grouped_spec;
        invalid_groups.groups[1].buttons[0] = 7;
        check(!panel.Show(owner.hwnd, invalid_groups, false, accent, 1), "UI rejects duplicate group indices");
        invalid_groups = grouped_spec;
        invalid_groups.groups.pop_back();
        check(!panel.Show(owner.hwnd, invalid_groups, false, accent, 1), "UI rejects omitted grouped buttons");
        invalid_groups = grouped_spec;
        invalid_groups.groups[0].buttons[0] = 99;
        check(!panel.Show(owner.hwnd, invalid_groups, false, accent, 1), "UI rejects out of range group index");
        invalid_groups = grouped_spec;
        invalid_groups.primary_button = 99;
        check(!panel.Show(owner.hwnd, invalid_groups, false, accent, 1), "UI rejects out of range primary index");
        const D2D1_COLOR_F white{1, 1, 1, 1}, black{0, 0, 0, 1};
        const auto tone = pulse::ui::ExtensionPanelTitleTone::Accent;
        check(pulse::ui::ExtensionPanelTitleColor(tone, false, white, white, black).r == 0 &&
            pulse::ui::ExtensionPanelTitleColor(tone, false, black, black, white).r == 1,
            "extreme accent titles fall back to readable foreground");
        check(pulse::ui::ExtensionPanelTitleColor(tone, true, black, white, white).r == 1,
            "high contrast title always uses host foreground");
        grouped_spec.groups.resize(1);
        grouped_spec.groups[0].buttons = {0, 1, 2, 3, 4, 5, 6, 7};
        check(panel.Show(owner.hwnd, grouped_spec, false, white, 1) &&
            panel.SaveCurrentFrame((output / L"groups-white-accent-light.png").c_str()), "bright accent title screenshot remains readable");
        check(panel.Show(owner.hwnd, grouped_spec, true, black, 2) &&
            panel.SaveCurrentFrame((output / L"groups-black-accent-dark-200.png").c_str()), "dark accent title screenshot remains readable");
        panel.Close();
        field_spec.fields = {{L"Folder", L"Initial folder"}, {L"Selection", L"0 selected"}, {L"View", L"Details"}};
        {
            auto availability_spec = field_spec;
            std::vector<bool> available(availability_spec.buttons.size(), true);
            int invoked = 0, availability_reads = 0;
            bool throw_read = false, close_read = false;
            availability_spec.on_command = [&](size_t) { ++invoked; };
            availability_spec.read_enabled = [&] {
                ++availability_reads;
                if (close_read) panel.Close();
                if (throw_read) throw std::runtime_error("availability unavailable");
                return available;
            };
            check(panel.Show(owner.hwnd, availability_spec, false, accent, 1.0f) && panel.ButtonEnabled(0),
                "availability read initializes enabled state");
            available[0] = false;
            check(!panel.InvokeButton(0) && invoked == 0 && !panel.ButtonEnabled(0),
                "invocation refresh rejects newly disabled command before timer");
            check(panel.SaveDebugSnapshot((output / L"availability-disabled-light.png").c_str(), false, 1.0f),
                "capture disabled button with production renderer");
            SendMessageW(panel.Hwnd(), WM_KEYDOWN, VK_RETURN, 0);
            check(invoked == 0, "keyboard cannot invoke disabled command");
            available[0] = true;
            const ULONGLONG availability_deadline = GetTickCount64() + 1500;
            while (!panel.ButtonEnabled(0) && GetTickCount64() < availability_deadline) {
                Pump();
                if (!panel.ButtonEnabled(0)) MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            check(panel.ButtonEnabled(0), "timer restores enabled presentation without clicking");
            check(panel.InvokeButton(0) && invoked == 1, "restored command can execute");
            throw_read = true;
            check(!panel.InvokeButton(0) && invoked == 1 && !panel.ButtonEnabled(0),
                "availability exception fails closed");
            throw_read = false;
            const auto correct = available;
            available.clear();
            check(!panel.RefreshEnabled() && !panel.ButtonEnabled(0), "incomplete availability fails closed");
            available = correct;
            check(panel.RefreshEnabled(), "availability recovers after incomplete read");
            const int before = availability_reads;
            check(panel.SaveDebugSnapshot((output / L"availability-enabled-dark.png").c_str(), true, 1.5f) &&
                availability_reads == before, "snapshot freezes availability without invoking host reader");
            close_read = true;
            check(!panel.InvokeButton(0) && !panel.IsOpen() && invoked == 1,
                "reentrant close during availability cannot execute stale command");
        }
        int reads = 0;
        bool close_during_read = false;
        std::vector<std::wstring> next_values{L"C:\\Fixture\\Updated", L"2 selected", L"List"};
        field_spec.read_fields = [&] {
            ++reads;
            if (close_during_read) panel.Close();
            return next_values;
        };
        if (!check(panel.Show(owner.hwnd, field_spec, false, accent, 1.0f), "show three read-only information fields")) return 1;
        check(reads == 0 && panel.FieldValues() == std::vector<std::wstring>{L"Initial folder", L"0 selected", L"Details"},
            "initial fields use supplied values without callback");
        check(panel.RefreshFields() && reads == 1 && panel.FieldValues() == next_values, "complete field refresh updates all values");
        check(panel.RefreshFields() && reads == 2 && panel.FieldValues() == next_values, "unchanged field refresh succeeds");
        const auto retained_values = panel.FieldValues();
        next_values = {L"Wrong count"};
        check(!panel.RefreshFields() && reads == 3 && panel.FieldValues() == retained_values,
            "mismatched field count preserves complete prior snapshot");
        next_values = {L"C:\\Fixture\\Timer", L"3 selected", L"Tiles"};
        const ULONGLONG timer_deadline = GetTickCount64() + 1500;
        while (reads == 3 && GetTickCount64() < timer_deadline) {
            Pump();
            if (reads == 3) MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        check(reads > 3 && panel.FieldValues() == next_values, "visible panel timer refreshes fields");
        next_values[0] = L"C:\\Fixture\\";
        while (next_values[0].size() < 512) next_values[0] += L"very-long-nested-folder-name\\";
        check(panel.RefreshFields() && panel.FieldValues() == next_values, "long path retained as field value");
        const int before_snapshot_reads = reads;
        check(panel.SaveDebugSnapshot((output / L"fields-long-light-100.png").c_str(), false, 1.0f), "long information fields light snapshot");
        check(panel.SaveDebugSnapshot((output / L"fields-long-dark-200.png").c_str(), true, 2.0f), "long information fields dark snapshot");
        check(reads == before_snapshot_reads, "snapshot clone never calls field reader");
        panel.Close();
        check(!panel.RefreshFields() && reads == before_snapshot_reads, "closed panel never reads fields");
        if (!check(panel.Show(owner.hwnd, field_spec, false, accent, 1.0f), "information panel reopens")) return 1;
        close_during_read = true;
        check(!panel.RefreshFields() && !panel.IsOpen(), "reader may close panel without applying stale values");
        close_during_read = false;
        if (!check(panel.Show(owner.hwnd, field_spec, false, accent, 1.0f), "information panel reopens after reentrant close")) return 1;
        const int before_owner_close_reads = reads;
        auto preview_spec = field_spec;
        int preview_commands = 0;
        int preview_reads = 0;
        int preview_enabled_reads = 0;
        preview_spec.on_command = [&](size_t) { ++preview_commands; panel.Close(); };
        preview_spec.read_fields = [&]() -> std::vector<std::wstring> {
            ++preview_reads;
            panel.Close();
            throw std::runtime_error("Preview must never read live fields");
        };
        preview_spec.read_enabled = [&]() -> std::vector<bool> {
            ++preview_enabled_reads;
            panel.Close();
            throw std::runtime_error("Preview must never read live availability");
        };
        const int visible_before_preview = VisiblePanelWindows();
        const HWND foreground_before_preview = GetForegroundWindow();
        check(pulse::ui::ExtensionPanel::SavePreviewSnapshot(owner.hwnd, preview_spec,
            (output / L"static-preview-disabled.png").c_str(), false, accent, 1.0f),
            "hidden static preview renders through production snapshot path");
        check(preview_commands == 0 && preview_reads == 0 && preview_enabled_reads == 0 && panel.IsOpen(),
            "static preview discards even destructive command, field and availability callbacks");
        check(VisiblePanelWindows() == visible_before_preview && GetForegroundWindow() == foreground_before_preview,
            "static preview creates no visible panel and does not change foreground");
        pulse::ui::ExtensionPanelSpec tree_preview;
        tree_preview.title = L"UI tree preview";
        tree_preview.tree = pulse::extensions::UiTree{};
        tree_preview.tree->root.id = "root";
        pulse::extensions::UiNode tree_text;
        tree_text.id = "text";
        tree_text.type = pulse::extensions::UiNodeType::Text;
        tree_text.binding = "context.directory";
        pulse::extensions::UiNode tree_button;
        tree_button.id = "button";
        tree_button.type = pulse::extensions::UiNodeType::Button;
        tree_button.text = "Preview must remain inert";
        tree_button.command = "refresh";
        tree_preview.tree->root.children = {tree_text, tree_button};
        tree_preview.tree_callbacks.on_command = [&](const std::string&) { ++preview_commands; };
        tree_preview.tree_callbacks.command_enabled = [&](const std::string&) { ++preview_enabled_reads; return true; };
        tree_preview.tree_callbacks.read_binding = [&](const std::string&) -> std::optional<std::string> {
            ++preview_reads; return "must not be read";
        };
        check(pulse::ui::ExtensionPanel::SavePreviewSnapshot(owner.hwnd, tree_preview,
            (output / L"static-tree-preview.png").c_str(), true, accent, 1.5f),
            "tree preview composites native child rendering into application PNG");
        check(preview_commands == 0 && preview_reads == 0 && preview_enabled_reads == 0,
            "tree preview discards all host callbacks before creating controls");
        DestroyWindow(owner.hwnd);
        owner.hwnd = nullptr;
        Pump();
        check(!panel.IsOpen() && panel.Hwnd() == nullptr, "owner destruction closes owned panel");
        check(!panel.InvokeButton(0) && calls == 3, "owner destruction revokes callback");
        check(!panel.RefreshFields() && reads == before_owner_close_reads, "owner destruction revokes field reader");
        report << "Result: " << (failures ? "FAIL" : "PASS") << " (" << failures << " failures)\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] component fixture: %s\n", error.what());
        return 1;
    }
}
