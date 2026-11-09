// Unified command geometry, with the existing SVG loader retained for artwork.
#include "ui_renderer.h"
#ifdef PULSE_STARTUP_TRACE_DIAGNOSTICS
#include "../app/startup_trace.h"
#define PULSE_SVG_MARK(label) app::startup_trace::Mark(label)
#else
#define PULSE_SVG_MARK(label) ((void)0)
#endif
#include "command_icons.h"
#include "fluent_color_svg.h"
#include "../common/windows_compat.h"
#include "../app/resource.h"
#include <d2d1svg.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::ui {
namespace {

command_icons::Icon CommandIconForResource(int resource_id) noexcept {
    using I = command_icons::Icon;
    switch (resource_id) {
    case IDR_FLUENT_CUT_SVG: return I::Cut;
    case IDR_FLUENT_COPY_SVG: return I::Copy;
    case IDR_FLUENT_PASTE_SVG: return I::Paste;
    case IDR_FLUENT_RENAME_SVG: return I::Rename;
    case IDR_FLUENT_DELETE_SVG: return I::Delete;
    case IDR_FLUENT_STAR_SVG: return I::Star;
    case IDR_FLUENT_HISTORY_SVG: return I::History;
    case IDR_FLUENT_DESKTOP_SVG: return I::Desktop;
    case IDR_FLUENT_DOWNLOADS_SVG: return I::Download;
    case IDR_FLUENT_RECYCLE_SVG: return I::Recycle;
    case IDR_FLUENT_DRIVE_SVG: return I::Drive;
    case IDR_FLUENT_FOLDER_SVG: return I::Folder;
    case IDR_FLUENT_NETWORK_SVG: return I::Network;
    case IDR_FLUENT_SEARCH_SVG: case IDR_FLUENT_OMNIBAR_SEARCH_SVG: return I::Search;
    case IDR_FLUENT_APPS_SVG: return I::Grid;
    case IDR_FLUENT_PANEL_SVG: return I::Panel;
    case IDR_FLUENT_PANEL_CLOSE_SVG: return I::PanelClose;
    case IDR_FLUENT_ADD_SVG: return I::Add;
    case IDR_FLUENT_PC_SVG: return I::Computer;
    case IDR_FLUENT_TAG_SVG: return I::Tag;
    case IDR_FLUENT_WORKSPACE_SVG: return I::Workspace;
    case IDR_FLUENT_PICTURES_SVG: return I::Image;
    case IDR_FLUENT_VIDEOS_SVG: return I::Video;
    case IDR_FLUENT_MUSIC_SVG: return I::Music;
    case IDR_FLUENT_DOCUMENTS_SVG: return I::File;
    default: return I::None;
    }
}

std::string_view QuotedAttr(std::string_view tag, std::string_view name) {
    const std::string needle = std::string(name) + "=";
    const size_t pos = tag.find(needle);
    if (pos == std::string_view::npos) return {};
    size_t i = pos + needle.size();
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i >= tag.size()) return {};
    const char quote = tag[i];
    if (quote != '"' && quote != '\'') return {};
    const size_t start = i + 1;
    const size_t end = tag.find(quote, start);
    if (end == std::string_view::npos) return {};
    return tag.substr(start, end - start);
}

std::string FirstStopColor(std::string_view gradient) {
    size_t pos = 0;
    while (pos < gradient.size()) {
        const size_t stop = gradient.find("<stop", pos);
        if (stop == std::string_view::npos) break;
        const size_t close = gradient.find('>', stop);
        if (close == std::string_view::npos) break;
        const std::string_view tag = gradient.substr(stop, close - stop);
        const std::string_view color = QuotedAttr(tag, "stop-color");
        if (!color.empty()) return std::string(color);
        pos = close + 1;
    }
    return "#0094f0";
}

