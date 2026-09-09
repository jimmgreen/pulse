// ui_fluent_svg.cpp — Toolbar Fluent Color SVG load / draw (Direct2D SVG subset).
#include "ui_renderer.h"
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

bool MainRenderer::EnsureFluentSvg(int resource_id) {
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

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) {
        fluent_svg_failed_.insert(resource_id);
        return false;
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byte_count = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byte_count == 0) {
        fluent_svg_failed_.insert(resource_id);
        return false;
    }

    ComPtr<ID2D1SvgDocument> document;
    if (!CreateSvgFromBytes(empty_state_svg_dc_.get(), bytes, byte_count, document)) {
        const std::string simplified = SimplifySvgForD2d(
            std::string_view(static_cast<const char*>(bytes), byte_count));
        if (simplified.empty() ||
            !CreateSvgFromBytes(empty_state_svg_dc_.get(), simplified.data(),
                                static_cast<DWORD>(simplified.size()), document)) {
            fluent_svg_failed_.insert(resource_id);
            return false;
        }
    }

    fluent_svgs_[resource_id] = std::move(document);
    return true;
}

bool MainRenderer::DrawFluentSvg(int resource_id, const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureFluentSvg(resource_id)) return false;
    const auto it = fluent_svgs_.find(resource_id);
    if (it == fluent_svgs_.end() || !it->second.get() || !empty_state_svg_dc_.get()) return false;

    ID2D1SvgDocument* svg = it->second.get();
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
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(art / 24.0f, art / 24.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(svg);
    empty_state_svg_dc_->SetTransform(previous);
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
    if (glyph == L"\xE8EF") return IDR_FLUENT_COPY_SVG;
    if (glyph == L"\xE8A9") return IDR_FLUENT_APPS_SVG;
    if (glyph == L"\xE8A0") return IDR_FLUENT_PANEL_SVG;
    if (glyph == L"\xE89F") return IDR_FLUENT_PANEL_CLOSE_SVG;
    return 0;
}

} // namespace pulse::ui
