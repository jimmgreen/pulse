// Quick Look shell: the unified header and the full-width footer (P1).
//
//   header (50 px): badge | name / meta | type tools | ‹ 6 / 12 › | Open with App | ⋯ | ×
//   footer (30 px): format facts on the left, type controls on the right
//
// Narrow windows drop the meta line first, then fold the type tools into ⋯,
// then shorten the open button to its icon and finally hide the position.
// Text goes through Compositor::DrawTextLayout, so it uses LumaText whenever
// the UI does; high contrast falls back to DirectWrite and system colours.
#include "quick_preview_window.h"
#include "FluentTokens.h"
#include "text_contrast_scope.h"
#include "command_icons.h"
#include "legacy_icons.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/preview_extensions.h"
#include "../common/text_format.h"
#include "../ops/clipboard.h"
#include <d2d1helper.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>

#pragma comment(lib, "shlwapi.lib")

namespace pulse::ui {
namespace {
constexpr float kHeaderHeight = 50.0f;
constexpr float kFooterHeight = 30.0f;
constexpr float kHeaderPadLeft = 14.0f;
constexpr float kBadgeSize = 26.0f;
constexpr float kIconButton = 30.0f;
constexpr float kCloseWidth = 46.0f;
constexpr float kTitleMin = 96.0f;    // narrower title areas fold the next level
constexpr float kMetaMin = 200.0f;    // the meta line needs this much
constexpr float kOpenTextMax = 190.0f;
constexpr float kGlyphSize = 16.0f * 0.66f;
constexpr wchar_t kPrevGlyph[] = L"\xE72B";
constexpr wchar_t kNextGlyph[] = L"\xE72A";
constexpr wchar_t kMoreGlyph[] = L"\xE712";
constexpr wchar_t kSizeGlyph[] = L"\xE740";   // 窗口大小 (FullScreen arrows)
constexpr wchar_t kOpenGlyph[] = L"\xE8A7";   // OpenInNewWindow
constexpr wchar_t kCloseGlyph[] = L"\xE711";
constexpr wchar_t kFolderGlyph[] = L"\xE8B7";

// "PotPlayer (64-bit)" -> "PotPlayer": a trailing parenthesised edition
// note only crowds the button.
std::wstring ShortAppName(std::wstring name) {
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (!name.empty() && (name.back() == L')' || name.back() == L'\xFF09')) {
        const size_t open = name.find_last_of(L"(\xFF08");
        if (open != std::wstring::npos && open > 0) {
            std::wstring head = name.substr(0, open);
            while (!head.empty() && iswspace(head.back())) head.pop_back();
            if (!head.empty()) name = std::move(head);
        }
    }
    return name;
}

bool HighContrast() {
    return TextHighContrast();
}

bool Inside(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

std::wstring LowerExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool OneOf(const std::wstring& ext, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* item : list)
        if (ext == item) return true;
    return false;
}

// Badge colour by file family (the mockup's palette).
uint32_t BadgeRgb(const std::wstring& ext, bool folder) {
    if (folder) return 0xD97706;
    if (ext == L"svg") return 0xDB2777;
    if (OneOf(ext, {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"tif", L"tiff", L"webp", L"heic",
                    L"heif", L"hif", L"avif", L"ico", L"cur", L"jxl", L"exr", L"hdr", L"qoi",
                    L"psd", L"psb", L"wmf", L"emf", L"cr2", L"cr3", L"crw", L"nef", L"nrw",
                    L"arw", L"sr2", L"dng", L"raf", L"orf", L"rw2", L"pef", L"srw", L"rwl"}))
        return 0x7C3AED;
    if (ext == L"md" || ext == L"markdown") return 0x0EA5E9;
    if (OneOf(ext, {L"csv", L"tsv", L"tab", L"psv", L"xls", L"xlsx", L"xlsm"})) return 0x16A34A;
    if (OneOf(ext, {L"json", L"xml", L"ipynb", L"yaml", L"yml", L"toml"})) return 0xCA8A04;
    if (ext == L"pdf" || ext == L"ai") return 0xDC2626;
    if (OneOf(ext, {L"doc", L"docx", L"docm", L"dotx", L"rtf", L"epub", L"odt"})) return 0x2B579A;
    if (OneOf(ext, {L"ppt", L"pptx", L"pptm", L"odp"})) return 0xC43E1C;
    if (OneOf(ext, {L"txt", L"log", L"ini", L"cfg", L"conf", L"properties", L"lpm"})) return 0x64748B;
    if (OneOf(ext, {L"cpp", L"c", L"h", L"hpp", L"cc", L"cxx", L"cs", L"java", L"js", L"jsx",
                    L"ts", L"tsx", L"py", L"rs", L"go", L"php", L"html", L"htm", L"css", L"scss",
                    L"sql", L"ps1", L"bat", L"cmd", L"sh", L"qml", L"cmake", L"kt", L"swift",
                    L"rb", L"lua"}))
        return 0x2563EB;
    if (OneOf(ext, {L"zip", L"7z", L"rar", L"tar", L"tgz", L"gz", L"txz", L"xz", L"tbz2", L"bz2",
                    L"cab", L"iso", L"wim", L"swm", L"esd", L"001", L"zipx", L"vhd", L"vhdx",
                    L"vdi", L"vmdk", L"dmg", L"xar", L"rpm", L"deb", L"arj", L"lzh", L"lha",
                    L"chm", L"udf", L"zst"}))
        return 0xEA580C;
    if (OneOf(ext, {L"mp3", L"wav", L"flac", L"m4a", L"aac", L"wma", L"ogg", L"oga", L"opus",
                    L"aif", L"aiff", L"ape", L"wv", L"tta", L"dsf", L"dff", L"mka", L"ac3",
                    L"dts", L"amr", L"caf", L"mpc"}))
        return 0x0891B2;
    if (OneOf(ext, {L"mp4", L"m4v", L"mov", L"wmv", L"avi", L"mkv", L"webm", L"mpg", L"mpeg",
                    L"m2ts", L"mts", L"3gp", L"flv", L"f4v", L"m2v", L"m2t", L"vob", L"3g2",
                    L"asf", L"ogv", L"rm", L"rmvb", L"mxf", L"divx", L"dv", L"y4m", L"mjpeg",
                    L"hevc", L"h264", L"264", L"265"}))
        return 0x9333EA;
    if (OneOf(ext, {L"ttf", L"otf", L"ttc", L"otc", L"woff", L"woff2"})) return 0x0F766E;
    return 0x475569;
}

bool IsHostExecutable(const wchar_t* exe) {
    const wchar_t* name = PathFindFileNameW(exe);
    return !_wcsicmp(name, L"OpenWith.exe") || !_wcsicmp(name, L"rundll32.exe") ||
           !_wcsicmp(name, L"explorer.exe") || !_wcsicmp(name, L"cmd.exe");
}

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

}  // namespace

// Full screen hides the header and the footer; Esc or F brings them back.
float QuickPreviewWindow::HeaderHeight() const noexcept { return (embedded_ || fullscreen_) ? 0.0f : kHeaderHeight * scale_; }
float QuickPreviewWindow::FooterHeight() const noexcept { return fullscreen_ || (embedded_ && video_.active()) ? 0.0f : (embedded_ ? 26.0f : kFooterHeight) * scale_; }

D2D1_RECT_F QuickPreviewWindow::FooterRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    return D2D1::RectF(0, height - FooterHeight(), width, height);
}

