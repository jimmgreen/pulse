#include <atomic>
#include "extension_panel.h"
#include "fluent_components.h"
#include "typography.h"
#include "window_helpers.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse::ui {
namespace {
constexpr wchar_t kWindowClass[] = L"Pulse.ExtensionPanel";
constexpr int kNone = -1;
constexpr int kClose = -2;
constexpr UINT_PTR kFieldsTimer = 0x455046;
}
D2D1_COLOR_F ExtensionPanelTitleColor(ExtensionPanelTitleTone tone, bool high_contrast,
    D2D1_COLOR_F accent, D2D1_COLOR_F background, D2D1_COLOR_F text) {
    if (tone != ExtensionPanelTitleTone::Accent || high_contrast) return text;
    const auto linear = [](float value) {
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    const auto luminance = [&](D2D1_COLOR_F color) {
        return 0.2126f * linear(color.r) + 0.7152f * linear(color.g) + 0.0722f * linear(color.b);
    };
    const float alpha = std::clamp(accent.a, 0.0f, 1.0f);
    const D2D1_COLOR_F visible{accent.r * alpha + background.r * (1 - alpha),
        accent.g * alpha + background.g * (1 - alpha), accent.b * alpha + background.b * (1 - alpha), 1};
    const float foreground_luminance = luminance(visible);
    const float background_luminance = luminance(background);
    const float contrast = ((std::max)(foreground_luminance, background_luminance) + 0.05f) /
        ((std::min)(foreground_luminance, background_luminance) + 0.05f);
    return contrast >= 4.5f ? accent : text;
}

namespace { std::atomic<uint64_t> next_panel_instance{0}; }

struct ExtensionPanel::Impl {
    uint64_t instance = 0;
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    ExtensionPanelSpec spec;
    ExtensionTreeView tree;
    Compositor compositor;
    fluent::Painter painter{&compositor};
    ComPtr<IDWriteTextFormat> title_format;
    ComPtr<IDWriteTextFormat> body_format;
    ComPtr<IDWriteTextFormat> field_label_format;
    bool dark = false;
    bool docked = false;
    D2D1_COLOR_F accent{};
    float scale = 1.0f;
    float scroll = 0.0f;
    float description_height = 0.0f;
    struct GroupBounds { D2D1_RECT_F title{}, description{}; };
    std::vector<GroupBounds> group_bounds;
    std::vector<D2D1_RECT_F> button_bounds;
    std::vector<size_t> button_order;
    float content_bottom = 0;
    int hover = kNone;
    int pressed = kNone;
    int focus = 0;
    bool keyboard_focus = true;
    unsigned long long generation = 0;

    ~Impl() { Close(); }
    void Close() { if (hwnd) DestroyWindow(hwnd); }
    float Width() const { return static_cast<float>(compositor.Width()); }
    float Height() const { return static_cast<float>(compositor.Height()); }
    bool Compact() const { return spec.density == ExtensionPanelDensity::Compact; }
    float RowPitch() const { return (Compact() ? 40.0f : 48.0f) * scale; }
    float FieldPitch() const { return (Compact() ? 68.0f : 76.0f) * scale; }
    float BodyGap() const { return (Compact() ? 12.0f : 16.0f) * scale; }
    float DescriptionHeight() const { return description_height; }
    float BodyTop() const { return (spec.content_only ? 12.0f : Compact() ? 76.0f : 88.0f) * scale; }
    float FieldsTop() const { return BodyTop() + DescriptionHeight() + (spec.description.empty() ? 0.0f : BodyGap()); }
    float FieldsHeight() const { return static_cast<float>(spec.fields.size()) * FieldPitch(); }
    float ButtonsTop() const { return FieldsTop() + FieldsHeight(); }
    float ContentBottom() const { return content_bottom; }
    RECT TreeBounds() const {
        return {static_cast<LONG>(24 * scale), static_cast<LONG>(FieldsTop()),
            static_cast<LONG>((std::max)(25 * scale, Width() - 24 * scale)),
            static_cast<LONG>((std::max)(FieldsTop() + scale, Height() - 16 * scale))};
    }
    void LayoutTree() {
        if (!spec.tree || !tree.Hwnd()) return;
        tree.SetTheme(dark, accent, scale);
        tree.SetBounds(TreeBounds());
    }
    float MaxScroll() const {
        // The tree owns its viewports; a second panel scrollbar has no content to move.
        return spec.tree ? 0.0f : (std::max)(0.0f, ContentBottom() - Height());
    }
    D2D1_RECT_F CloseRect() const { return spec.content_only ? D2D1_RECT_F{} : D2D1_RECT_F{Width() - 44.0f * scale, 0, Width(), 36.0f * scale}; }
    D2D1_RECT_F ButtonRect(size_t index) const {
        if (index >= button_bounds.size()) return {};
        auto rect = button_bounds[index];
        rect.top -= scroll;
        rect.bottom -= scroll;
        return rect;
    }
    void MeasureGroups(float width) {
        button_bounds.assign(spec.buttons.size(), {});
        button_order.clear();
        group_bounds.clear();
        if (spec.tree) {
            content_bottom = FieldsTop() + 480 * scale;
            return;
        }
        float top = ButtonsTop();
        const float text_width = (std::max)(1.0f, width - 48.0f * scale);
        const auto append_buttons = [&](const std::vector<size_t>& indices) {
            const size_t columns = static_cast<size_t>(ExtensionPanelColumns(spec.layout, indices.size(), width / scale));
            const float button_width = (text_width - static_cast<float>(columns - 1) * 12.0f * scale) / static_cast<float>(columns);
            for (size_t position = 0; position < indices.size(); ++position) {
                const size_t index = indices[position];
                const float left = 24.0f * scale + static_cast<float>(position % columns) * (button_width + 12.0f * scale);
                const float y = top + static_cast<float>(position / columns) * RowPitch();
                button_bounds[index] = {left, y, left + button_width, y + (Compact() ? 32.0f : 36.0f) * scale};
                button_order.push_back(index);
            }
            top += static_cast<float>((indices.size() + columns - 1) / columns) * RowPitch();
        };
        if (spec.groups.empty()) {
            std::vector<size_t> indices;
            for (size_t i = 0; i < spec.buttons.size(); ++i) indices.push_back(i);
            append_buttons(indices);
        } else for (const auto& group : spec.groups) {
            if (!group_bounds.empty()) top += BodyGap();
            const float title_measure = typography::MeasureWrapped(compositor.DwriteFactory(), body_format.get(), group.title, text_width);
            const float title_height = std::clamp(std::ceil(title_measure / (22.0f * scale)) * 22.0f * scale, 22.0f * scale, 44.0f * scale);
            GroupBounds bounds;
            bounds.title = {24.0f * scale, top, width - 24.0f * scale, top + title_height};
            top += title_height;
            if (!group.description.empty()) {
                const float measured = typography::MeasureWrapped(compositor.DwriteFactory(), field_label_format.get(), group.description, text_width);
                const float height = std::clamp(std::ceil(measured / (16.0f * scale)) * 16.0f * scale, 16.0f * scale, 48.0f * scale);
                bounds.description = {24.0f * scale, top, width - 24.0f * scale, top + height};
                top += height;
            }
            top += 8.0f * scale;
            group_bounds.push_back(bounds);
            append_buttons(group.buttons);
        }
        content_bottom = top + 16.0f * scale;
    }
    std::vector<bool> enabled;
    bool ButtonEnabled(size_t index) const {
        return index < spec.buttons.size() && spec.on_command &&
            (!spec.read_enabled || (index < enabled.size() && enabled[index]));
    }
    bool RefreshEnabled() {
        if (!hwnd || !IsWindow(hwnd) || !owner || !IsWindow(owner) || !spec.read_enabled) return false;
        const HWND reading_window = hwnd;
        const auto reading_generation = generation;
        const auto read = spec.read_enabled;
        std::vector<bool> values;
        try { values = read(); } catch (...) {}
        if (hwnd != reading_window || generation != reading_generation || !IsWindow(reading_window)) return false;
        const bool complete = values.size() == spec.buttons.size();
        if (!complete) values.assign(spec.buttons.size(), false);
        if (enabled != values) { enabled = std::move(values); InvalidateRect(hwnd, nullptr, FALSE); }
        return complete;
    }
    bool Invoke(size_t index) {
        const auto invoking_generation = generation;
        if (spec.read_enabled) RefreshEnabled();
        if (generation != invoking_generation || !ButtonEnabled(index)) return false;
        if (!hwnd || !IsWindow(hwnd) || !owner || !IsWindow(owner) ||
            index >= spec.buttons.size() || !spec.on_command) return false;
        // The callback may synchronously close this window or its owner.
        const auto callback = spec.on_command;
        callback(index);
        return true;
    }
    std::vector<std::wstring> FieldValues() const {
        std::vector<std::wstring> values;
        values.reserve(spec.fields.size());
        for (const auto& field : spec.fields) values.push_back(field.value);
        return values;
    }
    bool RefreshFields() {
        if (spec.tree) { tree.Refresh(); return tree.Hwnd() != nullptr; }
        if (!hwnd || !IsWindow(hwnd) || !owner || !IsWindow(owner) || spec.fields.empty() || !spec.read_fields) return false;
        const HWND reading_window = hwnd;
        const auto reading_generation = generation;
        const auto read = spec.read_fields;
        std::vector<std::wstring> values;
        try { values = read(); }
        catch (...) { return false; }
        if (hwnd != reading_window || generation != reading_generation || !IsWindow(reading_window) ||
            values.size() != spec.fields.size()) return false;
        bool changed = false;
        for (size_t i = 0; i < values.size(); ++i) {
            if (spec.fields[i].value == values[i]) continue;
            spec.fields[i].value = std::move(values[i]);
            changed = true;
        }
        if (changed) InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    bool Formats() {
        title_format.reset();
        body_format.reset();
        field_label_format.reset();
        compositor.RecreateTextFormats(scale);
        painter.SetScale(scale);
        return SUCCEEDED(typography::CreateTextFormat(compositor.DwriteFactory(),
            {typography::FontRole::Display, 18.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title_format)) &&
            SUCCEEDED(typography::CreateTextFormat(compositor.DwriteFactory(),
            {typography::FontRole::Text, 13.0f * scale}, &body_format)) &&
            SUCCEEDED(typography::CreateTextFormat(compositor.DwriteFactory(),
            {typography::FontRole::Text, 11.0f * scale}, &field_label_format));
    }
    void MeasureDescription(float width) {
        const float text_width = (std::max)(1.0f, width - 48.0f * scale);
        description_height = spec.description.empty() || !body_format.get() ? 0.0f : (std::min)(112.0f * scale,
            std::ceil(typography::MeasureWrapped(compositor.DwriteFactory(), body_format.get(), spec.description, text_width)));
        if (body_format.get() && field_label_format.get()) MeasureGroups(width);
        LayoutTree();
    }
    void SizeToContent(bool center_on_owner, const RECT* suggested = nullptr, float width_dip = 480.0f) {
        RECT position{};
        if (suggested) position = *suggested;
        else GetWindowRect(hwnd, &position);
        MONITORINFO monitor{sizeof(monitor)};
        const HMONITOR target = center_on_owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
            : MonitorFromRect(&position, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(target, &monitor)) return;
        const int width = (std::min)(static_cast<int>(width_dip * scale),
            static_cast<int>((std::max)(1L, monitor.rcWork.right - monitor.rcWork.left - 24)));
        MeasureDescription(static_cast<float>(width));
        const float content_height = ContentBottom();
        const int height = (std::min)(static_cast<int>(std::ceil(content_height)),
            static_cast<int>((std::max)(1L, monitor.rcWork.bottom - monitor.rcWork.top - 24)));
        if (center_on_owner) {
            CenterOwnedWindow(hwnd, owner, width, height);
        } else {
            const int left = static_cast<int>(std::clamp(position.left, monitor.rcWork.left, monitor.rcWork.right - width));
            const int top = static_cast<int>(std::clamp(position.top, monitor.rcWork.top, monitor.rcWork.bottom - height));
            SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        scroll = std::clamp(scroll, 0.0f, MaxScroll());
    }
    void DrawWrapped(const std::wstring& text, IDWriteTextFormat* format,
                     D2D1_RECT_F bounds, D2D1_COLOR_F color, float line_height = 0.0f) {
        if (text.empty() || !format) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(compositor.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            format, bounds.right - bounds.left, bounds.bottom - bounds.top, &layout))) return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (line_height > 0.0f)
            layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_height, line_height * 0.8f);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(compositor.DwriteFactory()->CreateEllipsisTrimmingSign(format, &ellipsis))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis.get());
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(compositor.Dc()->CreateSolidColorBrush(color, &brush)))
            compositor.DrawTextLayout({bounds.left, bounds.top}, layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    bool Render() {
        if (compositor.NeedsRecovery()) {
            // A rebuilt D2D context may reuse the old address; retire its resources first.
            painter.SetCompositor(nullptr);
            if (!compositor.Recover()) return false;
            painter.SetCompositor(&compositor);
            if (!Formats()) return false;
        }
        if (!compositor.Dc()) return false;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark, accent);
        BeginSurface(compositor, painter, theme, dark, high_contrast, false, scale);
        if (!spec.content_only) DrawWrapped(spec.title, title_format.get(), {24.0f * scale, 24.0f * scale,
            Width() - 52.0f * scale, 76.0f * scale},
            ExtensionPanelTitleColor(spec.title_tone, high_contrast, theme.accent, theme.bg, theme.text));
        fluent::ControlState close_state;
        close_state.hovered = hover == kClose;
        close_state.pressed = pressed == kClose;
        close_state.focused = focus == static_cast<int>(spec.buttons.size()) && GetFocus() == hwnd;
        close_state.keyboard_focus = close_state.focused && keyboard_focus;
        if (!spec.content_only) painter.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);
        if (!spec.content_only && close_state.focused && keyboard_focus) painter.DrawFocusRing(CloseRect(), 4.0f * scale);
        compositor.Dc()->PushAxisAlignedClip({0, BodyTop(), Width(), Height()}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawWrapped(spec.description, body_format.get(), {24.0f * scale, BodyTop() - scroll,
            Width() - 24.0f * scale, BodyTop() + DescriptionHeight() - scroll}, theme.text_secondary);
        for (size_t i = 0; i < spec.fields.size(); ++i) {
            const float top = FieldsTop() + static_cast<float>(i) * FieldPitch() - scroll;
            DrawWrapped(spec.fields[i].label, field_label_format.get(),
                {24.0f * scale, top, Width() - 24.0f * scale, top + 18.0f * scale}, theme.text_secondary);
            DrawWrapped(spec.fields[i].value, body_format.get(),
                {24.0f * scale, top + 20.0f * scale, Width() - 24.0f * scale, top + 64.0f * scale}, theme.text, 22.0f * scale);
        }
        for (size_t i = 0; i < group_bounds.size(); ++i) {
            auto title = group_bounds[i].title;
            title.top -= scroll; title.bottom -= scroll;
            auto description = group_bounds[i].description;
            description.top -= scroll; description.bottom -= scroll;
            DrawWrapped(spec.groups[i].title, body_format.get(), title, theme.text, 22.0f * scale);
            DrawWrapped(spec.groups[i].description, field_label_format.get(), description, theme.text_secondary, 16.0f * scale);
        }
        for (size_t i = 0; i < spec.buttons.size(); ++i) {
            fluent::ButtonSpec button;
            button.kind = spec.primary_button == i ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard;
            button.bounds = ButtonRect(i);
            const std::wstring label = FitTextEnd(spec.buttons[i], button.bounds.right - button.bounds.left - 4.0f * scale,
                [&](std::wstring_view text) { return painter.MeasureButtonWidth(text); });
            button.text = label;
            button.state.enabled = ButtonEnabled(i);
            button.state.hovered = hover == static_cast<int>(i);
            button.state.pressed = pressed == static_cast<int>(i);
            button.state.focused = focus == static_cast<int>(i) && GetFocus() == hwnd;
            button.state.keyboard_focus = button.state.focused && keyboard_focus;
            painter.DrawButton(button);
        }
        compositor.Dc()->PopAxisAlignedClip();
        if (MaxScroll() > 0) {
            fluent::ScrollbarSpec scrollbar;
            scrollbar.viewport = {0, BodyTop(), Width(), Height()};
            scrollbar.offset = scroll;
            scrollbar.viewport_extent = Height() - BodyTop();
            scrollbar.content_extent = ContentBottom() - BodyTop();
            scrollbar.opacity = 1;
            painter.DrawScrollbar(scrollbar);
        }
        EndSurface(compositor);
        return !compositor.NeedsRecovery();
    }
    bool SaveCapturedFrame(const wchar_t* path) {
        auto* frame_bitmap = compositor.HeldFrame();
        if (!frame_bitmap) return false;
        const auto size = frame_bitmap->GetPixelSize();
        if (!size.width || !size.height) return false;
        ComPtr<ID2D1Bitmap1> readable;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            frame_bitmap->GetPixelFormat(), 96.0f, 96.0f);
        if (FAILED(compositor.Dc()->CreateBitmap(size, nullptr, 0, &properties, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, frame_bitmap, nullptr))) return false;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        struct Unmap {
            ID2D1Bitmap1* bitmap;
            ~Unmap() { bitmap->Unmap(); }
        } unmap{readable.get()};
        std::vector<BYTE> composed;
        BYTE* pixels = mapped.bits;
        UINT stride = mapped.pitch;
        if (spec.tree && tree.Hwnd()) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(size.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(size.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
            if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); return false; }
            const auto previous = SelectObject(dc, bitmap);
            stride = size.width * 4;
            for (UINT y = 0; y < size.height; ++y)
                std::memcpy(static_cast<BYTE*>(bits) + static_cast<size_t>(y) * stride,
                    mapped.bits + static_cast<size_t>(y) * mapped.pitch, stride);
            const RECT bounds = TreeBounds();
            if (!tree.PaintTo(dc, bounds.left, bounds.top)) {
                SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
                return false;
            }
            composed.assign(static_cast<BYTE*>(bits), static_cast<BYTE*>(bits) + static_cast<size_t>(stride) * size.height);
            for (size_t i = 3; i < composed.size(); i += 4) composed[i] = 255;
            SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
            pixels = composed.data();
        }
        // A valid PNG alone is not evidence that the panel was rendered.
        unsigned minimum = 255;
        unsigned maximum = 0;
        for (UINT y = 0; y < size.height; ++y) {
            const BYTE* row = pixels + static_cast<size_t>(y) * stride;
            for (UINT x = 0; x < size.width; ++x) {
                const BYTE* pixel = row + static_cast<size_t>(x) * 4;
                if (!pixel[3]) continue;
                for (int channel = 0; channel < 3; ++channel) {
                    minimum = (std::min)(minimum, static_cast<unsigned>(pixel[channel]));
                    maximum = (std::max)(maximum, static_cast<unsigned>(pixel[channel]));
                }
            }
        }
        if (maximum <= minimum || maximum - minimum < 8) return false;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(size.width, size.height))) return false;
        WICPixelFormatGUID output_format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&output_format))) return false;
        ComPtr<IWICBitmap> source;
        if (FAILED(wic->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
            stride, stride * size.height, pixels, &source))) return false;
        if (FAILED(frame->WriteSource(source.get(), nullptr)) || FAILED(frame->Commit())) return false;
        return SUCCEEDED(encoder->Commit());
    }
    int Hit(LPARAM point) const {
        const float x = static_cast<float>(GET_X_LPARAM(point));
        const float y = static_cast<float>(GET_Y_LPARAM(point));
        if (ContainsRect(CloseRect(), x, y)) return kClose;
        if (y < BodyTop()) return kNone;
        for (size_t i = 0; i < spec.buttons.size(); ++i)
            if (ContainsRect(ButtonRect(i), x, y)) return static_cast<int>(i);
        return kNone;
    }
    void MoveFocus(int delta) {
        const int count = static_cast<int>(spec.buttons.size()) + 1;
        const auto current = std::find(button_order.begin(), button_order.end(), static_cast<size_t>(focus));
        const int position = current == button_order.end() ? count - 1 : static_cast<int>(current - button_order.begin());
        const int next = (position + delta + count) % count;
        focus = next == count - 1 ? count - 1 : static_cast<int>(button_order[static_cast<size_t>(next)]);
        keyboard_focus = true;
        if (focus < static_cast<int>(spec.buttons.size())) {
            const auto bounds = ButtonRect(static_cast<size_t>(focus));
            if (bounds.top < BodyTop()) scroll -= BodyTop() - bounds.top;
            else if (bounds.bottom > Height() - 12.0f * scale) scroll += bounds.bottom - Height() + 12.0f * scale;
            scroll = std::clamp(scroll, 0.0f, MaxScroll());
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->hwnd = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        return self->Handle(window, message, wparam, lparam);
    }
    LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_NCCALCSIZE: return 0;
        case WM_NCHITTEST: {
            if (docked) return HTCLIENT;
            const auto hit = BorderlessHitTest(window, lparam, 22.0f * scale, CloseRect());
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        case WM_CREATE:
            UpdateWindowTheme(window, dark);
            if (!compositor.Init(window) || !Formats()) return -1;
            return 0;
        case WM_SIZE:
            if (compositor.Dc()) compositor.Resize(LOWORD(lparam), HIWORD(lparam));
            MeasureDescription(static_cast<float>(LOWORD(lparam)));
            scroll = std::clamp(scroll, 0.0f, MaxScroll());
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
            Formats();
            if (!docked) SizeToContent(false, reinterpret_cast<RECT*>(lparam));
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            Render();
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (wparam == kFieldsTimer && IsWindowVisible(window)) {
                RefreshFields();
                RefreshEnabled();
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (spec.tree && tree.Hwnd()) { SetFocus(tree.Hwnd()); return 0; }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            hover = Hit(lparam);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover = kNone;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            scroll = std::clamp(scroll - static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 48.0f * scale,
                0.0f, MaxScroll());
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window);
            pressed = Hit(lparam);
            if (pressed >= 0) focus = pressed;
            else if (pressed == kClose) focus = static_cast<int>(spec.buttons.size());
            keyboard_focus = false;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int command = Hit(lparam) == pressed ? pressed : kNone;
            pressed = kNone;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            if (command == kClose) Close();
            else if (command >= 0) Invoke(static_cast<size_t>(command));
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressed = kNone;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                if (spec.content_only) SendMessageW(owner, WM_KEYDOWN, wparam, lparam);
                else Close();
                return 0;
            }
            if (wparam == VK_TAB) { MoveFocus(GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1); return 0; }
            if (wparam == VK_DOWN || wparam == VK_RIGHT) { MoveFocus(1); return 0; }
            if (wparam == VK_UP || wparam == VK_LEFT) { MoveFocus(-1); return 0; }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (focus == static_cast<int>(spec.buttons.size())) Close();
                else if (focus >= 0) Invoke(static_cast<size_t>(focus));
                return 0;
            }
            break;
        case WM_CLOSE: Close(); return 0;
        case WM_NCDESTROY: {
            const auto closed = std::move(spec.on_closed);
            KillTimer(window, kFieldsTimer);
            tree.Close();
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            ++generation;
            hwnd = nullptr;
            owner = nullptr;
            spec.on_command = {};
            spec.read_fields = {};
            spec.read_enabled = {};
            spec.tree_callbacks = {};
            title_format.reset();
            body_format.reset();
            field_label_format.reset();
            painter.SetCompositor(nullptr);
            compositor.Shutdown();
            if (closed) closed();
            return DefWindowProcW(window, message, wparam, lparam);
        }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    bool Create(HWND parent, ExtensionPanelSpec value, bool is_dark, D2D1_COLOR_F color, float dpi_scale, bool visible, const RECT* dock_bounds = nullptr, bool activate = true) {
        if (!IsWindow(parent) || value.buttons.size() > 8 || value.fields.size() > 3 ||
            !std::isfinite(dpi_scale) || dpi_scale <= 0) return false;
        if (value.groups.size() > 4 || (value.primary_button && *value.primary_button >= value.buttons.size())) return false;
        if (!value.groups.empty()) {
            std::vector<bool> used(value.buttons.size(), false);
            size_t count = 0;
            for (const auto& group : value.groups) {
                if (group.title.empty() || group.buttons.empty()) return false;
                for (const size_t index : group.buttons) {
                    if (index >= used.size() || used[index]) return false;
                    used[index] = true; ++count;
                }
            }
            if (count != value.buttons.size()) return false;
        }
        Close();
        ++generation;
        instance = ++next_panel_instance;
        spec = std::move(value);
        docked = dock_bounds != nullptr;
        owner = parent;
        dark = is_dark;
        accent = color;
        scale = std::clamp(dpi_scale, 0.75f, 4.0f);
        focus = 0;
        hover = pressed = kNone;
        scroll = 0;
        keyboard_focus = true;
        painter.SetCompositor(&compositor);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = kWindowClass;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        if (!CreateWindowExW(docked ? 0 : WS_EX_TOOLWINDOW, kWindowClass, spec.title.c_str(),
            (docked ? WS_CHILD | WS_CLIPSIBLINGS : WS_POPUP | WS_CAPTION | WS_SYSMENU) | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(480 * scale), static_cast<int>(420 * scale),
            owner, nullptr, window_class.hInstance, this)) return false;
        if (spec.tree && !tree.Create(hwnd, *spec.tree, spec.tree_callbacks, dark, accent, scale)) {
            Close();
            return false;
        }
        if (docked) SetWindowPos(hwnd, nullptr, dock_bounds->left, dock_bounds->top,
            dock_bounds->right - dock_bounds->left, dock_bounds->bottom - dock_bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        else SizeToContent(true);
        focus = button_order.empty() ? 0 : static_cast<int>(button_order.front());
        enabled.clear();
        if (spec.read_enabled) RefreshEnabled();
        if (!hwnd || !IsWindow(hwnd)) return false;
        if (!visible && docked && (spec.tree || (!spec.fields.empty() && spec.read_fields) || spec.read_enabled)) SetTimer(hwnd, kFieldsTimer, 250, nullptr);
        if (visible) {
            ShowWindow(hwnd, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
            if (activate && !docked) SetForegroundWindow(hwnd);
            if (activate) SetFocus(hwnd);
            if (spec.tree || (!spec.fields.empty() && spec.read_fields) || spec.read_enabled) SetTimer(hwnd, kFieldsTimer, 250, nullptr);
        }
        return true;
    }
};

ExtensionPanel::ExtensionPanel() : impl_(std::make_unique<Impl>()) {}
ExtensionPanel::~ExtensionPanel() = default;
bool ExtensionPanel::Show(HWND owner, ExtensionPanelSpec spec, bool dark, D2D1_COLOR_F accent, float scale) {
    return impl_->Create(owner, std::move(spec), dark, accent, scale, true);
}
void ExtensionPanel::Close() { impl_->Close(); }
bool ExtensionPanel::ShowDocked(HWND parent, const RECT& bounds, ExtensionPanelSpec spec,
    bool dark, D2D1_COLOR_F accent, float scale, bool visible) {
    return bounds.right > bounds.left && bounds.bottom > bounds.top &&
        impl_->Create(parent, std::move(spec), dark, accent, scale, visible, &bounds);
}
bool ExtensionPanel::IsDocked() const { return IsOpen() && impl_->docked; }
void ExtensionPanel::UpdateDocked(const RECT& bounds, bool dark, D2D1_COLOR_F accent, float scale) {
    if (!IsDocked()) return;
    UpdateAppearance(dark, accent, scale);
    RECT current{}; GetWindowRect(impl_->hwnd, &current);
    MapWindowPoints(nullptr, impl_->owner, reinterpret_cast<POINT*>(&current), 2);
    if (!EqualRect(&current, &bounds)) SetWindowPos(impl_->hwnd, nullptr, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
void ExtensionPanel::UpdateAppearance(bool dark, D2D1_COLOR_F accent, float scale) {
    if (!IsOpen() || !std::isfinite(scale) || scale <= 0) return;
    const float next_scale = std::clamp(scale, 0.75f, 4.0f);
    const bool changed = impl_->dark != dark || impl_->scale != next_scale ||
        impl_->accent.r != accent.r || impl_->accent.g != accent.g || impl_->accent.b != accent.b ||
        impl_->accent.a != accent.a;
    if (!changed) return;
    const bool resized = impl_->scale != next_scale;
    impl_->dark = dark;
    impl_->accent = accent;
    impl_->scale = next_scale;
    if (resized) impl_->Formats();
    UpdateWindowTheme(impl_->hwnd, dark);
    impl_->MeasureDescription(impl_->Width());
    impl_->LayoutTree();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}
bool ExtensionPanel::Remount(HWND owner, const RECT* dock_bounds, bool dark, D2D1_COLOR_F accent, float scale) {
    if (!IsOpen() || !IsWindow(owner) || !std::isfinite(scale) || scale <= 0 ||
        (dock_bounds && (dock_bounds->right <= dock_bounds->left || dock_bounds->bottom <= dock_bounds->top))) return false;
    const bool docked = dock_bounds != nullptr;
    if (impl_->owner == owner && impl_->docked == docked) {
        if (docked) UpdateDocked(*dock_bounds, dark, accent, scale);
        else UpdateAppearance(dark, accent, scale);
        return true;
    }
    const HWND window = impl_->hwnd;
    const HWND focus = GetFocus();
    const bool keep_focus = focus == window || IsChild(window, focus);
    const auto old_style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto old_ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const HWND old_owner = impl_->owner;
    const bool old_docked = impl_->docked;
    const LONG_PTR style = (docked ? WS_CHILD | WS_CLIPSIBLINGS : WS_POPUP | WS_CAPTION | WS_SYSMENU) |
        WS_CLIPCHILDREN | (old_style & WS_VISIBLE);
    SetLastError(0);
    SetWindowLongPtrW(window, GWL_STYLE, style);
    if (GetLastError()) return false;
    SetWindowLongPtrW(window, GWL_EXSTYLE, docked ? 0 : WS_EX_TOOLWINDOW);
    impl_->docked = docked;
    impl_->owner = owner;
    SetLastError(0);
    const HWND previous_parent = SetParent(window, docked ? owner : nullptr);
    const DWORD parent_error = GetLastError();
    if (!previous_parent && parent_error) {
        impl_->docked = old_docked;
        impl_->owner = old_owner;
        SetWindowLongPtrW(window, GWL_STYLE, old_style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, old_ex_style);
        return false;
    }
    if (!docked) SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    UpdateAppearance(dark, accent, scale);
    if (docked) {
        SetWindowPos(window, HWND_TOP, dock_bounds->left, dock_bounds->top,
            dock_bounds->right - dock_bounds->left, dock_bounds->bottom - dock_bounds->top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        impl_->SizeToContent(true);
    }
    impl_->LayoutTree();
    if (keep_focus && IsWindow(focus)) SetFocus(focus);
    return IsOpen();
}
bool ExtensionPanel::Undock(bool dark, D2D1_COLOR_F accent, float scale) {
    if (!IsDocked()) return false;
    const HWND owner = impl_->owner;
    auto spec = impl_->spec;
    Close();
    return Show(owner, std::move(spec), dark, accent, scale);
}
bool ExtensionPanel::SaveCurrentFrame(const wchar_t* path) {
    if (!IsOpen() || !path || !*path) return false;
    impl_->compositor.CaptureNextFrame();
    return impl_->Render() && impl_->SaveCapturedFrame(path);
}
bool ExtensionPanel::IsOpen() const { return impl_->hwnd && IsWindow(impl_->hwnd); }
HWND ExtensionPanel::Hwnd() const { return impl_->hwnd; }
HWND ExtensionPanel::TreeNodeWindow(const std::string& id) const { return impl_->tree.NodeWindow(id); }
std::optional<std::string> ExtensionPanel::TreeStateValue(const std::string& key) const { return impl_->tree.StateValue(key); }
uint64_t ExtensionPanel::Instance() const { return IsOpen() ? impl_->instance : 0; }
float ExtensionPanel::DpiScale() const { return impl_->scale; }
uint64_t ExtensionPanel::TreeStateRevision() const { return impl_->tree.StateRevision(); }
std::optional<extensions::UiNode> ExtensionPanel::TreeDataNode(const std::string& id) const {
    return impl_->tree.DataNode(id);
}
bool ExtensionPanel::BindTreeData(const std::string& id, const std::optional<extensions::UiDataSource>& source) {
    return IsOpen() && impl_->tree.BindDataSource(id, source);
}
bool ExtensionPanel::RequestTreeData(const std::string& id, const std::string& operation) {
    return IsOpen() && impl_->tree.RequestData(id, operation);
}
bool ExtensionPanel::SetTreeStates(const std::vector<extensions::UiStateEntry>& values) {
    return IsOpen() && impl_->tree.SetStates(values);
}
bool ExtensionPanel::UpdateTree(const extensions::UiTree& tree) {
    if (!IsOpen() || !impl_->spec.tree || !impl_->tree.UpdateTree(tree)) return false;
    impl_->spec.tree = tree;
    impl_->LayoutTree();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    return true;
}
bool ExtensionPanel::RevealTreeNode(const std::string& id) {
    return IsOpen() && impl_->tree.RevealNode(id);
}
bool ExtensionPanel::InvokeButton(size_t index) { return impl_->Invoke(index); }
RECT ExtensionPanel::ButtonBounds(size_t index) const {
    if (!IsOpen() || index >= impl_->spec.buttons.size()) return {};
    const auto rect = impl_->ButtonRect(index);
    return {static_cast<LONG>(rect.left), static_cast<LONG>(rect.top), static_cast<LONG>(rect.right), static_cast<LONG>(rect.bottom)};
}
bool ExtensionPanel::RefreshFields() { return impl_->RefreshFields(); }
std::vector<std::wstring> ExtensionPanel::FieldValues() const { return impl_->FieldValues(); }
bool ExtensionPanel::SaveDebugSnapshot(const wchar_t* path, bool dark, float scale, const std::string& reveal_node) {
    if (!IsOpen() || !path || !*path) return false;
    Impl snapshot;
    auto copy = impl_->spec;
    if (copy.tree) {
        copy.tree = impl_->tree.SnapshotTree();
        // Snapshot rows and bound values are frozen; no provider or host commands run here.
        copy.tree_callbacks = {};
    }
    copy.on_closed = {};
    copy.read_fields = {};
    if (copy.read_enabled) copy.read_enabled = [values = impl_->enabled] { return values; };
    if (!snapshot.Create(impl_->owner, std::move(copy), dark, impl_->accent, scale, false)) return false;
    snapshot.SizeToContent(false, nullptr, (std::min)(480.0f, impl_->Width() / impl_->scale));
    snapshot.tree.CopySnapshotViewState(impl_->tree);
    if (!reveal_node.empty() && !snapshot.tree.RevealNode(reveal_node)) return false;
    // Present flips the swap chain. Preserve its completed target before that
    // flip rather than reading the newly selected, not-yet-drawn back buffer.
    snapshot.compositor.CaptureNextFrame();
    return snapshot.Render() && snapshot.SaveCapturedFrame(path);
}

bool ExtensionPanel::SavePreviewSnapshot(HWND owner, ExtensionPanelSpec spec, const wchar_t* path,
    bool dark, D2D1_COLOR_F accent, float scale) {
    if (!path || !*path) return false;
    spec.on_command = {};
    spec.read_fields = {};
    spec.read_enabled = {};
    spec.on_closed = {};
    spec.tree_callbacks = {};
    Impl snapshot;
    if (!snapshot.Create(owner, std::move(spec), dark, accent, scale, false) || IsWindowVisible(snapshot.hwnd)) return false;
    snapshot.compositor.CaptureNextFrame();
    return snapshot.Render() && snapshot.SaveCapturedFrame(path);
}

}

namespace pulse::ui {
bool ExtensionPanel::RefreshEnabled() { return impl_->RefreshEnabled(); }
bool ExtensionPanel::ButtonEnabled(size_t index) const { return impl_->ButtonEnabled(index); }
}
