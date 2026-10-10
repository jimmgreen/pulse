// Visual contract for Pulse's own confirm dialogs. The open dialog hosts the
// main window's renderer and is exercised with pulse.exe --picker-live.
//
//   pulse_dialog_gallery.exe [--lang zh-CN|en-US] [out_dir]
//       Renders every scene offscreen in light, dark and high contrast at 100%
//       and 150% scale into out_dir (default build-dialog-verify).
//   pulse_dialog_gallery.exe --live <kind> [--dark] [--out <file>]
//       Opens the real window. kind: confirm-warning, confirm-danger,
//       confirm-three, confirm-items. The choice is written to --out (UTF-8).

#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/confirm_dialog_view.h"
#include "../ui/fluent_components.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace pulse::ui;
using pulse::l10n::StringId;

LRESULT CALLBACK GalleryWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct Variant {
    const wchar_t* name;
    bool dark;
    bool high_contrast;
};

constexpr Variant kVariants[] = {
    {L"light", false, false},
    {L"dark", true, false},
    {L"hc", false, true},
};

Theme ThemeFor(const Variant& v) {
    return v.high_contrast ? MakeHighContrastTheme() : MakeTheme(v.dark, HexColor(0x0078D4));
}

bool RenderScene(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                 bool high_contrast, float scale, int width, int height,
                 const std::wstring& output,
                 const std::function<void()>& draw) {
    compositor.Resize(width, height);
    auto* dc = compositor.Dc();
    if (!dc) return false;
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, high_contrast);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    painter.FillRoundedRect(D2D1::RectF(0, 0, w, h), 0, theme.bg);
    painter.StrokeRoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 12.0f * scale,
                              theme.stroke_card);
    draw();
    if (FAILED(dc->EndDraw())) return false;
    return compositor.SaveSnapshot(output.c_str());
}

// ---- confirm scenes -------------------------------------------------------

struct ConfirmScene {
    const wchar_t* name;
    ConfirmDialogSpec spec;
    ConfirmDialogVisual visual;
};

std::vector<ConfirmScene> ConfirmScenes() {
    std::vector<ConfirmScene> scenes;
    {
        ConfirmScene s{L"confirm_warning", {}, {}};
        s.spec.title = L"继续导出诊断信息？";
        s.spec.message = L"诊断包可能包含文件路径和最近打开的位置。只在需要排查问题时发送给开发者。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::ConfirmContinue);
        s.spec.tone = ConfirmTone::Warning;
        s.spec.default_choice = ConfirmChoice::Cancel;
        s.visual.focus = kConfirmCancel;
        s.visual.show_focus = true;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_danger", {}, {}};
        s.spec.title = L"永久删除 7 个项目？";
        s.spec.message = L"这些项目不会进入回收站，删除后无法恢复。";
        s.spec.confirm_text = L"永久删除";
        s.spec.danger = true;
        s.spec.items = {L"C:\\Users\\Example\\Desktop\\报告 2026.docx", L"C:\\Users\\Example\\Desktop\\预算.xlsx",
                        L"C:\\Users\\Example\\Desktop\\照片", L"C:\\Users\\Example\\Desktop\\notes.md",
                        L"C:\\Users\\Example\\Desktop\\demo.mp4", L"C:\\Users\\Example\\Desktop\\a.txt",
                        L"C:\\Users\\Example\\Desktop\\b.txt"};
        s.spec.default_choice = ConfirmChoice::Cancel;
        s.visual.focus = kConfirmCancel;
        s.visual.hover = kConfirmPrimary;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_three", {}, {}};
        s.spec.title = L"同时导出索引服务的诊断信息？";
        s.spec.message = L"索引服务的日志能帮助排查搜索问题，但会让诊断包更大。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::DiagnosticsIncludeServiceYes);
        s.spec.secondary_text = pulse::l10n::Get(StringId::DiagnosticsIncludeServiceNo);
        s.spec.tone = ConfirmTone::Question;
        s.spec.default_choice = ConfirmChoice::Secondary;
        s.visual.focus = kConfirmSecondary;
        s.visual.show_focus = true;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_items", {}, {}};
        s.spec.title = L"文件正在被其他程序使用";
        s.spec.message = L"「季度报告.docx」已在下列程序中打开。关闭它们后重试。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::RecoveryRetry);
        s.spec.items = {L"Microsoft Word（WINWORD.EXE）", L"Windows 资源管理器（explorer.exe）"};
        s.spec.item_glyph = L"\xE7C4";
        s.spec.note = L"关闭程序前请先保存它里面的改动。";
        s.spec.tone = ConfirmTone::Warning;
        scenes.push_back(std::move(s));
    }
    return scenes;
}

