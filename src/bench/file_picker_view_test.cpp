// The open dialog's own chrome: the title strip and the footer. The address
// row, sidebar and list are the main window's renderer and are tested there;
// the live window runs with pulse.exe --picker-live file|folder|image.
//
//   pulse_file_picker_view_test.exe [out_dir]
//       Checks layout and hit tests at 100%, 150% and 200% for every mode and
//       writes footer PNGs in light, dark and high contrast to out_dir
//       (default build\file-picker-visual).

#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/folder_picker_view.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

using namespace pulse::ui;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, float scale, int mode) {
    if (!ok) ++g_failures;
    if (!ok || scale == 1.0f)
        std::printf("[%s] %s (scale %.1f, mode %d)\n", ok ? "PASS" : "FAIL", what, scale, mode);
}

bool Inside(const D2D1_RECT_F& inner, const D2D1_RECT_F& outer) {
    return inner.left >= outer.left - 0.5f && inner.right <= outer.right + 0.5f &&
           inner.top >= outer.top - 0.5f && inner.bottom <= outer.bottom + 0.5f;
}

bool Overlaps(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left < b.right - 0.5f && b.left < a.right - 0.5f &&
           a.top < b.bottom - 0.5f && b.top < a.bottom - 0.5f;
}

float CenterX(const D2D1_RECT_F& r) { return (r.left + r.right) * 0.5f; }
float CenterY(const D2D1_RECT_F& r) { return (r.top + r.bottom) * 0.5f; }

FolderPickerChrome MakeChrome(PickerMode mode) {
    FolderPickerChrome c;
    c.mode = mode;
    c.title = mode == PickerMode::File ? L"打开" : mode == PickerMode::Image ? L"选择背景图片" : L"选择文件夹";
    c.filename_label = L"文件名(N):";
    c.filter_text = L"文本文档 (*.txt; *.md)";
    c.primary_text = mode == PickerMode::Folder ? L"选择文件夹" : L"打开";
    c.cancel_text = L"取消";
    c.summary = L"将选择：C:\\Users\\Example\\Documents\\季度报告\\2026 第三季度.docx";
    c.primary_enabled = true;
    c.hosted_edit = false;
    c.filename_text = L"2026 第三季度销售报告（最终版）.docx";
    return c;
}

void CheckLayout(fluent::Painter& painter, float scale, PickerMode mode) {
    const int m = static_cast<int>(mode);
    const FolderPickerChrome chrome = MakeChrome(mode);
    for (const float w_dip : {640.0f, 1000.0f, 1600.0f}) {
        const float w = w_dip * scale, h = 640.0f * scale;
        const FolderPickerChromeLayout l = LayoutPickerChrome(w, h, chrome, painter, scale);
        const D2D1_RECT_F client = D2D1::RectF(0, 0, w, h);
        Check(std::abs((l.title.bottom - l.title.top) - kPickerTitleDip * scale) < 0.5f,
              "title strip is 40 DIP", scale, m);
        Check(std::abs((l.footer.bottom - l.footer.top) - PickerFooterDip(mode, chrome.overwrite_pending) * scale) < 0.5f,
              "footer height matches what the renderer reserves", scale, m);
        Check(l.footer.bottom == h && l.footer.right == w, "footer spans the bottom edge", scale, m);
        Check(Inside(l.close, l.title) && l.close.right == w, "close sits in the title's corner", scale, m);
        Check(Inside(l.primary, l.footer) && Inside(l.cancel, l.footer),
              "buttons stay inside the footer", scale, m);
        Check(l.cancel.right <= l.primary.left && l.primary.right <= w - 15.0f * scale,
              "cancel then primary, right aligned", scale, m);
        Check(l.primary.right - l.primary.left >= 120.0f * scale - 0.5f &&
              l.cancel.right - l.cancel.left >= 120.0f * scale - 0.5f,
              "buttons keep the 120 DIP minimum", scale, m);
        Check(l.summary.right <= l.cancel.left, "the summary never runs under the buttons", scale, m);
        if (mode == PickerMode::Folder) {
            Check(Inside(l.filename, l.footer) && l.filename.right > l.filename.left && l.filter.right <= l.filter.left && !Overlaps(l.filename, l.primary),
                  "folder name field fits footer without type field or button overlap", scale, m);
        } else {
            Check(Inside(l.filename, l.footer) && Inside(l.filter, l.footer),
                  "name and type fields stay inside the footer", scale, m);
            Check(l.filename_label.right <= l.filename.left && l.filename.right <= l.filter.left,
                  "label, name, type in one row", scale, m);
            Check(!Overlaps(l.filename, l.primary) && !Overlaps(l.filter, l.primary) &&
                  !Overlaps(l.filter, l.cancel),
                  "the field row sits above the button row", scale, m);
            Check(l.filename.right - l.filename.left >= (l.filter.right - l.filter.left) * 0.9f,
                  "the name field gets the most room", scale, m);
            Check(std::abs((l.filename.bottom - l.filename.top) - 34.0f * scale) < 0.5f,
                  "fields are 34 DIP tall", scale, m);
        }
        Check(Inside(l.primary, client), "everything fits the window", scale, m);

        // Hits land on the control under the pointer and nowhere else.
        Check(HitTestPickerChrome(l, chrome, CenterX(l.close), CenterY(l.close)) == kPickClose,
              "close hit", scale, m);
        Check(HitTestPickerChrome(l, chrome, CenterX(l.primary), CenterY(l.primary)) == kPickPrimary,
              "primary hit", scale, m);
        Check(HitTestPickerChrome(l, chrome, CenterX(l.cancel), CenterY(l.cancel)) == kPickCancel,
              "cancel hit", scale, m);
        Check(HitTestPickerChrome(l, chrome, w * 0.5f, h * 0.5f) == kPickNone,
              "the middle of the window belongs to the shared renderer", scale, m);
        Check(HitTestPickerChrome(l, chrome, 60.0f * scale, CenterY(l.title)) == kPickNone,
              "the title text is a drag area", scale, m);
        FolderPickerChrome disabled = chrome;
        auto split = chrome;
        split.primary_drop_down = true;
        const auto split_layout = LayoutPickerChrome(w,h,split,painter,scale);
        Check(HitTestPickerChrome(split_layout,split,split_layout.primary.right-5*scale,CenterY(split_layout.primary))==kPickPrimaryDropDown,
              "Open split arrow has independent hit target",scale,m);
        Check(HitTestPickerChrome(split_layout,split,split_layout.primary.left+5*scale,CenterY(split_layout.primary))==kPickPrimary,
              "Open split main segment retains acceptance",scale,m);
        split.primary_enabled=false;
        Check(HitTestPickerChrome(split_layout,split,split_layout.primary.right-5*scale,CenterY(split_layout.primary))==kPickNone,
              "disabled Open split arrow ignores clicks",scale,m);
        disabled.primary_enabled = false;
        Check(HitTestPickerChrome(l, disabled, CenterX(l.primary), CenterY(l.primary)) == kPickNone,
              "a disabled primary ignores clicks", scale, m);
        if (mode != PickerMode::Folder) {
            Check(HitTestPickerChrome(l, chrome, CenterX(l.filename), CenterY(l.filename)) == kPickFilename,
                  "name field hit", scale, m);
            Check(HitTestPickerChrome(l, chrome, CenterX(l.filename_label), CenterY(l.filename_label)) == kPickFilename,
                  "the label focuses the name field", scale, m);
            Check(HitTestPickerChrome(l, chrome, CenterX(l.filter), CenterY(l.filter)) == kPickFilter,
                  "type list hit", scale, m);
        }
    }
}

