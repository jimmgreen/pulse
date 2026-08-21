// selftest_1b2.cpp — Stage 1B-2 console self-test.
//
// Headless coverage for the parts `--shot` cannot reach (interactive menus,
// OLE drag & drop): menu model construction + hit-test + verb dispatch,
// IDataObject CF_HDROP contents, drop-effect modifier semantics, breadcrumb
// splitting, and real ops-layer create/move through pulse_shell.exe.
// All file operations are confined to bench_data/opstest/selftest_1b2 and
// cleaned up afterwards.
#include "selftest_1b2.h"
#include "app_model.h"
#include "session.h"
#include "app_prefs.h"
#include "context_menu.h"
#include "context_menu_prefs.h"
#include "shell_verbs.h"
#include "places.h"
#include "link_resolve.h"
#include "details_meta.h"
#include "../ipc/ctx_menu_util.h"
#include "../index/index_engine.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_net_cache.h"
#include "../ui/fluent_menu.h"
#include "../ui/color_picker.h"
#include "../ui/drag_drop.h"
#include "../ui/ui_renderer.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../common/text_format.h"
#include "../common/utf8_file.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <dwrite_3.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {

namespace {

const wchar_t* kSandbox = L"C:\\Users\\SS\\Desktop\\pulse\\bench_data\\opstest\\selftest_1b2";

int g_pass = 0;
int g_fail = 0;
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
    auto segs = ui::SplitBreadcrumb(L"C:\\Users\\SS\\Desktop");
    Check(segs.size() == 5, L"breadcrumb: C:\\Users\\SS\\Desktop -> 5 segments");
    if (segs.size() == 5) {
        Check(segs[0].text == L"此电脑" && segs[0].path.empty(),
              L"breadcrumb: This PC root segment");
        Check(segs[1].text == L"C:" && segs[1].path == L"C:\\", L"breadcrumb: drive segment");
        Check(segs[2].path == L"C:\\Users" && segs[4].path == L"C:\\Users\\SS\\Desktop",
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
    auto uncLong = ui::SplitBreadcrumb(L"\\\\?\\UNC\\192.168.0.254\\share\\dir");
    Check(uncLong.size() == 3 && uncLong[0].path == L"\\\\192.168.0.254" &&
          uncLong[1].path == L"\\\\192.168.0.254\\share" &&
          uncLong[2].path == L"\\\\192.168.0.254\\share\\dir",
          L"breadcrumb: \\\\?\\UNC prefix restores leading \\\\");
    auto longp = ui::SplitBreadcrumb(L"\\\\?\\C:\\A\\B");
    Check(longp.size() == 4 && longp[3].path == L"C:\\A\\B",
          L"breadcrumb: long-path prefix stripped");

    Pane pane;
    pane.NewTab(L"\\\\192.168.0.254\\share\\folder");
    SidebarModel sidebar;
    const auto vm = BuildWindowViewModel(
        pane, sidebar, true, false, true, nullptr, 0);
    Check(vm.pane.path == L"\\\\192.168.0.254\\share\\folder",
          L"breadcrumb: pane display path keeps UNC prefix");
    auto fromVm = ui::SplitBreadcrumb(vm.pane.path);
    Check(fromVm.size() == 3 && fromVm[0].path == L"\\\\192.168.0.254" &&
          fromVm[1].path == L"\\\\192.168.0.254\\share" &&
          fromVm[2].path == L"\\\\192.168.0.254\\share\\folder",
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
    Check(!wvm.sidebar.empty() && wvm.sidebar[0].items.size() == 3,
          L"sidebar: workspace group holds both workspaces + frequent child");
    if (!wvm.sidebar.empty() && wvm.sidebar[0].items.size() == 3) {
        const auto& local = wvm.sidebar[0].items[0];
        const auto& nas = wvm.sidebar[0].items[1];
        const auto& sub = wvm.sidebar[0].items[2];
        Check(local.badge.empty() && local.icon_glyph == L"\xE8B7",
              L"sidebar: local workspace keeps default glyph, no badge");
        Check(nas.badge == L"当前 · 服务器" && nas.icon_glyph == L"\xE968" &&
              colorIs(nas.icon_color, 0x38BDF8),
              L"sidebar: active UNC workspace shows server badge + network glyph");
        Check(sub.badge.empty() && sub.icon_glyph == L"\xE968" &&
              colorIs(sub.icon_color, 0x38BDF8),
              L"sidebar: UNC frequent child gets network glyph, no badge");
    }
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
              L"\\\\192.168.0.254\\工程项目盘-2025\\00_软件",
              L"\\\\192.168.0.254\\工程项目盘-2025") == L"00_软件",
          L"navigation: UNC parent return selects the folder just left");
    Check(NavigationReturnChildName(
              L"\\\\192.168.0.254\\工程项目盘-2025\\00_软件\\工具",
              L"\\\\192.168.0.254\\工程项目盘-2025") == L"00_软件",
          L"navigation: deep UNC return selects the immediate child");
}

void TestMenuModel() {
    ui::ComPtr<IDWriteFactory3> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
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
          folder_items[2].text == L"在新标签打开" &&
          folder_items[3].command == CmdCopyPath,
          L"menu: 在新标签打开 sits before 复制路径");
    Check(!items.back().enabled, L"menu: undo disabled without a stack");