std::unordered_map<std::string, std::string> GradientFills(std::string_view svg) {
    std::unordered_map<std::string, std::string> fills;
    size_t pos = 0;
    while (pos < svg.size()) {
        size_t open = svg.find("<linearGradient", pos);
        size_t radial = svg.find("<radialGradient", pos);
        if (open == std::string_view::npos) open = radial;
        else if (radial != std::string_view::npos) open = std::min(open, radial);
        if (open == std::string_view::npos) break;
        const bool linear = svg.compare(open, 15, "<linearGradient") == 0;
        const char* close_tag = linear ? "</linearGradient>" : "</radialGradient>";
        const size_t close = svg.find(close_tag, open);
        if (close == std::string_view::npos) break;
        const std::string_view block = svg.substr(open, close - open);
        const size_t tag_end = block.find('>');
        const std::string_view open_tag = tag_end == std::string_view::npos ? block
                                                                           : block.substr(0, tag_end);
        const std::string_view id = QuotedAttr(open_tag, "id");
        if (!id.empty()) fills.emplace(std::string(id), FirstStopColor(block));
        pos = close + 1;
    }
    return fills;
}

std::string ResolveFill(std::string_view fill,
                        const std::unordered_map<std::string, std::string>& gradients) {
    if (fill.size() >= 6 && fill.substr(0, 5) == "url(#" && fill.back() == ')') {
        const std::string id(fill.substr(5, fill.size() - 6));
        if (const auto it = gradients.find(id); it != gradients.end()) return it->second;
        return "#0094f0";
    }
    if (!fill.empty() && fill != "none") return std::string(fill);
    return "#0094f0";
}

// Direct2D's SVG subset rejects some Color markup (radial matrix transforms,
// fill-opacity overlays). Keep viewBox + path with solid fills from the first
// gradient stop — official geometry, no hand-drawn shapes.
std::string SimplifySvgForD2d(std::string_view svg) {
    std::string view_box = "0 0 24 24";
    const size_t svg_open = svg.find("<svg");
    if (svg_open != std::string_view::npos) {
        const size_t svg_tag_end = svg.find('>', svg_open);
        if (svg_tag_end != std::string_view::npos) {
            const std::string_view vb = QuotedAttr(svg.substr(svg_open, svg_tag_end - svg_open),
                                                   "viewBox");
            if (!vb.empty()) view_box.assign(vb);
        }
    }
    const auto gradients = GradientFills(svg);
    std::string out;
    out.reserve(svg.size());
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
    out += view_box;
    out += "\">";
    size_t pos = 0;
    bool any_path = false;
    while (pos < svg.size()) {
        const size_t path_at = svg.find("<path", pos);
        if (path_at == std::string_view::npos) break;
        const size_t tag_end = svg.find('>', path_at);
        if (tag_end == std::string_view::npos) break;
        const std::string_view tag = svg.substr(path_at, tag_end - path_at);
        pos = tag_end + 1;
        if (!QuotedAttr(tag, "fill-opacity").empty()) continue;
        const std::string_view d = QuotedAttr(tag, "d");
        if (d.empty()) continue;
        const std::string fill = ResolveFill(QuotedAttr(tag, "fill"), gradients);
        const std::string_view fill_rule = QuotedAttr(tag, "fill-rule");
        out += "<path fill=\"";
        out += fill;
        out += "\" d=\"";
        out += d;
        out += '"';
        if (!fill_rule.empty()) {
            out += " fill-rule=\"";
            out += fill_rule;
            out += '"';
        }
        out += "/>";
        any_path = true;
    }
    out += "</svg>";
    return any_path ? out : std::string();
}