bool RenderFooter(Compositor& compositor, fluent::Painter& painter, float scale, PickerMode mode,
                  int theme_index, const std::wstring& out, float filter_turn = 0.0f) {
    const int width = static_cast<int>(1000.0f * scale);
    const int height = static_cast<int>((kPickerTitleDip + 120.0f + PickerFooterDip(mode)) * scale);
    compositor.Resize(width, height);
    FolderPickerChrome chrome = MakeChrome(mode);
    chrome.filter_turn = filter_turn;
    chrome.hover = kPickCancel;
    chrome.focus = kPickPrimary;
    chrome.show_focus = true;
    const FolderPickerChromeLayout layout = LayoutPickerChrome(
        static_cast<float>(width), static_cast<float>(height), chrome, painter, scale);
    const bool hc = theme_index == 2;
    const Theme theme = hc ? MakeHighContrastTheme() : MakeTheme(theme_index == 1, HexColor(0x0078D4));
    auto* dc = compositor.Dc();
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, hc);
    DrawPickerChrome(compositor, painter, theme, chrome, layout, hc);
    if (FAILED(dc->EndDraw())) return false;
    return compositor.SaveSnapshot(out.c_str());
}

// The jump-bar chrome (HTML 跳转条): program title, open folders, target line, conflict.
bool RenderShowcase(Compositor& compositor, fluent::Painter& painter, float scale, int theme_index,
                    bool overwrite, const std::wstring& out, bool jump = false, bool recent = false, bool multi = false) {
    const int width = static_cast<int>(1000.0f * scale), height = static_cast<int>(640.0f * scale);
    compositor.Resize(width, height);
    FolderPickerChrome c = MakeChrome(PickerMode::Save);
    c.title = L"另存为";
    c.app_name = L"Microsoft Word";
    c.primary_text = L"保存";
    c.filter_text = L"Word 文档 (*.docx)";
    c.strip_top = kPickerTitleDip + 46.0f;
    c.strip_left = 0.0f;
    c.recent_left = 208.0f;
    c.recent_top = 166.0f;
    c.context_top = 126.0f; c.context_left = 212.0f; c.context_right = 870.0f;
    c.strip_caption = L"已打开";
    c.open_folders = {{L"季度报告", L"Pulse", L"D:\\工作\\季度报告", true}, {L"设计稿-v3", L"Pulse", L"D:\\设计\\设计稿-v3"},
                      {L"下载", L"Pulse", L"C:\\Users\\SS\\Downloads"},{L"季度数据核对", L"资源管理器", L"E:\\财务\\2026\\季度数据核对", false, true}};
    c.last_location = {L"文档", L"Word 上次", L"C:\\Users\\SS\\Documents"};
    c.undo_jump_text = L"回到 文档";
    c.new_folder_text = L"新建文件夹";
    c.new_folder_key = L"Ctrl+Shift+N";
    c.summary_lead = L"将保存到：";
    c.summary_folder = L"D:\\工作\\季度报告\\";
    c.summary_leaf = L"2026 第三季度销售报告（最终版）.docx";
    c.hover = kPickOpenFolderBase + 1;
    c.more_options_text = L"更多选项";
    {   // HTML wordOptions(): two file properties and the program's check box.
        PickerOptionChip author{601, L"SS"}; author.property = true; author.label = L"作者";
        PickerOptionChip tags{602, L"添加标签"}; tags.property = tags.add = true; tags.label = L"标签";
        c.options = {author, tags, {603, L"保存缩略图", true, true}};
    }
    {   // HTML fchips: the program's type and 全部文件.
        PickerOptionChip word{kPickContextFilterBase, L"Word 文档"}; word.checked = true; word.label = L"*.docx";
        PickerOptionChip all{kPickContextFilterBase + 1, L"所有文件"}; all.label = L"*.*";
        c.context_filters = {word, all};
    }
    if (recent) {
        c.options.clear();
        c.context_filters[0].text = L"图片和 PSD"; c.context_filters[0].label = L"*.psd;*.png;*.jpg";
        c.mode = PickerMode::File;
        c.title = L"打开"; c.app_name = L"Adobe Photoshop"; c.primary_text = L"打开";
        c.filter_text = L"所有格式 (*.PSD;*.PNG;*.JPG…)";
        c.recent_caption = L"最近的图片 · 来自 Photoshop 和 Pulse，不限文件夹";
        c.recent_files = {{L"首页-深色.png", L"设计稿-v3\\导出 · 10 分钟前"}, {L"banner@2x.psd", L"设计稿-v3 · 昨天"},
                          {L"产品主图-白底.jpg", L"下载 · 昨天"}, {L"海报-双十一.psd", L"E:\\资料\\活动 · 3 天前"},
                          {L"季度报告-签字版.pdf", L"季度报告 · 上周"}, {L"图标集.psd", L"设计稿-v3 · 上周"}};
        c.recent_selected = 0;
        c.summary_lead = L"将选择：";
        c.summary_folder = L"D:\\设计\\设计稿-v3\\导出\\";
        c.summary_leaf = L"首页-深色.png";
        c.filename_text = L"首页-深色.png";
        c.undo_jump_text.clear();
    }
    if (multi) {  // HTML 多选: WeChat open, three files picked from the list.
        c.mode = PickerMode::File;
        c.title = L"打开"; c.app_name = L"微信"; c.primary_text = L"发送 (3)";
        c.filter_text = L"所有文件 (*.*)";
        c.options.clear(); c.undo_jump_text.clear();
        c.last_location.source = L"微信 上次";
        PickerOptionChip all{kPickContextFilterBase, L"所有文件"}; all.checked = true; all.label = L"*.*";
        c.context_filters = {all};
        c.selected_names = {L"区域销售明细.xlsx", L"汇报.pptx", L"季度报告-签字版.pdf"};
        c.filename_label = L"已选 3 项";
        c.selection_summary = L"3 个文件 · 9.4 MB";
        c.selection_hint = L"Ctrl 或 Shift 点击继续添加";
        c.summary_lead.clear(); c.summary_folder.clear(); c.summary_leaf.clear();
    }
    if (overwrite) {
        c.overwrite_pending = true;
        c.notice = L"目标名称已存在 2026 第三季度销售报告（最终版）.docx";
        c.overwrite_back_text = L"返回修改";
        c.rename_text = L"改名保存";
        c.primary_text = L"替换";
        c.hover = kPickNone;
    }
    const auto layout = LayoutPickerChrome(static_cast<float>(width), static_cast<float>(height), c, painter, scale);
    Check(layout.open_chips.size() >= 3 && layout.strip.left == 0.0f,
          "strip spans full width and fits three open folders at 1000 DIP", scale, 0);
    if (recent) {
        Check(layout.recent.left >= 208.0f * scale && layout.recent_caption.left >= 208.0f * scale,
              "recent caption stays inside pane beside sidebar", scale, 0);
        for (const auto& card : layout.recent_cards)
            Check(card.left >= 208.0f * scale, "recent card never covers sidebar", scale, 0);
    }
    Check(HitTestPickerChrome(layout, c, (layout.open_chips[1].left + layout.open_chips[1].right) / 2,
                              (layout.open_chips[1].top + layout.open_chips[1].bottom) / 2) == kPickOpenFolderBase + 1,
          "open folder chip hit", scale, 0);
    Check(HitTestPickerChrome(layout, c, (layout.new_folder.left + layout.new_folder.right) / 2,
                              (layout.new_folder.top + layout.new_folder.bottom) / 2) == kPickNewFolder, "new folder hit", scale, 0);
    Check(!Overlaps(layout.undo_jump, layout.new_folder) && !Overlaps(layout.last_location, layout.undo_jump),
          "strip items do not overlap", scale, 0);
    const bool hc = theme_index == 2;
    const Theme theme = hc ? MakeHighContrastTheme() : MakeTheme(theme_index == 1, HexColor(0x527D70));
    auto* dc = compositor.Dc();
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, hc);
    // Stand-ins for what the shared renderer draws: the address row and the panes.
    const float s = scale;
    painter.FillRoundedRect(D2D1::RectF(0, kPickerTitleDip * s, static_cast<float>(width), layout.footer.top), 0, theme.surface_sheet);
    painter.FillRoundedRect(D2D1::RectF(150 * s, 46 * s, 700 * s, 80 * s), 6 * s, theme.surface_card);
    painter.DrawText(L"数据 (D:)  ›  工作  ›  季度报告", D2D1::RectF(164 * s, 46 * s, 690 * s, 80 * s), compositor.TextFormat(), theme.text);
    painter.FillRoundedRect(D2D1::RectF(220 * s, (layout.recent.bottom > 0 ? layout.recent.bottom : layout.strip.bottom) + 6 * s, 990 * s, layout.footer.top - 8 * s), 6 * s, theme.surface_card);
    DrawPickerChrome(compositor, painter, theme, c, layout, hc);
    if (jump) {
        // HTML 输入跳转: "jd" typed in the address row.
        PickerJumpPanel p;
        p.open = true;
        p.query = L"季度";
        p.titles[kJumpOpen] = L"已打开"; p.titles[kJumpRecent] = L"Microsoft Word 最近使用";
        p.titles[kJumpFavorite] = L"收藏夹"; p.titles[kJumpEverywhere] = L"全盘";
        p.hints[0] = L"选择"; p.hints[1] = L"进入"; p.hints[2] = L"补全路径"; p.hints[3] = L"直接保存到这里"; p.hints[4] = L"返回";
        p.foot_note = L"支持拼音首字母 · 路径片段";
        p.rows = {
            {kJumpOpen, L"季度报告", L"D:\\工作\\季度报告", L"Pulse 标签", L"Alt+1"},
            {kJumpOpen, L"季度数据核对", L"E:\\财务\\2026\\季度数据核对", L"资源管理器", L"Alt+4", true},
            {kJumpRecent, L"季度报告\\2026 Q2", L"D:\\工作\\季度报告\\2026 Q2", L"7 月 3 日"},
            {kJumpEverywhere, L"季度报告备份", L"E:\\备份\\2025\\季度报告备份", L"文件夹"},
            {kJumpEverywhere, L"2025 第四季度销售报告.docx", L"D:\\归档\\2025\\2025 第四季度销售报告.docx", L"同类文件", L"", false, true},
            {kJumpCreate, L"在 季度报告 中新建文件夹“季度”", L"D:\\工作\\季度报告\\季度", L"", L"Ctrl+Shift+Enter"},
        };
        p.hover = 2;
        const auto anchor = D2D1::RectF(150 * s, 46 * s, 700 * s, 80 * s);
        const auto jl = LayoutPickerJump(p, anchor, static_cast<float>(width), layout.footer.top - 8 * s, s);
        Check(jl.rows.size() == p.rows.size() && jl.panel.bottom <= layout.footer.top, "jump panel fits six rows above the footer", scale, 0);
        Check(HitTestPickerJump(jl, (jl.rows[3].left + jl.rows[3].right) / 2, (jl.rows[3].top + jl.rows[3].bottom) / 2) == 3, "jump row hit", scale, 0);
        DrawPickerJump(compositor, painter, theme, p, jl, hc);
    }
    if (FAILED(dc->EndDraw())) return false;
    return compositor.SaveSnapshot(out.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool options_only = argc > 1 && std::wstring_view(argv[1]) == L"--options";
    const bool labels_only=argc>1 && std::wstring_view(argv[1])==L"--labels";
    const std::wstring output = (labels_only || options_only) ? (argc>2 ? argv[2] : L"bench_data\\picker-label-layout") : argc > 1 ? argv[1] : L"build\\file-picker-visual";
    CreateDirectoryW(output.c_str(), nullptr);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulsePickerChromeTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 900, 600,
                                nullptr, nullptr, wc.hInstance, nullptr);
    int written = 0;
    {
        Compositor compositor;
        if (!compositor.Init(hwnd)) return 2;
        if(labels_only) {
            for(float scale:{1.0f,1.5f,2.0f}) {
                compositor.RecreateTextFormats(scale);fluent::Painter painter(&compositor);painter.SetScale(scale);
                for(float width:{640.0f,1000.0f}) {
                    auto chrome=MakeChrome(PickerMode::File);chrome.primary_text=std::wstring(1024,L'W');chrome.filename_label=std::wstring(1024,L'F');
                    const auto layout=LayoutPickerChrome(width*scale,640*scale,chrome,painter,scale);
                    Check(Inside(layout.primary,layout.footer) && Inside(layout.cancel,layout.footer) && !Overlaps(layout.primary,layout.cancel),"long caller button label stays in bounded button without overlap",scale,2);
                    Check(layout.filename.right>layout.filename.left && Inside(layout.filename,layout.footer) && !Overlaps(layout.filename_label,layout.filename) && !Overlaps(layout.filename,layout.filter),"long caller filename label preserves input and filter widths",scale,2);
                    auto overwrite = chrome;
                    overwrite.overwrite_pending = true;
                    overwrite.overwrite_back_text = L"Back to editing";
                    overwrite.rename_text = L"Save with another name";
                    const auto confirm = LayoutPickerChrome(width * scale, 640 * scale, overwrite, painter, scale);
                    Check(std::abs(confirm.footer.bottom - confirm.footer.top - PickerFooterDip(PickerMode::File, true) * scale) < 0.5f,
                        "inline overwrite footer adds its banner row", scale, 2);
                    const D2D1_RECT_F buttons[] = {confirm.overwrite_back, confirm.rename, confirm.cancel, confirm.primary};
                    for (int i = 0; i < 4; ++i) {
                        Check(Inside(buttons[i], confirm.footer) && buttons[i].right > buttons[i].left &&
                            !Overlaps(buttons[i], confirm.summary) && !Overlaps(buttons[i], confirm.filename),
                            "overwrite actions stay inside footer clear of fields and hint", scale, 2);
                        for (int j = i + 1; j < 4; ++j)
                            Check(!Overlaps(buttons[i], buttons[j]), "overwrite actions do not overlap", scale, 2);
                    }
                    Check(HitTestPickerChrome(confirm, overwrite, (confirm.rename.left + confirm.rename.right) / 2,
                        (confirm.rename.top + confirm.rename.bottom) / 2) == kPickRename,
                        "rename action has its own hit target", scale, 2);
                    for(bool dark:{false,true}) {
                        compositor.Resize(static_cast<int>(width*scale),static_cast<int>(640*scale));
                        auto* dc=compositor.Dc();const auto theme=MakeTheme(dark,HexColor(0x0078D4));dc->BeginDraw();dc->Clear(theme.bg);painter.BeginFrame(theme,false);
                        DrawPickerChrome(compositor,painter,theme,chrome,layout,false);
                        Check(dc->EndDraw()==S_OK,"long labels render with end ellipsis",scale,2);
                        const auto name=output+L"\\labels-"+std::to_wstring(static_cast<int>(width))+L"-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
                        Check(compositor.SaveSnapshot(name.c_str()),"label layout screenshot saved for visual review",scale,2);
                    }
                }
            }
            compositor.Shutdown();DestroyWindow(hwnd);CoUninitialize();return g_failures ? 1 : 0;
        }
        if (options_only) {
            for (const float scale : {1.0f, 1.5f, 2.0f}) {
                compositor.RecreateTextFormats(scale);
                fluent::Painter painter(&compositor);
                painter.SetScale(scale);
                for (const float width : {420.0f, 640.0f, 1000.0f}) for (int t = 0; t < 3; ++t) {
                    PickerJumpPanel jump;
                    jump.open = true;
                    for (int i = 0; i < 20; ++i) jump.rows.push_back({kJumpOpen, L"Folder", L"C:\\Folder"});
                    jump.selected = 19;
                    const auto jl = LayoutPickerJump(jump, D2D1::RectF(10 * scale, 40 * scale, 300 * scale, 80 * scale), width * scale, 330 * scale, scale);
                    Check(jl.rows.size() == 20 && jl.rows[19].bottom > jl.rows[19].top,
                        "keyboard selection remains visible beyond first page", scale, 2);
                    Check(jl.panel.bottom <= 330 * scale, "short jump panel stays above footer", scale, 2);
                    Check(HitTestPickerJump(jl, CenterX(jl.rows[19]), CenterY(jl.rows[19])) == 19,
                        "scrolled jump hit preserves model index", scale, 2);
                    auto c = MakeChrome(PickerMode::File);
                    c.more_options_text = L"更多选项";
                    c.footer_tool = {506, L"工具", false, false, true, true};
                    c.options = {{501, L"只读", true, true}, {502, L"UTF-8", false, false, true, true, L"编码(E):"},
                        {503, L"作者 张三"}, {504, L"标签 添加"}, {505, L"扩展属性"}};
                    c.selected_names = {L"报告.docx", L"预算.xlsx", L"截图.png"};
                    c.strip_top = 86; c.strip_caption = L"已打开";
                    c.open_folders = {{L"报告", L"", L"C:\\Reports"}, {L"设计", L"", L"C:\\Design"}, {L"下载", L"", L"C:\\Downloads"}};
                    c.context_top = 126; c.context_left = 212; c.context_right = width - 130;
                    c.new_folder_text = L"新建文件夹"; c.undo_jump_text = L"回到 文档";
                    c.context_filters = {{601, L"全部文件"}, {602, L"图片"}};
                    c.recent_left = 208; c.recent_top = 166;
                    c.recent_files = {{L"图片.png", L"最近使用"}};
                    c.selection_summary = L"3 个文件 · 9.4 MB";
                    c.selection_hint = L"Ctrl 或 Shift 点击继续添加";
                    size_t thumbnail_requests = 0;
                    c.draw_recent_thumbnail = [&](size_t, const D2D1_RECT_F&) { ++thumbnail_requests; return false; };
                    const auto l = LayoutPickerChrome(width * scale, 640 * scale, c, painter, scale);
                    if (l.footer_tool.right > l.footer_tool.left) {
                        Check(l.summary.right < l.footer_tool.left && l.footer_tool.right < l.cancel.left, "footer tool separates summary and cancel", scale, 2);
                        Check(HitTestPickerChrome(l, c, CenterX(l.footer_tool), CenterY(l.footer_tool)) == 506, "footer tool retains host hit", scale, 2);
                    } else Check(width < 640, "footer tool only hides when narrow", scale, 2);
                    if (l.option_chips.size() > 1) {
                        Check(l.option_labels[1].right < l.option_buttons[1].left, "combo label remains outside dropdown", scale, 2);
                        Check(HitTestPickerChrome(l, c, CenterX(l.option_labels[1]), CenterY(l.option_labels[1])) == 502, "combo label retains whole-chip hit", scale, 2);
                    }
                    for (size_t i = 0; i < l.open_keycaps.size(); ++i) {
                        const auto& key = l.open_keycaps[i];
                        Check(Inside(key, l.open_chips[i]) && key.right - key.left + 0.01f >= painter.MeasureBadgeWidth(L"Alt " + std::to_wstring(i + 1)),
                              "each folder keycap fits full Alt digit label", scale, 2);
                        Check(HitTestPickerChrome(l, c, CenterX(key), CenterY(key)) == kPickOpenFolderBase + static_cast<int>(i),
                              "full keycap retains folder chip hit id", scale, 2);
                    }
                    for (size_t i = 0; i < l.recent_cards.size(); ++i) {
                        Check(std::abs(l.recent_thumbnails[i].bottom - l.recent_thumbnails[i].top - 74 * scale) < 0.01f,
                              "recent thumbnail is 74 DIP", scale, 2);
                        Check(Inside(l.recent_thumbnails[i], l.recent_cards[i]) && Inside(l.recent_names[i], l.recent_cards[i]) &&
                              Inside(l.recent_details[i], l.recent_cards[i]) && l.recent_details[i].bottom <= l.recent_cards[i].bottom - 6 * scale + 0.01f,
                              "thumbnail and both text rows fit card with bottom padding", scale, 2);
                        Check(!Overlaps(l.recent_thumbnails[i], l.recent_names[i]) && !Overlaps(l.recent_names[i], l.recent_details[i]),
                              "recent text never overlaps thumbnail or detail", scale, 2);
                    }
                    Check(Inside(l.more_options_badge, l.more_options) && Inside(l.more_options_label, l.more_options) &&
                          !Overlaps(l.more_options_badge, l.more_options_label), "more count badge has separate nonoverlapping label slot", scale, 2);
                    Check(HitTestPickerChrome(l, c, CenterX(l.more_options_badge), CenterY(l.more_options_badge)) == kPickMoreOptions,
                          "count badge retains more-options click target", scale, 2);
                    const auto ctx = D2D1::RectF(212 * scale, 126 * scale, (width - 130) * scale, 166 * scale);
                    for (const auto& button : {l.new_folder, l.undo_jump}) if (button.right > button.left)
                        Check(Inside(button, ctx) && !Overlaps(button, l.strip), "context action stays in body not jump strip", scale, 2);
                    for (size_t i = 0; i < l.context_filters.size(); ++i) {
                        Check(painter.MeasureButtonWidth(c.context_filters[i].text) <=
                              l.context_filters[i].right - l.context_filters[i].left + 0.01f,
                              "short context labels have full unellipsized text width", scale, 2);
                        Check(Inside(l.context_filters[i], ctx) && !Overlaps(l.context_filters[i], l.strip),
                              "context filter stays inside narrow body bounds", scale, 2);
                        Check(HitTestPickerChrome(l, c, CenterX(l.context_filters[i]), CenterY(l.context_filters[i])) == c.context_filters[i].id,
                              "context filter hit returns control id", scale, 2);
                    }
                    if (width == 1000) Check(l.context_filters.size() == 2 && l.new_folder.right > l.new_folder.left,
                        "wide context displays creation and both filters", scale, 2);
                    Check(l.strip.left == 0 && l.recent.top == 166 * scale && l.recent_caption.left >= 208 * scale,
                          "full width strip precedes body context then pane recent cards", scale, 2);
                    Check(l.option_chips.size() <= 4, "options cap at four", scale, 2);
                    Check(Inside(l.more_options, l.options), "overflow stays in option row", scale, 2);
                    Check(HitTestPickerChrome(l, c, CenterX(l.more_options), CenterY(l.more_options)) == kPickMoreOptions,
                        "overflow hit opens more options", scale, 2);
                    Check(!Overlaps(l.options, l.filename) && !Overlaps(l.options, l.primary), "options clear filename and actions", scale, 2);
                    for (size_t i = 0; i < l.option_chips.size(); ++i) {
                        Check(Inside(l.option_chips[i], l.options), "option chip stays in row", scale, 2);
                        Check(HitTestPickerChrome(l, c, CenterX(l.option_chips[i]), CenterY(l.option_chips[i])) == c.options[i].id,
                            "option hit returns host control id", scale, 2);
                    }
                    compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(640 * scale));
                    const auto theme = t == 2 ? MakeHighContrastTheme() : MakeTheme(t == 1, HexColor(0x527D70));
                    auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, t == 2);
                    DrawPickerChrome(compositor, painter, theme, c, l, t == 2);
                    Check(SUCCEEDED(dc->EndDraw()), "options and tray render", scale, 2);
                    Check(thumbnail_requests == l.recent_cards.size(), "uncached recent cards invoke cache-only thumbnail callback", scale, 2);
                    const auto file = output + L"\\options-" + std::to_wstring(static_cast<int>(width)) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + L"-" + std::to_wstring(t) + L".png";
                    Check(compositor.SaveSnapshot(file.c_str()), "options screenshot saved", scale, 2);
                    auto folder = c; folder.mode = PickerMode::Folder; folder.context_filters = {{603, L"只看文件夹", false, true}};
                    folder.undo_jump_text.clear(); folder.new_folder_text.clear(); folder.recent_files.clear();
                    const auto folder_layout = LayoutPickerChrome(width * scale, 640 * scale, folder, painter, scale);
                    if (width >= 640) Check(folder_layout.context_filters.size() == 1 &&
                        HitTestPickerChrome(folder_layout, folder, CenterX(folder_layout.context_filters[0]), CenterY(folder_layout.context_filters[0])) == 603,
                        "folder-only chip retains its supplied control identity", scale, 2);
                    dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, t == 2);
                    DrawPickerChrome(compositor, painter, theme, folder, folder_layout, t == 2);
                    Check(SUCCEEDED(dc->EndDraw()), "folder-only chip reuses vector folder and toggle control", scale, 2);
                    Check(compositor.SaveSnapshot((file + L"-folder.png").c_str()), "folder-only screenshot saved", scale, 2);
                    auto clash = c;
                    clash.overwrite_pending = true; clash.overwrite_replace_text = L"替换";
                    clash.rename_text = L"保存为 报告（最终版）(2).docx"; clash.primary_text = L"保存"; clash.primary_enabled = false;
                    clash.notice = L"季度报告里已有同名文件 · 昨天 18:02 · 1.3 MB";
                    const auto clash_layout = LayoutPickerChrome(width * scale, 640 * scale, clash, painter, scale);
                    Check(Inside(clash_layout.overwrite_replace, clash_layout.conflict) && Inside(clash_layout.rename, clash_layout.conflict) &&
                          !Overlaps(clash_layout.rename, clash_layout.overwrite_replace), "candidate rename and replace fit conflict banner", scale, 2);
                    Check(HitTestPickerChrome(clash_layout, clash, CenterX(clash_layout.overwrite_replace), CenterY(clash_layout.overwrite_replace)) == kPickOverwriteReplace &&
                          HitTestPickerChrome(clash_layout, clash, CenterX(clash_layout.primary), CenterY(clash_layout.primary)) == kPickNone,
                          "banner replacement is independent from disabled Save", scale, 2);
                    dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, t == 2);
                    DrawPickerChrome(compositor, painter, theme, clash, clash_layout, t == 2);
                    Check(SUCCEEDED(dc->EndDraw()), "HTML conflict actions render", scale, 2);
                    Check(compositor.SaveSnapshot((file + L"-clash.png").c_str()), "HTML conflict screenshot saved", scale, 2);
                    auto notice_chrome = c;
                    notice_chrome.auto_jump_notice = L"已跳到 Pulse 当前文件夹 季度报告";
                    notice_chrome.draw_recent_thumbnail = [&](size_t, const D2D1_RECT_F& r) {
                        painter.FillRoundedRect(r, 0, theme.accent);
                        return true;
                    };
                    const auto notice_layout = LayoutPickerChrome(width * scale, 640 * scale, notice_chrome, painter, scale);
                    if (width == 1000) {
                        Check(notice_layout.auto_jump_notice.right > notice_layout.auto_jump_notice.left &&
                              Inside(notice_layout.undo_jump, notice_layout.auto_jump_notice), "auto-jump explanation contains undo action", scale, 2);
                        Check(HitTestPickerChrome(notice_layout, notice_chrome, CenterX(notice_layout.undo_jump), CenterY(notice_layout.undo_jump)) == kPickUndoJump,
                              "auto-jump infobar undo keeps original hit", scale, 2);
                    }
                    Check(notice_layout.auto_jump_notice.top == 166 * scale &&
                          notice_layout.auto_jump_notice.bottom - notice_layout.auto_jump_notice.top == kPickerAutoJumpNoticeDip * scale &&
                          notice_layout.recent.top >= notice_layout.auto_jump_notice.bottom &&
                          !Overlaps(notice_layout.auto_jump_notice, notice_layout.new_folder),
                          "independent auto-jump band precedes recent cards without toolbar overlap", scale, 2);
                    dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, t == 2);
                    DrawPickerChrome(compositor, painter, theme, notice_chrome, notice_layout, t == 2);
                    Check(SUCCEEDED(dc->EndDraw()), "auto-jump explanation and selection summary render", scale, 2);
                    Check(compositor.SaveSnapshot((file + L"-notice.png").c_str()), "auto-jump screenshot saved", scale, 2);
                    PickerJumpPanel indexed;
                    indexed.open = true; indexed.query = L"季度";
                    indexed.index_notice = L"全盘索引服务未连接，正在使用用户索引：只搜索桌面、文档和下载。";
                    indexed.index_action_text = L"了解索引服务";
                    indexed.titles[kJumpOpen] = L"已打开";
                    indexed.titles[kJumpEverywhere] = L"用户文件夹";
                    indexed.group_details[kJumpOpen] = L"1";
                    indexed.group_details[kJumpEverywhere] = L"共 2 个 · 20 ms";
                    indexed.rows = {{kJumpOpen, L"季度报告", L"D:\\报告"}, {kJumpEverywhere, L"季度总结", L"D:\\总结"}};
                    const auto index_layout = LayoutPickerJump(indexed, D2D1::RectF(10 * scale, 40 * scale, (width - 10) * scale, 80 * scale),
                        width * scale, 490 * scale, scale);
                    Check(index_layout.index_notice.bottom <= index_layout.rows[0].top && index_layout.panel.bottom <= 490 * scale,
                          "index explanation reserves space without covering result rows", scale, 2);
                    Check(HitTestPickerJump(index_layout, CenterX(index_layout.index_action), CenterY(index_layout.index_action)) == kPickerJumpIndexInfoHit,
                          "index learn action has distinct hit", scale, 2);
                    dc->BeginDraw(); dc->Clear(theme.bg); painter.BeginFrame(theme, t == 2);
                    DrawPickerJump(compositor, painter, theme, indexed, index_layout, t == 2);
                    Check(SUCCEEDED(dc->EndDraw()), "index notice and supplied result statistics render", scale, 2);
                    Check(compositor.SaveSnapshot((file + L"-index.png").c_str()), "index explanation screenshot saved", scale, 2);
                    indexed.index_notice.clear(); indexed.index_action_text.clear(); indexed.group_details[kJumpEverywhere].clear();
                    const auto unknown = LayoutPickerJump(indexed, D2D1::RectF(10 * scale, 40 * scale, (width - 10) * scale, 80 * scale), width * scale, 490 * scale, scale);
                    Check(unknown.index_notice.right == unknown.index_notice.left && unknown.index_action.right == unknown.index_action.left,
                          "unknown index state has no fabricated notice or action", scale, 2);
                }
            }
            compositor.Shutdown(); DestroyWindow(hwnd); CoUninitialize();
            std::printf("options failures=%d\n", g_failures);
            return g_failures ? 1 : 0;
        }
        const wchar_t* theme_names[] = {L"light", L"dark", L"hc"};
        const wchar_t* mode_names[] = {L"folder", L"image", L"file"};
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            compositor.RecreateTextFormats(scale);
            fluent::Painter painter(&compositor);
            painter.SetScale(scale);
            if (scale == 1.5f) {
                for (const float turn : {0.5f, 1.0f}) {
                    const auto out = output + (turn < 1.0f ? L"\\filter_turning.png" : L"\\filter_open.png");
                    if (!RenderFooter(compositor, painter, scale, PickerMode::Image, 0, out, turn)) ++g_failures;
                    else ++written;
                }
            }
            if (scale == 1.5f) {
                const wchar_t* names[] = {L"light", L"dark", L"hc"};
                for (int t = 0; t < 2; ++t) {
                    const auto open = output + L"\\jump_" + names[t] + L"_recent.png";
                    if (!RenderShowcase(compositor, painter, scale, t, false, open, false, true)) ++g_failures;
                    else ++written;
                }
                for (int t = 0; t < 3; ++t) {
                    const auto multi = output + L"\\jump_" + names[t] + L"_multi.png";
                    if (!RenderShowcase(compositor, painter, scale, t, false, multi, false, false, true)) ++g_failures;
                    else ++written;
                }
                for (int t = 0; t < 3; ++t) {
                    const auto panel = output + L"\\jump_" + names[t] + L"_panel.png";
                    if (!RenderShowcase(compositor, painter, scale, t, false, panel, true)) ++g_failures;
                    else ++written;
                }
                for (int t = 0; t < 3; ++t) for (const bool overwrite : {false, true}) {
                    const auto out = output + L"\\jump_" + names[t] + (overwrite ? L"_conflict" : L"") + L".png";
                    if (!RenderShowcase(compositor, painter, scale, t, overwrite, out)) ++g_failures;
                    else ++written;
                }
            }
            for (const PickerMode mode : {PickerMode::Folder, PickerMode::Image, PickerMode::File}) {
                CheckLayout(painter, scale, mode);
                if (scale == 2.0f) continue;
                for (int t = 0; t < 3; ++t) {
                    const std::wstring out = output + L"\\chrome_" + mode_names[static_cast<int>(mode)] +
                        L"_" + theme_names[t] + (scale > 1.0f ? L"_150" : L"") + L".png";
                    const bool ok = RenderFooter(compositor, painter, scale, mode, t, out);
                    if (!ok) ++g_failures;
                    else ++written;
                }
            }
        }
        compositor.Shutdown();
    }
    DestroyWindow(hwnd);
    std::printf("chrome PNGs written=%d failures=%d\n", written, g_failures);
    CoUninitialize();
    return g_failures ? 1 : 0;
}