    ui::FluentMenuModel model;
    model.SetItems(items);
    model.Layout(dwrite.get(), 1.0f);    int seps = 0;
    for (const auto& it : items) if (it.separator_after) ++seps;
    int expect_h = (int)(4 * 2 + 36.0f * (int)items.size() + 5 * seps + 0.5f);
    Check(model.HeightPx() == expect_h, L"menu: layout height = rows*36 + separators");
    Check(model.WidthPx() >= 160 && model.WidthPx() <= 320, L"menu: width within clamp");

    auto long_undo = BuildItemMenu(true,
        L"撤销删除 万锦学校一期_结施_1#创新融合中心及运动场馆(运动场馆).dwg");
    ui::FluentMenuModel wide;
    wide.SetItems(std::move(long_undo));
    wide.Layout(dwrite.get(), 1.0f);
    Check(wide.WidthPx() <= 320, L"menu: long undo label does not stretch the flyout");

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
    auto nw = BuildNewMenu();
    Check(nw.size() == 2 && nw[0].command == CmdNewFolder && nw[1].command == CmdNewTextFile,
          L"menu: 新建▾ dropdown has 文件夹/文本文档");
}

// Explorer merge rules (优化.md §7): built-in verbs filtered, duplicate texts
// dropped, section appended at the bottom; software-owned submenus keep one
// level of hierarchy as a flyout (never flattened into the main list).
void TestShellMenuMerge() {
    Check(ipc::IsBuiltinContextVerb(L"Open", false), L"shellmenu: open filtered");
    Check(ipc::IsBuiltinContextVerb(L"copyaspath", false), L"shellmenu: copyaspath filtered");
    Check(ipc::IsBuiltinContextVerb(L"pintohome", false), L"shellmenu: pintohome filtered");
    Check(!ipc::IsBuiltinContextVerb(L"MergePdf", false), L"shellmenu: third-party verb kept");
    Check(!ipc::IsBuiltinContextVerb(L"refresh", false), L"shellmenu: refresh only filtered on background");
    Check(ipc::IsBuiltinContextVerb(L"refresh", true), L"shellmenu: background refresh filtered");
    Check(ipc::IsDroppedContextSubmenu(L"OpenAs"), L"shellmenu: 打开方式 submenu dropped (registry provides it)");
    Check(!ipc::IsDroppedContextSubmenu(L"sendto"), L"shellmenu: 发送到 submenu kept as a flyout");

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
    ShellMenuEntry empty_group;
    empty_group.text = L"空分组";
    auto no_fly = BuildItemMenu(false, L"");
    const size_t no_fly_base = no_fly.size();
    AppendShellSection(no_fly, { empty_group });
    Check(no_fly.size() == no_fly_base, L"shellmenu: 无子项的分组行被丢弃");

    // Cap: never more than 48 Explorer rows.
    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 60; ++i)
        many.push_back({ CmdShellComBase + 100 + i, L"动词 " + std::to_wstring(i), true });
    auto capped = BuildItemMenu(false, L"");
    const size_t cap_base = capped.size();
    AppendShellSection(capped, many);
    Check(capped.size() == cap_base + 48, L"shellmenu: section capped at 48 rows");

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
    Check(ipc::IsCompressVendorFlyout(L"Bandizip") &&
          ipc::IsCompressTopLevel(L"压缩为「photo.zip」"),
          L"prefs: compress flyout vs top-level zip row");

    ContextMenuPrefs prefs;
    prefs.persist = false;

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
          L"prefs: factory drops share / wallpaper / rotate");
    Check(has(L"Bandizip") && has(L"合并 PDF") && has(L"打印"),
          L"prefs: factory keeps vendor flyout, type verb, print");
    Check(!has(L"压缩为「photo.zip」"),
          L"prefs: compress top-level collapsed when flyout exists");
    Check(has(L"用 记事本 打开") && has(L"用 画图 打开") && !has(L"用 Word 打开") &&
          has(L"打开方式…"),
          L"prefs: static open-with MRU capped at 2 plus 打开方式");
    Check(filtered.size() >= 4 && filtered[0].text == L"Bandizip" &&
          filtered[1].text == L"合并 PDF",
          L"prefs: order is flyout then type verb");

    prefs.SetItemEnabled(ipc::CatalogKey(L"发送到", true), true);
    auto restored = ApplyExplorerPrefs(prefs, raw);
    bool sendto_back = false;
    for (const auto& e : restored)
        if (e.text == L"发送到") sendto_back = true;
    Check(sendto_back, L"prefs: per-item override re-enables 发送到");

    prefs.RecordSeen(ipc::CatalogKey(L"Bandizip", true), L"Bandizip", true,
                     CtxMenuCategory::Software, true);
    const std::wstring json = prefs.ToJson();
    ContextMenuPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) && loaded.item_enabled[ipc::CatalogKey(L"发送到", true)] &&
          loaded.seen.size() == 1 && loaded.seen[0].from_com &&
          loaded.explorer_cap == 12 && !loaded.share && loaded.print,
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
    Check(loaded.seen.size() == 1 && loaded.item_enabled.empty() && !loaded.share,
          L"prefs: restore defaults keeps seen, clears overrides");

    std::vector<ShellMenuEntry> many;
    for (int i = 0; i < 20; ++i)
        many.push_back({ CmdShellComBase + 200 + i, L"动词 " + std::to_wstring(i), true, {}, L"", true });
    auto capped = ApplyExplorerPrefs(prefs, many);
    Check(capped.size() == 12, L"prefs: Explorer section capped at 12");
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
          prefs.launch_on_startup && prefs.keep_running_on_close,
          L"appprefs: parse json");
    Check(prefs.ApplyLaunchOnStartup(false) && !prefs.launch_on_startup,
          L"appprefs: persist=false toggle does not write Run key");
    const std::wstring json = prefs.ToJson();
    AppPrefs loaded;
    loaded.persist = false;
    Check(loaded.FromJson(json) && !loaded.launch_on_startup && loaded.keep_running_on_close,
          L"appprefs: json round-trip");
    Check(json.find(L"\"launch_on_startup\":false") != std::wstring::npos &&
          json.find(L"\"keep_running_on_close\":true") != std::wstring::npos,
          L"appprefs: json contains both flags");
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
}

