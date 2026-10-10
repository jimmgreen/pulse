#include "global_search_window.h"
#include "global_search_result_state.h"
#include "../ui/FluentTokens.h"
#include "resource.h"
#include "../ui/empty_state_layout.h"
#include "../ui/fluent_components.h"
#include "../ui/typography.h"
#include "../ui/svg_bitmap.h"
#include "../ui/edit_host.h"
#include "../common/path_utils.h"
#include <uxtheme.h>
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../index/content_search_client.h"
#include "../index/network_agent_client.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

namespace pulse {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;
constexpr UINT kIndexStatus = WM_APP + 181, kIndexResult = WM_APP + 182, kContentResult = WM_APP + 183;
constexpr UINT kNetworkResult = WM_APP + 184;
constexpr UINT_PTR kDebounce = 1;
constexpr UINT_PTR kConnectTimeout = 2;
constexpr UINT_PTR kScopeTooltipTimer = 3;
constexpr uint64_t kSession = 0x50554c534547534full;
constexpr float kHeader = 76, kTabs = 52, kFooter = 48, kRow = 88;
constexpr float kEditCornerRadius = 4;
constexpr float kEditLeft = 116, kEditRight = 72, kEditTextInset = 8;
constexpr size_t kMaximumResults = 200;
const std::wstring& Text(StringId id) { return l10n::Get(id); }
std::wstring ParentPath(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash == 2 ? 3 : slash);
}
// Shell providers may block. This worker owns all its data and outlives the popup safely.
void OpenResult(std::wstring path, bool location) {
    std::thread([path = std::move(path), location] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (location) {
            PIDLIST_ABSOLUTE item = nullptr;
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
                SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                CoTaskMemFree(item);
            }
        } else {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&info);
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}
}

struct GlobalSearchWindow::Impl {
    HWND hwnd = nullptr, edit = nullptr;
    HFONT edit_font = nullptr;
    ComPtr<IDWriteTextFormat> edit_format;
    HBRUSH background = nullptr;
    bool dark = true, content_mode = false, current_only = false, composing = false, busy = false, truncated = false, search_pinyin = true;
    float scale = 1, width = 780, height = 488;
    // Extra header height (DIP) when the interface font size makes the query
    // line taller than the 35 DIP field; everything below moves down by it.
    float grow = 0;
    std::wstring current_folder, current_folder_tip, query, error;
    struct Row { std::wstring name, path, snippet; bool directory = false; };
    std::vector<Row> rows;
    int selected = 0, first = 0;
    uint32_t generation = 1;
    size_t total = 0;
    index::IndexClient filenames;
    index::NetworkAgentClient network;
    index::SearchResult local_result, network_result;
    app::GlobalProviderStatus providers;
    index::ContentSearchClient contents;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> write_factory;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> logo;
    ID2D1RenderTarget* logo_target = nullptr;
    ComPtr<ID2D1Bitmap> empty_art;
    ID2D1RenderTarget* empty_art_target = nullptr;
    float empty_art_scale = 0;
    ui::Compositor compositor;
    ui::fluent::Painter painter;
    ui::WindowMaterial material;
    ui::WindowEffect effect = ui::WindowEffect::None;
    std::wstring background_image;
    D2D1_COLOR_F accent_color = ui::GetAccentColor();
    bool backdrop = false;
    bool scope_hover = false, scope_tooltip_visible = false, tracking_mouse = false;
    GlobalSearchHandoffHandler handoff;
    D2D1_RECT_F handoff_rect{};
    bool handoff_hover = false;
    float scope_pointer_x = 0, scope_pointer_y = 0;