void QuickPreviewWindow::SetAccent(const D2D1_COLOR_F& accent) {
    accent_ = accent;
    accent_.a = 1.0f;
    if (hwnd_ && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
}

QuickPreviewWindow::ChromePalette QuickPreviewWindow::Palette() const {
    ChromePalette p;
    p.high_contrast = HighContrast();
    if (p.high_contrast) {
        p.bg = ColorFromRef(GetSysColor(COLOR_WINDOW));
        p.ink = p.ink2 = p.ink3 = p.line = ColorFromRef(GetSysColor(COLOR_WINDOWTEXT));
        p.hover = ColorFromRef(GetSysColor(COLOR_HIGHLIGHT));
        p.on_hover = ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT));
        p.accent = p.hover;
        p.on_accent = p.on_hover;
        return p;
    }
    p.bg = dark_ ? HexColor(0x151515) : HexColor(0xF7F7F7);
    p.ink = dark_ ? HexColor(0xF4F4F4) : HexColor(0x202020);
    p.ink2 = dark_ ? HexColor(0xA8A8A8) : HexColor(0x5F5F5F);
    p.ink3 = dark_ ? HexColor(0x767676) : HexColor(0x9A9A9A);
    p.line = dark_ ? HexColor(0xFFFFFF, 0.09f) : HexColor(0x000000, 0.09f);
    p.hover = dark_ ? HexColor(0xFFFFFF, 0.08f) : HexColor(0x000000, 0.06f);
    p.on_hover = p.ink;
    p.accent = accent_;
    p.on_accent = AutoAccentText(accent_);
    return p;
}

// Resolved once per item: the shell type name for the meta line and the
// default program for the open button.
void QuickPreviewWindow::RefreshItemFacts() {
    type_name_.clear();
    open_app_.clear();
    const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const std::wstring ext = folder ? std::wstring() : LowerExtension(item_.path);
    {
        using pulse::preview::PreviewFamily;
        const std::wstring dotted = L"." + ext;
        image_item_ = !folder && !ext.empty() &&
            (pulse::preview::IsImageExtension(dotted) ||
             pulse::preview::IsFamilyExtension(PreviewFamily::Psd, dotted) ||
             pulse::preview::IsFamilyExtension(PreviewFamily::Raw, dotted));
    }
    // A bare probe name: the registry answers, the disk is never touched.
    const std::wstring probe = folder ? std::wstring(L"folder") : L"file." + ext;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                       &info, sizeof(info), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        type_name_ = info.szTypeName;
    if (type_name_.empty()) {
        std::wstring upper = ext;
        for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
        type_name_ = folder ? std::wstring(pulse::l10n::Get(pulse::l10n::StringId::TypeFolder))
            : upper.empty() ? std::wstring(pulse::l10n::Get(pulse::l10n::StringId::TypeFile))
                            : upper + pulse::l10n::Get(pulse::l10n::StringId::File);
    }
    if (folder || ext.empty()) return;
    if (const auto it = open_app_cache_.find(ext); it != open_app_cache_.end()) {
        open_app_ = it->second;
        return;
    }
    const std::wstring dotted = L"." + ext;
    wchar_t exe[MAX_PATH * 2]{};
    DWORD length = ARRAYSIZE(exe);
    const bool has_exe = SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
                                                     dotted.c_str(), nullptr, exe, &length));
    if (!(has_exe && IsHostExecutable(exe))) {
        wchar_t name[256]{};
        length = ARRAYSIZE(name);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME,
                                        dotted.c_str(), nullptr, name, &length)))
            open_app_ = ShortAppName(name);
    }
    open_app_cache_.emplace(ext, open_app_);
}

std::wstring QuickPreviewWindow::OpenLabel() const {
    using pulse::l10n::StringId;
    if (item_.attrs & FILE_ATTRIBUTE_DIRECTORY)
        return command_message_ ? pulse::l10n::Get(StringId::OpenNewTab) : pulse::l10n::Get(StringId::Open);
    if (open_app_.empty()) return pulse::l10n::Get(StringId::ShellOpenWith);
    return Fill(pulse::l10n::Get(StringId::ShellOpenApp), L"{app}", open_app_);
}