int RunGallery(const std::wstring& out_dir) {
    CreateDirectoryW(out_dir.c_str(), nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = GalleryWndProc;
    window_class.lpszClassName = L"PulseDialogGallery";
    RegisterClassW(&window_class);
    HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"Pulse Dialog Gallery",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                nullptr, nullptr, window_class.hInstance, nullptr);
    if (!hwnd) return 1;
    Compositor compositor;
    if (!compositor.Init(hwnd)) {
        DestroyWindow(hwnd);
        return 2;
    }
    int failures = 0;
    int written = 0;
    const float scales[] = {1.0f, 1.5f};
    for (const float scale : scales) {
        compositor.RecreateTextFormats(scale);
        fluent::Painter painter(&compositor);
        painter.SetScale(scale);
        const std::wstring suffix = scale > 1.0f ? L"_150" : L"";
        for (const Variant& variant : kVariants) {
            const Theme theme = ThemeFor(variant);
            for (const ConfirmScene& scene : ConfirmScenes()) {
                const ConfirmDialogSpec spec = NormalizeConfirmSpec(scene.spec);
                ConfirmDialogFormats formats;
                formats.Create(compositor.DwriteFactory(), scale);
                const ConfirmDialogLayout layout = LayoutConfirmDialog(
                    spec, painter, compositor.DwriteFactory(), formats, scale);
                const std::wstring out = out_dir + L"\\" + scene.name + L"_" + variant.name +
                                         suffix + L".png";
                const bool ok = RenderScene(
                    compositor, painter, theme, variant.high_contrast, scale,
                    static_cast<int>(layout.width + 0.99f), static_cast<int>(layout.height + 0.99f),
                    out, [&] {
                        DrawConfirmDialog(compositor, painter, theme, spec, layout, formats,
                                          scene.visual, variant.dark, variant.high_contrast);
                    });
                ok ? ++written : ++failures;
            }
        }
    }
    compositor.Shutdown();
    DestroyWindow(hwnd);
    const std::wstring log = out_dir + L"\\gallery.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, log.c_str(), L"wb") == 0 && f) {
        std::fprintf(f, "written=%d failed=%d\r\n", written, failures);
        fclose(f);
    }
    return failures == 0 ? 0 : 4;
}

void WriteResult(const std::wstring& file, const std::wstring& text) {
    if (file.empty()) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                          nullptr);
    std::string utf8(static_cast<size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
    if (bytes > 1)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

int RunLive(const std::wstring& kind, bool dark, const std::wstring& out) {
    const D2D1_COLOR_F accent = HexColor(0x0078D4);
    if (kind.rfind(L"confirm-", 0) == 0) {
        const std::wstring name = L"confirm_" + kind.substr(8);
        for (const ConfirmScene& scene : ConfirmScenes()) {
            if (name != scene.name) continue;
            const ConfirmChoice choice = ShowConfirmDialogEx(nullptr, scene.spec, dark, accent);
            WriteResult(out, choice == ConfirmChoice::Confirm ? L"confirm"
                             : choice == ConfirmChoice::Secondary ? L"secondary" : L"cancel");
            return 0;
        }
        return 5;
    }
    return 5;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    std::wstring language = L"zh-CN";
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--lang" && i + 1 < argc) language = argv[++i];
        else args.emplace_back(argv[i]);
    }
    if (argv) LocalFree(argv);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), language);

    int result = 0;
    if (!args.empty() && args[0] == L"--live" && args.size() >= 2) {
        bool dark = false;
        std::wstring out;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--dark") dark = true;
            else if (args[i] == L"--out" && i + 1 < args.size()) out = args[++i];
        }
        result = RunLive(args[1], dark, out);
    } else {
        result = RunGallery(args.empty() ? L"build-dialog-verify" : args[0]);
    }
    CoUninitialize();
    return result;
}