struct FluentBitmapPlacement {
    int size_q = 0, fx = 0, fy = 0;
    float x = 0, y = 0;
};
bool FluentBitmapPlacementFor(ID2D1DeviceContext* dc, const D2D1_RECT_F& bounds, FluentBitmapPlacement& out) {
    D2D1_MATRIX_3X2_F m{};
    dc->GetTransform(&m);
    float dpi_x = 0, dpi_y = 0;
    dc->GetDpi(&dpi_x, &dpi_y);
    const float art = std::min(std::max(0.0f, bounds.right - bounds.left), std::max(0.0f, bounds.bottom - bounds.top));
    if (!(art > 1.0f && art <= 256.0f && m._11 == 1.0f && m._22 == 1.0f && m._12 == 0.0f &&
          m._21 == 0.0f && dpi_x == 96.0f && dpi_y == 96.0f)) return false;
    const long long qx = std::llround(((bounds.left + bounds.right - art) * 0.5 + m._31) * 4.0);
    const long long qy = std::llround(((bounds.top + bounds.bottom - art) * 0.5 + m._32) * 4.0);
    const long long ix = qx >= 0 ? qx / 4 : -((3 - qx) / 4);
    const long long iy = qy >= 0 ? qy / 4 : -((3 - qy) / 4);
    out = {static_cast<int>(std::lround(art * 4.0f)), static_cast<int>(qx - ix * 4),
           static_cast<int>(qy - iy * 4), static_cast<float>(ix - 1) - m._31, static_cast<float>(iy - 1) - m._32};
    return true;
}
uint64_t FluentBitmapKey(int resource_id, int size_q, int fx, int fy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(resource_id)) << 32) |
           (static_cast<uint64_t>(size_q & 0xfffff) << 4) | static_cast<uint64_t>((fx << 2) | fy);
}

bool CreateSvgFromBytes(ID2D1DeviceContext5* dc, const void* bytes, DWORD byte_count,
                        ComPtr<ID2D1SvgDocument>& out) {
    out.reset();
    if (!dc || !bytes || byte_count == 0) return false;
    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byte_count);
    if (!stream.get()) return false;
    if (FAILED(dc->CreateSvgDocument(stream.get(), D2D1::SizeF(24.0f, 24.0f), &out)) ||
        !out.get()) {
        out.reset();
        return false;
    }
    return true;
}

} // namespace

bool MainRenderer::EnsureFluentSvg(int resource_id, bool colorful) {
    if (!colorful && CommandIconForResource(resource_id) != command_icons::Icon::None)
        return compositor_ && compositor_->Dc();
    if (compat::LegacyMode()) return false;
    if (fluent_svg_failed_.count(resource_id)) return false;
    if (const auto it = fluent_svgs_.find(resource_id);
        it != fluent_svgs_.end() && it->second.get() && empty_state_svg_dc_.get()) {
        return true;
    }
    if (!compositor_ || !compositor_->Dc()) return false;
    if (!empty_state_svg_dc_.get() &&
        FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_)))) {
        return false;
    }

    ComPtr<ID2D1SvgDocument> document;
    PULSE_SVG_MARK("svg_parse_begin");
    const bool loaded = LoadFluentSvg(empty_state_svg_dc_.get(), resource_id, document, ResourceModule());
    PULSE_SVG_MARK("svg_parse_end");
    if (!loaded) {
        fluent_svg_failed_.insert(resource_id);
        return false;
    }

    fluent_svgs_[resource_id] = std::move(document);
    return true;
}

