// selftest_1b2.cpp — Stage 1B-2 console self-test.
//
// Headless coverage for the parts `--shot` cannot reach (interactive menus,
// OLE drag & drop): menu model construction + hit-test + verb dispatch,
// IDataObject CF_HDROP contents, drop-effect modifier semantics, breadcrumb
// splitting, and real ops-layer create/move through pulse_shell.exe.
// All file operations are confined to bench_data/opstest/selftest_1b2 and
// cleaned up afterwards.
#include "selftest_1b2.h"
#include "scroll_hover_taskbar_test.h"
#include "change_tracking_ui_test.h"
#include "change_tracking_app_test.h"
#include "name_highlight_ui_test.h"
#include "rename_editor_test.h"
#include "operation_toast_test.h"
#include "filter_animation.h"
#include "tab_shortcuts.h"
#include "keymap.h"
#include "keymap_input.h"
#include "quick_access.h"
#include "../common/windows_compat.h"
#include "app_input.h"
#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "app_sidebar_refresh.h"
#include "../ui/address_search_layout.h"
#include "../ui/layer_alphas.h"
#include "../ui/details_tree_rows.h"
#include "search_query.h"
#include "../common/localization.h"
#include "app_model.h"
#include "app_commands.h"
#include "app_worker.h"
#include "entry_sort.h"
#include "snapshot_patch.h"
#include "entry_order_hold.h"
#include "sidebar_scrollbar_fade.h"
#include "session.h"
#include "app_prefs.h"
#include "startup_location.h"
#include "tray_reveal.h"
#include "blank_pane_click.h"
#include "details_column_menu.h"
#include "entry_group.h"
#include "context_menu.h"
#include "actions/action_args.h"
#include "ai_mcp_run.h"
#include "ai_settings.h"
#include "actions/action_inventory.h"
#include "actions/action_files.h"
#include "ai_cleanup_scan.h"
#include "actions/action_params.h"
#include "actions/action_policy.h"
#include "actions/action_registry.h"
#include "actions/action_state.h"
#include "context_menu_controller.h"
#include "context_menu_prefs.h"
#include "global_search_handoff.h"
#include "shell_verbs.h"
#include "places.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "details_meta.h"
#include "../ipc/ctx_menu_util.h"
#include "../index/index_engine.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../ops/elevated_delete.h"
#include "../common/current_user_security.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "../ui/fluent_menu.h"
#include "../ui/advanced_search_dialog.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include <map>
#include "../ui/color_picker.h"
#include "../ui/bloom_accent_picker.h"
#include "../ui/drag_drop.h"
#include "../ui/ui_renderer.h"
#include "../ui/preview_footer_layout.h"
#include "../ui/preview_format_catalog.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../common/text_format.h"
#include "../common/utf8_file.h"
#include "locked_item_prompt.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

bool RunWorkspaceRestoreTest();
bool RunListingSelectionRestoreTest();
bool RunContentPatternSelectionTest();
bool RunContentHistoryOrderTest();
bool RunOperationIntentUiTest();
int RunTrayCompletionTest();
bool RunTrayTagsUiTest();
bool RunTabPinBoundaryTest();
bool RunDialogRecoveryTest();
bool RunTagColorCommandTest();
bool RunPreviewDeadlineTest();
bool RunTagDeleteViewTest();
namespace pulse::app { bool RunSearchRoutingTest(); bool RunTabTransferTest(); }

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool SeedCachedEditor(FluentMenu& menu) {
        HWND external = menu.external_edit_;
        menu.external_edit_ = nullptr;
        const bool created = menu.EnsureFilterEdit();
        menu.HideFilterEdit();
        menu.external_edit_ = external;
        return created;
    }
    static bool ExternalEditorLayout(const FluentMenu& menu, HWND editor) {
        return menu.external_edit_ == editor && menu.FilterHeaderPx() == 0 &&
            (!menu.edit_ || !IsWindowVisible(menu.edit_)) &&
            menu.base_y_ + menu.kShadowMargin >= menu.anchor_rect_.bottom &&
            IsWindowVisible(editor);
    }
    static bool ScopeLayout(const FluentMenu& menu, float left, float bottom) {
        return menu.open_ && !menu.external_edit_ && !menu.filter_fn_ && !menu.anchor_to_rect_ &&
            std::abs(menu.base_x_ + menu.kShadowMargin - left) < 2 &&
            std::abs(menu.base_y_ + menu.kShadowMargin - bottom) < 2;
    }
    static bool DriveHistoryInteraction(FluentMenu& menu, int mode) {
        const HWND edit = menu.external_edit_ ? menu.external_edit_ : menu.edit_;
        if (!menu.open_ || !edit || menu.animating_out_) return false;
        if (mode == 2) SetWindowTextW(edit, L"new-unmatched-query-72941");
        if (mode == 1) SendMessageW(edit, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(edit, WM_KEYDOWN, mode == 0 ? VK_ESCAPE : VK_RETURN, 0);
        return true;
    }
    static bool SaveHistorySnapshot(HWND window, Compositor& compositor,
                                    const std::wstring& path, bool dark, float scale) {
        FluentMenu menu;
        if (!menu.Create(window, &compositor, scale)) return false;
        menu.SetTheme(dark, D2D1::ColorF(0x0078D4));
        menu.SetMaxVisibleRows(9);
        menu.SetFilterMinWidth(440.0f);
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < 12; ++i) {
            FluentMenuItem item;
            item.command = 10 + i;
            item.trailing_command = 100 + i;
            item.text = i == 1 ? L"很长的历史搜索查询 project specification draft" : L"项目搜索 " + std::to_wstring(i + 1);
            item.shortcut = i % 2 == 0 ? L"当前文件夹" : L"全局";
            item.glyph = L"\xE81C";
            items.push_back(std::move(item));
        }
        return menu.SaveDebugSnapshot(path.c_str(), std::move(items), 1);
    }
    static bool CheckHistoryRows(float scale) {
        FluentMenu menu;
        menu.scale_ = scale;
        FluentMenuItem item;
        item.command = 10;
        item.trailing_command = 20;
        item.text = L"History query";
        menu.model_.SetItems(std::vector<FluentMenuItem>(51, item));
        menu.model_.Layout(nullptr, scale, 400.0f * scale);
        menu.SetMaxVisibleRows(9);
        if (std::abs(menu.BodyHeightPx() - 332.0f * scale) > 0.01f) return false;
        const float trailing_x = menu.kShadowMargin + menu.model_.WidthPx() - 20.0f * scale;
        if (menu.InvokeAt(0, trailing_x) != 20 || menu.InvokeRow(0) != 10 ||
            menu.InvokeAt(0, menu.kShadowMargin + 50.0f * scale) != 10) return false;
        menu.UpdateHover(50);
        if (menu.scroll_y_ <= 0 || menu.model_.RowTopPx(50) + menu.model_.RowHeightPx() >
            menu.scroll_y_ + menu.BodyHeightPx() + 0.01f) return false;
        menu.UpdateHover(0);
        if (menu.scroll_y_ > menu.model_.RowTopPx(0)) return false;
        item.enabled = false;
        menu.model_.SetItems({item});
        menu.model_.Layout(nullptr, scale, 400.0f * scale);
        return menu.InvokeAt(0, trailing_x) == 0 && menu.InvokeRow(0) == 0;
    }
    static void FillOverflowMenu(FluentMenu& menu, float scale, int count) {
        menu.scale_ = scale;
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < count; ++i) {
            FluentMenuItem item;
            item.command = 10 + i;
            item.text = L"Context verb " + std::to_wstring(i + 1);
            item.separator_after = i % 7 == 6;
            items.push_back(std::move(item));
        }
        menu.model_.SetItems(std::move(items));
        menu.model_.Layout(nullptr, scale, 300.0f * scale);
    }
    static bool CheckOverflowFit(float scale) {
        FluentMenu menu;
        FillOverflowMenu(menu, scale, 6);
        menu.ApplyHeightLimit(400.0f * scale);
        if (menu.overflow_ || menu.ArrowPx() != 0.0f ||
            std::abs(menu.BodyHeightPx() - static_cast<float>(menu.model_.HeightPx())) > 0.01f)
            return false;
        FillOverflowMenu(menu, scale, 60);
        menu.ApplyHeightLimit(400.0f * scale);
        return menu.overflow_ && menu.Scrollable() && menu.ArrowPx() > 0.0f &&
            std::abs(menu.BodyHeightPx() - 400.0f * scale) < 0.01f &&
            menu.MaxScrollPx() > 0.0f && menu.scroll_y_ == 0.0f;
    }
    static bool CheckOverflowPointer(float scale) {
        FluentMenu menu;
        FillOverflowMenu(menu, scale, 60);
        menu.ApplyHeightLimit(400.0f * scale);
        menu.open_ = true;
        const float arrow = menu.ArrowPx();
        auto at = [&](float body_y) {
            return POINT{ menu.kShadowMargin + 40, static_cast<LONG>(menu.kShadowMargin + body_y) };
        };
        bool ok = true;
        menu.OnMouse(at(arrow * 0.5f), false);
        ok = ok && menu.arrow_hover_ == -1 && menu.hover_row_ == -1;
        menu.OnMouse(at(arrow + menu.model_.RowTopPx(0) + 2.0f), false);
        ok = ok && menu.arrow_hover_ == 0 && menu.hover_row_ == 0;
        const float bottom = menu.BodyHeightPx() - arrow * 0.5f;
        menu.OnMouse(at(bottom), false);
        ok = ok && menu.arrow_hover_ == 1 && menu.hover_row_ == -1;
        menu.OnMouse(at(bottom), true); // a click pages down without invoking anything
        const float paged = menu.scroll_y_;
        ok = ok && paged > 0.0f && menu.result_ == 0 && menu.open_;
        // Rows under the pointer account for the arrow strip and the offset.
        menu.OnMouse(at(arrow + 2.0f), false);
        ok = ok && menu.hover_row_ == menu.model_.HitTestRow(paged + 2.0f);
        const float before = menu.scroll_y_;
        menu.OnMouse(at(arrow * 0.5f), true); // and the top arrow pages back up
        ok = ok && menu.scroll_y_ < before;
        menu.open_ = false;
        return ok;
    }
    static bool CheckOverflowKeysAndWheel(float scale) {
        FluentMenu menu;
        FillOverflowMenu(menu, scale, 60);
        menu.ApplyHeightLimit(400.0f * scale);
        menu.UpdateHover(59);
        const float row_bottom = menu.model_.RowTopPx(59) + menu.model_.RowHeightPx();
        if (std::abs(menu.scroll_y_ - menu.MaxScrollPx()) > 0.01f ||
            row_bottom > menu.scroll_y_ + menu.ViewportPx() + 0.01f) return false;
        menu.UpdateHover(0);
        if (menu.scroll_y_ != 0.0f) return false;
        if (menu.ScrollTo(-50.0f)) return false; // already at the top
        if (!menu.ScrollTo(1.0e6f) || std::abs(menu.scroll_y_ - menu.MaxScrollPx()) > 0.01f)
            return false;
        return menu.hover_row_ == -1; // scrolling drops the stale hover
    }
    static bool CheckOverflowArrowHover(float scale) {
        FluentMenu menu;
        FillOverflowMenu(menu, scale, 60);
        menu.ApplyHeightLimit(400.0f * scale);
        menu.arrow_hover_ = 1;
        menu.TickArrowScroll(); // the first tick only starts the clock
        if (menu.scroll_y_ != 0.0f) return false;
        menu.arrow_tick_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(70);
        menu.TickArrowScroll();
        const float moved = menu.scroll_y_;
        // One whole row per step: the next row's top lines up with the viewport.
        if (std::abs(moved - menu.model_.RowTopPx(1)) > 0.01f)
            return false;
        menu.arrow_hover_ = 0; // the pointer left the arrow
        menu.arrow_tick_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(70);
        menu.TickArrowScroll();
        if (menu.scroll_y_ != moved) return false;
        // Paging and stepping back always land on a row top.
        menu.ScrollRows(5);
        menu.ScrollRows(-1);
        return std::abs(menu.scroll_y_ - menu.model_.RowTopPx(5)) < 0.01f;
    }
    static bool SaveOverflowSnapshot(HWND window, Compositor& compositor,
                                     const std::wstring& path, bool dark, float scale) {
        FluentMenu menu;
        if (!menu.Create(window, &compositor, scale)) return false;
        menu.SetTheme(dark, D2D1::ColorF(0x0078D4));
        const wchar_t* verbs[] = { L"打开", L"在新标签页中打开", L"在新窗口中打开", L"打开方式",
            L"用 Visual Studio Code 打开", L"使用 Microsoft Defender 扫描", L"授予访问权限",
            L"还原以前的版本", L"发送到", L"剪切", L"复制", L"创建快捷方式", L"删除",
            L"重命名", L"属性" };
        const wchar_t* glyphs[] = { L"\xE8E5", L"\xE8A7", L"\xE8C8", L"\xE8C6", L"\xE77F",
            L"\xE8AC", L"\xE74D", L"\xE946" };
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < 15; ++i) {
            FluentMenuItem item;
            item.command = 10 + i;
            item.text = verbs[i];
            item.glyph = glyphs[i % 8];
            item.separator_after = i == 3 || i == 8 || i == 11;
            items.push_back(std::move(item));
        }
        // Lay out once to size the limit, then render the same rows mid-scroll
        // with the pointer resting on the bottom arrow.
        menu.model_.SetItems(items);
        menu.model_.Layout(compositor.DwriteFactory(), scale, 0.0f);
        menu.ApplyHeightLimit(360.0f * scale);
        menu.ScrollRows(4);
        menu.arrow_hover_ = 1;
        return menu.overflow_ && menu.SaveDebugSnapshot(path.c_str(), std::move(items), -1);
    }
    static bool CheckSubmenuColors(float scale) {
        FluentMenu menu;
        menu.scale_ = scale;
        FluentMenuItem strip;
        for (int i = 0; i < 8; ++i) {
            FluentMenuSwatch swatch;
            swatch.command = 90 + i;
            strip.quick_swatches.push_back(swatch);
        }
        FluentMenuItem clear;
        clear.command = 120;
        clear.text = L"No color dot";
        menu.sub_model_.SetItems({strip, clear});
        menu.sub_model_.Layout(nullptr, scale);
        // Deliberately different parent width: submenu hits must use its own geometry.
        menu.model_.SetItems({clear});
        menu.model_.Layout(nullptr, scale, 800.0f);
        const int y = menu.kShadowMargin + static_cast<int>(
            menu.sub_model_.RowTopPx(0) + menu.sub_model_.RowHeightPx() * 0.5f);
        for (int i = 0; i < 8; ++i) {
            int x = -1;
            for (int candidate = 0; candidate < menu.sub_model_.WidthPx() + menu.kShadowMargin; ++candidate) {
                if (menu.HitTestSwatch(menu.sub_model_, 0, static_cast<float>(candidate)) == i) {
                    x = candidate;
                    break;
                }
            }
            if (x < 0) return false;
            menu.open_ = true;
            menu.animating_out_ = false;
            menu.sub_parent_row_ = 0;
            menu.OnSubMouse(POINT{x, y}, false);
            if (menu.sub_hover_swatch_ != i) return false;
            menu.OnSubMouse(POINT{x, y}, true);
            if (menu.result_ != 90 + i) return false;
        }
        menu.open_ = true;
        menu.animating_out_ = false;
        menu.sub_parent_row_ = 0;
        menu.OnSubMouse(POINT{menu.kShadowMargin + 10,
            menu.kShadowMargin + static_cast<int>(menu.sub_model_.RowTopPx(1) +
                menu.sub_model_.RowHeightPx() * 0.5f)}, true);
        return menu.result_ == 120;
    }
};
} // namespace pulse::ui

namespace pulse::app {
struct TabControllerTestAccess {
    static void Duplicate(TabController& controller, WindowTabs& tabs, size_t index) {
        controller.DuplicateTab(tabs, index);
    }
};
bool RunPaneHeaderIconTest();
bool RunFolderSizesTest();
bool RunColumnResizeUiTest();

namespace {

bool IsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring WorkspaceRoot() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return L".";

    std::wstring current(path, length);
    const size_t executable_separator = current.find_last_of(L"\\/");
    if (executable_separator == std::wstring::npos) return L".";
    current.resize(executable_separator);

    // Support both Ninja's build/pulse.exe and multi-config build/Release/pulse.exe.
    for (int depth = 0; depth < 4; ++depth) {
        if (IsFile(current + L"\\CMakeLists.txt") &&
            IsFile(current + L"\\src\\app\\selftest_1b2.cpp")) {
            return current;
        }
        const size_t separator = current.find_last_of(L"\\/");
        if (separator == std::wstring::npos) break;
        current.resize(separator);
    }
    return L".";
}

std::wstring WorkspacePath(const wchar_t* relative) {
    return WorkspaceRoot() + L"\\" + relative;
}

const std::wstring kOpsTestRoot = WorkspacePath(L"bench_data\\opstest");
const std::wstring kSandbox = kOpsTestRoot + L"\\selftest_1b2";
const std::wstring kLogPath = WorkspacePath(L"bench_data\\selftest_1b2_last.log");

int g_pass = 0;
int g_fail = 0;
bool g_skip_visual = false;
FILE* g_log = nullptr; // also mirror output here (no console when piped)

void LogLine(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    wprintf(L"%s", buf);
    if (g_log) {
        fwprintf(g_log, L"%s", buf);
        fflush(g_log);
    }
}

void Check(bool cond, const wchar_t* name) {
    if (cond) {
        ++g_pass;
        LogLine(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        LogLine(L"[FAIL] %s\n", name);
    }
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

ops::OpsManager g_ops;

bool WaitOpDone(uint64_t prev_completed, int timeout_ms = 60000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_ops.Status().completed_ops > prev_completed) return true;
        Sleep(10);
    }
    return false;
}

ops::OpStatus RunOp(ops::OpRequest req) {
    uint64_t prev = g_ops.Status().completed_ops;
    g_ops.Submit(std::move(req));
    if (!WaitOpDone(prev)) fprintf(stderr, "[selftest] op TIMEOUT\n");
    return g_ops.Status();
}

void CleanSandbox() {
    // Shallow recursive delete, sandbox only.
    std::wstring pat = std::wstring(kSandbox) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = std::wstring(kSandbox) + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring sub = p + L"\\*";
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileW(sub.c_str(), &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (fd2.cFileName[0] == L'.') continue;
                    DeleteFileW((p + L"\\" + fd2.cFileName).c_str());
                } while (FindNextFileW(h2, &fd2));
                FindClose(h2);
            }
            RemoveDirectoryW(p.c_str());
        } else {
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// --- individual test groups -------------------------------------------------

void TestBreadcrumb() {
    auto segs = ui::SplitBreadcrumb(L"C:\\Users\\TestUser\\Desktop");
    Check(segs.size() == 5, L"breadcrumb: C:\\Users\\TestUser\\Desktop -> 5 segments");
    if (segs.size() == 5) {
        Check(segs[0].text == L"此电脑" && segs[0].path.empty(),
              L"breadcrumb: This PC root segment");
        Check(segs[1].text == L"C:" && segs[1].path == L"C:\\", L"breadcrumb: drive segment");
        Check(segs[2].path == L"C:\\Users" && segs[4].path == L"C:\\Users\\TestUser\\Desktop",
              L"breadcrumb: cumulative paths");
    }
    auto one = ui::SplitBreadcrumb(L"C:\\");
    Check(one.size() == 2 && one[0].path.empty() && one[1].path == L"C:\\",
          L"breadcrumb: drive root sits under This PC");
    auto pc = ui::SplitBreadcrumb(L"");
    Check(pc.size() == 1 && pc[0].text == L"此电脑" && pc[0].path.empty(),
          L"breadcrumb: empty path -> This PC only");
    auto unc = ui::SplitBreadcrumb(L"\\\\server\\share\\dir");
    Check(unc.size() == 3 && unc[0].text == L"server" && unc[0].path == L"\\\\server" &&
          unc[1].text == L"share" && unc[1].path == L"\\\\server\\share" &&
          unc[2].path == L"\\\\server\\share\\dir",
          L"breadcrumb: UNC splits server and share segments");
    auto uncRoot = ui::SplitBreadcrumb(L"\\\\server\\share");
    Check(uncRoot.size() == 2 && uncRoot[0].path == L"\\\\server" &&
          uncRoot[1].path == L"\\\\server\\share",
          L"breadcrumb: UNC share root -> server + share");
    auto uncSrv = ui::SplitBreadcrumb(L"\\\\server");
    Check(uncSrv.size() == 1 && uncSrv[0].text == L"server" &&
          uncSrv[0].path == L"\\\\server",
          L"breadcrumb: bare UNC server -> single segment");
    auto uncLong = ui::SplitBreadcrumb(L"\\\\?\\UNC\\192.0.2.10\\share\\dir");
    Check(uncLong.size() == 3 && uncLong[0].path == L"\\\\192.0.2.10" &&
          uncLong[1].path == L"\\\\192.0.2.10\\share" &&
          uncLong[2].path == L"\\\\192.0.2.10\\share\\dir",
          L"breadcrumb: \\\\?\\UNC prefix restores leading \\\\");
    auto longp = ui::SplitBreadcrumb(L"\\\\?\\C:\\A\\B");
    Check(longp.size() == 4 && longp[3].path == L"C:\\A\\B",
          L"breadcrumb: long-path prefix stripped");
    const std::wstring search_path =
        L"pulse:search:path:C:\\Users\\TestUser\\Desktop\\PulseSearchTest content:\u53d1\u7968";
    auto search = ui::SplitBreadcrumb(search_path);
    Check(search.size() == 1, L"breadcrumb: search query is a single segment");
    if (search.size() == 1) {
        Check(search[0].path == search_path, L"breadcrumb: search click keeps pulse:search:");
        Check(search[0].text.find(L'\\') == std::wstring::npos,
              L"breadcrumb: search label has no backslash");
    }

    Pane pane;
    pane.NewTab(L"\\\\192.0.2.10\\share\\folder");
    SidebarModel sidebar;
    const auto vm = BuildWindowViewModel(
        pane, sidebar, true, false, true, nullptr, 0);
    Check(vm.pane.path == L"\\\\192.0.2.10\\share\\folder",
          L"breadcrumb: pane display path keeps UNC prefix");
    auto fromVm = ui::SplitBreadcrumb(vm.pane.path);
    Check(fromVm.size() == 3 && fromVm[0].path == L"\\\\192.0.2.10" &&
          fromVm[1].path == L"\\\\192.0.2.10\\share" &&
          fromVm[2].path == L"\\\\192.0.2.10\\share\\folder",
          L"breadcrumb: click target stays UNC not CWD-relative");

    // UNC workspace gets the network glyph/color and a 服务器 badge.
    auto colorIs = [](const D2D1_COLOR_F& c, uint32_t rgb) {
        const auto want = ui::HexColor(rgb);
        return c.r == want.r && c.g == want.g && c.b == want.b;
    };
    PlacesCatalog cat;
    cat.persist = false;
    cat.PinWorkspace(L"C:\\local", L"local", 0, { L"C:\\local" });
    cat.PinWorkspace(L"\\\\server\\share", L"nas", 0, { L"\\\\server\\share" });
    cat.workspaces[1].frequent.push_back({ L"\\\\server\\share\\sub", 3 });
    const auto wvm = BuildWindowViewModel(pane, sidebar, true, false, true, &cat, 0);
    const auto workspaces = std::find_if(wvm.sidebar.begin(), wvm.sidebar.end(),
        [](const auto& section) {
            return section.id == static_cast<int>(SidebarSectionId::Workspaces);
        });
    Check(workspaces != wvm.sidebar.end() && workspaces->items.size() == 3,
          L"sidebar: workspace group holds both workspaces + frequent child");
    if (workspaces != wvm.sidebar.end() && workspaces->items.size() == 3) {
        const auto& local = workspaces->items[0];
        const auto& nas = workspaces->items[1];
        const auto& sub = workspaces->items[2];
        Check(local.badge.empty() && local.icon_glyph == L"\xE8B7",
              L"sidebar: local workspace keeps default glyph, no badge");
        Check(nas.badge == L"当前 · 服务器" && nas.icon_glyph == L"\xE968" &&
              colorIs(nas.icon_color, 0x38BDF8),
              L"sidebar: active UNC workspace shows server badge + network glyph");
        Check(sub.badge.empty() && sub.icon_glyph == L"\xE968" &&
              colorIs(sub.icon_color, 0x38BDF8),
              L"sidebar: UNC frequent child gets network glyph, no badge");
    }

    const auto access = BuildSidebarModel();
    const ui::SidebarItem* desktop = nullptr;
    Pane home;
    home.NewTab(L"C:\\");
    const auto avm = BuildWindowViewModel(home, access, true, false, false, nullptr, 0);
    for (const auto& group : avm.sidebar) {
        if (group.header != L"\u5FEB\u901F\u8BBF\u95EE") continue; // 快速访问
        for (const auto& item : group.items) {
            if (item.label == L"\u684C\u9762") { // 桌面
                desktop = &item;
                break;
            }
        }
    }
    Check(desktop && desktop->badge == L"\u684C\u9762" &&
          desktop->badge_color.a > 0.0f && !desktop->path.empty(),
          L"sidebar: quick access Desktop exposes editable badge color");

    // OneDrive only has a known folder while the client is signed in, so this
    // asserts the wiring on machines that actually have it.
    PWSTR onedrive = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_OneDrive, 0, nullptr, &onedrive)) && onedrive) {
        const std::wstring expected = fs::NormalizePath(onedrive);
        CoTaskMemFree(onedrive);
        const bool listed = std::any_of(access.cloud.begin(), access.cloud.end(),
            [&](const auto& entry) { return entry.path == expected; });
        Check(listed, L"sidebar: OneDrive gets its own section when the client is signed in");
        const bool still_pinned = std::any_of(access.quick_access.begin(),
            access.quick_access.end(),
            [&](const auto& entry) { return entry.path == expected; });
        Check(!still_pinned, L"sidebar: OneDrive leaves quick access for its own section");
    }

    // Hiding is a separate mask from collapsing: a hidden group is not laid out.
    const auto hidden_vm = BuildWindowViewModel(home, access, true, false, false, nullptr, 0,
        1u << static_cast<int>(SidebarSectionId::QuickAccess));
    const auto hidden_qa = std::find_if(hidden_vm.sidebar.begin(), hidden_vm.sidebar.end(),
        [](const auto& section) {
            return section.id == static_cast<int>(SidebarSectionId::QuickAccess);
        });
    const auto visible_starred = std::find_if(hidden_vm.sidebar.begin(), hidden_vm.sidebar.end(),
        [](const auto& section) {
            return section.id == static_cast<int>(SidebarSectionId::Starred);
        });
    Check(hidden_qa != hidden_vm.sidebar.end() && hidden_qa->hidden &&
          visible_starred != hidden_vm.sidebar.end() && !visible_starred->hidden,
          L"sidebar: the hidden mask targets only its own group");
}

void TestThisPcEnumeration() {
    std::vector<fs::DirEntry> entries;
    fs::EnumerateDirectory(L"", entries);
    Check(!entries.empty(), L"thispc: empty path enumerates at least one drive");
    bool all_dirs = true;
    bool has_c = false;
    for (const auto& e : entries) {
        if (!e.is_dir) all_dirs = false;
        if (e.name.find(L"C:") != std::wstring::npos ||
            e.full_path.starts_with(L"\\\\?\\C:\\"))
            has_c = true;
    }
    Check(all_dirs, L"thispc: all entries are directories");
    Check(has_c, L"thispc: contains the C: drive");
}

void TestLoadingPresentation() {
    Pane pane;
    pane.NewTab(L"C:\\pending-folder");
    SidebarModel sidebar;
    const auto vm = BuildWindowViewModel(
        pane, sidebar, true, false, true, nullptr, 0);
    Check(vm.pane.header_text == L"pending-folder",
          L"loading: directory title remains visible");
    Check(vm.status.status_text.empty(),
          L"loading: normal directory does not show search-like status");
}

LRESULT CALLBACK NavigationTestProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_APPCOMMAND && state && HandleBrowserNavigation(*state, lparam)) return TRUE;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TestMouseHistoryNavigation() {
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    Pane pane;
    pane.NewTab(L"pulse:settings/general");
    state->pane = &pane;
    auto* tab = pane.ActiveTab();
    tab->back_stack.push(L"pulse:settings/about");
    tab->back_stack.push(L"pulse:settings/appearance");
    WNDCLASSW wc{};
    wc.lpfnWndProc = NavigationTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseMouseHistorySelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"mouse history: hidden test window created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    Check(tab->back_stack.size() == 2, L"mouse history: button down does not navigate");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/appearance" && tab->back_stack.size() == 1 &&
          tab->forward_stack.size() == 1, L"mouse history: side button back navigates exactly once");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    Check(tab->current_path == L"pulse:settings/general" && tab->forward_stack.empty(),
          L"mouse history: side button forward restores location");
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    Check(tab->current_path == L"pulse:settings/general" && tab->back_stack.size() == 2,
          L"mouse history: empty forward history is a no-op");
    HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 1, 1,
                                hwnd, nullptr, wc.hInstance, nullptr);
    Check(edit != nullptr, L"mouse history: child edit created");
    if (edit) SendMessageW(edit, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/appearance",
          L"mouse history: side button over child edit reaches navigation");
    SendMessageW(hwnd, WM_APPCOMMAND, 0, MAKELPARAM(0, APPCOMMAND_BROWSER_FORWARD));
    Check(tab->current_path == L"pulse:settings/general",
          L"mouse history: driver browser command navigates forward");
    Check(!HandleBrowserNavigation(*state, MAKELPARAM(0, APPCOMMAND_VOLUME_UP)),
          L"mouse history: unrelated commands remain unhandled");
    tab->back_stack = {};
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    Check(tab->current_path == L"pulse:settings/general",
          L"mouse history: empty back history is a no-op");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    state->pane = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

LRESULT CALLBACK BlankPaneTestProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        switch (msg) {
        case WM_LBUTTONDOWN: return HandleLButtonDown(state, hwnd, msg, wparam, lparam);
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lparam) == state->hwndFilterEdit && HIWORD(wparam) == EN_CHANGE)
                SyncFilterEditor(*state);
            break;
        case WM_MOUSEMOVE: return HandleMouseMove(state, hwnd, msg, wparam, lparam);
        case WM_LBUTTONUP: return HandleLButtonUp(state, hwnd, msg, wparam, lparam);
        case WM_LBUTTONDBLCLK: return HandleLButtonDblClk(state, hwnd, msg, wparam, lparam);
        case WM_CAPTURECHANGED: return HandleCaptureChanged(state, hwnd, msg, wparam, lparam);
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TestFilterControls() {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ui::MainRenderer renderer;
        renderer.SetScale(scale);
        const auto bounds = D2D1::RectF(0, 0, 320 * scale, 500 * scale);
        const auto edit = renderer.FilterEditRect(bounds, 1, true);
        const auto clear = renderer.FilterClearRect(bounds, 1);
        Check(edit.right < clear.left && edit.right - edit.left >= 40 * scale,
            L"filter: narrow input retains text and clear space across DPI");
    }
    Pane animation;
    TickFilterAnimation(animation, true, 1000);
    TickFilterAnimation(animation, true, 1070);
    Check(animation.filter_expand > 0.8f && animation.filter_expand < 1.0f, L"filter: fast smooth expansion");
    TickFilterAnimation(animation, true, 1140);
    Check(animation.filter_expand == 1, L"filter: expansion finishes in 140ms");
    animation.view.filter_text = L"1";
    TickFilterAnimation(animation, false, 1300);
    Check(animation.filter_expand == 1, L"filter: content stays expanded without focus");
    animation.view.filter_text.clear();
    TickFilterAnimation(animation, false, 1400);
    TickFilterAnimation(animation, false, 1540);
    Check(animation.filter_expand == 0, L"filter: clear collapse finishes in 140ms without a tail");

    auto state = std::make_unique<AppState>();
    state->places.persist = false; state->appPrefs.persist = false; state->isolatedTest = true;
    WNDCLASSW wc{}; wc.lpfnWndProc = BlankPaneTestProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseFilterControlsSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"filter: isolated owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    if (state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(L"C:\\FilterFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(2);
        (*entries)[0].name = L"PulseSetup-1.0.11.exe";
        (*entries)[1].name = L"Documents"; (*entries)[1].is_dir = true;
        tab->SetSnapshot(entries);
        ShowFilterEditor(*state);
        SetWindowTextW(state->hwndFilterEdit, L"11");
        SyncFilterEditor(*state);
        Check(BuildVm(*state).pane.EntryCount() == 1, L"filter: typing narrows directory");
        SendMessageW(state->hwndFilterEdit, EM_SETSEL, 0, -1);
        SendMessageW(state->hwndFilterEdit, WM_CLEAR, 0, 0);
        Check(tab->filter_text.empty() && BuildVm(*state).pane.EntryCount() == 2,
            L"filter: native text deletion updates the model without WM_CHAR");
        SetWindowTextW(state->hwndFilterEdit, L"11");
        Check(tab->filter_text == L"11", L"filter: edit change notification synchronizes contents");
        const auto list = ListRect(*state);
        auto point = MAKELPARAM(static_cast<int>(list.left + 30), static_cast<int>(list.bottom - 30));
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        Check(!state->filterEditing && !state->filterFocusPending && GetFocus() != state->hwndFilterEdit,
            L"filter: outside click ends editing and pending focus");
        SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
        Check(tab->filter_text == L"11", L"filter: outside click preserves filter");
        state->pane->filter_expand = 1;
        auto vm = BuildVm(*state);
        const auto bounds = D2D1::RectF(0,0,static_cast<float>(state->compositor.Width()),
            static_cast<float>(state->compositor.Height()));
        const auto clear = state->renderer.FilterClearRect(bounds, 1);
        const auto edit = state->renderer.FilterEditRect(bounds, 1, true);
        Check(edit.right < clear.left, L"filter: edit and clear hit areas do not overlap");
        Check(edit.right - edit.left >= 120, (L"filter: an open filter leaves room to type (" +
              std::to_wstring(static_cast<int>(edit.right - edit.left)) + L" px)").c_str());
        point = MAKELPARAM(static_cast<int>((clear.left + clear.right) / 2), static_cast<int>((clear.top + clear.bottom) / 2));
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
        Check(tab->filter_text.empty() && BuildVm(*state).pane.EntryCount() == 2,
            L"filter: clear button restores files and folders");
        vm = BuildVm(*state);
        auto hit = state->renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700),
            (clear.left + clear.right) / 2, (clear.top + clear.bottom) / 2);
        Check(hit.region != ui::HitTestResult::FilterClear, L"filter: empty content has no clear hit target");
        auto hash_entries = std::make_shared<std::vector<fs::DirEntry>>(2);
        (*hash_entries)[0].name = L"1#2#小学教学楼结构";
        (*hash_entries)[0].is_dir = true;
        (*hash_entries)[1].name = L"2#楼变更结构审图回复0810";
        (*hash_entries)[1].is_dir = true;
        tab->SetSnapshot(hash_entries);
        ShowFilterEditor(*state);
        tab->scroll_y = 5000;
        tab->scroll_x = 500;
        state->scrollTargetY = 5000;
        SetWindowTextW(state->hwndFilterEdit, L"1#");
        vm = BuildVm(*state);
        Check(vm.pane.EntryCount() == 1 && vm.pane.SourceIndex(0) == 0,
            L"filter: 1# matches the literal desktop folder name");
        const auto match_rect = state->renderer.ItemRectInPane(vm.pane, FocusedPaneRect(*state), 0);
        Check(match_rect.top >= list.top && match_rect.bottom <= list.bottom &&
            tab->scroll_x == 0 && state->scrollTargetY == 0,
            L"filter: matches return to the visible viewport after scrolling");
        tab->scroll_y = 24;
        SyncFilterEditor(*state);
        Check(tab->scroll_y == 24, L"filter: unchanged edit notification preserves current scroll");
        HideFilterEditor(*state, true);
        if (state->hwndFilterEdit) DestroyWindow(state->hwndFilterEdit);
        state->hwndFilterEdit = nullptr;
    } else Check(false, L"filter: graphics initialized");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd); state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void TestRenameOutsideClick() {
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = BlankPaneTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRenameOutsideSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"rename outside: hidden owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    if (state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(L"C:\\RenameFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        fs::DirEntry entry; entry.name = L"New text document.txt";
        const auto original_path = tab->current_path;
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
        state->hwndRenameEdit = CreateWindowExW(0, L"EDIT", entry.name.c_str(), WS_POPUP,
            0, 0, 100, 24, hwnd, nullptr, wc.hInstance, nullptr);
        state->renameIndex = 0;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        const auto list = ListRect(*state);
        const auto point = MAKELPARAM(static_cast<int>(list.left + 30), static_cast<int>(list.bottom - 30));
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        Check(state->renameIndex == -1, L"rename outside: blank click ends edit without kill-focus message");
        SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
        Check(tab->current_path == original_path, L"rename outside: commit preserves directory");
        DestroyWindow(state->hwndRenameEdit);
        state->hwndRenameEdit = nullptr;
    } else Check(false, L"rename outside: graphics initialized");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void TestBlankPaneClickNavigation() {
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float height : {380.0f, 720.0f}) {
            for (bool occupied : {false, true}) {
                ui::MainRenderer renderer;
                renderer.SetScale(scale);
                ui::WindowViewModel vm;
                for (int g = 0; g < 3; ++g) {
                    ui::SidebarGroup group;
                    group.header = L"Scroll fixture";
                    for (int i = 0; i < 12; ++i) {
                        ui::SidebarItem item;
                        item.label = L"Item";
                        item.path = L"fixture:" + std::to_wstring(g) + L":" + std::to_wstring(i);
                        item.is_drive = g == 0;
                        item.is_tag = g == 1;
                        group.items.push_back(std::move(item));
                    }
                    vm.sidebar.push_back(std::move(group));
                }
                if (occupied) vm.tray_deck.cards.emplace_back();
                const float width_px = 1000.0f * scale, height_px = height * scale;
                vm.sidebar_scroll = renderer.SidebarMaxScroll(vm, width_px, height_px);
                D2D1_RECT_F track{}, thumb{};
                float maximum = 0.0f;
                const bool geometry = renderer.SidebarScrollbarGeometry(
                    vm, width_px, height_px, track, thumb, maximum);
                Check(geometry && std::abs(thumb.bottom - track.bottom) < 0.01f,
                      L"sidebar geometry: thumb reaches track bottom across DPI and tray states");
                const auto bounds = D2D1::RectF(0, 0, width_px, height_px);
                const auto thumb_hit = renderer.HitTest(vm, bounds,
                    (thumb.left + thumb.right) * 0.5f, (thumb.top + thumb.bottom) * 0.5f);
                Check(thumb_hit.region == ui::HitTestResult::Scrollbar && thumb_hit.sub_index == 2,
                      L"sidebar geometry: painted thumb has matching hit target");
                float first = -1.0f, last = -1.0f;
                for (float y = std::max(track.top, track.bottom - 90.0f * scale);
                     y < track.bottom; y += scale) {
                    const auto hit = renderer.HitTest(vm, bounds, 70.0f * scale, y);
                    if (hit.region == ui::HitTestResult::SidebarItem && hit.path == L"fixture:2:11") {
                        if (first < 0.0f) first = y;
                        last = y;
                    }
                }
                Check(first >= track.top && last - first >= 30.0f * scale,
                      L"sidebar geometry: final network row is fully visible and clickable at end");
                vm.sidebar[1].collapsed = true;
                Check(renderer.SidebarMaxScroll(vm, width_px, height_px) < maximum,
                      L"sidebar geometry: collapsing a group reduces scroll range");
                // The section menu hides a whole group instead of folding it.
                vm.sidebar[1].collapsed = false;
                vm.sidebar[1].hidden = true;
                Check(renderer.SidebarMaxScroll(vm, width_px, height_px) < maximum,
                      L"sidebar geometry: hiding a group removes it from the layout");
                vm.sidebar[1].hidden = false;
                // An empty pane must report its background so the menu can open;
                // with no section under the cursor the index stays -1 (pane menu).
                ui::WindowViewModel empty;
                const auto blank_hit = renderer.HitTest(empty, bounds, 70.0f * scale, 200.0f * scale);
                Check(blank_hit.region == ui::HitTestResult::SidebarBlank && blank_hit.index < 0,
                      L"sidebar geometry: empty pane background owns the section menu");
                // Empty space inside a section reports that section, so the
                // right-click menu matches what the cursor is over.
                ui::WindowViewModel banded;
                for (int g = 0; g < 2; ++g) {
                    ui::SidebarGroup group;
                    group.id = g;
                    group.header = L"Band fixture";
                    ui::SidebarItem item;
                    item.label = L"Row";
                    item.path = L"band:" + std::to_wstring(g);
                    group.items.push_back(std::move(item));
                    banded.sidebar.push_back(std::move(group));
                }
                std::vector<ui::SidebarGroupBand> bands;
                renderer.SidebarGroupBands(banded, width_px, height_px, bands);
                bool band_hit = false;
                for (const auto& band : bands) {
                    for (float y = band.top; y < band.bottom; y += scale) {
                        const auto hit = renderer.HitTest(banded, bounds, 70.0f * scale, y);
                        if (hit.region == ui::HitTestResult::SidebarBlank && hit.index == band.group) {
                            band_hit = true;
                            break;
                        }
                    }
                }
                Check(bands.size() == 2 && band_hit,
                      L"sidebar geometry: blank space resolves to the section under the cursor");
                // A row hit reports which section and which row it belongs to, so a
                // drag can tell a header-less section's row from a pinned row.
                bool row_identity = false;
                for (float y = bands.front().top; y < bands.front().bottom; y += scale) {
                    const auto hit = renderer.HitTest(banded, bounds, 70.0f * scale, y);
                    if (hit.region != ui::HitTestResult::SidebarItem) continue;
                    row_identity = hit.sidebar_section == 0 && hit.sidebar_item == 0;
                    break;
                }
                Check(row_identity,
                      L"sidebar geometry: a row hit carries its section id and row index");
                // Narrow rail: a folded section keeps one icon row, and that row
                // still reports the section plus its name (rail tooltips).
                ui::WindowViewModel folded_vm;
                for (int g = 0; g < 2; ++g) {
                    ui::SidebarGroup group;
                    group.id = g;
                    group.header = g == 0 ? L"\u6298\u53E0A" : L"\u6298\u53E0B";
                    group.icon_glyph = L"\xE8A9";
                    group.collapsed = true;
                    ui::SidebarItem item;
                    item.label = L"Row";
                    item.path = L"fold:" + std::to_wstring(g);
                    group.items.push_back(std::move(item));
                    folded_vm.sidebar.push_back(std::move(group));
                }
                const float narrow_w = 400.0f * scale;
                const D2D1_RECT_F narrow_bounds = D2D1::RectF(0, 0, narrow_w, height_px);
                bool rail_hit = false;
                for (float y = 0.0f; y < height_px; y += scale) {
                    const auto hit = renderer.HitTest(folded_vm, narrow_bounds,
                                                      24.0f * scale, y);
                    if (hit.region != ui::HitTestResult::SidebarHeader) continue;
                    rail_hit = hit.sidebar_section == 0 && hit.label == L"\u6298\u53E0A";
                    break;
                }
                Check(rail_hit,
                      L"sidebar geometry: a folded section keeps a labelled rail row");
                // The rail row stays put when the section is open, so the user
                // can fold it again from the same icon.
                ui::WindowViewModel open_vm = folded_vm;
                open_vm.sidebar[0].collapsed = false;
                bool open_rail_hit = false;
                for (float y = 0.0f; y < height_px; y += scale) {
                    const auto hit = renderer.HitTest(open_vm, narrow_bounds, 24.0f * scale, y);
                    if (hit.region != ui::HitTestResult::SidebarHeader) continue;
                    open_rail_hit = hit.sidebar_section == 0 && hit.label == L"\u6298\u53E0A";
                    break;
                }
                Check(open_rail_hit,
                      L"sidebar geometry: an expanded section keeps its rail row");
            }
        }
    }
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = BlankPaneTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseBlankPaneClickSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"blank pane: hidden event test window created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    const bool graphics = state->compositor.Init(hwnd);
    Check(graphics, L"blank pane: real hit-test graphics initialized");
    if (graphics) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(1.0f);
        state->window_tabs.NewTab(L"C:\\PulseBlankClickTest\\Child");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        {
            const auto saved_sidebar = state->sidebar;
            for (int i = 0; i < 40; ++i) {
                SidebarEntry entry;
                entry.label = L"Scroll fixture " + std::to_wstring(i);
                entry.path = L"C:\\PulseSidebarFixture\\" + std::to_wstring(i);
                state->sidebar.quick_access.push_back(std::move(entry));
            }
            state->trayCards[L"sidebar-test-ghost"].ghost = true;
            state->sidebarScroll = 100000.0f;
            auto vm = BuildVm(*state, false);
            const float maximum = state->renderer.SidebarMaxScroll(vm, 1000, 700);
            Check(maximum > 0.0f && std::abs(state->sidebarScroll - maximum) < 0.01f,
                  L"sidebar: clamp uses final occupied tray height");
            vm = BuildVm(*state, false);
            Check(std::abs(vm.sidebar_scroll - maximum) < 0.01f,
                  L"sidebar: rebuilding view model preserves bottom scroll position");
            state->sidebarScroll = 0.0f;
            vm = BuildVm(*state, false);
            D2D1_RECT_F track{}, thumb{};
            float max_scroll = 0.0f;
            Check(state->renderer.SidebarScrollbarGeometry(vm, 1000, 700, track, thumb, max_scroll),
                  L"sidebar: overflowing view exposes scrollbar geometry");
            const int sx = static_cast<int>((track.left + track.right) * 0.5f);
            const int sy = static_cast<int>(track.bottom - 1.0f);
            SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(sx, sy));
            Check(state->scrollbarDragging && state->scrollbarSidebar &&
                  std::abs(state->sidebarScroll - max_scroll) < 0.01f,
                  L"sidebar: clicking track bottom reaches end and captures sidebar drag");
            BYTE saved_keys[256]{};
            GetKeyboardState(saved_keys);
            BYTE drag_keys[256]{};
            memcpy(drag_keys, saved_keys, sizeof(drag_keys));
            drag_keys[VK_LBUTTON] |= 0x80;
            SetKeyboardState(drag_keys);
            SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                MAKELPARAM(sx, static_cast<int>(track.top - 20.0f)));
            Check(state->sidebarScroll == 0.0f,
                  L"sidebar: dragging thumb to top reaches zero without scrolling file pane");
            SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                MAKELPARAM(sx, static_cast<int>(track.bottom + 50.0f)));
            Check(std::abs(state->sidebarScroll - max_scroll) < 0.01f,
                  L"sidebar: dragging beyond track bottom clamps to full range");
            SetKeyboardState(saved_keys);
            SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(sx, sy));
            Check(!state->scrollbarDragging && !state->scrollbarSidebar && GetCapture() != hwnd,
                  L"sidebar: releasing thumb clears drag and capture");
            state->sidebar = saved_sidebar;
            state->trayCards.clear();
            state->sidebarScroll = 0.0f;
        }
        {
            auto vm = BuildVm(*state, false);
            bool tag_action = false, network_action = false;
            for (auto& group : vm.sidebar) {
                group.collapsed = true;
                if (group.header == l10n::Get(l10n::StringId::SidebarTags))
                    tag_action = group.add_action == ui::SidebarAddAction::CreateTag;
                if (group.header == l10n::Get(l10n::StringId::SidebarNetworkLocations))
                    network_action = group.add_action == ui::SidebarAddAction::AddNetwork;
            }
            Check(tag_action && network_action, L"sidebar: tag and network groups bind their own add actions");
            for (int order = 0; order < 2; ++order) {
                if (order) std::reverse(vm.sidebar.begin(), vm.sidebar.end());
                const auto sidebar = state->renderer.SidebarRect(1000, 700);
                bool tags_hit = false, network_hit = false, correct = true;
                for (float y = sidebar.top; y < sidebar.bottom; y += 2.0f) {
                    const auto hit = state->renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700),
                        sidebar.right - 42.0f, y);
                    if (hit.region != ui::HitTestResult::SidebarHeaderAction) continue;
                    correct = correct && hit.index >= 0 && static_cast<size_t>(hit.index) < vm.sidebar.size() &&
                        hit.sidebar_action == vm.sidebar[hit.index].add_action;
                    tags_hit |= hit.sidebar_action == ui::SidebarAddAction::CreateTag;
                    network_hit |= hit.sidebar_action == ui::SidebarAddAction::AddNetwork;
                }
                Check(correct && tags_hit && network_hit,
                    L"sidebar: plus hit targets retain actions when group order changes");
            }
        }
        {
            // #80: the This PC title is a link, its chevron folds, and every other
            // header stays a plain fold toggle.
            auto vm = BuildVm(*state, false);
            const int drives_id = static_cast<int>(app::SidebarSectionId::Drives);
            bool drives_link = false, others_plain = true;
            for (auto& group : vm.sidebar) {
                group.collapsed = true;
                if (group.id == drives_id) {
                    drives_link = group.navigable;
                    // The self-test state lists no volumes; an empty section is
                    // not laid out, so give it one row to keep the header.
                    group.hidden = false;
                    if (group.items.empty()) {
                        ui::SidebarItem drive;
                        drive.label = L"C:";
                        drive.path = L"C:\\";
                        group.items.push_back(std::move(drive));
                    }
                } else {
                    others_plain = others_plain && !group.navigable;
                }
            }
            Check(drives_link && others_plain, L"sidebar: only the This PC header is a link");
            const auto sidebar = state->renderer.SidebarRect(1000, 700);
            bool title_link = false, chevron_folds = false, others_fold = true;
            for (float y = sidebar.top; y < sidebar.bottom; y += 2.0f) {
                const auto title = state->renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700),
                    sidebar.left + 30.0f, y);
                if (title.region != ui::HitTestResult::SidebarHeader) continue;
                const bool drives = title.sidebar_section == drives_id;
                if (drives) title_link |= title.sub_index == 1;
                else others_fold = others_fold && title.sub_index != 1;
                const auto chevron = state->renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700),
                    sidebar.right - 20.0f, y);
                if (drives && chevron.region == ui::HitTestResult::SidebarHeader)
                    chevron_folds |= chevron.sub_index != 1;
            }
            Check(title_link && chevron_folds && others_fold,
                L"sidebar: This PC title navigates, its chevron and other headers fold");
        }
        const auto list = ListRect(*state);
        const int x = static_cast<int>(list.left + 30);
        const int y = static_cast<int>(list.bottom - 30);
        const LPARAM point = MAKELPARAM(x, y);
        auto press = [&] { SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point); };
        auto release = [&] { SendMessageW(hwnd, WM_LBUTTONUP, 0, point); };
        const std::wstring initial_folder = tab->current_path;
        Check(state->appPrefs.blank_click_action == kBlankClickOff, L"blank pane: back on empty click defaults off");
        press();
        Check(state->marqueePending && !state->blankClickTab,
              L"blank pane: disabled back still permits marquee selection");
        release();
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == initial_folder,
              L"blank pane: disabled single and double click preserve location");
        state->appPrefs.blank_click_action = kBlankClickBack;
        tab->selected.insert(0);
        tab->back_stack = {};
        tab->back_stack.push(L"C:\\PulseBlankClickSelection");
        press();
        Check(tab->SelectedCount() == 0 && state->blankClickTab == tab,
              L"blank pane: first click clears the selection and still arms back");
        release();
        Check(tab->current_path == initial_folder,
              L"blank pane: clearing a selection with one click stays put");
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == fs::NormalizePath(L"C:\\PulseBlankClickSelection"),
              L"blank pane: double click that clears a selection goes back");
        tab->current_path = initial_folder;
        tab->back_stack = {};
        press();
        state->appPrefs.blank_click_action = kBlankClickOff;
        release();
        Check(tab->current_path == initial_folder,
              L"blank pane: disabling during a press prevents pending navigation");
        state->appPrefs.blank_click_action = kBlankClickBack;
        press();
        Check(state->marqueePending && state->blankClickTab == tab && GetCapture() == hwnd,
              L"blank pane: real blank hit arms click and captures mouse");
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 1, y + 1));
        Check(state->marqueePending && state->blankClickTab == tab,
              L"blank pane: held-button move preserves pending click");
        release();
        Check(tab->current_path == initial_folder,
              L"blank pane: enabled single click does not navigate");
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == fs::NormalizePath(L"C:\\PulseBlankClickTest") && GetCapture() != hwnd,
              L"blank pane: restored folder without history navigates to parent on release");
        tab->back_stack = {};
        tab->back_stack.push(L"C:\\PulseBlankClickHistory");
        press();
        release();
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == fs::NormalizePath(L"C:\\PulseBlankClickHistory"),
              L"blank pane: available history takes precedence over parent");
        {
            // Up (B站 #11): the parent folder even when there is history.
            const std::wstring up_from = tab->current_path;
            const auto saved_back = tab->back_stack;
            tab->back_stack.push(L"C:\\PulseBlankClickUpHistory");
            state->appPrefs.blank_click_action = kBlankClickUp;
            press();
            Check(state->blankClickTab == tab, L"blank pane: up mode arms the blank click");
            release();
            SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
            release();
            Check(tab->current_path == fs::NormalizePath(fs::ParentPath(fs::NormalizePath(up_from))),
                  L"blank pane: up mode goes to the parent folder even with back history");
            state->appPrefs.blank_click_action = kBlankClickBack;
            tab->current_path = up_from;
            tab->back_stack = saved_back;
        }
        const std::wstring folder = tab->current_path;
        tab->current_path = L"pulse:search:showbox";
        tab->back_stack.push(folder);
        const auto history_size = tab->back_stack.size();
        press();
        Check(state->marqueePending && !state->blankClickTab,
              L"blank pane: search results allow selection without arming navigation");
        release();
        Check(tab->current_path == L"pulse:search:showbox" && tab->back_stack.size() == history_size,
              L"blank pane: search results preserve query and back history on blank click");
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == L"pulse:search:showbox",
              L"blank pane: double click in search results does not navigate");
        tab->current_path = folder;
        tab->back_stack.pop();
        const std::wstring before = tab->current_path;
        press();
        release();
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                     MAKELPARAM(x + GetSystemMetrics(SM_CXDRAG) + 1, y));
        release();
        Check(tab->current_path == before,
              L"blank pane: dragging on second click prevents navigation");
        press();
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                     MAKELPARAM(x + GetSystemMetrics(SM_CXDRAG) + 1, y));
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, point);
        release();
        Check(tab->current_path == before,
              L"blank pane: real marquee returning to origin does not navigate");
        press();
        ReleaseCapture();
        release();
        Check(tab->current_path == before,
              L"blank pane: capture cancellation prevents navigation");
        press();
        ++tab->view_generation;
        release();
        Check(tab->current_path == before,
              L"blank pane: navigation during press invalidates release");
        tab->back_stack.push(L"C:\\PulseBlankClickDouble");
        press();
        release();
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path == fs::NormalizePath(L"C:\\PulseBlankClickDouble"),
              L"blank pane: double click release does not navigate twice");
        tab->back_stack = {};
        tab->current_path = L"C:\\";
        press();
        release();
        Check(tab->current_path == L"C:\\", L"blank pane: drive root with no history stays put");
        // A double click there goes on to This PC, like the Up button (#68).
        press();
        release();
        SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        release();
        Check(tab->current_path.empty(), L"blank pane: double click at a drive root goes up to This PC");
        tab->back_stack = {};
        tab->forward_stack = {};
        tab->current_path = L"C:\\";
        auto& layout = *state->window_tabs.Active();
        auto other = std::make_unique<Pane>();
        other->NewTab(L"C:\\PulseBlankClickSplit\\Child");
        Pane* right = other.get();
        layout.panes.push_back(std::move(other));
        layout.layout = LayoutPreset::TwoVertical;
        RebuildLayoutRoot(layout);
        const auto split_vm = BuildVm(*state, false);
        Check(split_vm.pane_slots.size() == 2, L"blank pane: real split layout exposes both panes");
        if (split_vm.pane_slots.size() == 2) {
            const auto bounds = split_vm.pane_slots[1].rect;
            const LPARAM right_point = MAKELPARAM(static_cast<int>(bounds.left + 30),
                                                  static_cast<int>(bounds.bottom - 30));
            SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, right_point);
            Check(state->pane == right && state->blankClickTab == right->ActiveTab(),
                  L"blank pane: inactive split pane receives click context");
            SendMessageW(hwnd, WM_LBUTTONUP, 0, right_point);
            SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, right_point);
            SendMessageW(hwnd, WM_LBUTTONUP, 0, right_point);
            Check(right->ActiveTab()->current_path == fs::NormalizePath(L"C:\\PulseBlankClickSplit") &&
                  tab->current_path == L"C:\\",
                  L"blank pane: split click navigates only the clicked pane");
            // Growing the layout from a This PC pane clones This PC, not C:\ (#68).
            right->ActiveTab()->current_path.clear();
            ApplyLayoutPreset(*state, LayoutPreset::Three);
            Check(layout.panes.size() == 3 && layout.panes.back()->ActiveTab() &&
                  layout.panes.back()->ActiveTab()->current_path.empty(),
                  L"split: a new pane cloned from This PC opens This PC");
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        state->renderer.SetCompositor(nullptr);
        state->compositor.Shutdown();
    }
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void TestNotificationToast() {
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, L"toast: hidden window created");
    if (!window) return;
    ui::Compositor compositor;
    if (compositor.Init(window)) {
        compositor.RecreateTextFormats(1.0f);
        ui::NotificationToast toast;
        toast.Show(window, L"Index", L"The original index is preserved.");
        compositor.Dc()->BeginDraw();
        toast.Draw(compositor, ui::MakeTheme(false, D2D1::ColorF(0x0078D4)), 1.0f, false);
        Check(SUCCEEDED(compositor.Dc()->EndDraw()), L"toast: Fluent card renders");
        const auto bounds = toast.Bounds();
        Check(bounds.left >= 0 && bounds.right <= 640 && bounds.top >= 0 && bounds.bottom <= 480,
              L"toast: card stays inside window");
        const auto body = MAKELPARAM(static_cast<int>(bounds.left + 60), static_cast<int>(bounds.top + 20));
        Check(toast.HandleMessage(window, WM_LBUTTONDOWN, 0, body), L"toast: body blocks underlying click");
        Check(toast.HandleMessage(window, WM_LBUTTONUP, 0, MAKELPARAM(0, 0)) && GetCapture() != window && toast.IsVisible(),
              L"toast: releasing outside clears capture without dismissing");
        // With system animations on, the first frame of the entrance draws no
        // content and places no close button yet: draw the settled frame first.
        Sleep(ui::motion::SpringSettleMs(ui::motion::spring::Pop) + 50);
        compositor.Dc()->BeginDraw();
        toast.Draw(compositor, ui::MakeTheme(false, D2D1::ColorF(0x0078D4)), 1.0f, false);
        compositor.Dc()->EndDraw();
        const auto close = MAKELPARAM(static_cast<int>(bounds.right - 16), static_cast<int>((bounds.top + bounds.bottom) / 2));
        toast.HandleMessage(window, WM_LBUTTONDOWN, 0, close);
        toast.HandleMessage(window, WM_LBUTTONUP, 0, close);
        Check(!toast.IsVisible(), L"toast: close dismisses persistent message");
        compositor.Shutdown();
    } else {
        Check(false, L"toast: graphics initialized");
    }
    DestroyWindow(window);
}

void TestNavigationReturnSelection() {
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse", L"C:\\projects") == L"pulse",
          L"navigation: parent return selects the folder just left");
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse\\src\\app", L"C:\\projects") == L"pulse",
          L"navigation: deep ancestor return selects its immediate child");
    Check(NavigationReturnChildName(
              L"C:\\projects\\pulse", L"D:\\archive").empty(),
          L"navigation: unrelated destination has no return selection");
    Check(NavigationReturnChildName(
              L"\\\\192.0.2.10\\示例共享盘-2025\\00_软件",
              L"\\\\192.0.2.10\\示例共享盘-2025") == L"00_软件",
          L"navigation: UNC parent return selects the folder just left");
    Check(NavigationReturnChildName(
              L"\\\\192.0.2.10\\示例共享盘-2025\\00_软件\\工具",
              L"\\\\192.0.2.10\\示例共享盘-2025") == L"00_软件",
          L"navigation: deep UNC return selects the immediate child");
}

void TestAddressSearch() {
    bool layouts_ok = true;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f, 2.5f}) {
        for (const float width : {104.0f, 180.0f, 259.0f, 260.0f, 419.0f, 420.0f, 800.0f}) {
            const auto field = D2D1::RectF(10.0f * scale, 10.0f * scale,
                                          (10.0f + width) * scale, 46.0f * scale);
            const auto layout = ui::LayoutAddressSearch(field, scale);
            layouts_ok &= layout.scope.right <= layout.input.left &&
                layout.input.right <= layout.clear.left && layout.clear.right <= layout.close.left &&
                layout.close.right <= field.right && layout.input.right - layout.input.left >= 39.9f * scale;
        }
    }
    Check(layouts_ok, L"address search: controls and input fit narrow and high-DPI layouts");
    auto state = std::make_unique<AppState>();
    Check(!state->addressSearchCurrent, L"address search: default scope is entire index");
    state->addressSearching = true;
    ULONGLONG now = 1000;
    bool bounded = true;
    for (int i = 0; i < 40; ++i) {
        const float before = state->addressSearchAnimation;
        TickAddressSearch(*state, now += 16);
        bounded &= state->addressSearchAnimation >= before && state->addressSearchAnimation <= 1.0f;
    }
    Check(bounded && state->addressSearchAnimation == 1.0f,
          L"address search: entrance animation settles without overshoot");
    state->addressSearching = false;
    state->addressScopeAnimation = 1.0f;
    for (int i = 0; i < 40; ++i) TickAddressSearch(*state, now += 16);
    Check(state->addressSearchAnimation == 0.0f && state->addressScopeAnimation == 0.0f &&
          !TickAddressSearch(*state, now + 16), L"address search: exit and scope feedback stop repainting");
    AdvancedSearchSpec spec;
    spec.name = L"report 2026";
    spec.current_folder = L"C:\\Users\\TestUser\\Desktop";
    Check(SplitSearchQueryText(CompileSearchQuery(spec)).path_prefix.empty(),
          L"address search: entire index omits current directory constraint");
    spec.location = LocationScope::CurrentFolder;
    Check(!SplitSearchQueryText(CompileSearchQuery(spec)).path_prefix.empty(),
          L"address search: current folder adds recursive directory scope");
}

AppState* history_interaction_state = nullptr;
int history_interaction_mode = 0;
bool history_interaction_driven = false;
int history_interaction_ticks = 0;

bool history_scope_requested = false;
bool history_layout_ok = false;

void CALLBACK DriveHistoryInteractionTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    if (++history_interaction_ticks > 100) {
        if (history_interaction_state && history_interaction_state->menu)
            history_interaction_state->menu->Dismiss();
        KillTimer(nullptr, timer);
        return;
    }
    if (history_interaction_ticks < 8) return;
    if (history_interaction_mode == 3 && history_interaction_state && history_interaction_state->menu) {
        auto& state = *history_interaction_state;
        if (!history_scope_requested) {
            history_scope_requested = true;
            history_interaction_ticks = 0;
            ShowAddressSearchScope(state);
            return;
        }
        if (state.searchHistoryOpen || !state.menu->IsOpen()) return;
        const auto scope = ui::LayoutAddressSearch(state.renderer.SearchBarRect(
            static_cast<float>(state.compositor.Width())), state.scale).scope;
        POINT expected{static_cast<LONG>(scope.left), static_cast<LONG>(scope.bottom)};
        ClientToScreen(state.hwnd, &expected);
        history_layout_ok = ui::FluentMenuTestPeer::ScopeLayout(*state.menu,
            static_cast<float>(expected.x), static_cast<float>(expected.y) + static_cast<int>(4 * state.scale));
        history_interaction_driven = true;
        state.menu->Dismiss();
        KillTimer(nullptr, timer);
        return;
    }
    if (history_interaction_mode == 4 && history_interaction_state && history_interaction_state->menu) {
        auto& state = *history_interaction_state;
        HWND edit = state.hwndAddressEdit;
        Check(ui::FluentMenuTestPeer::SeedCachedEditor(*state.menu), L"live focus: reproduce previously used menu editor");
        SetFocus(edit);
        SetWindowTextW(edit, L"");
        std::wstring query;
        bool retained = true, updated = true;
        for (wchar_t ch : std::wstring(L"showdebug")) {
            query.push_back(ch);
            HWND target = GetFocus();
            if (target) SendMessageW(target, WM_CHAR, ch, 0);
            QueueAddressSearch(state);
            TickAddressSearch(state, state.addressLiveDue);
            auto* tab = ActiveTab(state);
            if (tab->pending_generation)
                DeliverIndexSearchResult(state, static_cast<uint32_t>(tab->pending_generation), {});
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            retained &= GetFocus() == edit && start == query.size() && end == query.size();
            updated &= app::SearchDisplayNeedle(tab->current_path.substr(13)) == query;
        }
        Check(retained, L"live focus: every character and result refresh retain focus and caret");
        Check(updated, L"live focus: results keep updating throughout continuous typing");
        SendMessageW(GetFocus(), WM_CHAR, VK_BACK, 0);
        QueueAddressSearch(state);
        TickAddressSearch(state, state.addressLiveDue);
        Check(GetFocus() == edit && GetWindowTextLengthW(edit) == 8,
              L"live focus: backspace updates query without losing focus");
        SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
        const auto generation = ActiveTab(state)->pending_generation;
        SetWindowTextW(edit, L"中文");
        QueueAddressSearch(state);
        TickAddressSearch(state, state.addressLiveDue);
        Check(GetFocus() == edit && ActiveTab(state)->pending_generation == generation,
              L"live focus: IME composition retains focus and defers partial search");
        SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
        TickAddressSearch(state, state.addressLiveDue);
        Check(GetFocus() == edit && GetWindowTextLengthW(edit) == 2,
              L"live focus: committed Unicode query retains focus");
        history_layout_ok = ui::FluentMenuTestPeer::ExternalEditorLayout(*state.menu, edit);
        history_interaction_driven = true;
        state.menu->Dismiss();
        KillTimer(nullptr, timer);
        return;
    }
    if (history_interaction_state && history_interaction_state->menu) {
        auto& state = *history_interaction_state;
        history_layout_ok = ui::FluentMenuTestPeer::ExternalEditorLayout(*state.menu, state.hwndAddressEdit);
    }
    if (history_interaction_state && history_interaction_state->menu &&
        ui::FluentMenuTestPeer::DriveHistoryInteraction(
            *history_interaction_state->menu, history_interaction_mode)) {
        history_interaction_driven = true;
        KillTimer(nullptr, timer);
    }
}

void TestAddressSearchHistoryInteraction(float scale = 1.0f, bool dark = false) {
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->searchHistory.persist = false;
    state->scale = scale;
    state->darkMode = dark;
    WNDCLASSW fixture_class{};
    fixture_class.lpfnWndProc = DefWindowProcW;
    fixture_class.hInstance = GetModuleHandleW(nullptr);
    fixture_class.lpszClassName = L"PulseHistoryTestFixture";
    RegisterClassW(&fixture_class);
    state->hwnd = CreateWindowExW(WS_EX_TOPMOST, fixture_class.lpszClassName, L"", WS_POPUP,
        40, 40, static_cast<int>(1000 * scale), static_cast<int>(600 * scale),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const bool graphics_ready = state->hwnd && state->compositor.Init(state->hwnd);
    Check(graphics_ready, L"search history: initialize application popup fixture");
    if (!graphics_ready) {
        if (state->hwnd) DestroyWindow(state->hwnd);
        return;
    }
    state->compositor.RecreateTextFormats(scale);
    state->renderer.SetCompositor(&state->compositor);
    state->renderer.SetScale(scale);
    ShowWindow(state->hwnd, SW_SHOWNORMAL);
    Pane pane;
    pane.view.current_path = L"C:\\pulse-history-test-no-disk-access";
    state->pane = &pane;
    AdvancedSearchSpec spec;
    spec.name = L"saved-history-query";
    spec.kind = index::SearchKind::Custom;
    spec.location = LocationScope::CurrentFolder;
    spec.current_folder = pane.view.current_path;
    spec.custom_exts = L"pdf;txt";
    spec.exclude_name = L"draft";
    const auto history_path = MakeSearchPath(CompileSearchQuery(spec));
    RecordSearchHistory(*state, history_path);
    RecordSearchHistory(*state, L"C:\\not-a-search");
    Check(state->searchHistory.entries.size() == 1 &&
          state->searchHistory.entries.front().path == history_path,
          L"search history: application records exact search path and ignores folders");
    ShowAddressSearch(*state);
    SetWindowTextW(state->hwndAddressEdit, L"ShowBoxDebug.log");
    const auto original_path = pane.view.current_path;
    auto* dc = state->compositor.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(dark ? 0x202020 : 0xf5f5f5));
    state->renderer.Render(BuildVm(*state), D2D1::RectF(0, 0, 1000 * scale, 600 * scale),
                           ui::MakeTheme(dark, D2D1::ColorF(0x0078d4)));
    dc->EndDraw();
    state->compositor.Present();
    auto run_popup = [&](int mode) {
        MSG pending{};
        while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
        }
        history_interaction_state = state.get();
        history_interaction_mode = mode;
        history_interaction_driven = false;
        history_interaction_ticks = 0;
        history_layout_ok = false;
        history_scope_requested = false;
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, DriveHistoryInteractionTimer);
        Check(timer != 0, L"search history: schedule popup keyboard interaction");
        if (timer) {
            ShowAddressSearchHistory(*state);
            KillTimer(nullptr, timer);
        }
        history_interaction_state = nullptr;
        Check(history_layout_ok, mode == 3
            ? L"search history: switching to scope resets history anchor and aligns scope button"
            : L"search history: only original editor is visible and results stay below it");
        Check(history_interaction_driven && !state->searchHistoryOpen,
              L"search history: real popup processes keyboard and closes");
    };
    run_popup(0);
    Check(pane.view.current_path == original_path && state->pendingIndexSearches.empty() &&
          state->searchHistory.entries.size() == 1,
          L"search history: Escape preserves path and does not submit search");
    run_popup(1);
    Check(pane.view.current_path == history_path && state->addressSearching &&
          state->addressSearchCurrent && state->addressSearchRoot == spec.current_folder,
          L"search history: selected entry restores exact path, scope and filters");
    // Index clients and filesystem workers are deliberately never started in this fixture.
    state->pendingIndexSearches.clear();
    run_popup(2);
    std::wstring raw;
    ParsePulsePath(pane.view.current_path, nullptr, &raw);
    const auto submitted = ParseSearchQuery(raw);
    Check(submitted.name == L"new-unmatched-query-72941" &&
          !state->pendingIndexSearches.empty() && state->searchHistory.entries.size() == 2,
          L"search history: Enter with no matching history submits the typed query");
    Check(submitted.kind == spec.kind && submitted.custom_exts == spec.custom_exts &&
          submitted.exclude_name == spec.exclude_name &&
          SplitSearchQueryText(raw).path_prefix == SplitSearchQueryText(CompileSearchQuery(spec)).path_prefix,
          L"search history: typed continuation keeps restored scope and filters");
    run_popup(3);
    Check(!state->searchScopePending, L"search history: scope transition consumes pending request");
    run_popup(4);
    HideAddressEditor(*state, false);
    if (state->hwndAddressEdit) DestroyWindow(state->hwndAddressEdit);
    state->hwndAddressEdit = nullptr;
    state->menu.reset();
    state->compositor.Shutdown();
    DestroyWindow(state->hwnd);
    state->hwnd = nullptr;
    state->pane = nullptr;
    if (state->editFont) DeleteObject(state->editFont);
    if (state->editBrush) DeleteObject(state->editBrush);
}

void TestGlobalSearchHandoff() {
    Check(GlobalSearchHandoffPath({}).empty() && GlobalSearchHandoffPath({L"", true, L"C:\\Docs"}).empty(),
          L"global search handoff: empty query opens the search box, not a results tab");
    GlobalSearchHandoff request;
    request.query = L"季度 预算";
    std::wstring kind, rest;
    bool parsed = ParsePulsePath(GlobalSearchHandoffPath(request), &kind, &rest);
    auto spec = ParseSearchQuery(rest);
    Check(parsed && kind == L"search" && spec.name == request.query && spec.content.empty() &&
          spec.location == LocationScope::Indexed,
          L"global search handoff: name query searches every indexed location");
    request.content = true;
    request.folder = L"C:\\Users\\TestUser\\Documents";
    const auto scoped = GlobalSearchHandoffPath(request);
    parsed = ParsePulsePath(scoped, &kind, &rest);
    spec = ParseSearchQuery(rest, request.folder);
    Check(parsed && kind == L"search" && spec.content == request.query && spec.name.empty() &&
          spec.location == LocationScope::CurrentFolder && spec.custom_folder == request.folder,
          L"global search handoff: content query keeps the current-folder scope");
    auto state = std::make_unique<AppState>();
    Pane pane;
    state->pane = &pane;
    pane.view.current_path = scoped;
    ui::WindowViewModel vm;
    FillAddressSearchView(*state, vm);
    Check(vm.address_search_text == request.query && vm.address_search_current && vm.address_search_content,
          L"global search handoff: main search box shows the same text, mode and scope");
}

void TestContinuousSearch() {
    auto state = std::make_unique<AppState>();
    Pane first;
    Pane second;
    state->pane = &first;
    first.view.current_path = MakeSearchPath(L"contract path:C:\\Documents");
    ui::WindowViewModel vm;
    FillAddressSearchView(*state, vm);
    Check(vm.address_searching && vm.address_search_text == L"contract" && vm.address_search_current,
          L"continuous search: results restore query and current-folder scope");
    state->hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"invoice", WS_POPUP,
                                            0, 0, 100, 30, nullptr, nullptr, nullptr, nullptr);
    Check(state->hwndAddressEdit != nullptr, L"continuous search: create hidden native edit fixture");
    if (state->hwndAddressEdit) {
        state->addressSearching = true;
        state->addressSearchRoot = L"C:\\Documents";
        state->addressSearchCurrent = false;
        SaveAddressSearchDraft(*state);
        state->addressSearching = false;
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text == L"invoice" && !vm.address_search_current,
              L"continuous search: unsubmitted draft and changed scope survive blur");
        state->pane = &second;
        second.view.current_path = MakeSearchPath(L"other");
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text == L"other" && !vm.address_search_current,
              L"continuous search: another tab has independent input");
        state->pane = &first;
        SetWindowTextW(state->hwndAddressEdit, L"");
        state->addressSearching = true;
        SaveAddressSearchDraft(*state);
        state->addressSearching = false;
        vm = {};
        FillAddressSearchView(*state, vm);
        Check(vm.address_search_text.empty() && !vm.address_search_has_text &&
              first.view.current_path == MakeSearchPath(L"contract path:C:\\Documents"),
              L"continuous search: clearing input preserves existing query results");
        DestroyWindow(state->hwndAddressEdit);
        state->hwndAddressEdit = nullptr;
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(1);
    (*entries)[0].name = L"old.txt";
    first.view.SetSnapshot(entries);
    first.view.loading = true;
    first.view.search_retaining_results = true;
    first.view.SelectAll();
    Check(first.view.snapshot->size() == 1 && first.view.CountBound() == 0 &&
          first.view.SelectedIndices().empty(), L"continuous search: old results stay visible but inactive");
    first.view.pending_search_offset = 0;
    index::SearchResult result;
    ApplySearchHits(first.view, L"empty", std::move(result));
    Check(!first.view.loading && !first.view.search_retaining_results && first.view.snapshot->empty(),
          L"continuous search: empty response replaces old results and completes loading");
    state->pane = nullptr;
}

void TestLiveAddressSearch() {
    auto state = std::make_unique<AppState>();
    state->searchHistory.persist = false;
    state->places.persist = false;
    auto& pane = *state->window_tabs.NewTab(L"C:\\live-search-test").FocusedPane();
    state->pane = &pane;
    pane.view.current_path = L"C:\\live-search-test";
    state->hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 400, 100, nullptr, nullptr, nullptr, nullptr);
    state->hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
        0, 0, 300, 30, state->hwnd, nullptr, nullptr, nullptr);
    state->addressSearching = state->addressEditing = true;
    auto type = [&](const wchar_t* query) {
        SetWindowTextW(state->hwndAddressEdit, query);
        SendMessageW(state->hwndAddressEdit, EM_SETSEL, wcslen(query), wcslen(query));
        QueueAddressSearch(*state);
    };
    type(L"show");
    const auto due = state->addressLiveDue;
    TickAddressSearch(*state, due - 1);
    Check(state->pendingIndexSearches.empty(), L"live search: debounce waits for short input pause");
    TickAddressSearch(*state, due);
    const auto old_generation = static_cast<uint32_t>(pane.view.pending_generation);
    Check(old_generation != 0 && pane.view.current_path == MakeSearchPath(L"show"),
          L"live search: typing starts a search without Enter");
    const auto back_count = pane.view.back_stack.size();
    type(L"showdebug");
    TickAddressSearch(*state, state->addressLiveDue);
    DWORD start = 0, end = 0;
    SendMessageW(state->hwndAddressEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    Check(pane.view.current_path == MakeSearchPath(L"showdebug") && start == 9 && end == 9 &&
          state->addressSearching && pane.view.back_stack.size() == back_count,
          L"live search: continued typing updates query without selecting text or adding back entries");
    const auto latest = pane.view.pending_generation;
    DeliverIndexSearchResult(*state, old_generation, {});
    Check(pane.view.pending_generation == latest && pane.view.loading,
          L"live search: stale response cannot replace the newer query");
    DeliverIndexSearchResult(*state, static_cast<uint32_t>(latest), {});
    Check(!pane.view.loading && pane.view.pending_generation == 0,
          L"live search: current response replaces displayed results");
    Check(state->searchHistory.entries.empty(), L"live search: intermediate keystrokes are not saved");
    TickAddressSearch(*state, state->addressHistoryDue);
    Check(state->searchHistory.entries.size() == 1 && state->searchHistory.entries.front().query == L"showdebug",
          L"live search: settled query is recorded once");
    state->addressSearchComposing = true;
    type(L"中文");
    TickAddressSearch(*state, state->addressLiveDue);
    Check(pane.view.pending_generation == 0, L"live search: IME composition does not dispatch partial text");
    state->addressSearchComposing = false;
    QueueAddressSearch(*state);
    TickAddressSearch(*state, state->addressLiveDue);
    Check(pane.view.current_path == MakeSearchPath(L"中文"), L"live search: committed IME text searches");
    type(L"");
    TickAddressSearch(*state, state->addressLiveDue);
    Check(pane.view.current_path == MakeSearchPath(L"") && state->addressHistoryDue == 0,
          L"live search: clearing input updates results without recording an empty query");
    type(L"cancel-on-context-change");
    pane.view.current_path = L"C:\\other";
    TickAddressSearch(*state, state->addressLiveDue);
    Check(pane.view.current_path == L"C:\\other", L"live search: pending input cannot search a changed context");
    state->addressSearching = false;
    DestroyWindow(state->hwndAddressEdit);
    DestroyWindow(state->hwnd);
    state->hwndAddressEdit = state->hwnd = nullptr;
    state->pane = nullptr;
}

int advanced_test_stage = 0;
int advanced_test_ticks = 0;
int advanced_test_mode = 0;
bool advanced_hidden_on_destroy = false;
HWND advanced_test_window = nullptr;

LRESULT CALLBACK AdvancedTestWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR, DWORD_PTR) {
    if (msg == WM_DESTROY) advanced_hidden_on_destroy = !IsWindowVisible(hwnd);
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

HWND FindAdvancedTestWindow(const wchar_t* class_name) {
    struct Context { const wchar_t* name; HWND window = nullptr; } context{class_name};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM param) -> BOOL {
        auto& value = *reinterpret_cast<Context*>(param);
        wchar_t name[128]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        if (wcscmp(name, value.name) == 0 && IsWindowVisible(window)) {
            value.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

// Hit ids from advanced_search_dialog.cpp (kHit*); fields answer 1000+.
constexpr int kAdvHitWholeWord = 11, kAdvHitClear = 13, kAdvHitMore = 20, kAdvHitMode = 40,
              kAdvHitNameHow = 500, kAdvFieldExts = 1001, kAdvFieldExclude = 1002;
constexpr int kAdvPadding = 0x10000;

// Where the dialog lays out target `id` (client px), via its selftest hook.
bool AdvancedHitPoint(HWND dialog, int id, POINT* point) {
    static const UINT message = RegisterWindowMessageW(L"Pulse.Test.AdvancedHitCenter");
    const LRESULT at = SendMessageW(dialog, message, static_cast<WPARAM>(id), 0);
    if (at == -1 || at == 0) return false;
    point->x = static_cast<short>(LOWORD(static_cast<DWORD_PTR>(at)));
    point->y = static_cast<short>(HIWORD(static_cast<DWORD_PTR>(at)));
    return true;
}

void CALLBACK AdvancedTestTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    if (++advanced_test_ticks > 150) {
        if (advanced_test_window) PostMessageW(advanced_test_window, WM_CLOSE, 0, 0);
        KillTimer(nullptr, timer);
        return;
    }
    HWND dialog = FindAdvancedTestWindow(L"PulseAdvancedSearchWindow");
    if (!dialog) return;
    auto click = [&](int id) {
        POINT at{};
        if (!AdvancedHitPoint(dialog, id, &at)) return false;
        const LPARAM pt = MAKELPARAM(at.x, at.y);
        PostMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, pt);
        PostMessageW(dialog, WM_LBUTTONUP, 0, pt);
        return true;
    };
    if (advanced_test_stage == 0) {
        advanced_test_window = dialog;
        SetWindowSubclass(dialog, AdvancedTestWindowProc, 80, 0);
        SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM param) -> BOOL {
            if (GetWindow(window, GW_OWNER) == reinterpret_cast<HWND>(param)) {
                SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(dialog));
        advanced_test_stage = 1;
        advanced_test_ticks = 0;
        return;
    }
    if (advanced_test_ticks < 8) return;
    if (advanced_test_stage == 1) {
        if (advanced_test_mode == 0) {
            PostMessageW(dialog, WM_CLOSE, 0, 0);
            KillTimer(nullptr, timer);
        } else if (advanced_test_mode == 1) {
            if (click(kAdvHitMore)) advanced_test_stage = 2;
        } else if (advanced_test_mode == 2) {
            if (click(kAdvHitClear)) advanced_test_stage = 4;
        } else if (click(kAdvHitMode + 1)) {
            advanced_test_stage = 3;
        }
        return;
    }
    if (advanced_test_stage == 2) {
        // 更多选项 is open: "starts with".
        if (click(kAdvHitNameHow + 1)) advanced_test_stage = 4;
        return;
    }
    if (advanced_test_stage == 3) {
        // Content mode carries 全字 inside the keyword box.
        if (click(kAdvHitWholeWord)) advanced_test_stage = 4;
        return;
    }
    PostMessageW(dialog, WM_COMMAND, IDOK, 0);
    KillTimer(nullptr, timer);
}

void TestAdvancedSearchDialog() {
    const auto language = l10n::preference();
    for (int mode = 0; mode < 4; ++mode) {
        l10n::SetLanguage(mode == 2 ? L"en-US" : l10n::LanguageId(language));
        advanced_test_mode = mode;
        advanced_test_ticks = advanced_test_stage = 0;
        advanced_hidden_on_destroy = false;
        advanced_test_window = nullptr;
        AdvancedSearchSpec spec;
        spec.name = L"show";
        spec.current_folder = L"C:\\Documents";
        spec.location = LocationScope::CurrentFolder;
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, AdvancedTestTimer);
        if (!timer) { Check(false, L"advanced search: timer fixture"); return; }
        const auto result = ui::ShowAdvancedSearchDialog(nullptr, spec, mode != 2, D2D1::ColorF(0x0078d4));
        KillTimer(nullptr, timer);
        Check(advanced_hidden_on_destroy, L"advanced search: cold and repeat close hide before surface teardown");
        const auto parsed = ParseSearchQuery(result.query);
        if (mode == 0) Check(!result.accepted, L"advanced search: close cancels without executing");
        else if (mode == 1) {
            Check(result.accepted && result.query.find(L"show*") != std::wstring::npos && !parsed.whole_word,
                  L"advanced search: starts-with pill compiles into search");
        } else if (mode == 2) {
            // 清空 keeps the folder the dialog was opened for.
            Check(result.accepted && result.query.find(L"show") == std::wstring::npos &&
                      result.query.find(L"path:") != std::wstring::npos,
                  L"advanced search: clear all resets conditions but keeps the current folder");
        } else {
            Check(result.accepted && parsed.whole_word && result.query.find(L"show") != std::wstring::npos,
                  L"advanced search: content mode and whole word compile into search");
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
}

int advanced_edit_stage = 0;

// No image capture: exercise the actual dialog's mouse target and editing path.
void CALLBACK AdvancedEditClickTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = FindAdvancedTestWindow(L"PulseAdvancedSearchWindow");
    if (!dialog) return;
    if (advanced_edit_stage == 0) {
        // The extension and exclude fields live under 更多选项.
        POINT more{};
        if (!AdvancedHitPoint(dialog, kAdvHitMore, &more)) return;
        PostMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(more.x, more.y));
        PostMessageW(dialog, WM_LBUTTONUP, 0, MAKELPARAM(more.x, more.y));
        advanced_edit_stage = 1;
        return;
    }
    POINT probe{};
    if (!AdvancedHitPoint(dialog, kAdvFieldExclude, &probe)) return;
    KillTimer(nullptr, timer);
    for (int field : {kAdvFieldExts, kAdvFieldExclude}) {
        for (int where : {0, kAdvPadding}) {
            POINT point{};
            const bool laid_out = AdvancedHitPoint(dialog, field | where, &point);
            Check(laid_out, L"advanced edit: field is laid out under more options");
            if (!laid_out) continue;
            const LONG y = point.y;
            // Query this dialog's child tree without depending on other apps' Z order.
            HWND target = ChildWindowFromPointEx(dialog, point,
                CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
            if (where == 0) Check(IsChild(dialog, target),
                L"advanced edit: empty unfocused field has a hittable child surface");
            Check(target == dialog || IsChild(dialog, target), L"advanced edit: click targets dialog or its child");
            if (target != dialog && !IsChild(dialog, target)) continue;
            MapWindowPoints(dialog, target, &point, 1);
            SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            HWND edit = GetFocus();
            wchar_t cls[32]{};
            GetClassNameW(edit, cls, ARRAYSIZE(cls));
            Check(IsChild(dialog, edit) && wcscmp(cls, L"Edit") == 0,
                  L"advanced edit: center and padding clicks focus native editor");
            RECT bounds{};
            GetWindowRect(edit, &bounds);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
            Check(bounds.top < y && bounds.bottom > y,
                  L"advanced edit: correct extension or exclusion field receives focus");
            SendMessageW(edit, WM_LBUTTONUP, 0, 0);
            SendMessageW(edit, WM_CHAR, L'中', 0);
            Check(GetWindowTextLengthW(edit) > 0, L"advanced edit: clicked field accepts Unicode input");
            SetWindowTextW(edit, L"");
        }
    }
    PostMessageW(dialog, WM_CLOSE, 0, 0);
}

void TestAdvancedEditClicks() {
    for (bool dark : {true, false}) {
        AdvancedSearchSpec spec;
        spec.name = L"showbox";
        advanced_edit_stage = 0;
        const UINT_PTR timer = SetTimer(nullptr, 0, 150, AdvancedEditClickTimer);
        Check(timer != 0, L"advanced edit: create click fixture");
        if (!timer) return;
        ui::ShowAdvancedSearchDialog(nullptr, spec, dark, D2D1::ColorF(0x0078d4));
        KillTimer(nullptr, timer);
    }
}

void TestCtrlDragSelection() {
    auto state = std::make_unique<AppState>();
    Pane pane;
    state->pane = &pane;
    auto& tab = pane.view;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(3));
    tab.SelectOnly(0);
    HandleListRowClick(*state, 0, true, false);
    Check(tab.IsSelected(0) && tab.SelectedCount() == 1,
          L"Ctrl-drag: selected file remains available on press");
    FinishListRowClick(*state);
    Check(!tab.IsSelected(0), L"Ctrl-click: release still deselects selected file");
    tab.SelectOnly(0);
    tab.ToggleSelect(1);
    HandleListRowClick(*state, 0, true, false);
    Check(tab.IsSelected(0) && tab.IsSelected(1) && tab.SelectedCount() == 2,
          L"Ctrl-drag: preserves all selected files and folders");
    // Starting a drag clears the deferred click, as does capture cancellation.
    state->clickCollapseIndex = -1;
    FinishListRowClick(*state);
    Check(tab.SelectedCount() == 2, L"Ctrl-drag: drag completion does not deselect source");
    HandleListRowClick(*state, 2, true, false);
    FinishListRowClick(*state);
    Check(tab.IsSelected(2) && tab.SelectedCount() == 3,
          L"Ctrl-click: unselected item is added exactly once");
    HandleListRowClick(*state, 1, false, false);
    Check(tab.SelectedCount() == 3, L"drag: plain press preserves multi-selection");
    FinishListRowClick(*state);
    Check(tab.IsSelected(1) && tab.SelectedCount() == 1,
          L"click: plain release collapses multi-selection");
    state->pane = nullptr;
}

void TestMenuModel() {
    if (!g_skip_visual) {
    HWND snapshot_window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor snapshot_compositor;
    if (snapshot_window && snapshot_compositor.Init(snapshot_window)) {
        const auto snapshot_dir = WorkspaceRoot() + L"\\bench_data";
        CreateDirectoryW(snapshot_dir.c_str(), nullptr);
        for (bool dark : {false, true}) {
            for (float scale : {1.0f, 1.5f}) {
                snapshot_compositor.RecreateTextFormats(scale);
                const auto path = snapshot_dir + L"\\history-menu-" +
                    (dark ? L"dark-" : L"light-") + (scale == 1.0f ? L"100.png" : L"150.png");
                Check(ui::FluentMenuTestPeer::SaveHistorySnapshot(snapshot_window,
                    snapshot_compositor, path, dark, scale), L"menu: history visual snapshot saved");
                const auto overflow_path = snapshot_dir + L"\\overflow-menu-" +
                    (dark ? L"dark-" : L"light-") + (scale == 1.0f ? L"100.png" : L"150.png");
                Check(ui::FluentMenuTestPeer::SaveOverflowSnapshot(snapshot_window,
                    snapshot_compositor, overflow_path, dark, scale),
                    L"menu overflow: visual snapshot saved");
            }
        }
        snapshot_compositor.Shutdown();
    } else {
        Check(false, L"menu: snapshot graphics initialized");
    }
    if (snapshot_window) DestroyWindow(snapshot_window);
    }
    Check(ui::FluentMenuTestPeer::CheckHistoryRows(1.0f),
          L"menu: history actions and scrolling at 100 percent DPI");
    Check(ui::FluentMenuTestPeer::CheckHistoryRows(1.5f),
          L"menu: history actions and scrolling at 150 percent DPI");
    for (float scale : {1.0f, 1.5f}) {
        const bool hi = scale != 1.0f;
        Check(ui::FluentMenuTestPeer::CheckOverflowFit(scale), hi
            ? L"menu overflow: a menu taller than the screen stays on it with arrows (#67, 150%)"
            : L"menu overflow: a menu taller than the screen stays on it with arrows (#67, 100%)");
        Check(ui::FluentMenuTestPeer::CheckOverflowPointer(scale), hi
            ? L"menu overflow: arrows page on click and rows hit-test through the scroll (150%)"
            : L"menu overflow: arrows page on click and rows hit-test through the scroll (100%)");
        Check(ui::FluentMenuTestPeer::CheckOverflowKeysAndWheel(scale), hi
            ? L"menu overflow: keyboard selection stays between the arrows; scrolling clamps (150%)"
            : L"menu overflow: keyboard selection stays between the arrows; scrolling clamps (100%)");
        Check(ui::FluentMenuTestPeer::CheckOverflowArrowHover(scale), hi
            ? L"menu overflow: resting on an arrow keeps scrolling until the pointer leaves (150%)"
            : L"menu overflow: resting on an arrow keeps scrolling until the pointer leaves (100%)");
    }
    Check(ui::FluentMenuTestPeer::CheckSubmenuColors(1.0f),
          L"menu: all submenu color dots hover and dispatch at 100 percent DPI");
    Check(ui::FluentMenuTestPeer::CheckSubmenuColors(1.5f),
          L"menu: all submenu color dots hover and dispatch at 150 percent DPI");
    const auto breadcrumb = BuildBreadcrumbMenu(true);
    const std::vector<int> breadcrumb_commands{CmdOpenInNewTab, CmdOpen, CmdCopyPath,
                                              CmdCopy, CmdOpenTerminal, CmdProperties};
    bool breadcrumb_ok = breadcrumb.size() == breadcrumb_commands.size();
    for (size_t i = 0; i < breadcrumb.size() && i < breadcrumb_commands.size(); ++i) {
        breadcrumb_ok &= breadcrumb[i].command == breadcrumb_commands[i] &&
                         breadcrumb[i].enabled && !breadcrumb[i].text.empty() &&
                         breadcrumb[i].shortcut.empty();
    }
    Check(breadcrumb_ok, L"breadcrumb: explicit folder actions with new tab first");
    const auto virtual_breadcrumb = BuildBreadcrumbMenu(false);
    Check(virtual_breadcrumb.size() == 3 &&
          virtual_breadcrumb[0].command == CmdOpenInNewTab &&
          virtual_breadcrumb[1].command == CmdOpen &&
          virtual_breadcrumb[2].command == CmdCopyPath,
          L"breadcrumb: virtual locations omit filesystem actions");
    ui::ComPtr<IDWriteFactory2> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                        reinterpret_cast<IUnknown**>(&dwrite));

    // Item menu: 打开 + icon strip (cut/copy/delete/rename) + verbs + undo.
    auto items = BuildItemMenu(false, L"");
    Check(items.size() == 9, L"menu: item menu is 打开+图标条+verbs+undo");
    // Command-id ranges must not overlap: CmdTabJoinGroupBase once collided
    // with CmdTabCloseOthers and "join group" closed every other tab.
    static_assert(CmdTabJoinGroupBase > CmdTabCloseRight &&
                  CmdTabJoinGroupBase + 32 <= CmdRecentBase,
                  "join-group ids must sit between tab commands and recents");
    std::vector<int> want{ CmdOpen, CmdNone, CmdCopyPath, CmdOpenTerminal, CmdProperties,
                           CmdPinWorkspace, CmdPinNetwork, CmdTags, CmdUndo };
    bool ids_ok = items.size() >= want.size();
    for (size_t i = 0; i < want.size() && i < items.size(); ++i)
        if (items[i].command != want[i]) ids_ok = false;
    Check(ids_ok, L"menu: verb order and ids");
    Check(items.size() > 1 && items[1].quick_swatches.size() == 4 &&
          items[1].quick_swatches[0].command == CmdCut &&
          items[1].quick_swatches[1].command == CmdCopy &&
          items[1].quick_swatches[2].command == CmdDelete &&
          items[1].quick_swatches[3].command == CmdRename &&
          !items[1].quick_swatches[0].glyph.empty(),
          L"menu: icon strip carries 剪切/复制/删除/重命名");
    Check(items.size() > 2 && items[2].shortcut == L"Ctrl+Shift+C",
          L"menu: 复制路径 carries Ctrl+Shift+C");
    auto folder_items = BuildItemMenu(false, L"", true);
    Check(folder_items.size() == 10, L"menu: folder item menu adds 在新标签打开");
    Check(folder_items.size() > 3 &&
          folder_items[2].command == CmdOpenInNewTab &&
          folder_items[2].text == L"在新标签页打开" &&
          folder_items[3].command == CmdCopyPath,
          L"menu: 在新标签打开 sits before 复制路径");
    Check(!items.back().enabled, L"menu: undo disabled without a stack");

    auto recycle_items = BuildRecycleItemMenu(false, L"");
    Check(!recycle_items.empty() && recycle_items[0].command == CmdRestoreRecycle &&
          recycle_items[1].command == CmdDelete,
          L"menu: recycle item menu is restore + permanent delete");
    auto recycle_bg = BuildRecycleBackgroundMenu(true, L"撤销", true);
    Check(!recycle_bg.empty() && recycle_bg[0].command == CmdEmptyRecycle &&
          recycle_bg[0].enabled,
          L"menu: recycle background can empty the bin");
    auto recycle_place = BuildRecyclePlaceMenu(true);
    Check(recycle_place.size() == 2 && recycle_place[0].command == CmdOpenRecycle &&
          recycle_place[1].command == CmdEmptyRecycle,
          L"menu: recycle quick access pin opens and empties");
    {
        std::vector<ui::FluentMenuItem> en_place;
        ui::FluentMenuItem open_en;
        open_en.command = CmdOpenRecycle;
        open_en.text = L"Open";
        en_place.push_back(std::move(open_en));
        ui::FluentMenuItem empty_en;
        empty_en.command = CmdEmptyRecycle;
        empty_en.text = L"Empty Recycle Bin";
        en_place.push_back(std::move(empty_en));
        ui::FluentMenuModel en_model;
        en_model.SetItems(std::move(en_place));
        en_model.Layout(dwrite.get(), 1.0f);
        ui::ComPtr<IDWriteTextFormat> fmt;
        ui::typography::CreateTextFormat(dwrite.get(),
            {ui::typography::FontRole::Text, ui::FluentMenuModel::kLabelDip,
             DWRITE_FONT_WEIGHT_NORMAL}, &fmt);
        float text_w = 0.0f;
        if (fmt.get()) {
            ui::ComPtr<IDWriteTextLayout> layout;
            const wchar_t* label = L"Empty Recycle Bin";
            dwrite->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)),
                                     fmt.get(), 10000.0f, 100.0f, &layout);
            DWRITE_TEXT_METRICS metrics{};
            if (layout.get()) layout->GetMetrics(&metrics);
            text_w = (std::max)(metrics.width, metrics.widthIncludingTrailingWhitespace);
        }
        // DrawMenuItem chrome since c7b98c6: inset 6 + pad 10 + icon 16 + gap 8,
        // then pad 10 + inset 6 (FluentMenuModel::Layout reserves the same).
        Check(static_cast<float>(en_model.WidthPx()) + 0.5f >= 56.0f + text_w,
              L"menu: Empty Recycle Bin fits in the flyout");
    }

    ui::FluentMenuModel model;
    model.SetItems(items);
    model.Layout(dwrite.get(), 1.0f);    int seps = 0;
    for (const auto& it : items) if (it.separator_after) ++seps;
    int expect_h = (int)(4 * 2 + 36.0f * (int)items.size() + 5 * seps + 0.5f);
    Check(model.HeightPx() == expect_h, L"menu: layout height = rows*36 + separators");
    {
        // #27: the compact list density gives 26 DIP menu rows; out-of-range values clamp.
        ui::FluentMenuModel compact;
        compact.SetItems(items);
        compact.SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(28)));
        compact.Layout(dwrite.get(), 1.0f);
        const int compact_h = (int)(4 * 2 + 26.0f * (int)items.size() + 5 * seps + 0.5f);
        Check(compact.HeightPx() == compact_h && compact.RowHeightPx() == 26.0f &&
              compact.HitTestRow(4.0f + 26.0f * 1.5f + (items[0].separator_after ? 5.0f : 0.0f)) == 1,
              L"menu: compact density uses 26 DIP rows for layout and hit-testing");
        compact.SetRowHeightDip(10.0f);
        compact.Layout(dwrite.get(), 1.0f);
        const float min_row = compact.RowHeightPx();
        compact.SetRowHeightDip(80.0f);
        compact.Layout(dwrite.get(), 2.0f);
        Check(min_row == 26.0f && compact.RowHeightPx() == 80.0f,
              L"menu: row height clamps to 26..40 DIP and scales with DPI");
    }
    Check(model.WidthPx() >= 160 && model.WidthPx() <= 320, L"menu: width within clamp");

    auto long_undo = BuildItemMenu(true,
        L"撤销删除 万锦学校一期_结施_1#创新融合中心及运动场馆(运动场馆).dwg");
    ui::FluentMenuModel wide;
    wide.SetItems(std::move(long_undo));
    wide.Layout(dwrite.get(), 1.0f);
    Check(wide.WidthPx() <= 320, L"menu: long undo label does not stretch the flyout");
    {
        bool long_row_truncated = false;
        for (int i = 0; i < wide.Count(); ++i) {
            const auto* it = wide.At(i);
            if (it && it->text.find(L"万锦学校") != std::wstring::npos)
                long_row_truncated = wide.Truncated(i);
        }
        Check(long_row_truncated, L"menu: clamped long label is flagged for the full-text tooltip");
        bool any_short_truncated = false;
        for (int i = 0; i < model.Count(); ++i) any_short_truncated |= model.Truncated(i);
        Check(!any_short_truncated, L"menu: labels that fit are not flagged as truncated");
    }

    std::vector<index::Hit> path_hits{
        {L"C:\\Users\\TestUser\\.codex", L".codex", true},
        {L"C:\\Users\\TestUser\\plugins\\.codex-plugin", L".codex-plugin", true}
    };
    auto path_items = BuildCommandPalette(L".codex", {}, {}, path_hits, false, path_hits.size());
    Check(path_items.size() >= 3 && path_items[0].shortcut_inline &&
          path_items[1].shortcut_inline && !path_items.back().shortcut_inline,
          L"menu: search paths use inline captions, result count remains trailing");
    ui::FluentMenuModel path_model;
    path_model.SetItems(path_items);
    path_model.Layout(dwrite.get(), 1.0f, 2600.0f);
    const float short_column = path_model.InlineLabelWidthPx();
    Check(short_column > 0.0f && short_column < 160.0f,
          L"menu: wide search keeps paths near short filenames");
    path_model.Layout(dwrite.get(), 1.0f, 640.0f);
    Check(std::abs(path_model.InlineLabelWidthPx() - short_column) < 1.0f,
          L"menu: window width does not push short-name paths away");
    path_items[1].text = std::wstring(200, L'W');
    path_model.SetItems(path_items);
    path_model.Layout(dwrite.get(), 1.0f, 2600.0f);
    Check(path_model.InlineLabelWidthPx() <= 240.0f,
          L"menu: long filename leaves room for path");
    path_model.Layout(dwrite.get(), 1.0f, 320.0f);
    Check(path_model.InlineLabelWidthPx() < 100.0f,
          L"menu: narrow search shares space without overlap");
    path_model.Layout(dwrite.get(), 2.0f, 5200.0f);
    Check(std::abs(path_model.InlineLabelWidthPx() - 480.0f) < 1.0f,
          L"menu: filename column scales at 200 percent DPI");

    // Hit-test: first row, separator dead zone, disabled row skipped by nav.
    int row0 = model.HitTestRow(model.RowTopPx(0) + 1.0f);
    Check(row0 == 0, L"menu: hit-test row 0");
    int sep_row = -1;
    for (int i = 0; i < model.Count(); ++i)
        if (model.At(i)->separator_after) { sep_row = i; break; }
    float sep_y = model.RowTopPx(sep_row) + model.RowHeightPx() + 2.0f;
    Check(model.HitTestRow(sep_y) == -1, L"menu: separator is a dead zone");
    int last = model.Count() - 1; // undo, disabled
    Check(model.NextEnabled(last - 1, 1) == 0, L"menu: nav wraps and skips disabled undo");
    Check(model.FirstEnabled() == 0, L"menu: first enabled is 打开");

    // Simulated dispatch: hit-test -> command id -> verb.
    int row = model.HitTestRow(model.RowTopPx(0) + 1.0f);
    int cmd = (row >= 0 && model.At(row)->enabled) ? model.At(row)->command : 0;
    Check(cmd == CmdOpen, L"menu: hit-test -> dispatch 打开");

    // Background menu + new dropdown (toolbar reuses the same component).
    auto bg = BuildBackgroundMenu(true, true, L"撤销移动 a.txt");
    Check(!bg.empty() && bg[0].command == CmdNewFolder, L"menu: background menu starts with 新建文件夹");
    bool has_invert = false, has_wildcard = false, has_select_all = false;
    for (const auto& it : bg) {
        if (it.command == CmdInvertSelection) has_invert = true;
        if (it.command == CmdSelectWildcard) has_wildcard = true;
        if (it.command == CmdSelectAll) has_select_all = true;
    }
    Check(has_select_all && has_invert && has_wildcard,
          L"menu: background has 全选 / 反选 / 通配选择");
    BackgroundViewOptions options;
    options.view_mode = ui::ViewMode::LargeIcons;
    options.sort_column = ui::SortColumn::Size;
    options.sort_direction = ui::SortDirection::Desc;
    options.details_panel = true;
    AppendBackgroundViewCommands(bg, options);
    // Real folders end the view submenu with "apply to all folders"; 照片墙
    // follows the eight view modes.
    Check(bg[0].children.size() == 11 && bg[0].children[1].radio &&
          bg[0].children[9].command == CmdDetailsPanel && bg[0].children[9].checked &&
          bg[0].children.back().command == CmdApplyViewToAllFolders,
          L"menu: background reflects view and details pane");
    const auto& sort = bg[1].children;
    Check(sort.size() == 11 && sort[2].command == CmdSortCreated && sort[3].command == CmdSortAccessed &&
          sort[5].command == CmdSortSize && sort[5].radio &&
          !sort[6].radio && sort[7].radio && sort[7].separator_after &&
          sort[8].command == CmdFolderSortTop && sort[8].radio &&
          !sort[9].radio && !sort[10].radio,
          L"menu: background reflects size descending with separate radio groups");
    Check(bg[2].command == CmdRefresh && bg[2].shortcut == L"F5" &&
          bg.back().command == CmdFolderProperties,
          L"menu: background refresh and explicit folder properties");
    options.filesystem = false;
    options.show_path = true;
    options.sort_column = ui::SortColumn::Path;
    auto virtual_bg = BuildBackgroundMenu(false, false, L"");
    AppendBackgroundViewCommands(virtual_bg, options);
    Check(virtual_bg[1].children.size() == 10 && virtual_bg[1].children[4].radio &&
          virtual_bg.back().command != CmdFolderProperties,
          L"menu: search offers path sorting without folder properties");
    options.indexed_search = true;
    std::vector<ui::FluentMenuItem> search_menu;
    AppendBackgroundViewCommands(search_menu, options);
    Check(search_menu[1].children[0].enabled && !search_menu[1].children[2].enabled &&
          !search_menu[1].children[4].enabled,
          L"menu: indexed search disables unsupported type and path sorts");
    for (float scale : { 1.0f, 1.5f, 2.0f }) {
        ui::FluentMenuModel sort_model;
        sort_model.SetItems(sort);
        sort_model.Layout(dwrite.get(), scale);
        bool hit_tests_ok = true;
        for (int i = 0; i < sort_model.Count(); ++i)
            hit_tests_ok &= sort_model.HitTestRow(sort_model.RowTopPx(i) +
                sort_model.RowHeightPx() * 0.5f) == i;
        Check(hit_tests_ok && sort_model.WidthPx() <= static_cast<int>(320 * scale),
              L"menu: sort flyout layout and hit testing at 100/150/200 percent DPI");
    }
    options.can_sort = false;
    std::vector<ui::FluentMenuItem> curated;
    AppendBackgroundViewCommands(curated, options);
    Check(!curated[1].enabled && std::none_of(curated[1].children.begin(),
          curated[1].children.end(), [](const auto& item) { return item.enabled || item.radio; }),
          L"menu: curated views disable sorting and do not claim a selected sort");
    auto nw = BuildNewMenu();
    Check(nw.size() == 3 && nw[0].command == CmdNewFolder && nw[1].command == CmdNewTextFile &&
          nw[2].command == CmdNewMarkdownFile,
          L"menu: 新建▾ dropdown has 文件夹/文本文档");
}

// Explorer merge rules (优化.md §7): built-in verbs filtered, duplicate texts
// dropped, section appended at the bottom; software-owned submenus keep one
// level of hierarchy as a flyout (never flattened into the main list).
void TestShellMenuMerge() {
    Check(ipc::IsBuiltinContextVerb(L"Open", false), L"shellmenu: open filtered");
    Check(ipc::IsBuiltinContextVerb(L"copyaspath", false), L"shellmenu: copyaspath filtered");
    Check(ipc::IsBuiltinContextVerb(L"pintohome", false), L"shellmenu: pintohome filtered");
    Check(!ipc::IsBuiltinContextVerb(L"PinToStartScreen", false) &&
          !ipc::IsBuiltinContextVerb(L"pintostart", false),
          L"shellmenu: 固定到开始 kept (Pulse has no row for it)");
    Check(!ipc::IsBuiltinContextVerb(L"MergePdf", false), L"shellmenu: third-party verb kept");
    Check(!ipc::IsBuiltinContextVerb(L"refresh", false), L"shellmenu: refresh only filtered on background");
    Check(ipc::IsBuiltinContextVerb(L"refresh", true), L"shellmenu: background refresh filtered");
    // 0ba8422: Explorer's 打开方式 flyout is kept whole and replaces the static
    // row (it used to be dropped); 发送到 is never taken for it.
    Check(ipc::IsOpenWithSubmenuVerb(L"OpenAs"), L"shellmenu: 打开方式 flyout recognised (replaces the static row)");
    Check(!ipc::IsOpenWithSubmenuVerb(L"sendto"), L"shellmenu: 发送到 submenu is not the 打开方式 flyout");

    Check(ipc::CleanMenuText(L"打开方式(&H)...\tCtrl+O") == L"打开方式(H)...",
          L"shellmenu: text loses & mnemonic and \\t shortcut");
    Check(ipc::CleanMenuText(L"A && B") == L"A & B", L"shellmenu: && stays a literal ampersand");

    // AppendShellSection: dedupe against built-ins + across entries, flat rows.
    auto items = BuildItemMenu(false, L"");
    const size_t base_count = items.size();
    std::vector<ShellMenuEntry> entries{
        { CmdShellStaticBase + 0, L"打印", true },
        { CmdShellStaticBase + 1, L"属性", true },       // dup of built-in row
        { CmdShellComBase + 5, L"合并 PDF", true },
        { CmdShellComBase + 6, L"打印", true },          // dup of static entry
        { CmdShellComBase + 7, L"", true },              // empty text dropped
    };
    AppendShellSection(items, entries);
    Check(items.size() == base_count + 2, L"shellmenu: section dedupes 属性/打印 and empty rows");
    Check(items[base_count - 1].separator_after, L"shellmenu: separator before Explorer section");
    Check(items[base_count].command == CmdShellStaticBase + 0 &&
          items[base_count + 1].command == CmdShellComBase + 5,
          L"shellmenu: statics precede COM extras at the bottom");
    for (size_t i = base_count; i < items.size(); ++i)
        Check(items[i].quick_swatches.empty(), L"shellmenu: Explorer rows stay flat");

    // Software-owned submenu: header + children survive as a one-level flyout
    // (command 0 header row carries the children; never flattened).
    ShellMenuEntry group;
    group.text = L"Bandizip";
    group.children = { { CmdShellComBase + 20, L"压缩为 zip", true },
                       { CmdShellComBase + 21, L"压缩为 7z", true } };
    auto fly = BuildItemMenu(false, L"");
    const size_t fly_base = fly.size();
    AppendShellSection(fly, { group });
    Check(fly.size() == fly_base + 1 && fly.back().command == CmdNone &&
          fly.back().children.size() == 2 &&
          fly.back().children[0].command == CmdShellComBase + 20 &&
          fly.back().children[1].text == L"压缩为 7z",
          L"shellmenu: 软件子菜单保留为一级飞出");
    ui::FluentMenuModel fly_model;
    fly_model.SetItems(fly);
    Check(fly_model.At((int)fly_base) &&
          fly_model.At((int)fly_base)->children.size() == 2 &&
          fly_model.At((int)fly_base)->shortcut.empty(),
          L"shellmenu: 飞出行带 children、不占用 shortcut 列");

    ui::FluentMenuModel patch_model;
    ui::FluentMenuItem patch_a;
    patch_a.command = CmdShellComBase + 1;
    patch_a.text = L"合并 PDF";
    ui::FluentMenuItem patch_b;
    patch_b.command = CmdShellComBase + 2;
    patch_b.text = L"打印";
    patch_model.SetItems({ patch_a, patch_b });
    patch_a.command = CmdShellComBase + 9;
    patch_b.command = CmdShellComBase + 8;
    Check(patch_model.PatchCommands({ patch_a, patch_b }) &&
          patch_model.At(0) && patch_model.At(0)->command == CmdShellComBase + 9 &&
          patch_model.At(1) && patch_model.At(1)->command == CmdShellComBase + 8,
          L"shellmenu: display-equal COM ids patch in place");
    Check(!patch_model.PatchCommands({ patch_a }),
          L"shellmenu: refuse to shrink an open menu structure");
    ShellMenuEntry empty_group;
    empty_group.text = L"空分组";
    auto no_fly = BuildItemMenu(false, L"");
    const size_t no_fly_base = no_fly.size();
    AppendShellSection(no_fly, { empty_group });
    Check(no_fly.size() == no_fly_base, L"shellmenu: 无子项的分组行被丢弃");

    // No cap: a long menu keeps every Explorer row, flat (hiding is the
    // user's call in 管理右键项).
    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 60; ++i)
        many.push_back({ CmdShellComBase + 100 + i, L"动词 " + std::to_wstring(i), true });
    auto long_menu = BuildItemMenu(false, L"");
    const size_t long_base = long_menu.size();
    AppendShellSection(long_menu, many);
    Check(long_menu.size() == long_base + 60, L"shellmenu: a long section keeps every row");

    // DedupeStaticVerbs mirrors the same rules for the registry side.
    std::vector<StaticVerb> verbs{
        { L"print", L"打印", L"" },
        { L"edit", L"编辑", L"" },
        { L"edit2", L"编辑", L"" },   // dup display
        { L"openas", L"打开方式…", L"" },
    };
    auto deduped = DedupeStaticVerbs(std::move(verbs), { L"编辑" }, 8);
    Check(deduped.size() == 2 && deduped[0].display == L"打印" &&
          deduped[1].display == L"打开方式…",
          L"shellmenu: static verbs dedupe against built-ins and each other");

    ipc::StaticVerbRegFlags cascade_ok;
    cascade_ok.has_subcommands = true;
    Check(ipc::KeepStaticVerb(cascade_ok), L"shellmenu: SubCommands verbs are kept");
    ipc::StaticVerbRegFlags handler_ok;
    handler_ok.has_explorer_command = true;
    Check(ipc::KeepStaticVerb(handler_ok), L"shellmenu: ExplorerCommandHandler verbs are kept");
    ipc::StaticVerbRegFlags ext_skip;
    ext_skip.has_command = true;
    ext_skip.extended = true;
    Check(!ipc::KeepStaticVerb(ext_skip), L"shellmenu: Extended static verbs stay hidden");
    ipc::StaticVerbRegFlags empty_skip;
    Check(!ipc::KeepStaticVerb(empty_skip), L"shellmenu: verbs without a launch path are dropped");

    ipc::CtxFlyoutChild nested;
    nested.nested = true;
    nested.text = L"分组";
    ipc::CtxFlyoutChild leaf;
    leaf.id = 42;
    leaf.verb = L"share_phone";
    leaf.text = L"手机";
    nested.nested_leaves.push_back(leaf);
    ipc::CtxFlyoutChild builtin;
    builtin.verb = L"open";
    builtin.text = L"打开";
    std::vector<ipc::CtxFlyoutChild> leaves;
    ipc::FlattenFlyoutChildren({ nested, builtin }, false, leaves);
    Check(leaves.size() == 1 && leaves[0].id == 42 && leaves[0].text == L"手机" &&
          !leaves[0].nested,
          L"shellmenu: nested flyout children flatten to leaves");
    Check(ipc::KeepFlyoutParentWithoutLeaves(100, 50, 200),
          L"shellmenu: parent with a live id is kept when the flyout is empty");
    Check(!ipc::KeepFlyoutParentWithoutLeaves(0, 50, 200),
          L"shellmenu: parent id 0 is not kept as a clickable row");
    Check(!ipc::KeepFlyoutParentWithoutLeaves(10, 50, 200),
          L"shellmenu: parent ids outside the handler range are dropped");
}

void TestContextMenuPrefs() {
    using ipc::ClassifyExplorerItem;
    using ipc::CtxMenuCategory;
    Check(ClassifyExplorerItem(L"sendto", L"发送到", true) == CtxMenuCategory::Share,
          L"prefs: 发送到 is share");
    Check(ClassifyExplorerItem(L"wallpaper", L"设置为桌面背景", false) == CtxMenuCategory::Wallpaper,
          L"prefs: wallpaper classified");
    Check(ClassifyExplorerItem(L"rotate90", L"向右旋转", false) == CtxMenuCategory::Rotate,
          L"prefs: rotate classified");
    Check(ClassifyExplorerItem(L"link", L"创建快捷方式", false) == CtxMenuCategory::Shortcut,
          L"prefs: shortcut classified");
    Check(ClassifyExplorerItem(L"print", L"打印", false) == CtxMenuCategory::Print,
          L"prefs: print classified");
    Check(ClassifyExplorerItem(L"__openwith", L"用 记事本 打开", false) == CtxMenuCategory::OpenWith,
          L"prefs: 用 X 打开 classified");
    Check(ClassifyExplorerItem(L"", L"合并 PDF", false) == CtxMenuCategory::Software,
          L"prefs: type verb stays software");
    Check(ClassifyExplorerItem(L"", L"Bandizip", true) == CtxMenuCategory::Software,
          L"prefs: vendor flyout stays software");
    Check(ClassifyExplorerItem(L"", L"泛泰快传", true) == CtxMenuCategory::Share,
          L"prefs: 泛泰快传 flyout is share");
    Check(ClassifyExplorerItem(L"", L"泛泰快传", false) == CtxMenuCategory::Share,
          L"prefs: 泛泰快传 row is share");
    Check(ipc::IsCompressVendorFlyout(L"Bandizip") &&
          ipc::IsCompressTopLevel(L"压缩为「photo.zip」"),
          L"prefs: compress flyout vs top-level zip row");

    ContextMenuPrefs prefs;
    prefs.persist = false;
    // The fixtures below run with 发送到 and the system verbs switched off;
    // the defaults (Explorer parity, everything on) are asserted separately.
    prefs.SetGroupEnabled(ipc::CtxMenuGroup::Share, false);
    prefs.SetGroupEnabled(ipc::CtxMenuGroup::System, false);

    ShellMenuEntry sendto;
    sendto.text = L"发送到";
    sendto.verb = L"sendto";
    sendto.from_com = true;
    sendto.children = { { CmdShellComBase + 1, L"文档", true } };

    ShellMenuEntry bandizip;
    bandizip.text = L"Bandizip";
    bandizip.from_com = true;
    bandizip.children = { { CmdShellComBase + 20, L"压缩为 zip", true } };

    std::vector<ShellMenuEntry> raw{
        { CmdShellStaticBase + 0, L"打印", true, {}, L"print", false },
        { CmdShellStaticBase + 1, L"用 记事本 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 2, L"用 画图 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 3, L"用 Word 打开", true, {}, L"__openwith", false },
        { CmdShellStaticBase + 4, L"打开方式…", true, {}, L"openas", false },
        { CmdShellComBase + 5, L"设置为桌面背景", true, {}, L"wallpaper", true },
        { CmdShellComBase + 6, L"向右旋转", true, {}, L"rotate90", true },
        { CmdShellComBase + 7, L"压缩为「photo.zip」", true, {}, L"", true },
        { CmdShellComBase + 8, L"合并 PDF", true, {}, L"", true },
        sendto,
        bandizip,
    };

    auto filtered = ApplyExplorerPrefs(prefs, raw);
    auto has = [&](std::wstring_view text) {
        for (const auto& e : filtered)
            if (e.text == text) return true;
        return false;
    };
    Check(!has(L"发送到") && !has(L"设置为桌面背景") && !has(L"向右旋转"),
          L"prefs: switched-off groups hide share / wallpaper / rotate");
    Check(has(L"Bandizip") && has(L"合并 PDF") && has(L"打印"),
          L"prefs: other groups keep vendor flyout, type verb, print");
    {
        // Pulse's menu must never be shorter than Explorer's: by default every
        // group Explorer's classic menu shows is on.
        ContextMenuPrefs defaults;
        defaults.persist = false;
        const auto native = ApplyExplorerPrefs(defaults, raw);
        auto shown = [&](std::wstring_view text) {
            for (const auto& e : native)
                if (e.text == text) return true;
            return false;
        };
        Check(shown(L"发送到") && shown(L"设置为桌面背景") && shown(L"向右旋转") &&
              shown(L"Bandizip") && shown(L"打印"),
              L"prefs: defaults show every Explorer group (发送到 / wallpaper / rotate)");
        ContextMenuPrefs reset;
        reset.persist = false;
        reset.SetGroupEnabled(ipc::CtxMenuGroup::Share, false);
        reset.ResetToDefaults();
        Check(reset.share && reset.GroupEnabled(ipc::CtxMenuGroup::System) && !reset.open_with_com,
              L"prefs: restore defaults turns 发送到 and the system group back on");
        // Files written before version 3 stored the old off defaults.
        ContextMenuPrefs old_file;
        old_file.persist = false;
        Check(old_file.FromJson(L"{\"version\":2,\"categories\":{\"share\":false,\"rotate\":false,"
                                L"\"system_extra\":false,\"print\":false}}") &&
              old_file.share && old_file.rotate && old_file.system_extra && !old_file.print &&
              old_file.migrated,
              L"prefs: version 2 files turn 发送到 / system groups on once, keep other switches");
        ContextMenuPrefs current_file;
        current_file.persist = false;
        Check(current_file.FromJson(L"{\"version\":3,\"categories\":{\"share\":false,\"rotate\":false}}") &&
              !current_file.share && !current_file.rotate && current_file.wallpaper &&
              !current_file.migrated,
              L"prefs: version 3 files keep a group the user switched off");
    }
    ContextMenuPrefs with_share;
    with_share.persist = false;
    with_share.SetGroupEnabled(ipc::CtxMenuGroup::Share, true);
    const auto sharing = ApplyExplorerPrefs(with_share, raw);
    bool share_shown = false;
    for (const auto& e : sharing)
        if (e.text == L"发送到") share_shown = true;
    Check(share_shown, L"prefs: switching the share group on shows 发送到");
    Check(!has(L"压缩为「photo.zip」"),
          L"prefs: compress top-level collapsed when flyout exists");
    bool in_bandizip = false;
    for (const auto& e : filtered)
        if (e.text == L"Bandizip")
            for (const auto& c : e.children)
                if (c.text == L"压缩为「photo.zip」") in_bandizip = true;
    Check(in_bandizip, L"prefs: named compress row folds into vendor flyout");

    std::vector<ShellMenuEntry> named_only{
        { CmdShellComBase + 30, L"压缩为 \"a.zip\"", true, {}, L"", true },
        { CmdShellComBase + 31, L"压缩为 \"b.7z\"", true, {}, L"", true },
        { CmdShellComBase + 32, L"合并 PDF", true, {}, L"", true },
    };
    auto grouped = ApplyExplorerPrefs(prefs, named_only);
    Check(grouped.size() == 2 && grouped[0].text == L"压缩" &&
          grouped[0].children.size() == 2 && grouped[1].text == L"合并 PDF",
          L"prefs: named compress rows synthesize one 压缩 flyout");

    Check(ipc::IsDisabledHandler(L"{11111111-1111-1111-1111-111111111111}",
                                 { L"{11111111-1111-1111-1111-111111111111}" }),
          L"prefs: disabled handler CLSID matches");

    ContextMenuPrefs compress_prefs;
    compress_prefs.persist = false;
    compress_prefs.RecordSeen(ipc::CatalogKey(L"压缩为「old.zip」", false), L"压缩为「old.zip」", false,
                     CtxMenuCategory::Software, true);
    compress_prefs.RecordSeen(ipc::CatalogKey(L"压缩为「new.7z」", false), L"压缩为「new.7z」", false,
                     CtxMenuCategory::Software, true);
    ContextMenuPrefs migrated;
    migrated.persist = false;
    Check(migrated.FromJson(compress_prefs.ToJson()), L"prefs: json loads for compress coalesce");
    int compress_rows = 0;
    bool has_canonical = false;
    for (const auto& item : migrated.seen) {
        if (ipc::IsCompressTopLevel(item.text) && item.key != ipc::CompressCatalogKey())
            ++compress_rows;
        if (item.key == ipc::CompressCatalogKey()) has_canonical = true;
    }
    Check(compress_rows == 0 && has_canonical,
          L"prefs: filename compress seen rows coalesce to 压缩为…");

    const std::wstring adobe_clsid = L"{22222222-2222-2222-2222-222222222222}";
    ContextMenuPrefs handler_prefs;
    handler_prefs.persist = false;
    ShellMenuEntry adobe;
    adobe.command = CmdShellComBase + 40;
    adobe.text = L"Adobe PDF";
    adobe.from_com = true;
    adobe.clsid = adobe_clsid;
    adobe.handler = L"Adobe PDF";
    handler_prefs.SetItemEnabled(ipc::HandlerCatalogKey(adobe_clsid), false);
    auto without_adobe = ApplyExplorerPrefs(handler_prefs, { adobe, named_only[2] });
    Check(without_adobe.size() == 1 && without_adobe[0].text == L"合并 PDF",
          L"prefs: disabled handler CLSID is omitted from the fusion zone");
    Check(!handler_prefs.HandlerEnabled(adobe_clsid),
          L"prefs: explicit handler disable skips CoCreate");

    ContextMenuPrefs slow_handler;
    slow_handler.persist = false;
    const std::wstring slow_key = ipc::HandlerCatalogKey(adobe_clsid);
    slow_handler.RecordComTiming(slow_key, 1000);
    slow_handler.RecordComTiming(slow_key, 1000);
    Check(!slow_handler.ComDisabled(slow_key), L"prefs: two handler timeouts do not disable");
    slow_handler.RecordComTiming(slow_key, 1000);
    Check(slow_handler.ComDisabled(slow_key) && !slow_handler.HandlerEnabled(adobe_clsid),
          L"prefs: three handler timeouts disable the CLSID");
    auto disabled_clsids = slow_handler.DisabledHandlerClsids();
    bool listed = false;
    for (const auto& c : disabled_clsids)
        if (ipc::ToLowerVerb(c) == ipc::ToLowerVerb(adobe_clsid)) listed = true;
    Check(listed, L"prefs: disabled handler CLSIDs are sent to the host");
    slow_handler.SetItemEnabled(slow_key, true);
    Check(slow_handler.HandlerEnabled(adobe_clsid),
          L"prefs: re-enabling a handler clears the timeout skip");
    Check(ipc::IsHandlerCatalogKey(slow_key) &&
          ipc::HandlerClsidFromKey(slow_key) == ipc::ToLowerVerb(adobe_clsid),
          L"prefs: handler catalog key round-trips the CLSID");

    // SendTo is exempt from the slow-extension auto-disable (#77): it wraps
    // the whole default menu, so it is structurally the last worker home.
    ContextMenuPrefs sendto_prefs;
    sendto_prefs.persist = false;
    const std::wstring sendto_key = ipc::SendToHandlerCatalogKey();
    for (int i = 0; i < 5; ++i)
        Check(!sendto_prefs.RecordComTiming(sendto_key, 1500),
              L"prefs: SendTo timeouts never register as slow hits (#77)");
    Check(sendto_prefs.slow_ext.find(sendto_key) == sendto_prefs.slow_ext.end() &&
              !sendto_prefs.ComDisabled(sendto_key) && !sendto_prefs.ComDeferred(sendto_key) &&
              sendto_prefs.DisabledHandlerClsids().empty(),
          L"prefs: SendTo stays enabled no matter how often it times out (#77)");
    sendto_prefs.slow_ext[sendto_key].disabled = true;
    sendto_prefs.slow_ext[sendto_key].timeout_hits = 3;
    ContextMenuPrefs sendto_loaded;
    sendto_loaded.persist = false;
    Check(sendto_loaded.FromJson(sendto_prefs.ToJson()) &&
              sendto_loaded.slow_ext.find(sendto_key) == sendto_loaded.slow_ext.end() &&
              sendto_loaded.HandlerEnabled(ipc::kSendToHandlerClsid),
          L"prefs: loading purges the persisted SendTo disable state (#77)");
    // The settings page must not show a slow-disabled handler as on (#77).
    ContextMenuPrefs row_prefs;
    row_prefs.persist = false;
    row_prefs.slow_ext[slow_key].disabled = true;
    Check(!row_prefs.RowEnabled(slow_key, CtxMenuCategory::Software, true) &&
              row_prefs.ItemEnabled(slow_key, CtxMenuCategory::Software, true),
          L"prefs: a slow-disabled handler row reports off despite no override (#77)");
    row_prefs.SetItemEnabled(slow_key, true);
    Check(row_prefs.RowEnabled(slow_key, CtxMenuCategory::Software, true),
          L"prefs: toggling a slow-disabled row back on clears the skip (#77)");

    Check(has(L"用 记事本 打开") && has(L"用 画图 打开") && !has(L"用 Word 打开") &&
          has(L"打开方式…"),
          L"prefs: static open-with MRU capped at 2 plus 打开方式");
    Check(filtered.size() >= 4 && filtered[0].text == L"Bandizip" &&
          filtered[1].text == L"合并 PDF",
          L"prefs: order is flyout then type verb");

    prefs.SetItemEnabled(ipc::CatalogKey(L"发送到", true), false);
    auto restored = ApplyExplorerPrefs(prefs, raw);
    bool sendto_back = false;
    for (const auto& e : restored)
        if (e.text == L"发送到") sendto_back = true;
    Check(!sendto_back,
          L"prefs: a per-item override hides 发送到 even while its group is on");

    prefs.RecordSeen(ipc::CatalogKey(L"Bandizip", true), L"Bandizip", true,
                     CtxMenuCategory::Software, true);
    const std::wstring json = prefs.ToJson();
    ContextMenuPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) &&
          !loaded.item_enabled[ipc::CatalogKey(L"发送到", true)] &&
          loaded.seen.size() == 1 && loaded.seen[0].from_com &&
          !loaded.share && loaded.print,
          L"prefs: JSON round-trip keeps override, seen, and defaults");

    Check(prefs.RecordComTiming(L".dwg", 800) && prefs.ComDeferred(L".dwg") == false, L"prefs: one slow COM hit does not defer");
    prefs.RecordComTiming(L".dwg", 800);
    prefs.RecordComTiming(L".dwg", 800);
    Check(prefs.ComDeferred(L".dwg") && !prefs.ComDisabled(L".dwg"),
          L"prefs: three 500ms+ COM hits defer the extension");
    prefs.RecordComTiming(L".cad", 1200);
    prefs.RecordComTiming(L".cad", 1200);
    prefs.RecordComTiming(L".cad", 1200);
    Check(prefs.ComDisabled(L".cad"), L"prefs: three 1000ms+ COM hits disable the extension");
    const std::wstring json_slow = prefs.ToJson();
    ContextMenuPrefs slow_loaded;
    slow_loaded.persist = false;
    Check(slow_loaded.FromJson(json_slow) && slow_loaded.ComDeferred(L".dwg") &&
              slow_loaded.ComDisabled(L".cad"),
          L"prefs: slow COM stats round-trip");

    loaded.ResetToDefaults();
    Check(loaded.seen.empty() && loaded.item_enabled.empty() && loaded.share,
          L"prefs: restore defaults clears seen and overrides");
    ContextMenuPrefs old_cap;
    old_cap.persist = false;
    Check(old_cap.FromJson(L"{\"version\":1,\"explorer_cap\":12}") &&
              old_cap.ToJson().find(L"explorer_cap") == std::wstring::npos,
          L"prefs: an old explorer_cap is read without error and not written back");

    // Pulse's own commands (#41, #44-⑩): hide, persist, filter, restore.
    {
        ContextMenuPrefs builtin;
        builtin.persist = false;
        builtin.SetBuiltinVisible(BuiltinMenuItem::PinWorkspace, false);
        builtin.SetBuiltinVisible(BuiltinMenuItem::PinNetwork, false);
        builtin.SetBuiltinVisible(BuiltinMenuItem::Tags, false);
        builtin.SetBuiltinVisible(BuiltinMenuItem::Undo, false);
        builtin.SetBuiltinVisible(BuiltinMenuItem::RowStar, false);
        ContextMenuPrefs reread;
        reread.persist = false;
        Check(reread.FromJson(builtin.ToJson()) && reread.builtin_hidden == builtin.builtin_hidden &&
                  !reread.BuiltinVisible(BuiltinMenuItem::Tags) &&
                  reread.BuiltinVisible(BuiltinMenuItem::CopyPath),
              L"prefs: hidden Pulse commands round-trip");
        Check(RowActionMask(reread.builtin_hidden) == (kRowActionNewTab | kRowActionMore) &&
                  RowActionMask(0) == kRowActionsAll,
              L"prefs: hiding the row star keeps the new-tab and more buttons");

        auto menu = BuildItemMenu(true, L"撤销移动 a.txt", true);
        ApplyBuiltinMenuPrefs(menu, builtin);
        const std::vector<int> kept_ids{ CmdOpen, CmdNone, CmdOpenInNewTab, CmdCopyPath,
                                         CmdOpenTerminal, CmdProperties };
        bool kept_ok = menu.size() == kept_ids.size();
        for (size_t i = 0; kept_ok && i < menu.size(); ++i) kept_ok = menu[i].command == kept_ids[i];
        Check(kept_ok, L"menu: hidden Pulse commands are dropped, the rest keep their order");
        Check(!menu.empty() && menu[1].separator_after && !menu.back().separator_after,
              L"menu: dropping the tail leaves no trailing separator");

        ContextMenuPrefs select_off;
        select_off.persist = false;
        select_off.SetBuiltinVisible(BuiltinMenuItem::SelectCommands, false);
        auto bg = BuildBackgroundMenu(true, true, L"撤销移动 a.txt");
        ApplyBuiltinMenuPrefs(bg, select_off);
        size_t copy_at = bg.size(), terminal_at = bg.size();
        bool select_gone = true;
        for (size_t i = 0; i < bg.size(); ++i) {
            if (bg[i].command == CmdCopyPath) copy_at = i;
            if (bg[i].command == CmdOpenTerminal) terminal_at = i;
            if (bg[i].command == CmdSelectAll || bg[i].command == CmdInvertSelection ||
                bg[i].command == CmdSelectWildcard) select_gone = false;
        }
        Check(select_gone && copy_at + 1 == terminal_at && terminal_at < bg.size() &&
                  bg[copy_at].separator_after && bg.back().command == CmdUndo,
              L"menu: a hidden middle group folds into the separator above it");

        auto untouched = BuildItemMenu(false, L"");
        const size_t untouched_size = untouched.size();
        ApplyBuiltinMenuPrefs(untouched, ContextMenuPrefs{});
        Check(untouched.size() == untouched_size, L"menu: default prefs keep every Pulse command");
        Check(BuiltinItemForCommand(CmdOpen) == BuiltinMenuItem::Count &&
                  BuiltinItemForCommand(CmdProperties) == BuiltinMenuItem::Count &&
                  BuiltinItemForCommand(CmdUnpinQuickAccess) == BuiltinMenuItem::QuickAccess,
              L"menu: 打开 and 属性 can never be hidden");

        // Drag-to-reorder on the settings preview: per-surface order, saved
        // by key, applied by moving rows among their slots.
        auto copy_first = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        copy_first.erase(std::find(copy_first.begin(), copy_first.end(), BuiltinMenuItem::CopyPath));
        copy_first.insert(copy_first.begin(), BuiltinMenuItem::CopyPath);
        ContextMenuPrefs ordered;
        ordered.persist = false;
        ordered.SetBuiltinOrder(BuiltinMenuSurface::Item, copy_first);
        ContextMenuPrefs ordered_read;
        ordered_read.persist = false;
        Check(ordered_read.FromJson(ordered.ToJson()) &&
                  ordered_read.BuiltinOrder(BuiltinMenuSurface::Item) == copy_first &&
                  ordered_read.BuiltinOrderCustom(BuiltinMenuSurface::Item) &&
                  !ordered_read.BuiltinOrderCustom(BuiltinMenuSurface::Background),
              L"prefs: a moved Pulse menu order round-trips per surface");
        ContextMenuPrefs default_order;
        default_order.persist = false;
        default_order.SetBuiltinOrder(BuiltinMenuSurface::Item, BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item));
        Check(default_order.builtin_order[0].empty() && default_order.ToJson().find(L"open_new_tab,") == std::wstring::npos,
              L"prefs: the table order is stored as no order");
        const auto cleaned = NormalizeBuiltinMenuOrder(BuiltinMenuSurface::Background,
            { BuiltinMenuItem::Undo, BuiltinMenuItem::Undo, BuiltinMenuItem::RowStar });
        Check(cleaned.size() == BuiltinMenuDefaultOrder(BuiltinMenuSurface::Background).size() &&
                  cleaned.front() == BuiltinMenuItem::Undo &&
                  std::count(cleaned.begin(), cleaned.end(), BuiltinMenuItem::Undo) == 1 &&
                  std::find(cleaned.begin(), cleaned.end(), BuiltinMenuItem::RowStar) == cleaned.end(),
              L"prefs: a saved order drops repeats and foreign rows and refills missing ones");

        auto moved = BuildItemMenu(true, L"", true);
        ApplyBuiltinMenuPrefs(moved, ordered, BuiltinMenuSurface::Item);
        const std::vector<int> moved_ids{ CmdOpen, CmdNone, CmdCopyPath, CmdOpenInNewTab, CmdOpenTerminal,
                                          CmdProperties, CmdPinWorkspace, CmdPinNetwork, CmdTags, CmdUndo };
        bool moved_ok = moved.size() == moved_ids.size();
        for (size_t i = 0; moved_ok && i < moved.size(); ++i) moved_ok = moved[i].command == moved_ids[i];
        Check(moved_ok && moved[5].separator_after && !moved[2].separator_after,
              L"menu: a moved Pulse command takes its new place, groups keep their separators");

        auto undo_first = BuiltinMenuDefaultOrder(BuiltinMenuSurface::Item);
        undo_first.erase(std::find(undo_first.begin(), undo_first.end(), BuiltinMenuItem::Undo));
        undo_first.insert(undo_first.begin(), BuiltinMenuItem::Undo);
        ContextMenuPrefs undo_prefs;
        undo_prefs.persist = false;
        undo_prefs.SetBuiltinOrder(BuiltinMenuSurface::Item, undo_first);
        auto undo_menu = BuildItemMenu(true, L"", true);
        ApplyBuiltinMenuPrefs(undo_menu, undo_prefs, BuiltinMenuSurface::Item);
        Check(undo_menu.size() == moved_ids.size() && undo_menu[2].command == CmdUndo &&
                  !undo_menu[2].separator_after && undo_menu.back().command == CmdTags &&
                  !undo_menu.back().separator_after,
              L"menu: separators stay with their slot when the last row moves up");
        auto plain = BuildItemMenu(true, L"", true);
        ApplyBuiltinMenuPrefs(plain, undo_prefs);
        Check(plain.size() == moved_ids.size() && plain.back().command == CmdUndo,
              L"menu: without a surface the order is left alone");

        const auto visible = BuiltinMenuVisibleRows(BuiltinMenuSurface::Item, 0, undo_first);
        Check(visible.size() > 3 && visible[2].row.item == BuiltinMenuItem::Undo &&
                  !visible.back().separator_after,
              L"preview: rows follow the saved order");
        Check(RowActionMask(0, { BuiltinMenuItem::RowMore, BuiltinMenuItem::RowStar, BuiltinMenuItem::RowNewTab }) ==
                  (kRowActionsAll | ((2u | (0u << 2) | (1u << 4)) << kRowActionOrderShift)) &&
                  RowActionMask(0, BuiltinMenuDefaultOrder(BuiltinMenuSurface::RowButtons)) == kRowActionsAll,
              L"prefs: the row buttons carry their order to the renderer");

        builtin.ResetToDefaults();
        Check(builtin.builtin_hidden == 0, L"prefs: restore defaults shows every Pulse command again");
        ContextMenuPrefs legacy;
        legacy.persist = false;
        Check(legacy.FromJson(L"{\"version\":1,\"categories\":{\"software\":true}}") &&
                  legacy.builtin_hidden == 0,
              L"prefs: files without pulse_items show every Pulse command");
    }

    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 20; ++i)
        many.push_back({ CmdShellComBase + 200 + i, L"动词 " + std::to_wstring(i), true, {}, L"", true });
    auto uncapped = ApplyExplorerPrefs(prefs, many);
    Check(uncapped.size() == many.size(), L"prefs: the Explorer section has no row cap");

    // Rows seeded from the on-disk cache still need one live pass: that cache is
    // written at install time, and a build without resources shipped an empty
    // 打开方式 row that made the menu drop the whole entry.
    ContextMenuController seeded;
    StaticVerb empty_row;
    empty_row.verb = L"openas";
    seeded.MergeStaticCache({ { L".xlsx", { empty_row } } });
    Check(seeded.RequestStaticPrefetch(L".xlsx"),
          L"prefs: a seeded extension still re-enumerates from the registry");
    seeded.CompleteStaticVerbs(L".xlsx", { empty_row }, seeded.cache_generation());
    Check(!seeded.RequestStaticPrefetch(L".xlsx"),
          L"prefs: a live pass clears the seeded marker");

    // Catalog keys are normalized: one switch then covers every spelling the
    // shell uses for the same verb, and an older catalog migrates onto them.
    Check(ipc::CatalogKey(L"用 PDF-XChange Editor 打开", false) ==
          ipc::CatalogKey(L"用PDF-XChange Editor打开", false),
          L"prefs: spacing differences collapse into one catalog key");
    ContextMenuPrefs catalog;
    catalog.persist = false;
    Check(catalog.RecordSeen(ipc::CatalogKey(L"新建(N)", false), L"新建(N)", false,
                             CtxMenuCategory::Software, false) &&
          !catalog.RecordSeen(ipc::CatalogKey(L"新建(W)", false), L"新建(W)", false,
                              CtxMenuCategory::Software, false) &&
          catalog.seen.size() == 1,
          L"prefs: 新建(N) and 新建(W) share one catalog row");
    const std::wstring legacy_catalog =
        L"{\"version\":1,\"items\":{\"v:新建(w)\":{\"enabled\":false}},"
        L"\"seen\":[{\"key\":\"v:新建(n)\",\"text\":\"新建(N)\",\"kind\":\"verb\","
        L"\"category\":\"software\",\"source\":\"static\"}]}";
    ContextMenuPrefs migrated_keys;
    migrated_keys.persist = false;
    Check(migrated_keys.FromJson(legacy_catalog) && migrated_keys.seen.size() == 1 &&
          migrated_keys.seen[0].key == ipc::CatalogKey(L"新建(W)", false) &&
          migrated_keys.item_enabled[ipc::CatalogKey(L"新建(N)", false)] == false,
          L"prefs: an older catalog migrates keys and keeps its overrides");
}

// Diagnostic entry (PULSE_SELFTEST_CASE=context-verbs): dumps the real registry
// static verbs for a few extensions plus what the UI-side pref filter keeps, so
// "why is this verb missing from the right-click menu" is answerable without
// guessing. Read-only: no menu opens and nothing is invoked.
void DumpContextVerbs() {
    wchar_t line[600]{};
    // The machine-level cache seeded at install time is the first suspect when a
    // verb shows up here but not in the live menu.
    std::unordered_map<std::wstring, std::vector<StaticVerb>> machine;
    if (LoadMachineStaticVerbCache(machine)) {
        swprintf_s(line, L"[DUMP] machine cache: %zu extension(s)\n", machine.size());
        LogLine(line);
        const auto found = machine.find(L".xlsx");
        if (found == machine.end()) {
            LogLine(L"[DUMP] machine cache: no .xlsx entry\n");
        } else {
            swprintf_s(line, L"[DUMP] machine cache .xlsx: %zu verb(s)\n",
                       found->second.size());
            LogLine(line);
            for (const auto& verb : found->second) {
                swprintf_s(line, L"[DUMP]   cached text=\"%ls\" verb=\"%ls\"\n",
                           verb.display.c_str(), verb.verb.c_str());
                LogLine(line);
            }
        }
    } else {
        LogLine(L"[DUMP] machine cache: missing or unreadable\n");
    }
    const std::wstring extensions[] = { L".xlsx", L".txt", L".jpg", L".png" };
    for (const auto& ext : extensions) {
        const auto verbs = EnumerateStaticVerbs(ext);
        swprintf_s(line, L"[DUMP] %ls: %zu static verb(s)\n", ext.c_str(), verbs.size());
        LogLine(line);
        for (const auto& verb : verbs) {
            swprintf_s(line,
                L"[DUMP]   text=\"%ls\" verb=\"%ls\" cmd=%d app=%d children=%zu\n",
                verb.display.c_str(), verb.verb.c_str(), verb.command.empty() ? 0 : 1,
                verb.app_path.empty() ? 0 : 1, verb.children.size());
            LogLine(line);
        }
        ContextMenuPrefs prefs;
        prefs.persist = false;
        std::vector<ShellMenuEntry> entries;
        for (size_t i = 0; i < verbs.size(); ++i) {
            ShellMenuEntry entry;
            entry.text = verbs[i].display;
            entry.verb = verbs[i].verb;
            entry.command = CmdShellStaticBase + static_cast<int>(i) * ipc::kStaticVerbStride;
            entries.push_back(std::move(entry));
        }
        const auto kept = ApplyExplorerPrefs(prefs, entries);
        swprintf_s(line, L"[DUMP] %ls: %zu kept after Explorer prefs\n", ext.c_str(),
                   kept.size());
        LogLine(line);
        for (const auto& entry : kept) {
            swprintf_s(line, L"[DUMP]   kept text=\"%ls\" verb=\"%ls\"\n",
                       entry.text.c_str(), entry.verb.c_str());
            LogLine(line);
        }
    }
}

void TestAppPrefsAndSettingsPath() {
    std::wstring kind, rest;
    Check(ParsePulsePath(L"pulse:settings:general", &kind, &rest) &&
          kind == L"settings" && rest == L"general",
          L"settings: parse general path");
    Check(ParsePulsePath(L"pulse:settings:context", &kind, &rest) &&
          kind == L"settings" && rest == L"context",
          L"settings: parse context path");
    Check(ParsePulsePath(L"pulse:settings:index", &kind, &rest) &&
          kind == L"settings" && rest == L"index",
          L"settings: parse index path");
    Check(MakeSettingsPath() == L"pulse:settings:general",
          L"settings: default page is general");
    Check(MakeSettingsPath(L"context") == L"pulse:settings:context",
          L"settings: context page path");
    Check(MakeSettingsPath(L"index") == L"pulse:settings:index",
          L"settings: index page path");
    Check(fs::IsVirtualPath(L"pulse:settings:general"),
          L"settings: pulse:settings is virtual");

    AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.FromJson(L"{\"launch_on_startup\":true,\"keep_running_on_close\":true}") &&
          prefs.launch_on_startup && prefs.keep_running_on_close &&
          !prefs.open_folders_in_pulse,
          L"appprefs: parse json");
    Check(prefs.ApplyLaunchOnStartup(false) && !prefs.launch_on_startup,
          L"appprefs: persist=false toggle does not write Run key");
    Check(prefs.ApplyFolderOpen(true) && prefs.open_folders_in_pulse,
          L"appprefs: persist=false folder-open toggle does not write HKCU");
    const std::wstring json = prefs.ToJson();
    AppPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) && !loaded.launch_on_startup && loaded.keep_running_on_close &&
          loaded.open_folders_in_pulse && loaded.language == L"system" &&
          !loaded.show_status_performance,
          L"appprefs: json round-trip");
    {
        AppPrefs icon;
        icon.persist = false;
        Check(icon.FromJson(L"{\"keep_running_on_close\":true}") && icon.notify_icon_mode == 0,
              L"tray icon: older preferences keep the icon (#57)");
        icon.notify_icon_mode = 2;
        AppPrefs icon_loaded;
        icon_loaded.persist = false;
        Check(icon_loaded.FromJson(icon.ToJson()) && icon_loaded.notify_icon_mode == 2,
              L"tray icon: the choice survives a reload");
        Check(icon_loaded.FromJson(L"{\"notify_icon_mode\":7}") && icon_loaded.notify_icon_mode == 0,
              L"tray icon: an unknown value falls back to always");
    }
    Check(json.find(L"\"launch_on_startup\":false") != std::wstring::npos &&
          json.find(L"\"keep_running_on_close\":true") != std::wstring::npos &&
          json.find(L"\"open_folders_in_pulse\":true") != std::wstring::npos &&
          json.find(L"\"language\":\"system\"") != std::wstring::npos &&
          json.find(L"\"show_status_performance\":false") != std::wstring::npos,
          L"appprefs: json contains both flags");
    Check(prefs.FromJson(L"{\"show_status_performance\":true}") &&
          prefs.show_status_performance,
          L"appprefs: parse show_status_performance");
    Check(FolderOpenCommandLine(L"C:\\Pulse\\pulse.exe") ==
              L"\"C:\\Pulse\\pulse.exe\" \"%1\"",
          L"appprefs: folder-open command quotes exe and %1");
    Check(FolderOpenCommandIsOurs(L"\"C:\\Pulse\\pulse.exe\" \"%1\"",
                                  L"C:\\Pulse\\pulse.exe"),
          L"appprefs: folder-open command matches our exe");
    Check(!FolderOpenCommandIsOurs(L"\"C:\\Windows\\explorer.exe\" \"%1\"",
                                   L"C:\\Pulse\\pulse.exe"),
          L"appprefs: folder-open command ignores explorer");
    AppPrefs density;
    density.persist = false;
    density.row_height = 40;
    AppPrefs density_loaded;
    density_loaded.persist = false;
    Check(density_loaded.FromJson(density.ToJson()) && density_loaded.row_height == 40,
          L"appprefs: row_height round-trip");
    Check(density_loaded.FromJson(L"{\"row_height\":99}") && density_loaded.row_height == 34,
          L"appprefs: row_height out of range falls back to default");

    bool effect_ids_ok = true;
    for (int i = 0; i < ui::kWindowEffectCount; ++i) {
        const auto e = static_cast<ui::WindowEffect>(i);
        if (ui::WindowEffectFromId(ui::WindowEffectId(e)) != e || !ui::WindowEffectLabel(e)[0])
            effect_ids_ok = false;
    }
    Check(effect_ids_ok, L"appprefs: window effect id/label round-trip");
    AppPrefs effect_prefs;
    effect_prefs.persist = false;
    Check(effect_prefs.FromJson(
              L"{\"window_effect\":\"none\",\"background_image\":\"C:\\\\wall.jpg\"}") &&
          effect_prefs.window_effect == L"none" &&
          effect_prefs.background_image == L"C:\\wall.jpg",
          L"appprefs: parse none effect + background image");

    uint32_t accent = 0;
    Check(!ParseAccentRgb(L"", accent), L"appprefs: empty accent follows Windows");
    Check(ParseAccentRgb(L"2FDFF1", accent) && accent == 0x2FDFF1u,
          L"appprefs: parse accent 2FDFF1");
    Check(ParseAccentRgb(L"#2fdff1", accent) && accent == 0x2FDFF1u,
          L"appprefs: parse accent #2fdff1");
    Check(!ParseAccentRgb(L"xyz", accent), L"appprefs: reject bad accent hex");
    AppPrefs follow;
    follow.persist = false;
    Check(follow.FromJson(L"{}") && follow.accent_rgb.empty(),
          L"appprefs: missing accent_rgb uses theme default");
    AppPrefs custom;
    custom.persist = false;
    Check(custom.FromJson(L"{\"accent_rgb\":\"2FDFF1\"}") &&
          custom.accent_rgb == L"2FDFF1",
          L"appprefs: accent_rgb from json");
    AppPrefs custom_round;
    custom_round.persist = false;
    Check(custom_round.FromJson(custom.ToJson()) && custom_round.accent_rgb == L"2FDFF1" &&
          custom.ToJson().find(L"\"accent_rgb\":\"2FDFF1\"") != std::wstring::npos,
          L"appprefs: accent_rgb round-trip");
    AppPrefs bad_accent;
    bad_accent.persist = false;
    Check(bad_accent.FromJson(L"{\"accent_rgb\":\"gggggg\"}") && bad_accent.accent_rgb.empty(),
          L"appprefs: invalid accent_rgb falls back to theme default");
}

void TestBloomAccentGeometry() {
    Check(ui::kBloomDotCount == 19, L"bloom: 19 dots");
    Check(std::fabs(ui::BloomDotHue(1, 0, 6) - 90.0f) < 0.01f,
          L"bloom: ring1 index0 hue 90");
    Check(std::fabs(ui::BloomDotHue(2, 0, 12) - 90.0f) < 0.01f,
          L"bloom: ring2 index0 hue 90");
    Check(std::fabs(ui::BloomDotHue(1, 3, 6) - 270.0f) < 0.01f,
          L"bloom: ring1 index3 hue 270");
    Check(std::fabs(ui::BloomDotHue(1, 1, 6) - 150.0f) < 0.01f,
          L"bloom: ring1 index1 hue 150");
    Check(ui::BloomDotRgb(0) == 0xFFFFFFu, L"bloom: center is white");
    Check(ui::BloomDotAt(0).ring == 0 && ui::BloomDotAt(7).ring == 2,
          L"bloom: index 0 center, 7 first outer");
    const uint32_t pastel = ui::BloomDotRgb(1);
    const uint32_t sat = ui::BloomDotRgb(7);
    Check(pastel != sat && pastel != 0xFFFFFFu && sat != 0xFFFFFFu,
          L"bloom: pastel and saturated dots differ");
}

void TestTreeGuides() {
    // 树状引线: a / b(open) / b1, b2(open) / b21 / b3, then c; and a
    // placeholder row under an open folder counts as its child.
    ui::DetailsTreeRows t;
    const auto add = [&](uint16_t depth, bool open, ui::TreeRowKind kind = ui::TreeRowKind::Entry) {
        ui::DetailsTreeRow r;
        r.depth = depth; r.kind = kind; r.expandable = open; r.expanded = open;
        t.rows.push_back(r);
    };
    add(0, false); add(0, true); add(1, false); add(1, true); add(2, false); add(1, false); add(0, false);
    const ui::TreeGuides all = ui::BuildTreeGuides(t, 0, 6, -1);
    Check(all.rows.size() == 7 && all.rows[0].join < 0 && all.rows[1].join < 0 && all.rows[6].join < 0,
          L"tree guides: top-level rows join no line");
    Check(all.rows[2].join == 0 && !all.rows[2].last && all.rows[3].join == 0 && !all.rows[3].last &&
          all.rows[5].join == 0 && all.rows[5].last,
          L"tree guides: children tick into their folder's line, the last one ends it in an elbow");
    Check(all.rows[4].join == 1 && all.rows[4].last && all.rows[4].through == 1u,
          L"tree guides: a grandchild's own line ends while its grandparent's runs on past it");
    const ui::TreeGuides top = ui::BuildTreeGuides(t, 0, 3, -1);
    Check(top.rows.size() == 4 && !top.rows[3].last,
          L"tree guides: a line continues past the last visible row when more children follow");
    const ui::TreeGuides mid = ui::BuildTreeGuides(t, 4, 5, -1);
    Check(mid.rows[0].through == 1u && mid.rows[1].last,
          L"tree guides: rows scrolled into the middle still know their ancestors' lines");
    const ui::TreeGuides hi = ui::BuildTreeGuides(t, 0, 6, 4);
    Check(hi.hi_level == 1 && hi.hi_first == 4 && hi.hi_last == 4 && hi.Highlighted(4, 1) &&
          !hi.Highlighted(4, 0) && !hi.Highlighted(2, 0),
          L"tree guides: the focus row's folder line is the highlighted one");
    const ui::TreeGuides hi2 = ui::BuildTreeGuides(t, 0, 6, 2);
    Check(hi2.hi_level == 0 && hi2.hi_first == 2 && hi2.hi_last == 5 && hi2.Highlighted(4, 0),
          L"tree guides: highlighting a folder line covers its whole subtree");
    Check(ui::BuildTreeGuides(t, 0, 6, 1).hi_level < 0 && ui::BuildTreeGuides(t, 0, 6, -1).hi_level < 0,
          L"tree guides: a top-level or missing focus row highlights nothing");
    add(1, false, ui::TreeRowKind::Loading);
    t.rows[6].expandable = t.rows[6].expanded = true;
    const ui::TreeGuides ph = ui::BuildTreeGuides(t, 6, 7, -1);
    Check(ph.rows[1].join == 0 && ph.rows[1].last, L"tree guides: a loading row joins its folder's line");
}

void TestTreeSticky() {
    // 滚动时固定上级: A(open) / a1(open) / a11, a12 / a2, then B; rows 10 high.
    ui::DetailsTreeRows t;
    const auto add = [&](uint16_t depth, bool open) {
        ui::DetailsTreeRow r;
        r.depth = depth; r.expandable = open; r.expanded = open;
        t.rows.push_back(r);
    };
    add(0, true); add(1, true); add(2, false); add(2, false); add(1, false); add(0, false);
    const auto pins = [&](float scroll, int max_rows = ui::kTreeStickyMaxRows) {
        const int last = static_cast<int>(t.rows.size()) - 1;
        return ui::PinnedTreeRows(t, 10.0f, max_rows,
            [&](int row) { return static_cast<float>(row) * 10.0f - scroll; },
            [&](float y) {
                for (int i = 0; i <= last; ++i)
                    if (static_cast<float>(i) * 10.0f - scroll + 10.0f > y) return i;
                return last;
            });
    };
    Check(pins(0.0f).empty(), L"tree sticky: nothing pins while the folder row is still in place");
    const auto p5 = pins(5.0f);
    Check(p5.size() == 2 && p5[0].row == 0 && p5[0].y == 0.0f && p5[1].row == 1 && p5[1].y == 10.0f,
          L"tree sticky: an open folder pins once its row goes under the top, and so does a child the pin covers");
    const auto p25 = pins(25.0f);
    Check(p25.size() == 2 && p25[0].row == 0 && p25[0].y == 0.0f && p25[1].row == 1 && p25[1].y == 5.0f,
          L"tree sticky: a nested folder pins below its parent and the next sibling pushes it up");
    const auto p45 = pins(45.0f);
    Check(p45.size() == 1 && p45[0].row == 0 && p45[0].y == -5.0f,
          L"tree sticky: the next top-level folder pushes the last pin out");
    Check(pins(25.0f, 1).size() == 1 && pins(60.0f).empty(),
          L"tree sticky: at most max_rows pins, none once the subtree has passed");
    t.rows[0].expanded = false;
    Check(pins(5.0f).empty(), L"tree sticky: only open folders pin");
}

void TestLayerAlphas() {
    // 侧边栏与标题栏 over DWM Acrylic / Mica (backdrop on, not drawn by Pulse).
    const auto close_to = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    const auto old_layers = [](bool dark, int look, float& title, float& sheet, float& card) {
        static constexpr float kScale[5] = {1.40f, 1.20f, 1.0f, 0.80f, 0.60f};
        const float k = ui::LerpStops(static_cast<float>(look) / 100.0f, kScale);
        title = dark ? 0.72f : 0.78f;
        sheet = (std::min)(1.0f, (dark ? 0.55f : 0.60f) * k);
        card = (std::min)(1.0f, (dark ? 0.72f : 0.78f) * k);
    };
    bool same = true, mixed = true;
    for (int dark = 0; dark < 2; ++dark) {
        for (int look = 0; look <= 60; ++look) {
            float t = 0, sh = 0, c = 0;
            old_layers(dark != 0, look, t, sh, c);
            const ui::LayerAlphas a = ui::ComputeLayerAlphas(false, false, true, dark != 0, look, 14, 10);
            same = same && close_to(a.title, t) && close_to(a.sheet, sh) && close_to(a.card, c);
            const float stacked = 1.0f - (1.0f - t) * (1.0f - sh) * (1.0f - c);
            mixed = mixed && close_to(a.pane, stacked) &&
                    close_to(a.pane_mix[0] + a.pane_mix[1] + a.pane_mix[2], stacked);
        }
    }
    Check(same, L"layers: 侧边栏与标题栏 at 10 gives the old title, sheet and card up to 60");
    Check(mixed, L"layers: a cut-through pane card has the old stacked opacity and colour weights");
    const ui::LayerAlphas vivid = ui::ComputeLayerAlphas(false, false, true, false, 90, 14, 10);
    Check(close_to(vivid.pane, 0.80f), L"layers: interface transparency 90 leaves the file list 80% opaque");
    const auto side = [](const ui::LayerAlphas& a) { return 1.0f - (1.0f - a.title) * (1.0f - a.sheet); };
    const ui::LayerAlphas solid = ui::ComputeLayerAlphas(false, false, true, false, 50, 14, 0);
    Check(close_to(solid.title, 1.0f) && close_to(solid.sheet, 1.0f) && close_to(solid.card, 1.0f),
          L"layers: 侧边栏与标题栏 0 is solid like no window effect (the active tab keeps its colour)");
    bool clear = true;
    for (int dark = 0; dark < 2; ++dark) {
        const ui::LayerAlphas a = ui::ComputeLayerAlphas(false, false, true, dark != 0, 50, 14, 100);
        clear = clear && close_to(a.title, 0.0f) && close_to(side(a), 0.30f) && a.sheet > 0.25f;
    }
    Check(clear, L"layers: at 100 the title strip is the material and the sheet keeps 30% for the active tab");
    bool falling = true;
    float last_side = 2.0f;
    for (int v = 0; v <= 100; v += 5) {
        const ui::LayerAlphas a = ui::ComputeLayerAlphas(false, false, true, false, 60, 14, v);
        const float content = 1.0f - (1.0f - side(a)) * (1.0f - a.card);
        falling = falling && side(a) <= last_side + 1e-5f && a.title <= side(a) + 1e-5f &&
                  content >= a.pane - 1e-4f &&
                  close_to(a.pane, ui::ComputeLayerAlphas(false, false, true, false, 60, 14, 10).pane);
        last_side = side(a);
    }
    Check(falling, L"layers: the sidebar clears steadily; the file list and Settings cards never get thinner");
    const ui::LayerAlphas image = ui::ComputeLayerAlphas(true, true, true, false, 50, 14, 100);
    const ui::LayerAlphas image10 = ui::ComputeLayerAlphas(true, true, true, false, 50, 14, 10);
    const ui::LayerAlphas drawn = ui::ComputeLayerAlphas(false, true, true, false, 50, 14, 100);
    Check(close_to(image.title, image10.title) && close_to(image.sheet, image10.sheet) && image.pane < 0.0f &&
              drawn.pane < 0.0f,
          L"layers: a background image ignores 侧边栏与标题栏; a sampled material never cuts through");
}

void TestBloomSpring() {
    auto run = [](float from, float to, float k, float d, float& peak, float& lo, float& end) {
        float v = from;
        float vel = 0.0f;
        peak = from;
        lo = from;
        bool finite = true;
        for (int i = 0; i < 180; ++i) {
            ui::BloomStepSpring(v, vel, to, k, d, 0.016f);
            if (!std::isfinite(v) || !std::isfinite(vel)) finite = false;
            peak = (std::max)(peak, v);
            lo = (std::min)(lo, v);
        }
        end = v;
        Check(finite, L"bloom: spring stayed finite");
    };
    float peak = 0.0f, lo = 0.0f, end = 0.0f;
    run(1.0f, 1.18f, 200.0f, 30.0f, peak, lo, end);
    Check(peak < 1.35f && lo > 0.85f && std::fabs(end - 1.18f) < 0.05f,
          L"bloom: hover scale must not explode");
    run(0.0f, 5.0f, 100.0f, 30.0f, peak, lo, end);
    Check(peak < 6.0f && lo > -1.0f && std::fabs(end - 5.0f) < 0.15f,
          L"bloom: push spring must not explode");
}

void TestDragDropPure() {
    Check(ui::VolumeRoot(L"C:\\A\\B") == L"C:\\", L"dnd: volume root drive");
    Check(ui::VolumeRoot(L"\\\\srv\\share\\x") == L"\\\\srv\\share", L"dnd: volume root UNC");
    const DWORD both = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", both) == DROPEFFECT_MOVE,
          L"dnd: same volume defaults to move");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"D:\\dst", both) == DROPEFFECT_COPY,
          L"dnd: cross volume defaults to copy");
    Check(ui::ComputeDropEffect(MK_CONTROL, L"C:\\a.txt", L"C:\\dst", both) == DROPEFFECT_COPY,
          L"dnd: Ctrl forces copy");
    Check(ui::ComputeDropEffect(MK_SHIFT, L"D:\\a.txt", L"C:\\dst", both) == DROPEFFECT_MOVE,
          L"dnd: Shift forces move");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", DROPEFFECT_COPY) == DROPEFFECT_COPY,
          L"dnd: falls back to allowed effect");
    Check(ui::ComputeDropEffect(0, L"C:\\a.txt", L"C:\\dst", DROPEFFECT_LINK) == DROPEFFECT_LINK,
          L"dnd: no copy/move allowed -> link");

    Check(ui::FirstDroppableFolder({}).empty(), L"dnd: no sources -> no header folder");
    Check(ui::FirstDroppableFolder({ L"C:\\pulse_no_such_file.txt" }).empty(),
          L"dnd: missing path is not a header folder");
    wchar_t windir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windir, ARRAYSIZE(windir)) > 0) {
        Check(ui::FirstDroppableFolder({ windir }) == windir,
              L"dnd: directory is a header-drop folder");
        wchar_t self[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) > 0) {
            Check(ui::FirstDroppableFolder({ self }).empty(),
                  L"dnd: file is not a header-drop folder");
            Check(ui::FirstDroppableFolder({ self, windir }) == windir,
                  L"dnd: first real directory wins among mixed sources");
        }
    }
    Check(ui::LooksLikeFolderShortcut(L"C:\\Projects\\work.lnk"),
          L"dnd: .lnk suffix is a header-drop shortcut");
    Check(ui::LooksLikeFolderShortcut(L"C:\\Projects\\Work.LNK"),
          L"dnd: .lnk suffix is case-insensitive");
    Check(!ui::LooksLikeFolderShortcut(L"C:\\Projects\\work.txt"),
          L"dnd: non-lnk is not a header-drop shortcut");
    Check(!ui::LooksLikeFolderShortcut(L"lnk"), L"dnd: short names are not shortcuts");
}

void TestDirWatch() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulseWatchTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    std::atomic<int> hits{0};
    std::mutex mu;
    std::vector<fs::DirNotifyEvent> events;
    fs::DirWatch watch;
    const bool started = watch.Start(dir, [&](bool, std::vector<fs::DirNotifyEvent> batch) {
        hits.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu);
        for (auto& event : batch) events.push_back(std::move(event));
    });
    Check(started, L"watch: start on temp directory");
    Sleep(250);
    const int baseline = hits.load(std::memory_order_relaxed);
    Check(baseline <= 2, L"watch: overlapped pending is not treated as a change storm");

    const std::wstring file = dir + L"\\created.txt";
    HANDLE hf = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char body[] = "pulse";
    DWORD written = 0;
    if (hf != INVALID_HANDLE_VALUE) {
        WriteFile(hf, body, sizeof(body) - 1, &written, nullptr);
        CloseHandle(hf);
    }
    const ULONGLONG deadline = GetTickCount64() + 2000;
    bool saw_added = false;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mu);
            for (const auto& event : events) {
                if (event.action == FILE_ACTION_ADDED &&
                    _wcsicmp(event.name.c_str(), L"created.txt") == 0) {
                    saw_added = true;
                    break;
                }
            }
        }
        if (saw_added) break;
        Sleep(20);
    }
    Check(hits.load(std::memory_order_relaxed) > baseline, L"watch: new file notifies");
    Check(saw_added, L"watch: current directory create reports FILE_ACTION_ADDED");
    Sleep(200);
    Check(hits.load(std::memory_order_relaxed) - baseline < 30,
          L"watch: a single create does not spin the callback");
    watch.Stop();
    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());
}

bool SnapshotHasName(const fs::SnapshotPtr& snap, const wchar_t* name) {
    if (!snap) return false;
    for (const auto& entry : *snap) {
        if (_wcsicmp(entry.name.c_str(), name) == 0) return true;
    }
    return false;
}

void TestViewZoomChain() {
    using ui::ViewMode;
    using ui::ZoomViewMode;
    constexpr ViewMode Det = ViewMode::Details, Med = ViewMode::MediumIcons,
        Lrg = ViewMode::LargeIcons, XL = ViewMode::ExtraLargeIcons;
    Check(ZoomViewMode(Det, Med, 1) == Med && ZoomViewMode(Med, Med, 1) == Lrg &&
          ZoomViewMode(Lrg, Med, 1) == XL && ZoomViewMode(XL, Med, 1) == XL,
          L"view zoom: zooming in walks Details, medium, large, extra large and stops");
    Check(ZoomViewMode(XL, Med, -1) == Lrg && ZoomViewMode(Lrg, Med, -1) == Med &&
          ZoomViewMode(Med, Med, -1) == Det && ZoomViewMode(Det, Med, -1) == Det,
          L"view zoom: zooming out returns to Details and stops (no tiles or content)");
    Check(ZoomViewMode(Det, Lrg, 1) == Lrg && ZoomViewMode(Det, XL, 1) == XL && ZoomViewMode(Det, XL, 2) == XL,
          L"view zoom: the first step out of Details returns to the last icon size");
    Check(ZoomViewMode(Det, ViewMode::SmallIcons, 1) == Med && ZoomViewMode(Det, ViewMode::Tiles, 1) == Med,
          L"view zoom: an off-chain last size falls back to medium icons");
    for (ViewMode off : {ViewMode::List, ViewMode::SmallIcons, ViewMode::Tiles, ViewMode::Content}) {
        Check(ZoomViewMode(off, Lrg, 1) == Lrg && ZoomViewMode(off, Lrg, -1) == Det &&
              ZoomViewMode(off, Lrg, 0) == off,
              L"view zoom: list, small icons, tiles and content join the chain at its ends");
    }
    Check(ZoomViewMode(Det, Med, 9) == XL && ZoomViewMode(XL, Med, -9) == Det,
          L"view zoom: large steps clamp to the chain");
    for (int i = 0; i < 8; ++i) {
        const ViewMode mode = ui::ViewModeFromIndex(i);
        Check(ui::IsZoomIconMode(mode) == (mode == Med || mode == Lrg || mode == XL),
              L"view zoom: only medium, large and extra large are chain icon sizes");
    }
}

void TestContinuousIconSize() {
    using ui::ViewMode;
    using ui::ZoomIconDip;
    std::vector<float> up;
    for (float dip = ui::kMinIconDip; up.size() < 24 && dip < ui::kMaxIconDip;)
        up.push_back(dip = ZoomIconDip(dip, 1, 1.0f));
    const auto reaches = [&](float dip) { return std::find(up.begin(), up.end(), dip) != up.end(); };
    Check(reaches(48.0f) && reaches(96.0f) && reaches(240.0f) && !up.empty() &&
          up.back() == ui::kMaxIconDip && up.size() == 14,
          (L"icon size: zooming in stops on each preset and ends at 256 DIP (" +
           std::to_wstring(up.size()) + L" notches)").c_str());
    Check(ZoomIconDip(48.0f, 1, 1.0f) == 57.0f && ZoomIconDip(96.0f, -1, 1.0f) == 81.0f &&
          ZoomIconDip(136.0f, 1, 1.5f) == 160.0f && ZoomIconDip(100.0f, -1, 1.0f) == 96.0f,
          L"icon size: a notch is 2^(1/4) and stops on presets (extra large in device pixels)");
    Check(ZoomIconDip(ui::kMinIconDip, -1, 1.0f) == 0.0f && ZoomIconDip(34.0f, -1, 1.0f) == ui::kMinIconDip &&
          ZoomIconDip(ui::kMaxIconDip, 1, 1.0f) == ui::kMaxIconDip,
          L"icon size: zooming clamps to 32..256 DIP and leaves for Details below 32");
    Check(ui::IconModeForDip(57.0f, 1.0f) == ViewMode::MediumIcons &&
          ui::IconModeForDip(68.0f, 1.0f) == ViewMode::LargeIcons &&
          ui::IconModeForDip(151.0f, 1.0f) == ViewMode::LargeIcons &&
          ui::IconModeForDip(152.0f, 1.0f) == ViewMode::ExtraLargeIcons &&
          ui::IconModeForDip(130.0f, 3.0f) == ViewMode::ExtraLargeIcons,
          L"icon size: a size belongs to the nearest preset on a log scale");
    Check(ui::NormalizeIconDip(ViewMode::LargeIcons, 96.0f, 1.0f) == 0.0f &&
          ui::NormalizeIconDip(ViewMode::ExtraLargeIcons, 160.0f, 1.5f) == 0.0f &&
          ui::NormalizeIconDip(ViewMode::LargeIcons, 114.2f, 1.0f) == 114.0f &&
          ui::NormalizeIconDip(ViewMode::Details, 114.0f, 1.0f) == 0.0f &&
          ui::NormalizeIconDip(ViewMode::MediumIcons, 10.0f, 1.0f) == ui::kMinIconDip,
          L"icon size: presets are stored as 0, custom sizes as whole DIP in range");

    const D2D1_RECT_F viewport = D2D1::RectF(0.0f, 0.0f, 1000.0f, 700.0f);
    const float block = ui::IconGridNameBlockDip(2);
    auto metrics = [&](ViewMode mode, float dip) {
        return ui::ViewLayout(mode, viewport, 200, 0.0f, 0.0f, 1.0f, ui::ViewLayoutParams(block, dip)).Metrics();
    };
    const auto large = metrics(ViewMode::LargeIcons, 0.0f), large96 = metrics(ViewMode::LargeIcons, 96.0f);
    const auto xl = metrics(ViewMode::ExtraLargeIcons, 0.0f), xl240 = metrics(ViewMode::ExtraLargeIcons, 240.0f);
    const auto custom = metrics(ViewMode::LargeIcons, 120.0f);
    Check(large96.cell_width == large.cell_width && large96.cell_height == large.cell_height &&
          xl240.cell_width == xl.cell_width && xl240.slot_height == xl.slot_height,
          L"icon size: a custom size equal to a preset lays out like the preset");
    Check(custom.icon_size == 120.0f && custom.slot_height > 110.0f && custom.slot_height < 120.0f &&
          custom.columns < large.columns && custom.columns > xl.columns,
          (L"icon size: a size between presets has its own slot and columns (" +
           std::to_wstring(custom.columns) + L" columns)").c_str());
    Check(ui::IconGridNameWidth(ViewMode::LargeIcons, 1.0f, 120.0f) >
          ui::IconGridNameWidth(ViewMode::LargeIcons, 1.0f) &&
          ui::IconGridNameWidth(ViewMode::LargeIcons, 1.0f, 96.0f) ==
          ui::IconGridNameWidth(ViewMode::LargeIcons, 1.0f),
          L"icon size: names wrap at the custom size's cell width");

    AppPrefs prefs;
    prefs.persist = false;
    prefs.folder_views.Set(L"C:\\Photos", ViewMode::LargeIcons);
    Check(prefs.folder_views.SetIconDip(L"C:\\Photos", 136.0f) &&
          !prefs.folder_views.SetIconDip(L"c:/photos/", 136.0f) &&
          prefs.folder_views.SetIconDip(L"", 57.0f) &&
          !prefs.folder_views.SetIconDip(L"pulse:settings", 57.0f),
          L"icon size: folders and This PC keep a size, virtual views do not");
    AppPrefs reloaded;
    reloaded.persist = false;
    reloaded.FromJson(prefs.ToJson());
    Check(reloaded.folder_views.FindIconDip(L"C:\\PHOTOS\\") == 136.0f &&
          reloaded.folder_views.FindIconDip(L"") == 57.0f &&
          reloaded.folder_views.Find(L"C:\\Photos") == ViewMode::LargeIcons,
          L"icon size: sizes survive reload next to the view modes");
    reloaded.FromJson(L"{\"folder_icon_dip\":[\"x|C:\\\\Bad\",\"120\",\"|C:\\\\Empty\",\"9999|C:\\\\Big\"]}");
    Check(!reloaded.folder_views.FindIconDip(L"C:\\Bad") && !reloaded.folder_views.FindIconDip(L"C:\\Empty") &&
          !reloaded.folder_views.FindIconDip(L"C:\\Big") && !reloaded.folder_views.FindIconDip(L"C:\\Photos"),
          L"icon size: malformed saved sizes are ignored");
    Check(prefs.folder_views.SetIconDip(L"C:\\Photos", 0.0f) && !prefs.folder_views.FindIconDip(L"C:\\Photos"),
          L"icon size: 0 forgets a folder's size");
    prefs.folder_views.SetIconDip(L"C:\\Photos", 136.0f);
    prefs.folder_views.ApplyToAll(ViewMode::Details);
    Check(!prefs.folder_views.FindIconDip(L"C:\\Photos") && prefs.folder_views.FindIconDip(L"") == 57.0f,
          L"icon size: apply to all folders drops folder sizes, This PC keeps its own");
    // Photo wall (P2b): kept for folders and This PC like the sizes.
    Check(prefs.folder_views.SetWall(L"C:\\Photos", true) && !prefs.folder_views.SetWall(L"c:/photos/", true) &&
          prefs.folder_views.SetWall(L"", true) && !prefs.folder_views.SetWall(L"pulse:settings", true),
          L"photo wall: folders and This PC keep it, virtual views do not");
    reloaded.FromJson(prefs.ToJson());
    Check(reloaded.folder_views.FindWall(L"C:\\PHOTOS\\") && reloaded.folder_views.FindWall(L"") &&
          !reloaded.folder_views.FindWall(L"C:\\Other"),
          L"photo wall: survives reload next to the view modes");
    Check(prefs.folder_views.SetWall(L"C:\\Photos", false) && !prefs.folder_views.FindWall(L"C:\\Photos") &&
          !prefs.folder_views.SetWall(L"C:\\Photos", false),
          L"photo wall: turning it off forgets the folder's choice");
    prefs.folder_views.SetWall(L"C:\\Photos", true);
    prefs.folder_views.ApplyToAll(ViewMode::Details);
    Check(!prefs.folder_views.FindWall(L"C:\\Photos") && prefs.folder_views.FindWall(L""),
          L"photo wall: apply to all folders drops folder choices, This PC keeps its own");
    prefs.ResetToDefaults();
    Check(!prefs.folder_views.FindIconDip(L""), L"icon size: reset clears saved sizes");
    Check(!prefs.folder_views.FindWall(L""), L"photo wall: reset clears it");
}

void TestWallRows() {
    // Justified rows: full rows fill the width, the last keeps its height.
    std::vector<float> aspects(37, 1.5f);
    aspects[5] = 0.0f;   // not known yet: 4:3
    aspects[9] = 0.6f;   // portrait
    aspects[20] = 12.0f; // panorama, capped
    const float width = 1000.0f, row = 100.0f, scale = 1.0f;
    const ui::WallRows wall = ui::BuildWallRows(aspects, width, row, scale);
    bool filled = true, heights = true;
    for (size_t r = 0; r + 1 < wall.RowCount(); ++r) {
        const int last = wall.row_first[r + 1] - 1;
        filled = filled && std::fabs(wall.x[static_cast<size_t>(last)] + wall.w[static_cast<size_t>(last)] - (width - 8.0f)) < 0.5f;
        heights = heights && wall.row_h[r] <= row + 0.01f && wall.row_h[r] > row * 0.45f;
    }
    Check(wall.RowCount() > 3 && wall.row_first.back() == 37 && filled && heights,
          (L"photo wall: full rows fill the width at about the row height (" + std::to_wstring(wall.RowCount()) +
           L" rows)").c_str());
    const ui::WallRows one = ui::BuildWallRows(std::vector<float>(1, 0.0f), width, row, scale);
    Check(one.RowCount() == 1 && std::fabs(one.row_h[0] - row) < 0.01f && std::fabs(one.w[0] - row * 4.0f / 3.0f) < 0.01f,
          L"photo wall: a short last row keeps the row height; unknown pictures are 4:3");
    Check(wall.RowOf(0) == 0 && wall.RowOf(36) == static_cast<int>(wall.RowCount()) - 1 && wall.RowOf(37) == -1 &&
              wall.RowAtY(wall.row_top[1] + 1.0f) == 1 && wall.RowAtY(0.0f) == -1,
          L"photo wall: rows are found by index and by height");

    // ViewLayout on the rows: rects, hit test, visible range, arrows.
    const D2D1_RECT_F view = D2D1::RectF(0.0f, 0.0f, width, 300.0f);
    ui::ViewLayoutParams params(0.0f, 96.0f);
    params.wall = &wall;
    const float scroll = wall.row_top[2];
    const ui::ViewLayout layout(ui::ViewMode::LargeIcons, view, aspects.size(), 0.0f, scroll, scale, params);
    bool rects = true, hits = true;
    for (int i = 0; i < 37; ++i) {
        const D2D1_RECT_F r = layout.ItemRect(i);
        const size_t rr = static_cast<size_t>(wall.RowOf(i));
        rects = rects && std::fabs(r.left - wall.x[static_cast<size_t>(i)]) < 0.01f &&
                std::fabs(r.top - (wall.row_top[rr] - scroll)) < 0.01f;
        if (r.bottom > view.top + 1.0f && r.top < view.bottom - 1.0f)
            hits = hits && layout.HitTest((r.left + r.right) * 0.5f, std::clamp((r.top + r.bottom) * 0.5f,
                                                                               view.top + 0.5f, view.bottom - 1.0f)) == i;
    }
    const D2D1_RECT_F a = layout.ItemRect(wall.row_first[2]);
    const auto range = layout.VisibleRange();
    const bool covers = range.first <= wall.row_first[2] &&
                        range.second >= layout.HitTest(view.right - 9.0f, view.bottom - 2.0f);
    Check(rects && hits && layout.HitTest(a.right + 3.0f, a.top + 5.0f) == -1 && covers &&
              std::fabs(layout.ContentHeight() - wall.content_height) < 0.01f,
          L"photo wall: items are hit where drawn, gaps hit nothing, the visible range covers the view");
    const int from = wall.row_first[2] + 1;
    const int down = layout.MoveIndex(from, 0, 1);
    const float cx = wall.x[static_cast<size_t>(from)] + wall.w[static_cast<size_t>(from)] * 0.5f;
    Check(wall.RowOf(down) == 3 && wall.x[static_cast<size_t>(down)] <= cx + 0.5f &&
              wall.x[static_cast<size_t>(down)] + wall.w[static_cast<size_t>(down)] >= cx - 0.5f &&
              layout.MoveIndex(from, 1, 0) == from + 1 && layout.MoveIndex(0, 0, -1) == 0,
          L"photo wall: Down goes to the picture under the current one");

    // Many items: built in one pass, ends where it should.
    const ui::WallRows big = ui::BuildWallRows(std::vector<float>(100000, 1.33f), 1600.0f, 150.0f, 1.5f);
    Check(big.row_first.back() == 100000 && big.RowCount() > 1000 &&
              big.RowAtY(big.content_height - 20.0f) == static_cast<int>(big.RowCount()) - 1,
          L"photo wall: 100000 pictures lay out in one pass");
}

void TestInlineRename() {
    using ui::ViewMode;
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"inline rename: isolated owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    if (state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(L"C:\\InlineRenameFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        std::wstring long_name;
        for (int i = 0; i < 30; ++i) long_name += L"\x957F\x6587\x4EF6\x540D";  // 长文件名
        long_name += L".jpg";
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(4);
        (*entries)[0].name = long_name;
        (*entries)[1].name = L"notes.txt";
        (*entries)[2].name = L"report.docx";
        (*entries)[3].name = L"invoice_0001234567_ACME_Corporation_May_2024.pdf";
        tab->SetSnapshot(entries);
        tab->view_mode = ViewMode::MediumIcons;

        // Field geometry: two cells (280 dip at most), six lines, never narrower.
        ui::WindowViewModel vm = BuildVm(*state);
        const D2D1_RECT_F list = state->renderer.PaneListRect(vm.pane, FocusedPaneRect(*state));
        const D2D1_RECT_F cell = state->renderer.ItemRectInPane(vm.pane, FocusedPaneRect(*state), vm.pane.ViewIndex(0));
        const float cell_w = cell.right - cell.left;
        const float max_w = std::min({2.0f * cell_w + 8.0f, std::max(cell_w, 280.0f), list.right - list.left});
        const D2D1_RECT_F field = state->renderer.RenameFieldRect(vm.pane, list, 0);
        const float field_w = field.right - field.left;
        const float field_h = field.bottom - field.top;
        Check(field_w > cell_w + 1.0f && field_w <= max_w + 1.0f && field_h > 80.0f && field_h <= 6.0f * 22.0f + 6.0f,
              (L"inline rename: a long name gets up to two cells and six lines (" + std::to_wstring(field_w) +
               L" x " + std::to_wstring(field_h) + L", cell " + std::to_wstring(cell_w) + L")").c_str());
        const D2D1_RECT_F latin = state->renderer.RenameFieldRect(vm.pane, list, 3);
        Check(latin.bottom - latin.top > 40.0f,
              (L"inline rename: a long name without spaces wraps too (" + std::to_wstring(latin.right - latin.left) +
               L" x " + std::to_wstring(latin.bottom - latin.top) + L")").c_str());
        const D2D1_RECT_F short_field = state->renderer.RenameFieldRect(vm.pane, list, 1);
        vm.pane.rename_index = 1;
        vm.pane.rename_text = L"notes.txt";
        vm.pane.rename_field_w = 250.0f;
        const D2D1_RECT_F kept = state->renderer.RenameFieldRect(vm.pane, list, 1);
        Check(short_field.right - short_field.left >= cell_w - 0.5f &&
              std::fabs((kept.right - kept.left) - std::min(max_w, 250.0f)) <= 1.0f,
              L"inline rename: the field starts at the cell width and does not narrow while typing");

        // Keys: Tab relays, refused characters, the extension question.
        tab->SelectOnly(1);
        ShowRenameOverlay(*state);
        Check(state->renameIndex == 1 && state->hwndRenameEdit != nullptr, L"inline rename: the field opens");
        if (state->hwndRenameEdit) {
            SendMessageW(state->hwndRenameEdit, WM_KEYDOWN, VK_TAB, 0);
            Check(state->renameIndex == 2 && tab->selected_index == 2,
                  L"inline rename: Tab commits and renames the next item");
            HWND edit = state->hwndRenameEdit;
            wchar_t buf[128]{};
            SendMessageW(edit, WM_CHAR, L':', 1);
            GetWindowTextW(edit, buf, ARRAYSIZE(buf));
            Check(wcscmp(buf, L"report.docx") == 0 && !state->renameHint.empty() && state->renameHintUntil != 0,
                  L"inline rename: a character a name cannot hold is refused with a hint");
            SetWindowTextW(edit, L"report.pdf");
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            const bool asked = state->renameIndex == 2 && state->renameConfirmExt && state->renameHintUntil == 0;
            SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
            DWORD from = 0, to = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
            Check(asked && state->renameIndex == 2 && !state->renameConfirmExt && from == 7 && to == 10,
                  (L"inline rename: a changed extension asks first; Esc goes back with the extension selected (" +
                   std::to_wstring(from) + L".." + std::to_wstring(to) + L")").c_str());
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            const bool asked_again = state->renameConfirmExt;
            SendMessageW(edit, WM_CHAR, L'x', 1);
            Check(asked_again && !state->renameConfirmExt && state->renameIndex == 2,
                  L"inline rename: typing again drops the question");
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            HideRenameOverlay(*state, true);  // focus lost with the question open
            Check(state->renameIndex == -1 && tab->held_renames.empty(),
                  L"inline rename: an unanswered extension question never commits");
        }
        if (state->hwndRenameEdit) DestroyWindow(state->hwndRenameEdit);
        state->hwndRenameEdit = nullptr;
    } else Check(false, L"inline rename: graphics initialized");
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
}

void TestViewZoomAnchor() {
    using ui::ViewMode;
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"view anchor: isolated owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    if (state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(L"C:\\ZoomAnchorFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(400);
        for (size_t i = 0; i < entries->size(); ++i)
            (*entries)[i].name = L"photo_" + std::to_wstring(1000 + i) + L".jpg";
        tab->SetSnapshot(entries);
        tab->view_mode = ViewMode::Details;
        const D2D1_RECT_F list = ListRect(*state);
        auto rect_of = [&](int index) {
            const auto vm = BuildVm(*state);
            return state->renderer.ItemRectInPane(vm.pane, FocusedPaneRect(*state),
                vm.pane.ViewIndex(index));
        };
        auto mid_y = [](const D2D1_RECT_F& r) { return (r.top + r.bottom) * 0.5f; };

        tab->selected_index = 200;
        EnsureRowVisible(*state, *tab, 200);
        tab->scroll_y += (list.bottom - list.top) * 0.4f;  // focused row mid-screen
        ClampScroll(*state);
        const float details_y = mid_y(rect_of(200));
        SetViewMode(*state, ViewMode::LargeIcons);
        const D2D1_RECT_F large = rect_of(200);
        Check(tab->scroll_y > 0.0f && std::fabs(mid_y(large) - details_y) <= 1.0f &&
              large.top >= list.top && large.bottom <= list.bottom,
              (L"view anchor: the focused item keeps its place when switching to icons (" +
               std::to_wstring(details_y) + L" -> " + std::to_wstring(mid_y(large)) + L", item " +
               std::to_wstring(large.top) + L".." + std::to_wstring(large.bottom) + L", list " +
               std::to_wstring(list.top) + L".." + std::to_wstring(list.bottom) + L", scroll " +
               std::to_wstring(tab->scroll_y) + L")").c_str());
        SetViewMode(*state, ViewMode::Details);
        Check(std::fabs(mid_y(rect_of(200)) - details_y) <= 1.0f,
              (L"view anchor: switching back to Details returns the focused row to its place (" +
               std::to_wstring(mid_y(rect_of(200))) + L")").c_str());

        // Ctrl+wheel: the item under the pointer stays under it (rows, not columns).
        const POINT pt{static_cast<LONG>(list.left + 40.0f), static_cast<LONG>(list.top + 60.0f)};
        const int under = state->renderer.ItemFromPointInPane(BuildVm(*state).pane,
            FocusedPaneRect(*state), static_cast<float>(pt.x), static_cast<float>(pt.y));
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        const LPARAM at = MAKELPARAM(screen.x, screen.y);
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), at);
        const D2D1_RECT_F zoomed = rect_of(under);
        const float py = static_cast<float>(pt.y);
        Check(under >= 0 && under != 200 && tab->view_mode == ViewMode::LargeIcons &&
              zoomed.top <= py && py < zoomed.bottom,
              L"view anchor: Ctrl+wheel keeps the item under the pointer on the pointer's row");
        state->zoomWheelTick = 0;
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA / 2), at);
        const bool half_waits = tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 0.0f;
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA / 2), at);
        Check(half_waits && tab->view_mode == ViewMode::LargeIcons &&
              std::fabs(tab->icon_dip - 114.0f) < 0.5f,
              (L"view zoom: touchpad half notches add up to one step (" +
               std::to_wstring(tab->icon_dip) + L" DIP)").c_str());
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL,
            MAKEWPARAM(MK_CONTROL, static_cast<WORD>(-3 * WHEEL_DELTA)), at);
        Check(tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 0.0f,
              L"view zoom: a fast spin moves one size per message");

        // Continuous sizes: zooming out walks down to the smallest size, then
        // Details; the first step back returns to that size, remembered per folder.
        int notches = 0;
        while (ui::IsZoomIconMode(tab->view_mode) && notches < 16) {
            ZoomIconSize(*state, -1);
            ++notches;
        }
        Check(tab->view_mode == ViewMode::Details && notches == 8 &&
              tab->last_icon_mode == ViewMode::MediumIcons && tab->last_icon_dip == ui::kMinIconDip,
              (L"icon size: zooming out stops at 32 DIP before Details (" +
               std::to_wstring(notches) + L" notches)").c_str());
        ZoomIconSize(*state, 1);
        Check(tab->view_mode == ViewMode::MediumIcons && tab->icon_dip == ui::kMinIconDip &&
              state->appPrefs.folder_views.FindIconDip(tab->current_path) == ui::kMinIconDip,
              L"icon size: zooming in from Details returns to the last size, kept for the folder");
        SetIconSize(*state, 150.0f);
        SetViewMode(*state, ViewMode::Details);
        ShowLastIconSize(*state);
        Check(tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 150.0f,
              L"icon size: the icon button returns to a custom size");
        SetViewMode(*state, ViewMode::LargeIcons);
        Check(tab->icon_dip == 0.0f && !state->appPrefs.folder_views.FindIconDip(tab->current_path),
              L"icon size: choosing a size from the menu returns to its preset");

        // Header size slider (P1e): icon views only; drag, plain wheel, double-click.
        state->pane->header_animation.Tick(true, 1);
        SetViewMode(*state, ViewMode::LargeIcons);
        const ui::WindowViewModel hvm = BuildVm(*state);
        const D2D1_RECT_F client = D2D1::RectF(0.0f, 0.0f, static_cast<float>(state->compositor.Width()),
                                               static_cast<float>(state->compositor.Height()));
        const ui::PaneViewModel& hpane = hvm.pane_slots.front().pane;
        const D2D1_RECT_F hrect = hvm.pane_slots.front().rect;
        const D2D1_RECT_F slider = state->renderer.PaneSizeSliderRect(hpane, hrect);
        const float sy = (slider.top + slider.bottom) * 0.5f;
        Check(slider.right - slider.left > 60.0f &&
              state->renderer.HitTest(hvm, client, (slider.left + slider.right) * 0.5f, sy).region ==
                  ui::HitTestResult::PaneSizeSlider &&
              state->renderer.PaneSizeSliderValueAt(hpane, hrect, slider.left) == ui::kMinIconDip &&
              state->renderer.PaneSizeSliderValueAt(hpane, hrect, slider.right) == ui::kMaxIconDip,
              L"view header: icon views show a 32..256 DIP size slider in the pane header");
        const auto client_at = [](float x, float y) {
            return MAKELPARAM(static_cast<int>(x), static_cast<int>(y));
        };
        // Press where the slider reads about 110 DIP (the track's ends move with the design).
        float press_x = slider.left;
        for (float px = slider.left; px <= slider.right; px += 1.0f) {
            if (std::fabs(state->renderer.PaneSizeSliderValueAt(hpane, hrect, px) - 110.0f) <
                std::fabs(state->renderer.PaneSizeSliderValueAt(hpane, hrect, press_x) - 110.0f))
                press_x = px;
        }
        HandleLButtonDown(state.get(), hwnd, WM_LBUTTONDOWN, MK_LBUTTON, client_at(press_x, sy));
        const float pressed = state->paneSizeDragging && tab->view_mode == ViewMode::LargeIcons
            ? tab->icon_dip : 0.0f;
        HandleMouseMove(state.get(), hwnd, WM_MOUSEMOVE, MK_LBUTTON, client_at(slider.right + 30.0f, sy));
        HandleLButtonUp(state.get(), hwnd, WM_LBUTTONUP, 0, client_at(slider.right + 30.0f, sy));
        Check(pressed > 100.0f && pressed < 140.0f && !state->paneSizeDragging &&
              tab->view_mode == ViewMode::ExtraLargeIcons && tab->icon_dip == ui::kMaxIconDip &&
              state->appPrefs.folder_views.FindIconDip(tab->current_path) == ui::kMaxIconDip,
              (L"view header: dragging the slider resizes the icons and keeps the size for the folder (" +
               std::to_wstring(pressed) + L" -> " + std::to_wstring(tab->icon_dip) + L")").c_str());
        POINT wheel_pt{static_cast<LONG>((slider.left + slider.right) * 0.5f), static_cast<LONG>(sy)};
        ClientToScreen(hwnd, &wheel_pt);
        const LPARAM on_slider = MAKELPARAM(wheel_pt.x, wheel_pt.y);
        state->zoomWheelTick = 0;
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), on_slider);
        const bool wheel_down = tab->view_mode == ViewMode::ExtraLargeIcons && tab->icon_dip == 0.0f;
        SetIconSize(*state, ui::kMinIconDip);
        state->zoomWheelTick = 0;
        HandleMouseWheel(state.get(), hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), on_slider);
        Check(wheel_down && tab->view_mode == ViewMode::MediumIcons && tab->icon_dip == ui::kMinIconDip,
              L"view header: the plain wheel on the slider changes the size and stays in the icon views");
        HandleLButtonDblClk(state.get(), hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, client_at(press_x, sy));
        if (GetCapture() == hwnd) ReleaseCapture();
        state->paneSizeDragging = false;
        Check(tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 0.0f,
              L"view header: double-clicking the slider returns to large icons");
        ui::PaneViewModel narrow_pane = hpane;
        const D2D1_RECT_F narrow = D2D1::RectF(hrect.left, hrect.top, hrect.left + 400.0f, hrect.bottom);
        const D2D1_RECT_F size_button = state->renderer.PaneSizeButtonRect(narrow_pane, narrow);
        const D2D1_RECT_F folded = state->renderer.PaneSizeSliderRect(narrow_pane, narrow);
        narrow_pane.size_slider_open = true;
        const D2D1_RECT_F opened = state->renderer.PaneSizeSliderRect(narrow_pane, narrow);
        Check(size_button.right > size_button.left && folded.right <= folded.left &&
              opened.right > opened.left && opened.right <= size_button.left && opened.left >= narrow.left,
              L"view header: a narrow pane folds the slider into a button that opens it over the title");
        SetViewMode(*state, ViewMode::Details);
        const ui::WindowViewModel dvm = BuildVm(*state);
        const D2D1_RECT_F hidden = state->renderer.PaneSizeSliderRect(dvm.pane_slots.front().pane,
                                                                      dvm.pane_slots.front().rect);
        Check(hidden.right <= hidden.left, L"view header: Details hides the size slider");

        // Photo wall (P2b): the segment holds details, icons and photo wall
        // ahead of the view menu; the wall keeps the size and the folder keeps it.
        SetViewMode(*state, ViewMode::LargeIcons);
        const ui::WindowViewModel gvm = BuildVm(*state);
        const ui::PaneViewModel& gpane = gvm.pane_slots.front().pane;
        const D2D1_RECT_F grect = gvm.pane_slots.front().rect;
        const D2D1_RECT_F seg_list = state->renderer.PaneDetailsRect(grect, 1.0f, true);
        const D2D1_RECT_F seg_grid = state->renderer.PaneMediumIconsRect(grect, 1.0f, true);
        const D2D1_RECT_F seg_wall = state->renderer.PaneWallRect(gpane, grect);
        const D2D1_RECT_F seg_more = state->renderer.PaneViewButtonRect(grect);
        const auto region_at = [&](const ui::WindowViewModel& v, const D2D1_RECT_F& r) {
            return state->renderer.HitTest(v, client, (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f).region;
        };
        Check(seg_wall.right > seg_wall.left && seg_list.right <= seg_grid.left && seg_grid.right <= seg_wall.left &&
              seg_wall.right <= seg_more.left && state->renderer.PaneSizeSliderRect(gpane, grect).right <= seg_list.left &&
              region_at(gvm, seg_list) == ui::HitTestResult::PaneDetails &&
              region_at(gvm, seg_grid) == ui::HitTestResult::PaneMediumIcons &&
              region_at(gvm, seg_wall) == ui::HitTestResult::PaneWall &&
              region_at(gvm, seg_more) == ui::HitTestResult::PaneViewButton,
              L"photo wall: the header segment holds details, icons and photo wall ahead of the view menu");
        const D2D1_RECT_F grid_item = state->renderer.ItemRectInPane(gpane, grect, 3);
        const auto press = [&](const D2D1_RECT_F& r) {
            const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
            HandleLButtonDown(state.get(), hwnd, WM_LBUTTONDOWN, MK_LBUTTON, client_at(cx, cy));
            HandleLButtonUp(state.get(), hwnd, WM_LBUTTONUP, 0, client_at(cx, cy));
        };
        press(seg_wall);
        const ui::WindowViewModel wvm = BuildVm(*state);
        const D2D1_RECT_F wall_item = state->renderer.ItemRectInPane(wvm.pane_slots.front().pane,
                                                                     wvm.pane_slots.front().rect, 3);
        Check(tab->wall && tab->view_mode == ViewMode::LargeIcons && wvm.pane_slots.front().pane.wall &&
              state->appPrefs.folder_views.FindWall(tab->current_path) &&
              (std::fabs(wall_item.left - grid_item.left) > 1.0f || std::fabs(wall_item.right - grid_item.right) > 1.0f ||
               std::fabs(wall_item.top - grid_item.top) > 1.0f),
              L"photo wall: the wall button keeps the size, lays the items out in rows and is kept for the folder");
        SetIconSize(*state, 150.0f);
        SetViewMode(*state, ViewMode::Details);
        ShowLastIconSize(*state);
        Check(tab->wall && tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 150.0f,
              L"photo wall: resizing and the way back from Details keep the wall");
        press(seg_grid);
        Check(!tab->wall && tab->view_mode == ViewMode::LargeIcons && tab->icon_dip == 150.0f &&
              !state->appPrefs.folder_views.FindWall(tab->current_path),
              L"photo wall: the icons button returns to the grid at the same size");

        // At the top with nothing selected there is nothing to follow.
        tab->selected_index = -1;
        SetViewMode(*state, ViewMode::Details);
        tab->scroll_x = tab->scroll_y = 0.0f;
        SetViewMode(*state, ViewMode::LargeIcons);
        const bool large_top = tab->scroll_y == 0.0f;
        SetViewMode(*state, ViewMode::MediumIcons);
        Check(large_top && tab->scroll_y == 0.0f,
              L"view anchor: at the top with nothing selected, switching views stays at the top");
    } else Check(false, L"view anchor: graphics initialized");
    state->watches.Stop();
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
}

void TestFolderViews() {
    using ui::ViewMode;
    AppPrefs prefs;
    prefs.persist = false;
    prefs.folder_views.Set(L"C:\\Pictures", ViewMode::LargeIcons);
    prefs.folder_views.Set(L"C:\\Pictures\\Work", ViewMode::Details);
    prefs.folder_views.Set(L"C:\\Pictures\\Work\\Draft", ViewMode::List);
    Check(prefs.folder_views.Find(L"C:\\Pictures") == ViewMode::LargeIcons &&
          prefs.folder_views.Find(L"C:\\Pictures\\Work") == ViewMode::Details &&
          prefs.folder_views.Find(L"C:\\Pictures\\Work\\Draft") == ViewMode::List,
          L"folder views: three levels keep independent choices");
    Check(!prefs.folder_views.Find(L"C:\\Pictures\\Other") &&
          !prefs.folder_views.Find(L"C:\\Pictures2"),
          L"folder views: children and siblings do not inherit");
    Check(prefs.folder_views.Find(L"\\\\?\\c:\\PICTURES\\") == ViewMode::LargeIcons &&
          prefs.folder_views.Find(L"C:/Pictures/") == ViewMode::LargeIcons,
          L"folder views: case, separators, trailing slash and extended prefix agree");
    const std::wstring unc = L"\\\\server\\share\\图片";
    const std::wstring long_path = L"C:\\" + std::wstring(280, L'x');
    prefs.folder_views.Set(unc, ViewMode::Tiles);
    prefs.folder_views.Set(long_path, ViewMode::Content);
    Check(prefs.folder_views.Find(L"\\\\?\\UNC\\SERVER\\share\\图片\\") == ViewMode::Tiles,
          L"folder views: UNC aliases agree");
    Check(!prefs.folder_views.Set(L"pulse:settings", ViewMode::List) &&
          !prefs.folder_views.Set(L"relative", ViewMode::List),
          L"folder views: virtual and nonabsolute paths are not persisted");
    Check(prefs.folder_views.Set(L"", ViewMode::Tiles) &&
          prefs.folder_views.Find(L"") == ViewMode::Tiles &&
          !prefs.folder_views.Find(L"pulse:settings") && !prefs.folder_views.Find(L"relative"),
          L"folder views: This PC keeps its own choice without leaking into virtual views");
    AppPrefs reloaded;
    reloaded.persist = false;
    reloaded.FromJson(prefs.ToJson());
    Check(reloaded.folder_views.Find(unc) == ViewMode::Tiles &&
          reloaded.folder_views.Find(long_path) == ViewMode::Content &&
          reloaded.folder_views.Find(L"C:\\Pictures\\Work") == ViewMode::Details &&
          reloaded.folder_views.Find(L"") == ViewMode::Tiles,
          L"folder views: preferences round trip including This PC, UNC, Unicode and long paths");
    for (int i = 0; i < 8; ++i) {
        const auto mode = ui::ViewModeFromIndex(i);
        prefs.folder_views.Set(L"C:\\AllModes", mode);
        reloaded.FromJson(prefs.ToJson());
        Check(reloaded.folder_views.Find(L"C:\\AllModes") == mode,
              L"folder views: every display mode survives reload");
    }
    reloaded.FromJson(L"{}");
    Check(!reloaded.folder_views.Find(unc) && !reloaded.folder_views.Find(L""),
          L"folder views: old preferences load without stale choices");
    prefs.ResetToDefaults();
    Check(!prefs.folder_views.Find(long_path) && !prefs.folder_views.Find(L""),
          L"folder views: reset clears saved choices");

    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"folder views: isolated owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    const bool ready = state->compositor.Init(hwnd);
    Check(ready, L"folder views: renderer initialized");
    if (ready) {
        state->renderer.SetCompositor(&state->compositor);
        const auto parent = WorkspacePath(L"bench_data/folder-view-fixture");
        const auto child = parent + L"\\child";
        const auto grandchild = child + L"\\grandchild";
        state->window_tabs.NewTab(parent);
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        StartLoadingPath(*state, *tab, parent);
        SetViewMode(*state, ViewMode::LargeIcons);
        tab->NavigateTo(child);
        StartLoadingPath(*state, *tab, child);
        Check(tab->view_mode == ViewMode::Details, L"folder views: navigating into child resets inherited large icons");
        SetViewMode(*state, ViewMode::List);
        tab->NavigateTo(grandchild);
        StartLoadingPath(*state, *tab, grandchild);
        Check(tab->view_mode == ViewMode::Details, L"folder views: third level starts independently");
        SetViewMode(*state, ViewMode::Details);
        Check(state->appPrefs.folder_views.Find(grandchild) == ViewMode::Details,
              L"folder views: explicitly choosing the current mode also saves it");
        StartLoadingPath(*state, *tab, tab->GoBack());
        Check(tab->view_mode == ViewMode::List, L"folder views: back restores child mode");
        StartLoadingPath(*state, *tab, tab->GoBack());
        Check(tab->view_mode == ViewMode::LargeIcons, L"folder views: back restores parent mode");
        StartLoadingPath(*state, *tab, tab->GoForward());
        Check(tab->view_mode == ViewMode::List, L"folder views: forward restores child mode");
        Check(tab->last_icon_mode == ViewMode::LargeIcons,
              L"view zoom: a list folder keeps the last icon size for the header button");
        state->appPrefs.FromJson(state->appPrefs.ToJson());
        tab->view_mode = ViewMode::SmallIcons;
        tab->last_icon_mode = ViewMode::MediumIcons;
        StartLoadingPath(*state, *tab, parent, PathLoadReason::RestoreSession);
        Check(tab->view_mode == ViewMode::LargeIcons, L"folder views: saved folder choice wins over session mode");
        Check(tab->last_icon_mode == ViewMode::LargeIcons,
              L"view zoom: opening a saved icon folder records its size");
        tab->view_mode = ViewMode::Tiles;
        StartLoadingPath(*state, *tab, parent + L"\\legacy", PathLoadReason::RestoreSession);
        Check(tab->view_mode == ViewMode::Tiles, L"folder views: legacy session without folder preference keeps its mode");
        StartLoadingPath(*state, *tab, parent);
        StartLoadingPath(*state, *tab, parent + L"\\legacy");
        Check(tab->view_mode == ViewMode::Tiles, L"folder views: returning to migrated legacy folder preserves its mode");
        // This PC: tiles chosen there survive a drive visit, going back and a reload.
        tab->NavigateTo(L"");
        StartLoadingPath(*state, *tab, L"");
        Check(tab->view_mode == ViewMode::Details, L"folder views: This PC starts in the default mode");
        SetViewMode(*state, ViewMode::Tiles);
        tab->NavigateTo(parent);
        StartLoadingPath(*state, *tab, parent);
        Check(tab->view_mode == ViewMode::LargeIcons,
              L"folder views: a folder opened from This PC keeps its own mode");
        StartLoadingPath(*state, *tab, tab->GoBack());
        Check(tab->current_path.empty() && tab->view_mode == ViewMode::Tiles,
              L"folder views: going back to This PC keeps tiles");
        Tab opened_at_this_pc;
        opened_at_this_pc.NavigateTo(L"");
        const bool reopen_adds_nothing = !opened_at_this_pc.CanGoBack();
        opened_at_this_pc.NavigateTo(parent);
        Check(reopen_adds_nothing && opened_at_this_pc.CanGoBack() &&
              opened_at_this_pc.GoBack().empty(),
              L"folder views: a tab opened at This PC can go back to it from a drive");
        state->appPrefs.FromJson(state->appPrefs.ToJson());
        StartLoadingPath(*state, *tab, parent);
        StartLoadingPath(*state, *tab, L"");
        Check(tab->view_mode == ViewMode::Tiles, L"folder views: This PC mode survives a reload");
        state->shot.active = true;
        tab->view_mode = ViewMode::Content;
        StartLoadingPath(*state, *tab, parent);
        Check(tab->view_mode == ViewMode::Content, L"folder views: explicit screenshot mode is respected");
        state->shot.active = false;
        NewTab(*state, parent);
        Check(ActiveTab(*state)->view_mode == ViewMode::LargeIcons,
              L"folder views: opening a new tab restores saved mode");
    }
    state->watches.Stop();
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
}

void TestFolderSorts() {
    using ui::SortColumn;
    using ui::SortDirection;
    using ui::ViewMode;
    const FolderSort by_name{};
    const FolderSort newest{ SortColumn::Mtime, SortDirection::Desc };
    const FolderSort biggest{ SortColumn::Size, SortDirection::Desc };
    AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.folder_sorts.Default() == by_name && !prefs.folder_sorts.Find(L"C:\\A"),
          L"folder sorts: fresh preferences default to name ascending");
    prefs.folder_sorts.Set(L"C:\\A", by_name);
    prefs.folder_sorts.Set(L"C:\\B", newest);
    Check(prefs.folder_sorts.Find(L"C:\\A") == by_name &&
          prefs.folder_sorts.Find(L"C:\\B") == newest &&
          !prefs.folder_sorts.Find(L"C:\\B\\child"),
          L"folder sorts: folders keep independent choices and children do not inherit");
    Check(prefs.folder_sorts.Find(L"\\\\?\\c:\\b\\") == newest &&
          prefs.folder_sorts.Find(L"C:/B/") == newest,
          L"folder sorts: case, separators, trailing slash and extended prefix agree");
    Check(!prefs.folder_sorts.Set(L"pulse:search:x", newest) &&
          !prefs.folder_sorts.Set(L"", newest) &&
          !prefs.folder_sorts.Set(L"relative", newest) &&
          !prefs.folder_sorts.Set(L"C:\\Bad", { static_cast<SortColumn>(9), SortDirection::Asc }),
          L"folder sorts: virtual, nonabsolute and invalid values are rejected");
    const std::wstring unc = L"\\\\server\\share\\照片";
    prefs.folder_sorts.Set(unc, biggest);
    AppPrefs reloaded;
    reloaded.persist = false;
    reloaded.FromJson(prefs.ToJson());
    Check(reloaded.folder_sorts.Find(L"C:\\B") == newest &&
          reloaded.folder_sorts.Find(unc) == biggest &&
          reloaded.folder_sorts.Find(L"C:\\A") == by_name,
          L"folder sorts: preferences round trip including UNC and Unicode");
    bool every_survives = true;
    for (int c = 0; c < 5; ++c) {
        for (int d = 0; d < 2; ++d) {
            const FolderSort each{ static_cast<SortColumn>(c), static_cast<SortDirection>(d) };
            prefs.folder_sorts.Set(L"C:\\Every", each);
            reloaded.FromJson(prefs.ToJson());
            every_survives = every_survives && reloaded.folder_sorts.Find(L"C:\\Every") == each;
        }
    }
    Check(every_survives, L"folder sorts: every column and direction survives reload");
    prefs.folder_views.Set(L"C:\\B", ViewMode::LargeIcons);
    prefs.folder_views.ApplyToAll(ViewMode::List);
    prefs.folder_sorts.ApplyToAll(biggest);
    Check(!prefs.folder_sorts.Find(L"C:\\B") && !prefs.folder_views.Find(L"C:\\B") &&
          prefs.folder_sorts.Default() == biggest && prefs.folder_views.Default() == ViewMode::List,
          L"folder sorts: apply to all replaces defaults and forgets per-folder choices");
    reloaded.FromJson(prefs.ToJson());
    Check(reloaded.folder_sorts.Default() == biggest &&
          reloaded.folder_views.Default() == ViewMode::List,
          L"folder sorts: applied defaults survive reload");
    reloaded.FromJson(L"{}");
    Check(reloaded.folder_sorts.Default() == by_name &&
          reloaded.folder_views.Default() == ViewMode::Details && !reloaded.folder_sorts.Find(unc),
          L"folder sorts: old preferences load with name order and details view");
    {
        using pulse::app::GroupBy;
        prefs.folder_groups.Set(L"C:\\G1", GroupBy::Size);
        prefs.folder_groups.Set(L"pulse:recent", GroupBy::None);
        prefs.folder_groups.ApplyToAll(GroupBy::Type);
        Check(prefs.folder_groups.Resolve(L"C:\\G1") == GroupBy::Type &&
              prefs.folder_groups.Resolve(L"C:\\Fresh") == GroupBy::Type &&
              prefs.folder_groups.Resolve(L"pulse:recent") == GroupBy::None &&
              prefs.folder_groups.Resolve(L"pulse:recycle") == GroupBy::Date &&
              prefs.folder_groups.Resolve(L"") == GroupBy::None,
              L"folder groups: apply to all covers real folders and keeps virtual views (#75)");
        reloaded.FromJson(prefs.ToJson());
        Check(reloaded.folder_groups.Default() == GroupBy::Type &&
              reloaded.folder_groups.Resolve(L"C:\\Fresh") == GroupBy::Type &&
              reloaded.folder_groups.Resolve(L"pulse:recent") == GroupBy::None,
              L"folder groups: applied default survives reload");
        prefs.folder_groups.Set(L"C:\\G1", GroupBy::None);
        Check(prefs.folder_groups.Resolve(L"C:\\G1") == GroupBy::None,
              L"folder groups: a later per-folder choice beats the default");
        reloaded.FromJson(L"{}");
        Check(!reloaded.folder_groups.Default() && reloaded.folder_groups.Resolve(L"C:\\Fresh") == GroupBy::None,
              L"folder groups: old preferences keep folders ungrouped");
    }
    prefs.ResetToDefaults();
    Check(prefs.folder_sorts.Default() == by_name &&
          prefs.folder_views.Default() == ViewMode::Details,
          L"folder sorts: reset restores defaults");

    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"folder sorts: isolated owner created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    const bool ready = state->compositor.Init(hwnd);
    Check(ready, L"folder sorts: renderer initialized");
    if (ready) {
        state->renderer.SetCompositor(&state->compositor);
        const auto a = WorkspacePath(L"bench_data/folder-sort-fixture/a");
        const auto b = WorkspacePath(L"bench_data/folder-sort-fixture/b");
        const auto c = WorkspacePath(L"bench_data/folder-sort-fixture/c");
        const auto sort_of = [](const app::Tab* t) {
            return t ? FolderSort{ t->sort_column, t->sort_direction } : FolderSort{};
        };
        state->window_tabs.NewTab(a);
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        StartLoadingPath(*state, *tab, a);
        Check(sort_of(tab) == by_name, L"folder sorts: unsaved folder opens by name");
        tab->NavigateTo(b);
        StartLoadingPath(*state, *tab, b);
        SetSort(*state, SortColumn::Mtime, SortDirection::Desc);
        Check(state->appPrefs.folder_sorts.Find(b) == newest, L"folder sorts: choosing an order saves it");
        tab->NavigateTo(a);
        StartLoadingPath(*state, *tab, a);
        Check(sort_of(tab) == by_name, L"folder sorts: leaving a date-sorted folder does not carry its order");
        tab->NavigateTo(b);
        StartLoadingPath(*state, *tab, b);
        Check(sort_of(tab) == newest, L"folder sorts: reopening a folder restores its saved order (#26)");
        StartLoadingPath(*state, *tab, tab->GoBack());
        Check(sort_of(tab) == by_name, L"folder sorts: back restores the previous folder's order");
        StartLoadingPath(*state, *tab, tab->GoForward());
        Check(sort_of(tab) == newest, L"folder sorts: forward restores the saved order");
        tab->search_content_active = true;
        SetSort(*state, SortColumn::Size, SortDirection::Asc);
        tab->search_content_active = false;
        tab->content_sort_override = false;
        Check(state->appPrefs.folder_sorts.Find(b) == newest,
              L"folder sorts: sorting search results does not change the folder's order");
        NewTab(*state, b);
        Check(sort_of(ActiveTab(*state)) == newest, L"folder sorts: a new tab restores the saved order");

        auto* active = ActiveTab(*state);
        SetViewMode(*state, ViewMode::Tiles);
        Check(ApplyViewToAllFolders(*state, false), L"folder sorts: apply to all runs on a real folder");
        active->NavigateTo(c);
        StartLoadingPath(*state, *active, c);
        Check(sort_of(active) == newest && active->view_mode == ViewMode::Tiles,
              L"folder sorts: apply to all makes unsaved folders match (#31)");
        active->NavigateTo(a);
        StartLoadingPath(*state, *active, a);
        Check(sort_of(active) == newest && active->view_mode == ViewMode::Tiles,
              L"folder sorts: apply to all overrides earlier per-folder choices");
        const std::wstring real_path = active->current_path;
        active->current_path = L"pulse:recent";
        Check(!ApplyViewToAllFolders(*state, false), L"folder sorts: apply to all ignores virtual views");
        active->current_path = real_path;
        SetGroupBy(*state, 4);
        Check(ApplyGroupToAllFolders(*state, 3, false), L"folder groups: apply to all runs on a real folder");
        active->NavigateTo(c);
        StartLoadingPath(*state, *active, c);
        Check(active->group_by == 3, L"folder groups: apply to all groups unsaved folders (#75)");
        active->NavigateTo(a);
        StartLoadingPath(*state, *active, a);
        Check(active->group_by == 3, L"folder groups: apply to all overrides earlier per-folder grouping");
        active->current_path = L"pulse:recent";
        Check(!ApplyGroupToAllFolders(*state, 3, false), L"folder groups: apply to all ignores virtual views");
        active->current_path = real_path;
        state->appPrefs.folder_groups.Clear();
    }
    state->watches.Stop();
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
}

void TestStartupLocation() {
    AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.startup_open == 0 && prefs.new_tab_open == 0 && prefs.home_folder.empty() &&
          app::RestoresLastTabs(prefs) && app::DefaultLocation(prefs).empty(),
          L"startup location: defaults restore the last tabs and point at This PC");
    Check(app::NewTabLocation(prefs, L"D:\\Current") == L"D:\\Current",
          L"startup location: new tabs keep the current folder by default");
    prefs.startup_open = 1;
    prefs.new_tab_open = 1;
    prefs.home_folder = L"\\\\server\\share\\照片";
    AppPrefs reloaded;
    reloaded.persist = false;
    Check(reloaded.FromJson(prefs.ToJson()) && reloaded.startup_open == 1 &&
          reloaded.new_tab_open == 1 && reloaded.home_folder == prefs.home_folder &&
          !app::RestoresLastTabs(reloaded),
          L"startup location: choices round trip including UNC and Unicode");
    Check(app::NewTabLocation(reloaded, L"D:\\Current") == prefs.home_folder,
          L"startup location: new tabs open the default location when chosen (#34)");
    reloaded.FromJson(L"{\"startup_open\":7,\"new_tab_open\":-1}");
    Check(reloaded.startup_open == 0 && reloaded.new_tab_open == 0 && reloaded.home_folder.empty(),
          L"startup location: invalid or missing values keep the old behavior");
    prefs.ResetToDefaults();
    Check(prefs.startup_open == 0 && prefs.new_tab_open == 0 && prefs.home_folder.empty(),
          L"startup location: reset restores defaults");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring root = std::wstring(temp) + L"PulseStartupLocation-" +
                              std::to_wstring(GetCurrentProcessId());
    const std::wstring work = root + L"\\work";
    const std::wstring home = root + L"\\home";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(work.c_str(), nullptr);
    CreateDirectoryW(home.c_str(), nullptr);

    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"startup location: isolated owner created");
    if (hwnd) {
        state->hwnd = hwnd;
        const bool ready = state->compositor.Init(hwnd);
        Check(ready, L"startup location: renderer initialized");
        if (ready) {
            state->renderer.SetCompositor(&state->compositor);
            state->window_tabs.NewTab(work);
            state->pane = state->window_tabs.Active()->panes.front().get();
            StartLoadingPath(*state, *state->pane->ActiveTab(), work);
            const std::wstring work_path = state->pane->ActiveTab()->current_path;
            Check(NewTabPath(*state) == work_path,
                  L"startup location: current-folder mode opens the active folder");
            const size_t before = state->window_tabs.items.size();
            state->appPrefs.new_tab_open = 1;
            Check(NewTabPath(*state).empty(), L"startup location: default location starts as This PC");
            OpenNewTab(*state);
            const app::Tab* opened = ActiveTab(*state);
            Check(state->window_tabs.items.size() == before + 1 && opened &&
                  opened->current_path.empty(),
                  L"startup location: a new tab opens This PC, not C:\\");
            state->appPrefs.home_folder = home;
            OpenNewTab(*state);
            opened = ActiveTab(*state);
            const std::wstring home_path = opened ? opened->current_path : std::wstring();
            Check(state->window_tabs.items.size() == before + 2 && !home_path.empty() &&
                  _wcsicmp(home_path.c_str(), fs::NormalizePath(home).c_str()) == 0,
                  L"startup location: a new tab opens the chosen folder");
            state->appPrefs.new_tab_open = 0;
            OpenNewTab(*state);
            opened = ActiveTab(*state);
            Check(opened && opened->current_path == home_path,
                  L"startup location: switching back follows the current folder again");

            std::wstring picked = work;
            bool picker_shown = false;
            SettingsController::UiCallbacks callbacks;
            callbacks.pick_folder = [&](std::wstring& path, std::wstring_view) {
                picker_shown = true;
                if (picked.empty()) return false;
                path = picked;
                return true;
            };
            state->settings.BindUi(state->appPrefs, state->ctxMenuPrefs, state->index,
                                   state->networkIndex, std::move(callbacks));
            state->settings.HomeFolder(0);
            Check(picker_shown && state->appPrefs.home_folder == work,
                  L"startup location: choosing a folder stores it");
            picked.clear();
            state->settings.HomeFolder(0);
            Check(state->appPrefs.home_folder == work,
                  L"startup location: cancelling the picker keeps the folder");
            state->settings.HomeFolder(1);
            Check(state->appPrefs.home_folder.empty(),
                  L"startup location: This PC button clears the folder");
            state->settings.StartupOpen(1);
            state->settings.NewTabOpen(1);
            Check(state->appPrefs.startup_open == 1 && state->appPrefs.new_tab_open == 1,
                  L"startup location: segmented choices select the default location");
            state->settings.StartupOpen(5);
            state->settings.NewTabOpen(-1);
            Check(state->appPrefs.startup_open == 1 && state->appPrefs.new_tab_open == 1,
                  L"startup location: out-of-range choices are ignored");
            state->settings.StartupOpen(0);
            state->settings.NewTabOpen(0);
            Check(state->appPrefs.startup_open == 0 && state->appPrefs.new_tab_open == 0,
                  L"startup location: segmented choices return to the old behavior");
            state->settings.ResetUi();
        }
        state->watches.Stop();
        DestroyWindow(hwnd);
        state->hwnd = nullptr;
    }
    state.reset();
    RemoveDirectoryW(work.c_str());
    RemoveDirectoryW(home.c_str());
    RemoveDirectoryW(root.c_str());
}

// Close to the tray and back with startup = default location: the window
// starts over there (pinned tabs stay), a second launch with a folder starts
// over at that folder, and "restore last tabs" leaves everything alone. The
// tray icon itself is never added here: the hide step is simulated.
// Settings > Quick Look: codec detection never blocks the caller. It used to
// run MFTEnumEx inside BuildVm, where COM pumped a title-bar WM_NCHITTEST that
// re-entered BuildVm and probed again until the stack overflowed.
void TestPreviewCodecProbe() {
    const ULONGLONG start = GetTickCount64();
    unsigned mask = 0;
    for (int i = 0; i < 9; ++i) mask = ui::DetectPreviewCodecs(true, nullptr);
    Check(GetTickCount64() - start < 200, L"preview codecs: detection does not block the caller");
    while (!(mask & ui::kPreviewCodecsDetected) && GetTickCount64() - start < 20000) {
        Sleep(20);
        mask = ui::DetectPreviewCodecs(false, nullptr);
    }
    Check((mask & ui::kPreviewCodecsDetected) != 0, L"preview codecs: the worker publishes a detected mask");
}

void TestLockedItemPrompt() {
    const l10n::StringId ids[] = {l10n::StringId::LockedItemTitle, l10n::StringId::LockedItemMessage,
        l10n::StringId::LockedItemEndHint, l10n::StringId::LockedItemCloseHint,
        l10n::StringId::LockedItemEndRetry, l10n::StringId::LockedItemRetry};
    bool all_loaded = true;
    for (const auto id : ids) all_loaded = all_loaded && !l10n::Get(id).empty();
    Check(all_loaded, L"locked item: every prompt string is loaded");
    ops::OpStatus status;
    status.phase = ops::OpPhase::Failed;
    status.locked_path = L"C:\\Work\\report.docx";
    ops::LockOwner word;
    word.pid = 4242;
    word.app_name = L"Microsoft Word";
    word.image_name = L"WINWORD.EXE";
    status.lock_owners.push_back(word);
    const ui::ConfirmDialogSpec spec = LockedItemPromptSpec(status, true);
    Check(spec.message.find(L"report.docx") != std::wstring::npos &&
          spec.message.find(L"C:\\Work") == std::wstring::npos &&
          spec.message.find(L"{name}") == std::wstring::npos,
          L"locked item: the prompt names the file, not its folder");
    Check(spec.items.size() == 1 && spec.items[0].find(L"WINWORD.EXE, PID 4242") != std::wstring::npos,
          L"locked item: the prompt lists the owning process");
    Check(spec.note == l10n::Get(l10n::StringId::LockedItemEndHint) && spec.danger &&
              spec.confirm_text == l10n::Get(l10n::StringId::LockedItemEndRetry),
          L"locked item: ending owners warns about unsaved work");
    Check(LockedItemCanEndAny(status), L"locked item: closable owners allow end-and-retry");
    status.lock_owners.back().closable = false;
    const ui::ConfirmDialogSpec retry = LockedItemPromptSpec(status, false);
    Check(!LockedItemCanEndAny(status) &&
              retry.note == l10n::Get(l10n::StringId::LockedItemCloseHint) && !retry.danger &&
              retry.confirm_text == l10n::Get(l10n::StringId::LockedItemRetry),
          L"locked item: protected owners only get the close-and-retry hint");
    // One killable owner among protected ones still offers the button: the rest
    // (Pulse itself, services) cannot be ended from here, but the user must not
    // be left with 重试 alone.
    ops::LockOwner helper;
    helper.pid = 4243;
    helper.image_name = L"Pulse.Index.exe";
    status.lock_owners.push_back(helper);
    Check(LockedItemCanEndAny(status),
          L"locked item: one killable owner keeps end-and-retry next to a protected one");
}

void TestTrayReveal() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring root = std::wstring(temp) + L"PulseTrayReveal-" +
                              std::to_wstring(GetCurrentProcessId());
    const std::wstring work = root + L"\\work";
    const std::wstring other = root + L"\\other";
    const std::wstring home = root + L"\\home";
    CreateDirectoryW(root.c_str(), nullptr);
    for (const auto* dir : {&work, &other, &home}) CreateDirectoryW(dir->c_str(), nullptr);

    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, L"tray reveal: isolated owner created");
    if (hwnd) {
        state->hwnd = hwnd;
        const bool ready = state->compositor.Init(hwnd);
        Check(ready, L"tray reveal: renderer initialized");
        if (ready) {
            state->renderer.SetCompositor(&state->compositor);
            state->window_tabs.NewTab(work);
            state->pane = state->window_tabs.Active()->panes.front().get();
            StartLoadingPath(*state, *state->pane->ActiveTab(), work);
            NewTab(*state, other);
            NewTab(*state, work);
            state->window_tabs.items[0]->pinned = true;
            auto active_is = [&](const std::wstring& path) {
                const app::Tab* tab = ActiveTab(*state);
                return tab && (path.empty() ? tab->current_path.empty()
                                            : _wcsicmp(tab->current_path.c_str(),
                                                       fs::NormalizePath(path).c_str()) == 0);
            };

            state->hidden_to_tray = true;
            Check(!TakeFreshStart(*state) && !state->hidden_to_tray && state->window_tabs.items.size() == 3,
                  L"tray reveal: restoring last tabs keeps the window as it was");
            state->appPrefs.startup_open = 1;
            state->appPrefs.home_folder = home;
            Check(!TakeFreshStart(*state), L"tray reveal: nothing starts over unless the window was closed");

            InstallTrayRevealHook(*state);
            state->tray_controller.Attach(hwnd, GetModuleHandleW(nullptr));
            state->hidden_to_tray = true;
            state->tray_controller.RestoreWindow();
            Check(IsWindowVisible(hwnd) && state->window_tabs.items.size() == 2 &&
                  state->window_tabs.items[0]->pinned && state->window_tabs.active == 1 &&
                  active_is(home) && !state->hidden_to_tray,
                  L"tray reveal: shown from the tray it starts over at the default location, pinned tabs stay");
            NewTab(*state, other);
            state->tray_controller.RestoreWindow();
            Check(state->window_tabs.items.size() == 3 && active_is(other),
                  L"tray reveal: raising a visible window leaves its tabs alone");

            ShowWindow(hwnd, SW_HIDE);
            state->hidden_to_tray = true;
            OpenFolderInNewTab(*state, other);
            Check(IsWindowVisible(hwnd) && state->window_tabs.items.size() == 2 && active_is(other) &&
                  state->window_tabs.items[0]->pinned,
                  L"tray reveal: a second launch with a folder starts over at that folder");

            ShowWindow(hwnd, SW_HIDE);
            state->hidden_to_tray = true;
            state->appPrefs.home_folder.clear();
            state->tray_controller.RestoreWindow();
            Check(state->window_tabs.items.size() == 2 && active_is(std::wstring()),
                  L"tray reveal: a This PC default starts over at This PC");
            ShowWindow(hwnd, SW_HIDE);

            // Sign-in launch into the tray (startup_launch.h): neither setting
            // keeps an icon, yet one shows until the window is revealed.
            state->appPrefs.keep_running_on_close = false;
            state->appPrefs.global_search_enabled = false;
            const size_t tabs_before = state->window_tabs.items.size();
            const bool started_hidden = state->tray_controller.StartHidden(true);
            Check(started_hidden && !IsWindowVisible(hwnd) && state->tray_controller.IconVisible(),
                  L"start in tray: the window stays hidden behind a tray icon");
            state->tray_controller.HandleTaskbarCreated();
            Check(state->tray_controller.IconVisible(),
                  L"start in tray: the icon comes back after Explorer recreates the taskbar");
            state->tray_controller.RestoreWindow();
            Check(IsWindowVisible(hwnd) && IsZoomed(hwnd) && !state->tray_controller.IconVisible() &&
                  state->window_tabs.items.size() == tabs_before,
                  L"start in tray: a click restores the maximized window as it was and drops the icon");
            state->tray_controller.HandleTaskbarCreated();
            Check(!state->tray_controller.IconVisible(),
                  L"start in tray: a recreated taskbar does not bring back a dropped icon");
            ShowWindow(hwnd, SW_RESTORE);
            ShowWindow(hwnd, SW_HIDE);

            // Notification-area icon setting (#57).
            Check(TrayIconWanted(true, 0, false) && !TrayIconWanted(true, 1, false) &&
                  TrayIconWanted(true, 1, true) && !TrayIconWanted(true, 2, true) &&
                  !TrayIconWanted(false, 0, true) && !TrayIconWanted(false, 1, true),
                  L"tray icon: always, only in the background or never; none without background running");
            state->appPrefs.keep_running_on_close = true;
            state->appPrefs.notify_icon_mode = 2;
            ShowWindow(hwnd, SW_SHOW);
            HideMainWindowToTray(*state);
            Check(!IsWindowVisible(hwnd) && !state->tray_controller.IconVisible() && state->hidden_to_tray,
                  L"tray icon never: closing hides the window without an icon");
            state->tray_controller.RestoreWindow();
            Check(IsWindowVisible(hwnd) && !state->tray_controller.IconVisible() && !state->hidden_to_tray,
                  L"tray icon never: a second launch brings the window back, still without an icon");
            state->appPrefs.notify_icon_mode = 1;
            HideMainWindowToTray(*state);
            Check(!IsWindowVisible(hwnd) && state->tray_controller.IconVisible(),
                  L"tray icon in background: closing shows the icon");
            state->tray_controller.RestoreWindow();
            Check(IsWindowVisible(hwnd) && !state->tray_controller.IconVisible(),
                  L"tray icon in background: the icon goes when the window comes back");
            state->appPrefs.notify_icon_mode = 0;
            state->tray_controller.SetVisible(WantsTrayIcon(*state, false));
            Check(state->tray_controller.IconVisible(),
                  L"tray icon always: shown while the window is open");
            state->tray_controller.SetVisible(false);
            ShowWindow(hwnd, SW_HIDE);
            state->appPrefs.notify_icon_mode = 2;
            Check(state->tray_controller.StartHidden(false, false) && !IsWindowVisible(hwnd) &&
                  !state->tray_controller.IconVisible(),
                  L"tray icon never: a sign-in launch into the tray stays hidden without an icon");
            state->appPrefs.keep_running_on_close = false;
            state->appPrefs.notify_icon_mode = 0;
            state->tray_controller.Detach();
        }
        state->watches.Stop();
        DestroyWindow(hwnd);
        state->hwnd = nullptr;
    }
    state.reset();
    for (const auto* dir : {&work, &other, &home}) RemoveDirectoryW(dir->c_str());
    RemoveDirectoryW(root.c_str());
}

void TestNavigateAlwaysEnumerates() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring parent = std::wstring(temp) + L"PulseEnumParent-" +
                                std::to_wstring(GetCurrentProcessId());
    const std::wstring child = parent + L"\\child";
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(child.c_str(), nullptr);

    const std::wstring before = parent + L"\\before.txt";
    HANDLE hf = CreateFileW(before.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> hits{0};
    WorkResult last;
    WorkerPool worker;
    worker.Start([&](WorkResult res) {
        std::lock_guard<std::mutex> lock(mu);
        last = std::move(res);
        hits.fetch_add(1, std::memory_order_relaxed);
        cv.notify_one();
    });

    const std::wstring path = fs::NormalizePath(parent);
    worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return hits.load() >= 1; });
    }
    Check(!last.error && last.snapshot, L"enum: first parent listing succeeds");
    Check(SnapshotHasName(last.snapshot, L"before.txt"),
          L"enum: first listing contains the original file");
    Check(SnapshotHasName(last.snapshot, L"child"),
          L"enum: first listing contains the child folder");

    fs::DirectoryIdentity id1, id2;
    Check(fs::QueryDirectoryIdentity(path, id1), L"enum: query parent identity");

    const std::wstring after = parent + L"\\after-up.txt";
    hf = CreateFileW(after.c_str(), GENERIC_WRITE, 0, nullptr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    Check(fs::QueryDirectoryIdentity(path, id2) && fs::SameDirectoryIdentity(id1, id2),
          L"enum: creating a child file does not change directory identity");

    const int at = hits.load();
    worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(10), [&] { return hits.load() > at; });
    }
    Check(!last.error && last.snapshot, L"enum: second parent listing succeeds");
    Check(SnapshotHasName(last.snapshot, L"after-up.txt"),
          L"enum: parent listing includes file created while viewing a child");

    worker.Stop();
    DeleteFileW(before.c_str());
    DeleteFileW(after.c_str());
    RemoveDirectoryW(child.c_str());
    RemoveDirectoryW(parent.c_str());
}

void TestSnapshotPatch() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulsePatchTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring a = dir + L"\\alpha.txt";
    const std::wstring b = dir + L"\\beta.txt";
    HANDLE hf = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);

    std::vector<fs::DirEntry> entries;
    fs::DirNotifyEvent added;
    added.action = FILE_ACTION_ADDED;
    added.name = L"alpha.txt";
    Check(ApplyDirNotify(entries, dir, added, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 1 && _wcsicmp(entries[0].name.c_str(), L"alpha.txt") == 0,
          L"patch: ADDED inserts a real file");

    hf = CreateFileW(b.c_str(), GENERIC_WRITE, 0, nullptr,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    added.name = L"beta.txt";
    Check(ApplyDirNotify(entries, dir, added, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 2,
          L"patch: second ADDED keeps sort order");
    Check(_wcsicmp(entries[0].name.c_str(), L"alpha.txt") == 0 &&
          _wcsicmp(entries[1].name.c_str(), L"beta.txt") == 0,
          L"patch: name sort is alpha then beta");

    fs::DirNotifyEvent renamed;
    renamed.action = FILE_ACTION_RENAMED_NEW_NAME;
    renamed.old_name = L"beta.txt";
    renamed.name = L"gamma.txt";
    MoveFileW(b.c_str(), (dir + L"\\gamma.txt").c_str());
    Check(ApplyDirNotify(entries, dir, renamed, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied,
          L"patch: RENAMED applies");
    Check(entries.size() == 2 && _wcsicmp(entries[1].name.c_str(), L"gamma.txt") == 0,
          L"patch: rename updates the listing name");

    fs::DirNotifyEvent removed;
    removed.action = FILE_ACTION_REMOVED;
    removed.name = L"alpha.txt";
    DeleteFileW(a.c_str());
    Check(ApplyDirNotify(entries, dir, removed, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::Applied &&
          entries.size() == 1,
          L"patch: REMOVED drops the file");

    fs::DirNotifyEvent nested;
    nested.action = FILE_ACTION_ADDED;
    nested.name = L"sub\\file.txt";
    Check(ApplyDirNotify(entries, dir, nested, ui::SortColumn::Name, ui::SortDirection::Asc) ==
              NotifyPatch::NeedFullEnum,
          L"patch: nested names require a full enumeration");

    DeleteFileW((dir + L"\\gamma.txt").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestSnapshotPatchBatch() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulsePatchBatch-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const auto full = [&](const wchar_t* name) { return dir + L"\\" + name; };
    const auto make = [&](const wchar_t* name, DWORD bytes) {
        HANDLE hf = CreateFileW(full(name).c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;
        std::vector<char> data(bytes, 'x');
        DWORD written = 0;
        if (bytes) WriteFile(hf, data.data(), bytes, &written, nullptr);
        CloseHandle(hf);
    };
    const struct { const wchar_t* name; DWORD bytes; } seed[] = {
        {L"alpha.txt", 0}, {L"beta.log", 10}, {L"Gamma.txt", 10}, {L"delta.txt", 20},
        {L"epsilon.md", 5}, {L"file2.txt", 0}, {L"file10.txt", 30}, {L"zeta.txt", 10}};
    std::vector<fs::DirEntry> initial;
    for (const auto& item : seed) {
        make(item.name, item.bytes);
        fs::DirEntry entry;
        if (FillDirEntry(dir, item.name, entry)) initial.push_back(std::move(entry));
    }
    CreateDirectoryW(full(L"docs").c_str(), nullptr);
    {
        fs::DirEntry entry;
        if (FillDirEntry(dir, L"docs", entry)) initial.push_back(std::move(entry));
    }

    // Disk reaches its final state first; both paths then stat the same files.
    make(L"file3.txt", 7);
    DeleteFileW(full(L"beta.log").c_str());
    MoveFileW(full(L"delta.txt").c_str(), full(L"omega.txt").c_str());
    MoveFileW(full(L"Gamma.txt").c_str(), full(L"gamma.txt").c_str());
    make(L"zeta.txt", 99);
    make(L"file20.txt", 1);
    make(L"alpha.txt", 3);  // empty -> tiny: changes its size group
    MoveFileW(full(L"epsilon.md").c_str(), full(L"epsilon.txt").c_str());  // changes its type group
    const auto ev = [](DWORD action, const wchar_t* name, const wchar_t* old_name = L"") {
        fs::DirNotifyEvent event;
        event.action = action;
        event.name = name;
        event.old_name = old_name;
        return event;
    };
    // New rows go to their sort position (2bef32f). One at a time, that
    // position depends on rows a later rename holds out of order (#13), so
    // the rows that move (new files, alpha changing its size group) arrive
    // last here; the batch merges them in after all renames either way.
    const std::vector<fs::DirNotifyEvent> events = {
        ev(FILE_ACTION_REMOVED, L"beta.log"),
        ev(FILE_ACTION_RENAMED_NEW_NAME, L"omega.txt", L"delta.txt"),
        ev(FILE_ACTION_RENAMED_NEW_NAME, L"gamma.txt", L"Gamma.txt"),
        ev(FILE_ACTION_ADDED, L"temp.tmp"), ev(FILE_ACTION_REMOVED, L"temp.tmp"),
        ev(FILE_ACTION_MODIFIED, L"zeta.txt"),
        ev(FILE_ACTION_ADDED, L"ghost.txt"), ev(FILE_ACTION_REMOVED, L"missing.txt"),
        ev(FILE_ACTION_RENAMED_NEW_NAME, L"epsilon.txt", L"epsilon.md"),
        ev(FILE_ACTION_MODIFIED, L"alpha.txt"),
        ev(FILE_ACTION_ADDED, L"file3.txt"), ev(FILE_ACTION_MODIFIED, L"file3.txt"),
        ev(FILE_ACTION_ADDED, L"file20.txt")};

    bool same = true;
    const ui::SortColumn cols[] = {ui::SortColumn::Name, ui::SortColumn::Size,
                                   ui::SortColumn::Mtime, ui::SortColumn::Type};
    const ui::SortDirection dirs[] = {ui::SortDirection::Asc, ui::SortDirection::Desc};
    std::vector<fs::DirEntry> last;
    const int groups[] = {static_cast<int>(GroupBy::None), static_cast<int>(GroupBy::Type),
                          static_cast<int>(GroupBy::Size)};
    for (const int group : groups) {
    const ScopedEntryGrouping grouping(group);
    for (const auto col : cols) {
        for (const auto sort_dir : dirs) {
            std::vector<fs::DirEntry> base = initial;
            std::sort(base.begin(), base.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) {
                return EntryLess(a, b, col, sort_dir);
            });
            std::vector<fs::DirEntry> seq = base;
            for (const auto& event : events)
                ApplyDirNotify(seq, dir, event, col, sort_dir);
            std::vector<fs::DirEntry> batch = base;
            if (ApplyDirNotifyBatch(batch, dir, events, col, sort_dir) != NotifyPatch::Applied ||
                batch.size() != seq.size()) {
                same = false;
                continue;
            }
            // Grouped, a row that changes group moves like a new one, and one
            // at a time its spot depends on the rows held in place around it
            // then; the batch places it after the whole burst. Same rows there.
            std::vector<fs::DirEntry> seq_rows = seq, batch_rows = batch;
            if (group != static_cast<int>(GroupBy::None)) {
                const auto by_name = [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.name < b.name; };
                std::sort(seq_rows.begin(), seq_rows.end(), by_name);
                std::sort(batch_rows.begin(), batch_rows.end(), by_name);
            }
            for (size_t i = 0; i < seq_rows.size(); ++i) {
                if (seq_rows[i].name != batch_rows[i].name || seq_rows[i].size != batch_rows[i].size ||
                    seq_rows[i].is_dir != batch_rows[i].is_dir)
                    same = false;
            }
            last = batch;
        }
    }
    }
    Check(same, L"patch batch: matches per-event patching (same order ungrouped, same rows grouped)");
    const auto has = [&](const wchar_t* name) {
        return std::any_of(last.begin(), last.end(),
                           [&](const fs::DirEntry& e) { return e.name == name; });
    };
    Check(last.size() == 10 && has(L"file3.txt") && has(L"file20.txt") && has(L"omega.txt") &&
              has(L"gamma.txt") && !has(L"Gamma.txt") && !has(L"beta.log") &&
              !has(L"delta.txt") && !has(L"temp.tmp") && !has(L"ghost.txt"),
          L"patch batch: adds, removes, renames and case-only renames resolve");
    const std::vector<fs::DirNotifyEvent> nested = {
        ev(FILE_ACTION_ADDED, L"a.txt"), ev(FILE_ACTION_ADDED, L"b.txt"),
        ev(FILE_ACTION_ADDED, L"c.txt"), ev(FILE_ACTION_ADDED, L"d.txt"),
        ev(FILE_ACTION_ADDED, L"sub\\file.txt")};
    std::vector<fs::DirEntry> scratch = initial;
    Check(ApplyDirNotifyBatch(scratch, dir, nested, ui::SortColumn::Name,
                              ui::SortDirection::Asc) == NotifyPatch::NeedFullEnum,
          L"patch batch: nested names require a full enumeration");

    for (const wchar_t* name : {L"alpha.txt", L"gamma.txt", L"omega.txt", L"epsilon.txt",
                                L"file2.txt", L"file3.txt", L"file10.txt", L"file20.txt",
                                L"zeta.txt"})
        DeleteFileW(full(name).c_str());
    RemoveDirectoryW(full(L"docs").c_str());
    RemoveDirectoryW(dir.c_str());
}

// #13: patches keep rows on screen in place like File Explorer.
void TestSnapshotPatchHoldsRows() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulsePatchHold-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const auto full = [&](const wchar_t* name) { return dir + L"\\" + name; };
    const auto make = [&](const wchar_t* name, DWORD bytes) {
        HANDLE hf = CreateFileW(full(name).c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;
        std::vector<char> data(bytes, 'x');
        DWORD written = 0;
        if (bytes) WriteFile(hf, data.data(), bytes, &written, nullptr);
        CloseHandle(hf);
    };
    const auto ev = [](DWORD action, const wchar_t* name, const wchar_t* old_name = L"") {
        fs::DirNotifyEvent event;
        event.action = action;
        event.name = name;
        event.old_name = old_name;
        return event;
    };
    const auto names = [](const std::vector<fs::DirEntry>& entries) {
        std::wstring out;
        for (const auto& entry : entries) {
            if (!out.empty()) out += L",";
            out += entry.name;
        }
        return out;
    };
    std::vector<fs::DirEntry> entries;
    for (const wchar_t* name : {L"alpha.txt", L"beta.txt", L"gamma.txt"}) {
        make(name, 1);
        fs::DirEntry entry;
        if (FillDirEntry(dir, name, entry)) entries.push_back(std::move(entry));
    }
    const auto name_asc = [](std::vector<fs::DirEntry>& list, const fs::DirNotifyEvent& event,
                             const std::wstring& folder) {
        return ApplyDirNotify(list, folder, event, ui::SortColumn::Name, ui::SortDirection::Asc);
    };

    MoveFileW(full(L"alpha.txt").c_str(), full(L"zeta.txt").c_str());
    Check(name_asc(entries, ev(FILE_ACTION_RENAMED_NEW_NAME, L"zeta.txt", L"alpha.txt"), dir) ==
              NotifyPatch::Applied &&
          names(entries) == L"zeta.txt,beta.txt,gamma.txt",
          L"patch hold: a renamed row keeps its place instead of re-sorting");
    make(L"aardvark.txt", 1);
    Check(name_asc(entries, ev(FILE_ACTION_ADDED, L"aardvark.txt"), dir) == NotifyPatch::Applied &&
          names(entries) == L"aardvark.txt,zeta.txt,beta.txt,gamma.txt",
          L"patch hold: a new row goes before the first row that sorts after it");
    make(L"beta.txt", 5000);
    Check(ApplyDirNotify(entries, dir, ev(FILE_ACTION_MODIFIED, L"beta.txt"),
                         ui::SortColumn::Size, ui::SortDirection::Desc) == NotifyPatch::Applied &&
          names(entries) == L"aardvark.txt,zeta.txt,beta.txt,gamma.txt" &&
          entries[2].size == 5000u,
          L"patch hold: a modified row keeps its place and takes the new size");

    {
        const ScopedEntryGrouping grouping(static_cast<int>(GroupBy::Type));
        CreateDirectoryW(full(L"new").c_str(), nullptr);
        make(L"notes.md", 1);
        name_asc(entries, ev(FILE_ACTION_ADDED, L"new"), dir);
        name_asc(entries, ev(FILE_ACTION_ADDED, L"notes.md"), dir);
        Check(names(entries) == L"new,notes.md,aardvark.txt,zeta.txt,beta.txt,gamma.txt",
              L"patch hold: grouped, a new row goes to its sort position in its group");
        MoveFileW(full(L"zeta.txt").c_str(), full(L"zeta.md").c_str());
        name_asc(entries, ev(FILE_ACTION_RENAMED_NEW_NAME, L"zeta.md", L"zeta.txt"), dir);
        Check(names(entries) == L"new,notes.md,zeta.md,aardvark.txt,beta.txt,gamma.txt",
              L"patch hold: grouped, a rename into another group moves to its place there");
    }

    for (const wchar_t* name : {L"zeta.md", L"beta.txt", L"gamma.txt", L"aardvark.txt", L"notes.md"})
        DeleteFileW(full(name).c_str());
    RemoveDirectoryW(full(L"new").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestEntryOrderHold() {
    const auto entry = [](const wchar_t* name, bool is_dir = false, uint64_t size = 1) {
        fs::DirEntry e;
        e.name = name;
        e.is_dir = is_dir;
        e.attrs = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        e.size = size;
        return e;
    };
    const auto names = [](const std::vector<fs::DirEntry>& entries) {
        std::wstring out;
        for (const auto& e : entries) {
            if (!out.empty()) out += L",";
            out += e.name;
        }
        return out;
    };
    const auto sorted = [](std::vector<fs::DirEntry> list) {
        std::sort(list.begin(), list.end(), [](const fs::DirEntry& a, const fs::DirEntry& b) {
            return EntryLess(a, b, ui::SortColumn::Name, ui::SortDirection::Asc);
        });
        return list;
    };
    const auto keep = [](const std::vector<fs::DirEntry>& shown, const std::vector<fs::DirEntry>& fresh,
                         const std::vector<EntryRename>& renames) {
        return KeepEntryOrder(shown, fresh, renames, ui::SortColumn::Name, ui::SortDirection::Asc);
    };

    const std::vector<fs::DirEntry> shown = {entry(L"alpha"), entry(L"echo"), entry(L"delta"),
                                             entry(L"beta")};
    const std::vector<fs::DirEntry> fresh = sorted({entry(L"alpha", false, 42), entry(L"beta"),
                                                    entry(L"charlie"), entry(L"delta"),
                                                    entry(L"zulu")});
    const auto held = keep(shown, fresh, {{L"echo", L"zulu"}});
    Check(names(held) == L"alpha,charlie,zulu,delta,beta",
          L"order hold: rows keep their place, a renamed row its old one, new rows merge in sorted");
    Check(!held.empty() && held[0].size == 42u,
          L"order hold: kept rows take the fresh metadata");
    Check(names(keep(shown, fresh, {})) == L"alpha,charlie,delta,beta,zulu",
          L"order hold: without a rename hint the new name is just a new row");
    Check(names(keep({}, fresh, {})) == names(fresh),
          L"order hold: nothing shown yet means plain sort order");

    {
        const ScopedEntryGrouping grouping(static_cast<int>(GroupBy::Type));
        const std::vector<fs::DirEntry> shown_grouped = {entry(L"docs", true), entry(L"c.txt"),
                                                         entry(L"a.txt")};
        const auto fresh_grouped = sorted({entry(L"docs", true), entry(L"new", true),
                                           entry(L"z.md"), entry(L"a.txt"), entry(L"b.txt"),
                                           entry(L"c.txt")});
        Check(names(keep(shown_grouped, fresh_grouped, {})) == L"docs,new,z.md,b.txt,c.txt,a.txt",
              L"order hold: grouped, new rows merge in at their sort position");
        const auto fresh_moved = sorted({entry(L"docs", true), entry(L"a.md"), entry(L"c.txt")});
        Check(names(keep(shown_grouped, fresh_moved, {{L"a.txt", L"a.md"}})) == L"docs,a.md,c.txt",
              L"order hold: grouped, a rename into another group leaves its old row");
    }

    {
        std::vector<std::wstring> selected = {L"apple.txt", L"fig.txt", L"pending.txt"};
        std::wstring focus = L"apple.txt";
        FollowHeldRenames({{L"apple.txt", L"zebra.txt"}, {L"pending.txt", L"later.txt"}},
                          {entry(L"zebra.txt"), entry(L"fig.txt"), entry(L"pending.txt")},
                          selected, focus);
        Check(selected.size() == 3 && selected[0] == L"zebra.txt" && selected[1] == L"fig.txt" &&
                  selected[2] == L"pending.txt" && focus == L"zebra.txt",
              L"order hold: a selection captured before the rename follows the new name");
    }

    std::vector<EntryRename> renames = {{L"pending.txt", L"later.txt"}, {L"done.txt", L"new.txt"},
                                        {L"Case.txt", L"case.txt"}};
    PruneHeldRenames(renames, {entry(L"pending.txt"), entry(L"new.txt"), entry(L"case.txt")});
    Check(renames.size() == 1 && renames[0].old_name == L"pending.txt",
          L"order hold: renames the listing already reflects are dropped");
}

// #44: the sidebar scrollbar appears while scrolling or hovered, then fades.
void TestSidebarScrollbarFade() {
    const auto close_to = [](float a, float b) { return std::abs(a - b) < 0.001f; };
    ScrollbarFade fade;
    Check(!fade.Tick(1000) && fade.Opacity() == 0.0f && fade.Expand() == 0.0f &&
              !fade.Moving(1000),
          L"sidebar scrollbar: hidden and idle until something happens");
    fade.Reveal(1000);
    Check(fade.Moving(1000) && fade.Tick(1050) && close_to(fade.Opacity(), 0.5f),
          L"sidebar scrollbar: scrolling fades it in");
    fade.Tick(1100);
    Check(close_to(fade.Opacity(), 1.0f) && fade.Expand() == 0.0f,
          L"sidebar scrollbar: scrolling shows the thin thumb, not the track");
    Check(!fade.Tick(1900) && close_to(fade.Opacity(), 1.0f) && !fade.Moving(1500) &&
              fade.Moving(1900),
          L"sidebar scrollbar: holds for a moment after the last scroll");
    fade.Tick(2000);
    Check(close_to(fade.Opacity(), 0.75f), L"sidebar scrollbar: then fades out");
    fade.Tick(2400);
    Check(fade.Opacity() == 0.0f && !fade.Moving(2400),
          L"sidebar scrollbar: fully hidden after the fade");
    fade.Reveal(2500);
    fade.Tick(2550);
    fade.Reveal(3200);  // scrolling again restarts the hold
    fade.Tick(3500);
    Check(close_to(fade.Opacity(), 1.0f), L"sidebar scrollbar: each scroll restarts the hold");

    fade.SetHot(true, 5000);
    fade.Tick(5000);
    fade.Tick(5200);
    Check(close_to(fade.Opacity(), 1.0f) && close_to(fade.Expand(), 1.0f) && !fade.Moving(5200),
          L"sidebar scrollbar: hovering shows the full thumb and track");
    fade.Tick(9000);
    Check(close_to(fade.Opacity(), 1.0f) && close_to(fade.Expand(), 1.0f),
          L"sidebar scrollbar: stays while the pointer rests on it");
    fade.SetHot(false, 9000);
    fade.Tick(9060);
    Check(close_to(fade.Opacity(), 1.0f) && close_to(fade.Expand(), 0.5f),
          L"sidebar scrollbar: leaving collapses the track first");
    fade.Tick(10000);
    fade.Tick(10400);
    Check(fade.Opacity() == 0.0f && fade.Expand() == 0.0f,
          L"sidebar scrollbar: and fades out after the hold");
}

void TestSnapshotStorePutKeepsWorkerGeneration() {
    fs::SnapshotStore store(8);
    auto first = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry a;
    a.name = L"a.txt";
    first->push_back(a);
    store.Update(L"C:\\pulse-gen", 3, first);

    auto incremental = std::make_shared<std::vector<fs::DirEntry>>(*first);
    fs::DirEntry b;
    b.name = L"b.txt";
    incremental->push_back(b);
    Check(store.Put(L"C:\\pulse-gen", incremental) == 3,
          L"snapshot: Put keeps the last worker generation");

    auto worker = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry c;
    c.name = L"c.txt";
    worker->push_back(c);
    store.Update(L"C:\\pulse-gen", 4, worker);
    auto snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->size() == 1 && snap->front().name == L"c.txt",
          L"snapshot: later worker Update is not discarded after Put");

    store.Update(L"C:\\pulse-gen", 2, first);
    snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->front().name == L"c.txt",
          L"snapshot: older worker generation is still ignored");

    store.MarkDirty(L"C:\\pulse-gen");
    uint64_t gen = 0;
    Check(!store.GetOrStart(L"C:\\pulse-gen", gen) && gen == 4,
          L"snapshot: dirty GetOrStart does not bump past the worker generation");
    store.Update(L"C:\\pulse-gen", 5, first);
    snap = store.Peek(L"C:\\pulse-gen");
    Check(snap && snap->front().name == L"a.txt",
          L"snapshot: worker Update after dirty GetOrStart still applies");
}

void TestDataObject() {
    std::vector<std::wstring> paths{ L"C:\\fake_a.txt", L"D:\\fake_b.txt" };
    auto* obj = ui::FileDataObject::Create(paths);

    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&fmt) == S_OK, L"dnd: IDataObject advertises CF_HDROP");
    FORMATETC mixed{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                     TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE };
    Check(obj->QueryGetData(&mixed) == S_OK, L"dnd: tymed is a bit field");
    FORMATETC stream_only{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM };
    Check(obj->QueryGetData(&stream_only) != S_OK, L"dnd: stream-only HDROP is unsupported");
    FORMATETC fnamew{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILENAMEW), nullptr,
                      DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&fnamew) == S_OK, L"dnd: FileNameW advertised");
    FORMATETC idl{ (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST), nullptr,
                   DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&idl) == S_OK, L"dnd: Shell IDList advertised");
    STGMEDIUM med{};
    bool names_ok = false;
    if (SUCCEEDED(obj->GetData(&fmt, &med)) && med.hGlobal) {
        HDROP hd = static_cast<HDROP>(med.hGlobal);
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        wchar_t buf[MAX_PATH]{};
        names_ok = n == 2 &&
            DragQueryFileW(hd, 0, buf, MAX_PATH) && paths[0] == buf &&
            DragQueryFileW(hd, 1, buf, MAX_PATH) && paths[1] == buf;
        ReleaseStgMedium(&med);
    }
    Check(names_ok, L"dnd: CF_HDROP carries both paths in order");

    STGMEDIUM fnmed{};
    bool fn_ok = false;
    if (SUCCEEDED(obj->GetData(&fnamew, &fnmed)) && fnmed.hGlobal) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(fnmed.hGlobal))) {
            fn_ok = paths[0] == p;
            GlobalUnlock(fnmed.hGlobal);
        }
        ReleaseStgMedium(&fnmed);
    }
    Check(fn_ok, L"dnd: FileNameW is the first path");

    IEnumFORMATETC* en = nullptr;
    Check(SUCCEEDED(obj->EnumFormatEtc(DATADIR_GET, &en)) && en,
          L"dnd: EnumFormatEtc works");
    if (en) en->Release();

    // Target writes the performed effect back (Explorer move semantics).
    Check(ui::PreferredDropEffect(obj) == (DROPEFFECT_COPY | DROPEFFECT_MOVE),
          L"dnd: preferred effect advertised");
    obj->Release();
}

void TestClipboardText() {
    Check(ops::WriteClipboardText(L"C:\\A\\B"), L"clipboard: write text");
    bool ok = false;
    bool opened = false;
    for (int attempt = 0; attempt < 20 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened) Sleep(5);
    }
    if (opened) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                ok = (p == std::wstring(L"C:\\A\\B"));
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    Check(ok, L"clipboard: text round-trip");
}

void TestUniqueName() {
    CreateDirectoryW(kOpsTestRoot.c_str(), nullptr);
    CreateDirectoryW(kSandbox.c_str(), nullptr);
    CleanSandbox();
    std::wstring d = kSandbox;
    Check(UniqueChildName(d, L"新建文件夹", L"") == L"新建文件夹", L"new: first name");
    CreateDirectoryW((d + L"\\新建文件夹").c_str(), nullptr);
    Check(UniqueChildName(d, L"新建文件夹", L"") == L"新建文件夹 (2)", L"new: collision bumps index");
    Check(UniqueChildName(d, L"新建文本文档", L".txt") == L"新建文本文档.txt",
          L"new: text file extension");
}

void TestMultiSelect() {
    Tab tab;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(5);
    for (int i = 0; i < 5; ++i) {
        (*entries)[static_cast<size_t>(i)].name = L"f" + std::to_wstring(i);
        (*entries)[static_cast<size_t>(i)].size = static_cast<uint64_t>(i + 1) * 100;
    }
    tab.SetSnapshot(entries);

    tab.SelectOnly(1);
    Check(tab.IsSelected(1) && tab.SelectedCount() == 1 && tab.selected_index == 1,
          L"select: click replaces");

    tab.ToggleSelect(3);
    Check(tab.IsSelected(1) && tab.IsSelected(3) && tab.SelectedCount() == 2,
          L"select: ctrl-click adds");

    tab.ToggleSelect(1);
    Check(!tab.IsSelected(1) && tab.IsSelected(3) && tab.SelectedCount() == 1,
          L"select: ctrl-click removes");

    tab.SelectRange(0, 3);
    Check(tab.SelectedCount() == 4 && tab.IsSelected(0) && tab.IsSelected(3) && !tab.IsSelected(4),
          L"select: shift range");

    tab.SelectAll();
    Check(tab.all_selected && tab.SelectedCount() == 5 && tab.IsSelected(4) && tab.selected.empty(),
          L"select: ctrl-a uses all_selected flag");

    tab.MoveFocus(2, false);
    Check(tab.SelectedCount() == 1 && tab.selected_index == 2 && !tab.all_selected,
          L"select: move focus replaces");

    tab.MoveFocus(4, true);
    Check(tab.SelectedCount() == 3 && tab.IsSelected(2) && tab.IsSelected(4) && tab.selected_index == 4,
          L"select: shift-extend");

    tab.SelectOnly(0);
    tab.ToggleSelect(2);
    tab.AddRange(2, 4);
    Check(tab.SelectedCount() == 4 && tab.IsSelected(0) && !tab.IsSelected(1) && tab.IsSelected(4) &&
          tab.selected_index == 4 && tab.selection_anchor == 2,
          L"select: ctrl-shift adds a range to the selection");
    tab.ToggleSelect(4);
    Check(!tab.IsSelected(4) && tab.HasCaret() && tab.FocusIndex() == 4 && tab.selection_anchor == 4,
          L"select: ctrl-click keeps focus on the item it deselected");
    tab.SetCaret(1);
    Check(tab.FocusIndex() == 1 && !tab.IsSelected(1) && tab.SelectedCount() == 3,
          L"select: ctrl-arrow moves focus without selecting");
    tab.SelectOnly(3);
    Check(!tab.HasCaret() && tab.FocusIndex() == 3, L"select: a new selection drops the moved focus");

    tab.ClearSelection();
    Check(tab.SelectedCount() == 0 && !tab.IsSelected(0) && tab.selected_index < 0,
          L"select: escape clears");

    tab.FocusFirstEntry();
    Check(tab.HasFocusHint() && tab.SelectedCount() == 0 && tab.selected_index < 0 && !tab.IsSelected(0),
          L"select: an opened folder focuses its first row without selecting it");
    tab.MoveFocus(0, false);
    Check(!tab.HasFocusHint() && tab.IsSelected(0) && tab.SelectedCount() == 1,
          L"select: the first arrow press selects the focused row");
    tab.ClearSelection();
    Check(!tab.HasFocusHint(), L"select: escape does not bring the focus hint back");

    tab.SelectOnly(0);
    tab.ToggleSelect(2);
    tab.ToggleSelect(4);
    std::vector<std::wstring> names{ L"f0", L"f2", L"f4" };
    auto resorted = std::make_shared<std::vector<fs::DirEntry>>(5);
    for (int i = 0; i < 5; ++i) {
        (*resorted)[static_cast<size_t>(i)].name = L"f" + std::to_wstring(4 - i);
    }
    tab.SetSnapshot(resorted);
    tab.RemapSelection(names, L"f2");
    Check(tab.IsSelected(0) && tab.IsSelected(2) && tab.IsSelected(4)
          && tab.selected_index == 2 && tab.SelectedCount() == 3,
          L"select: remap by name after sort");

    ui::PaneViewModel pane;
    pane.selected_index = 1;
    pane.all_selected = true;
    pane.selected_count = 5;
    Check(pane.IsRowSelected(0) && pane.IsRowSelected(4), L"select: view-model all_selected");
    pane.all_selected = false;
    std::unordered_set<int> selected_indices{ 3 };
    pane.selected_indices = &selected_indices;
    Check(pane.IsRowSelected(1) && pane.IsRowSelected(3) && !pane.IsRowSelected(0),
          L"select: view-model mixed set + focus");

    Check(NameMatchesPattern(L"photo.JPG", L"*.jpg") &&
          NameMatchesPattern(L"a1.txt", L"a?.txt") &&
          !NameMatchesPattern(L"photo.png", L"*.jpg") &&
          NameMatchesPattern(L"Report.pdf", L"report"),
          L"select: wildcard and substring name matching");

    auto mixed = std::make_shared<std::vector<fs::DirEntry>>(4);
    (*mixed)[0].name = L"keep.txt";
    (*mixed)[1].name = L"shot.jpg";
    (*mixed)[2].name = L"shot.JPG";
    (*mixed)[3].name = L"notes.md";
    tab.SetSnapshot(mixed);
    tab.SelectOnly(1);
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.IsSelected(0) && !tab.IsSelected(1) && tab.IsSelected(2) && tab.IsSelected(3) &&
          tab.SelectedCount() == 3,
          L"select: invert listing");

    tab.SelectOnly(0);
    tab.InvertIndices({1, 2});
    Check(tab.IsSelected(0) && tab.IsSelected(1) && tab.IsSelected(2) && !tab.IsSelected(3),
          L"select: invert only the visible subset");

    tab.filter_text = L"*.jpg";
    std::vector<int> jpg;
    CollectFilterMatches(tab, nullptr, jpg);
    Check(jpg.size() == 2 && jpg[0] == 1 && jpg[1] == 2, L"select: filter wildcard *.jpg");
    tab.SelectIndices(jpg);
    Check(tab.IsSelected(1) && tab.IsSelected(2) && !tab.IsSelected(0) && tab.SelectedCount() == 2,
          L"select: select matches *.jpg");
    tab.filter_text.clear();

    tab.SelectAll();
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.SelectedCount() == 0, L"select: invert of all_selected clears");
}

void TestHiddenFiles() {
    AppPrefs prefs;
    Check(prefs.FromJson(L"{}") && !prefs.show_hidden_files,
          L"hidden: old preferences default to hidden off");
    Check(!prefs.show_protected_os_files,
          L"hidden: protected operating system files start hidden");
    prefs.show_hidden_files = true;
    prefs.show_protected_os_files = true;
    AppPrefs loaded;
    Check(loaded.FromJson(prefs.ToJson()) && loaded.show_hidden_files &&
          loaded.show_protected_os_files,
          L"hidden: preference JSON roundtrip");
    Pane pane;
    auto& tab = pane.view;
    tab.current_path = L"\\\\server\\share";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(4);
    (*entries)[0].name = L"visible.txt";
    (*entries)[1].name = L"desktop.ini";
    (*entries)[1].attrs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    (*entries)[2].name = L".ordinary";
    (*entries)[3].name = L"hidden-folder";
    (*entries)[3].attrs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY;
    (*entries)[3].is_dir = true;
    tab.SetSnapshot(entries);
    ui::PaneViewModel vm;
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 2 && vm.SourceIndex(1) == 2 && vm.ViewIndex(1) == -1,
          L"hidden: cached UNC attributes filter files and folders, not dot names");
    const auto cached = vm.filter_map;
    FillPaneViewModel(vm, pane);
    Check(vm.filter_map == cached && tab.snapshot == entries,
          L"hidden: visibility reuses map and preserves the complete snapshot");
    tab.SelectRange(0, 2);
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: range selection cannot include invisible files");
    tab.SelectAll();
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: select all excludes invisible files");
    tab.SetShowHiddenFiles(true);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 3 && tab.SelectedCount() == 0,
          L"hidden: toggle restores hidden files and still gates protected system files");
    tab.SetShowProtectedOsFiles(true);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 4,
          L"hidden: protected system files follow their own option");
    tab.SetShowProtectedOsFiles(false);
    tab.SelectOnly(1);
    tab.SetShowHiddenFiles(false);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 2 && tab.SelectedCount() == 0 && tab.file_count == 2,
          L"hidden: toggle back resets selection and visible counts");
    auto changed = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    tab.SelectOnly(0);
    (*changed)[0].attrs = FILE_ATTRIBUTE_HIDDEN;
    tab.SetSnapshot(changed);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 1 && tab.SelectedCount() == 0,
          L"hidden: watcher attributes invalidate visibility and stale selection");
    tab.SetSnapshot(entries);
    tab.InvertIndices({0, 1, 2, 3});
    Check(tab.SelectedIndices() == std::vector<int>({0, 2}),
          L"hidden: inverse selection excludes hidden entries");
    tab.filter_text = L"*.ini";
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 0, L"hidden: name filtering composes with hidden filtering");
    tab.filter_text.clear();
    tab.current_path = L"pulse:recycle";
    tab.SetSnapshot(entries);
    FillPaneViewModel(vm, pane);
    Check(vm.EntryCount() == 4, L"hidden: recycle payloads remain accessible");
}

void TestSplitLayout() {
    Pane a, b, c, d;
    std::vector<Pane*> two{ &a, &b };
    auto tree = MakePresetTree(LayoutPreset::TwoVertical, two);
    Check(tree && !tree->is_leaf, L"layout: two-vertical is a split");
    std::vector<Pane*> vis;
    tree->CollectPanes(vis);
    Check(vis.size() == 2 && vis[0] == &a && vis[1] == &b, L"layout: two-vertical leaves");

    D2D1_RECT_F bounds = D2D1::RectF(0, 0, 1000, 800);
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    LayoutSplitTree(*tree, bounds, 8.0f, laid);
    Check(laid.size() == 2, L"layout: two leaves placed");
    Check(laid.size() == 2 && laid[0].second.right <= laid[1].second.left + 0.1f,
          L"layout: left pane is left of right");

    std::vector<SplitterLayout> splitters;
    laid.clear();
    LayoutSplitTree(*tree, bounds, 8.0f, laid, &splitters);
    Check(splitters.size() == 1 && splitters[0].orientation == SplitOrientation::Vertical,
          L"layout: two-vertical exposes a vertical splitter");
    ApplySplitRatio(*tree, bounds, 8.0f, 250.0f, 400.0f);
    laid.clear();
    LayoutSplitTree(*tree, bounds, 8.0f, laid);
    Check(laid.size() == 2 && laid[0].second.right < 300.0f &&
          laid[1].second.left > laid[0].second.right,
          L"layout: dragging splitter resizes panes");
    std::vector<float> ratios;
    CollectSplitRatios(*tree, ratios);
    Check(ratios.size() == 1 && ratios[0] < 0.35f, L"layout: collect dragged split ratio");
    auto restored = MakePresetTree(LayoutPreset::TwoVertical, two);
    ApplySplitRatios(*restored, ratios);
    laid.clear();
    LayoutSplitTree(*restored, bounds, 8.0f, laid);
    Check(laid.size() == 2 && laid[0].second.right < 300.0f,
          L"layout: apply saved split ratio");

    std::vector<Pane*> four{ &a, &b, &c, &d };
    auto grid = MakePresetTree(LayoutPreset::FourGrid, four);
    vis.clear();
    grid->CollectPanes(vis);
    Check(vis.size() == 4, L"layout: four-grid has 4 leaves");
    Check(LayoutPresetCount(LayoutPreset::Three) == 3, L"layout: three count");

    auto splitMenu = BuildSplitMenu(1);
    Check(splitMenu.size() >= 5 && splitMenu[1].command == CmdLayoutTwoVertical,
          L"menu: split presets");
    Check(splitMenu[1].radio && splitMenu[0].radio_group && !splitMenu[0].radio,
          L"menu: split radio marks current preset on the leading edge");
    Check(splitMenu[1].text.find(L"●") == std::wstring::npos,
          L"menu: split selection is not a text bullet");
    Check(splitMenu[0].pictogram == ui::fluent::MenuPictogram::LayoutSingle &&
          splitMenu[1].pictogram == ui::fluent::MenuPictogram::LayoutSideBySide &&
          splitMenu[2].pictogram == ui::fluent::MenuPictogram::LayoutStacked &&
          splitMenu[3].pictogram == ui::fluent::MenuPictogram::LayoutThree &&
          splitMenu[4].pictogram == ui::fluent::MenuPictogram::LayoutFour,
          L"menu: split presets use distinct layout pictograms");
    // The palette's command rows come from the action registry; these views
    // stand in for ListActions (an app state is not needed for the rows).
    namespace act = app::actions;
    std::vector<act::ActionView> fake_actions;
    auto add_fake = [&](std::string_view id, const wchar_t* title, const wchar_t* domain,
                        bool enabled = true) -> act::ActionView& {
        act::ActionView v;
        v.index = static_cast<int>(fake_actions.size());
        v.id = id;
        v.title = title;
        v.domain = domain;
        v.enabled = enabled;
        if (!enabled) v.why = L"先选中一个文件";
        fake_actions.push_back(std::move(v));
        return fake_actions.back();
    };
    {
        auto& layout = add_fake("window.layout", L"分栏布局", L"窗口");          // 0
        for (const wchar_t* choice : { L"单栏", L"左右两栏", L"四宫格" }) {
            ui::FluentMenuItem row;
            row.command = CmdLayoutSingle;
            row.text = choice;
            layout.choices.push_back(std::move(row));
        }
        layout.choices[2].command = CmdLayoutFourGrid;
    }
    add_fake("file.new_folder", L"新建文件夹", L"文件");                          // 1
    add_fake("tab.new", L"新建标签页", L"标签页").shortcut = L"Ctrl+T";          // 2
    add_fake("file.rename", L"重命名", L"文件", false);                           // 3
    add_fake("file.batch_rename", L"批量重命名", L"文件");                        // 4
    add_fake("app.exit", l10n::Get(l10n::StringId::ExitPulse).c_str(), L"应用");  // 5
    add_fake("settings.set", L"显示隐藏的文件", L"设置 · 文件列表").key = "show_hidden";  // 6
    const auto mixed_rows = [&](const wchar_t* needle) {
        return act::PaletteRows(fake_actions, needle, act::PaletteMode::Mixed, {});
    };
    const auto command_rows = [&](const wchar_t* needle, const std::vector<int>& used = {}) {
        return act::PaletteRows(fake_actions, needle, act::PaletteMode::Commands, used);
    };
    auto pal = BuildCommandPalette(L"", {}, { L"C:\\Users" }, {}, false);
    Check(!pal.empty() && pal.back().command == CmdRecentBase, L"menu: command palette recent");
    Check(pal.back().badge_text == L"历史路径" && pal.back().shortcut.empty(),
          L"menu: recent rows use 历史路径 badge");
    auto palQ = BuildCommandPalette(L"四宫", mixed_rows(L"四宫"), { L"C:\\Users" }, {}, false);
    Check(!palQ.empty() && palQ[0].text == L"分栏布局" && palQ[0].children.size() == 3 &&
          palQ[0].children[2].command == CmdLayoutFourGrid,
          L"menu: a choice's label finds its action (四宫 → 分栏布局 ›)");
    {
        bool offers_exit = false;
        for (const auto& it : command_rows(l10n::Get(l10n::StringId::ExitPulse).c_str()))
            offers_exit = offers_exit || it.command == act::PaletteCommand(5);
        Check(offers_exit, L"menu: the palette offers Exit Pulse (#57)");
    }
    {
        const auto rows = command_rows(L"xj");
        bool initials = false;
        for (const auto& it : rows)
            if (it.text == L"新建文件夹") initials = it.match_start == 0 && it.match_length == 2;
        Check(initials, L"palette: pinyin initials match and mark the matched characters");
        const auto renames = command_rows(L"重命名");
        Check(renames.size() == 4 && renames[0].heading && renames[0].text == L"文件" &&
              renames[1].text == L"批量重命名" && renames[1].match_start == 2 &&
              renames[1].match_length == 3 && renames[2].heading && renames[3].text == L"重命名" &&
              !renames[3].enabled && renames[3].shortcut == L"先选中一个文件",
              L"palette: unavailable actions come last, under their heading, with the reason");
        const auto none = command_rows(L"把上个月的截图挪到图片");
        Check(none.size() == 2 && !none[0].enabled && none[1].command == act::kPaletteAskAssistant &&
              none[1].text.find(L"把上个月的截图挪到图片") != std::wstring::npos,
              L"palette: no match offers the assistant with the words");
        const auto start = command_rows(L"", { 4 });
        Check(start.size() >= 4 && start[0].heading && start[1].command == act::PaletteCommand(4) &&
              start[2].heading,
              L"palette: an empty > lists this session's actions, then 常用");
        Check(mixed_rows(L"").empty() && mixed_rows(L"新建").size() == 2,
              L"palette: mixed mode adds only the best few commands");
    }
    auto palLong = BuildCommandPalette(L"", {}, { L"\\\\?\\C:\\Users\\TestUser" }, {}, false);
    bool recent_ok = false;
    for (const auto& it : palLong) {
        if (it.command != CmdRecentBase) continue;
        Check(it.shortcut.find(L"\\\\?\\") == std::wstring::npos,
              L"menu: palette recent must not keep \\\\?\\ prefix");
        Check(it.badge_text == L"历史路径", L"menu: long path recent uses badge");
        Check(it.text == L"TestUser", L"menu: recent title is folder name");
        recent_ok = true;
    }
    Check(recent_ok, L"menu: palette shows Windows-style path title");

    const auto qDot = ParseOmnibarQuery(L".codex");
    Check(qDot.kind == OmnibarQuery::Kind::Mixed && qDot.needle == L".codex" &&
          !LooksLikeFilesystemPath(qDot.needle), L"omnibar: dot folder uses filename search");
    const auto qDotCommand = ParseOmnibarQuery(L">.codex");
    Check(qDotCommand.kind == OmnibarQuery::Kind::Command,
          L"omnibar: explicit command prefix remains supported");
    const auto qDotProject = ParseOmnibarQuery(L".codex", true);
    Check(qDotProject.kind == OmnibarQuery::Kind::Project,
          L"omnibar: project scope preserved");
    const auto qCmd = ParseOmnibarQuery(L">新建");
    Check(qCmd.kind == OmnibarQuery::Kind::Command && qCmd.needle == L"新建" && qCmd.prefix == L'>',
          L"omnibar: > prefix is command");
    const auto qSearch = ParseOmnibarQuery(L"?foo");
    Check(qSearch.kind == OmnibarQuery::Kind::Search && qSearch.needle == L"foo",
          L"omnibar: ? prefix is search");
    const auto qSlash = ParseOmnibarQuery(L"/bar");
    Check(qSlash.kind == OmnibarQuery::Kind::Search && qSlash.needle == L"bar" && qSlash.prefix == L'/',
          L"omnibar: / prefix is search alias");
    const auto qPath = ParseOmnibarQuery(L"C:\\Windows");
    Check(qPath.kind == OmnibarQuery::Kind::Mixed && qPath.needle == L"C:\\Windows",
          L"omnibar: bare path has no prefix");
    Check(LooksLikeFilesystemPath(L"C:\\Windows") && LooksLikeFilesystemPath(L"\\\\server\\share") &&
          !LooksLikeFilesystemPath(L"新建文件夹"),
          L"omnibar: path-like detection");
    auto palCmd = BuildCommandPalette(L">新建", command_rows(L"新建"), { L"C:\\新建" }, {}, false);
    bool cmd_has_folder = false, cmd_has_layout = false, cmd_has_recent = false;
    for (const auto& it : palCmd) {
        cmd_has_folder = cmd_has_folder || it.command == act::PaletteCommand(1);
        cmd_has_layout = cmd_has_layout || it.text == L"分栏布局";
        cmd_has_recent = cmd_has_recent || it.command == CmdRecentBase;
    }
    Check(cmd_has_folder, L"omnibar: >新建 matches 新建文件夹");
    Check(!cmd_has_layout, L"omnibar: >新建 does not keep unrelated commands");
    Check(!cmd_has_recent, L"omnibar: the > palette lists commands only");
    auto palSearch = BuildCommandPalette(L"?foo", mixed_rows(L"新建"), { L"C:\\Users" }, {}, false);
    bool search_has_cmd = false;
    for (const auto& it : palSearch)
        if (it.command >= act::kPaletteCmdBase && it.command < act::kPaletteCmdEnd) search_has_cmd = true;
    Check(!search_has_cmd, L"omnibar: ? search hides commands");
    auto palDup = BuildCommandPalette(L"", {}, { L"C:\\A", L"C:\\A", L"C:\\B" }, {}, false, 0, L"C:\\B");
    int recent_n = 0;
    int recent_idx = -1;
    for (const auto& it : palDup) {
        if (it.command >= CmdRecentBase && it.command < CmdIndexBase) {
            ++recent_n;
            recent_idx = it.command - CmdRecentBase;
        }
    }
    Check(recent_n == 1 && recent_idx == 0, L"omnibar: recents dedupe and skip current path");
    index::Hit hit;
    hit.name = L"Users";
    hit.path = L"C:\\Users";
    hit.is_dir = true;
    auto palSkipHit = BuildCommandPalette(L"", {}, { L"C:\\Users" }, { hit }, false);
    bool recent_after_hit = false;
    for (const auto& it : palSkipHit)
        if (it.command == CmdRecentBase) recent_after_hit = true;
    Check(!recent_after_hit, L"omnibar: recent skipped when index already listed the path");

    ui::MainRenderer chrome;
    chrome.SetScale(1.0f);
    ui::WindowViewModel chrome_vm;
    ui::TabView chrome_tab;
    chrome_tab.title = L"Test";
    chrome_tab.active = true;
    chrome_vm.tabs.push_back(chrome_tab);
    Check(std::abs(chrome.TitleBarHeight() - ui::kTitleBarHeight) < 0.01f,
          L"chrome: title bar uses kTitleBarHeight");
    D2D1_RECT_F tab_rc{};
    Check(chrome.TabItemRect(chrome_vm, 1400.0f, 0, &tab_rc) &&
          std::abs((tab_rc.bottom - tab_rc.top) - (ui::kTitleBarHeight - 8.0f)) < 0.01f,
          L"chrome: tab height is title bar minus 8dip padding");
    Check(tab_rc.right - tab_rc.left > 170.0f,
          L"chrome: a single tab can grow past the old 176px cap");

    ui::WindowViewModel pinned_vm;
    ui::TabView pinned_tab;
    pinned_tab.title = L"Pinned";
    pinned_tab.active = true;
    pinned_tab.pinned = true;
    pinned_vm.tabs.push_back(pinned_tab);
    D2D1_RECT_F pinned_rc{};
    Check(chrome.TabItemRect(pinned_vm, 758.0f, 0, &pinned_rc) &&
          std::abs((pinned_rc.right - pinned_rc.left) - 112.0f) < 0.01f,
          L"chrome: pinned tab shows a compact name by default");
    const auto named_hit = chrome.HitTest(pinned_vm, D2D1::RectF(0, 0, 758, 269),
        pinned_rc.right - 1.0f, (pinned_rc.top + pinned_rc.bottom) * 0.5f);
    Check(named_hit.region == ui::HitTestResult::Tab,
          L"chrome: named pinned tab has no close affordance");
    pinned_vm.show_pinned_tab_names = false;
    Check(chrome.TabItemRect(pinned_vm, 758.0f, 0, &pinned_rc) &&
          std::abs((pinned_rc.right - pinned_rc.left) - 36.0f) < 0.01f,
          L"chrome: pinned tab keeps its icon-only width");
    const float pinned_y = (pinned_rc.top + pinned_rc.bottom) * 0.5f;
    const auto pinned_hit = chrome.HitTest(pinned_vm, D2D1::RectF(0, 0, 758, 269),
                                           pinned_rc.right - 1.0f, pinned_y);
    const auto new_tab_hit = chrome.HitTest(pinned_vm, D2D1::RectF(0, 0, 758, 269),
                                            pinned_rc.right + 5.0f, pinned_y);
    Check(pinned_hit.region == ui::HitTestResult::Tab &&
          new_tab_hit.region == ui::HitTestResult::TabNew,
          L"chrome: pinned tab and new-tab button do not overlap");

    // Crowded strips: tabs, then "+", then the settings pill, never overlapping.
    auto crowded = [](int count, int active) {
        ui::WindowViewModel vm;
        for (int i = 0; i < count; ++i) {
            ui::TabView t;
            t.title = L"Tab " + std::to_wstring(i);
            t.active = i == active;
            vm.tabs.push_back(t);
        }
        return vm;
    };
    const float crowd_w = 1200.0f;
    const D2D1_RECT_F crowd_rect = D2D1::RectF(0, 0, crowd_w, 640);
    for (const int count : {1, 6, 12, 40}) {
        const auto vm = crowded(count, count - 1);
        D2D1_RECT_F first{};
        chrome.TabItemRect(vm, crowd_w, 0, &first);
        const float mid_y = (first.top + first.bottom) * 0.5f;
        float last_tab = -1.0f, new_left = -1.0f, new_right = -1.0f, settings_left = -1.0f;
        for (float x = 0.0f; x < crowd_w; x += 1.0f) {
            const auto scan_hit = chrome.HitTest(vm, crowd_rect, x, mid_y);
            const auto region = scan_hit.region;
            if (region == ui::HitTestResult::Tab || region == ui::HitTestResult::TabClose) last_tab = x;
            if (region == ui::HitTestResult::TabNew) { if (new_left < 0.0f) new_left = x; new_right = x; }
            if (region == ui::HitTestResult::SettingsButton && settings_left < 0.0f) settings_left = x;
        }
        Check(new_left > last_tab && new_right < settings_left && settings_left > 0.0f,
              (L"chrome: " + std::to_wstring(count) + L" tabs stay left of new-tab and settings").c_str());
    }
    {
        auto vm = crowded(40, 30);
        D2D1_RECT_F active_rc{}, other_rc{};
        Check(chrome.TabItemRect(vm, crowd_w, 30, &active_rc) && chrome.TabItemRect(vm, crowd_w, 29, &other_rc) &&
              std::abs((active_rc.right - active_rc.left) - 120.0f) < 0.01f &&
              std::abs((other_rc.right - other_rc.left) - 96.0f) < 0.01f,
              L"chrome: crowded tabs stop at the minimum width while the active tab keeps room for close");
        const float mid_y = (active_rc.top + active_rc.bottom) * 0.5f;
        const auto active_hit = chrome.HitTest(vm, crowd_rect, (active_rc.left + active_rc.right) * 0.5f, mid_y);
        Check(active_hit.region == ui::HitTestResult::Tab && active_hit.index == 30,
              L"chrome: scrolling strip reveals the active tab");
        const auto other_hit = chrome.HitTest(vm, crowd_rect, (other_rc.left + other_rc.right) * 0.5f, mid_y);
        Check(other_hit.region == ui::HitTestResult::Tab && other_hit.index == 29,
              L"chrome: icon-only tabs have no close button");
        D2D1_RECT_F strip_start{}, first{};
        chrome.TabItemRect(crowded(1, 0), crowd_w, 0, &strip_start);
        const float view_left = strip_start.left + 24.0f; // kTabScrollArrowW
        chrome.TabItemRect(vm, crowd_w, 0, &first);
        const auto left_arrow = chrome.HitTest(vm, crowd_rect, strip_start.left + 12.0f, mid_y);
        Check(left_arrow.region == ui::HitTestResult::TabScroll && left_arrow.index == -1 && first.right <= view_left,
              L"chrome: earlier tabs are scrolled behind the left arrow");
        Check(chrome.ScrollTabStrip(vm, crowd_w, 400.0f, mid_y, 100.0f) &&
              chrome.TabItemRect(vm, crowd_w, 0, &first) && first.left >= view_left - 0.01f,
              L"chrome: wheel scrolls the strip back to the first tab");
        Check(!chrome.ScrollTabStrip(crowded(3, 0), crowd_w, 400.0f, mid_y, 1.0f),
              L"chrome: a strip that fits ignores the wheel");
    }
    {
        // Tabs below the maximum width, where a close would otherwise reflow them.
        int count = 4;
        for (D2D1_RECT_F probe{}; count < 30; ++count) {
            chrome.TabItemRect(crowded(count, count - 1), 1400.0f, 0, &probe);
            if (probe.right - probe.left < 239.0f) break;
        }
        auto vm = crowded(count, count - 1);
        D2D1_RECT_F before{}, frozen{}, released{};
        chrome.TabItemRect(vm, 1400.0f, 0, &before);
        chrome.FreezeTabWidths(vm, 1400.0f);
        vm.tabs.pop_back();
        vm.tabs.back().active = true;
        chrome.TabItemRect(vm, 1400.0f, 0, &frozen);
        Check(std::abs((frozen.right - frozen.left) - (before.right - before.left)) < 0.01f,
              L"chrome: closing a tab keeps tab widths while the pointer stays");
        Check(chrome.ReleaseTabWidths() && chrome.TabItemRect(vm, 1400.0f, 0, &released) &&
              released.right - released.left > frozen.right - frozen.left + 1.0f && !chrome.ReleaseTabWidths(),
              L"chrome: leaving the strip lets the remaining tabs grow");
    }

    ui::PaneViewModel pvm;
    pvm.filter_text = L"foo";
    pvm.filter_map = std::make_shared<ui::PaneViewModel::FilterMap>(
        ui::PaneViewModel::FilterMap{ 2, 5, 9 });
    Check(pvm.EntryCount() == 3 && pvm.SourceIndex(1) == 5 && pvm.ViewIndex(9) == 2,
          L"filter: view/source index mapping");
}

// 兼容打开方式 (settings index 9): the folder verb stays unregistered while the
// scope stays selected, the state reads healthy, and the choice survives a save.
// persist = false keeps every Apply* away from the registry.
// get_state, the selector and waiting (actions/action_state.h), on panes
// filled by hand; the wait for a finished task runs a real copy in the sandbox.
namespace {

uint64_t AsLocalTicks(int y, int m, int d, int h, int mi) {
    SYSTEMTIME local{}, utc{};
    local.wYear = static_cast<WORD>(y);
    local.wMonth = static_cast<WORD>(m);
    local.wDay = static_cast<WORD>(d);
    local.wHour = static_cast<WORD>(h);
    local.wMinute = static_cast<WORD>(mi);
    FILETIME ft{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    SystemTimeToFileTime(&utc, &ft);
    return app::actions::FileTimeTicks(ft);
}

fs::DirEntry AsEntry(const wchar_t* name, uint64_t size, uint64_t mtime, bool dir = false, DWORD attrs = 0) {
    fs::DirEntry e;
    e.name = name;
    e.size = size;
    e.mtime.dwLowDateTime = static_cast<DWORD>(mtime & 0xFFFFFFFFu);
    e.mtime.dwHighDateTime = static_cast<DWORD>(mtime >> 32);
    e.is_dir = dir;
    e.attrs = attrs | (dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    return e;
}

int64_t AsInt(const json::Value* v) {
    int64_t n = -1;
    if (v) v->get_int(n, -(int64_t{1} << 53), int64_t{1} << 53);
    return n;
}

const json::Value& AsPane(const json::Value& state, size_t i) {
    static const json::Value none;
    const json::Value* panes = state.find("panes");
    return panes && i < panes->items().size() ? panes->items()[i] : none;
}

std::vector<int> AsPick(AppState& s, const char* selector_json, uint64_t now, std::string* reason = nullptr) {
    namespace act = app::actions;
    json::Value args;
    act::Selector sel;
    std::string error;
    if (!json::Parse(selector_json, args) || !act::ParseSelector(args, now, sel, error)) return {-99};
    const act::SelectorMatch m = act::ResolveSelector(s, sel);
    if (reason) *reason = m.reason;
    act::ApplySelection(s, m);
    return m.matched;
}

// Runs the message loop until `done` or `ms` pass; `tick` runs each turn.
bool AsPump(const std::function<bool()>& done, DWORD ms, const std::function<void(ULONGLONG)>& tick = {}) {
    const ULONGLONG start = GetTickCount64();
    while (!done()) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed > ms) return false;
        if (tick) tick(elapsed);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return true;
}

} // namespace

// P3: the assistant's gate per action and find_actions' rows
// (plans/ai/P3_AI接入动作表.html).
void TestActionPolicy() {
    namespace act = app::actions;
    const auto gate = [](std::string_view id, std::wstring_view target = {}) { return act::AiPolicy(id, target).gate; };
    Check(gate("view.sort") == act::Gate::Direct, L"policy: a view action runs at once");
    Check(gate("tag.toggle") == act::Gate::Direct, L"policy: organizing runs at once");
    Check(gate("settings.set") == act::Gate::Direct, L"policy: settings run at once (the card says old -> new)");
    Check(gate("file.new_folder") == act::Gate::Direct, L"policy: a new folder runs at once and undoes");
    Check(gate("file.move_to") == act::Gate::Plan && gate("file.rename") == act::Gate::Plan,
          L"policy: moves and renames show a plan first");
    Check(gate("file.recycle") == act::Gate::Plan && gate("recycle.empty") == act::Gate::Plan,
          L"policy: deleting shows its card first");
    Check(gate("app.exit") == act::Gate::Locked, L"policy: exiting Pulse is the user's only");
    Check(gate("no.such.action") == act::Gate::Locked, L"policy: unknown ids are locked");
    Check(gate("index.install_full") == act::Gate::Confirm, L"policy: a system action asks first");
    Check(gate("nav.open", L"C:\\AsT\\setup.exe") == act::Gate::Confirm &&
              gate("nav.open", L"C:\\AsT\\run.PS1") == act::Gate::Confirm &&
              gate("nav.open", L"C:\\AsT\\app.lnk") == act::Gate::Confirm,
          L"policy: programs, scripts and shortcuts ask first");
    Check(gate("nav.open", L"C:\\AsT\\report.pdf") == act::Gate::Direct &&
              gate("nav.open", L"C:\\AsT\\photo.jpg") == act::Gate::Direct,
          L"policy: documents and media open at once");
    Check(act::IsProgramPath(L"x.msi") && act::IsProgramPath(L"x.BAT") && !act::IsProgramPath(L"x.txt") &&
              !act::IsProgramPath(L"exe"),
          L"policy: program extensions");

    // find_actions: the best rows first, runnable before the rest.
    std::vector<act::ActionView> views;
    auto add = [&](std::string_view id, const wchar_t* title, bool enabled = true) -> act::ActionView& {
        act::ActionView v;
        v.index = static_cast<int>(views.size());
        v.id = id;
        v.title = title;
        v.enabled = enabled;
        if (!enabled) v.why = L"先选中一个文件";
        views.push_back(std::move(v));
        return views.back();
    };
    add("details.toggle", L"详情面板").value = L"关";
    add("view.sort", L"排序方式");
    add("file.rename", L"重命名", false);
    add("settings.set", L"显示文件夹大小").key = "size_bars";
    add("settings.set", L"显示隐藏的文件").key = "show_hidden";
    add("settings.set", L"列表行高").key = "row_height";
    add("tab.close_others", L"关闭其他标签页", false);
    {
        const auto hits = act::FindActions(views, L"详情面板", 8);
        Check(!hits.empty() && hits.front()->id == "details.toggle", L"find_actions: a title finds its action");
    }
    {
        const auto hits = act::FindActions(views, L"文件夹大小", 8);
        Check(!hits.empty() && hits.front()->key == "size_bars", L"find_actions: a setting is found by the words naming it");
    }
    {
        const auto hits = act::FindActions(views, L"文件夹大小 列表", 8);
        Check(!hits.empty() && hits.front()->key == "size_bars", L"find_actions: the longer, more specific word ranks first");
    }
    Check(act::FindActions(views, L"zzqqxx", 8).empty(), L"find_actions: nothing for unrelated words");
    Check(act::FindActions(views, L"详情 排序 重命名 文件", 2).size() <= 2, L"find_actions: at most max rows");
    {
        const json::Value row = act::ActionJson(views[0]);
        Check(row.get_string("id") == "details.toggle" && row.get_string("now") == "\xE5\x85\xB3" && row.find("args"),
              L"find_actions: a row has its id, current value and args");
        const json::Value off = act::ActionJson(views[6]);
        const json::Value* enabled = off.find("enabled");
        Check(enabled && enabled->is_bool() && !enabled->as_bool() && !off.get_string("why").empty(),
              L"find_actions: a row that cannot run says why");
        // A file step takes its items as arguments: the selection's state is
        // only what happens without them, not "cannot run".
        const json::Value file = act::ActionJson(views[2]);
        Check(!file.find("enabled") && file.get_string("without_args").find("items") != std::string::npos,
              L"find_actions: a file step without a selection still runs with items");
        const json::Value set = act::ActionJson(views[4]);
        Check(set.get_string("id") == "settings.set" && set.get_string("key") == "show_hidden" &&
                  set.get_string("values") == "true|false",
              L"find_actions: a setting row has its key and values");
    }
    {
        const json::Value spec = act::ArgSpecJson(act::ArgSpec("view.sort"));
        Check(spec.get_string("by") == "name|modified|type|size|created" && spec.get_string("desc") == "bool",
              L"find_actions: view.sort's args");
        Check(act::ArgSpec("window.overview").empty(), L"find_actions: an action without args has no spec");
    }
}

// P4: actions run with arguments (action_args.h) - the tab, tag, name or
// keys the model gave; each says what changed and undoes it.
// P4c: what a file step would do, before its card (action_files.h). Plans
// only: nothing is queued.
void TestFilePlans() {
    namespace act = app::actions;
    // Next to the exe: %TEMP% may be C:\Windows\Temp, which the plans refuse.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring base = exe;
    base.resize(base.find_last_of(L'\\') + 1);
    const std::wstring root = base + L"PulseFilePlans-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring a = root + L"\\a.txt", b = root + L"\\b.txt", sub = root + L"\\sub";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(sub.c_str(), nullptr);
    const auto write = [](const std::wstring& path, const char* text) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, text, static_cast<DWORD>(strlen(text)), &n, nullptr);
        CloseHandle(h);
    };
    write(a, "12345");
    write(b, "123");
    write(sub + L"\\x.txt", "x");
    write(sub + L"\\y.txt", "y");

    auto owner = std::make_unique<AppState>();
    AppState& s = *owner;
    s.places.persist = false;
    s.appPrefs.persist = false;
    s.isolatedTest = true;
    const auto with = [](std::initializer_list<std::wstring> items, const char* key = nullptr, const std::wstring& value = {}) {
        json::Value v = json::Value::Object();
        json::Value list = json::Value::Array();
        for (const auto& i : items) list.push(json::Utf8(i));
        v.set("items", std::move(list));
        if (key) v.set(key, json::Utf8(value));
        return v;
    };

    Check(act::FileOpOf("file.move_to") == act::FileOp::Move && act::FileOpOf("file.paste") == act::FileOp::Move &&
              act::FileOpOf("recycle.empty") == act::FileOp::EmptyBin && !act::IsFileOp("file.copy"),
          L"file plans: the file steps are known");
    Check(act::FileOpUndoes(act::FileOp::Recycle) && !act::FileOpUndoes(act::FileOp::Purge) &&
              !act::FileOpUndoes(act::FileOp::Restore) && !act::FileOpUndoes(act::FileOp::EmptyBin),
          L"file plans: permanent deletes and restores do not undo");
    Check(act::AiPolicy("file.delete_permanent").gate == act::Gate::Plan && act::AiPolicy("recycle.restore").gate == act::Gate::Plan &&
              act::FindAction("recycle.empty") != nullptr,
          L"file plans: the new rows show a plan first");

    act::FilePlan p = act::PlanFileOp(s, "file.move_to", with({a, b}, "dest", root + L"\\new"), nullptr);
    Check(p.ok && !p.skip && !p.dest_exists && p.count == 2 && p.bytes == 8 && p.files_only && p.rows.size() == 2 &&
              p.rows[0].name == L"a.txt" && p.rows[0].to == root + L"\\new",
          L"file plans: a move lists each item and makes a missing folder");
    p = act::PlanFileOp(s, "file.move_to", with({a}, "dest", root), nullptr);
    Check(p.ok && p.skip, L"file plans: a move into its own folder does nothing");
    p = act::PlanFileOp(s, "file.copy_to", with({sub}, "dest", sub + L"\\inner"), nullptr);
    Check(!p.ok && !p.why.empty(), L"file plans: a folder never goes into itself");
    p = act::PlanFileOp(s, "file.copy_to", with({a}, "dest", root + L"\\sub"), nullptr);
    Check(p.ok && p.dest_exists && p.clashes == 0, L"file plans: a copy into an existing folder");
    p = act::PlanFileOp(s, "file.move_to", with({a}), nullptr);
    Check(!p.ok && !p.why.empty(), L"file plans: no destination and no other pane is refused");

    p = act::PlanFileOp(s, "file.rename", with({a}, "name", L"b.txt"), nullptr);
    Check(!p.ok, L"file plans: a rename onto a taken name fails");
    p = act::PlanFileOp(s, "file.rename", with({a}, "name", L"bad:name"), nullptr);
    Check(!p.ok, L"file plans: a rename to an invalid name fails");
    p = act::PlanFileOp(s, "file.rename", with({a}, "name", L"a.txt"), nullptr);
    Check(p.ok && p.skip, L"file plans: the same name does nothing");
    p = act::PlanFileOp(s, "file.rename", with({a}, "name", L"c.txt"), nullptr);
    Check(p.ok && p.new_name == L"c.txt" && p.rows.size() == 1 && p.rows[0].to == L"c.txt", L"file plans: a rename");
    p = act::PlanFileOp(s, "file.rename", with({a, b}, "name", L"c.txt"), nullptr);
    Check(!p.ok, L"file plans: a rename is one item");

    p = act::PlanFileOp(s, "file.group_into_folder", with({a, b}, "name", L"sub"), nullptr);
    Check(!p.ok, L"file plans: a group into a taken name fails");
    p = act::PlanFileOp(s, "file.group_into_folder", with({a, b}, "name", L"grp"), nullptr);
    Check(p.ok && p.dest == root && p.new_name == L"grp" && p.count == 2, L"file plans: a group into a new folder");
    p = act::PlanFileOp(s, "file.dissolve", with({a}), nullptr);
    Check(!p.ok, L"file plans: only folders dissolve");
    p = act::PlanFileOp(s, "file.dissolve", with({sub}), nullptr);
    Check(p.ok && p.inner == 2 && p.dest == root, L"file plans: a dissolve counts what moves up");

    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, ARRAYSIZE(win));
    p = act::PlanFileOp(s, "file.move_to", with({std::wstring(win) + L"\\System32"}, "dest", root), nullptr);
    Check(!p.ok && !p.why.empty(), L"file plans: system folders are never touched");
    p = act::PlanFileOp(s, "file.copy_to", with({a}, "dest", std::wstring(win) + L"\\Temp"), nullptr);
    Check(!p.ok, L"file plans: nothing goes into a system folder");
    p = act::PlanFileOp(s, "file.recycle", with({std::wstring(win).substr(0, 3)}), nullptr);
    Check(!p.ok, L"file plans: a drive itself is refused");
    p = act::PlanFileOp(s, "file.recycle", with({root + L"\\missing.txt"}), nullptr);
    Check(!p.ok, L"file plans: a missing item is refused");
    p = act::PlanFileOp(s, "file.recycle", json::Value::Object(), nullptr);
    Check(!p.ok && !p.why.empty(), L"file plans: no items and no selection is refused");
    p = act::PlanFileOp(s, "file.recycle", with({a, b}), nullptr);
    Check(p.ok == app::ai::ReadBin(root[0]).available && (!p.ok || (p.count == 2 && p.bytes == 8 && p.bin_max > 0)),
          L"file plans: a recycle checks its drive's bin");
    Check(app::ai::ReadBin(root[0]).available || !ops::RecycleBinAvailable(root.substr(0, 3)),
          L"file plans: the bin check agrees with the delete's");
    {
        act::FilePlan q = act::PlanFileOp(s, "file.recycle", with({L"\\\\?\\" + a}), nullptr);
        Check(q.ok == p.ok && (q.sources.empty() || q.sources[0] == a),
              L"file plans: a \\\\?\\ path plans as the plain path");
    }
    p = act::PlanFileOp(s, "file.delete_permanent", with({a, sub}), nullptr);
    Check(p.ok && p.count == 2 && !p.files_only, L"file plans: a permanent delete lists its items");
    p = act::PlanFileOp(s, "recycle.restore", json::Value::Object(), nullptr);
    Check(!p.ok, L"file plans: a restore needs the Recycle Bin open");
    {
        // Emptying the bin: a plan only - it reads the bin and empties nothing.
        SHQUERYRBINFO before{};
        before.cbSize = sizeof(before);
        const bool read = SUCCEEDED(SHQueryRecycleBinW(nullptr, &before));
        const act::FilePlan e = act::PlanFileOp(s, "recycle.empty", json::Value::Object(), nullptr);
        Check(!read || (e.ok && e.op == act::FileOp::EmptyBin && e.count == static_cast<size_t>(before.i64NumItems) &&
                        e.skip == (before.i64NumItems == 0) && (!e.skip || !e.why.empty())),
              L"file plans: emptying the bin counts what is in it; an empty bin is nothing to do");
        SHQUERYRBINFO after{};
        after.cbSize = sizeof(after);
        Check(!read || (SUCCEEDED(SHQueryRecycleBinW(nullptr, &after)) && after.i64NumItems == before.i64NumItems),
              L"file plans: planning to empty the bin empties nothing");
        Check(act::McpGroupOf("recycle.empty") == act::McpGroup::Files &&
                  act::AiPolicy("recycle.empty").risk == act::ActionRisk::PermanentDelete,
              L"file plans: emptying the bin is a permanent delete in the files group");
    }

    for (const wchar_t* f : {L"\\a.txt", L"\\b.txt", L"\\sub\\x.txt", L"\\sub\\y.txt"}) DeleteFileW((root + f).c_str());
    RemoveDirectoryW(sub.c_str());
    RemoveDirectoryW(root.c_str());
}

void TestActionArgs() {
    namespace act = app::actions;
    auto owner = std::make_unique<AppState>();
    AppState& s = *owner;
    s.places.persist = false;
    s.appPrefs.persist = false;
    s.isolatedTest = true;
    const auto args = [](const char* text) {
        json::Value v;
        json::Parse(text, v);
        return v;
    };

    const char* ids[] = {"tab.pin", "tab.color", "tab.rename", "tab.group", "tab.duplicate", "tab.close_others",
                         "tab.activate", "pane.focus", "window.activate", "sidebar.section", "view.icon_size",
                         "tag.create", "tag.edit", "tag.delete", "tag.group", "smartview.save", "recent.remove",
                         "alias.set", "tray.remove", "search.open", "index.status", "net.place", "keymap.read",
                         "keymap.set"};
    bool all = true, direct = true;
    for (const char* id : ids) {
        all = all && act::HasArgRunner(id);
        direct = direct && act::AiPolicy(id).gate == act::Gate::Direct;
    }
    Check(all && !act::HasArgRunner("view.sort") && !act::HasArgRunner("no.such"), L"args: every P4a action has a runner");
    Check(direct, L"args: P4a actions run at once (they undo)");

    bool round = true;
    for (const char* w : {"red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"}) {
        const int i = act::ColorIndex(w);
        round = round && i >= 0 && !act::ColorName(i).empty();
    }
    Check(round && act::ColorIndex("none") == -1 && act::ColorIndex("plaid") == -2, L"args: color words");

    // ---- Tabs: pin, color, rename, each undone ------------------------------
    s.window_tabs.items.clear();
    for (const wchar_t* t : {L"发票", L"合同", L"报销单"}) {
        auto tab = std::make_unique<LayoutTab>();
        tab->title = t;
        s.window_tabs.items.push_back(std::move(tab));
    }
    s.window_tabs.active = 0;
    LayoutTab* invoice = s.window_tabs.items[0].get();
    LayoutTab* contract = s.window_tabs.items[1].get();
    LayoutTab* claims = s.window_tabs.items[2].get();
    {
        act::ArgRun r = act::RunWithArgs(s, "tab.pin", args(R"({"tab":["发票","合同"]})"));
        Check(r.ok && !r.skip && invoice->pinned && contract->pinned && !claims->pinned && !r.line_bold.empty() && r.undo,
              L"args: tab.pin pins the tabs named");
        act::ArgRun again = act::RunWithArgs(s, "tab.pin", args(R"({"tab":["发票","合同"]})"));
        Check(again.ok && again.skip, L"args: pinning pinned tabs is already so");
        r.undo(s);
        Check(!invoice->pinned && !contract->pinned, L"args: tab.pin undoes");
        Check(invoice->marker_rgb == 0 && contract->marker_rgb == 0, L"args: undoing a pin takes back the color it gave");
        Check(s.window_tabs.items[0].get() == invoice && s.window_tabs.items[1].get() == contract &&
                  s.window_tabs.items[2].get() == claims,
              L"args: undoing a pin puts the tabs back in place");
    }
    {
        // Pin one tab (it gets a color), then color it: the line says 无 → 蓝色.
        act::ArgRun pin = act::RunWithArgs(s, "tab.pin", args(R"({"tab":"报销单"})"));
        act::ArgRun color = act::RunWithArgs(s, "tab.color", args(R"({"tab":"报销单","color":"blue"})"));
        Check(pin.ok && color.ok && color.data.get_string("from") == json::Utf8(l10n::Get(l10n::StringId::ActColorNone)),
              L"args: a pin's own color counts as none");
        if (color.undo) color.undo(s);
        if (pin.undo) pin.undo(s);
        Check(!claims->pinned && claims->marker_rgb == 0 && s.window_tabs.items[2].get() == claims,
              L"args: pin then color undoes to where it was");
    }
    {
        act::ArgRun bad = act::RunWithArgs(s, "tab.color", args(R"({"tab":"发票"})"));
        Check(!bad.ok && !bad.error.empty() && !bad.why.empty(), L"args: tab.color needs a color");
        const uint32_t before = claims->marker_rgb;
        act::ArgRun r = act::RunWithArgs(s, "tab.color", args(R"({"tab":3,"color":"blue"})"));
        Check(r.ok && claims->marker_rgb == TabController::Palette()[act::ColorIndex("blue")] && r.arg == act::ColorName(act::ColorIndex("blue")),
              L"args: tab.color by number (1 = first)");
        if (r.undo) r.undo(s);
        Check(claims->marker_rgb == before, L"args: tab.color undoes");
    }
    {
        act::ArgRun r = act::RunWithArgs(s, "tab.rename", args(R"({"tab":"合同","name":"  报销  "})"));
        Check(r.ok && contract->title == L"报销", L"args: tab.rename trims the name");
        if (r.undo) r.undo(s);
        Check(contract->title == L"合同", L"args: tab.rename undoes");
    }
    Check(!act::RunWithArgs(s, "tab.activate", args(R"({"tab":99})")).ok &&
              !act::RunWithArgs(s, "tab.pin", args(R"({"tab":"没有这个"})")).ok,
          L"args: a tab that is not there fails with a reason");

    // ---- Tags: create, delete, undo brings it back ----------------------------
    {
        const size_t before = s.places.tags.size();
        act::ArgRun r = act::RunWithArgs(s, "tag.create", args(R"({"name":"P4测试","color":"blue"})"));
        Check(r.ok && s.places.tags.size() == before + 1 && r.arg == L"P4测试", L"args: tag.create makes the tag");
        Check(!act::RunWithArgs(s, "tag.create", args(R"({"name":"P4测试"})")).ok, L"args: tag.create refuses a name in use");
        Check(!act::RunWithArgs(s, "tag.create", args(R"({"name":"  "})")).ok, L"args: tag.create needs a name");
        act::ArgRun del = act::RunWithArgs(s, "tag.delete", args(R"({"tag":"P4测试"})"));
        Check(del.ok && s.places.tags.size() == before && del.undo, L"args: tag.delete deletes it");
        del.undo(s);
        bool back = false;
        for (const ColorTag& t : s.places.tags) back = back || t.name == L"P4测试";
        Check(back, L"args: undoing tag.delete brings the tag back");
        // The brought-back tag has a new id: undoing the create (old id) leaves it.
        if (r.undo) r.undo(s);
        Check(s.places.tags.size() == before + 1, L"args: an old undo does not touch a tag made since");
        act::ArgRun made = act::RunWithArgs(s, "tag.create", args(R"({"name":"P4临时"})"));
        if (made.undo) made.undo(s);
        bool gone = true;
        for (const ColorTag& t : s.places.tags) gone = gone && t.name != L"P4临时";
        Check(made.ok && gone, L"args: undoing tag.create removes the tag");
        act::RunWithArgs(s, "tag.delete", args(R"({"tag":"P4测试"})"));
    }

    // ---- Keymap: bad input changes nothing --------------------------------------
    Check(!act::RunWithArgs(s, "keymap.set", args(R"({"action":"no_such","keys":"Ctrl+Shift+F"})")).ok,
          L"args: keymap.set needs a known action");
    Check(!act::RunWithArgs(s, "keymap.set", args(R"({"action":"NewFolder","keys":"Ctrl+Banana"})")).ok,
          L"args: keymap.set rejects keys it cannot read");

    // ---- P4b: what asks, what is locked, and their words ---------------------------
    bool ext_words = true, locks = true, ask_gate = true;
    for (const act::ActionEntry& e : act::kActions) {
        if (e.risk == act::ActionRisk::External) ext_words = ext_words && act::HasAskText(e.id);
        if (e.risk == act::ActionRisk::UserOnly) locks = locks && act::HasLockText(e.id);
        if (e.risk == act::ActionRisk::External || e.risk == act::ActionRisk::System)
            ask_gate = ask_gate && act::AiPolicy(e.id).gate == act::Gate::Confirm;
    }
    Check(ext_words, L"args: every External action has its own confirm words");
    Check(locks, L"args: every only-you action says why and where");
    Check(ask_gate, L"args: External and System actions ask every time");
    Check(act::AiPolicy("assistant.prefs").gate == act::Gate::Locked && act::AiPolicy("drive.unlock").gate == act::Gate::Locked,
          L"args: the assistant's own settings and BitLocker unlock are the user's");
    bool runners = true;
    for (const char* id : {"git.refresh", "git.push", "git.pull", "shell.verbs", "shell.run_verb", "toolbar.open_with",
                           "preview.cloud_download", "drive.eject", "drive.disconnect"})
        runners = runners && act::HasArgRunner(id);
    Check(runners && act::AiPolicy("git.refresh").gate == act::Gate::Direct &&
              act::AiPolicy("shell.verbs").gate == act::Gate::Direct,
          L"args: P4b runners exist; the reads run at once");
    {
        const act::ArgLock e_drive = act::LockFor(s, "drive.unlock", args(R"({"drive":"E:"})"));
        const act::ArgLock any = act::LockFor(s, "drive.unlock", args("{}"));
        Check(!e_drive.title.empty() && !e_drive.sub.empty() && e_drive.where.find(L'E') != std::wstring::npos &&
                  !any.where.empty() && any.where != e_drive.where,
              L"args: the unlock lock card names the drive the user double-clicks");
        Check(!act::LockFor(s, "app.exit", args("{}")).where.empty() &&
                  !act::LockFor(s, "assistant.prefs", args("{}")).where.empty(),
              L"args: exit and the assistant's settings say where the user does them");
    }
    // No repository, no drive, nothing selected: no card, and the runner says why.
    Check(act::AskFor(s, "git.push", args("{}")).state == act::ArgAsk::State::Fail &&
              !act::RunWithArgs(s, "git.push", args("{}")).ok && !act::RunWithArgs(s, "git.refresh", args("{}")).ok,
          L"args: git outside a repository asks nothing and fails with a reason");
    Check(act::AskFor(s, "drive.eject", args(R"({"drive":"?"})")).state == act::ArgAsk::State::Fail &&
              !act::RunWithArgs(s, "drive.eject", args(R"({"drive":"?"})")).ok,
          L"args: eject needs a real drive");
    Check(act::AskFor(s, "shell.run_verb", args(R"({"verb":"打印"})")).state == act::ArgAsk::State::Fail &&
              !act::RunWithArgs(s, "shell.run_verb", args("{}")).ok,
          L"args: a shell item needs a selection and a name");
    Check(!act::RunWithArgs(s, "toolbar.open_with", args(R"({"app":"no such app"})")).ok,
          L"args: open_with names a pinned app");
    Check(act::AskFor(s, "view.sort", args("{}")).title.empty() && !act::HasAskText("view.sort"),
          L"args: actions without words use the generic card");
}

// P5: what AI apps (MCP) may run - the groups behind the settings switches,
// ai.json's mcp_allow, and the checks run_actions makes before any step runs.
void TestMcpRunActions() {
    namespace act = app::actions;
    namespace ai = app::ai;
    using G = act::McpGroup;
    size_t counts[act::kMcpGroupCount] = {};
    for (const act::ActionEntry& e : act::kActions) ++counts[static_cast<size_t>(act::McpGroupOf(e.id))];
    size_t total = 0;
    for (size_t n : counts) total += n;
    Check(total == std::size(act::kActions) && counts[static_cast<size_t>(G::View)] > 0 &&
              counts[static_cast<size_t>(G::Locked)] == 3 && counts[static_cast<size_t>(G::Files)] == 11,
          L"mcp: every action is in one group; 3 are only the user's, 11 change files");
    Check(act::McpGroupOf("nav.back") == G::View && act::McpGroupOf("settings.set") == G::Organize &&
              act::McpGroupOf("file.new_folder") == G::Create && act::McpGroupOf("file.recycle") == G::Files &&
              act::McpGroupOf("file.delete_permanent") == G::Files && act::McpGroupOf("git.push") == G::External &&
              act::McpGroupOf("app.exit") == G::Locked && act::McpGroupOf("assistant.prefs") == G::Locked,
          L"mcp: sample actions sit in the groups the settings page names");
    bool files_are_files = true;
    for (const act::ActionEntry& e : act::kActions)
        if (act::IsFileOp(e.id)) files_are_files = files_are_files && act::McpGroupOf(e.id) == G::Files;
    Check(files_are_files, L"mcp: every file step is behind the 移动、重命名和删除 switch");

    // ai.json: no mcp_allow = view and create; the switches write the list; locked never.
    ai::Settings fresh;
    Check(ai::McpAllows(fresh, "view") && ai::McpAllows(fresh, "create") && !ai::McpAllows(fresh, "organize") &&
              !ai::McpAllows(fresh, "files") && !ai::McpAllows(fresh, "external") && !ai::McpAllows(fresh, "locked"),
          L"mcp: without mcp_allow only 查看与界面 and 新建 are on");
    ai::Settings edited;
    ai::SetMcpAllow(edited, "organize", true);
    ai::SetMcpAllow(edited, "view", false);
    ai::SetMcpAllow(edited, "locked", true);
    const ai::Settings back = ai::ParseSettings(ai::SerializeSettings(edited));
    Check(back.mcp_allow_set && ai::McpAllows(back, "organize") && !ai::McpAllows(back, "view") &&
              ai::McpAllows(back, "create") && !ai::McpAllows(back, "locked"),
          L"mcp: switches survive ai.json; turning one on keeps the defaults; locked cannot be switched on");
    bool malformed = true;
    const ai::Settings old_file = ai::ParseSettings(R"({"mcp_enabled":true,"log_days":30,"clients":[],"allow":[]})", &malformed);
    Check(!malformed && !old_file.mcp_allow_set && ai::McpAllows(old_file, "view") && !ai::McpAllows(old_file, "files"),
          L"mcp: an ai.json from before P5 reads as the defaults");

    // run_actions from an AI app: refused before any step when a step is unknown,
    // only the user's, switched off, or a second file step. Nothing runs.
    auto owner = std::make_unique<AppState>();
    AppState& s = *owner;
    s.places.persist = false;
    s.appPrefs.persist = false;
    s.isolatedTest = true;
    auto run_of = [&](const char* steps_json, std::initializer_list<G> on) {
        auto run = std::make_shared<ai::McpActionsRun>();
        run->client = L"Selftest";
        json::Value steps;
        json::Parse(steps_json, steps);
        for (size_t i = 0; i < steps.size(); ++i) run->steps.push_back(steps.at(i));
        for (G g : on) run->allowed[static_cast<size_t>(g)] = true;
        ai::StartMcpRunActions(s, run);
        return run;
    };
    auto code_of = [](const std::shared_ptr<ai::McpActionsRun>& run) {
        std::lock_guard lock(run->mu);
        return run->done ? (run->outcome.ok ? std::string("ok") : run->outcome.error.code) : std::string("running");
    };
    const auto off = run_of(R"([{"action":"settings.set","args":{"key":"x","value":1}}])", {G::View});
    Check(code_of(off) == "not_allowed" && off->outcome.error.message.find("step 1 (settings.set)") != std::string::npos &&
              off->outcome.summary.find(L'\n') != std::wstring::npos,
          L"mcp: a step whose group is off: not_allowed naming the step, with 本次运行's summary");
    const auto locked = run_of(R"([{"action":"app.exit"}])", {G::View, G::Organize, G::Create, G::Files, G::External});
    Check(code_of(locked) == "locked", L"mcp: an only-you action is refused (locked) with every switch on");
    const auto unknown = run_of(R"([{"action":"no.such.action"}])", {G::View});
    Check(code_of(unknown) == "invalid_args", L"mcp: an unknown action: invalid_args, nothing done");
    const auto two_files = run_of(R"([{"action":"file.recycle"},{"action":"file.move_to","args":{"folder":"C:\\x"}}])",
                                  {G::View, G::Files});
    Check(code_of(two_files) == "invalid_args", L"mcp: two file steps in one call are refused before either runs");
    const auto later_off = run_of(R"([{"action":"nav.back"},{"action":"git.push"}])", {G::View});
    Check(code_of(later_off) == "not_allowed" && later_off->undo.empty(),
          L"mcp: a later step switched off stops the call before the first step runs");
    Check(code_of(run_of("[]", {G::View})) == "invalid_args", L"mcp: no steps: invalid_args");
    // A new folder is asked about (and made) in the folder shown now.
    const auto no_folder = run_of(R"([{"action":"file.new_folder","args":{"name":"A"}}])", {G::View, G::Create});
    Check(code_of(no_folder) == "invalid_args" && no_folder->outcome.error.message.find("shows no folder") != std::string::npos,
          L"mcp: a new folder with no folder open is refused before Pulse asks");
    const auto moved = run_of(R"([{"action":"nav.go","args":{"path":"C:\\"}},{"action":"file.new_folder","args":{"name":"A"}}])",
                              {G::View, G::Create});
    Check(code_of(moved) == "invalid_args" && moved->outcome.error.message.find("step 1 (nav.go)") != std::string::npos &&
              moved->undo.empty(),
          L"mcp: a new folder after a step that changes the folder is refused before anything runs");
}

void TestActionState() {
    namespace act = app::actions;
    auto owner = std::make_unique<AppState>();
    AppState& s = *owner;
    s.places.persist = false;
    s.appPrefs.persist = false;
    s.isolatedTest = true;

    // ---- Panes in screen order -------------------------------------------
    app::LayoutTab& lt = LiveLayout(s);
    lt.panes.clear();
    for (int i = 0; i < 4; ++i) lt.panes.push_back(std::make_unique<Pane>());
    std::vector<Pane*> four;
    for (auto& p : lt.panes) four.push_back(p.get());
    lt.layout = LayoutPreset::FourGrid;
    lt.root = MakePresetTree(LayoutPreset::FourGrid, four);
    lt.focused_index = 3;
    lt.target_index = 1;
    const wchar_t* folders[] = { L"C:\\AsT\\One", L"C:\\AsT\\Two", L"\\\\?\\C:\\AsT\\Three", L"C:\\AsT\\Four" };
    for (int i = 0; i < 4; ++i) {
        four[static_cast<size_t>(i)]->view.current_path = folders[i];
        four[static_cast<size_t>(i)]->view.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    }
    {
        std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
        LayoutSplitTree(*lt.root, D2D1::RectF(0, 0, 1000, 800), 8.0f, laid);
        const std::vector<Pane*> order = act::PanesOnScreen(lt);
        bool reading = order.size() == 4 && laid.size() == 4;
        auto rect_of = [&](Pane* p) {
            for (const auto& entry : laid)
                if (entry.first == p) return entry.second;
            return D2D1::RectF();
        };
        if (reading) {
            const D2D1_RECT_F r0 = rect_of(order[0]), r1 = rect_of(order[1]), r2 = rect_of(order[2]), r3 = rect_of(order[3]);
            reading = r0.left < r1.left && r0.top == r1.top && r2.top > r0.top && r2.left < r3.left && r2.top == r3.top;
        }
        Check(reading, L"state: four panes are numbered left to right, then top to bottom");
        // A custom tree whose tree order is not reading order: [A over B] | C.
        Pane a, b, c;
        app::LayoutTab custom;
        std::vector<Pane*> left_pair{ &a, &b };
        custom.root = SplitContainer::Join(SplitOrientation::Vertical, 0.5f,
                                           MakePresetTree(LayoutPreset::TwoHorizontal, left_pair),
                                           SplitContainer::CreateLeaf(&c));
        const std::vector<Pane*> mixed = act::PanesOnScreen(custom);
        Check(mixed.size() == 3 && mixed[0] == &a && mixed[1] == &c && mixed[2] == &b,
              L"state: a custom split is numbered by position, not tree order");
    }
    {
        const json::Value state = act::StateSnapshot(s);
        const std::vector<Pane*> order = act::PanesOnScreen(lt);
        bool fields = state.find("panes") && state.find("panes")->items().size() == 4;
        for (size_t i = 0; fields && i < 4; ++i) {
            const json::Value& p = AsPane(state, i);
            const bool is_focused = order[i] == lt.panes[3].get();
            const bool is_target = order[i] == lt.panes[1].get();
            fields = AsInt(p.find("pane")) == static_cast<int64_t>(i) + 1 &&
                     p.get_bool("focused") == is_focused && p.get_bool("target") == is_target;
        }
        Check(fields, L"state: pane numbers, the focused pane and the target pane");
        Check(AsPane(state, 2).get_string("folder") == "C:\\AsT\\Three",
              L"state: folders are shown without the \\\\?\\ prefix");
        Check(state.find("tab") && state.find("tab")->get_string("layout") == "four" &&
              state.find("window") && state.find("window")->get_string("title") == "Four",
              L"state: the tab's layout and the window title from the focused folder");
        Check(state.find("preview") && state.find("preview")->is_null() && state.find("dialog") &&
              state.find("dialog")->is_null() && state.find("panels") && state.find("file_ops") &&
              !state.find("file_ops")->get_bool("active"),
              L"state: no preview, dialog or file task when none is open");
    }

    // ---- One pane, the selector's fixture ----------------------------------
    lt.panes.resize(2);
    std::vector<Pane*> two{ lt.panes[0].get(), lt.panes[1].get() };
    lt.layout = LayoutPreset::TwoVertical;
    lt.root = MakePresetTree(LayoutPreset::TwoVertical, two);
    lt.focused_index = 0;
    lt.target_index = 1;
    Tab& tab = lt.panes[0]->view;
    tab.current_path = L"C:\\AsT\\Downloads";
    tab.view_mode = ui::ViewMode::Tiles;
    tab.sort_column = ui::SortColumn::Size;
    tab.sort_direction = ui::SortDirection::Desc;
    // "Now" is Wednesday 2026-10-28 15:00 local; the week began on Monday the 26th.
    const uint64_t now = AsLocalTicks(2026, 10, 28, 15, 0);
    const uint64_t kb = 1024, mb = 1024 * 1024;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    entries->push_back(AsEntry(L"发票-10月.pdf", 300 * kb, AsLocalTicks(2026, 10, 28, 10, 0)));     // 0
    entries->push_back(AsEntry(L"合同扫描.PDF", 3 * mb, AsLocalTicks(2026, 10, 28, 9, 0)));         // 1
    entries->push_back(AsEntry(L"截图.png", 1 * mb, AsLocalTicks(2026, 10, 28, 8, 0)));             // 2
    entries->push_back(AsEntry(L"素材包.zip", 48 * mb, AsLocalTicks(2026, 10, 27, 21, 0)));         // 3
    entries->push_back(AsEntry(L"说明书.pdf", 2 * mb, AsLocalTicks(2026, 10, 25, 12, 0)));          // 4
    entries->push_back(AsEntry(L"视频.mp4", 150 * mb, AsLocalTicks(2026, 9, 14, 12, 0)));           // 5
    entries->push_back(AsEntry(L"资料", 0, AsLocalTicks(2026, 10, 28, 7, 0), true));                // 6
    entries->push_back(AsEntry(L"隐藏.pdf", 1 * kb, AsLocalTicks(2026, 10, 28, 7, 0), false,
                               FILE_ATTRIBUTE_HIDDEN));                                             // 7
    entries->push_back(AsEntry(L"logo.png", 20 * kb, AsLocalTicks(2026, 10, 26, 11, 0)));           // 8
    tab.SetSnapshot(entries);

    Check(AsPick(s, R"({"extensions":["pdf"],"modified":"today"})", now) == std::vector<int>({0, 1}) &&
              tab.SelectedIndices() == std::vector<int>({0, 1}),
          L"selector: today's PDFs are selected; hidden files are never candidates");
    Check(AsPick(s, R"({"name":"*.pdf"})", now) == std::vector<int>({0, 1, 4}),
          L"selector: a wildcard name matches whole names, ignoring case");
    Check(AsPick(s, R"({"name":["发票","logo"]})", now) == std::vector<int>({0, 8}),
          L"selector: names without wildcards match what they contain, any of them");
    Check(AsPick(s, R"({"type":"image"})", now) == std::vector<int>({2, 8}) &&
              AsPick(s, R"({"type":"folder"})", now) == std::vector<int>({6}),
          L"selector: type uses search_files' kinds");
    Check(AsPick(s, R"({"size":{"larger_than":"100MB"}})", now) == std::vector<int>({5}) &&
              AsPick(s, R"({"size":{"smaller_than":"1MB"}})", now) == std::vector<int>({0, 8}),
          L"selector: sizes compare files only, never folders");
    Check(AsPick(s, R"({"modified":"yesterday"})", now) == std::vector<int>({3}) &&
              AsPick(s, R"({"modified":"this_week"})", now) == std::vector<int>({0, 1, 2, 3, 6, 8}) &&
              AsPick(s, R"({"modified":"last_month"})", now) == std::vector<int>({5}) &&
              AsPick(s, R"({"modified":{"after":"2026-10-25","before":"2026-10-25"}})", now) ==
                  std::vector<int>({4}),
          L"selector: date presets start on Monday and include the whole last day");
    AsPick(s, R"({"extensions":["pdf"],"modified":"today"})", now);
    AsPick(s, R"({"items":["LOGO.png"],"mode":"add"})", now);
    Check(tab.SelectedIndices() == std::vector<int>({0, 1, 8}), L"selector: add keeps the selection and adds");
    tab.SelectIndices({0, 6});
    AsPick(s, R"({"selected":true,"type":"folder","mode":"remove"})", now);
    Check(tab.SelectedIndices() == std::vector<int>({0}), L"selector: remove drops matches within the selection");
    {
        const app::TagId id = s.places.CreateTag(L"AsT 重要", 0xE81123);
        int tag_index = -1;
        for (size_t i = 0; i < s.places.tags.size(); ++i)
            if (s.places.tags[i].id == id) tag_index = static_cast<int>(i);
        s.places.SetTagged(tag_index, L"C:\\AsT\\Downloads\\说明书.pdf", true);
        std::string reason;
        Check(tag_index >= 0 && AsPick(s, R"({"tag":"ast 重要"})", now) == std::vector<int>({4}),
              L"selector: a tag's name selects the items carrying it");
        AsPick(s, R"({"tag":"没有这个标签"})", now, &reason);
        Check(reason == "unknown_tag", L"selector: an unknown tag says so");
    }
    {
        tab.SelectIndices({2});
        std::string reason;
        const auto none = AsPick(s, R"({"name":"不存在"})", now, &reason);
        Check(none.empty() && reason == "no_match" && tab.SelectedIndices() == std::vector<int>({2}),
              L"selector: nothing matched leaves the selection alone and says why");
        tab.filter_text = L"发票";
        const auto hidden = AsPick(s, R"({"name":"*.png"})", now, &reason);
        json::Value args;
        act::Selector sel;
        std::string error;
        json::Parse(R"({"name":"*.png"})", args);
        act::ParseSelector(args, now, sel, error);
        const act::SelectorMatch m = act::ResolveSelector(s, sel);
        Check(hidden.empty() && reason == "filter_hides" && m.hidden_by_filter == 2 &&
                  tab.SelectedIndices() == std::vector<int>({2}),
              L"selector: items the filter box hides are not selected, only counted");
        tab.filter_text.clear();
        tab.loading = true;
        AsPick(s, R"({"name":"*.png"})", now, &reason);
        Check(reason == "loading", L"selector: a pane still loading asks to wait");
        tab.loading = false;
    }
    {
        Tab& right = lt.panes[1]->view;
        right.current_path = L"C:\\AsT\\Desktop";
        auto r = std::make_shared<std::vector<fs::DirEntry>>();
        r->push_back(AsEntry(L"a.txt", 1, now));
        r->push_back(AsEntry(L"b.mp4", 200 * mb, now));
        right.SetSnapshot(r);
        Check(AsPick(s, R"({"pane":2,"type":"video","size":{"larger_than":"100MB"}})", now) ==
                  std::vector<int>({1}) &&
                  AsPick(s, R"({"pane":"target","name":"a.txt"})", now) == std::vector<int>({0}) &&
                  right.SelectedIndices() == std::vector<int>({0}),
              L"selector: pane numbers and the target pane pick the pane");
    }
    {
        json::Value args;
        act::Selector sel;
        std::string error;
        int bad = 0;
        for (const char* text : { R"({})", R"({"mode":"swap","name":"a"})", R"({"type":"car"})",
                                  R"({"modified":"tomorrow"})", R"({"size":{"larger_than":"lots"}})",
                                  R"({"pane":5,"name":"a"})", R"({"modified":{"after":"2026-13-01"}})" })
            if (json::Parse(text, args) && !act::ParseSelector(args, now, sel, error) && !error.empty()) ++bad;
        Check(bad == 7, L"selector: invalid conditions are rejected with a reason");
        uint64_t bytes = 0;
        Check(act::ParseByteSize(L"1.5 GB", bytes) && bytes == 1610612736ull &&
                  act::ParseByteSize(L"500k", bytes) && bytes == 512000 &&
                  act::ParseByteSize(L"2048", bytes) && bytes == 2048 && !act::ParseByteSize(L"abc", bytes),
              L"selector: byte sizes in B, KB, MB and GB");
        Check(act::WildcardMatch(L"aXXbc.txt", L"A*B?.TXT") && act::WildcardMatch(L"", L"*") &&
                  !act::WildcardMatch(L"x.pdf.bak", L"*.pdf") && act::WildcardMatch(L"报告-最终.docx", L"报告*.docx"),
              L"selector: wildcards * and ?, ignoring case");
    }
    {
        json::Value args;
        act::Selector sel;
        std::string error;
        json::Parse(R"({"extensions":["pdf"]})", args);
        act::ParseSelector(args, now, sel, error);
        const json::Value report = act::ApplySelection(s, act::ResolveSelector(s, sel));
        Check(AsInt(report.find("matched")) == 3 && AsInt(report.find("selected")) == 3 &&
                  report.find("names") && report.find("names")->items().size() == 3 &&
                  AsInt(report.find("bytes")) == static_cast<int64_t>(300 * kb + 3 * mb + 2 * mb),
              L"selector: the report gives matches, the selection, names and bytes");
    }
    {
        // P4d: a name search's results come from all over. search_done is
        // over once every page is in; the match counts their folders.
        Tab& right = lt.panes[1]->view;
        const std::wstring keep_path = right.current_path;
        const fs::SnapshotPtr keep_rows = right.snapshot;
        right.current_path = L"pulse:search:发票";
        auto hits = std::make_shared<std::vector<fs::DirEntry>>();
        for (const wchar_t* path : { L"C:\\AsT\\A\\发票1.pdf", L"C:\\AsT\\B\\发票2.PDF", L"C:\\AsT\\B\\发票3.pdf",
                                     L"C:\\AsT\\C\\发票.xlsx" }) {
            fs::DirEntry e = AsEntry(wcsrchr(path, L'\\') + 1, 10 * kb, now);
            e.full_path = path;
            hits->push_back(std::move(e));
        }
        right.search_entries = hits;
        right.SetSnapshot(hits);
        right.search_total = hits->size();
        right.search_next_offset = hits->size();
        act::WaitSpec spec;
        spec.kind = act::WaitKind::SearchDone;
        spec.pane = 2;
        json::Value out;
        Check(act::WaitReady(s, spec, out) && out.get_bool("ok") && out.get_bool("complete") &&
                  AsInt(out.find("results")) == 4,
              L"wait: search_done is over once a name search has every result");
        right.search_total = 9;            // five more than scrolling loaded
        right.search_loading_more = true;  // and their page is on its way
        out = json::Value::Object();
        Check(!act::WaitReady(s, spec, out) && AsInt(out.find("total")) == 9,
              L"wait: search_done waits while results are still coming");
        right.search_loading_more = false;
        right.search_total = 4;
        spec.pane = 1;
        out = json::Value::Object();
        Check(act::WaitReady(s, spec, out) && out.get_bool("ok") && !out.find("results"),
              L"wait: search_done in a folder has nothing to wait for");
        Check(act::PaneShowsSearch(s, 2) && !act::PaneShowsSearch(s, 1), L"selector: a search page is told from a folder");
        json::Value args;
        act::Selector sel;
        std::string error;
        json::Parse(R"({"pane":2,"extensions":["pdf"]})", args);
        act::ParseSelector(args, now, sel, error);
        const act::SelectorMatch m = act::ResolveSelector(s, sel);
        Check(m.results && m.reason.empty() && m.matched == std::vector<int>({0, 1, 2}) && m.folders == 2 &&
                  m.bytes == 30 * kb,
              L"selector: in search results it matches every result and counts their folders");
        json::Parse(R"({"wait":"search_done","timeout_ms":99999})", args);
        Check(act::ParseWait(args, spec, error) && spec.kind == act::WaitKind::SearchDone && spec.timeout_ms == 30000,
              L"wait: search_done is capped at 30 s");
        right.search_entries.reset();
        right.search_total = right.search_next_offset = 0;
        right.current_path = keep_path;
        right.SetSnapshot(keep_rows);
    }
    {
        // P4d: a content search's results live in a store; they are matched
        // on its thread and handed back from a timer, never at once.
        Tab& right = lt.panes[1]->view;
        const std::wstring keep_path = right.current_path;
        const fs::SnapshotPtr keep_rows = right.snapshot;
        right.current_path = L"pulse:search?content:ZX2026";
        auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
        std::vector<index::ContentHit> hits;
        for (const wchar_t* path : { L"C:\\AsT\\甲\\记录1.txt", L"C:\\AsT\\乙\\记录2.txt", L"C:\\AsT\\乙\\备注.md",
                                     L"C:\\AsT\\丙\\记录3.txt" }) {
            index::ContentHit hit;
            hit.path = path;
            hit.name = wcsrchr(path, L'\\') + 1;
            hit.size = 100;
            hit.modified = now;
            hits.push_back(std::move(hit));
        }
        const bool filled = store->Append(hits);
        right.content_results = store;
        right.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        right.ClearSelection();
        json::Value args;
        act::Selector sel;
        std::string error;
        json::Parse(R"({"pane":2,"extensions":["txt"]})", args);
        act::ParseSelector(args, now, sel, error);
        bool called = false;
        act::SelectorMatch got;
        const bool started = act::ResolveSelectorAsync(s, sel, [&](AppState&, act::SelectorMatch m) {
            called = true;
            got = std::move(m);
        });
        const bool later = !called;
        AsPump([&] { return called; }, 5000);
        Check(filled && started && later && called && got.reason.empty() && got.results && got.matched.size() == 3 &&
                  got.folders == 3 && got.bytes == 300,
              L"selector: content search results are matched off the UI thread, with their folders and bytes");
        act::ApplySelection(s, got);
        Check(right.SelectedCount() == 3, L"selector: the match of content search results is selected");
        json::Parse(R"({"pane":2,"tag":"AsT 重要"})", args);
        act::ParseSelector(args, now, sel, error);
        called = false;
        act::ResolveSelectorAsync(s, sel, [&](AppState&, act::SelectorMatch m) {
            called = true;
            got = std::move(m);
        });
        AsPump([&] { return called; }, 3000);
        Check(called && got.reason == "tag_in_results" && right.SelectedCount() == 3,
              L"selector: a tag in content search results says why and changes nothing");
        json::Parse(R"({"pane":1,"name":"a"})", args);
        act::ParseSelector(args, now, sel, error);
        Check(!act::ResolveSelectorAsync(s, sel, [](AppState&, act::SelectorMatch) {}),
              L"selector: a folder is matched at once, not through the store");
        right.ClearSelection();
        right.content_results.reset();
        right.current_path = keep_path;
        right.SetSnapshot(keep_rows);
    }

    // ---- Caps -----------------------------------------------------------------
    {
        auto many = std::make_shared<std::vector<fs::DirEntry>>();
        for (int i = 0; i < 250; ++i)
            many->push_back(AsEntry((L"f" + std::to_wstring(1000 + i) + L".txt").c_str(), 1, now));
        tab.SetSnapshot(many);
        tab.SelectAll();
        Tab& right = lt.panes[1]->view;
        right.SetSnapshot(many);
        right.SelectAll();
        const std::wstring dir = kOpsTestRoot + L"\\action_state";
        CreateDirectoryW(kOpsTestRoot.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        std::vector<std::wstring> staged;
        for (int i = 0; i < 12; ++i) {
            const std::wstring p = dir + L"\\tray" + std::to_wstring(i) + L".txt";
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            staged.push_back(p);
        }
        s.tray.Collect(staged, false);
        const json::Value state = act::StateSnapshot(s);
        const json::Value& left = AsPane(state, 0);
        const json::Value& other = AsPane(state, 1);
        const json::Value* lsel = left.find("selected");
        const json::Value* osel = other.find("selected");
        Check(lsel && AsInt(lsel->find("count")) == 250 && lsel->find("paths") &&
                  lsel->find("paths")->items().size() == act::kStateFocusedPaths &&
                  lsel->find("paths")->items()[0].as_string() == "C:\\AsT\\Downloads\\f1000.txt",
              L"state: the focused pane lists 200 of 250 selected paths and the count");
        Check(osel && AsInt(osel->find("count")) == 250 && osel->find("names") &&
                  osel->find("names")->items().size() == act::kStateOtherNames && !osel->find("paths"),
              L"state: other panes list 20 names");
        const json::Value* tray = state.find("tray");
        Check(tray && AsInt(tray->find("count")) == 12 && tray->find("names") &&
                  tray->find("names")->items().size() == act::kStateTrayNames,
              L"state: the tray gives its count and 10 names");
        Check(left.get_string("view") == "tiles" && left.find("sort") &&
                  left.find("sort")->items().size() == 2 && left.find("sort")->items()[0].as_string() == "size" &&
                  left.find("sort")->items()[1].as_string() == "desc" && state.find("tab") &&
                  state.find("tab")->get_string("layout") == "side_by_side",
              L"state: view, sort and layout use the names get_state documents");
        s.tray.Clear();
        for (const std::wstring& p : staged) DeleteFileW(p.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // ---- Waiting ----------------------------------------------------------------
    {
        act::WaitSpec spec;
        std::string error;
        json::Value args;
        json::Parse(R"({"wait":"preview_open","timeout_ms":99999})", args);
        Check(act::ParseWait(args, spec, error) && spec.kind == act::WaitKind::PreviewOpen && spec.timeout_ms == 10000,
              L"wait: timeouts are capped per kind");
        json::Parse(R"({"wait":"listing_loaded"})", args);
        Check(act::ParseWait(args, spec, error) && spec.timeout_ms == 5000, L"wait: listing_loaded waits 5 s by default");
        json::Parse(R"({"wait":"soon"})", args);
        Check(!act::ParseWait(args, spec, error), L"wait: an unknown kind is rejected");

        tab.loading = true;
        spec = {};
        spec.kind = act::WaitKind::ListingLoaded;
        spec.timeout_ms = 3000;
        bool called = false;
        json::Value got;
        ULONGLONG loaded_at = 0;
        act::WaitFor(s, spec, nullptr, [&](AppState&, json::Value out) { called = true; got = std::move(out); });
        const bool returned_at_once = !called;
        AsPump([&] { return called; }, 4000, [&](ULONGLONG elapsed) {
            if (elapsed >= 150 && tab.loading) { tab.loading = false; loaded_at = GetTickCount64(); }
        });
        Check(returned_at_once && called && got.get_bool("ok") && AsInt(got.find("loaded")) == 250 &&
                  loaded_at && GetTickCount64() - loaded_at < 1000,
              L"wait: listing_loaded returns soon after the listing arrives, without blocking");

        tab.loading = true;
        spec.timeout_ms = 200;
        called = false;
        act::WaitFor(s, spec, nullptr, [&](AppState&, json::Value out) { called = true; got = std::move(out); });
        AsPump([&] { return called; }, 3000);
        Check(called && !got.get_bool("ok") && got.get_bool("timed_out") && AsInt(got.find("loaded")) == 250,
              L"wait: a timeout reports how far it got, not an error");
        tab.loading = false;

        spec = {};
        spec.kind = act::WaitKind::OpFinished;
        called = false;
        act::WaitFor(s, spec, nullptr, [&](AppState&, json::Value out) { called = true; got = std::move(out); });
        Check(called && got.get_bool("ok"), L"wait: op_finished with nothing running returns at once");
    }
    {
        const std::wstring dir = kOpsTestRoot + L"\\action_state_ops";
        CreateDirectoryW(kOpsTestRoot.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW((dir + L"\\dst").c_str(), nullptr);
        const std::wstring src = dir + L"\\copy-me.bin";
        HANDLE h = CreateFileW(src.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<char> block(1 << 20, 'p');
            DWORD written = 0;
            for (int i = 0; i < 4; ++i) WriteFile(h, block.data(), static_cast<DWORD>(block.size()), &written, nullptr);
            CloseHandle(h);
        }
        s.ops.Start([] {});
        Sleep(800);  // the ops worker brings up pulse_shell.exe
        ops::OpRequest r;
        r.type = ops::OpType::Copy;
        r.sources.push_back(src);
        r.dest_dir = dir + L"\\dst";
        const uint64_t task = s.ops.Submit(std::move(r));
        act::WaitSpec spec;
        spec.kind = act::WaitKind::OpFinished;
        spec.task = task;
        spec.timeout_ms = 30000;
        bool called = false;
        json::Value got;
        act::WaitFor(s, spec, nullptr, [&](AppState&, json::Value out) { called = true; got = std::move(out); });
        AsPump([&] { return called; }, 35000);
        Check(task && called && got.get_bool("ok") && got.get_string("phase") == "completed" &&
                  AsInt(got.find("task")) == static_cast<int64_t>(task) && Exists(dir + L"\\dst\\copy-me.bin"),
              L"wait: op_finished returns a real copy's result");
        spec.task = 1;  // long gone or never this run's
        called = false;
        act::WaitFor(s, spec, nullptr, [&](AppState&, json::Value out) { called = true; got = std::move(out); });
        Check(task <= 1 || (called && !got.get_bool("ok")), L"wait: an old task number does not hang");
        s.ops.Stop();
        DeleteFileW((dir + L"\\dst\\copy-me.bin").c_str());
        DeleteFileW(src.c_str());
        RemoveDirectoryW((dir + L"\\dst").c_str());
        RemoveDirectoryW(dir.c_str());
    }
}

void TestIntegrationCompat() {
    AppPrefs prefs;
    prefs.persist = false;
    ContextMenuPrefs context;
    context.persist = false;
    pulse::index::IndexClient index;
    pulse::index::NetworkAgentClient network;
    SettingsController settings;
    settings.BindUi(prefs, context, index, network, {});
    AppPrefs fresh;
    fresh.FromJson(L"{}");
    Check(!fresh.integration_folders_compat, L"compat: off by default");
    settings.IntegrationAction(0);
    Check(prefs.integration_enabled && prefs.open_folders_in_pulse && settings.IntegrationState() == 1,
          L"compat: the master switch registers the folder verb by default");
    settings.IntegrationAction(9);
    Check(prefs.integration_folders_compat && !prefs.open_folders_in_pulse && prefs.integration_folders &&
              prefs.take_over_win_e && prefs.take_over_this_pc && settings.IntegrationState() == 1,
          L"compat: on drops only the folder verb and reads healthy");
    AppPrefs loaded;
    loaded.FromJson(prefs.ToJson());
    Check(loaded.integration_folders_compat && loaded.integration_folders,
          L"compat: the choice round trips through app.json");
    settings.IntegrationAction(7);
    settings.IntegrationAction(7);
    Check(!prefs.open_folders_in_pulse && settings.IntegrationState() == 1,
          L"compat: re-applying another scope leaves the folder verb off");
    settings.IntegrationAction(9);
    Check(!prefs.integration_folders_compat && prefs.open_folders_in_pulse && settings.IntegrationState() == 1,
          L"compat: off registers the folder verb again");
    settings.IntegrationAction(0);
    settings.IntegrationAction(9);
    Check(!prefs.integration_enabled && !prefs.open_folders_in_pulse && prefs.integration_folders_compat &&
              settings.IntegrationState() == 0,
          L"compat: with the master off it is only a saved choice");
}

void TestQuickAccessPinReorder() {
    PlacesCatalog pins;
    pins.persist = false;
    const std::vector<std::wstring> original{ L"C:\\a", L"C:\\b", L"C:\\c" };
    pins.quick_access_paths = original;
    Check(pins.ReorderQuickAccessPinned(L"C:\\a", 2) &&
          pins.quick_access_paths == std::vector<std::wstring>{ L"C:\\b", L"C:\\a", L"C:\\c" },
          L"quick access: downward drag lands at the indicated gap");
    pins.quick_access_paths = original;
    Check(!pins.ReorderQuickAccessPinned(L"C:\\a", 1) && pins.quick_access_paths == original,
          L"quick access: dropping immediately after the source preserves order");
    Check(pins.ReorderQuickAccessPinned(L"C:\\a", 3) &&
          pins.quick_access_paths == std::vector<std::wstring>{ L"C:\\b", L"C:\\c", L"C:\\a" },
          L"quick access: the final insertion gap appends the dragged pin");
    Check(pins.ReorderQuickAccessPinned(L"C:\\a", 0) && pins.quick_access_paths == original,
          L"quick access: upward drag still reaches the first gap");
}

void TestQuickAccess() {
    TestQuickAccessPinReorder();
    PlacesCatalog cat;
    cat.persist = false;
    Check(cat.SetQuickAccessPinned({L"C:\\", L"C:\\Projects", L"c:/projects/",
                                   L"\\\\offline-host\\share\\folder", L"pulse:recent"}, true) &&
          cat.quick_access_paths.size() == 3, L"quick access: roots, UNC and normalized deduplication");
    Check(!cat.SetQuickAccessPinned({L"C:\\Projects"}, true), L"quick access: pin is idempotent");
    cat.ToggleStarred(L"C:\\Projects", PlaceItemKind::Folder);
    cat.RemapPaths(L"C:\\Projects", L"C:\\Renamed");
    Check(cat.IsQuickAccessPinned(L"C:\\Renamed") && !cat.IsQuickAccessPinned(L"C:\\Projects"),
          L"quick access: rename remaps pinned path");
    cat.SetQuickAccessPinned({L"C:\\Renamed"}, false);
    Check(cat.IsStarred(L"C:\\Renamed"), L"quick access: unpin preserves independent favorite");
    cat.SetQuickAccessPinned({L"C:\\tree\\child", L"C:\\other\\child"}, true);
    cat.RemapPaths(L"C:\\tree", L"C:\\other");
    Check(cat.quick_access_paths.size() == 3, L"quick access: descendant remap deduplicates destination");

    Tab tab;
    tab.current_path = L"C:\\";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry folder; folder.name = L"folder"; folder.is_dir = true;
    fs::DirEntry file; file.name = L"file.txt";
    entries->push_back(folder); entries->push_back(folder); entries->push_back(file);
    tab.snapshot = entries;
    tab.SelectOnly(0);
    Check(QuickAccessTargets(&tab, false).size() == 1, L"quick access: selected directory");
    tab.selected.insert(1);
    Check(QuickAccessTargets(&tab, false).size() == 2, L"quick access: multiple directories");
    tab.selected.insert(2);
    Check(QuickAccessTargets(&tab, false).empty(), L"quick access: mixed selection rejected");
    Check(QuickAccessTargets(&tab, true) == std::vector<std::wstring>{L"C:\\"},
          L"quick access: background targets current path, ignores selection");
    tab.current_path = MakeRecyclePath();
    Check(QuickAccessTargets(&tab, false).empty() && QuickAccessTargets(&tab, true).empty(),
          L"quick access: recycle location rejected");

    {
        AppState state;
        state.places.persist = false;
        state.ctxMenuPrefs.persist = false;
        std::vector<ui::FluentMenuItem> menu;
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdPinQuickAccess &&
              menu[0].text == L"固定到快速访问", L"quick access: menu label and pin command");
        state.places.SetQuickAccessPinned({L"C:\\one"}, true);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdPinQuickAccess,
              L"quick access: mixed pin state offers idempotent pin");
        state.places.SetQuickAccessPinned({L"C:\\two"}, true);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one", L"C:\\two"});
        Check(menu.size() == 1 && menu[0].command == CmdUnpinQuickAccess &&
              menu[0].text == L"从快速访问取消固定", L"quick access: all pinned offers unpin");
        state.ctxMenuPrefs.SetItemEnabled(L"pulse:quick-access", false);
        menu.clear();
        AppendQuickAccessCommand(state, menu, {L"C:\\one"});
        Check(menu.empty(), L"quick access: menu preference is respected");

        // Section menu: hiding is per section id; the bulk pair owns every bit.
        state.sidebarHiddenMask = 0;
        ToggleSidebarSection(state, 3);
        Check(state.sidebarHiddenMask == (1u << 3), L"sidebar sections: a toggle hides one section");
        ToggleSidebarSection(state, 3);
        ToggleSidebarSection(state, -1);
        ToggleSidebarSection(state, kSidebarSectionCount);
        Check(state.sidebarHiddenMask == 0, L"sidebar sections: out-of-range sections are ignored");
        // Unhiding Quick access also restores its built-in links, so the section
        // can never come back empty with nothing left to right-click.
        state.sidebarQuickAccessHiddenMask = 0x1Fu;
        state.sidebarHiddenMask = 1u << static_cast<int>(SidebarSectionId::QuickAccess);
        ToggleSidebarSection(state, static_cast<int>(SidebarSectionId::QuickAccess));
        Check(state.sidebarQuickAccessHiddenMask == kDefaultQuickAccessHiddenMask,
              L"sidebar sections: unhiding quick access restores its built-in links");
        SetEverySidebarSectionCollapsed(state, true);
        Check(state.sidebarCollapsedMask == (1u << kSidebarSectionCount) - 1u,
              L"sidebar sections: collapse all covers every section");
        SetEverySidebarSectionCollapsed(state, false);
        Check(state.sidebarCollapsedMask == 0, L"sidebar sections: expand all clears the mask");

        // Section order: the cloud section ships above Drives; a reorder keeps the
        // masks pointing at the same sections because they are keyed by id.
        const auto default_order = DefaultSidebarOrder();
        const auto cloud_pos = std::find(default_order.begin(), default_order.end(),
            static_cast<int>(SidebarSectionId::Cloud));
        const auto drives_pos = std::find(default_order.begin(), default_order.end(),
            static_cast<int>(SidebarSectionId::Drives));
        Check(cloud_pos != default_order.end() && drives_pos != default_order.end() &&
              cloud_pos < drives_pos,
              L"sidebar sections: the cloud section defaults above drives");
        Check(default_order.front() == static_cast<int>(SidebarSectionId::Cloud) &&
              default_order[1] == static_cast<int>(SidebarSectionId::Starred) &&
              default_order.back() == static_cast<int>(SidebarSectionId::Networks),
              L"sidebar sections: OneDrive leads and network locations close the pane");
        const auto messy = NormalizeSidebarOrder({ 5, 5, 99, static_cast<int>(SidebarSectionId::Tags) });
        std::vector<int> unique_ids = messy;
        std::sort(unique_ids.begin(), unique_ids.end());
        unique_ids.erase(std::unique(unique_ids.begin(), unique_ids.end()), unique_ids.end());
        Check(messy.size() == static_cast<size_t>(kSidebarSectionCount) &&
              messy[0] == 5 && messy[1] == static_cast<int>(SidebarSectionId::Tags) &&
              unique_ids.size() == messy.size(),
              L"sidebar sections: order sanitizes duplicates and unknown ids");
        Check(NormalizeSidebarOrder({}).size() == static_cast<size_t>(kSidebarSectionCount),
              L"sidebar sections: an empty order falls back to the default");
        // Pinned folders reorder by dragging; unknown paths and no-ops are safe.
        {
            PlacesCatalog pins;
            pins.persist = false;
            pins.quick_access_paths = { L"C:\\a", L"C:\\b", L"C:\\c" };
            Check(pins.ReorderQuickAccessPinned(L"C:\\c", 0) &&
                  pins.quick_access_paths[0] == L"C:\\c" &&
                  pins.quick_access_paths[1] == L"C:\\a" &&
                  pins.quick_access_paths[2] == L"C:\\b",
                  L"quick access: a dragged pin moves to the requested position");
            Check(!pins.ReorderQuickAccessPinned(L"C:\\missing", 1) &&
                  !pins.ReorderQuickAccessPinned(L"C:\\c", 0),
                  L"quick access: reordering ignores unknown paths and keeps no-ops");
        }

        // A volume change rebuilds the model: clearing the rows and refreshing
        // must bring the drives and the quick-access links back.
        state.sidebar.drives.clear();
        state.sidebar.quick_access.clear();
        state.worker.Start([](WorkResult) {});
        RefreshSidebarModel(state);
        bool sidebar_applied = false;
        const auto sidebar_deadline = GetTickCount64() + 10000;
        while (!sidebar_applied && GetTickCount64() < sidebar_deadline) {
            sidebar_applied = TickSidebarRefresh(state, GetTickCount64());
            if (!sidebar_applied) Sleep(10);
        }
        CancelSidebarRefresh(state);
        state.worker.Stop();
        Check(sidebar_applied && !state.sidebar.drives.empty() && !state.sidebar.quick_access.empty(),
              L"sidebar refresh: a volume change rebuilds the drive and quick-access rows");
        Pane pane; pane.NewTab(L"C:\\");
        const auto sidebar = BuildSidebarModel();
        const auto vm = BuildWindowViewModel(pane, sidebar, true, false, false, &state.places, 0);
        const auto group = std::find_if(vm.sidebar.begin(), vm.sidebar.end(), [](const auto& value) {
            return !value.items.empty() && std::any_of(value.items.begin(), value.items.end(),
                [](const auto& item) { return item.path == fs::NormalizePath(L"C:\\two"); });
        });
        Check(group != vm.sidebar.end() && group->items.back().indent == 0 &&
              group->items.back().path == fs::NormalizePath(L"C:\\two"),
              L"quick access: independent sidebar entry appended at top level");
        const auto starred_section = std::find_if(vm.sidebar.begin(), vm.sidebar.end(),
            [](const auto& section) {
                return section.id == static_cast<int>(SidebarSectionId::Starred);
            });
        Check(starred_section != vm.sidebar.end() && starred_section->header.empty() &&
              !starred_section->items.empty() &&
              starred_section->items.front().path == MakeStarredPath(),
              L"sidebar sections: the starred root is a header-less section of its own");

        // Every built-in quick-access link carries its set id, and the starred
        // root is no longer one of them.
        const bool quick_access_tagged = !sidebar.quick_access.empty() &&
            std::all_of(sidebar.quick_access.begin(), sidebar.quick_access.end(),
                [](const auto& entry) {
                    return entry.builtin >= 0 &&
                           entry.builtin < static_cast<int>(BuiltinQuickAccess::Count);
                });
        Check(quick_access_tagged && !sidebar.starred.empty() &&
              sidebar.starred.front().path == MakeStarredPath(),
              L"sidebar sections: built-in quick-access links are tagged");

        // Rail tooltips: the collapsed sidebar shows icons only, so a hovered row
        // (or a folded section's icon) names itself.
        state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarItem);
        state.hoverLabel = L"Rail row";
        Check(TooltipForHover(state) == L"Rail row",
              L"sidebar rail: a hovered row reports its name");
        state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarHeader);
        Check(TooltipForHover(state) == L"Rail row",
              L"sidebar rail: a hovered folded section reports its name");
        state.hoverRegion = 0;
        state.hoverLabel.clear();

        // Header icons: quick access and the drive list ("This PC") carry one.
        const int access_index =
            SidebarSectionIndex(vm, static_cast<int>(SidebarSectionId::QuickAccess));
        const int drives_index =
            SidebarSectionIndex(vm, static_cast<int>(SidebarSectionId::Drives));
        Check(access_index >= 0 && drives_index >= 0 &&
              !vm.sidebar[static_cast<size_t>(access_index)].icon_glyph.empty() &&
              vm.sidebar[static_cast<size_t>(drives_index)].icon_glyph == L"\xE977",
              L"sidebar sections: quick access and This PC carry header icons");
        // The Desktop row and the This PC section must not share an icon.
        const auto desktop_entry = std::find_if(sidebar.quick_access.begin(),
            sidebar.quick_access.end(), [](const auto& entry) {
                return entry.builtin == static_cast<int>(BuiltinQuickAccess::Desktop);
            });
        Check(desktop_entry != sidebar.quick_access.end() &&
              desktop_entry->glyph != vm.sidebar[static_cast<size_t>(drives_index)].icon_glyph,
              L"sidebar sections: This PC stays distinguishable from Desktop");
        Check(vm.sidebar[static_cast<size_t>(drives_index)].header ==
                  l10n::Get(l10n::StringId::SidebarDrives),
              L"sidebar sections: the drive section is titled by the resource string");

        // OneDrive rows stand on their own too (no header) when accounts exist.
        if (!sidebar.cloud.empty()) {
            const auto cloud_section = std::find_if(vm.sidebar.begin(), vm.sidebar.end(),
                [](const auto& section) {
                    return section.id == static_cast<int>(SidebarSectionId::Cloud);
                });
            Check(cloud_section != vm.sidebar.end() && cloud_section->header.empty() &&
                  cloud_section->items.size() == sidebar.cloud.size(),
                  L"sidebar sections: OneDrive rows stand on their own without a header");
        }

        // A reorder moves whole sections; masks keep following the section id.
        const std::vector<int> reordered{
            static_cast<int>(SidebarSectionId::Drives),
            static_cast<int>(SidebarSectionId::Cloud),
            static_cast<int>(SidebarSectionId::Workspaces),
            static_cast<int>(SidebarSectionId::Starred),
            static_cast<int>(SidebarSectionId::QuickAccess),
            static_cast<int>(SidebarSectionId::SavedSearches),
            static_cast<int>(SidebarSectionId::Tags),
            static_cast<int>(SidebarSectionId::Networks) };
        const auto reordered_vm = BuildWindowViewModel(pane, sidebar, true, false, false,
            &state.places, 0, 1u << static_cast<int>(SidebarSectionId::Cloud), true, &reordered);
        Check(reordered_vm.sidebar.size() == reordered.size() &&
              reordered_vm.sidebar[0].id == static_cast<int>(SidebarSectionId::Drives) &&
              reordered_vm.sidebar[1].id == static_cast<int>(SidebarSectionId::Cloud) &&
              reordered_vm.sidebar[1].hidden && !reordered_vm.sidebar[0].hidden,
              L"sidebar sections: a reorder moves sections and the mask follows the id");

        // Built-in quick-access links can be switched off one by one.
        const auto no_desktop = BuildWindowViewModel(pane, sidebar, true, false, false,
            &state.places, 0, 0, true, nullptr,
            1u << static_cast<int>(BuiltinQuickAccess::Desktop));
        const std::wstring desktop_label = l10n::Get(l10n::StringId::Desktop);
        bool desktop_present = false;
        for (const auto& section : no_desktop.sidebar) {
            for (const auto& item : section.items)
                if (item.label == desktop_label) desktop_present = true;
        }
        Check(!desktop_present && !desktop_label.empty(),
              L"sidebar sections: a switched-off built-in link leaves the sidebar");
        const auto with_desktop = BuildWindowViewModel(pane, sidebar, true, false, false,
            &state.places, 0);
        bool desktop_default_present = false;
        for (const auto& section : with_desktop.sidebar) {
            for (const auto& item : section.items)
                if (item.label == desktop_label) desktop_default_present = true;
        }
        Check(desktop_default_present,
              L"sidebar sections: built-in links stay visible by default");
        // The Downloads link shows its localized name like Explorer ("下载"),
        // not the folder name on disk.
        PWSTR downloads_raw = nullptr;
        std::wstring downloads_path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads_raw)) && downloads_raw)
            downloads_path = fs::NormalizePath(downloads_raw);
        CoTaskMemFree(downloads_raw);
        std::wstring downloads_label;
        for (const auto& section : with_desktop.sidebar) {
            for (const auto& item : section.items)
                if (!downloads_path.empty() && item.path == downloads_path) downloads_label = item.label;
        }
        Check(downloads_path.empty() ||
              (!downloads_label.empty() && downloads_label == l10n::Get(l10n::StringId::Downloads)),
              L"quick access: Downloads uses the localized name");

        // A quick-access badge survives the sidebar model rebuild (#41). Use
        // whichever quick-access row this machine shows, not a fixed built-in.
        std::wstring badge_path;
        for (const auto& section : with_desktop.sidebar) {
            if (section.header != L"\u5FEB\u901F\u8BBF\u95EE") continue; // 快速访问
            for (const auto& item : section.items)
                if (badge_path.empty() && !item.path.empty()) badge_path = item.path;
        }
        Check(!badge_path.empty(), L"quick access: fixture shows a quick-access row");
        if (!badge_path.empty()) {
            state.places.SetQuickAccessBadge(badge_path, L"  工作  ", 0x2E7D32);
            const auto badged = BuildWindowViewModel(pane, BuildSidebarModel(), true, false, false,
                &state.places, 0);
            bool badge_shown = false;
            for (const auto& section : badged.sidebar) {
                for (const auto& item : section.items) {
                    if (_wcsicmp(item.path.c_str(), badge_path.c_str()) == 0 &&
                        item.badge == L"工作")
                        badge_shown = true;
                }
            }
            Check(badge_shown, L"quick access: badge survives a sidebar model rebuild");
            state.places.SetQuickAccessBadge(badge_path, L"", kDefaultBadgeRgb);
            Check(state.places.FindQuickAccessBadge(badge_path) == nullptr,
                  L"quick access: clearing a badge removes it");
        }
    }

    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    const auto test_dir = kSandbox + L"\\quick_access_profile";
    std::filesystem::create_directories(test_dir);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", test_dir.c_str());
    {
        cat.persist = true;
        Check(cat.SetQuickAccessBadge(L"C:\\badge dir", L"盘 \"A\"", 0x123456) &&
              cat.Save(), L"quick access: save profile");
        PlacesCatalog loaded;
        Check(loaded.Load() && loaded.quick_access_paths == cat.quick_access_paths,
              L"quick access: disk roundtrip retains order and offline paths");
        const QuickAccessBadge* loaded_badge = loaded.FindQuickAccessBadge(L"c:\\BADGE DIR\\");
        Check(loaded_badge && loaded_badge->badge == L"盘 \"A\"" && loaded_badge->badge_rgb == 0x123456,
              L"quick access: badge text and color survive a restart");
        WriteUtf8FileAtomic(test_dir + L"\\places.json", L"{\"starred_items\":[]}");
        Check(loaded.Load() && loaded.quick_access_paths.empty(), L"quick access: old profile defaults empty");
        // Deleting every tag sticks: a saved empty list is not re-seeded on the
        // next start, while a profile that never saved tags still gets them.
        // No writer may outlive the fixture directory, so nothing is queued.
        loaded.persist = false;
        WriteUtf8FileAtomic(test_dir + L"\\places.json", L"{}");
        WriteUtf8FileAtomic(test_dir + L"\\tags.json",
                            L"{\"version\":2,\"tags_generation\":\"5\",\"tags\":[\n  ]\n}\n");
        Check(loaded.Load() && loaded.tags.empty(), L"tags: a saved empty tag list stays empty after a restart");
        DeleteFileW((test_dir + L"\\tags.json").c_str());
        Check(loaded.Load() && loaded.tags.size() == 7, L"tags: a profile without saved tags gets the defaults");
    }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
    cat.persist = false;
    Check(compat::WindowDpi(GetDesktopWindow()) > 0 &&
          compat::SystemMetricsForDpi(SM_CXSIZEFRAME, 144) > 0, L"compat: current DPI path");
    wchar_t previous_compat[16]{};
    GetEnvironmentVariableW(L"PULSE_COMPAT_81", previous_compat, ARRAYSIZE(previous_compat));
    SetEnvironmentVariableW(L"PULSE_COMPAT_81", L"1");
    Check(!compat::ModernWindows() && compat::WindowDpi(GetDesktopWindow()) > 0 &&
          compat::SystemMetricsForDpi(SM_CXSIZEFRAME, 144) > 0, L"compat: legacy DPI path");
    SetEnvironmentVariableW(L"PULSE_COMPAT_81", previous_compat[0] ? previous_compat : nullptr);
}

void TestPlacesAndIndex() {
    Check(fs::IsVirtualPath(L"pulse:tag:0") && fs::IsVirtualPath(L"pulse:search:foo") &&
          fs::IsVirtualPath(L"pulse:starred"),
          L"places: pulse: paths are virtual");
    Check(!fs::IsVirtualPath(L"C:\\Users"), L"places: filesystem path is not virtual");
    std::wstring kind, rest;
    Check(ParsePulsePath(L"pulse:tag:3", &kind, &rest) && kind == L"tag" && rest == L"3",
          L"places: parse tag virtual path");
    Check(ParsePulsePath(L"pulse:search:hello", &kind, &rest) && kind == L"search" && rest == L"hello",
          L"places: parse search virtual path");
    Check(ParsePulsePath(L"pulse:starred", &kind, &rest) && kind == L"starred" && rest.empty(),
          L"places: parse starred virtual path");
    Check(MakeStarredPath() == L"pulse:starred", L"places: starred path helper");
    Check(ParsePulsePath(L"pulse:recent", &kind, &rest) && kind == L"recent" && rest.empty() &&
          MakeRecentPath() == L"pulse:recent", L"places: recent virtual path");
    Check(ParsePulsePath(L"pulse:recycle", &kind, &rest) && kind == L"recycle" && rest.empty() &&
          MakeRecyclePath() == L"pulse:recycle" && fs::IsRecycleViewPath(L"pulse:recycle"),
          L"places: recycle virtual path");

    PlacesCatalog cat;
    cat.persist = false;
    cat.EnsureDefaults();
    Check(cat.tags.size() == 7, L"places: seven default color tags");
    Check(!cat.tags[0].id.empty() && cat.ResolveTagRef(L"0") == cat.tags[0].id &&
          cat.ResolveTagRef(cat.tags[0].id) == cat.tags[0].id,
          L"places: stable tag id and legacy index resolution");
    Check(cat.PinWorkspace(L"C:\\proj", L"proj", 1, { L"C:\\proj" }) == 0, L"places: pin workspace");
    Check(cat.FindWorkspace(L"C:\\proj") == 0, L"places: find workspace");
    Check(cat.PinWorkspace(L"C:\\other", L"other", 0, { L"C:\\other" }) == 1,
          L"places: pin second workspace");
    Check(cat.active_workspace == 1, L"places: latest pin becomes active");
    Check(cat.UnpinWorkspace(0) && cat.workspaces.size() == 1 &&
          cat.FindWorkspace(L"C:\\other") == 0 && cat.active_workspace == 0,
          L"places: unpin first workspace shifts later index");
    Check(cat.UnpinWorkspace(L"C:\\other") && cat.workspaces.empty() &&
          cat.active_workspace == -1,
          L"places: unpin active workspace clears current");
    Check(!cat.UnpinWorkspace(0) && !cat.UnpinWorkspace(L"C:\\missing"),
          L"places: unpin missing workspace is a no-op");
    Check(cat.PinWorkspace(L"C:\\proj", L"proj", 1, { L"C:\\proj" }) == 0, L"places: re-pin workspace");
    Check(cat.ToggleTag(0, L"C:\\proj\\a.txt") && cat.PathHasTag(L"C:\\proj\\a.txt", 0),
          L"places: toggle tag on");
    Check(cat.ToggleTag(0, L"C:\\proj\\a.txt") && !cat.PathHasTag(L"C:\\proj\\a.txt", 0),
          L"places: toggle tag off");
    Check(cat.ToggleStarred(L"C:\\proj\\a.txt", PlaceItemKind::File) &&
          cat.IsStarred(L"C:\\proj\\a.txt") && cat.IsStarred(L"\\\\?\\C:\\proj\\a.txt") &&
          cat.starred_items.size() == 1 &&
          cat.starred_items[0].path.find(L".lnk") == std::wstring::npos &&
          cat.starred_items[0].path.find(L"a.txt") != std::wstring::npos,
          L"places: star indexes the real path, not a .lnk");
    Check(cat.ToggleStarred(L"C:\\proj\\folder", PlaceItemKind::Folder) &&
          cat.IsStarred(L"C:\\proj\\folder") && cat.starred_items.size() == 2 &&
          cat.starred_items[0].kind == PlaceItemKind::Folder,
          L"places: folders are indexed the same way as files");
    Check(cat.SetStarredBadge(L"C:\\proj\\folder", L"  xx项目  ", 0x123456) &&
          cat.FindStarred(L"c:\\PROJ\\folder") &&
          cat.FindStarred(L"c:\\PROJ\\folder")->badge == L"xx项目" &&
          cat.FindStarred(L"C:\\proj\\folder")->badge_rgb == 0x123456,
          L"places: starred badge trims text and preserves color");
    Check(!cat.ToggleStarred(L"C:\\proj\\a.txt") && !cat.IsStarred(L"C:\\proj\\a.txt") &&
          cat.IsStarred(L"C:\\proj\\folder"),
          L"places: unstar removes only that index");
    Check(!cat.ToggleStarred(L"C:\\proj\\folder") && cat.starred_items.empty(),
          L"places: unstar last folder clears the index");
    Check(!cat.ToggleStarred(L"pulse:starred") && cat.starred_items.empty(),
          L"places: virtual paths cannot be starred");
    for (int i = 0; i < 105; ++i)
        cat.RecordRecent(L"C:\\recent\\item" + std::to_wstring(i), PlaceItemKind::File);
    Check(cat.recent_items.size() == 100 &&
          cat.recent_items.front().path.find(L"item104") != std::wstring::npos,
          L"places: recent list caps at 100 and keeps newest first");
    cat.RecordRecent(L"C:\\recent\\item50", PlaceItemKind::Folder);
    Check(cat.recent_items.front().path.find(L"item50") != std::wstring::npos &&
          cat.recent_items.front().kind == PlaceItemKind::Folder &&
          cat.RecentItems(RecentFilter::Folders).size() == 1,
          L"places: repeated recent item moves to front and updates kind");
    Check(cat.RemoveRecent(L"c:\\RECENT\\item50") &&
          cat.RecentItems(RecentFilter::Folders).empty(),
          L"places: remove recent is case insensitive");
    Check(cat.ClearRecent() && cat.recent_items.empty(), L"places: clear recent");
    Check(cat.SetTagged(1, L"C:\\proj\\a.txt", true) && cat.PathHasTag(L"C:\\proj\\a.txt", 1),
          L"places: set tagged");
    const uint64_t tag_revision = cat.TagRevision();
    std::vector<TagAdsUpdate> deferred_ads;
    Check(cat.SetTaggedBatch(2,
                             { L"C:\\proj\\batch-a.txt", L"C:\\proj\\batch-b.txt",
                               L"C:\\proj\\batch-a.txt" },
                             true, &deferred_ads) &&
          cat.TagRevision() == tag_revision + 1 && deferred_ads.size() == 2 &&
          cat.PathHasTag(L"C:\\proj\\batch-a.txt", 2) &&
          cat.PathHasTag(L"C:\\proj\\batch-b.txt", 2),
          L"places: batch tagging rebuilds once and deduplicates paths");
    deferred_ads.clear();
    Check(cat.ToggleTagBatch(2,
                             { L"C:\\proj\\batch-a.txt", L"C:\\proj\\batch-b.txt" },
                             &deferred_ads) && deferred_ads.size() == 2 &&
          !cat.PathHasTag(L"C:\\proj\\batch-a.txt", 2) &&
          !cat.PathHasTag(L"C:\\proj\\batch-b.txt", 2),
          L"places: batch tag toggle");
    const TagId custom_id = cat.CreateTag(L"  自定义标签  ", 0x123456);
    Check(!custom_id.empty() && cat.FindTag(custom_id) &&
          cat.FindTag(custom_id)->name == L"自定义标签",
          L"places: create tag trims name and assigns stable id");
    std::vector<std::wstring> mixed_paths{ L"C:\\proj\\one.txt", L"C:\\proj\\two.txt" };
    Check(cat.SetTagsBatch(custom_id, { mixed_paths[0] }, true) &&
          cat.GetSelectionState(custom_id, mixed_paths) == TagSelectionState::Mixed,
          L"places: mixed multi-selection state");
    Check(cat.SetTagsBatch(custom_id, mixed_paths, true) &&
          cat.GetSelectionState(custom_id, mixed_paths) == TagSelectionState::All,
          L"places: mixed click unifies to all");
    Check(cat.RenameTag(custom_id, L"已重命名") &&
          cat.SetTagColor(custom_id, 0x654321) &&
          cat.FindTag(custom_id)->name == L"已重命名" &&
          cat.FindTag(custom_id)->rgb == 0x654321,
          L"places: rename and recolor preserve tag id");
    cat.RemapPaths(L"C:\\proj\\one.txt", L"C:\\moved\\one.txt");
    cat.CloneAssignments(L"C:\\proj\\two.txt", L"C:\\copy\\two.txt");
    Check(cat.GetSelectionState(custom_id, { L"C:\\moved\\one.txt" }) == TagSelectionState::All &&
          cat.GetSelectionState(custom_id, { L"C:\\copy\\two.txt" }) == TagSelectionState::All,
          L"places: move remaps and copy clones assignments");
    Check(cat.DeleteTag(custom_id) == 3 && !cat.FindTag(custom_id),
          L"places: deleting tag removes all assignments");

    const std::wstring ads_file = kSandbox + L"\\tag_ads_v2.tmp";
    HANDLE ads_handle = CreateFileW(ads_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ads_handle != INVALID_HANDLE_VALUE) CloseHandle(ads_handle);
    const std::vector<TagAdsRecord> ads_records{
        { L"stable-id", L"设计", 0x3B82F6 }, { L"stable-id-2", L"复核", 0xEF4444 }
    };
    Check(WriteTagAdsV2(ads_file, ads_records), L"tags: write v2 ADS");
    const auto loaded_ads = ReadTagAdsV2(ads_file);
    Check(loaded_ads.size() == 2 && loaded_ads[0].id == L"stable-id" &&
          loaded_ads[0].name == L"设计" && loaded_ads[0].rgb == 0x3B82F6,
          L"tags: read v2 ADS metadata");
    PlacesCatalog imported;
    imported.persist = false;
    imported.EnsureDefaults();
    const size_t imported_definition_count = imported.tags.size();
    const TagId existing_id = imported.tags[0].id;
    const std::wstring existing_name = imported.tags[0].name;
    Check(WriteTagAdsV2(ads_file, { { L"external-id", existing_name, 0x010203 } }),
          L"tags: write same-name external ADS record");
    imported.ReadAdsIntoCatalog(ads_file);
    Check(imported.tags.size() == imported_definition_count &&
          imported.GetSelectionState(existing_id, { ads_file }) == TagSelectionState::All &&
          !imported.FindTag(L"external-id"),
          L"tags: ADS import reuses case-insensitive existing definition");
    DeleteFileW(ads_file.c_str());
    Check(cat.PinNetwork(L"\\\\server\\share", L"share") == 0, L"places: pin UNC");

    index::Engine engine;
    engine.AddForTest(L"C:\\proj\\src", L"src", true);
    engine.AddForTest(L"C:\\proj\\readme.md", L"readme.md", false);
    index::Query q;
    q.needle = L"read";
    auto hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits.size() == 1 && hits.hits[0].name == L"readme.md",
          L"index: substring search");
    q.needle = L"src";
    q.folders_only = true;
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].is_dir, L"index: folders_only");

    engine.AddForTest(L"C:\\proj\\notes.txt", L"notes.txt", false);
    engine.AddForTest(L"C:\\other\\read.exe", L"read.exe", false);
    engine.AddForTest(L"C:\\proj\\src.bak", L"src.bak", false);
    q.folders_only = false;
    q.rank = false;
    q.limit = 48;
    q.needle = L"read md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: AND terms");
    q.needle = L"readme.md | notes.txt";
    hits = engine.Search(q);
    Check(hits.total == 2, L"index: OR terms");
    q.needle = L"read !md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"read.exe", L"index: NOT term");
    q.needle = L"ext:md";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: ext: filter");
    q.needle = L"folder:src";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].is_dir, L"index: folder: filter");
    q.needle = L"readme.*";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"readme.md", L"index: wildcard");
    q.needle = L"\"readme.md\"";
    hits = engine.Search(q);
    Check(hits.total == 1, L"index: quoted exact");
    q.needle = L"path:other ext:exe";
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"read.exe", L"index: path: filter");
    q.needle = L"src";
    q.rank = true;
    q.limit = 1;
    hits = engine.Search(q);
    Check(hits.total >= 2 && hits.hits[0].name == L"src" && hits.hits[0].is_dir,
          L"index: exact folder ranks above prefix file");
    q.rank = false;
    q.limit = 48;
    q.needle = L"re";
    hits = engine.Search(q);
    const size_t n_re = hits.total;
    q.needle = L"read";
    hits = engine.Search(q);
    Check(hits.total >= 1 && hits.total <= n_re, L"index: longer needle narrows or equals");
    engine.AddForTest(L"C:\\proj\\big.bin", L"big.bin", false, 5ull * 1024 * 1024, 0);
    q.needle = L"ext:bin size:>1mb";
    q.rank = false;
    q.limit = 48;
    hits = engine.Search(q);
    Check(hits.total == 1 && hits.hits[0].name == L"big.bin", L"index: size: filter");

    index::Engine paged;
    paged.AddForTest(L"C:\\pages\\page_d.txt", L"page_d.txt", false);
    paged.AddForTest(L"C:\\pages\\page_b.txt", L"page_b.txt", false);
    paged.AddForTest(L"C:\\pages\\page_a.txt", L"page_a.txt", false);
    paged.AddForTest(L"C:\\pages\\page_c.txt", L"page_c.txt", false);
    index::Query page_query;
    page_query.needle = L"page_";
    page_query.rank = false;
    page_query.sort = index::ResultSort::Name;
    page_query.limit = 2;
    auto page1 = paged.Search(page_query);
    page_query.offset = 2;
    auto page2 = paged.Search(page_query);
    Check(page1.total == 4 && page1.hits.size() == 2 &&
          page1.hits[0].name == L"page_a.txt" && page1.hits[1].name == L"page_b.txt",
          L"index: sorted first page");
    Check(page2.total == 4 && page2.hits.size() == 2 &&
          page2.hits[0].name == L"page_c.txt" && page2.hits[1].name == L"page_d.txt",
          L"index: sorted later page uses offset");

    auto cache_sample = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 32; ++i) {
        fs::DirEntry entry;
        entry.name = L"snapshot-entry-" + std::to_wstring(i) + std::wstring(32, L'x');
        cache_sample->push_back(std::move(entry));
    }
    fs::SnapshotStore measured_store(8, 0);
    measured_store.Update(L"C:\\cache-a", 1, cache_sample);
    const size_t one_snapshot_bytes = measured_store.ResidentBytes();
    fs::SnapshotStore bounded_store(8, one_snapshot_bytes + one_snapshot_bytes / 2);
    bounded_store.Update(L"C:\\cache-a", 1, cache_sample);
    bounded_store.Update(L"C:\\cache-b", 2, cache_sample);
    Check(bounded_store.EntryCount() == 1 && !bounded_store.Peek(L"C:\\cache-a") &&
          bounded_store.Peek(L"C:\\cache-b"),
          L"snapshot: byte budget evicts least-recently-used directory");

    fs::SnapshotStore recent_store(2, 0);
    recent_store.Update(L"C:\\cache-a", 1, cache_sample);
    recent_store.Update(L"C:\\cache-b", 2, cache_sample);
    uint64_t recent_generation = 0;
    Check(recent_store.GetOrStart(L"C:\\cache-a", recent_generation) == cache_sample &&
          recent_generation == 1, L"snapshot: cache hit retains generation");
    recent_store.Update(L"C:\\cache-c", 3, cache_sample);
    Check(recent_store.Peek(L"C:\\cache-a") && !recent_store.Peek(L"C:\\cache-b") &&
          recent_store.ResidentBytes() == 2 * one_snapshot_bytes,
          L"snapshot: cache hit protects directory from capacity eviction");
    recent_store.Put(L"C:\\cache-a", cache_sample);
    recent_store.Update(L"C:\\cache-d", 4, cache_sample);
    Check(recent_store.Peek(L"C:\\cache-a") && !recent_store.Peek(L"C:\\cache-c") &&
          recent_store.ResidentBytes() == 2 * one_snapshot_bytes,
          L"snapshot: put refreshes recency without inflating memory accounting");

    Check(fs::IsUncPath(L"\\\\server\\share") && !fs::IsUncPath(L"C:\\Users"),
          L"net: UNC detection");

    cat.PinWorkspace(L"C:\\proj", L"proj", 0, { L"C:\\proj" });
    cat.RecordVisit(L"C:\\proj\\src");
    cat.RecordVisit(L"C:\\proj\\src");
    cat.RecordVisit(L"C:\\proj\\docs");
    auto freq = cat.FrequentChildren(cat.FindWorkspace(L"C:\\proj"), 8);
    Check(!freq.empty() && freq[0].find(L"src") != std::wstring::npos,
          L"places: frequent child is most visited");

    Pane pane;
    pane.NewTab(L"C:\\proj");
    auto snap = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry file;
    file.name = L"a.txt";
    snap->push_back(file);
    pane.ActiveTab()->SetSnapshot(snap);
    pane.ActiveTab()->filter_text = L"#红";
    cat.SetTagged(0, L"C:\\proj\\a.txt", true);
    ui::PaneViewModel pvm2;
    FillPaneViewModel(pvm2, pane, &cat);
    Check(pvm2.filter_map && pvm2.filter_map->size() == 1,
          L"filter: #红 matches default red tag");
    ui::PaneViewModel cached_pvm;
    FillPaneViewModel(cached_pvm, pane, &cat);
    Check(cached_pvm.filter_map == pvm2.filter_map,
          L"filter: unchanged view reuses cached index");
    Check(cached_pvm.tag_dots == pvm2.tag_dots,
          L"tags: unchanged view reuses cached row dots");

    Pane large_pane;
    large_pane.NewTab(L"C:\\large");
    auto large_snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    large_snapshot->resize(100000);
    for (size_t i = 0; i < large_snapshot->size(); ++i)
        (*large_snapshot)[i].name = L"item-" + std::to_wstring(i) + L".dat";
    large_pane.ActiveTab()->SetSnapshot(large_snapshot);
    cat.SetTagged(0, L"C:\\large\\item-99999.dat", true);
    ui::PaneViewModel large_vm;
    FillPaneViewModel(large_vm, large_pane, &cat);
    Check(large_vm.EntryCount() == 100000 && large_vm.entries.empty() &&
          large_vm.tag_dots && large_vm.tag_dots->empty() &&
          large_vm.row_cache && large_vm.row_cache->rows.empty(),
          L"tags: 100k view keeps rows and tag dots lazily materialized");
}

void TestRecycleAndBatchRename() {
    Check(fs::RecycleIndexPath(L"C:\\$Recycle.Bin\\S-1-5-18\\$RABC123") ==
              L"C:\\$Recycle.Bin\\S-1-5-18\\$IABC123",
          L"recycle: $R content path maps to $I index");
    Check(fs::RecycleIndexPath(L"C:\\$Recycle.Bin\\sid\\$rxyz") ==
              L"C:\\$Recycle.Bin\\sid\\$ixyz",
          L"recycle: lowercase $r maps to $i");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(GetTickCount64());
    const std::wstring index_path = std::wstring(temp) + L"$Ipulse_test_" + suffix;
    const std::wstring content_path = std::wstring(temp) + L"$Rpulse_test_" + suffix;
    const std::wstring original = L"C:\\Users\\TestUser\\Desktop\\photo.jpg";
    FILETIME deleted{};
    deleted.dwLowDateTime = 1;
    deleted.dwHighDateTime = 2;
    {
        HANDLE file = CreateFileW(index_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, L"recycle: create temp $I file");
        if (file != INVALID_HANDLE_VALUE) {
            uint64_t ver = 2;
            uint64_t size = 4096;
            uint32_t nchars = static_cast<uint32_t>(original.size() + 1);
            DWORD written = 0;
            WriteFile(file, &ver, 8, &written, nullptr);
            WriteFile(file, &size, 8, &written, nullptr);
            WriteFile(file, &deleted, 8, &written, nullptr);
            WriteFile(file, &nchars, 4, &written, nullptr);
            WriteFile(file, original.c_str(), nchars * 2, &written, nullptr);
            CloseHandle(file);
        }
    }
    fs::RecycleItem item;
    Check(!fs::ReadRecycleIndex(index_path, item),
          L"recycle: orphan metadata without a payload is rejected");
    HANDLE payload = CreateFileW(content_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(payload != INVALID_HANDLE_VALUE, L"recycle: create matching payload");
    if (payload != INVALID_HANDLE_VALUE) CloseHandle(payload);
    Check(fs::ReadRecycleIndex(index_path, item) && item.name == L"photo.jpg" &&
          item.original_path == original && item.size == 4096 &&
          item.deleted.dwHighDateTime == 2,
          L"recycle: parse Windows $I v2 metadata");

    const std::wstring fixture = std::wstring(temp) + L"PulseRecycleFixture_" + suffix;
    const std::wstring sid = pulse::CurrentUserSidString();
    const std::wstring own = fixture + L"\\" + sid;
    const std::wstring other = fixture + L"\\S-1-5-21-111-222-333-9999";
    const bool fixture_ready = !sid.empty() && CreateDirectoryW(fixture.c_str(), nullptr) &&
        CreateDirectoryW(own.c_str(), nullptr) && CreateDirectoryW(other.c_str(), nullptr);
    Check(fixture_ready, L"recycle: create isolated current-user and other-user fixture");
    if (fixture_ready) {
        Check(CopyFileW(index_path.c_str(), (other + L"\\$Iother").c_str(), TRUE) &&
              CopyFileW(content_path.c_str(), (other + L"\\$Rother").c_str(), TRUE) &&
              CopyFileW(index_path.c_str(), (own + L"\\$Iorphan").c_str(), TRUE),
              L"recycle: fixture contains foreign live item and own orphan");
        std::vector<fs::DirEntry> entries;
        Check(fs::EnumerateRecycleBinAtRoot(fixture, entries) && entries.empty(),
              L"recycle: empty user bin excludes other users and orphan records");
        Check(CopyFileW(index_path.c_str(), (own + L"\\$Ilive").c_str(), TRUE) &&
              CopyFileW(content_path.c_str(), (own + L"\\$Rlive").c_str(), TRUE),
              L"recycle: create own live file fixture");
        Check(fs::EnumerateRecycleBinAtRoot(fixture, entries) && entries.size() == 1 &&
              entries[0].name == L"photo.jpg" && entries[0].size == 4096 &&
              entries[0].recycle_path == fs::NormalizePath(own + L"\\$Rlive") && !entries[0].is_dir,
              L"recycle: list contains only current user's restorable file");
        Check(DeleteFileW((own + L"\\$Rlive").c_str()) != FALSE,
              L"recycle: simulate externally removed payload");
        entries.clear();
        Check(fs::EnumerateRecycleBinAtRoot(fixture, entries) && entries.empty(),
              L"recycle: refresh removes stale record after payload disappears");
        Check(CreateDirectoryW((own + L"\\$Rlive").c_str(), nullptr) != FALSE,
              L"recycle: create directory payload fixture");
        Check(fs::EnumerateRecycleBinAtRoot(fixture, entries) && entries.size() == 1 &&
              entries[0].is_dir, L"recycle: directory payload remains supported");
        entries.clear();
        Check(fs::EnumerateRecycleBinAtRoot(fixture + L"\\missing", entries) && entries.empty(),
              L"recycle: missing recycle root is empty");
        DeleteFileW((own + L"\\$Iorphan").c_str());
        DeleteFileW((own + L"\\$Ilive").c_str());
        RemoveDirectoryW((own + L"\\$Rlive").c_str());
        DeleteFileW((other + L"\\$Iother").c_str());
        DeleteFileW((other + L"\\$Rother").c_str());
    }
    RemoveDirectoryW(own.c_str());
    RemoveDirectoryW(other.c_str());
    Check(RemoveDirectoryW(fixture.c_str()) != FALSE,
          L"recycle: isolated fixture cleaned without touching real recycle bins");
    DeleteFileW(index_path.c_str());
    DeleteFileW(content_path.c_str());

    fs::RecycleBinInfo info;
    fs::QueryRecycleBinInfo(info);
    Check(true, L"recycle: SHQueryRecycleBin occupancy probe");

    std::wstring stem, ext;
    SplitFileName(L"photo.jpg", stem, ext);
    Check(stem == L"photo" && ext == L".jpg", L"batch-rename: split stem/ext");
    SplitFileName(L".gitignore", stem, ext);
    Check(stem == L".gitignore" && ext.empty(), L"batch-rename: leading-dot name has no ext");
    Check(!IsValidFileName(L"a:b") && !IsValidFileName(L"a.") && IsValidFileName(L"ok.txt"),
          L"batch-rename: invalid Windows names");

    BatchRenameRule rule;
    rule.find = L"photo";
    rule.replace = L"img";
    Check(ApplyBatchRenameRule(L"Photo.jpg", 0, rule) == L"img.jpg",
          L"batch-rename: case-insensitive find/replace on stem");
    rule = {};
    rule.prefix = L"new-";
    rule.insert_number = true;
    rule.start = 1;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"new-photo (1).jpg",
          L"batch-rename: prefix plus Windows-style number");

    Check(!BatchRenamePatternUsesIndex(L"{name}{ext}") &&
              BatchRenamePatternUsesIndex(L"{name} ({n}){ext}") &&
              BatchRenamePatternUsesIndex(L"{name}_{n:3}{ext}"),
          L"batch-rename: index token detection ignores {name}");

    rule = {};
    rule.pattern = L"{name}_{n:3}{ext}";
    rule.start = 1;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo_001.jpg",
          L"batch-rename: {n:3} zero-pads the index");

    rule = {};
    rule.pattern = L"{name}.png";
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.png",
          L"batch-rename: template can change extension");

    rule = {};
    rule.find = L"photo";
    rule.replace = L"img";
    rule.pattern = L"{name}_{n:3}{ext}";
    Check(ApplyBatchRenameRule(L"Photo.jpg", 0, rule) == L"img_001.jpg",
          L"batch-rename: find/replace then template");

    rule = {};
    rule.find = L"jpg";
    rule.replace = L"png";
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.jpg",
          L"batch-rename: find/replace stays on stem by default");
    rule.replace_in_extension = true;
    Check(ApplyBatchRenameRule(L"photo.jpg", 0, rule) == L"photo.png",
          L"batch-rename: find/replace can include extension");

    rule = {};
    rule.pattern = L"same{ext}";
    const auto collided = PreviewBatchRename(
        { L"C:\\pulse_ren_a.txt", L"C:\\pulse_ren_b.txt" }, rule);
    Check(collided.size() == 2 &&
              collided[0].status == BatchRenameStatus::Collision &&
              collided[1].status == BatchRenameStatus::Collision &&
              collided[0].new_name == L"same.txt",
          L"batch-rename: two files mapping to the same name collide");

    const std::wstring a = std::wstring(temp) + L"pulse_batch_a.txt";
    const std::wstring b = std::wstring(temp) + L"pulse_batch_b.txt";
    HANDLE fa = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE fb = CreateFileW(b.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fa != INVALID_HANDLE_VALUE) CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE) CloseHandle(fb);
    rule = {};
    rule.prefix = L"renamed-";
    const auto preview = PreviewBatchRename({ a, b }, rule);
    Check(preview.size() == 2 && preview[0].status == BatchRenameStatus::Ok &&
          preview[1].status == BatchRenameStatus::Ok &&
          preview[0].new_name == L"renamed-pulse_batch_a.txt",
          L"batch-rename: preview two files");
    rule.prefix.clear();
    rule.find = L"pulse_batch";
    rule.replace = L"pulse_batch";
    const auto unchanged = PreviewBatchRename({ a }, rule);
    Check(!unchanged.empty() && unchanged[0].status == BatchRenameStatus::Unchanged,
          L"batch-rename: unchanged when name stays the same");
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());

    const auto qa = BuildSidebarModel();
    Check(!qa.quick_access.empty() && qa.quick_access.back().path == MakeRecyclePath(),
          L"sidebar: recycle bin is a quick access pin");
    fs::RecycleBinInfo occupied;
    occupied.valid = true;
    occupied.items = 4;
    occupied.bytes = 2048;
    const auto qa_occ = BuildSidebarModel(&occupied);
    Check(!qa_occ.quick_access.empty() &&
          qa_occ.quick_access.back().path == MakeRecyclePath() &&
          !qa_occ.quick_access.back().detail.empty(),
          L"sidebar: recycle occupancy is on the quick access row");
    Pane recycle_home;
    recycle_home.NewTab(L"C:\\");
    const auto recycle_vm = BuildWindowViewModel(
        recycle_home, qa_occ, true, false, false, nullptr, 0);
    bool recycle_in_access = false;
    const auto recycle_access = std::find_if(recycle_vm.sidebar.begin(), recycle_vm.sidebar.end(),
        [](const auto& section) {
            return section.id == static_cast<int>(SidebarSectionId::QuickAccess);
        });
    if (recycle_access != recycle_vm.sidebar.end()) {
        for (const auto& pin : recycle_access->items) {
            if (pin.path == MakeRecyclePath()) {
                recycle_in_access = !pin.detail.empty();
                break;
            }
        }
    }
    Check(recycle_in_access, L"sidebar: recycle bin sits in the quick access group");

    const auto crumbs = ui::SplitBreadcrumb(L"pulse:recycle");
    Check(crumbs.size() == 2 && crumbs[0].path.empty() && crumbs[1].path == L"pulse:recycle",
          L"recycle: breadcrumb is This PC / Recycle Bin");
}

void TestOpsThroughShell() {
    std::wstring dir = kSandbox;
    g_ops.Start([] {});
    Sleep(800); // let the ops worker bring up pulse_shell.exe

    Check(ops::TerminalCommandLine(L"C:\\A\\B") == L"-d \"C:\\A\\B\"",
          L"ops: wt.exe command line");
    Check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"",
          L"ops: wt.exe command line keeps drive root");
    Check(ops::TerminalCommandLine(L"C:\\quoted\" folder\\") ==
              L"-d \"C:\\quoted\\\" folder\\\\\"",
          L"ops: wt.exe command line escapes quotes and trailing slashes");

    // CreateFolder via the ops layer (menu 新建文件夹 path).
    {
        ops::OpRequest r;
        r.type = ops::OpType::CreateFolder;
        r.sources.push_back(dir + L"\\newfolder");
        RunOp(std::move(r));
        Check(Exists(dir + L"\\newfolder"), L"ops: CreateFolder creates the directory");
        Check(g_ops.CanUndo(), L"ops: CreateFolder is undoable");
    }
    // Undo of create = recycle-delete the created item.
    {
        uint64_t prev = g_ops.Status().completed_ops;
        g_ops.Undo();
        WaitOpDone(prev);
        Check(!Exists(dir + L"\\newfolder"), L"ops: undo create removes the folder");
    }
    // CreateTextFile.
    {
        ops::OpRequest r;
        r.type = ops::OpType::CreateTextFile;
        r.sources.push_back(dir + L"\\新建文本文档.txt");
        RunOp(std::move(r));
        Check(Exists(dir + L"\\新建文本文档.txt"), L"ops: CreateTextFile creates the file");
    }
    // Simulated drop execution: move the file into a subfolder using the same
    // effect computation the drop handler runs.
    {
        CreateDirectoryW((dir + L"\\dst").c_str(), nullptr);
        DWORD effect = ui::ComputeDropEffect(0, dir + L"\\新建文本文档.txt", dir + L"\\dst",
                                             DROPEFFECT_COPY | DROPEFFECT_MOVE);
        ops::OpRequest r;
        r.type = effect == DROPEFFECT_MOVE ? ops::OpType::Move : ops::OpType::Copy;
        r.sources.push_back(dir + L"\\新建文本文档.txt");
        r.dest_dir = dir + L"\\dst";
        RunOp(std::move(r));
        Check(!Exists(dir + L"\\新建文本文档.txt") && Exists(dir + L"\\dst\\新建文本文档.txt"),
              L"ops: drop-execute same-volume move lands");
    }

    {
        const std::wstring target = dir + L"\\ctrl-copy";
        const std::wstring folder = dir + L"\\dst";
        const std::wstring file = folder + L"\\新建文本文档.txt";
        CreateDirectoryW(target.c_str(), nullptr);
        const DWORD effect = ui::ComputeDropEffect(MK_CONTROL, file, target,
                                                   DROPEFFECT_COPY | DROPEFFECT_MOVE);
        ops::OpRequest request;
        request.type = effect == DROPEFFECT_COPY ? ops::OpType::Copy : ops::OpType::Move;
        request.sources = {file, folder};
        request.dest_dir = target;
        RunOp(std::move(request));
        Check(Exists(file) && Exists(folder) &&
              Exists(target + L"\\新建文本文档.txt") &&
              Exists(target + L"\\dst\\新建文本文档.txt"),
              L"Ctrl-drop: copies files and folders while preserving sources");
        Check(ui::ComputeDropEffect(0, file, target, DROPEFFECT_COPY | DROPEFFECT_MOVE)
              == DROPEFFECT_MOVE, L"Ctrl-drop: releasing Ctrl restores same-volume move");
    }

    // Explorer context-menu session end-to-end: REQ_CTX_QUERY through the pipe,
    // pulse_shell builds the real IContextMenu on its session STA thread, the
    // filtered/flattened items come back via RSP_CTX_ITEMS. No invoke here
    // (it would launch an app); the session is closed like a dismissed menu.
    {
        const std::wstring file = dir + L"\\dst\\新建文本文档.txt";
        std::mutex m;
        std::condition_variable cv;
        bool got = false;
        uint32_t got_token = 0;
        std::vector<ops::ShellMenuItem> got_items;
        g_ops.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items,
                                       bool partial, ops::ShellMenuAnswer) {
            std::lock_guard<std::mutex> lk(m);
            got_token = token;
            got_items = std::move(items);
            if (!partial) got = true;
            cv.notify_one();
        });
        const uint32_t token = g_ops.QueryShellMenu({ file }, nullptr, false, false);
        Check(token != 0, L"ctx: query issues a session token");
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(20), [&] { return got; });
        }
        Check(got && got_token == token, L"ctx: RSP_CTX_ITEMS arrives for the session");
        bool clean = true;
        for (const auto& it : got_items) {
            const bool invalid = it.text.empty() || ipc::IsBuiltinContextVerb(it.verb, false);
            if (invalid) {
                clean = false;
                LogLine(L"[info] ctx: unfiltered verb='%s' text='%s'\n",
                        it.verb.c_str(), it.text.c_str());
            }
        }
        Check(clean, L"ctx: items are filtered (no built-in verbs, no empty rows)");
        LogLine(L"[info] ctx: %zu Explorer items for .txt\n", got_items.size());
        g_ops.CloseShellMenu(token);
        g_ops.SetShellMenuCallback(nullptr);
    }

    g_ops.Stop();
}

void TestViewLayouts() {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ui::ViewLayout gutter(ui::ViewMode::Details, D2D1::RectF(0, 0, 600 * scale, 300 * scale),
                              20, 0, 0, scale);
        Check(gutter.HitTest(4 * scale, 10 * scale) == -1 &&
              gutter.HitTest(582 * scale, 10 * scale) == -1 &&
              gutter.HitTest(24 * scale, 10 * scale) == 0 &&
              gutter.ItemRect(0).left == 8 * scale,
              L"layout: details side gutters are empty hit targets at each DPI");
    }
    const D2D1_RECT_F viewport = D2D1::RectF(10.0f, 20.0f, 1010.0f, 720.0f);
    for (int i = 0; i < 8; ++i) {
        const ui::ViewMode mode = ui::ViewModeFromIndex(i);
        ui::ViewLayout layout(mode, viewport, 100000, 0.0f, 0.0f, 1.0f);
        const auto range = layout.VisibleRange();
        Check(range.first == 0 && range.second >= range.first && range.second < 1000,
              L"view: 100k layout materializes visible range only");
        const D2D1_RECT_F first = layout.ItemRect(0);
        Check(layout.HitTest((first.left + first.right) * 0.5f,
                             (first.top + first.bottom) * 0.5f) == 0,
              L"view: item rect and hit-test agree");
        Check(ui::ParseViewMode(ui::ViewModeName(mode)) == mode,
              L"view: stable persistence name round-trips");
    }
    ui::ViewLayout narrow(ui::ViewMode::ExtraLargeIcons,
        D2D1::RectF(0, 0, 180, 500), 20, 0, 0, 1.0f);
    Check(narrow.Metrics().columns == 1, L"view: narrow icon pane degrades to one column");
    const D2D1_RECT_F iconCell = narrow.ItemRect(0);
    const D2D1_RECT_F iconRect = narrow.IconRect(0);
    const D2D1_RECT_F iconName = narrow.NameRect(0);
    const float cellCenter = (iconCell.left + iconCell.right) * 0.5f;
    Check(std::abs((iconRect.left + iconRect.right) * 0.5f - cellCenter) < 0.01f &&
          std::abs((iconName.left + iconName.right) * 0.5f - cellCenter) < 0.01f,
          L"view: icon and filename share the cell center axis");
    ui::ViewLayout hidpiIcons(ui::ViewMode::ExtraLargeIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    // e922dbc: the tier keeps 240 device pixels of artwork. Cells are that plus
    // 8 DIP, absorb at most 16 DIP of the leftover and the row is centred
    // (900 px at 150%: 3 x 276 with 9 px gaps, 27 px margins).
    const auto& xl = hidpiIcons.Metrics();
    const D2D1_RECT_F xl_first = hidpiIcons.ItemRect(0);
    const D2D1_RECT_F xl_last = hidpiIcons.ItemRect(xl.columns - 1);
    Check(std::abs(xl.icon_size - 240.0f) < 0.01f && xl.columns == 3 &&
          xl.cell_width + 0.01f >= 240.0f + 8.0f * 1.5f &&
          xl.cell_width <= 240.0f + 24.0f * 1.5f + 0.01f &&
          std::abs(xl_first.left - (900.0f - xl_last.right)) < 0.01f,
          L"view: extra-large Explorer tier remains capped in physical pixels");
    Check(hidpiIcons.NameRect(0).top >= hidpiIcons.IconRect(0).bottom,
          L"view: extra-large filename never overlaps icon bounds");
    ui::ViewLayout hidpiLarge(ui::ViewMode::LargeIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiLarge.Metrics().icon_size - 144.0f) < 0.01f &&
          hidpiLarge.Metrics().cell_width + 0.01f >= 144.0f + 8.0f * 1.5f &&
          hidpiLarge.NameRect(0).top >= hidpiLarge.IconRect(0).bottom,
          L"view: large icons scale with DPI and keep filename below image");
    ui::ViewLayout hidpiMedium(ui::ViewMode::MediumIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiMedium.Metrics().icon_size - 72.0f) < 0.01f &&
          hidpiMedium.NameRect(0).top >= hidpiMedium.IconRect(0).bottom,
          L"view: medium icons scale with DPI and keep filename below image");
    // Icon grid density: more columns than the old fill-the-row grid (medium
    // at 620 DIP: 5 -> 6, large at 1000 DIP: 8 -> 9), blank gaps, centred rows.
    ui::ViewLayout medium620(ui::ViewMode::MediumIcons, D2D1::RectF(0, 0, 620, 600), 60, 0, 0, 1.0f);
    ui::ViewLayout large1000(ui::ViewMode::LargeIcons, D2D1::RectF(0, 0, 1000, 600), 60, 0, 0, 1.0f);
    Check(medium620.Metrics().columns == 6 && large1000.Metrics().columns == 9,
          L"view: icon grids fit more columns (medium 620: 6, large 1000: 9)");
    const D2D1_RECT_F m0 = medium620.ItemRect(0), m1 = medium620.ItemRect(1), m6 = medium620.ItemRect(6);
    Check(std::abs(m1.left - m0.right - 6.0f) < 0.01f && std::abs(m6.top - m0.bottom - 2.0f) < 0.01f &&
          medium620.HitTest(m0.right + 3.0f, (m0.top + m0.bottom) * 0.5f) == -1,
          L"view: icon grid gaps are 6 x 2 DIP and blank to hit-testing");
    ui::ViewLayout spare(ui::ViewMode::MediumIcons, D2D1::RectF(0, 0, 380, 600), 3, 0, 0, 1.0f);
    const auto& sm = spare.Metrics();
    Check(sm.columns == 3 && std::abs(sm.cell_width - 106.0f) < 0.01f &&
          std::abs(spare.ItemRect(0).left - (380.0f - spare.ItemRect(2).right)) < 0.01f,
          L"view: a wide leftover centres the row instead of stretching cells past +16 DIP");
    Check(std::abs(spare.ThumbnailRect(0).top - spare.ItemRect(0).top - 4.0f) < 0.01f &&
          std::abs(spare.NameRect(0).top - spare.ThumbnailRect(0).bottom - 4.0f) < 0.01f &&
          spare.NameRect(0).bottom <= spare.ItemRect(0).bottom,
          L"view: icon cells keep thumbnail and two name lines inside the tighter cell");
    // A folder's names get 1..4 lines; the name block resizes the cells.
    {
        std::vector<int> short_names(20, 1), with_dirs(20, 1), very_long(10, 5);
        std::vector<int> mixed(19, 1), long_names{1, 2, 3, 3, 3, 3, 3, 3};
        mixed.push_back(2);
        Check(ui::AutoIconNameLines(short_names, false) == 1 && ui::AutoIconNameLines(with_dirs, true) == 2 &&
              ui::AutoIconNameLines(mixed, false) == 2 && ui::AutoIconNameLines(long_names, false) == 3 &&
              ui::AutoIconNameLines(very_long, false) == 4,
              L"view: folder name lines follow the 75th/95th percentile rule (1..4)");
        ui::ViewLayout two(ui::ViewMode::MediumIcons, D2D1::RectF(0, 0, 620, 600), 60, 0, 0, 1.0f);
        ui::ViewLayout three(ui::ViewMode::MediumIcons, D2D1::RectF(0, 0, 620, 600), 60, 0, 0, 1.0f,
                             ui::IconGridNameBlockDip(3));
        const D2D1_RECT_F name3 = three.NameRect(0), size3 = three.FolderSizeRect(0);
        Check(two.Metrics().name_lines == 2 && three.Metrics().name_lines == 3 &&
              std::abs(three.Metrics().cell_height - two.Metrics().cell_height - 18.0f) < 0.01f &&
              std::abs(name3.bottom - name3.top - 56.0f) < 0.01f && name3.bottom <= three.ItemRect(0).bottom &&
              std::abs(size3.top - name3.top - 36.0f) < 0.01f &&
              three.Metrics().columns == two.Metrics().columns,
              L"view: a three-line folder grows cells by one line and keeps the size on the last line");
        ui::ViewLayout wide(ui::ViewMode::MediumIcons, D2D1::RectF(0, 0, 380, 600), 3, 0, 0, 1.0f);
        Check(std::abs(ui::IconGridNameWidth(ui::ViewMode::MediumIcons, 1.0f) - 100.0f) < 0.01f &&
              std::abs(ui::IconGridNameWidth(ui::ViewMode::LargeIcons, 1.0f) - 114.0f) < 0.01f &&
              std::abs(ui::IconGridNameWidth(ui::ViewMode::ExtraLargeIcons, 1.5f) - 267.0f) < 0.01f &&
              two.NameRect(0).right - two.NameRect(0).left <= ui::IconGridNameWidth(ui::ViewMode::MediumIcons, 1.0f) &&
              std::abs(wide.NameRect(0).right - wide.NameRect(0).left -
                       ui::IconGridNameWidth(ui::ViewMode::MediumIcons, 1.0f)) < 0.01f,
              L"view: folder name lines are measured at the widest cell width");
    }
    ui::ViewLayout list(ui::ViewMode::List, viewport, 1000, 0, 0, 1.0f);
    Check(list.MaxScrollX() > 0 && list.MaxScrollY() == 0,
          L"view: list is column-major with horizontal scrolling");
    auto menu = BuildViewMenu(ui::ViewMode::Details);
    Check(menu.size() == 10 && menu[5].radio && !menu[0].radio && !menu[8].radio &&
          menu[8].command == CmdViewWall && menu[9].command == CmdDetailsPanel,
          L"view: menu has eight view choices, the photo wall, and the details panel");
    const auto wall_menu = BuildViewMenu(ui::ViewMode::LargeIcons, false, false, true);
    const auto wall_details = BuildViewMenu(ui::ViewMode::Details, false, false, true);
    Check(wall_menu[8].radio && !wall_menu[1].radio && wall_details[5].radio && !wall_details[8].radio,
          L"view: the photo wall row is checked only while an icon view shows the wall");
    const auto folder_menu = BuildViewMenu(ui::ViewMode::Details, false, true);
    Check(folder_menu.size() == 11 && folder_menu.back().command == CmdApplyViewToAllFolders &&
          folder_menu[9].separator_after,
          L"view: real folders offer apply view and sort to all folders");
    Check(menu[0].glyph_scale > menu[1].glyph_scale &&
          menu[1].glyph_scale > menu[2].glyph_scale,
          L"view: icon menu communicates extra-large, large, and medium scale");

    ui::MainRenderer columns;
    columns.SetScale(1.0f);
    const D2D1_RECT_F pane = D2D1::RectF(0.0f, 0.0f, 1000.0f, 400.0f);
    const auto search = columns.DetailsColumns(pane, {}, true);
    Check(search.count == 5 && search.widths[1] >= 110.0f,
          L"search: details view has a path column");
    const float name_path = search.DividerX(0);
    const auto dragged = columns.ResizeSearchColumnDivider(pane, {}, 0, name_path - 80.0f);
    const auto widened = columns.DetailsColumns(pane, {}, true, dragged);
    Check(dragged[0] > 0.0f && dragged[3] < 1.0f && widened.widths[1] > search.widths[1] + 40.0f,
          L"search: dragging the path divider widens the path column");
}


// Content-fitted details columns (1.0.39). No compositor here, so fitted
// widths are the fallbacks: Date 130, Type 128, Size 90 DIP.
void TestListColumns() {
    using K = ui::MainRenderer::ColumnKind;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ui::MainRenderer r;
        r.SetScale(scale);
        auto pane = [&](float w) { return D2D1::RectF(0.0f, 0.0f, w * scale, 400.0f * scale); };
        auto fills = [&](const ui::MainRenderer::DetailsColumnLayout& c) {
            float sum = 0.0f;
            bool positive = true;
            for (int i = 0; i < c.count; ++i) {
                sum += c.widths[static_cast<size_t>(i)];
                positive &= c.widths[static_cast<size_t>(i)] > 0.0f;
            }
            return positive && std::abs(sum - (c.right - c.left)) < 0.05f;
        };
        auto close_to = [&](float px, float dip) { return std::abs(px - dip * scale) < 0.05f; };

        const auto wide = r.DetailsColumns(pane(1200.0f), {}, false);
        Check(wide.count == 4 && wide.kinds[0] == K::Name && wide.kinds[1] == K::Date &&
              wide.kinds[2] == K::Type && wide.kinds[3] == K::Size && fills(wide) &&
              close_to(wide.Width(K::Date), 130.0f) && close_to(wide.Width(K::Size), 90.0f),
              L"columns: wide details shows name/date/type/size at fitted widths");
        const auto mid = r.DetailsColumns(pane(540.0f), {}, false);
        Check(mid.Has(K::Date) && !mid.Has(K::Type) && mid.Has(K::Size) && fills(mid) &&
              mid.Width(K::Name) >= 210.0f * scale - 0.05f,
              L"columns: narrowing hides Type first and keeps the name readable");
        const auto slim = r.DetailsColumns(pane(400.0f), {}, false);
        Check(!slim.Has(K::Date) && !slim.Has(K::Type) && slim.Has(K::Size) && fills(slim),
              L"columns: very narrow panes hide Date after Type");
        const auto tiny = r.DetailsColumns(pane(200.0f), {}, false);
        Check(tiny.Width(K::Name) >= 80.0f * scale - 0.05f && fills(tiny),
              L"columns: the name keeps its 80 DIP floor");

        const auto legacy = r.DetailsColumns(pane(1200.0f), {0.42f, 0.61f, 0.82f}, false);
        Check(close_to(legacy.Width(K::Date), 130.0f) && close_to(legacy.Width(K::Type), 128.0f),
              L"columns: pre-1.0.39 divider ratios read back as automatic widths");
        const auto manual = r.DetailsColumns(pane(1200.0f), {200.0f, 0.0f, 0.0f}, false);
        Check(close_to(manual.Width(K::Date), 200.0f) && close_to(manual.Width(K::Type), 128.0f),
              L"columns: a manual DIP width is honoured and the rest stay fitted");

        ui::DetailsColumnWidths dividers{};
        dividers = r.ResizeDetailsColumnDivider(pane(1200.0f), dividers, 0, wide.DividerX(0) - 50.0f * scale);
        const auto grown = r.DetailsColumns(pane(1200.0f), dividers, false);
        Check(close_to(grown.Width(K::Date), 180.0f) && close_to(grown.Width(K::Size), 90.0f) &&
              std::abs(grown.DividerX(1) - wide.DividerX(1)) < 0.05f,
              L"columns: dragging the name divider resizes Date and keeps its right edge");
        dividers = r.ResizeDetailsColumnDivider(pane(1200.0f), dividers, 1, grown.DividerX(1) + 20.0f * scale);
        const auto moved = r.DetailsColumns(pane(1200.0f), dividers, false);
        Check(close_to(moved.Width(K::Date), 200.0f) && close_to(moved.Width(K::Type), 108.0f) &&
              std::abs(moved.DividerX(1) - grown.DividerX(1) - 20.0f * scale) < 0.05f,
              L"columns: dragging metadata divider transfers width between its neighbors");
        std::array<float, 4> unused{};
        r.AutoFitColumnDivider(pane(1200.0f), dividers, false, unused, 1);
        const auto refit = r.DetailsColumns(pane(1200.0f), dividers, false);
        Check(dividers[0] == 0.0f && dividers[1] == 0.0f && close_to(refit.Width(K::Date), 130.0f),
              L"columns: double-click auto-fit restores fitted widths");

        const auto search_wide = r.DetailsColumns(pane(1000.0f), {}, true);
        Check(search_wide.kinds[1] == K::Path && !search_wide.two_line && fills(search_wide),
              L"columns: wide search keeps a folder column");
        const auto search_narrow = r.DetailsColumns(pane(700.0f), {}, true);
        Check(!search_narrow.Has(K::Path) && search_narrow.two_line && search_narrow.Has(K::Type) &&
              fills(search_narrow),
              L"columns: narrow search moves the folder under the name");
        ui::PaneViewModel vm;
        vm.view_mode = ui::ViewMode::Details;
        vm.is_search = true;
        Check(r.ListRowHeightDip(vm, pane(700.0f)).row_height_dip >= 42.0f &&
              r.ListRowHeightDip(vm, pane(1000.0f)).row_height_dip < 42.0f,
              L"columns: only the two-line search layout grows the row height");
        std::array<float, 4> search_dividers{};
        search_dividers = r.ResizeSearchColumnDivider(pane(1000.0f), search_dividers, 0,
                                                      search_wide.DividerX(0) - 60.0f * scale);
        const auto search_moved = r.DetailsColumns(pane(1000.0f), {}, true, search_dividers);
        Check(std::abs(search_moved.widths[1] - search_wide.widths[1] - 60.0f * scale) < 0.05f,
              L"columns: the name/folder divider keeps the requested width");
        for (bool search : {false, true}) for (float width : {400.0f, 540.0f, 650.0f, 700.0f, 1000.0f, 1200.0f}) {
            const auto initial = r.DetailsColumns(pane(width), {}, search);
            bool stable = true, tracks = true;
            for (int divider = 0; divider < initial.count - 1; ++divider) for (int direction : {-1, 1}) {
                ui::DetailsColumnWidths normal{};
                std::array<float, 4> searching{};
                // The real mouse-down handler captures the rendered widths.
                for (int i = 0; i < initial.count; ++i) {
                    const auto kind = initial.kinds[i];
                    const int slot = kind == K::Date ? 0 : kind == K::Type ? 1 : kind == K::Size ? 2 : -1;
                    if (slot >= 0) {
                        normal[slot] = initial.widths[i] / scale;
                        searching[slot + 1] = normal[slot];
                    } else if (kind == K::Name && initial.Has(K::Path)) searching[0] = initial.widths[i] / scale;
                }
                auto current = r.DetailsColumns(pane(width), normal, search, searching);
                stable &= current.count == initial.count && current.kinds == initial.kinds;
                const float start = initial.DividerX(divider);
                for (int step = 0; step < 160; ++step) {
                    const float cursor = start + direction * (step < 80 ? step : 159 - step) * 4.0f * scale;
                    const float before = current.DividerX(divider);
                    const float left = divider == 0 ? current.left : current.DividerX(divider - 1);
                    const float right = before + current.widths[divider + 1];
                    const bool metadata = current.kinds[divider] != K::Name && current.kinds[divider] != K::Path;
                    if (search) searching = r.ResizeSearchColumnDivider(pane(width), searching, divider, cursor);
                    else normal = r.ResizeDetailsColumnDivider(pane(width), normal, divider, cursor);
                    current = r.DetailsColumns(pane(width), normal, search, searching);
                    stable &= current.count == initial.count && current.kinds == initial.kinds && fills(current);
                    if (metadata && cursor >= left + 48 * scale && cursor <= right - 48 * scale)
                        tracks &= std::abs(current.DividerX(divider) - cursor) < 0.1f;
                }
            }
            Check(stable, L"columns: repeated drags retain visible columns across widths and search layouts");
            Check(tracks, L"columns: metadata divider follows the pointer instead of consuming name width");
        }
    }
}

// Created / accessed columns and the column chooser (#26-4, #36, #47-1):
// optional columns, their drop order, manual widths, prefs, sorting, date
// groups and the header menu. Same fallback widths as TestListColumns;
// the two new date columns fall back to 130 DIP.
void TestOptionalColumns() {
    using K = ui::MainRenderer::ColumnKind;
    constexpr float scale = 1.5f;
    ui::MainRenderer r;
    r.SetScale(scale);
    auto pane = [&](float w) { return D2D1::RectF(0.0f, 0.0f, w * scale, 400.0f * scale); };
    auto fills = [&](const ui::MainRenderer::DetailsColumnLayout& c) {
        float sum = 0.0f;
        for (int i = 0; i < c.count; ++i) sum += c.widths[static_cast<size_t>(i)];
        return std::abs(sum - (c.right - c.left)) < 0.05f;
    };
    auto close_to = [&](float px, float dip) { return std::abs(px - dip * scale) < 0.05f; };

    Check(r.DetailsColumnsMask() == ui::kDetailsColumnsDefault,
          L"optional columns: the renderer starts with modified / type / size");
    r.SetDetailsColumns(ui::kDetailsColumnsAll);
    const auto all = r.DetailsColumns(pane(1600.0f), {}, false);
    Check(all.count == 6 && all.kinds[0] == K::Name && all.kinds[1] == K::Date &&
          all.kinds[2] == K::Created && all.kinds[3] == K::Accessed && all.kinds[4] == K::Type &&
          all.kinds[5] == K::Size && fills(all) && close_to(all.Width(K::Created), 130.0f) &&
          close_to(all.Width(K::Accessed), 130.0f),
          L"optional columns: all shown in order name, modified, created, accessed, type, size");
    bool order_kept = true, filled = true;
    for (float w = 260.0f; w <= 1600.0f; w += 20.0f) {
        const auto c = r.DetailsColumns(pane(w), {}, false);
        filled &= fills(c) && c.Width(K::Name) >= 80.0f * scale - 0.05f;
        order_kept &= (c.Has(K::Created) || !c.Has(K::Accessed)) && (c.Has(K::Type) || !c.Has(K::Created)) &&
                      (c.Has(K::Date) || !c.Has(K::Type));
    }
    Check(order_kept && filled,
          L"optional columns: narrowing drops accessed, created, type, then modified");
    // 状态: chosen by default, drawn only for cloud folders, right after the name.
    r.SetDetailsColumns(ui::kDetailsColumnsDefault);
    const auto cloud = r.DetailsColumns(pane(1200.0f), {}, false, {}, true);
    const auto plain = r.DetailsColumns(pane(1200.0f), {}, false, {}, false);
    const auto cloud_search = r.DetailsColumns(pane(1200.0f), {}, true, {}, true);
    Check(cloud.count == 5 && cloud.kinds[1] == K::Status && cloud.kinds[2] == K::Date && fills(cloud) &&
              !plain.Has(K::Status) && plain.count == 4 && !cloud_search.Has(K::Status),
          L"cloud status: shown after the name in cloud folders only, never in search results");
    bool status_order = true;
    for (float w = 260.0f; w <= 1200.0f; w += 20.0f) {
        const auto c = r.DetailsColumns(pane(w), {}, false, {}, true);
        status_order &= fills(c) && (c.Has(K::Status) || !c.Has(K::Type)) && (c.Has(K::Date) || !c.Has(K::Status));
    }
    Check(status_order, L"cloud status: narrowing drops type, then status, then modified");
    ui::DetailsColumnWidths status_widths{};
    status_widths = r.ResizeDetailsColumnDivider(pane(1200.0f), status_widths, 1,
                                                 cloud.DividerX(1) + 30.0f * scale, true);
    Check(status_widths[0] > 1.0f && status_widths[1] <= 1.0f,
          L"cloud status: the divider after status resizes modified, status keeps its fitted width");
    r.SetDetailsColumns(ui::kDetailsColumnsAll);
    const auto search = r.DetailsColumns(pane(1600.0f), {}, true);
    Check(!search.Has(K::Created) && !search.Has(K::Accessed) && search.Has(K::Path) &&
          search.Has(K::Date) && fills(search),
          L"optional columns: search results never show creation / access times");

    ui::DetailsColumnWidths widths{};
    widths[3] = 200.0f;
    const auto manual = r.DetailsColumns(pane(1600.0f), widths, false);
    Check(close_to(manual.Width(K::Created), 200.0f) && close_to(manual.Width(K::Accessed), 130.0f),
          L"optional columns: created keeps its own manual width");
    const int created_divider = manual.IndexOf(K::Created);
    widths = r.ResizeDetailsColumnDivider(pane(1600.0f), widths, created_divider,
                                          manual.DividerX(created_divider) - 40.0f * scale);
    const auto dragged = r.DetailsColumns(pane(1600.0f), widths, false);
    Check(close_to(dragged.Width(K::Created), 160.0f) && close_to(dragged.Width(K::Accessed), 170.0f) &&
          widths[3] > 1.0f && widths[4] > 1.0f,
          L"optional columns: dragging between created and accessed stores both widths");
    std::array<float, 4> unused{};
    r.AutoFitColumnDivider(pane(1600.0f), widths, false, unused, created_divider);
    Check(widths[3] == 0.0f && widths[4] == 0.0f,
          L"optional columns: double-click auto-fit resets the date columns");
    Check(ui::MainRenderer::ManualColumnSlot(K::Created, false) == 3 &&
          ui::MainRenderer::ManualColumnSlot(K::Accessed, false) == 4 &&
          ui::MainRenderer::ManualColumnSlot(K::Created, true) < 0 &&
          ui::MainRenderer::ManualColumnSlot(K::Size, true) == 3,
          L"optional columns: manual width slots");

    r.SetDetailsColumns(ui::kDetailsColumnSize);
    const auto size_only = r.DetailsColumns(pane(1200.0f), {}, false);
    Check(size_only.count == 2 && size_only.kinds[1] == K::Size && fills(size_only),
          L"optional columns: hiding modified and type leaves name and size (#26-4)");
    r.SetDetailsColumns(0);
    const auto name_only = r.DetailsColumns(pane(1200.0f), {}, false);
    Check(name_only.count == 1 && fills(name_only), L"optional columns: name alone fills the row");
    r.SetDetailsColumns(0xFFFFFFFFu);
    Check(r.DetailsColumnsMask() == ui::kDetailsColumnsAll, L"optional columns: unknown bits are ignored");

    // Prefs: global choice, default when missing, garbage normalized.
    AppPrefs prefs;
    prefs.persist = false;
    Check(prefs.details_columns == ui::kDetailsColumnsDefault, L"optional columns: default prefs keep the classic set");
    prefs.details_columns = ui::kDetailsColumnModified | ui::kDetailsColumnCreated;
    prefs.folder_sorts.Set(L"C:\\Photos", {ui::SortColumn::Created, ui::SortDirection::Desc});
    AppPrefs reloaded;
    reloaded.persist = false;
    const auto photos = reloaded.FromJson(prefs.ToJson()) ? reloaded.folder_sorts.Find(L"C:\\Photos")
                                                         : std::optional<app::FolderSort>{};
    Check(reloaded.details_columns == prefs.details_columns && photos &&
          photos->column == ui::SortColumn::Created && photos->direction == ui::SortDirection::Desc,
          L"optional columns: shown columns and a per-folder created sort round trip");
    reloaded.FromJson(L"{\"details_columns\":4095}");
    Check(reloaded.details_columns == ui::kDetailsColumnsAll, L"optional columns: stored masks are normalized");
    reloaded.FromJson(L"{}");
    Check(reloaded.details_columns == ui::kDetailsColumnsDefault, L"optional columns: missing key keeps the default");

    // Sorting by creation / access time.
    auto at = [](uint64_t days) {
        const uint64_t t = 133'000'000'000'000'000ull + days * 864'000'000'000ull;
        return FILETIME{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    };
    fs::DirEntry a, b, dir;
    a.name = L"a.txt"; a.mtime = at(9); a.ctime = at(1); a.atime = at(5);
    b.name = L"b.txt"; b.mtime = at(1); b.ctime = at(9); b.atime = at(3);
    dir.name = L"z"; dir.is_dir = true; dir.attrs = FILE_ATTRIBUTE_DIRECTORY; dir.ctime = at(20); dir.atime = at(0);
    using ui::SortColumn; using ui::SortDirection;
    Check(EntryLess(a, b, SortColumn::Created, SortDirection::Asc, FolderSortMode::FoldersFirst) &&
          !EntryLess(a, b, SortColumn::Created, SortDirection::Desc, FolderSortMode::FoldersFirst) &&
          EntryLess(b, a, SortColumn::Accessed, SortDirection::Asc, FolderSortMode::FoldersFirst) &&
          EntryLess(a, b, SortColumn::Accessed, SortDirection::Desc, FolderSortMode::FoldersFirst),
          L"optional columns: entries sort by creation and access time");
    Check(EntryLess(dir, a, SortColumn::Created, SortDirection::Asc, FolderSortMode::FoldersFirst) &&
          EntryLess(dir, a, SortColumn::Created, SortDirection::Desc, FolderSortMode::Mixed) &&
          EntryLess(a, dir, SortColumn::Accessed, SortDirection::Desc, FolderSortMode::Mixed),
          L"optional columns: folder placement modes apply to the new date sorts");

    // Date groups follow the sorted date column.
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    fs::DirEntry fresh = a, old = b;
    fresh.mtime = now_ft; fresh.ctime = at(0);
    old.mtime = at(0); old.ctime = now_ft;
    const GroupClock by_created = MakeGroupClock(SortColumn::Created);
    const GroupClock by_modified = MakeGroupClock();
    Check(GroupRank(fresh, GroupBy::Date, by_modified) == 1 && GroupRank(fresh, GroupBy::Date, by_created) == 8 &&
          GroupRank(old, GroupBy::Date, by_created) == 1 &&
          GroupKey(old, GroupBy::Date, by_created) != GroupKey(old, GroupBy::Date, by_modified),
          L"optional columns: date groups bucket the sorted date column (#47)");
    Check(GroupCompare(old, fresh, GroupBy::Date, by_created, SortColumn::Created, SortDirection::Desc) < 0 &&
          GroupCompare(fresh, old, GroupBy::Date, by_created, SortColumn::Mtime, SortDirection::Desc) < 0 &&
          GroupCompare(fresh, old, GroupBy::Date, by_created, SortColumn::Created, SortDirection::Asc) < 0,
          L"optional columns: date group order follows the column and direction");

    // Header menu.
    const auto menu = BuildDetailsColumnMenu(ui::kDetailsColumnsDefault);
    const int created_cmd = kDetailsColumnToggleBase + static_cast<int>(K::Created);
    Check(menu.size() == 8 && menu[0].checked && !menu[0].enabled && menu[1].checked &&
          menu[2].command == created_cmd && !menu[2].checked && menu[5].checked &&
          menu[6].command == kDetailsColumnToggleBase + static_cast<int>(K::Status) && menu[6].checked &&
          menu[6].separator_after && menu[7].command == kDetailsColumnsReset && !menu[7].enabled,
          L"optional columns: header menu lists name (fixed), the six columns and reset");
    const uint32_t with_created = ApplyDetailsColumnCommand(ui::kDetailsColumnsDefault, created_cmd);
    Check(with_created == (ui::kDetailsColumnsDefault | ui::kDetailsColumnCreated) &&
          ApplyDetailsColumnCommand(with_created, created_cmd) == ui::kDetailsColumnsDefault &&
          ApplyDetailsColumnCommand(with_created, kDetailsColumnsReset) == ui::kDetailsColumnsDefault &&
          ApplyDetailsColumnCommand(with_created, kDetailsColumnToggleBase) == with_created &&
          ApplyDetailsColumnCommand(with_created, 0) == with_created &&
          BuildDetailsColumnMenu(with_created)[2].checked && BuildDetailsColumnMenu(with_created)[7].enabled,
          L"optional columns: header menu toggles a column, resets, and ignores name / dismiss");
    const uint32_t drawn = (1u << static_cast<uint32_t>(K::Name)) | ui::kDetailsColumnsDefault;
    const auto narrow_menu = BuildDetailsColumnMenu(with_created, drawn, false);
    const auto search_menu = BuildDetailsColumnMenu(with_created, drawn, true);
    Check(narrow_menu[2].badge_text == l10n::Get(l10n::StringId::DetailsColumnNoRoom) &&
          !narrow_menu[2].tooltip.empty() && narrow_menu[1].badge_text.empty() &&
          narrow_menu[3].badge_text.empty() && narrow_menu[0].badge_text.empty() &&
          search_menu[2].badge_text == l10n::Get(l10n::StringId::DetailsColumnNotInSearch) &&
          BuildDetailsColumnMenu(with_created)[2].badge_text.empty(),
          L"optional columns: a chosen column the pane cannot show says why");
    const uint32_t drawn_plain = drawn & ~ui::kDetailsColumnStatus;
    Check(BuildDetailsColumnMenu(with_created, drawn_plain, false)[6].badge_text ==
              l10n::Get(l10n::StringId::DetailsColumnCloudOnly) &&
              BuildDetailsColumnMenu(with_created, drawn, false)[6].badge_text.empty(),
          L"cloud status: outside cloud folders the menu says it is for cloud folders only");
    AppPrefs legacy;
    legacy.persist = false;
    legacy.FromJson(L"{\"details_columns\":28}");
    AppPrefs current;
    current.persist = false;
    current.FromJson(L"{\"details_columns\":28,\"details_columns_v\":2}");
    Check(legacy.details_columns == (28u | ui::kDetailsColumnStatus) && current.details_columns == 28u,
          L"cloud status: masks saved before v2 get status once, newer masks are kept");

    // Sort menu: the new dates appear for folders, not for search results.
    BackgroundViewOptions folder_options;
    BackgroundViewOptions search_options;
    search_options.show_path = true;
    auto has_command = [](const std::vector<ui::FluentMenuItem>& items, int command) {
        return std::any_of(items.begin(), items.end(), [&](const auto& item) { return item.command == command; });
    };
    folder_options.sort_column = SortColumn::Created;
    const auto folder_sort = BuildSortMenu(folder_options);
    const auto search_sort = BuildSortMenu(search_options);
    const auto created_row = std::find_if(folder_sort.begin(), folder_sort.end(),
        [](const auto& item) { return item.command == CmdSortCreated; });
    Check(created_row != folder_sort.end() && created_row->radio && has_command(folder_sort, CmdSortAccessed) &&
          !has_command(search_sort, CmdSortCreated) && !has_command(search_sort, CmdSortAccessed),
          L"optional columns: sort menu offers created / accessed for folders only");
}

} // namespace

// Staging tray card stack (v1.0.40): cyclic window, top-card-only hit
// testing, hold-and-fling to the back, spring back on a short drag, wheel and
// footer paging, dismiss tumble + smoke, clear.
void TrayStackSettle(AppState& s, int ms) {
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (GetTickCount64() < until) {
        TickTrayDeck(s);
        Sleep(16);
    }
    TickTrayDeck(s);
}

void TestTrayStack() {
    Check(!l10n::Get(l10n::StringId::StagingTrayEmpty).empty() &&
          !l10n::Get(l10n::StringId::TrayPrev).empty() &&
          !l10n::Get(l10n::StringId::TrayNext).empty() &&
          !l10n::Get(l10n::StringId::TrayFlingHint).empty(),
          L"tray stack: new strings resolve (localization range covers them)");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"PulseTrayStackSelftest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> files;
    for (int i = 0; i < 5; ++i) {
        const std::wstring p = dir + L"\\card" + std::to_wstring(i) + L".txt";
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, "pulse", 5, &written, nullptr);
            CloseHandle(h);
        }
        files.push_back(p);
    }

    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    state->tray.Collect(files, false);
    Check(TrayItemTotalCount(state->tray) == 5, L"tray stack: five staged items");
    {
        const auto all = TrayDeckEntries(state->tray, 0, 5);
        const auto wrap = TrayDeckEntries(state->tray, 3, 4);
        bool cyclic = all.size() == 5 && wrap.size() == 4;
        for (size_t k = 0; cyclic && k < wrap.size(); ++k)
            cyclic = wrap[k].item && all[(3 + k) % 5].item &&
                     wrap[k].item->path == all[(3 + k) % 5].item->path;
        Check(cyclic, L"tray stack: window wraps around the end of the stack");
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = BlankPaneTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseTrayStackSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"tray stack: hidden test window created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    if (state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(1.0f);
        state->window_tabs.NewTab(L"C:\\PulseTrayStackFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        TrayStackSettle(*state, 900);
        auto vm = BuildVm(*state, false);
        const auto& deck = vm.tray_deck;
        Check(deck.total_count == 5 && deck.live_count >= 3,
              L"tray stack: view model carries the visible window");
        const auto first = TrayDeckEntries(state->tray, 0, 1);
        Check(deck.live_count >= 3 && !first.empty() &&
              deck.cards[0].path == first.front().item->path &&
              deck.cards[0].depth < 0.05f && std::abs(deck.cards[1].depth - 1.0f) < 0.05f &&
              std::abs(deck.cards[2].depth - 2.0f) < 0.05f,
              L"tray stack: top card first, two layers peek underneath");

        // The panel is the shelf (b7b1cd5): a fan of the staged items to drag
        // out, the release split button, and a chevron that lists every item.
        BYTE saved_keys[256]{};
        GetKeyboardState(saved_keys);
        const auto scan_shelf = [&](const ui::WindowViewModel& model, std::map<int, D2D1_RECT_F>& zones) {
            zones.clear();
            const D2D1_RECT_F area = state->renderer.StagingTrayRect(model, 1000.0f, 700.0f);
            for (float y = area.top; y < area.bottom; y += 2.0f) {
                for (float x = area.left; x < area.right; x += 2.0f) {
                    const auto hit = state->renderer.HitTest(model, D2D1::RectF(0, 0, 1000, 700), x, y);
                    if (hit.region != ui::HitTestResult::TrayShelf) continue;
                    auto [it, fresh] = zones.emplace(hit.index, D2D1::RectF(x, y, x, y));
                    if (!fresh) {
                        it->second.left = std::min(it->second.left, x); it->second.top = std::min(it->second.top, y);
                        it->second.right = std::max(it->second.right, x); it->second.bottom = std::max(it->second.bottom, y);
                    }
                }
            }
        };
        const auto zone_list = [](const std::map<int, D2D1_RECT_F>& zones) {
            std::wstring text;
            for (const auto& [k, rc] : zones) text += L" " + std::to_wstring(k);
            return text;
        };
        const auto has_zone = [](const std::map<int, D2D1_RECT_F>& zones, ui::TrayShelfHit h) {
            return zones.count(static_cast<int>(h)) != 0;
        };
        const auto click_zone = [&](const std::map<int, D2D1_RECT_F>& zones, ui::TrayShelfHit h) {
            const auto it = zones.find(static_cast<int>(h));
            if (it == zones.end()) return;
            const LPARAM pt = MAKELPARAM(static_cast<int>((it->second.left + it->second.right) * 0.5f),
                                         static_cast<int>((it->second.top + it->second.bottom) * 0.5f));
            SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt);
            SendMessageW(hwnd, WM_LBUTTONUP, 0, pt);
        };
        const D2D1_RECT_F panel = state->renderer.StagingTrayRect(vm, 1000.0f, 700.0f);
        Check(panel.bottom - panel.top > 60.0f, L"tray stack: panel has room for the stack");
        std::map<int, D2D1_RECT_F> zones;
        scan_shelf(vm, zones);
        Check(has_zone(zones, ui::TrayShelfHit::Fan) && has_zone(zones, ui::TrayShelfHit::Release) &&
              has_zone(zones, ui::TrayShelfHit::ReleaseMenu) && has_zone(zones, ui::TrayShelfHit::Expand),
              (L"tray shelf: fan, release split button and expand hit-test (zones" + zone_list(zones) + L")").c_str());
        click_zone(zones, ui::TrayShelfHit::Expand);
        Check(state->trayView.open, L"tray shelf: expand opens the item list");
        TrayStackSettle(*state, 600);
        vm = BuildVm(*state, false);
        scan_shelf(vm, zones);
        Check(has_zone(zones, ui::TrayShelfHit::Row) && has_zone(zones, ui::TrayShelfHit::Collapse),
              (L"tray shelf: open list shows item rows and collapse (zones" + zone_list(zones) + L")").c_str());
        click_zone(zones, ui::TrayShelfHit::Collapse);
        Check(!state->trayView.open, L"tray shelf: collapse returns to the shelf");
        TrayStackSettle(*state, 600);
        vm = BuildVm(*state, false);

        // Clear: every visible card tumbles off, staggered; nothing lingers.
        std::vector<std::wstring> all;
        for (const auto& b : state->tray.batches())
            for (const auto& item : b.items) all.push_back(item.path);
        MarkTrayExit(*state, all, true);
        state->tray.Clear();
        TickTrayDeck(*state);
        vm = BuildVm(*state, false);
        int ghosts = 0;
        for (const auto& c : vm.tray_deck.cards) if (c.ghost) ++ghosts;
        Check(vm.tray_deck.live_count == 0 && ghosts >= 3 && ghosts <= 8,
              L"tray stack: clear tumbles the visible cards");
        TrayStackSettle(*state, 1300);
        vm = BuildVm(*state, false);
        Check(vm.tray_deck.cards.empty() && state->trayCards.empty(),
              L"tray stack: clear leaves no animation state behind");

        // The same file staged twice (Ctrl+C again, or once to copy and once
        // to move) settles as one card. Two cards would share the path's
        // animation entry and pull it toward two depths on every tick, so the
        // stack never came to rest and its text shimmered.
        state->tray.Collect({ files[0] }, false);
        state->tray.Collect({ files[0] }, true);
        TrayStackSettle(*state, 900);
        vm = BuildVm(*state, false);
        const bool duplicate_moving = TickTrayDeck(*state);
        Check(!duplicate_moving && vm.tray_deck.live_count == 1 && vm.tray_deck.cards.size() == 1 &&
              vm.tray_deck.cards[0].depth < 0.05f,
              (L"tray stack: a file staged twice settles as one card (moving=" + std::to_wstring(duplicate_moving) +
               L" live=" + std::to_wstring(vm.tray_deck.live_count) + L")").c_str());
        state->tray.Clear();
        SetKeyboardState(saved_keys);
    } else Check(false, L"tray stack: graphics initialized");
    if (GetCapture() == hwnd) ReleaseCapture();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    for (const auto& f : files) DeleteFileW(f.c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestLinkResolve() {
    Check(fs::StripLnkSuffix(L"计算图形.dwg.lnk") == L"计算图形.dwg",
          L"link: strip suffix");
    Check(fs::StripLnkSuffix(L"note.txt") == L"note.txt",
          L"link: strip keeps non-lnk name");
    Check(fs::StripLnkSuffix(L"A.LNK") == L"A", L"link: strip is case-insensitive");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring dir = std::wstring(temp) + L"PulseLinkTest-" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring target_file = dir + L"\\target.txt";
    const std::wstring target_dir = dir + L"\\folder";
    const std::wstring lnk_file = dir + L"\\target.txt.lnk";
    const std::wstring lnk_dir = dir + L"\\folder.lnk";
    const std::wstring lnk_dead = dir + L"\\dead.lnk";
    const char body[] = "hello";
    HANDLE hf = CreateFileW(target_file.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(hf, body, sizeof(body) - 1, &written, nullptr);
    CloseHandle(hf);
    CreateDirectoryW(target_dir.c_str(), nullptr);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                COINIT_DISABLE_OLE1DDE);
    auto make_lnk = [](const std::wstring& link, const std::wstring& target) {
        Microsoft::WRL::ComPtr<IShellLinkW> sl;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&sl)))) return false;
        if (FAILED(sl->SetPath(target.c_str()))) return false;
        Microsoft::WRL::ComPtr<IPersistFile> pf;
        if (FAILED(sl.As(&pf))) return false;
        return SUCCEEDED(pf->Save(link.c_str(), TRUE));
    };
    Check(make_lnk(lnk_file, target_file) && make_lnk(lnk_dir, target_dir) &&
          make_lnk(lnk_dead, dir + L"\\gone.txt"), L"link: create fixtures");

    fs::DirEntry e1;
    std::error_code link_error;
    // Shell may expand a short TEMP path; compare the referenced file identity.
    Check(ResolveLink(lnk_file, e1) &&
          std::filesystem::equivalent(e1.link_target, target_file, link_error) &&
          !e1.link_target_is_dir && e1.link_target_size == sizeof(body) - 1,
          L"link: file target resolves with size");
    fs::DirEntry e2;
    Check(ResolveLink(lnk_dir, e2) && e2.link_target_is_dir,
          L"link: folder target resolves");
    e1.name = L"a.txt.lnk";
    e2.name = L"z-folder.lnk";
    for (const auto column : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Mtime, ui::SortColumn::Type}) {
        for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc})
            Check(EntryLess(e2, e1, column, direction) && !EntryLess(e1, e2, column, direction),
                L"link: folder shortcuts sort before files in both directions");
    }
    Check(!e2.is_dir, L"link: sorting preserves shortcut file identity for operations");
    // Folder sort modes: the pref only changes how folders group, never the names.
    Check(EntryLess(e2, e1, ui::SortColumn::Name, ui::SortDirection::Asc,
                    FolderSortMode::FoldersFirst) &&
          EntryLess(e2, e1, ui::SortColumn::Name, ui::SortDirection::Desc,
                    FolderSortMode::FoldersFirst),
        L"sort: folders-first keeps folders on top in both directions");
    Check(EntryLess(e2, e1, ui::SortColumn::Name, ui::SortDirection::Asc,
                    FolderSortMode::FollowDirection) &&
          EntryLess(e1, e2, ui::SortColumn::Name, ui::SortDirection::Desc,
                    FolderSortMode::FollowDirection),
        L"sort: follow-direction sends folders to the bottom when reversed");
    Check(!EntryLess(e2, e1, ui::SortColumn::Name, ui::SortDirection::Asc,
                     FolderSortMode::Mixed) &&
          EntryLess(e1, e2, ui::SortColumn::Name, ui::SortDirection::Asc,
                    FolderSortMode::Mixed),
        L"sort: mixed mode orders folders and files by name alone");
    wchar_t live_link[32768]{};
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_LINK", live_link, ARRAYSIZE(live_link))) {
        std::vector<fs::DirEntry> entries;
        const auto parent = fs::ParentPath(live_link);
        fs::EnumerateDirectory(parent, entries);
        ResolveLinksInPlace(parent, entries, {});
        const auto filename = std::filesystem::path(live_link).filename().wstring();
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.name == filename; });
        Check(found != entries.end() && found->link_target_is_dir && !(found->attrs & FILE_ATTRIBUTE_HIDDEN),
            L"link: actual desktop shortcut enumerated and resolved as visible folder");
    }
    fs::DirEntry e3;
    Check(!ResolveLink(lnk_dead, e3) && e3.link_target.empty(),
          L"link: missing target does not penetrate");
    if (com == S_OK) CoUninitialize();

    DeleteFileW(lnk_file.c_str());
    DeleteFileW(lnk_dir.c_str());
    DeleteFileW(lnk_dead.c_str());
    DeleteFileW(target_file.c_str());
    RemoveDirectoryW(target_dir.c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestDetailsMeta() {
    Check(pulse::format::GroupedInt(0) == L"0", L"meta: grouped int zero");
    Check(pulse::format::GroupedInt(12345) == L"12,345", L"meta: grouped int small");
    Check(pulse::format::GroupedInt(2895851315ull) == L"2,895,851,315",
          L"meta: grouped int large");
    Check(FormatAccessMask(FILE_ALL_ACCESS, false) == L"完全控制",
          L"meta: mask full control");
    Check(FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_WRITE, false) == L"修改",
          L"meta: mask modify");
    Check(FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, true) ==
          L"读取和执行、列出文件夹内容、读取", L"meta: mask read-execute dir");
    Check(FormatAccessMask(FILE_GENERIC_WRITE, false) == L"写入", L"meta: mask write only");
    Check(FormatAccessMask(DELETE, false) == L"删除", L"meta: mask delete only");
    Check(FormatAccessMask(0, false) == L"特殊权限", L"meta: mask empty");
}

void TestMoveTabRun() {
    std::vector<int> order{ 0, 1, 2, 3, 4, 5 };
    Check(MoveTabRun(order, 1, 2, -1) == 0 &&
          order == (std::vector<int>{ 1, 2, 0, 3, 4, 5 }),
          L"tabrun: rotate left moves block");
    Check(MoveTabRun(order, 0, 2, 1) == 1 &&
          order == (std::vector<int>{ 0, 1, 2, 3, 4, 5 }),
          L"tabrun: rotate right restores");
    Check(MoveTabRun(order, 2, 2, 1) == 3 &&
          order == (std::vector<int>{ 0, 1, 4, 2, 3, 5 }),
          L"tabrun: rotate right displaces single");
    order = { 0, 1, 2, 3 };
    Check(MoveTabRun(order, 0, 2, -1) == 0 &&
          order == (std::vector<int>{ 0, 1, 2, 3 }),
          L"tabrun: left edge is a no-op");
    Check(MoveTabRun(order, 2, 2, 1) == 2 &&
          order == (std::vector<int>{ 0, 1, 2, 3 }),
          L"tabrun: right edge is a no-op");
    Check(MoveTabRun(order, 0, 1, 1) == 1 &&
          order == (std::vector<int>{ 1, 0, 2, 3 }),
          L"tabrun: len-1 degenerates to a swap");

    const std::vector<int> right_groups{ 7, 7, 0 };
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, right_groups, 1, 1) &&
          order == (std::vector<int>{ 2, 0, 1 }),
          L"tabgroup: member crossing right free tab moves the whole group");
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, right_groups, 0, 1) &&
          order == (std::vector<int>{ 2, 0, 1 }),
          L"tabgroup: any member preserves its group when crossing right");

    const std::vector<int> left_groups{ 0, 7, 7 };
    order = { 0, 1, 2 };
    Check(MoveTabGroupAcrossFreeTab(order, left_groups, 1, -1) &&
          order == (std::vector<int>{ 1, 2, 0 }),
          L"tabgroup: member crossing left free tab moves the whole group");
    order = { 0, 1, 2 };
    Check(!MoveTabGroupAcrossFreeTab(order, left_groups, 1, 1) &&
          order == (std::vector<int>{ 0, 1, 2 }),
          L"tabgroup: move rejects a non-free outside neighbor");
}

void TestNormalizeGroupRuns() {
    auto make = [](int group) {
        auto t = std::make_unique<LayoutTab>();
        t->tab_group = group;
        return t;
    };
    {   // Split run: 1,0,1,2 -> 1,1,0,2 and active tab follows its pointer.
        WindowTabs tabs;
        tabs.items.push_back(make(1)); tabs.items.push_back(make(0));
        tabs.items.push_back(make(1)); tabs.items.push_back(make(2));
        tabs.active = 2;
        const LayoutTab* active = tabs.items[2].get();
        NormalizeGroupRuns(tabs);
        Check(tabs.items.size() == 4 &&
              tabs.items[0]->tab_group == 1 && tabs.items[1]->tab_group == 1 &&
              tabs.items[2]->tab_group == 0 && tabs.items[3]->tab_group == 2,
              L"tabgroup: split run collapses into one run");
        Check(tabs.Active() == active, L"tabgroup: active tab survives normalize");
    }
    {   // Interleaved groups: 1,2,1,2 -> 1,1,2,2 (first-appearance order kept).
        WindowTabs tabs;
        for (int g : { 1, 2, 1, 2 }) tabs.items.push_back(make(g));
        NormalizeGroupRuns(tabs);
        Check(tabs.items[0]->tab_group == 1 && tabs.items[1]->tab_group == 1 &&
              tabs.items[2]->tab_group == 2 && tabs.items[3]->tab_group == 2,
              L"tabgroup: interleaved groups normalize pairwise");
    }
    {   // Already contiguous: untouched.
        WindowTabs tabs;
        for (int g : { 0, 1, 1, 0 }) tabs.items.push_back(make(g));
        NormalizeGroupRuns(tabs);
        Check(tabs.items[0]->tab_group == 0 && tabs.items[2]->tab_group == 1 &&
              tabs.items[3]->tab_group == 0, L"tabgroup: contiguous runs untouched");
    }
}

void TestChipBlockDragGeometry() {
    Check(CollapsedChipBlockW(24.0f, 6.0f) == 30.0f,
          L"chipdrag: block width is chip plus gap");
    Check(CollapsedChipBlockW(0.0f, 4.0f) == 4.0f,
          L"chipdrag: zero-width chip still reserves the gap");
    // Moving left: the block's left edge must pass the neighbor center.
    Check(ChipBlockCrossed(100.0f, 30.0f, 120.0f, -1),
          L"chipdrag: left edge past center crosses");
    Check(!ChipBlockCrossed(125.0f, 30.0f, 120.0f, -1),
          L"chipdrag: left edge short of center holds");
    // Moving right: the block's right edge must pass the neighbor center.
    Check(ChipBlockCrossed(100.0f, 30.0f, 120.0f, 1),
          L"chipdrag: right edge past center crosses");
    Check(!ChipBlockCrossed(100.0f, 30.0f, 140.0f, 1),
          L"chipdrag: right edge short of center holds");
    Check(!ChipBlockCrossed(100.0f, 30.0f, 120.0f, 0),
          L"chipdrag: zero direction never crosses");
    // Displaced unit track start = where it sits minus where it must land.
    Check(DisplacedRestDelta(200.0f, 0.0f, 260.0f) == -60.0f,
          L"chipdrag: displaced unit starts one block back");
    Check(DisplacedRestDelta(200.0f, -30.0f, 170.0f) == 0.0f,
          L"chipdrag: in-flight slide that already arrived needs no track");
    Check(DisplacedRestDelta(170.0f, 30.0f, 260.0f) == -60.0f,
          L"chipdrag: in-flight slide composes with the new delta");
}

void TestFindGroupRun() {
    const std::vector<int> gids{ 0, 7, 7, 7, 0 }; // group 7 = tabs 1..3
    {   // Identity order: the run spans display positions 1..3.
        const std::vector<int> order{ 0, 1, 2, 3, 4 };
        const GroupRun r = FindGroupRun(order, gids, 2, 7);
        Check(r.pos == 1 && r.len == 3, L"grouprun: run bounds from middle member");
        Check(FindGroupRun(order, gids, 1, 7).pos == 1 &&
              FindGroupRun(order, gids, 3, 7).pos == 1,
              L"grouprun: same run from any member");
    }
    {   // Permuted order (mid-drag): lookup follows order indirection.
        const std::vector<int> order{ 3, 0, 1, 2, 4 };
        const GroupRun r = FindGroupRun(order, gids, 2, 7);
        Check(r.pos == 2 && r.len == 2, L"grouprun: permuted run after split");
        const GroupRun head = FindGroupRun(order, gids, 0, 7);
        Check(head.pos == 0 && head.len == 1, L"grouprun: split-off member is its own run");
    }
    {   // Rejections: ungrouped tab, wrong gid, out of range.
        const std::vector<int> order{ 0, 1, 2, 3, 4 };
        Check(FindGroupRun(order, gids, 0, 0).len == 0,
              L"grouprun: gid 0 is never a run");
        Check(FindGroupRun(order, gids, 0, 7).len == 0,
              L"grouprun: tab at pos not in gid");
        Check(FindGroupRun(order, gids, -1, 7).len == 0 &&
              FindGroupRun(order, gids, 5, 7).len == 0,
              L"grouprun: out-of-range pos rejected");
        Check(FindGroupRun(order, gids, 2, 9).len == 0,
              L"grouprun: unknown gid rejected");
    }
    {   // Whole-strip group: run reaches both edges.
        const std::vector<int> g2{ 4, 4, 4 };
        const std::vector<int> order{ 0, 1, 2 };
        const GroupRun r = FindGroupRun(order, g2, 0, 4);
        Check(r.pos == 0 && r.len == 3, L"grouprun: edge-to-edge run");
    }
    {   // Hop keeps contiguity: MoveTabRun on the found run never inserts the
        // dragged tab between members.
        const std::vector<int> g3{ 0, 5, 5, 0 };
        std::vector<int> order{ 0, 1, 2, 3 }; // tab 0 drags right into group 5
        const GroupRun r = FindGroupRun(order, g3, 1, 5);
        MoveTabRun(order, r.pos, r.len, -1); // run slides left past tab 0
        Check(order == (std::vector<int>{ 1, 2, 0, 3 }),
              L"grouprun: hop lands the tab past the whole run");
    }
}

bool SameLayoutTabs(const std::vector<LayoutTabSnapshot>& a,
                    const std::vector<LayoutTabSnapshot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pinned != b[i].pinned || a[i].group != b[i].group ||
            a[i].title != b[i].title || a[i].marker_rgb != b[i].marker_rgb ||
            a[i].layout != b[i].layout ||
            a[i].focused != b[i].focused || a[i].target != b[i].target ||
            a[i].split_ratios != b[i].split_ratios ||
            a[i].panes.size() != b[i].panes.size()) return false;
        for (size_t p = 0; p < a[i].panes.size(); ++p) {
            if (a[i].panes[p].path != b[i].panes[p].path ||
                a[i].panes[p].view != b[i].panes[p].view ||
                a[i].panes[p].columns != b[i].panes[p].columns ||
                a[i].panes[p].search_columns != b[i].panes[p].search_columns)
                return false;
        }
    }
    return true;
}

void TestSessionLayoutTabs() {
    {   // Round-trip: two layout tabs, groups, special characters, split ratios.
        std::vector<LayoutTabSnapshot> tabs(2);
        tabs[0].pinned = true;
        tabs[0].group = 1;
        tabs[0].title = L"工作";
        tabs[0].marker_rgb = 0x0078D4;
        tabs[0].layout = 1;
        tabs[0].focused = 1;
        tabs[0].target = 0;
        tabs[0].split_ratios = { 0.42f };
        tabs[0].panes.push_back({ L"C:\\", ui::ViewMode::Details, { 0.42f, 0.61f, 0.82f },
                                  { 0.28f, 0.52f, 0.70f, 0.86f } });
        tabs[0].panes.push_back({ L"D:\\代码\\路径 \"quoted\"\\dir", ui::ViewMode::LargeIcons });
        tabs[1].group = 2;
        tabs[1].layout = 0;
        tabs[1].title = L"引\"号\\组\n名";
        tabs[1].panes.push_back({ L"\\\\?\\UNC\\server\\share\\dir", ui::ViewMode::List });

        const std::wstring json = LayoutTabsToJson(tabs);
        std::vector<LayoutTabSnapshot> back;
        Check(ParseLayoutTabs(json, back), L"layouttabs: round-trip parses");
        Check(!back.empty() && back[0].marker_rgb == 0x0078D4,
              L"layouttabs: individual tab color survives JSON round-trip");
        Check(SameLayoutTabs(tabs, back), L"layouttabs: round-trip preserves all fields");
    }
    {   // Missing searchCols still loads (older sessions).
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(
            L"[{\"pinned\":false,\"group\":0,\"layout\":0,\"focused\":0,\"panes\":["
            L"{\"path\":\"C:\\\\\",\"view\":\"details\",\"cols\":\"4200,6100,8200\"}]}]", out) &&
            !out.empty() && !out[0].panes.empty() &&
            out[0].panes[0].columns[0] > 0.0f &&
            out[0].panes[0].search_columns[0] == 0.0f,
              L"layouttabs: older JSON without searchCols stays default");
    }
    {   // Created / accessed widths add two cols values only when set, so
        // sessions without them stay readable by older builds.
        std::vector<LayoutTabSnapshot> tabs(1), out;
        tabs[0].panes.push_back({ L"C:\\", ui::ViewMode::Details, { 140.0f, 0.0f, 90.0f, 160.0f, 0.0f } });
        Check(LayoutTabsToJson(tabs).find(L"\"cols\":\"1400000,0,900000,1600000,0\"") != std::wstring::npos &&
              ParseLayoutTabs(LayoutTabsToJson(tabs), out) && SameLayoutTabs(tabs, out),
              L"layouttabs: created / accessed column widths round-trip");
        tabs[0].panes[0].columns = { 140.0f, 0.0f, 90.0f };
        Check(LayoutTabsToJson(tabs).find(L"\"cols\":\"1400000,0,900000\"") != std::wstring::npos,
              L"layouttabs: classic column widths keep the three-value format");
        out.clear();
        Check(ParseLayoutTabs(
            L"[{\"panes\":[{\"path\":\"C:\\\\\",\"view\":\"details\",\"cols\":\"1400000,0,900000,7\"}]}]", out) &&
            out.size() == 1 && out[0].panes.size() == 1 && out[0].panes[0].columns[0] == 0.0f,
              L"layouttabs: malformed column widths fall back to automatic");
    }
    {   // Empty array and empty layout tab.
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(L"[]", out) && out.empty(),
              L"layouttabs: empty array");
        std::vector<LayoutTabSnapshot> tabs(1);
        out.clear();
        Check(ParseLayoutTabs(LayoutTabsToJson(tabs), out) &&
              SameLayoutTabs(tabs, out),
              L"layouttabs: empty tab round-trips");
    }
    {   // Unknown view falls back to details; out-of-range focused survives parsing.
        std::vector<LayoutTabSnapshot> out;
        Check(ParseLayoutTabs(
                  L"[{\"layout\":1,\"focused\":99,\"panes\":["
                  L"{\"path\":\"C:\\\\x\",\"view\":\"bogus\"}]}]",
                  out) &&
              out.size() == 1 && out[0].focused == 99 && out[0].layout == 1 &&
              out[0].panes.size() == 1 && out[0].panes[0].path == L"C:\\x" &&
              out[0].panes[0].view == ui::ViewMode::Details,
              L"layouttabs: unknown view + focused parsed as-is");
    }
    {   // Corrupt input: rejects or skips without crashing, never throws.
        std::vector<LayoutTabSnapshot> out;
        Check(!ParseLayoutTabs(L"", out), L"layouttabs: empty string rejected");
        Check(!ParseLayoutTabs(L"not json", out), L"layouttabs: garbage rejected");
        Check(!ParseLayoutTabs(L"[{\"layout\":1,\"panes\":[{\"path\":\"C:\\\\\"}]", out),
              L"layouttabs: truncated array rejected");
        Check(ParseLayoutTabs(L"[{\"panes\":[{}]}]", out) && out.size() == 1 &&
              out[0].panes.size() == 1 && out[0].panes[0].path.empty(),
              L"layouttabs: empty object tolerated");
    }
}

// Settings > 快捷键: the keymap table, prefs lines, translation and checks.
void TestKeymap() {
    namespace k = pulse::app;
    using k::KeyChord;
    using k::KeyAction;
    using T = k::Keymap::Translation;
    const uint8_t C = k::kKeyCtrl, A = k::kKeyAlt, S = k::kKeyShift;
    auto chord = [](const wchar_t* text) { KeyChord c{}; k::ParseChord(text, c); return c; };

    k::Keymap map;
    Check(map.AllDefault() && map.ToPrefs().empty(), L"keymap: defaults store nothing");
    Check(map.Translate(KeyChord{static_cast<uint8_t>(C | S), 'C'}).kind == T::Pass &&
          map.Translate(KeyChord{0, VK_F4}).kind == T::Pass &&
          map.Translate(KeyChord{A, 'D'}).kind == T::Pass,
          L"keymap: default keys pass through untouched");
    bool ids_ok = true;
    for (int i = 0; i < k::kKeyActionCount; ++i) {
        const auto& info = k::ActionInfo(static_cast<KeyAction>(i));
        ids_ok = ids_ok && info.action == static_cast<KeyAction>(i) && k::ActionFromId(info.id) == info.action;
    }
    Check(ids_ok, L"keymap: table order and ids match KeyAction");
    Check(k::ActionFromId("NoSuchAction") == KeyAction::Count, L"keymap: unknown id");

    // Every default key reaches exactly its own action (no duplicates in the table).
    bool unique = true;
    for (int i = 0; i < k::kKeyActionCount; ++i) {
        const auto action = static_cast<KeyAction>(i);
        if (k::ActionInfo(action).kind == k::KeyKind::Global) continue;
        for (const KeyChord c : map.EffectiveChords(action)) {
            const auto owner = map.Find(c);
            if (owner.action != action) {
                unique = false;
                LogLine(L"  keymap duplicate: %s -> %d (want %d)\n", k::FormatChord(c).c_str(),
                        static_cast<int>(owner.action), i);
            }
        }
    }
    Check(unique, L"keymap: default keys are unique");

    for (const wchar_t* text : {L"Ctrl+Alt+Shift+C", L"F8", L"Shift+F10", L"Ctrl+#C0", L"Alt+Left", L"Ctrl+Tab", L"Apps"}) {
        KeyChord c{};
        const bool ok = k::ParseChord(text, c) && k::FormatChord(c) == text;
        if (!ok) LogLine(L"  keymap round trip failed: %s\n", text);
        Check(ok, L"keymap: chord text round trip");
    }
    KeyChord bad{};
    Check(!k::ParseChord(L"Ctrl+", bad) && !k::ParseChord(L"C+Ctrl", bad) && !k::ParseChord(L"Ctrl+Shift", bad) &&
          !k::ParseChord(L"F25", bad) && !k::ParseChord(L"", bad) && !k::ParseChord(L"Ctrl+Bogus", bad),
          L"keymap: malformed chords rejected");
    Check(chord(L"ctrl+shift+c") == KeyChord{static_cast<uint8_t>(C | S), 'C'}, L"keymap: parsing ignores case");

    // 复制到目标栏 moved to F8 (the reported Ctrl+Alt+C conflict).
    map.SetBindings(KeyAction::CopyToTarget, {KeyChord{0, VK_F8}});
    const auto lines = map.ToPrefs();
    Check(lines.size() == 1 && lines[0] == L"CopyToTarget=F8", L"keymap: only the change is stored");
    const k::Keymap reread = k::Keymap::FromPrefs(lines);
    Check(reread.Bindings(KeyAction::CopyToTarget) == std::vector<KeyChord>{KeyChord{0, VK_F8}} &&
          !reread.IsDefault(KeyAction::CopyToTarget) && reread.IsDefault(KeyAction::MoveToTarget),
          L"keymap: prefs lines read back");
    const T moved = map.Translate(KeyChord{0, VK_F8});
    Check(moved.kind == T::Rewrite && moved.target == KeyChord{static_cast<uint8_t>(C | A), 'C'} &&
          moved.action == KeyAction::CopyToTarget, L"keymap: the new key becomes the default key");
    const T old_key = map.Translate(KeyChord{static_cast<uint8_t>(C | A), 'C'});
    Check(old_key.kind == T::Swallow && old_key.action == KeyAction::CopyToTarget,
          L"keymap: the old default key no longer acts");
    map.Reset(KeyAction::CopyToTarget);
    Check(map.AllDefault(), L"keymap: reset restores the default");

    // A second key next to the default, and an action without any key.
    map.SetBindings(KeyAction::Refresh, {KeyChord{0, VK_F5}, KeyChord{C, 'R'}});
    Check(map.Translate(KeyChord{C, 'R'}).kind == T::Rewrite && map.Translate(KeyChord{0, VK_F5}).kind == T::Pass,
          L"keymap: extra key beside the default");
    const k::Keymap cleared = k::Keymap::FromPrefs({L"CopyPath="});
    Check(cleared.Bindings(KeyAction::CopyPath).empty() &&
          cleared.Translate(KeyChord{static_cast<uint8_t>(C | S), 'C'}).kind == T::Swallow &&
          cleared.ToPrefs() == std::vector<std::wstring>{L"CopyPath="},
          L"keymap: an action can be left without a key");
    const k::Keymap junk = k::Keymap::FromPrefs({L"Bogus=F8", L"CopyPath=Nope", L"=", L"Layout=Shift", L"Open=F9"});
    Check(junk.AllDefault(), L"keymap: unknown, malformed and fixed entries are ignored");

    // Numbered groups: only the modifiers move.
    k::Keymap ranges;
    ranges.SetRangeMods(KeyAction::TabNumber, A);
    const T tab3 = ranges.Translate(KeyChord{A, '3'});
    Check(tab3.kind == T::Rewrite && tab3.target == KeyChord{C, '3'} && tab3.action == KeyAction::TabNumber,
          L"keymap: Alt+3 reaches tab 3");
    Check(ranges.Translate(KeyChord{C, '3'}).kind == T::Swallow, L"keymap: Ctrl+3 is free after the move");
    Check(ranges.ToPrefs() == std::vector<std::wstring>{L"TabNumber=Alt"} &&
          k::Keymap::FromPrefs({L"TabNumber=Alt"}).RangeMods(KeyAction::TabNumber) == A,
          L"keymap: range modifiers stored");

    // Shift steps backwards for Ctrl+Tab and the window switcher.
    k::Keymap tabs;
    tabs.SetBindings(KeyAction::NextTab, {KeyChord{C, VK_NEXT}});
    const T back = tabs.Translate(KeyChord{static_cast<uint8_t>(C | S), VK_NEXT});
    Check(back.kind == T::Rewrite && back.target == KeyChord{static_cast<uint8_t>(C | S), VK_TAB},
          L"keymap: Shift with the new key steps backwards");
    Check(tabs.Translate(KeyChord{static_cast<uint8_t>(C | S), VK_TAB}).kind == T::Swallow,
          L"keymap: Ctrl+Shift+Tab moves with Ctrl+Tab");

    // Conflict checks.
    k::ShortcutEnv env;
    env.taken = {KeyChord{static_cast<uint8_t>(C | A), 'C'}, KeyChord{static_cast<uint8_t>(C | A), 'X'}};
    env.chinese_ime = true;
    k::Keymap m;
    auto issue = [&](KeyAction action, const wchar_t* text) { return k::CheckChord(m, action, chord(text), env).issue; };
    using I = k::ChordIssue;
    Check(issue(KeyAction::CopyPath, L"Alt+F4") == I::Reserved, L"keymap check: Alt+F4 reserved");
    Check(k::CheckChord(m, KeyAction::CopyPath, KeyChord{0, 'Q'}, env).issue == I::PlainKey &&
          k::CheckChord(m, KeyAction::CopyPath, KeyChord{S, '5'}, env).issue == I::PlainKey,
          L"keymap check: single letters and digits rejected");
    Check(issue(KeyAction::CopyPath, L"Win+F") == I::WinKey && issue(KeyAction::GlobalSearch, L"Win+Shift+F") == I::None,
          L"keymap check: Win only for the global hotkey");
    Check(issue(KeyAction::CopyToTarget, L"Ctrl+Alt+X") == I::Taken, L"keymap check: another program's hotkey");
    const auto dup = k::CheckChord(m, KeyAction::CopyPath, chord(L"Ctrl+Shift+F"), env);
    Check(dup.issue == I::Duplicate && dup.level == k::ChordLevel::Warn && dup.other.action == KeyAction::AdvancedSearch,
          L"keymap check: another action's key can be replaced");
    Check(issue(KeyAction::CopyPath, L"Ctrl+3") == I::DuplicateFixed && issue(KeyAction::CopyPath, L"Down") == I::DuplicateFixed,
          L"keymap check: numbered groups and fixed keys cannot be replaced");
    m.SetBindings(KeyAction::AdvancedSearch, {chord(L"Ctrl+Alt+F")});
    Check(issue(KeyAction::CopyPath, L"Ctrl+Shift+F") == I::Ime, L"keymap check: IME key warns");
    env.ime_ignored = {chord(L"Ctrl+Shift+F")};
    Check(issue(KeyAction::CopyPath, L"Ctrl+Shift+F") == I::None, L"keymap check: dismissed IME warning");
    env.ime_ignored.clear();
    Check(issue(KeyAction::CopyPath, L"Ctrl+Y") == I::TextEdit, L"keymap check: text editing key is a note");
    Check(issue(KeyAction::CopyPath, L"Ctrl+Shift+C") == I::Same, L"keymap check: same key");
    Check(issue(KeyAction::SwitchWindow, L"Alt+Q") == I::NeedsCtrl && issue(KeyAction::SwitchWindow, L"Ctrl+Shift+Q") == I::ShiftReserved,
          L"keymap check: the switcher needs Ctrl, Shift steps back");
    Check(issue(KeyAction::NextTab, L"Ctrl+Shift+PgDn") == I::ShiftReserved, L"keymap check: Shift reserved for backwards");
    Check(issue(KeyAction::GlobalSearch, L"Alt+Space") != I::Reserved && issue(KeyAction::CopyPath, L"Alt+Space") == I::Reserved,
          L"keymap check: Alt+Space only for the global hotkey");

    const auto overlap = k::CheckRange(m, KeyAction::ViewMode, C, env);
    Check(overlap.issue == I::RangeBad && overlap.bad == 8, L"keymap range: Ctrl+1-8 collide with tabs");
    Check(k::CheckRange(m, KeyAction::TabNumber, S, env).issue == I::RangeNeedsMod, L"keymap range: needs Ctrl or Alt");
    const auto free_range = k::CheckRange(m, KeyAction::Layout, static_cast<uint8_t>(A | S), env);
    Check(free_range.level == k::ChordLevel::Ok && free_range.digits.size() == 4, L"keymap range: Alt+Shift+1-4 free");
    Check(k::CheckRange(m, KeyAction::Layout, static_cast<uint8_t>(C | A), env).issue == I::RangeSame,
          L"keymap range: current modifiers");

    k::Keymap fresh;
    const auto issues = k::CollectIssues(fresh, env);
    Check(issues.size() == 3 && issues[0].level == k::ChordLevel::Block && issues[1].level == k::ChordLevel::Block &&
          issues[2].issue == I::Ime && issues[2].action == KeyAction::AdvancedSearch,
          L"keymap: issues list (taken first, then the IME)");
    const auto suggestions = k::SuggestChords(fresh, KeyAction::CopyToTarget, env);
    bool suggestions_ok = suggestions.size() == 3;
    for (const KeyChord c : suggestions)
        suggestions_ok = suggestions_ok && k::CheckChord(fresh, KeyAction::CopyToTarget, c, env).level == k::ChordLevel::Ok;
    Check(suggestions_ok && suggestions[0] == chord(L"Ctrl+Alt+Shift+C"), L"keymap: three free suggestions");
    const auto probe = k::ProbeCandidates(fresh);
    Check(std::find(probe.begin(), probe.end(), chord(L"Ctrl+Alt+C")) != probe.end() && probe.size() > 60,
          L"keymap: probe list covers the keys in effect");

    // Messages the handlers expect.
    Check(k::KeyMessageFor(chord(L"Alt+D")) == WM_SYSKEYDOWN && k::KeyMessageFor(chord(L"Ctrl+Alt+C")) == WM_KEYDOWN,
          L"keymap input: Alt alone arrives as a system key");
    const LPARAM lp = k::KeyMessageLParam(KeyChord{C, k::SwitcherVirtualKey()}, 1);
    Check(((lp >> 16) & 0xFF) == 0x29 && !(lp & (LPARAM{1} << 29)), L"keymap input: switcher key keeps scan code 0x29");
    Check((k::KeyMessageLParam(chord(L"Alt+Left"), 1) & (LPARAM{1} << 29)) &&
          (k::KeyMessageLParam(chord(L"Alt+Left"), 1) & (LPARAM{1} << 24)),
          L"keymap input: context and extended bits");
    {
        BYTE before[256]{};
        GetKeyboardState(before);
        {
            k::ModifierOverride over(static_cast<uint8_t>(C | A));
            Check((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000),
                  L"keymap input: modifiers reported while handled");
        }
        Check(((GetKeyState(VK_CONTROL) & 0x8000) != 0) == ((before[VK_CONTROL] & 0x80) != 0) &&
              ((GetKeyState(VK_MENU) & 0x8000) != 0) == ((before[VK_MENU] & 0x80) != 0),
              L"keymap input: modifiers restored");
    }
}

void TestTabShortcuts() {
    WindowTabs tabs;
    Check(!TabShortcutTarget(tabs, false), L"tab shortcuts: empty strip has no target");
    tabs.items.push_back(std::make_unique<LayoutTab>());
    Check(TabShortcutTarget(tabs, false) == 0 && TabShortcutTarget(tabs, true) == 0,
          L"tab shortcuts: single tab stays selected in both directions");
    for (int i = 0; i < 3; ++i) tabs.items.push_back(std::make_unique<LayoutTab>());
    Check(TabShortcutTarget(tabs, false) == 1 && TabShortcutTarget(tabs, true) == 3,
          L"tab shortcuts: forward and backward wrap from first tab");
    tabs.active = 3;
    Check(TabShortcutTarget(tabs, false) == 0 && TabShortcutTarget(tabs, true) == 2,
          L"tab shortcuts: forward wraps from last tab");
    tabs.tab_groups.push_back({ 1, L"Hidden", 0, true });
    tabs.items[1]->tab_group = 1;
    tabs.active = 0;
    Check(TabShortcutTarget(tabs, false) == 2,
          L"tab shortcuts: follows visible order past collapsed group");
    tabs.active = 1;
    Check(TabShortcutTarget(tabs, true) == 0 && TabShortcutTarget(tabs, false) == 2,
          L"tab shortcuts: hidden active tab navigates to visible neighbors");
    Check(IsTabShortcut(VK_TAB, true, false, false) &&
          IsTabShortcut(VK_TAB, true, true, false) &&
          !IsTabShortcut(VK_TAB, false, false, false) &&
          !IsTabShortcut(VK_TAB, true, false, true) &&
          !IsTabShortcut(L'1', true, false, false),
          L"tab shortcuts: only Ctrl+Tab and Ctrl+Shift+Tab are reserved");

    BYTE saved_keys[256]{};
    const bool have_keys = GetKeyboardState(saved_keys) != FALSE;
    BYTE keys[256]{};
    keys[VK_CONTROL] = 0x80;
    Check(have_keys && SetKeyboardState(keys), L"tab shortcuts: set edit test modifiers");
    if (have_keys) {
        auto state = std::make_unique<AppState>();
        LRESULT result = -1;
        Check(HandleHostedEditMessage(*state, nullptr, WM_CHAR, VK_TAB, 0, result) && result == 0,
              L"tab shortcuts: hosted edits consume Ctrl+Tab character");
        keys[VK_SHIFT] = 0x80;
        SetKeyboardState(keys);
        result = -1;
        Check(HandleHostedEditMessage(*state, nullptr, WM_CHAR, VK_TAB, 0, result) && result == 0,
              L"tab shortcuts: hosted edits consume Ctrl+Shift+Tab character");
        SetKeyboardState(saved_keys);
    }
}

void TestStagingTrayDeletion() {
    {
        StagingTray tray;
        tray.Collect({ L"C:\\tray-test\\a.txt", L"C:\\tray-test\\folder\\b.txt",
                       L"C:\\tray-test\\folder-other\\keep.txt" }, false);
        tray.Collect({ L"C:\\tray-test\\a.txt" }, true);
        tray.RemoveDeleted({});
        Check(tray.batches().size() == 2, L"tray: no successful deletions keep batches");
        tray.RemoveDeleted({ L"c:\\TRAY-test\\A.txt" });
        Check(tray.batches().size() == 1 && tray.batches()[0].items.size() == 2,
              L"tray: deletion removes duplicate copy and cut entries and empty batches");
        tray.RemoveDeleted({ L"C:\\tray-test\\folder" });
        Check(tray.batches().size() == 1 && tray.batches()[0].items.size() == 1 &&
              tray.batches()[0].items[0].path.find(L"folder-other") != std::wstring::npos,
              L"tray: folder deletion removes descendants but preserves sibling prefixes");
        tray.RemoveDeleted({ L"C:\\tray-test" });
        Check(tray.batches().empty(), L"tray: deleting final ancestor clears tray");
        const auto source = WorkspacePath(L"src\\app\\app_model.cpp");
        tray.Collect({ source, source }, false);
        const auto size = tray.batches()[0].items[0].size;
        tray.RemoveItem(0, 0);
        Check(size > 0 && tray.batches()[0].total_size == size,
              L"tray: removing an item updates batch size");
        std::wstring saved;
        tray.ToJson(saved);
        StagingTray restored;
        Check(restored.FromJson(saved) && restored.batches()[0].total_size == size,
              L"tray: restored batch retains size accounting");
        restored.RemoveDeleted({ source });
        Check(restored.batches().empty(), L"tray: deletion also clears restored entries");
    }
}

void TestStagingTraySession() {
    // Each batch nests its own "items" array and paths may contain brackets;
    // a reload must bring back every batch, not stop at the first ']'.
    StagingTray tray;
    tray.Collect({ L"C:\\tray-test\\a]b.txt", L"C:\\tray-test\\c.txt" }, false);
    tray.Collect({ L"C:\\tray-test\\[x]\\d.txt" }, true);
    std::wstring tray_json;
    tray.ToJson(tray_json);
    SessionSnapshot saved;
    saved.tray = tray;
    saved.details_panel = true;
    const std::wstring json = SessionToJson(saved);
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    const auto data_dir = WorkspacePath((L"bench_data/tray-session-" + std::to_wstring(GetCurrentProcessId())).c_str());
    std::filesystem::create_directories(data_dir);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_dir.c_str());
    SessionSnapshot snap;
    Check(SaveSession(saved) && LoadSession(snap), L"session: complete file round trip loads");
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
    std::filesystem::remove_all(data_dir);
    const auto& batches = snap.tray.batches();
    Check(batches.size() == 2 && batches[0].items.size() == 2 && !batches[0].move_intent &&
          batches[0].items[0].path.find(L"a]b.txt") != std::wstring::npos &&
          batches[1].items.size() == 1 && batches[1].move_intent &&
          batches[1].items[0].path.find(L"[x]\\d.txt") != std::wstring::npos,
          L"session: every tray batch survives reload, including bracketed paths");
    Check(snap.details_panel, L"session: fields after the tray still load");
    Check(!ParseSessionJson(json.substr(0, json.rfind(L'}')), snap) && snap.tray.batches().size() == 2,
          L"session: truncated document retains previous tray");
    Check(!snap.tray.FromJson(tray_json + L"garbage") && snap.tray.batches().size() == 2,
          L"tray: trailing garbage cannot replace previous batches");
    Check(!snap.tray.FromJson(L"[{\"move\":false,\"items\":[\"x\"]},]") && snap.tray.batches().size() == 2,
          L"tray: balanced invalid syntax cannot replace previous batches");

    // A rename or move reported by the completed job also retargets staged
    // children of the folder, never siblings that only share a name prefix.
    StagingTray moved;
    moved.Collect({ L"C:\\tray-test\\dir", L"C:\\tray-test\\dir\\sub\\a.txt",
                    L"C:\\tray-test\\dir-other\\b.txt" }, true);
    moved.ReplacePath(L"C:\\tray-test\\dir", L"C:\\tray-test\\renamed");
    const auto& items = moved.batches()[0].items;
    Check(items.size() == 3 &&
          items[0].path == fs::NormalizePath(L"C:\\tray-test\\renamed") &&
          items[1].path == fs::NormalizePath(L"C:\\tray-test\\renamed\\sub\\a.txt") &&
          items[2].path.find(L"dir-other") != std::wstring::npos,
          L"tray: a completed folder rename remaps staged children only");
    moved.ReplacePath(L"", L"C:\\x");
    Check(moved.batches()[0].items[0].path == fs::NormalizePath(L"C:\\tray-test\\renamed"),
          L"tray: an empty source path changes nothing");
}

void TestAuditTabs() {
    WindowTabs tabs;
    auto& first = tabs.NewTab(L"C:\\visible");
    auto hidden = std::make_unique<Pane>();
    hidden->NewTab(L"C:\\hidden");
    auto* hidden_ptr = hidden.get();
    first.panes.push_back(std::move(hidden));
    const auto visible = first.VisiblePanes();
    Check(visible.size() == 1 && visible[0] == first.panes[0].get() &&
          std::find(visible.begin(), visible.end(), hidden_ptr) == visible.end(),
          L"AUD-020: cached hidden pane is absent from folder-reuse candidates");
    first.root = MakePresetTree(LayoutPreset::TwoVertical, {first.panes[0].get(), hidden_ptr});
    Check(first.VisiblePanes().size() == 2, L"AUD-020: expanding split includes both visible panes");

    WindowTabs duplicate;
    duplicate.NewTabAtLocation(0, L"");
    bool loaded_empty = false;
    bool bound_before_load = false;
    TabController::Callbacks callbacks;
    callbacks.default_new_tab = [](std::wstring& path) { path = L"C:\\default"; return true; };
    callbacks.layout_changed = [&] { bound_before_load = duplicate.Active()->ActiveFolder()->current_path.empty(); };
    callbacks.load_tab = [&](Tab& tab) { loaded_empty = bound_before_load && tab.current_path.empty(); };
    TabController controller(callbacks);
    TabControllerTestAccess::Duplicate(controller, duplicate, 0);
    Check(duplicate.items.size() == 2 && loaded_empty && duplicate.Active()->ActiveFolder()->current_path.empty(),
          L"AUD-021: duplicate This PC binds and loads explicit empty location despite default folder setting");

    WindowTabs groups;
    auto* pinned = &groups.NewTab(L"C:\\pinned");
    pinned->pinned = true;
    groups.NewTab(L"C:\\one");
    groups.NewTab(L"C:\\free");
    auto* active = &groups.NewTab(L"C:\\two");
    TabGroup group; group.id = 42; groups.tab_groups.push_back(group);
    Check(!SetTabGroup(groups, 0, 42) && pinned->tab_group == 0 && groups.items[0].get() == pinned,
          L"AUD-022: pinned tab rejects group membership without moving prefix");
    Check(SetTabGroup(groups, 1, 42) && SetTabGroup(groups, 3, 42) &&
          groups.items[0].get() == pinned && groups.items[0]->pinned &&
          groups.items[1]->tab_group == 42 && groups.items[2]->tab_group == 42 && groups.Active() == active,
          L"AUD-022: normal group run stays after pinned prefix and preserves active identity");
    Check(!SetTabGroup(groups, 99, 42) && !SetTabGroup(groups, 1, 99),
          L"AUD-022: invalid tab and group leave membership intact");
}

void TestLayoutOwnedTabs() {
    WindowTabs tabs;
    tabs.NewTab(L"C:\\work");
    LayoutTab& first = *tabs.Active();
    auto extra = std::make_unique<Pane>();
    extra->NewTab(L"C:\\assets");
    first.panes.push_back(std::move(extra));
    std::vector<Pane*> used{ first.panes[0].get(), first.panes[1].get() };
    first.root = MakePresetTree(LayoutPreset::TwoVertical, used);
    first.layout = LayoutPreset::TwoVertical;
    first.focused_index = 0;
    Check(LayoutTabTitle(first).find(L"3") == std::wstring::npos &&
          LayoutTabTitle(first).find(L"·") != std::wstring::npos,
          L"layouttabs: multi-pane title uses a light suffix");

    tabs.NewTab(L"D:\\raw");
    Check(tabs.items.size() == 2, L"layouttabs: two window tabs");
    Check(tabs.Active()->layout == LayoutPreset::Single &&
          tabs.Active()->panes.size() == 1,
          L"layouttabs: new tab is a single-pane layout");
    Check(tabs.Active()->panes[0]->view.current_path.find(L"raw") != std::wstring::npos,
          L"layouttabs: new tab opens the requested folder");

    tabs.SwitchTab(0);
    Check(tabs.Active()->layout == LayoutPreset::TwoVertical &&
          tabs.Active()->panes.size() == 2,
          L"layouttabs: switch restores pane count and preset");
    Check(tabs.Active()->panes[0]->view.current_path.find(L"work") != std::wstring::npos &&
          tabs.Active()->panes[1]->view.current_path.find(L"assets") != std::wstring::npos,
          L"layouttabs: switch restores each column path");

    const LayoutTabSnapshot first_snap = CaptureLayoutTab(*tabs.items[0]);
    const LayoutTabSnapshot second_snap = CaptureLayoutTab(*tabs.items[1]);
    WindowTabs restored;
    bool loading_panes_registered = true;
    RestoreWindowTabs(restored, { first_snap, second_snap }, {}, 1,
        [&](Tab& tab, const std::wstring& path) {
            bool registered = false;
            for (const auto& layout : restored.items)
                for (const auto& pane : layout->panes)
                    registered |= pane->ActiveTab() == &tab;
            loading_panes_registered &= registered;
            tab.current_path = path;
        });
    Check(loading_panes_registered,
          L"layouttabs: restoring panes are registered before directory loading");
    Check(restored.items.size() == 2 && restored.active == 1,
          L"layouttabs: session restore keeps two tabs and active index");
    Check(restored.items[0]->layout == LayoutPreset::TwoVertical &&
          restored.items[0]->panes.size() == 2 &&
          restored.items[0]->panes[0]->view.current_path.find(L"work") != std::wstring::npos &&
          restored.items[0]->panes[1]->view.current_path.find(L"assets") != std::wstring::npos,
          L"layouttabs: session restore keeps the first tab's split folders");
    Check(restored.items[1]->layout == LayoutPreset::Single &&
          restored.items[1]->panes.size() == 1 &&
          restored.items[1]->panes[0]->view.current_path.find(L"raw") != std::wstring::npos,
          L"layouttabs: session restore keeps the second tab's single folder");
}

void TestUtf8PersistFile() {
    wchar_t temp_dir[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
    const std::wstring path = std::wstring(temp_dir) + L"pulse-utf8-persist-test.json";
    const std::wstring json =
        L"{\"name\":\"紧急修补\",\"unc\":\"\\\\192.0.2.10\\示例共享盘\"}\n";
    Check(WriteUtf8FileAtomic(path, json), L"utf8file: write Chinese JSON");
    std::wstring loaded;
    Check(ReadUtf8File(path, loaded) && loaded == json, L"utf8file: Chinese round-trip");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool utf8 = false;
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 4096) {
            std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
                const std::string raw(bytes.data(), bytes.data() + read);
                utf8 = raw.find("\xE7\xB4\xA7\xE6\x80\xA5\xE4\xBF\xAE\xE8\xA1\xA5") !=
                       std::string::npos;
            }
        }
        CloseHandle(file);
    }
    Check(utf8, L"utf8file: on-disk bytes are UTF-8");
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".tmp").c_str());
}

void TestColorPickerModel() {
    // Known value from the QFluent reference screenshot (#2FDFF1).
    {
        const ui::HsvColor hsv = ui::HsvFromRgb(0x2FDFF1);
        Check(hsv.h == 186 && hsv.s == 205 && hsv.v == 241,
              L"colorpicker: HsvFromRgb(#2FDFF1) -> 186/205/241");
    }
    Check(ui::RgbFromHsv(0, 0, 128) == 0x808080, L"colorpicker: gray roundtrip exact");
    Check(ui::RgbFromHsv(360, 255, 255) == 0xFF0000, L"colorpicker: hue 360 wraps to red");
    Check(ui::RgbFromHsv(0, 255, 255) == 0xFF0000, L"colorpicker: pure red exact");
    // Roundtrip within +-2 per channel (hue quantization loses a little).
    bool roundtrip = true;
    for (const uint32_t rgb : { 0x000000u, 0xFFFFFFu, 0x2FDFF1u, 0xEF4444u,
                                0x123456u, 0xA855F7u, 0x94A3B8u, 0x0078D4u }) {
        const ui::HsvColor hsv = ui::HsvFromRgb(rgb);
        const uint32_t back = ui::RgbFromHsv(hsv.h, hsv.s, hsv.v);
        for (int shift = 0; shift < 24; shift += 8) {
            const int a = static_cast<int>((rgb >> shift) & 0xff);
            const int b = static_cast<int>((back >> shift) & 0xff);
            if (std::abs(a - b) > 2) roundtrip = false;
        }
    }
    Check(roundtrip, L"colorpicker: RGB<->HSV roundtrip within tolerance");

    uint32_t argb = 0;
    Check(ui::ParseHexColor(L"#ff2fdff1", argb) && argb == 0xFF2FDFF1u,
          L"colorpicker: parse #aarrggbb");
    Check(ui::ParseHexColor(L"2FDFF1", argb) && argb == 0xFF2FDFF1u,
          L"colorpicker: parse rrggbb implies opaque alpha");
    Check(!ui::ParseHexColor(L"#12345", argb), L"colorpicker: reject 5 digits");
    Check(!ui::ParseHexColor(L"#gggggg", argb), L"colorpicker: reject non-hex");
    Check(!ui::ParseHexColor(L"", argb), L"colorpicker: reject empty");
    Check(ui::FormatHexColor(0xFF2FDFF1u, true) == L"#ff2fdff1",
          L"colorpicker: format #aarrggbb lowercase");
    Check(ui::FormatHexColor(0xFF2FDFF1u, false) == L"#2fdff1",
          L"colorpicker: format #rrggbb lowercase");
}


void TestDetailsPreviewInteraction() {
    ui::PreviewViewport view;
    const auto rect = D2D1::RectF(20, 30, 420, 330);
    view.SetContent(rect, 1600, 1200, true);
    Check(std::abs(view.zoom - .25f) < .001f && view.MaxX() == 0 && view.MaxY() == 0,
        L"preview: initial image fits entirely without cropping");
    view.ZoomAt(1, 120, 130);
    auto drawn = view.ContentRect();
    Check(std::abs((120 - drawn.left) / view.zoom - 400) < .01f &&
          std::abs((130 - drawn.top) / view.zoom - 400) < .01f,
        L"preview: wheel zoom preserves the source point under the pointer");
    const float old_x = view.x, old_y = view.y;
    view.Pan(-77, -43);
    Check(view.x == old_x + 77 && view.y == old_y + 43,
        L"preview: diagonal grab follows pointer displacement on both axes");
    view.Pan(-100000, -100000);
    drawn = view.ContentRect();
    Check(std::abs(drawn.right - rect.right) < .01f && std::abs(drawn.bottom - rect.bottom) < .01f,
        L"preview: grab reaches bottom-right content without overscroll");
    view.Pan(100000, 100000);
    drawn = view.ContentRect();
    Check(drawn.left == rect.left && drawn.top == rect.top,
        L"preview: grab reaches top-left content without overscroll");
    view.ToggleFit(200, 180);
    view.SetContent(D2D1::RectF(20, 30, 820, 630), 1600, 1200, true);
    Check(view.fit && std::abs(view.zoom - .5f) < .001f,
        L"preview: fit mode readjusts when the preview panel expands");
    view.SetContent(rect, 160, 1200, true);
    view.ZoomAt(1, 220, 180);
    view.Pan(-300, -300);
    Check(view.x == 0 && view.y > 0, L"preview: a narrow image pans only in the overflowing direction");
    view = {};
    view.SetContent(rect, 2200, 16000, false);
    view.Pan(-100000, -100000);
    Check(view.x == 1800 && view.y == 15700,
        L"preview: long wide text reaches real content beyond the former 4000px limit");
    view.SetContent(rect, 50, 60, false);
    Check(view.x == 0 && view.y == 0, L"preview: short content clamps stale pan on resize");
    view = {};
    view.SetContent(rect, 800, 600, true);
    Check(view.fit && view.x == 0 && view.y == 0,
        L"preview: reset for another file restores fit and clears both offsets");

    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float height : {380.0f, 720.0f}) {
            for (float width : {300.0f, 480.0f}) {
                ui::MainRenderer renderer;
                renderer.SetScale(scale);
                renderer.SetDetailsPanelVisible(true);
                renderer.SetDetailsPanelWidth(width);
                const auto window = D2D1::RectF(0, 0, 1400 * scale, height * scale);
                const auto panel = renderer.DetailsPanelRect(window.right, window.bottom);
                ui::WindowViewModel vm;
                vm.details_visible = true;
                vm.details.has_selection = true;
                vm.details.multi_count = 1;
                vm.details.preview_only = true;
                vm.details.preview_expansion = 1;
                const auto footer_bounds = D2D1::RectF(panel.left + 12 * scale,
                    panel.bottom - 28 * scale, panel.right - 12 * scale, panel.bottom);
                const auto footer = ui::MakePreviewFooterLayout(footer_bounds, scale);
                Check(footer.caption.right < footer.chevron.x - 4 * scale &&
                      std::abs(footer.chevron.x - (footer_bounds.left + footer_bounds.right) * .5f) < .01f,
                    L"preview footer: filename leaves a gap around the centered chevron");
                const auto image_footer = ui::MakePreviewFooterLayout(footer_bounds, scale);
                Check(image_footer.caption.right < image_footer.zoom.left &&
                      image_footer.zoom.left > image_footer.chevron.x + 4 * scale,
                    L"preview footer: zoom feedback cannot overlap filename or chevron");
                const float cx = (panel.left + panel.right) * .5f;
                Check(renderer.HitTest(vm, window, cx, panel.bottom - 14 * scale).region ==
                    ui::HitTestResult::DetailsPreviewToggle,
                    L"preview layout: full preview leaves a reachable bottom fold control at every DPI/size");
                Check(renderer.HitTest(vm, window, cx, panel.bottom - 32 * scale).region ==
                    ui::HitTestResult::DetailsPreview &&
                    std::abs(renderer.DetailsContentHeightDip(vm, window.right, window.bottom) -
                             (panel.bottom - panel.top) / scale) < .01f,
                    L"preview layout: full preview fills available height with no hidden metadata scroll");
                vm.details.preview_only = false;
                vm.details.preview_expansion = 0;
                vm.details.scroll_y = 600;
                int fold_hits = 0;
                for (float y = panel.top; y < panel.bottom; y += scale) {
                    const auto hit = renderer.HitTest(vm, window, cx, y);
                    if (hit.region == ui::HitTestResult::DetailsPreviewToggle) ++fold_hits;
                }
                Check(fold_hits >= 27 && fold_hits <= 29,
                    L"preview layout: fold control stays pinned and clickable while metadata scrolls");
                vm.details.has_selection = false;
                Check(renderer.HitTest(vm, window, cx, panel.bottom - 14 * scale).region !=
                    ui::HitTestResult::DetailsPreviewToggle,
                    L"preview layout: empty selection does not expose an inactive fold control");
            }
        }
    }

    // Pane widths follow the window the way File Explorer does: the file list keeps
    // its minimum width and either panel can grow into whatever is left.
    {
        ui::MainRenderer renderer;
        renderer.SetScale(1.0f);
        renderer.SetDetailsPanelVisible(true);
        renderer.SetDetailsPanelWidth(480.0f);
        renderer.SetSidebarWidthDip(224.0f);
        Check(std::abs(renderer.DetailsMaxWidthDip(1600.0f) - 1256.0f) < 0.5f,
            L"panels: the details panel grows until the list reaches its minimum");
        Check(std::abs(renderer.SidebarMaxWidthDip(1600.0f) - 1000.0f) < 0.5f,
            L"panels: the open details panel is reserved by the sidebar limit");
        renderer.SetDetailsPanelVisible(false);
        Check(std::abs(renderer.SidebarMaxWidthDip(1600.0f) - 1240.0f) < 0.5f,
            L"panels: hiding details still reserves space for the shared toolbar");
        renderer.SetDetailsPanelVisible(true);
        renderer.SetSidebarWidthDip(700.0f);
        const float list_left = renderer.EffectiveSidebarWidth(1000.0f) + renderer.Margin();
        const float list_right = 1000.0f - renderer.Margin() - renderer.DetailsPanelWidth(1000.0f);
        Check(list_right - list_left >= ui::kListMinWidthDip - 0.5f,
            L"panels: a narrow window pushes the panes back instead of starving the list");
        Check(std::abs(renderer.EffectiveSidebarWidth(880.0f) - ui::kSidebarRailWidthDip) < 0.5f,
            L"panels: a narrow window still folds the sidebar into its rail");
        // Same window in DIPs at 150%: the sidebar preference must not be mixed
        // into the DIP math as pixels.
        ui::MainRenderer scaled;
        scaled.SetScale(1.5f);
        scaled.SetDetailsPanelVisible(true);
        scaled.SetDetailsPanelWidth(480.0f);
        scaled.SetSidebarWidthDip(224.0f);
        Check(std::abs(scaled.DetailsMaxWidthDip(2400.0f) - 1256.0f) < 0.5f &&
              std::abs(scaled.SidebarMaxWidthDip(2400.0f) - 1000.0f) < 0.5f,
            L"panels: window limits stay in DIPs when the window is scaled");
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            scaled.SetScale(scale);
            scaled.SetSidebarWidthDip(224.0f);
            Check(std::abs(scaled.EffectiveSidebarWidth(1600.0f * scale) - 224.0f * scale) < 0.5f,
                L"panels: preferred sidebar width is scaled exactly once");
            scaled.SetSidebarWidthDip(1800.0f);
            Check(std::abs(scaled.EffectiveSidebarWidth(1600.0f * scale) - 1000.0f * scale) < 0.5f,
                L"panels: capped sidebar width is converted from DIPs to pixels");
            Check(std::abs(scaled.EffectiveSidebarWidth(880.0f * scale) - ui::kSidebarRailWidthDip * scale) < 0.5f,
                L"panels: collapsed sidebar rail scales exactly once");
        }
    }

    // The preview on/off chip lives on the name row (left of the star), so it is
    // still reachable once the well collapses; switching it off must remove the
    // well's hit area entirely.
    {
        ui::MainRenderer renderer;
        renderer.SetScale(1.0f);
        renderer.SetDetailsPanelVisible(true);
        renderer.SetDetailsPanelWidth(360.0f);
        const auto window = D2D1::RectF(0, 0, 1400.0f, 720.0f);
        const auto panel = renderer.DetailsPanelRect(window.right, window.bottom);
        const float panel_cx = (panel.left + panel.right) * 0.5f;
        ui::WindowViewModel vm;
        vm.details_visible = true;
        vm.details.has_selection = true;
        vm.details.multi_count = 1;
        auto chip_reachable = [&] {
            for (float yy = panel.top; yy < panel.bottom; yy += 2.0f) {
                for (float xx = panel.right - 120.0f; xx < panel.right; xx += 2.0f) {
                    if (renderer.HitTest(vm, window, xx, yy).region ==
                        ui::HitTestResult::DetailsPreviewEnable) return true;
                }
            }
            return false;
        };
        const bool well_open = renderer.HitTest(vm, window, panel_cx, panel.top + 20.0f).region ==
            ui::HitTestResult::DetailsPreview;
        const bool chip_on = chip_reachable();
        vm.details.preview_enabled = false;
        const bool well_gone = renderer.HitTest(vm, window, panel_cx, panel.top + 20.0f).region !=
            ui::HitTestResult::DetailsPreview;
        const bool chip_off_still_there = chip_reachable();
        Check(well_open && chip_on && well_gone && chip_off_still_there,
            L"preview layout: the preview switch collapses the well but stays clickable on the name row");
    }

    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    const auto data_dir = std::wstring(kSandbox) + L"\\preview-session";
    std::filesystem::create_directories(data_dir);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_dir.c_str());
    SessionSnapshot saved, loaded;
    saved.details_panel = true;
    saved.details_preview_only = true;
    saved.details_preview = false;
    saved.details_panel_width = 420;
    saved.sidebar_collapsed = 0x2;
    saved.sidebar_hidden = 0x4;
    saved.sidebar_order = { 3, 6, 0, 7, 1, 2, 4, 5 };
    saved.quick_access_hidden = (1 << 1) | (1 << 4);
    Check(SaveSession(saved) && LoadSession(loaded) && loaded.details_preview_only &&
        !loaded.details_preview && loaded.details_panel_width == 420,
        L"preview: folded mode, preview switch and panel width survive session reload");
    Check(loaded.sidebar_collapsed == 0x2 && loaded.sidebar_hidden == 0x4,
        L"session: sidebar collapse and hide masks survive reload");
    Check(loaded.sidebar_order == saved.sidebar_order &&
        loaded.quick_access_hidden == saved.quick_access_hidden,
        L"session: section order and hidden quick-access links survive reload");
    WriteUtf8FileAtomic(data_dir + L"\\session.json", L"{\"detailsPanel\":1,\"version\":4}");
    SessionSnapshot legacy;
    LoadSession(legacy);
    Check(!legacy.details_preview_only, L"preview: older sessions default to expanded details");
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
}

int RunSelfTest1B2() {
    // These model assertions use the Chinese resource strings explicitly.
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONERR$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::error_code directory_error;
    std::filesystem::create_directories(WorkspacePath(L"bench_data"), directory_error);
    if (directory_error) return 1;
    FILE* g_log_local = nullptr;
    g_log = _wfopen_s(&g_log_local, kLogPath.c_str(), L"w, ccs=UTF-8") == 0
        ? g_log_local : nullptr;

    wchar_t tag_delete_case[64]{};
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", tag_delete_case, ARRAYSIZE(tag_delete_case)) &&
        wcscmp(tag_delete_case, L"tag-delete-view") == 0) {
        const bool passed = ::RunTagDeleteViewTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    wchar_t skip_visual[8]{};
    g_skip_visual = GetEnvironmentVariableW(L"PULSE_SELFTEST_NO_SCREENSHOTS", skip_visual, ARRAYSIZE(skip_visual)) > 0;
    if (g_skip_visual) LogLine(L"[SKIP] Screenshot capture disabled\n");
    wchar_t test_case[64]{};
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"scroll-hover-taskbar") == 0) {
        const bool passed = RunScrollHoverTaskbarTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"keymap") == 0) {
        TestKeymap();
        LogLine(L"\n== keymap: %d passed, %d failed ==\n", g_pass, g_fail);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"audit-tabs") == 0) {
        TestAuditTabs();
        LogLine(L"\n== audit-tabs: %d passed, %d failed ==\n", g_pass, g_fail);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tab-pin-boundary") == 0) {
        const bool passed = ::RunTabPinBoundaryTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tray-tags") == 0) {
        const bool passed = ::RunTrayTagsUiTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"preview-deadline") == 0) {
        const bool passed = ::RunPreviewDeadlineTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tag-color-command") == 0) {
        const bool passed = ::RunTagColorCommandTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"dialog-recovery") == 0) {
        const bool passed = ::RunDialogRecoveryTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tray-completion") == 0) {
        const int result = ::RunTrayCompletionTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"operation-intent-ui") == 0) {
        const bool passed = ::RunOperationIntentUiTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        (wcscmp(test_case, L"content-pattern-selection") == 0 || wcscmp(test_case, L"content-history-order") == 0)) {
        const bool passed = wcscmp(test_case, L"content-pattern-selection") == 0 ?
            ::RunContentPatternSelectionTest() : ::RunContentHistoryOrderTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"listing-selection-restore") == 0) {
        const bool passed = ::RunListingSelectionRestoreTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"workspace-restore") == 0) {
        const bool passed = ::RunWorkspaceRestoreTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"column-resize-ui") == 0) {
        const bool passed = RunColumnResizeUiTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"folder-sizes") == 0) {
        const bool passed = RunFolderSizesTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"pane-header-icons") == 0) {
        const bool passed = RunPaneHeaderIconTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"folder-views") == 0) {
        TestViewZoomChain();
        TestContinuousIconSize();
        TestViewZoomAnchor();
        TestInlineRename();
        TestWallRows();
        TestFolderViews();
        TestFolderSorts();
        TestStartupLocation();
        TestTrayReveal();
        TestPreviewCodecProbe();
        TestLockedItemPrompt();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tray-session") == 0) {
        TestStagingTrayDeletion();
        TestStagingTraySession();
        LogLine(L"\n== tray-session: %d passed, %d failed ==\n", g_pass, g_fail);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"pr-shell") == 0) {
        TestMenuModel();
        TestShellMenuMerge();
        TestQuickAccess();
        TestDetailsPreviewInteraction();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"pin-reorder") == 0) {
        TestQuickAccessPinReorder();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"quick-access") == 0) {
        TestQuickAccess();
        LogLine(L"\n== quick-access: %d passed, %d failed ==\n", g_pass, g_fail);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"integration-compat") == 0) {
        TestIntegrationCompat();
        LogLine(L"\n== integration-compat: %d passed, %d failed ==\n", g_pass, g_fail);
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"snapshot-patch") == 0) {
        TestSnapshotPatch();
        TestSnapshotPatchBatch();
        TestSnapshotPatchHoldsRows();
        TestEntryOrderHold();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"context-verbs") == 0) {
        DumpContextVerbs();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"release-panels-hidden") == 0) {
        TestDetailsPreviewInteraction();
        TestHiddenFiles();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"action-state") == 0) {
        TestActionState();
        TestActionPolicy();
        TestActionArgs();
        TestFilePlans();
        TestMcpRunActions();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tray-stack") == 0) {
        TestTrayStack();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"list-columns") == 0) {
        TestListColumns();
        TestOptionalColumns();
        TestViewLayouts();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"filter-controls") == 0) {
        TestFilterControls();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"filename-render") == 0) {
        const bool passed = RunFilenameRenderTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"filter-search-ui") == 0) {
        const bool passed = RunNameHighlightUiTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"folder-shortcut") == 0) {
        TestLinkResolve();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"operation-toast") == 0) {
        TestNotificationToast();
        const bool passed = RunOperationToastTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed && !g_fail ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"search-routing") == 0) {
        const bool passed = pulse::app::RunSearchRoutingTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"tab-transfer") == 0) {
        const bool passed = pulse::app::RunTabTransferTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"address-editor") == 0) {
        const bool passed = RunAddressEditorTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"rename-editor") == 0) {
        const bool passed = RunRenameEditorTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"rename-outside") == 0) {
        TestRenameOutsideClick();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"change-app") == 0) {
        const bool passed = RunChangeTrackingAppTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"change-ui") == 0) {
        const bool passed = RunChangeTrackingUiTest();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return passed ? 0 : 1;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"blank-pane") == 0) {
        TestBlankPaneClickNavigation();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_SELFTEST_CASE", test_case, ARRAYSIZE(test_case)) &&
        wcscmp(test_case, L"advanced-edit") == 0) {
        TestAdvancedEditClicks();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        return g_fail ? 1 : 0;
    }

    TestDetailsPreviewInteraction();
    TestBreadcrumb();
    TestThisPcEnumeration();
    TestLoadingPresentation();
    TestNavigationReturnSelection();
    TestMouseHistoryNavigation();
    TestBlankPaneClickNavigation();
    TestNotificationToast();
    TestMenuModel();
    TestShellMenuMerge();
    TestContextMenuPrefs();
    TestAppPrefsAndSettingsPath();
    TestDragDropPure();
    TestAddressSearch();
    TestAddressSearchHistoryInteraction();
    TestAddressSearchHistoryInteraction(1.5f, true);
    TestContinuousSearch();
    TestGlobalSearchHandoff();
    TestLiveAddressSearch();
    TestAdvancedSearchDialog();
    TestAdvancedEditClicks();
    TestCtrlDragSelection();
    TestDirWatch();
    TestViewZoomChain();
    TestContinuousIconSize();
    TestViewZoomAnchor();
    TestFolderViews();
    TestFolderSorts();
    TestStartupLocation();
    TestTrayReveal();
    TestPreviewCodecProbe();
    TestLockedItemPrompt();
    TestNavigateAlwaysEnumerates();
    TestSnapshotPatch();
    TestSnapshotPatchBatch();
    TestSnapshotPatchHoldsRows();
    TestEntryOrderHold();
    TestSidebarScrollbarFade();
    TestSnapshotStorePutKeepsWorkerGeneration();
    TestDataObject();
    TestClipboardText();
    TestUniqueName();
    TestMultiSelect();
    TestSplitLayout();
    TestActionState();
    TestActionPolicy();
    TestActionArgs();
    TestFilePlans();
    TestMcpRunActions();
    TestViewLayouts();
    TestListColumns();
    TestOptionalColumns();
    TestTrayStack();
    TestHiddenFiles();
    TestQuickAccess();
    TestIntegrationCompat();
    TestPlacesAndIndex();
    TestRecycleAndBatchRename();
    TestLinkResolve();
    TestDetailsMeta();
    TestMoveTabRun();
    TestNormalizeGroupRuns();
    TestChipBlockDragGeometry();
    TestFindGroupRun();
    TestSessionLayoutTabs();
    TestTabShortcuts();
    TestKeymap();
    TestStagingTrayDeletion();
    TestStagingTraySession();
    TestLayoutOwnedTabs();
    TestUtf8PersistFile();
    TestColorPickerModel();
    TestBloomAccentGeometry();
    TestBloomSpring();
    TestLayerAlphas();
    TestTreeGuides();
    TestTreeSticky();
    TestOpsThroughShell();

    // Cleanup: real-delete the whole sandbox via the ops layer is overkill;
    // it only contains files we made, so delete in place.
    {
        ops::OpsManager cleanup;
        cleanup.Start([] {});
        Sleep(600);
        ops::OpRequest r;
        r.type = ops::OpType::RealDelete;
        r.sources.push_back(std::wstring(kSandbox));
        uint64_t prev = cleanup.Status().completed_ops;
        cleanup.Submit(std::move(r));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (cleanup.Status().completed_ops == prev &&
               std::chrono::steady_clock::now() < deadline) Sleep(10);
        cleanup.Stop();
    }
    Check(!Exists(kSandbox), L"cleanup: sandbox removed");

    LogLine(L"\n== 1B-2 self test: %d passed, %d failed ==\n", g_pass, g_fail);
    if (g_log) fclose(g_log);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}

} // namespace pulse::app
