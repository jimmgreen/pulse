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
        Check(std::abs((l.footer.bottom - l.footer.top) - PickerFooterDip(mode) * scale) < 0.5f,
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool labels_only=argc>1 && std::wstring_view(argv[1])==L"--labels";
    const std::wstring output = labels_only ? (argc>2 ? argv[2] : L"bench_data\\picker-label-layout") : argc > 1 ? argv[1] : L"build\\file-picker-visual";
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
                    Check(std::abs(confirm.footer.bottom - confirm.footer.top - 140.0f * scale) < 0.5f,
                        "inline overwrite footer reserves 140 DIP", scale, 2);
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