bool MainRenderer::DrawFluentSvg(int resource_id, const D2D1_RECT_F& bounds, float opacity,
                                  const D2D1_COLOR_F* foreground, bool colorful) {
    const auto command = CommandIconForResource(resource_id);
    if (!colorful && command != command_icons::Icon::None) {
        auto* dc = compositor_ ? compositor_->Dc() : nullptr;
        if (!dc || !brTextSecondary_.get()) return false;
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        const auto saved_color = brTextSecondary_->GetColor();
        if (foreground) brTextSecondary_->SetColor(*foreground);
        const bool drawn = command_icons::Draw(dc, brTextSecondary_.get(), paneHeaderStroke_.get(),
                                                command, bounds, opacity);
        brTextSecondary_->SetColor(saved_color);
        return drawn;
    }
    if (!EnsureFluentSvg(resource_id, colorful)) return false;
    const auto it = fluent_svgs_.find(resource_id);
    if (it == fluent_svgs_.end() || !it->second.get() || !empty_state_svg_dc_.get()) return false;

    // Same placement as DrawFluentSvgFitted, from a cached bitmap copied to
    // whole device pixels. Scaled or rotated transforms draw the vector art.
    ID2D1DeviceContext* dc = compositor_->Dc();
    FluentBitmapPlacement placement;
    if (FluentBitmapPlacementFor(dc, bounds, placement)) {
        if (ID2D1Bitmap1* bitmap = FluentSvgBitmap(resource_id, it->second.get(),
                placement.size_q, placement.fx, placement.fy)) {
            const D2D1_SIZE_U px = bitmap->GetPixelSize();
            const float x = placement.x;
            const float y = placement.y;
            const D2D1_RECT_F dest = D2D1::RectF(x, y, x + static_cast<float>(px.width),
                                                 y + static_cast<float>(px.height));
            dc->DrawBitmap(bitmap, &dest, std::clamp(opacity, 0.0f, 1.0f),
                           D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            return true;
        }
    }
    return DrawFluentSvgFitted(empty_state_svg_dc_.get(), it->second.get(), bounds, opacity);
}

// Vendor marks share the document cache under negative keys (one per theme):
// their monochrome parts are currentColor, written in before parsing.
bool MainRenderer::DrawProviderLogo(int resource_id, bool dark, const D2D1_RECT_F& bounds) {
    if (compat::LegacyMode() || !compositor_ || !compositor_->Dc() || resource_id <= 0) return false;
    const int key = -(resource_id * 2 + (dark ? 1 : 0));
    if (fluent_svg_failed_.count(key)) return false;
    auto it = fluent_svgs_.find(key);
    if (it == fluent_svgs_.end() || !it->second.get() || !empty_state_svg_dc_.get()) {
        if (!empty_state_svg_dc_.get() &&
            FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_)))) {
            return false;
        }
        const HMODULE module = ResourceModule();
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
        const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
        const DWORD byte_count = resource ? SizeofResource(module, resource) : 0;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        if (!bytes || byte_count == 0) {
            fluent_svg_failed_.insert(key);
            return false;
        }
        std::string markup(static_cast<const char*>(bytes), byte_count);
        const std::string_view color = dark ? "#F2F2F2" : "#1B1B1B";
        for (size_t at = markup.find("currentColor"); at != std::string::npos; at = markup.find("currentColor", at))
            markup.replace(at, 12, color);
        ComPtr<ID2D1SvgDocument> document;
        if (!CreateSvgFromBytes(empty_state_svg_dc_.get(), markup.data(), static_cast<DWORD>(markup.size()), document)) {
            const std::string simplified = SimplifySvgForD2d(markup);
            if (simplified.empty() || !CreateSvgFromBytes(empty_state_svg_dc_.get(), simplified.data(),
                                                          static_cast<DWORD>(simplified.size()), document)) {
                fluent_svg_failed_.insert(key);
                return false;
            }
        }
        it = fluent_svgs_.insert_or_assign(key, std::move(document)).first;
    }
    return DrawFluentSvgFitted(empty_state_svg_dc_.get(), it->second.get(), bounds, 1.0f);
}

ID2D1DeviceContext5* MainRenderer::FluentRasterContext() {
    if (fluent_raster_dc_.get()) return fluent_raster_dc_.get();
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return nullptr;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    dc->GetDevice(&device);
    if (!device.get() || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) return nullptr;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&fluent_raster_dc_)))) return nullptr;
    return fluent_raster_dc_.get();
}