void TestDataObject() {
    std::vector<std::wstring> paths{ L"C:\\fake_a.txt", L"D:\\fake_b.txt" };
    auto* obj = ui::FileDataObject::Create(paths);

    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    Check(obj->QueryGetData(&fmt) == S_OK, L"dnd: IDataObject advertises CF_HDROP");
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
    CreateDirectoryW(L"C:\\Users\\SS\\Desktop\\pulse\\bench_data\\opstest", nullptr);
    CreateDirectoryW(kSandbox, nullptr);
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

    tab.ClearSelection();
    Check(tab.SelectedCount() == 0 && !tab.IsSelected(0) && tab.selected_index < 0,
          L"select: escape clears");

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
    auto pal = BuildCommandPalette({ L"C:\\Users" });
    Check(!pal.empty() && pal.back().command == CmdRecentBase, L"menu: command palette recent");
    Check(pal.back().badge_text == L"历史路径" && pal.back().shortcut.empty(),
          L"menu: recent rows use 历史路径 badge");
    auto palQ = BuildCommandPalette(L"四宫", { L"C:\\Users" }, {}, false);
    Check(!palQ.empty() && palQ[0].command == CmdLayoutFourGrid, L"menu: palette filters by query");
    auto palLong = BuildCommandPalette(L"", { L"\\\\?\\C:\\Users\\W" }, {}, false);
    bool recent_ok = false;
    for (const auto& it : palLong) {
        if (it.command != CmdRecentBase) continue;
        Check(it.shortcut.find(L"\\\\?\\") == std::wstring::npos,
              L"menu: palette recent must not keep \\\\?\\ prefix");
        Check(it.badge_text == L"历史路径", L"menu: long path recent uses badge");
        Check(it.text == L"W", L"menu: recent title is folder name");
        recent_ok = true;
    }
    Check(recent_ok, L"menu: palette shows Windows-style path title");

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
    auto palCmd = BuildCommandPalette(L">新建", { L"C:\\新建" }, {}, false);
    Check(!palCmd.empty() && palCmd[0].command == CmdNewFolder, L"omnibar: >新建 matches 新建文件夹");
    bool cmd_has_layout = false;
    for (const auto& it : palCmd)
        if (it.command == CmdLayoutFourGrid) cmd_has_layout = true;
    Check(!cmd_has_layout, L"omnibar: >新建 does not keep unrelated commands");
    Check(palCmd.back().command == CmdRecentBase && palCmd.back().badge_text == L"历史路径",
          L"omnibar: command mode still lists recents at the end");
    auto palSearch = BuildCommandPalette(L"?foo", { L"C:\\Users" }, {}, false);
    bool search_has_cmd = false;
    for (const auto& it : palSearch)
        if (it.command == CmdSettings || it.command == CmdNewFolder) search_has_cmd = true;
    Check(!search_has_cmd, L"omnibar: ? search hides commands");
    auto palDup = BuildCommandPalette(L"", { L"C:\\A", L"C:\\A", L"C:\\B" }, {}, false, 0, L"C:\\B");
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
    auto palSkipHit = BuildCommandPalette(L"", { L"C:\\Users" }, { hit }, false);
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

    ui::PaneViewModel pvm;
    pvm.filter_text = L"foo";
    pvm.filter_map = std::make_shared<ui::PaneViewModel::FilterMap>(
        ui::PaneViewModel::FilterMap{ 2, 5, 9 });
    Check(pvm.EntryCount() == 3 && pvm.SourceIndex(1) == 5 && pvm.ViewIndex(9) == 2,
          L"filter: view/source index mapping");
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

    const std::wstring ads_file = L"bench_data\\tag_ads_v2.tmp";
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

    Check(fs::IsUncPath(L"\\\\server\\share") && !fs::IsUncPath(L"C:\\Users"),
          L"net: UNC detection");
    {
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirEntry e;
        e.name = L"cached.txt";
        e.size = 42;
        e.is_dir = false;
        entries->push_back(e);
        const std::wstring unc = L"\\\\pulse-selftest\\share";
        Check(fs::SaveNetSnapshot(unc, entries), L"netcache: save UNC snapshot");
        uint64_t ts = 0;
        auto loaded = fs::LoadNetSnapshot(unc, &ts);
        Check(loaded && loaded->size() == 1 && (*loaded)[0].name == L"cached.txt" &&
              (*loaded)[0].size == 42, L"netcache: load UNC snapshot");
        Check(!fs::FormatCacheAge(ts).empty(), L"netcache: age string");
    }

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

void TestOpsThroughShell() {
    std::wstring dir = kSandbox;
    g_ops.Start([] {});
    Sleep(800); // let the ops worker bring up pulse_shell.exe

    Check(ops::TerminalCommandLine(L"C:\\A\\B") == L"-d \"C:\\A\\B\"",
          L"ops: wt.exe command line");
    Check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\"",
          L"ops: wt.exe command line keeps drive root");

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
        g_ops.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items) {
            std::lock_guard<std::mutex> lk(m);
            got_token = token;
            got_items = std::move(items);
            got = true;
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
            if (it.text.empty()) clean = false;
            if (ipc::IsBuiltinContextVerb(it.verb, false)) clean = false;
        }
        Check(clean, L"ctx: items are filtered (no built-in verbs, no empty rows)");
        LogLine(L"[info] ctx: %zu Explorer items for .txt\n", got_items.size());
        g_ops.CloseShellMenu(token);
        g_ops.SetShellMenuCallback(nullptr);
    }

    g_ops.Stop();
}