std::wstring QuickPreviewWindow::MetaLine() const {
    std::wstring line = type_name_;
    const auto add = [&line](const std::wstring& part) {
        if (part.empty()) return;
        if (!line.empty()) line += L" \x00B7 ";
        line += part;
    };
    if (!(item_.attrs & FILE_ATTRIBUTE_DIRECTORY)) add(pulse::format::ByteSize(item_.size, true));
    if (item_.modified) {
        FILETIME time{static_cast<DWORD>(item_.modified & 0xFFFFFFFFull),
                      static_cast<DWORD>(item_.modified >> 32)};
        add(pulse::format::LocalFileTime(time));
    }
    return line;
}

ComPtr<IDWriteTextLayout> QuickPreviewWindow::UiLayout(std::wstring_view text,
                                                     IDWriteTextFormat* format, float width,
                                                     float height,
                                                     DWRITE_TEXT_ALIGNMENT alignment) const {
    ComPtr<IDWriteTextLayout> layout;
    if (!format || !compositor_.DwriteFactory() || text.empty()) return layout;
    const auto generation = typography::Generation();
    if (ui_layout_generation_ != generation) {
        ui_layout_cache_.clear();
        ui_trimming_cache_.clear();
        ui_layout_text_bytes_ = 0;
        ui_layout_generation_ = generation;
    }
    width = (std::max)(1.0f, width);
    height = (std::max)(1.0f, height);
    for (auto it = ui_layout_cache_.begin(); it != ui_layout_cache_.end(); ++it) {
        if (it->text != text || it->format.get() != format || it->width != width ||
            it->height != height || it->alignment != alignment) continue;
        layout.p = it->layout.get();
        ++ui_layout_hits_;
        layout->AddRef();
        auto entry = std::move(*it);
        ui_layout_cache_.erase(it);
        ui_layout_cache_.push_back(std::move(entry));
        return layout;
    }
    if (FAILED(compositor_.DwriteFactory()->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), format, (std::max)(1.0f, width),
            (std::max)(1.0f, height), &layout)) || !layout.get())
        return layout;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ++ui_layout_creations_;
    layout->SetTextAlignment(alignment);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    for (const auto& entry : ui_trimming_cache_) {
        if (entry.format.get() == format) { sign = entry.sign.get(); break; }
    }
    if (!sign) {
        UiTrimmingEntry entry;
        if (SUCCEEDED(compositor_.DwriteFactory()->CreateEllipsisTrimmingSign(format, &entry.sign))) {
            ++ui_sign_creations_;
            entry.format.p = format;
            format->AddRef();
            if (ui_trimming_cache_.size() >= 16) ui_trimming_cache_.pop_front();
            ui_trimming_cache_.push_back(std::move(entry));
            sign = ui_trimming_cache_.back().sign.get();
        }
    }
    if (sign) layout->SetTrimming(&trimming, sign);
    constexpr size_t budget = 128 * 1024;
    if (text.size() <= budget / sizeof(wchar_t)) {
        const size_t bytes = text.size() * sizeof(wchar_t);
        while (!ui_layout_cache_.empty() &&
               (ui_layout_cache_.size() >= 64 || ui_layout_text_bytes_ + bytes > budget)) {
            ui_layout_text_bytes_ -= ui_layout_cache_.front().text.size() * sizeof(wchar_t);
            ui_layout_cache_.pop_front();
        }
        UiLayoutEntry entry;
        entry.text = text;
        entry.format.p = format;
        format->AddRef();
        entry.width = width;
        entry.height = height;
        entry.alignment = alignment;
        entry.layout.p = layout.get();
        layout->AddRef();
        ui_layout_cache_.push_back(std::move(entry));
        ui_layout_text_bytes_ += bytes;
    }
    return layout;
}

HRESULT QuickPreviewWindow::CreateUiBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& color,
                                         ID2D1SolidColorBrush** result) {
    *result = nullptr;
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (device.get() != ui_brush_device_.get()) {
        ui_brush_cache_.clear();
        ui_brush_device_ = std::move(device);
    }
    for (auto& entry : ui_brush_cache_) {
        if (entry.color.r == color.r && entry.color.g == color.g &&
            entry.color.b == color.b && entry.color.a == color.a) {
            *result = entry.brush.get();
            (*result)->AddRef();
            return S_OK;
        }
    }
    UiBrushEntry entry;
    const HRESULT hr = dc->CreateSolidColorBrush(color, &entry.brush);
    if (FAILED(hr)) return hr;
    ++ui_brush_creations_;
    entry.color = color;
    *result = entry.brush.get();
    (*result)->AddRef();
    if (ui_brush_cache_.size() >= 32) ui_brush_cache_.pop_front();
    ui_brush_cache_.push_back(std::move(entry));
    return hr;
}