namespace {
ComPtr<ID2D1Bitmap1> RasterFluentSvg(ID2D1DeviceContext5* raster, ID2D1SvgDocument* svg,
                                  int size_q, int fx, int fy) {
    const float art = static_cast<float>(size_q) / 4.0f;
    const UINT32 side = static_cast<UINT32>(std::ceil(art + 0.75f)) + 2;
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(raster->CreateBitmap(D2D1::SizeU(side, side), nullptr, 0, props, &bitmap))) return {};
    ComPtr<ID2D1SvgElement> root;
    svg->GetRoot(&root);
    if (root.get()) root->SetAttributeValue(L"opacity", 1.0f);
    svg->SetViewportSize(D2D1::SizeF(24.0f, 24.0f));
    raster->SetTarget(bitmap.get());
    raster->Clear(D2D1::ColorF(0, 0.0f));
    raster->SetTransform(D2D1::Matrix3x2F::Scale(art / 24.0f, art / 24.0f) *
                         D2D1::Matrix3x2F::Translation(1.0f + fx * 0.25f, 1.0f + fy * 0.25f));
    raster->DrawSvgDocument(svg);
    return bitmap;
}
}

ID2D1Bitmap1* MainRenderer::FluentSvgBitmap(int resource_id, ID2D1SvgDocument* svg,
                                            int size_q, int fx, int fy) {
    const uint64_t key = FluentBitmapKey(resource_id, size_q, fx, fy);
    if (const auto found = fluent_bitmaps_.find(key); found != fluent_bitmaps_.end()) return found->second.get();
    if (fluent_bitmaps_.size() >= 512) fluent_bitmaps_.clear();
    auto& slot = fluent_bitmaps_[key]; // A null slot preserves the vector fallback on failure.
    auto* raster = FluentRasterContext();
    if (!raster || !svg) return nullptr;
    PULSE_SVG_MARK("svg_raster_begin");
    raster->BeginDraw();
    auto bitmap = RasterFluentSvg(raster, svg, size_q, fx, fy);
    const HRESULT hr = raster->EndDraw();
    raster->SetTarget(nullptr);
    PULSE_SVG_MARK("svg_raster_end");
    if (FAILED(hr)) return nullptr;
    slot = std::move(bitmap);
    return slot.get();
}

void MainRenderer::PrepareFluentSvgBitmaps(const std::vector<std::pair<int, D2D1_RECT_F>>& requests) {
    if (requests.empty() || !compositor_ || !compositor_->Dc()) return;
    struct Pending { uint64_t key; ID2D1SvgDocument* svg; FluentBitmapPlacement placement; ComPtr<ID2D1Bitmap1> bitmap; };
    std::vector<Pending> pending;
    for (const auto& [id, bounds] : requests) {
        FluentBitmapPlacement placement;
        if (!id || !FluentBitmapPlacementFor(compositor_->Dc(), bounds, placement)) continue;
        const auto key = FluentBitmapKey(id, placement.size_q, placement.fx, placement.fy);
        if (fluent_bitmaps_.contains(key) || std::any_of(pending.begin(), pending.end(),
            [key](const auto& item) { return item.key == key; })) continue;
        if (!EnsureFluentSvg(id, true)) continue;
        pending.push_back({key, fluent_svgs_.at(id).get(), placement, {}});
    }
    if (pending.empty()) return;
    auto* raster = FluentRasterContext();
    if (!raster) {
        for (const auto& item : pending) fluent_bitmaps_[item.key] = {};
        return;
    }
    PULSE_SVG_MARK("svg_batch_begin");
    raster->BeginDraw();
    for (auto& item : pending)
        item.bitmap = RasterFluentSvg(raster, item.svg, item.placement.size_q, item.placement.fx, item.placement.fy);
    const HRESULT hr = raster->EndDraw();
    raster->SetTarget(nullptr);
    PULSE_SVG_MARK("svg_batch_end");
    // Failed batches publish only negative cache entries, never unfinished bitmaps.
    // This keeps the original vector fallback without retrying every icon this frame.
    // Device-loss recovery clears these with the other device-dependent resources.
    if (fluent_bitmaps_.size() + pending.size() > 512) fluent_bitmaps_.clear();
    for (auto& item : pending) {
        if (FAILED(hr)) item.bitmap.reset();
        fluent_bitmaps_[item.key] = std::move(item.bitmap);
    }
}