void TestViewLayouts() {
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
    Check(std::abs(hidpiIcons.Metrics().icon_size - 240.0f) < 0.01f &&
          std::abs(hidpiIcons.Metrics().cell_width - 280.0f) < 0.01f,
          L"view: extra-large Explorer tier remains capped in physical pixels");
    Check(hidpiIcons.NameRect(0).top >= hidpiIcons.IconRect(0).bottom,
          L"view: extra-large filename never overlaps icon bounds");
    ui::ViewLayout hidpiLarge(ui::ViewMode::LargeIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiLarge.Metrics().icon_size - 144.0f) < 0.01f &&
          hidpiLarge.Metrics().cell_width >= 180.0f &&
          hidpiLarge.NameRect(0).top >= hidpiLarge.IconRect(0).bottom,
          L"view: large icons scale with DPI and keep filename below image");
    ui::ViewLayout hidpiMedium(ui::ViewMode::MediumIcons,
        D2D1::RectF(0, 0, 900, 700), 20, 0, 0, 1.5f);
    Check(std::abs(hidpiMedium.Metrics().icon_size - 72.0f) < 0.01f &&
          hidpiMedium.NameRect(0).top >= hidpiMedium.IconRect(0).bottom,
          L"view: medium icons scale with DPI and keep filename below image");
    ui::ViewLayout list(ui::ViewMode::List, viewport, 1000, 0, 0, 1.0f);
    Check(list.MaxScrollX() > 0 && list.MaxScrollY() == 0,
          L"view: list is column-major with horizontal scrolling");
    auto menu = BuildViewMenu(ui::ViewMode::Details);
    Check(menu.size() == 9 && menu[5].radio && !menu[0].radio &&
          menu[8].command == CmdDetailsPanel,
          L"view: menu has eight view choices plus details panel");
    Check(menu[0].glyph_scale > menu[1].glyph_scale &&
          menu[1].glyph_scale > menu[2].glyph_scale,
          L"view: icon menu communicates extra-large, large, and medium scale");
}

} // namespace

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
    Check(ResolveLink(lnk_file, e1) &&
          e1.link_target == fs::NormalizePath(target_file) &&
          !e1.link_target_is_dir && e1.link_target_size == sizeof(body) - 1,
          L"link: file target resolves with size");
    fs::DirEntry e2;
    Check(ResolveLink(lnk_dir, e2) && e2.link_target_is_dir,
          L"link: folder target resolves");
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
}