    ui::Theme Theme() const { return ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark, accent_color); }
    D2D1_COLOR_F EditBackground() const {
        return Theme().header_bg;
    }
    static UINT32 Rgb(D2D1_COLOR_F color) {
        return (static_cast<UINT32>(std::lround(color.r * 255)) << 16) |
            (static_cast<UINT32>(std::lround(color.g * 255)) << 8) | static_cast<UINT32>(std::lround(color.b * 255));
    }
    void ApplyAppearance() {
        if (!hwnd) return;
        backdrop = ui::ApplyWindowEffect(hwnd, background_image.empty() ? effect : ui::WindowEffect::None, dark);
        if (background) DeleteObject(background);
        background = CreateSolidBrush(Background());
        Invalidate();
        if (edit) InvalidateRect(edit, nullptr, TRUE);
    }

    int Px(float dip) const { return static_cast<int>(std::lround(dip * scale)); }
    COLORREF Background() const {
        const auto color = Theme().header_bg;
        return RGB(static_cast<BYTE>(color.r * 255), static_cast<BYTE>(color.g * 255), static_cast<BYTE>(color.b * 255));
    }
    D2D1_COLOR_F Color(UINT32 value) const { return D2D1::ColorF(value); }
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    float Header() const { return kHeader + grow; }
    float ListTop() const { return 136 + grow; }
    int PageSize() const { return std::max(1, static_cast<int>((height - Header() - kTabs - kFooter) / kRow)); }
    void ClampSelection(bool reveal_selection = true) {
        if (rows.empty()) selected = -1;
        else if (selected >= 0) selected = std::min(selected, static_cast<int>(rows.size()) - 1);
        if (reveal_selection && selected >= 0) {
            if (selected < first) first = selected;
            if (selected >= first + PageSize()) first = selected - PageSize() + 1;
        }
        first = std::clamp(first, 0, std::max(0, static_cast<int>(rows.size()) - PageSize()));
    }
    void Cancel() {
        ++generation;
        filenames.CancelSession(kSession);
        contents.Suspend();
        busy = false;
        if (hwnd) { KillTimer(hwnd, kDebounce); KillTimer(hwnd, kConnectTimeout); }
    }
    void Hide() {
        Cancel();
        HideScopeTooltip();
        handoff_hover = false;
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
    }
    void Changed() {
        Cancel();
        const int count = GetWindowTextLengthW(edit);
        query.resize(static_cast<size_t>(count) + 1);
        GetWindowTextW(edit, query.data(), count + 1);
        query.resize(static_cast<size_t>(count));
        rows.clear(); selected = first = 0; total = 0; error.clear(); truncated = false;
        local_result = {}; network_result = {}; providers = {};
        if (!query.empty() && !composing) { busy = true; SetTimer(hwnd, kDebounce, 180, nullptr); }
        Invalidate();
    }
    void Search() {
        KillTimer(hwnd, kDebounce);
        if (!IsWindowVisible(hwnd) || composing || query.empty()) return;
        if (content_mode) {
            index::ContentSearchRequest request;
            request.session_id = kSession; request.generation = generation;
            request.needle = query; request.maximum_hits = kMaximumResults;
            request.indexed = true; request.task_scan = true;
            request.skip_system_locations = true;
            const auto config = contents.GetConfig();
            request.maximum_file_bytes = config.maximum_file_bytes;
            request.maximum_document_bytes = config.maximum_document_bytes;
            if (current_only && !current_folder.empty()) request.root = current_folder;
            contents.SearchAsync(std::move(request));
        } else {
            index::Query request;
            request.session_id = kSession; request.needle = query; request.limit = kMaximumResults;
            if (!search_pinyin) request.needle = L"nopinyin: " + request.needle;
            if (current_only) request.path_prefix = current_folder;
            filenames.SearchAsync(request, generation);
            providers.ExpectNetwork(!network.Roots().empty());
            network.SearchAsync(request, generation);
            SetTimer(hwnd, kConnectTimeout, 10000, nullptr);
        }
    }
    void MergeFilenames() {
        const auto position = app::GlobalSearchPosition::Capture(rows, selected, first);
        rows.clear();
        index::Query request;
        request.needle = search_pinyin ? query : L"nopinyin: " + query;
        request.limit = kMaximumResults;
        const auto merged = index::MergeSearchResults(request, local_result, network_result, true);
        for (const auto& hit : merged.hits) rows.push_back({hit.name, hit.path, {}, hit.is_dir});
        total = merged.total;
        truncated = total > rows.size(); busy = providers.Busy();
        error = providers.Error() ? Text(rows.empty() ? StringId::GlobalSearchFailed : StringId::SearchIncomplete) : std::wstring{};
        if (!busy) KillTimer(hwnd, kConnectTimeout);
        const int restored = position.Selection(rows);
        selected = restored; first = position.First(rows);
        ClampSelection(false);
        if (restored < 0) selected = -1;
        Invalidate();
    }
    void TimeoutFilenames() {
        providers.Timeout();
        MergeFilenames();
    }
    void AcceptFilenames(bool from_network, uint32_t id, index::SearchResult result) {
        if (id != generation || content_mode) return;
        if (from_network && result.error && !providers.network_expected) return;
        providers.Accept(from_network, result.error);
        if (from_network) network_result = std::move(result);
        else local_result = std::move(result);
        MergeFilenames();
    }
    void Open(bool location, void (*open_result)(std::wstring, bool) = OpenResult) {
        if (rows.empty() || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        auto path = rows[static_cast<size_t>(selected)].path;
        Hide(); open_result(std::move(path), location);
    }
    // Leading/trailing blanks would only make the main window's query differ
    // from what the popup searched for.
    GlobalSearchHandoff MakeHandoff() const {
        GlobalSearchHandoff out;
        const auto begin = query.find_first_not_of(L" \t");
        if (begin != std::wstring::npos) out.query = query.substr(begin, query.find_last_not_of(L" \t") - begin + 1);
        out.content = content_mode;
        if (current_only && !current_folder.empty()) out.folder = current_folder;
        return out;
    }
    void Handoff() {
        if (!handoff) return;
        const auto request = MakeHandoff();
        const auto handler = handoff; // the handler may replace handoff
        Hide();
        handler(request);
    }
    bool InHandoff(float x, float y) const {
        return handoff && handoff_rect.right > handoff_rect.left &&
            x >= handoff_rect.left && x < handoff_rect.right && y >= handoff_rect.top && y < handoff_rect.bottom;
    }
    void HideScopeTooltip() {
        if (hwnd) KillTimer(hwnd, kScopeTooltipTimer);
        const bool redraw = scope_tooltip_visible;
        scope_hover = false;
        scope_tooltip_visible = false;
        if (redraw) Invalidate();
    }
    void UpdateScopeHover(float x, float y) {
        const bool hovered = current_only && !current_folder_tip.empty() &&
            x >= width - 126 && x < width - 12 && y >= Header() && y < 129 + grow;
        scope_pointer_x = x;
        scope_pointer_y = y;
        if (hovered == scope_hover) {
            if (hovered && scope_tooltip_visible) Invalidate();
            return;
        }
        HideScopeTooltip();
        scope_hover = hovered;
        if (scope_hover) SetTimer(hwnd, kScopeTooltipTimer, 400, nullptr);
    }
    bool Key(WPARAM key) {
        if (composing) return false;
        if (key == VK_ESCAPE) { Hide(); return true; }
        if (key == VK_RETURN) {
            if (GetKeyState(VK_SHIFT) & 0x8000) Handoff();
            else Open((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            return true;
        }
        if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
            if (selected < 0) selected = key == VK_UP || key == VK_PRIOR ? static_cast<int>(rows.size()) - 1 : 0;
            else selected = std::max(0, selected + (key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -PageSize() : PageSize()));
            ClampSelection(); Invalidate(); return true;
        }
        if (key == VK_TAB) { content_mode = !content_mode; Changed(); return true; }
        return false;
    }
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_IME_STARTCOMPOSITION) { self->composing = true; self->Cancel(); }
        if (message == WM_IME_ENDCOMPOSITION) {
            self->composing = false;
            const auto result = self->DefaultEdit(window, message, wp, lp);
            self->Changed(); return result;
        }
        if (message == WM_KEYDOWN && self->Key(wp)) return 0;
        if (message == WM_CHAR && !self->composing && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
        LRESULT result = 0;
        if (ui::HandleChildEditMessage(self->compositor, self->edit_format.Get(), self->Theme().text,
            self->EditBackground(), self->background, window, message, wp, lp, result)) return result;
        return self->DefaultEdit(window, message, wp, lp);
    }
    LRESULT DefaultEdit(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        return ui::DefPresentedChildEditProc(compositor, edit_format.Get(), Theme().text,
            EditBackground(), window, message, wp, lp);
    }
    void Layout() {
        RECT rect{}; GetClientRect(hwnd, &rect);
        width = static_cast<float>(rect.right) / scale; height = static_cast<float>(rect.bottom) / scale;
        if (target) {
            if (target.Get() == compositor.Dc()) compositor.Resize(rect.right, rect.bottom);
            target->SetDpi(96 * scale, 96 * scale);
            ComPtr<ID2D1HwndRenderTarget> window_target;
            if (SUCCEEDED(target.As(&window_target))) window_target->Resize(D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
        }
        // The query line follows the interface font size like every other
        // hosted edit (UI family, 23 DIP x font size x DPI).
        if (HFONT font = ui::typography::CreateEditFont(scale, 23.0f)) {
            if (edit_font) DeleteObject(edit_font);
            edit_font = font;
        }
        if (EnsureTarget()) {
            edit_format.Reset();
            write_factory->CreateTextFormat(ui::typography::PreferredTextFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                static_cast<float>(ui::typography::EditFontPixels(scale, 23.0f)), l10n::LocaleName(), &edit_format);
        }
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
        // One text line plus 4 DIP, never below the original 35 DIP field.
        const float line_dip = static_cast<float>(ui::EditLineHeight(edit, edit_font, 1 << 16)) / scale;
        grow = std::max(0.0f, std::ceil(line_dip + 4.0f - 35.0f));
        const float field_h = 35 + grow;
        const float edit_left = kEditLeft + kEditTextInset;
        const float edit_width = width - kEditRight - edit_left;
        MoveWindow(edit, Px(edit_left), Px(23), Px(edit_width), Px(field_h), TRUE);
        // Clip the redirected child as well as its parent-painted background.
        const HRGN edit_region = CreateRoundRectRgn(0, 0, Px(edit_width) + 1, Px(field_h) + 1,
            Px(kEditCornerRadius * 2), Px(kEditCornerRadius * 2));
        if (edit_region && !SetWindowRgn(edit, edit_region, TRUE)) DeleteObject(edit_region);
        if (edit_format)
            ui::PresentChildEdit(compositor, edit_format.Get(), Theme().text, EditBackground(), edit);
        ClampSelection(); Invalidate();
    }
    bool EnsureTarget() {
        if (target) return true;
        if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return false;
        if (!write_factory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_factory.GetAddressOf())))) return false;
        RECT rect{}; GetClientRect(hwnd, &rect);
        if (compositor.NeedsRecovery()) {
            if (!compositor.Recover()) return false;
            material.Invalidate();
        } else if (!compositor.Dc() && !compositor.Init(hwnd)) return false;
        material.SetCompositor(&compositor);
        target = compositor.Dc();
        target->SetDpi(96 * scale, 96 * scale);
        painter.SetCompositor(&compositor);
        // This surface uses logical DIPs through its render-target DPI.
        painter.SetScale(1.0f);
        return SUCCEEDED(target->CreateSolidColorBrush(Color(0xffffff), &brush));
    }
    void Fill(D2D1_RECT_F rect, UINT32 color, float radius = 0) {
        brush->SetColor(Color(color));
        if (radius) target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        else target->FillRectangle(rect, brush.Get());
    }
    void Label(const std::wstring& text, D2D1_RECT_F rect, float size, UINT32 color, bool bold = false, const wchar_t* face = L"Segoe UI", bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        ComPtr<IDWriteTextFormat> format;
        // Glyph icons keep their slots; text follows Settings > Interface font size.
        const bool icon_face = std::wstring_view(face).find(L"Icons") != std::wstring_view::npos;
        const float text_size = icon_face ? size : size * ui::typography::UiFontScale();
        if (FAILED(write_factory->CreateTextFormat(face, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, text_size, l10n::LocaleName(), &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(write_factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) format->SetTrimming(&trim, ellipsis.Get());
        brush->SetColor(Color(color));
        ComPtr<IDWriteTextLayout> layout;
        if (highlight && !query.empty() && SUCCEEDED(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
            std::max(0.0f, rect.right - rect.left), std::max(0.0f, rect.bottom - rect.top), &layout))) {
            ComPtr<ID2D1SolidColorBrush> accent;
            if (SUCCEEDED(target->CreateSolidColorBrush(Theme().accent, &accent))) {
                for (size_t start = 0; start + query.size() <= text.size(); ++start) {
                    if (CompareStringOrdinal(text.data() + start, static_cast<int>(query.size()), query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL)
                        layout->SetDrawingEffect(accent.Get(), {static_cast<UINT32>(start), static_cast<UINT32>(query.size())});
                }
            }
            target->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format.Get(), rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void DrawLogo() {
        if (logo_target != target.Get()) { logo.Reset(); logo_target = target.Get(); }
        if (!logo) {
            const auto module = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_GLOBAL_SEARCH_LOGO), RT_RCDATA);
            const auto data = resource ? LoadResource(module, resource) : nullptr;
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (data && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(module, resource))) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &logo);
        }
        if (logo) target->DrawBitmap(logo.Get(), {27, 20 + grow / 2, 65, 58 + grow / 2});
    }
    float TextWidth(const std::wstring& text, float size = 12, bool bold = false) {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(write_factory->CreateTextFormat(L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * ui::typography::UiFontScale(),
            l10n::LocaleName(), &format)) &&
            SUCCEEDED(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), 2000, 40, &layout)))
            layout->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }
    void Keycap(const std::wstring& text, D2D1_RECT_F rect, UINT32 color) {
        brush->SetColor(Theme().fill_input);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, 4, 4), brush.Get());
        Label(text, rect, 12, color, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Returns the left edge of what was drawn. With handoff_only (the result
    // list is truncated) only the "continue in Pulse" hint is shown.
    float DrawKeys(float bottom, UINT32 color, UINT32 accent, bool handoff_only) {
        const std::wstring keys[] = {L"↑", L"↓", L"Enter", L"Ctrl + Enter", L"Shift + Enter"};
        const std::wstring actions[] = {L"", Text(StringId::GlobalSearchSelect), Text(StringId::Open),
            Text(StringId::OpenLocation), Text(StringId::GlobalSearchHandoff)};
        constexpr int kHandoffKey = 4;
        const int last = handoff ? kHandoffKey : kHandoffKey - 1;
        const int stop = handoff_only ? kHandoffKey : 0;
        handoff_rect = {};
        float right = width - 22;
        for (int i = last; i >= stop; --i) {
            const float group_right = right;
            const UINT32 tint = i == kHandoffKey && handoff_hover ? accent : color;
            if (!actions[i].empty()) {
                const float label_width = TextWidth(actions[i]);
                Label(actions[i], {right - label_width, bottom + 10, right, bottom + 38}, 12, tint);
                right -= label_width + 6;
            }
            const float key_width = std::max(24.0f, TextWidth(keys[i]) + 14);
            Keycap(keys[i], {right - key_width, bottom + 12, right, bottom + 36}, tint);
            if (i == kHandoffKey) handoff_rect = {right - key_width - 4, bottom + 6, group_right + 4, height - 6};
            right -= key_width + (i == 1 ? 4 : 16);
        }
        return right;
    }
    void DrawScopeTooltip(const ui::Theme& theme) {
        if (!scope_tooltip_visible || !current_only || current_folder_tip.empty()) return;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(ui::typography::CreateTextFormat(compositor.DwriteFactory(),
            {ui::typography::FontRole::Text, 12 * scale}, &format))) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(write_factory->CreateTextLayout(current_folder_tip.data(),
            static_cast<UINT32>(current_folder_tip.size()), format.Get(),
            std::max(1.0f, (width - 32) * scale), 10000, &layout))) return;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return;
        const float tooltip_width = std::min(
            ui::typography::MeasureLine(&compositor, format.Get(), current_folder_tip) + 16 * scale,
            (width - 16) * scale);
        const float tooltip_height = std::ceil(metrics.height) + 16 * scale;
        const float x = std::round(std::clamp((scope_pointer_x + 12) * scale,
            8 * scale, width * scale - tooltip_width - 8 * scale));
        const float y = std::round(std::clamp((scope_pointer_y + 18) * scale,
            8 * scale, std::max(8 * scale, height * scale - tooltip_height - 8 * scale)));
        const D2D1_RECT_F bounds{x, y, x + tooltip_width, y + tooltip_height};
        // Painter/LumaText rasterize at physical pixel sizes on a 96-DPI target.
        // Restore the floating window's DIP coordinate system after the tooltip.
        target->SetDpi(96, 96);
        painter.SetScale(scale);
        if (painter.BeginFrame(theme, ui::IsHighContrast())) {
            painter.DrawTooltip(bounds, metrics.lineCount > 1 ? std::wstring_view{} : current_folder_tip);
            if (metrics.lineCount > 1) {
                brush->SetColor(theme.text);
                target->DrawTextLayout({x + 8 * scale, y + 8 * scale}, layout.Get(), brush.Get());
            }
        }
        target->SetDpi(96 * scale, 96 * scale);
    }
    std::wstring FooterStatus() const {
        if (!error.empty()) return rows.empty() ? L"" : error;
        return busy ? Text(StringId::GlobalSearchLoading)
                    : std::to_wstring(total) + Text(StringId::GlobalSearchResults);
    }

    void Paint() {
        PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
        if (EnsureTarget()) {
            const auto theme = Theme();
            const UINT32 fg = Rgb(theme.text), muted = dark ? 0xa8abb2 : 0x676a70;
            const UINT32 line = dark ? 0x3b3d41 : 0xe2e3e7, accent = Rgb(theme.accent);
            target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.0f));
            const bool live = target.Get() == compositor.Dc();
            const bool image_mode = !ui::IsHighContrast() && !background_image.empty() && effect == ui::WindowEffect::None;
            bool drawn = false;
            if (live && !ui::IsHighContrast()) {
                const D2D1_RECT_F bounds{0, 0, width, height};
                drawn = image_mode ? material.DrawSourceCover(compositor.Dc(), bounds, background_image) :
                    material.DrawBackdrop(compositor.Dc(), bounds, effect, dark, background_image);
            }
            auto tint = theme.bg;
            if (image_mode) tint.a = drawn ? (dark ? 0.55f : 0.60f) : 1.0f;
            else if (drawn) tint.a = 0;
            else if (live && !ui::IsHighContrast() && backdrop && compositor.UsesTransparentComposition()) tint.a = dark ? 0.72f : 0.78f;
            brush->SetColor(tint); target->FillRectangle({0, 0, width, height}, brush.Get());
            Fill({kEditLeft, 23, width - kEditRight, 58 + grow}, Rgb(theme.header_bg), kEditCornerRadius);
            const float mid = grow / 2;   // header items stay centred on the taller field
            DrawLogo();
            Fill({80, 24 + mid, 81, 54 + mid}, line);
            Label(L"\xE721", {91, 22 + mid, 115, 57 + mid}, 21, muted, false, L"Segoe Fluent Icons");
            Keycap(L"Esc", {width - 60, 23 + mid, width - 22, 53 + mid}, muted);
            Fill({0, Header(), width, Header() + 1}, line);
            Label(Text(StringId::SearchModeName), {28, 83 + grow, 110, 120 + grow}, 16, content_mode ? muted : fg, !content_mode);
            Label(Text(StringId::SearchModeContent), {124, 83 + grow, 208, 120 + grow}, 16, content_mode ? fg : muted, content_mode);
            const float tab_center = (content_mode ? 124.0f : 28.0f) +
                TextWidth(Text(content_mode ? StringId::SearchModeContent : StringId::SearchModeName), 16, true) * 0.5f;
            Fill({tab_center - 12, 124 + grow, tab_center + 12, 127 + grow}, accent, 1.5f);
            const UINT32 disabled = dark ? 0x6f7279 : 0x9a9da3;
            Label(Text(StringId::GlobalSearchEverywhere), {width - 230, 83 + grow, width - 130, 120 + grow}, 15,
                current_only ? muted : fg, !current_only, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
            Label(Text(StringId::GlobalSearchCurrentFolder), {width - 122, 83 + grow, width - 22, 120 + grow}, 15,
                current_folder.empty() ? disabled : current_only ? fg : muted,
                current_only && !current_folder.empty(), L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
            const float scope_center = current_only ? width - 72 : width - 180;
            Fill({scope_center - 12, 124 + grow, scope_center + 12, 127 + grow}, accent, 1.5f);
            Fill({0, 128 + grow, width, 129 + grow}, line);
            const float bottom = height - kFooter;
            target->PushAxisAlignedClip({0, 130 + grow, width, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (rows.empty()) {
                const auto& status = !error.empty() ? error : busy ? Text(StringId::GlobalSearchLoading) : query.empty() ? Text(StringId::GlobalSearchEmpty) : Text(StringId::GlobalSearchNoResults);
                const D2D1_RECT_F bounds{16, ListTop(), width - 16, bottom - 8};
                if (!busy && error.empty()) {
                    const auto layout = ui::MakePaneEmptyLayout(bounds, 1, false);
                    if (empty_art_target != target.Get() || empty_art_scale != scale) {
                        empty_art.Reset(); empty_art_target = target.Get(); empty_art_scale = scale;
                        ui::CreateSvgResourceBitmap(target.Get(), IDR_EMPTY_FOLDER_SVG,
                            D2D1::SizeU(static_cast<UINT32>(Px(280)), static_cast<UINT32>(Px(280 * 360.0f / 512))), D2D1::SizeF(512, 360), &empty_art);
                    }
                    if (empty_art) target->DrawBitmap(empty_art.Get(), layout.art, dark ? 1.0f : 0.68f);
                    Label(status, layout.title, 16, muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    Label(status, bounds, 16, muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
            for (int i = first; i < static_cast<int>(rows.size()) && i < first + PageSize() + 1; ++i) {
                const float y = ListTop() + static_cast<float>(i - first) * kRow;
                const auto& row = rows[static_cast<size_t>(i)];
                if (i == selected) { Fill({5, y, width - 5, y + kRow - 3}, dark ? 0x293947 : 0xe1effa, 6); Fill({5, y + 9, 8, y + kRow - 12}, accent, 2); }
                const auto dot = row.name.find_last_of(L'.');
                std::wstring extension = dot == std::wstring::npos ? L"" : row.name.substr(dot);
                for (auto& letter : extension) letter = static_cast<wchar_t>(towlower(letter));
                const bool excel = extension == L".xlsx" || extension == L".xls" || extension == L".csv";
                const bool word = extension == L".docx" || extension == L".doc";
                const bool pdf = extension == L".pdf", powerpoint = extension == L".pptx" || extension == L".ppt";
                const UINT32 icon_color = row.directory ? 0xd59b27 : excel ? 0x178753 : pdf ? 0xc4413b : powerpoint ? 0xc55a39 : 0x367cb5;
                Fill({29, y + 23, 58, y + 57}, icon_color, 4);
                if (excel || word || pdf || powerpoint) Label(excel ? L"X" : word ? L"W" : pdf ? L"PDF" : L"P", {33, y + 27, 58, y + 54}, pdf ? 11.0f : 19.0f, 0xffffff, true);
                else Label(row.directory ? L"\xE8B7" : L"\xE8A5", {34, y + 27, 58, y + 54}, 19, 0xffffff, false, L"Segoe Fluent Icons");
                Label(row.name, {78, y + 8, width - 30, y + 35}, 17, fg, true, L"Segoe UI", true);
                Label(ParentPath(row.path), {78, y + 34, width - 30, y + 57}, 14, muted);
                if (content_mode) Label(row.snippet, {78, y + 56, width - 30, y + 80}, 14, muted, false, L"Segoe UI", true);
            }
            target->PopAxisAlignedClip();
            if (rows.size() > static_cast<size_t>(PageSize())) {
                const float track = bottom - 142 - grow, thumb = std::max(20.0f, track * static_cast<float>(PageSize()) / static_cast<float>(rows.size()));
                const float top = 138 + grow + (track - thumb) * static_cast<float>(first) / static_cast<float>(rows.size() - static_cast<size_t>(PageSize()));
                Fill({width - 4, top, width - 1, top + thumb}, muted, 1.5f);
            }
            Fill({0, bottom, width, bottom + 1}, line);
            Label(FooterStatus(), {25, bottom + 8, 155, height - 8}, 12, muted);
            if (truncated && handoff) {
                // Too many hits for the popup: point at the main window, which pages through all of them.
                const float keys_left = DrawKeys(bottom, muted, accent, true);
                Label(Text(StringId::GlobalSearchTruncatedShort), {165, bottom + 8, std::max(165.0f, keys_left), height - 8}, 12, muted);
            } else if (truncated) {
                handoff_rect = {};
                Label(Text(StringId::GlobalSearchTruncated), {165, bottom + 8, width - 22, height - 8}, 12, muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else DrawKeys(bottom, muted, accent, false);
            DrawScopeTooltip(theme);
            const HRESULT rendered = target->EndDraw();
            if (FAILED(rendered)) { compositor.NotifyDeviceLost(rendered); empty_art.Reset(); empty_art_target = nullptr; logo.Reset(); logo_target = nullptr; brush.Reset(); target.Reset(); }
            else if (live) compositor.Present();
        }
        EndPaint(hwnd, &paint);
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyAppearance(); return 0;
        case WM_SIZE: if (edit) Layout(); return 0;
        case WM_DPICHANGED: {
            scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            const auto* rect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(); return 0;
        }
        case WM_CLOSE: Hide(); return 0;
        case WM_ACTIVATEAPP: if (!wp) Hide(); return 0;
        case WM_SETFOCUS: SetFocus(edit); return 0;
        case WM_KEYDOWN: if (Key(wp)) return 0; break;
        case WM_COMMAND: if (reinterpret_cast<HWND>(lp) == edit && HIWORD(wp) == EN_CHANGE) Changed(); return 0;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp); SetBkColor(dc, Background());
            SetTextColor(dc, ui::HcEditText(dark ? RGB(245, 245, 247) : RGB(32, 33, 36)));
            return reinterpret_cast<LRESULT>(ui::EditBackBrush(background));
        }
        case WM_TIMER:
            if (wp == kDebounce) Search();
            else if (wp == kConnectTimeout) {
                KillTimer(hwnd, kConnectTimeout);
                if (busy && !content_mode) TimeoutFilenames();
            } else if (wp == kScopeTooltipTimer) {
                KillTimer(hwnd, kScopeTooltipTimer);
                if (scope_hover && current_only && !current_folder_tip.empty()) {
                    scope_tooltip_visible = true;
                    Invalidate();
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_mouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                tracking_mouse = TrackMouseEvent(&tracking) != FALSE;
            }
            const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            UpdateScopeHover(x, y);
            if (const bool hover = InHandoff(x, y); hover != handoff_hover) { handoff_hover = hover; Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse = false;
            HideScopeTooltip();
            if (handoff_hover) { handoff_hover = false; Invalidate(); }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && handoff_hover) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            HideScopeTooltip();
            if (y < Header() && x > width - 70) Hide();
            else if (y >= Header() && y < 129 + grow) {
                if (x < 220) { content_mode = x >= 114; Changed(); }
                else if (x >= width - 230 && x < width - 126) {
                    if (current_only) { current_only = false; Changed(); }
                } else if (x >= width - 126 && x < width - 12 && !current_folder.empty()) {
                    if (!current_only) { current_only = true; Changed(); }
                }
            } else if (y >= ListTop() && y < height - kFooter) {
                selected = first + static_cast<int>((y - ListTop()) / kRow); ClampSelection(); Invalidate();
            } else if (InHandoff(x, y)) {
                handoff_hover = false;
                Handoff();
                return 0;
            }
            if (IsWindowVisible(hwnd)) SetFocus(edit); return 0;
        }
        case WM_LBUTTONDBLCLK: if (GET_Y_LPARAM(lp) >= Px(ListTop()) && GET_Y_LPARAM(lp) < Px(height - kFooter)) Open(false); return 0;
        case WM_MOUSEWHEEL:
            first = std::clamp(first - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3, 0, std::max(0, static_cast<int>(rows.size()) - PageSize()));
            Invalidate(); return 0;
        case kIndexStatus: Invalidate(); return 0;
        case kIndexResult: {
            index::SearchResult result;
            if (filenames.TakeResult(static_cast<uint32_t>(wp), result) && IsWindowVisible(hwnd)) {
                AcceptFilenames(false, static_cast<uint32_t>(wp), std::move(result));
            }
            return 0;
        }
        case kNetworkResult: {
            index::SearchResult result;
            if (network.TakeResult(static_cast<uint32_t>(wp), result) && IsWindowVisible(hwnd)) {
                AcceptFilenames(true, static_cast<uint32_t>(wp), std::move(result));
            }
            return 0;
        }
        case kContentResult: {
            index::ContentSearchUpdate update;
            while (contents.TakeUpdate(update)) {
                if (update.progress.generation != generation || !content_mode || !IsWindowVisible(hwnd)) continue;
                const bool had_rows = !rows.empty();
                for (auto& hit : update.hits) {
                    if (rows.size() >= kMaximumResults) break;
                    rows.push_back({std::move(hit.name), std::move(hit.path), std::move(hit.snippet), false});
                }
                total = rows.size(); truncated = update.progress.truncated;
                busy = !update.progress.done;
                if (update.progress.error && update.progress.error != ERROR_CANCELLED) error = Text(StringId::GlobalSearchFailed);
                if (!had_rows && !rows.empty()) selected = 0;
                ClampSelection(false); Invalidate();
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    bool Show(HWND owner, bool use_dark, float requested_scale, const std::wstring& folder, bool pinyin) {
        dark = use_dark; scale = requested_scale > 0 ? requested_scale : 1;
        search_pinyin = pinyin;
        const HWND foreground = GetForegroundWindow();
        POINT cursor{}; GetCursorPos(&cursor);
        const HMONITOR active_monitor = foreground && foreground != hwnd ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) :
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (const HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            const auto dpi = reinterpret_cast<MonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
            UINT x = 96, y = 96;
            if (dpi && SUCCEEDED(dpi(active_monitor, 0, &x, &y))) scale = static_cast<float>(x) / 96.0f;
            FreeLibrary(shcore);
        }
        (void)owner;
        current_folder = folder;
        current_folder_tip = path::StripExtendedPathPrefix(folder);
        if (folder.empty()) current_only = false;
        HideScopeTooltip();
        if (!hwnd) {
            WNDCLASSEXW cls{sizeof(cls)}; cls.style = CS_DBLCLKS;
            cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Pulse.GlobalSearch";
            RegisterClassExW(&cls);
            // A hidden owner would hide/minimize this window too; the owner only chooses the monitor.
            if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"Pulse Search", WS_POPUP | WS_CLIPCHILDREN,
                0, 0, Px(780), Px(488), nullptr, nullptr, cls.hInstance, this)) return false;
            edit = ui::CreateChildEdit(hwnd);
            if (!edit) { DestroyWindow(hwnd); hwnd = nullptr; return false; }
            SetWindowTheme(edit, L"", L"");
            EnsureTarget();
            ShowWindow(edit, SW_SHOW);
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(edit, EM_SETLIMITTEXT, 2048, 0);
            filenames.Start(hwnd, kIndexStatus, kIndexResult);
            network.Start(hwnd, kIndexStatus, kNetworkResult);
            contents.Start(hwnd, kContentResult, index::ContentAgentMode::Instant);
        }
        ApplyAppearance();
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(StringId::GlobalSearchPlaceholder).c_str()));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(active_monitor, &monitor);
        const auto& work = monitor.rcWork;
        const int w = std::min(Px(780), static_cast<int>(work.right - work.left) - Px(32));
        const int h = std::min(Px(488), static_cast<int>(work.bottom - work.top) - Px(32));
        SetWindowPos(hwnd, HWND_TOPMOST, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 3, w, h, SWP_SHOWWINDOW);
        Layout(); SetForegroundWindow(hwnd); SetFocus(edit); SendMessageW(edit, EM_SETSEL, 0, -1);
        Changed(); return true;
    }
    ~Impl() {
        Cancel(); filenames.Stop(); network.Stop(); contents.Stop();
        if (edit) RemoveWindowSubclass(edit, EditProc, 1);
        if (hwnd) DestroyWindow(hwnd);
        if (edit_font) DeleteObject(edit_font);
        if (background) DeleteObject(background);
    }
};
GlobalSearchWindow::GlobalSearchWindow() = default;
GlobalSearchWindow::~GlobalSearchWindow() = default;
bool GlobalSearchWindow::Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Show(owner, dark, scale, current_folder, search_pinyin);
}
void GlobalSearchWindow::SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->dark = dark; impl_->effect = effect; impl_->background_image = background_image; impl_->accent_color = accent;
    impl_->ApplyAppearance();
}
void GlobalSearchWindow::SetHandoffHandler(GlobalSearchHandoffHandler handler) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->handoff = std::move(handler);
}
void GlobalSearchWindow::Hide() { if (impl_) impl_->Hide(); }
void GlobalSearchWindow::Shutdown() { impl_.reset(); }
bool GlobalSearchWindow::Visible() const { return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd); }
}