bool LoadFluentSvg(ID2D1DeviceContext5* dc, int resource_id, ComPtr<ID2D1SvgDocument>& out, HMODULE module) {
    out.reset();
    if (!dc) return false;
    if (!module) module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byte_count = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byte_count == 0) return false;
    if (CreateSvgFromBytes(dc, bytes, byte_count, out)) return true;
    const std::string simplified = SimplifySvgForD2d(
        std::string_view(static_cast<const char*>(bytes), byte_count));
    return !simplified.empty() &&
           CreateSvgFromBytes(dc, simplified.data(), static_cast<DWORD>(simplified.size()), out);
}

bool DrawFluentSvgFitted(ID2D1DeviceContext5* dc, ID2D1SvgDocument* svg,
                         const D2D1_RECT_F& bounds, float opacity) {
    if (!dc || !svg) return false;
    ComPtr<ID2D1SvgElement> root;
    svg->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));

    const float available_width = std::max(0.0f, bounds.right - bounds.left);
    const float available_height = std::max(0.0f, bounds.bottom - bounds.top);
    const float art = std::min(available_width, available_height);
    if (art <= 1.0f) return false;
    const float left = (bounds.left + bounds.right - art) * 0.5f;
    const float top = (bounds.top + bounds.bottom - art) * 0.5f;

    svg->SetViewportSize(D2D1::SizeF(24.0f, 24.0f));
    D2D1_MATRIX_3X2_F previous{};
    dc->GetTransform(&previous);
    dc->SetTransform(
        D2D1::Matrix3x2F::Scale(art / 24.0f, art / 24.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    dc->DrawSvgDocument(svg);
    dc->SetTransform(previous);
    return true;
}

int FluentSvgIdForGlyph(std::wstring_view glyph) {
    if (glyph.empty()) return 0;
    if (glyph == L"\xE735") return IDR_FLUENT_STAR_SVG;
    if (glyph == L"\xE823") return IDR_FLUENT_HISTORY_SVG;
    if (glyph == L"\xE7F4") return IDR_FLUENT_DESKTOP_SVG;
    if (glyph == L"\xE896") return IDR_FLUENT_DOWNLOADS_SVG;
    if (glyph == L"\xE75C") return IDR_FLUENT_RECYCLE_SVG;
    if (glyph == L"\xE7F1") return IDR_FLUENT_DRIVE_SVG;
    if (glyph == L"\xE8B7") return IDR_FLUENT_FOLDER_SVG;
    if (glyph == L"\xE968") return IDR_FLUENT_NETWORK_SVG;
    if (glyph == L"\xE721") return IDR_FLUENT_SEARCH_SVG;
    // "This PC": monitor on a stand next to a tower, the way Explorer draws it.
    if (glyph == L"\xE977") return IDR_FLUENT_PC_SVG;
    if (glyph == L"\xE8EF") return IDR_FLUENT_COPY_SVG;
    if (glyph == L"\xE8A9") return IDR_FLUENT_APPS_SVG;
    if (glyph == L"\xE8EC") return IDR_FLUENT_TAG_SVG;
    if (glyph == L"\xF5BE") return IDR_FLUENT_WORKSPACE_SVG;
    // The library folders in 快速访问, in the same color set as 下载 and 桌面
    // (Fluent has no color music note: headphones stand for 音乐).
    if (glyph == L"\xE91B") return IDR_FLUENT_PICTURES_SVG;
    if (glyph == L"\xE714") return IDR_FLUENT_VIDEOS_SVG;
    if (glyph == L"\xE8D6") return IDR_FLUENT_MUSIC_SVG;
    if (glyph == L"\xE7C3") return IDR_FLUENT_DOCUMENTS_SVG;
    if (glyph == L"\xE8A0") return IDR_FLUENT_PANEL_SVG;
    if (glyph == L"\xE89F") return IDR_FLUENT_PANEL_CLOSE_SVG;
    return 0;
}

} // namespace pulse::ui