void TestNormalizeGroupRuns() {
    auto make = [](int group) {
        auto t = std::make_unique<Tab>();
        t->tab_group = group;
        return t;
    };
    {   // Split run: 1,0,1,2 -> 1,1,0,2 and active tab follows its pointer.
        app::Pane pane;
        pane.tabs.push_back(make(1)); pane.tabs.push_back(make(0));
        pane.tabs.push_back(make(1)); pane.tabs.push_back(make(2));
        pane.active_tab = 2;
        const Tab* active = pane.tabs[2].get();
        NormalizeGroupRuns(pane);
        Check(pane.tabs.size() == 4 &&
              pane.tabs[0]->tab_group == 1 && pane.tabs[1]->tab_group == 1 &&
              pane.tabs[2]->tab_group == 0 && pane.tabs[3]->tab_group == 2,
              L"tabgroup: split run collapses into one run");
        Check(pane.ActiveTab() == active, L"tabgroup: active tab survives normalize");
    }
    {   // Interleaved groups: 1,2,1,2 -> 1,1,2,2 (first-appearance order kept).
        app::Pane pane;
        for (int g : { 1, 2, 1, 2 }) pane.tabs.push_back(make(g));
        NormalizeGroupRuns(pane);
        Check(pane.tabs[0]->tab_group == 1 && pane.tabs[1]->tab_group == 1 &&
              pane.tabs[2]->tab_group == 2 && pane.tabs[3]->tab_group == 2,
              L"tabgroup: interleaved groups normalize pairwise");
    }
    {   // Already contiguous: untouched.
        app::Pane pane;
        for (int g : { 0, 1, 1, 0 }) pane.tabs.push_back(make(g));
        NormalizeGroupRuns(pane);
        Check(pane.tabs[0]->tab_group == 0 && pane.tabs[2]->tab_group == 1 &&
              pane.tabs[3]->tab_group == 0, L"tabgroup: contiguous runs untouched");
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

bool SamePaneTabs(const std::vector<PaneSessionSnapshot>& a,
                  const std::vector<PaneSessionSnapshot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].active != b[i].active) return false;
        if (a[i].groups.size() != b[i].groups.size() ||
            a[i].tabs.size() != b[i].tabs.size()) return false;
        for (size_t g = 0; g < a[i].groups.size(); ++g) {
            if (a[i].groups[g].id != b[i].groups[g].id ||
                a[i].groups[g].name != b[i].groups[g].name ||
                a[i].groups[g].color_rgb != b[i].groups[g].color_rgb ||
                a[i].groups[g].collapsed != b[i].groups[g].collapsed) return false;
        }
        for (size_t t = 0; t < a[i].tabs.size(); ++t) {
            if (a[i].tabs[t].path != b[i].tabs[t].path ||
                a[i].tabs[t].pinned != b[i].tabs[t].pinned ||
                a[i].tabs[t].group != b[i].tabs[t].group ||
                a[i].tabs[t].view != b[i].tabs[t].view ||
                a[i].tabs[t].columns != b[i].tabs[t].columns) return false;
        }
    }
    return true;
}