float QuickPreviewWindow::UiTextWidth(std::wstring_view text, IDWriteTextFormat* format) const {
    const ComPtr<IDWriteTextLayout> layout = UiLayout(text, format, 4000.0f, 100.0f);
    DWRITE_TEXT_METRICS metrics{};
    if (!layout.get() || FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

void QuickPreviewWindow::DrawUiText(ID2D1DeviceContext* dc, std::wstring_view text,
                                    IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                                    const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT alignment) {
    if (!dc || rect.right <= rect.left) return;
    const ComPtr<IDWriteTextLayout> layout =
        UiLayout(text, format, rect.right - rect.left, rect.bottom - rect.top, alignment);
    if (!layout.get()) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    CreateUiBrush(dc, color, &brush);
    if (!brush.get()) return;
    compositor_.DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), brush.get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void QuickPreviewWindow::ToggleLabels(const wchar_t* (&labels)[2]) const {
    using pulse::l10n::StringId;
    labels[0] = pulse::l10n::Get(StringId::Rendered).c_str();
    labels[1] = pulse::l10n::Get(StringId::Source).c_str();
    if (toggle_kind_ == ToggleKind::TableSource || toggle_kind_ == ToggleKind::TableHandler)
        labels[0] = pulse::l10n::Get(StringId::Table).c_str();
    else if (toggle_kind_ == ToggleKind::TreeSource)
        labels[0] = pulse::l10n::Get(StringId::Tree).c_str();
    else if (toggle_kind_ == ToggleKind::NotebookSource)
        labels[0] = pulse::l10n::Get(StringId::Notebook).c_str();
    else if (toggle_kind_ == ToggleKind::DocHandler)
        labels[0] = pulse::l10n::Get(StringId::Document).c_str();
    else if (toggle_kind_ == ToggleKind::TextReading) {
        labels[0] = pulse::l10n::Get(StringId::PreviewReadingMode).c_str();
        labels[1] = pulse::l10n::Get(StringId::PreviewMonospaceMode).c_str();
    }
    if (toggle_kind_ == ToggleKind::TableHandler || toggle_kind_ == ToggleKind::DocHandler)
        labels[1] = pulse::l10n::Get(StringId::System).c_str();
}

float QuickPreviewWindow::ToggleSegmentWidth(int segment) const {
    const wchar_t* labels[2]{};
    ToggleLabels(labels);
    IDWriteTextFormat* format = compositor_.SmallFormat();
    return (std::max)(40.0f * scale_, UiTextWidth(labels[segment & 1], format) + 22.0f * scale_);
}

void QuickPreviewWindow::LayoutHeader() {
    if (embedded_) { header_ = {}; return; }
    const float s = scale_;
    const float width = static_cast<float>(compositor_.Width());
    const float header = HeaderHeight();
    const float cy = header * 0.5f;
    HeaderLayout h;
    h.close = D2D1::RectF(width - kCloseWidth * s, 0, width, header);
    h.counter_text.clear();
    if (item_.count > 0 && item_.position > 0)
        h.counter_text = std::to_wstring(item_.position) + L" / " + std::to_wstring(item_.count);
    const float button = kIconButton * s;
    const float counter_w = h.counter_text.empty() ? 0.0f
        : (std::max)(44.0f * s, UiTextWidth(h.counter_text, compositor_.SmallFormat()) + 10.0f * s);
    h.open_label = OpenLabel();
    const float label_w = (std::min)(kOpenTextMax * s, UiTextWidth(h.open_label, open_format_.get()));
    const float open_full = 12.0f * s + 16.0f * s + 6.0f * s + label_w + 12.0f * s;
    const bool has_tools = markdown_shown_;
    const float tools_w = has_tools ? ToggleSegmentWidth(0) + ToggleSegmentWidth(1) + 4.0f * s : 0.0f;
    const float left_fixed = (kHeaderPadLeft + kBadgeSize + 10.0f) * s;
    const auto title_room = [&](bool tools, bool open_text, bool counter) {
        float right = kCloseWidth * s + 2.0f * s + (button + 4.0f * s) * 2.0f;  // close, more, size
        right += open_text ? open_full : button;                           // open
        right += 8.0f * s + button * 2.0f + (counter ? counter_w : 0.0f);  // nav
        if (tools) right += tools_w + 8.0f * s + 13.0f * s;                // pill + separator
        return width - left_fixed - right - 12.0f * s;
    };
    h.tools_visible = has_tools;
    h.open_text = true;
    h.counter_visible = !h.counter_text.empty();
    const float title_min = kTitleMin * s;
    float room = title_room(h.tools_visible, h.open_text, h.counter_visible);
    if (room < title_min && h.tools_visible) {
        h.tools_visible = false;
        room = title_room(false, h.open_text, h.counter_visible);
    }
    if (room < title_min) {
        h.open_text = false;
        room = title_room(false, false, h.counter_visible);
    }
    if (room < title_min && h.counter_visible) {
        h.counter_visible = false;
        room = title_room(false, false, false);
    }
    // A short meta line stays as long as it fits whole.
    const float meta_need = (std::min)(kMetaMin * s, UiTextWidth(MetaLine(), compositor_.SmallFormat()) + 4.0f * s);
    // The meta line folds first: once any control has folded it stays hidden.
    const bool nothing_folded = h.tools_visible == has_tools && h.open_text &&
                                h.counter_visible == !h.counter_text.empty();
    h.meta_visible = nothing_folded && room >= meta_need;
    // Right to left.
    float x = h.close.left - 2.0f * s;
    h.more = D2D1::RectF(x - button, cy - button * 0.5f, x, cy + button * 0.5f);
    x = h.more.left - 4.0f * s;
    h.size = D2D1::RectF(x - button, cy - button * 0.5f, x, cy + button * 0.5f);
    x = h.size.left - 4.0f * s;
    const float open_w = h.open_text ? open_full : button;
    h.open = D2D1::RectF(x - open_w, cy - button * 0.5f, x, cy + button * 0.5f);
    x = h.open.left - 8.0f * s;
    h.next = D2D1::RectF(x - button, cy - button * 0.5f, x, cy + button * 0.5f);
    x = h.next.left;
    if (h.counter_visible) {
        h.counter = D2D1::RectF(x - counter_w, 0, x, header);
        x = h.counter.left;
    }
    h.prev = D2D1::RectF(x - button, cy - button * 0.5f, x, cy + button * 0.5f);
    x = h.prev.left;
    if (h.tools_visible) {
        const float sx = std::round(x - 6.0f * s);
        h.separator = D2D1::RectF(sx, cy - 9.0f * s, sx + (std::max)(1.0f, std::round(s)), cy + 9.0f * s);
        x = h.separator.left - 6.0f * s;
        const float pill_h = 26.0f * s;
        h.tools = D2D1::RectF(x - tools_w, cy - pill_h * 0.5f, x, cy + pill_h * 0.5f);
        x = h.tools.left;
    }
    const float badge = kBadgeSize * s;
    h.badge = D2D1::RectF(kHeaderPadLeft * s, cy - badge * 0.5f, kHeaderPadLeft * s + badge, cy + badge * 0.5f);
    const float title_left = h.badge.right + 10.0f * s;
    const float title_right = (std::max)(title_left, x - 12.0f * s);
    if (h.meta_visible) {
        h.name = D2D1::RectF(title_left, cy - 19.0f * s, title_right, cy + 1.0f * s);
        h.meta = D2D1::RectF(title_left, cy + 1.0f * s, title_right, cy + 18.0f * s);
    } else {
        h.name = D2D1::RectF(title_left, 0, title_right, header);
    }
    header_ = std::move(h);
}

bool QuickPreviewWindow::HeaderInteractive(POINT client) const {
    if (HitChromeButton(client) != ChromeButton::None) return true;
    return HitMarkdownToggle(client) >= 0;
}

void QuickPreviewWindow::DrawHeader(ID2D1DeviceContext* dc, const ChromePalette& p) {
    if (embedded_) return;
    if (!dc) return;
    const float s = scale_;
    const float width = static_cast<float>(compositor_.Width());
    const float header = HeaderHeight();
    ComPtr<ID2D1SolidColorBrush> bg, hover, ink, dim;
    CreateUiBrush(dc, p.bg, &bg);
    CreateUiBrush(dc, p.hover, &hover);
    CreateUiBrush(dc, p.ink2, &ink);
    CreateUiBrush(dc, p.ink3, &dim);
    if (bg.get()) dc->FillRectangle(D2D1::RectF(0, 0, width, header), bg.get());
    if (p.high_contrast) {
        ComPtr<ID2D1SolidColorBrush> line;
        CreateUiBrush(dc, p.line, &line);
        if (line.get()) dc->FillRectangle(D2D1::RectF(0, header - 1.0f, width, header), line.get());
    }

    // Badge: the extension on the family colour (outlined in high contrast).
    const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const std::wstring ext = LowerExtension(item_.path);
    const float radius = 6.0f * s;
    const D2D1_ROUNDED_RECT badge{header_.badge, radius, radius};
    D2D1_COLOR_F badge_ink = HexColor(0xFFFFFF);
    if (p.high_contrast) {
        ComPtr<ID2D1SolidColorBrush> outline;
        CreateUiBrush(dc, p.ink, &outline);
        if (outline.get()) dc->DrawRoundedRectangle(badge, outline.get(), (std::max)(1.0f, s));
        badge_ink = p.ink;
    } else {
        ComPtr<ID2D1SolidColorBrush> fill;
        CreateUiBrush(dc, HexColor(BadgeRgb(ext, folder)), &fill);
        if (fill.get()) dc->FillRoundedRectangle(badge, fill.get());
    }
    if (folder) {
        ComPtr<ID2D1SolidColorBrush> glyph;
        CreateUiBrush(dc, badge_ink, &glyph);
        if (glyph.get())
            command_icons::DrawGlyph(dc, glyph.get(), kFolderGlyph, header_.badge, 14.0f * s);
    } else {
        std::wstring label = ext.empty() ? std::wstring(L"FILE") : ext.substr(0, 4);
        for (wchar_t& c : label) c = static_cast<wchar_t>(std::towupper(c));
        IDWriteTextFormat* format = label.size() > 3 && badge_small_format_.get()
            ? badge_small_format_.get() : badge_format_.get();
        DrawUiText(dc, label, format, header_.badge, badge_ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Name and meta.
    DrawUiText(dc, title_, compositor_.HeaderFormat(), header_.name, p.ink);
    if (header_.meta_visible) DrawUiText(dc, MetaLine(), compositor_.SmallFormat(), header_.meta, p.ink2);

    // Type tools: the view toggle (Rendered | Source, Table | System, ...).
    if (header_.tools_visible && markdown_shown_) {
        ComPtr<ID2D1SolidColorBrush> text;
        CreateUiBrush(dc, p.ink, &text);
        DrawMarkdownToggle(dc, text.get());
        ComPtr<ID2D1SolidColorBrush> line;
        CreateUiBrush(dc, p.high_contrast ? p.line : WithAlpha(p.ink, dark_ ? 0.16f : 0.14f), &line);
        if (line.get()) dc->FillRectangle(header_.separator, line.get());
    }

    // Icon buttons.
    IDWriteTextFormat* glyph_format = close_format_.get() ? close_format_.get() : compositor_.IconFormat();
    const float glyph_size = glyph_format ? glyph_format->GetFontSize() : kGlyphSize * s;
    const auto icon_button = [&](ChromeButton which, const D2D1_RECT_F& rect, const wchar_t* glyph,
                                 bool enabled) {
        const bool hot = chrome_hover_ == which;
        if (hot && hover.get())
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 7.0f * s, 7.0f * s), hover.get());
        ComPtr<ID2D1SolidColorBrush> brush;
        CreateUiBrush(dc, hot && p.high_contrast ? p.on_hover : enabled ? p.ink2 : p.ink3, &brush);
        if (!brush.get()) return;
        if (!command_icons::DrawGlyph(dc, brush.get(), glyph, rect, glyph_size))
            DrawLegacyIcon(dc, compositor_.DwriteFactory(), glyph, rect, brush.get());
    };
    const bool first = item_.count > 0 && item_.position <= 1;
    const bool last = item_.count > 0 && item_.position >= item_.count;
    icon_button(ChromeButton::Prev, header_.prev, kPrevGlyph, !first);
    if (header_.counter_visible)
        DrawUiText(dc, header_.counter_text, compositor_.SmallFormat(), header_.counter, p.ink2,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    icon_button(ChromeButton::Next, header_.next, kNextGlyph, !last);

    // Primary button: open with the default program.
    {
        const bool hot = chrome_hover_ == ChromeButton::Open;
        D2D1_COLOR_F fill = p.accent;
        if (hot && !p.high_contrast) {
            const float k = dark_ ? 1.10f : 0.90f;
            fill = D2D1::ColorF((std::min)(1.0f, fill.r * k), (std::min)(1.0f, fill.g * k),
                                (std::min)(1.0f, fill.b * k), 1.0f);
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        CreateUiBrush(dc, fill, &brush);
        const D2D1_ROUNDED_RECT shape{header_.open, 6.0f * s, 6.0f * s};
        if (brush.get()) dc->FillRoundedRectangle(shape, brush.get());
        if (p.high_contrast && hot) {
            ComPtr<ID2D1SolidColorBrush> ring;
            CreateUiBrush(dc, p.ink, &ring);
            if (ring.get()) dc->DrawRoundedRectangle(shape, ring.get(), 2.0f * s);
        }
        const float icon = 16.0f * s;
        const float cy = (header_.open.top + header_.open.bottom) * 0.5f;
        const float ix = header_.open_text ? header_.open.left + 12.0f * s
                                           : (header_.open.left + header_.open.right - icon) * 0.5f;
        const D2D1_RECT_F icon_rect = D2D1::RectF(ix, cy - icon * 0.5f, ix + icon, cy + icon * 0.5f);
        const OpenWithIconCache::Badge app_icon = folder || p.high_contrast
            ? OpenWithIconCache::Badge{} : open_icons_.BadgeFor(item_.name, static_cast<uint32_t>(std::lround(icon)));
        if (app_icon.bitmap) {
            dc->DrawBitmap(app_icon.bitmap, icon_rect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            ComPtr<ID2D1SolidColorBrush> glyph;
            CreateUiBrush(dc, p.on_accent, &glyph);
            if (glyph.get() && !command_icons::DrawGlyph(dc, glyph.get(), kOpenGlyph, icon_rect, 14.0f * s))
                DrawLegacyIcon(dc, compositor_.DwriteFactory(), kOpenGlyph, icon_rect, glyph.get());
        }
        if (header_.open_text)
            DrawUiText(dc, header_.open_label, open_format_.get(),
                       D2D1::RectF(icon_rect.right + 6.0f * s, header_.open.top,
                                   header_.open.right - 12.0f * s, header_.open.bottom),
                       p.on_accent);
    }
    icon_button(ChromeButton::Size, header_.size, kSizeGlyph, true);
    icon_button(ChromeButton::More, header_.more, kMoreGlyph, true);

    // Close.
    if (close_hover_) {
        ComPtr<ID2D1SolidColorBrush> red;
        CreateUiBrush(dc, p.high_contrast ? p.hover : HexColor(0xC42B1C), &red);
        if (red.get()) dc->FillRectangle(header_.close, red.get());
    }
    ComPtr<ID2D1SolidColorBrush> close_ink;
    CreateUiBrush(dc, close_hover_ ? (p.high_contrast ? p.on_hover : HexColor(0xFFFFFF)) : p.ink,
                              &close_ink);
    if (close_ink.get() && !command_icons::DrawGlyph(dc, close_ink.get(), kCloseGlyph, header_.close, glyph_size))
        DrawLegacyIcon(dc, compositor_.DwriteFactory(), kCloseGlyph, header_.close, close_ink.get());
}

void QuickPreviewWindow::SetFooterParts(std::vector<std::wstring> parts) {
    footer_parts_ = std::move(parts);
}

void QuickPreviewWindow::AddFooterControl(int id, std::wstring text) {
    FooterControl control;
    control.id = id;
    control.text = std::move(text);
    footer_controls_.push_back(std::move(control));
}

int QuickPreviewWindow::HitFooterControl(POINT client) const {
    for (const FooterControl& control : footer_controls_)
        if (Inside(control.rect, client)) return control.id;
    return 0;
}

bool QuickPreviewWindow::CycleFooterFocus(bool backwards) {
    if (footer_controls_.empty()) {
        footer_focus_ = 0;
        return false;
    }
    const int count = static_cast<int>(footer_controls_.size());
    int index = -1;
    for (int i = 0; i < count; ++i)
        if (footer_controls_[static_cast<size_t>(i)].id == footer_focus_) index = i;
    // Past either end the focus leaves the footer, like Tab leaving a toolbar.
    index = index < 0 ? (backwards ? count - 1 : 0) : index + (backwards ? -1 : 1);
    footer_focus_ = index >= 0 && index < count ? footer_controls_[static_cast<size_t>(index)].id : 0;
    return true;
}

void QuickPreviewWindow::ActivateFooterControl(int id) {
    if (id == kFooterZoom && native_kind_ == NativeKind::Bitmap) {
        ToggleFitActual();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterZoom && native_kind_ == NativeKind::Pages) {
        TogglePagesFit();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if ((id == kFooterZoomOut || id == kFooterZoomIn) && native_kind_ == NativeKind::Bitmap) {
        const D2D1_RECT_F content = ContentRect();
        ZoomAt((content.left + content.right) * 0.5f, (content.top + content.bottom) * 0.5f,
               id == kFooterZoomIn ? 1.25f : 0.8f);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterRotate && native_kind_ == NativeKind::Bitmap && frame_count_ <= 1) {
        image_turns_ = (image_turns_ + 1) & 3;
        const float x = pan_x_;
        pan_x_ = -pan_y_;
        pan_y_ = x;
        const D2D1_RECT_F content = ContentRect();
        if (!image_fit_ && image_zoom_ < FitScale(content.right - content.left, content.bottom - content.top))
            SetFitMode();
        ClampPan(content.right - content.left, content.bottom - content.top);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterImageBg && native_kind_ == NativeKind::Bitmap) {
        image_backdrop_ = (image_backdrop_ + 1) % 4;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterInfo && native_kind_ == NativeKind::Bitmap) {
        // The picture keeps its zoom; the view just gets narrower.
        info_open_ = !info_open_;
        const D2D1_RECT_F content = ContentRect();
        ClampPan(content.right - content.left, content.bottom - content.top);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if ((id == kFooterZoomOut || id == kFooterZoomIn) && native_kind_ == NativeKind::Pages) {
        const D2D1_RECT_F content = ContentRect();
        ZoomPages((content.left + content.right) * 0.5f, (content.top + content.bottom) * 0.5f,
                  id == kFooterZoomIn ? 1.25f : 0.8f);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterToc) {
        toc_hidden_ = !toc_hidden_;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterCopyPath && native_kind_ == NativeKind::Tree) {
        pulse::ops::WriteClipboardText(tree_.CurrentPath());
    } else if (id == kFooterFoldAll && native_kind_ == NativeKind::Tree) {
        tree_.ExpandAll(tree_.AnyCollapsed());
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kFooterReadWidth) {
        reading_width_ = (reading_width_ + 1) % 3;
        text_layout_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

float QuickPreviewWindow::InfoDrawerWidth() const noexcept {
    // Only beside a single picture, and only when the picture keeps room.
    if (!info_open_ || !image_item_ || pages_count_ > 1) return 0.0f;
    if (native_kind_ != NativeKind::Bitmap && native_kind_ != NativeKind::None) return 0.0f;
    const float width = 250.0f * scale_;
    return static_cast<float>(compositor_.Width()) >= width + 320.0f * scale_ ? std::round(width) : 0.0f;
}

void QuickPreviewWindow::DrawInfoDrawer(ID2D1DeviceContext* dc, const ChromePalette& p) {
    const float drawer_w = InfoDrawerWidth();
    if (!dc || drawer_w <= 0.0f || native_kind_ != NativeKind::Bitmap) return;
    using pulse::l10n::StringId;
    const float s = scale_;
    const D2D1_RECT_F content = ContentRect();
    const D2D1_RECT_F panel = D2D1::RectF(content.right, content.top,
                                          static_cast<float>(compositor_.Width()), content.bottom);
    ComPtr<ID2D1SolidColorBrush> bg, line;
    CreateUiBrush(dc, p.bg, &bg);
    CreateUiBrush(dc, p.line, &line);
    if (!bg.get() || !line.get()) return;
    dc->FillRectangle(panel, bg.get());
    dc->FillRectangle(D2D1::RectF(panel.left, panel.top, panel.left + (std::max)(1.0f, std::round(s)),
                                  panel.bottom), line.get());
    IDWriteTextFormat* fine = compositor_.SmallFormat();
    IDWriteTextFormat* body = compositor_.TextFormat();
    if (!fine || !body) return;
    dc->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_ALIASED);
    const float pad = 16.0f * s, left = panel.left + pad, right = panel.right - pad;
    float y = panel.top + 12.0f * s;
    DrawUiText(dc, pulse::l10n::Get(StringId::Information), body,
               D2D1::RectF(left, y, right, y + 28.0f * s), p.ink);
    y += 34.0f * s;

    // Rows: what Pulse knows itself, then the shell's photo properties.
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    const uint32_t shown_w = source_w_ ? source_w_ : decoded_w_;
    const uint32_t shown_h = source_h_ ? source_h_ : decoded_h_;
    if (shown_w && shown_h) {
        wchar_t dims[64];
        swprintf_s(dims, L"%u \x00D7 %u", shown_w, shown_h);
        rows.emplace_back(pulse::l10n::ServiceText(L"尺寸"), dims);
    }
    std::wstring ext = LowerExtension(item_.path);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    if (!ext.empty()) rows.emplace_back(pulse::l10n::Get(StringId::Format), ext);
    ImageStats stats;
    const bool has_stats = thumbnails_.Stats(item_.path, preview_pixels_, item_.modified, item_.size, stats);
    if (has_stats)
        rows.emplace_back(pulse::l10n::Get(StringId::PreviewInfoAlpha),
                          pulse::l10n::Get(stats.has_alpha ? StringId::PreviewInfoYes : StringId::PreviewInfoNo));
    std::vector<PreviewProperty> properties;
    thumbnails_.Properties(item_.path, item_.attrs, generation_, item_.modified, item_.size, properties);
    for (const PreviewProperty& property : properties)
        if (property.label != L"尺寸" && !property.value.empty())
            rows.emplace_back(pulse::l10n::ServiceText(property.label), property.value);
    rows.emplace_back(pulse::l10n::Get(StringId::ColumnSize), pulse::format::ByteSize(item_.size, true));
    if (item_.modified) {
        FILETIME time{static_cast<DWORD>(item_.modified & 0xFFFFFFFFull),
                      static_cast<DWORD>(item_.modified >> 32)};
        rows.emplace_back(pulse::l10n::Get(StringId::ModifiedTime), pulse::format::LocalFileTime(time));
    }
    float label_w = 0.0f;
    for (const auto& row : rows) label_w = (std::max)(label_w, UiTextWidth(row.first, fine));
    label_w = (std::min)(label_w, (right - left) * 0.45f);
    const float row_h = 26.0f * s;
    for (const auto& row : rows) {
        DrawUiText(dc, row.first, fine, D2D1::RectF(left, y, left + label_w, y + row_h), p.ink3);
        DrawUiText(dc, row.second, fine, D2D1::RectF(left + label_w + 10.0f * s, y, right, y + row_h),
                   p.ink, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += row_h;
    }

    // Histogram: the three channels as translucent areas over a hairline frame.
    if (!has_stats) {
        dc->PopAxisAlignedClip();
        return;
    }
    y += 14.0f * s;
    DrawUiText(dc, pulse::l10n::Get(StringId::PreviewInfoHistogram), fine,
               D2D1::RectF(left, y, right, y + 22.0f * s), p.ink3);
    y += 26.0f * s;
    const float chart_h = 86.0f * s;
    const D2D1_RECT_F chart = D2D1::RectF(left, y, right, y + chart_h);
    if (chart.bottom > panel.bottom - 8.0f * s) {
        dc->PopAxisAlignedClip();
        return;
    }
    dc->DrawRoundedRectangle(D2D1::RoundedRect(chart, 4.0f * s, 4.0f * s), line.get(), (std::max)(1.0f, s));
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    const D2D1_RECT_F plot = D2D1::RectF(chart.left + 4.0f * s, chart.top + 6.0f * s,
                                         chart.right - 4.0f * s, chart.bottom - 2.0f * s);
    const float alpha = p.high_contrast ? 0.9f : (dark_ ? 0.55f : 0.45f);
    const struct { const uint16_t* bins; uint32_t color; } channels[] = {
        {stats.r, 0xE5484Du}, {stats.g, 0x30A46Cu}, {stats.b, 0x3E7BFAu}};
    for (const auto& channel : channels) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (!factory.get() || FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) break;
        sink->BeginFigure(D2D1::Point2F(plot.left, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
        const float step = (plot.right - plot.left) / 63.0f;
        for (int i = 0; i < 64; ++i) {
            const float v = static_cast<float>(channel.bins[i]) / 1000.0f;
            sink->AddLine(D2D1::Point2F(plot.left + step * static_cast<float>(i),
                                        plot.bottom - v * (plot.bottom - plot.top)));
        }
        sink->AddLine(D2D1::Point2F(plot.right, plot.bottom));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) break;
        ComPtr<ID2D1SolidColorBrush> fill;
        CreateUiBrush(dc, D2D1::ColorF(channel.color, alpha), &fill);
        if (fill.get()) dc->FillGeometry(path.get(), fill.get());
    }
    dc->PopAxisAlignedClip();
}

void QuickPreviewWindow::DrawFooter(ID2D1DeviceContext* dc, const ChromePalette& p) {
    if (!dc || FooterHeight() <= 0) {
        for (auto& control : footer_controls_) control.rect = {};
        return;
    }
    const float s = scale_;
    const D2D1_RECT_F bar = FooterRect();
    ComPtr<ID2D1SolidColorBrush> bg, line;
    CreateUiBrush(dc, p.bg, &bg);
    CreateUiBrush(dc, p.line, &line);
    if (bg.get()) dc->FillRectangle(bar, bg.get());
    if (line.get()) dc->FillRectangle(D2D1::RectF(bar.left, bar.top, bar.right, bar.top + (std::max)(1.0f, std::round(s))), line.get());
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!format) return;
    // Right: type controls, laid out right to left.
    float right = bar.right - 8.0f * s;
    for (auto it = footer_controls_.rbegin(); it != footer_controls_.rend(); ++it) {
        const float w = UiTextWidth(it->text, format) + 14.0f * s;
        it->rect = D2D1::RectF(right - w, bar.top + 4.0f * s, right, bar.bottom - 4.0f * s);
        right = it->rect.left - 4.0f * s;
        const bool hot = footer_hover_ == it->id;
        if (hot || it->on) {
            ComPtr<ID2D1SolidColorBrush> hover;
            CreateUiBrush(dc, p.hover, &hover);
            if (hover.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(it->rect, 6.0f * s, 6.0f * s), hover.get());
        }
        DrawUiText(dc, it->text, format, it->rect, hot && p.high_contrast ? p.on_hover : p.ink2,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        if (footer_focus_ == it->id) {
            // Keyboard focus: a 2 px accent ring just outside the chip.
            ComPtr<ID2D1SolidColorBrush> ring;
            CreateUiBrush(dc, p.accent, &ring);
            const float stroke = (std::max)(1.0f, std::round(2.0f * s));
            const D2D1_RECT_F r = D2D1::RectF(it->rect.left - stroke * 0.5f, it->rect.top - stroke * 0.5f,
                                              it->rect.right + stroke * 0.5f, it->rect.bottom + stroke * 0.5f);
            if (ring.get()) dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 7.0f * s, 7.0f * s), ring.get(), stroke);
        }
    }
    // Then the passive keyboard hint, if there is room for it.
    if (!footer_hint_.empty()) {
        const float w = UiTextWidth(footer_hint_, format);
        if (right - w > bar.left + 360.0f * s) {
            DrawUiText(dc, footer_hint_, format, D2D1::RectF(right - w, bar.top, right, bar.bottom), p.ink3,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
            right -= w + 12.0f * s;
        }
    }
    // Left: facts joined by a tertiary middle dot.
    std::vector<std::wstring> parts = footer_parts_;
    if (parts.empty()) {
        const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring ext = LowerExtension(item_.path);
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
        if (folder || ext.empty()) { if (!type_name_.empty()) parts.push_back(type_name_); }
        else parts.push_back(ext);
        if (!folder && item_.size) parts.push_back(pulse::format::ByteSize(item_.size, true));
    }
    std::wstring label;
    std::vector<UINT32> dots;
    for (const std::wstring& part : parts) {
        if (part.empty()) continue;
        if (!label.empty()) {
            dots.push_back(static_cast<UINT32>(label.size() + 1));
            label += L"  \x00B7  ";
        }
        label += part;
    }
    float left = bar.left + 12.0f * s;
    if (native_kind_ == NativeKind::Table && table_.IsSpreadsheet() && table_.HasData()) {
        // Workbook: sheet tabs first, then the facts.
        const float limit = bar.left + (std::max)(180.0f * s, (right - bar.left) * 0.6f);
        const float used = table_.DrawTabsIn(dc, compositor_.DwriteFactory(),
            D2D1::RectF(bar.left + 2.0f * s, bar.top + 1.0f * s, limit, bar.bottom), CurrentTheme(), dark_, false);
        left = used + 12.0f * s;
    }
    if (label.empty() || right - left < 24.0f * s) return;
    const ComPtr<IDWriteTextLayout> layout = UiLayout(label, format, right - 8.0f * s - left, bar.bottom - bar.top);
    if (!layout.get()) return;
    ComPtr<ID2D1SolidColorBrush> ink, dim;
    CreateUiBrush(dc, p.ink2, &ink);
    CreateUiBrush(dc, p.ink3, &dim);
    if (dim.get())
        for (const UINT32 at : dots) layout->SetDrawingEffect(dim.get(), DWRITE_TEXT_RANGE{at + 1, 1});
    if (ink.get())
        compositor_.DrawTextLayout(D2D1::Point2F(left, bar.top), layout.get(), ink.get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace pulse::ui