void TestSessionPaneTabs() {
    {   // Round-trip: multiple panes, groups, special characters.
        std::vector<PaneSessionSnapshot> panes(2);
        panes[0].active = 1;
        panes[0].groups.push_back({ 1, L"工作", 0x00CC6639, false });
        panes[0].groups.push_back({ 2, L"引\"号\\组\n名", 0x003B82F6, true });
        panes[0].tabs.push_back({ L"C:\\", true, 0, ui::ViewMode::Details, { 0.42f, 0.61f, 0.82f } });
        panes[0].tabs.push_back({ L"D:\\代码\\路径 \"quoted\"\\dir", false, 1,
                                  ui::ViewMode::LargeIcons });
        panes[0].tabs.push_back({ L"\\\\?\\UNC\\server\\share\\dir", false, 2,
                                  ui::ViewMode::List });
        panes[1].active = 0;
        panes[1].tabs.push_back({ L"C:\\Users", false, 0, ui::ViewMode::Tiles });

        const std::wstring json = PaneTabsToJson(panes);
        std::vector<PaneSessionSnapshot> back;
        Check(ParsePaneTabs(json, back), L"sessiontabs: round-trip parses");
        Check(SamePaneTabs(panes, back), L"sessiontabs: round-trip preserves all fields");
    }
    {   // Empty array and empty panes.
        std::vector<PaneSessionSnapshot> out;
        Check(ParsePaneTabs(L"[]", out) && out.empty(),
              L"sessiontabs: empty array");
        std::vector<PaneSessionSnapshot> panes(1); // no groups, no tabs
        out.clear();
        Check(ParsePaneTabs(PaneTabsToJson(panes), out) &&
              SamePaneTabs(panes, out),
              L"sessiontabs: empty pane round-trips");
    }
    {   // Unknown view falls back to details; out-of-range active survives
        // parsing (the restore side clamps it).
        std::vector<PaneSessionSnapshot> out;
        Check(ParsePaneTabs(
                  L"[{\"active\":99,\"groups\":[],\"tabs\":["
                  L"{\"path\":\"C:\\\\x\",\"pinned\":false,\"group\":7,\"view\":\"bogus\"}]}]",
                  out) &&
              out.size() == 1 && out[0].active == 99 &&
              out[0].tabs.size() == 1 && out[0].tabs[0].path == L"C:\\x" &&
              out[0].tabs[0].group == 7 &&
              out[0].tabs[0].view == ui::ViewMode::Details,
              L"sessiontabs: unknown view + active parsed as-is");
    }
    {   // Corrupt input: rejects or skips without crashing, never throws.
        std::vector<PaneSessionSnapshot> out;
        Check(!ParsePaneTabs(L"", out), L"sessiontabs: empty string rejected");
        Check(!ParsePaneTabs(L"not json", out), L"sessiontabs: garbage rejected");
        Check(!ParsePaneTabs(L"[{\"active\":1,\"groups\":[{\"id\":1}]", out),
              L"sessiontabs: truncated array rejected");
        Check(!ParsePaneTabs(L"[{\"tabs\":[{\"path\":\"C:\\\\\"}]", out),
              L"sessiontabs: unbalanced escape rejected");
        Check(ParsePaneTabs(L"[{\"tabs\":[{}]}]", out) && out.size() == 1 &&
              out[0].tabs.size() == 1 && out[0].tabs[0].path.empty(),
              L"sessiontabs: empty object tolerated");
    }
}

void TestUtf8PersistFile() {
    wchar_t temp_dir[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp_dir), temp_dir);
    const std::wstring path = std::wstring(temp_dir) + L"pulse-utf8-persist-test.json";
    const std::wstring json =
        L"{\"name\":\"紧急修补\",\"unc\":\"\\\\192.168.0.254\\工程项目盘\"}\n";
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

int RunSelfTest1B2() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONERR$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    FILE* g_log_local = nullptr;
    g_log = _wfopen_s(&g_log_local, L"C:\\Users\\SS\\Desktop\\pulse\\bench_data\\selftest_1b2_last.log", L"w, ccs=UTF-8") == 0
        ? g_log_local : nullptr;

    TestBreadcrumb();
    TestThisPcEnumeration();
    TestLoadingPresentation();
    TestNavigationReturnSelection();
    TestMenuModel();
    TestShellMenuMerge();
    TestContextMenuPrefs();
    TestAppPrefsAndSettingsPath();
    TestDragDropPure();
    TestDataObject();
    TestClipboardText();
    TestUniqueName();
    TestMultiSelect();
    TestSplitLayout();
    TestViewLayouts();
    TestPlacesAndIndex();
    TestLinkResolve();
    TestDetailsMeta();
    TestMoveTabRun();
    TestNormalizeGroupRuns();
    TestChipBlockDragGeometry();
    TestFindGroupRun();
    TestSessionPaneTabs();
    TestUtf8PersistFile();
    TestColorPickerModel();
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
