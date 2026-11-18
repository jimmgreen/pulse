#include "folder_thumbnail.h"
#include "shell_bitmap.h"
#include "shared_image_file.h"
#include "../fs/cloud_state.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdio>

namespace pulse::preview {
namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr DWORD kSkip = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
    FILE_ATTRIBUTE_REPARSE_POINT | ::pulse::fs::kCloudPlaceholderAttrs;
constexpr uint64_t kCacheLimit = 64ull * 1024 * 1024;
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
thread_local DWORD test_budget_ms = 450;
thread_local unsigned test_completed_pictures = ~0u;
#endif
struct Entry { std::wstring name; WIN32_FIND_DATAW data{}; int rank = 2; };
struct Picture { std::vector<uint8_t> data; UINT width = 0, height = 0; std::wstring label; };
struct DiagnosticTrace;
thread_local DiagnosticTrace* active_trace = nullptr;
struct DiagnosticTrace {
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER start{}, frequency{};
    DiagnosticTrace* previous = active_trace;
    DiagnosticTrace() {
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_BACKEND_TRACE", path, 32768);
        if (length && length < 32768) {
            file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        }
        active_trace = this;
    }
    ~DiagnosticTrace() {
        Log("request_end"); active_trace = previous;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void Log(const char* stage, uint64_t detail = 0) const {
        if (file == INVALID_HANDLE_VALUE || !frequency.QuadPart) return;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        const double ms = double(now.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
        char line[192];
        const int count = std::snprintf(line, sizeof(line), "pid=%lu request=%lld ms=%.3f stage=%s detail=%llu\r\n",
            GetCurrentProcessId(), start.QuadPart, ms, stage, static_cast<unsigned long long>(detail));
        if (count > 0 && count < static_cast<int>(sizeof(line))) {
            DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
        }
    }
};
void Trace(const char* stage, uint64_t detail = 0) { if (active_trace) active_trace->Log(stage, detail); }
struct GdiSession {
    ULONG_PTR token = 0;
    GdiSession() {
        Trace("gdi_start_begin");
        Gdiplus::GdiplusStartupInput input; const auto status = Gdiplus::GdiplusStartup(&token, &input, nullptr);
        Trace("gdi_start_end", status);
    }
    ~GdiSession() {
        Trace("gdi_shutdown_begin"); if (token) Gdiplus::GdiplusShutdown(token); Trace("gdi_shutdown_end");
    }
};
struct FindHandle { HANDLE value; ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); } };
uint64_t HashBytes(uint64_t hash, const void* bytes, size_t size) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}
uint64_t HashText(uint64_t hash, const std::wstring& text) {
    return HashBytes(hash, text.data(), text.size() * sizeof(wchar_t));
}
std::wstring Extension(const std::wstring& path) {
    auto pos = path.find_last_of(L'.');
    std::wstring ext = pos == std::wstring::npos ? L"" : path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return ext;
}
bool InList(const std::wstring& ext, const wchar_t* list) {
    return !ext.empty() && std::wstring(list).find(ext + L";") != std::wstring::npos;
}
int Rank(const std::wstring& name) {
    const auto ext = Extension(name);
    if (InList(ext, L".jpg;.jpeg;.png;.bmp;.gif;.webp;.tif;.tiff;.heic;.heif;.avif;.ico;")) return 0;
    if (InList(ext, L".mp4;.mkv;.avi;.mov;.wmv;.webm;.m4v;.mpeg;.mpg;")) return 1;
    return 2;
}
bool LocalFolder(std::wstring path, WIN32_FILE_ATTRIBUTE_DATA& info) {
    const std::wstring original = path;
    if (path.starts_with(L"\\\\?\\UNC\\")) return false;
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    if (path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return false;
    const std::wstring root = path.substr(0, 3);
    const UINT type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) return false;
    // Reject ancestor junctions as well: a local-looking path can lead to a share.
    for (size_t end = 3; ; ) {
        const auto prefix = std::wstring(L"\\\\?\\") + path.substr(0, end);
        const DWORD attrs = GetFileAttributesW(prefix.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | ::pulse::fs::kCloudPlaceholderAttrs))) return false;
        if (end >= path.size()) break;
        end = path.find_first_of(L"\\/", end + 1);
        if (end == std::wstring::npos) end = path.size();
    }
    return GetFileAttributesExW(original.c_str(), GetFileExInfoStandard, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool Pixels(IWICImagingFactory* factory, IWICBitmapSource* source, UINT cap, Picture& out) {
    Trace("pixels_begin");
    UINT w = 0, h = 0;
    if (FAILED(source->GetSize(&w, &h)) || !w || !h || uint64_t(w) * h > 64000000) return false;
    const double scale = std::min(1.0, double(cap) / std::max(w, h));
    out.width = std::max(1u, UINT(w * scale)); out.height = std::max(1u, UINT(h * scale));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(source,
        out.width, out.height, WICBitmapInterpolationModeFant))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(),
        GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    out.data.resize(size_t(out.width) * out.height * 4);
    Trace("copy_pixels_begin", uint64_t(w) * h);
    const HRESULT copied = converter->CopyPixels(nullptr, out.width * 4, static_cast<UINT>(out.data.size()), out.data.data());
    Trace("copy_pixels_end", static_cast<uint32_t>(copied));
    return SUCCEEDED(copied);
}
bool CachedPicture(IWICImagingFactory* factory, const std::wstring& path, UINT cap, Picture& out, bool extract = false, bool force = false) {
    ComPtr<IShellItem> item;
    ComPtr<IThumbnailCache> cache;
    Trace("shell_item_begin");
    const HRESULT item_status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    Trace("shell_item_end", static_cast<uint32_t>(item_status));
    if (FAILED(item_status)) return false;
    Trace("shell_cache_create_begin");
    const HRESULT cache_status = CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache));
    Trace("shell_cache_create_end", static_cast<uint32_t>(cache_status));
    if (FAILED(cache_status)) return false;
    ComPtr<ISharedBitmap> shared;
    WTS_CACHEFLAGS flags{}; WTS_THUMBNAILID id{};
    Trace("shell_get_begin", force ? 2 : extract ? 1 : 0);
    const HRESULT thumbnail_status = cache->GetThumbnail(item.Get(), cap,
        force ? WTS_FORCEEXTRACTION : (extract ? WTS_EXTRACT : WTS_INCACHEONLY), &shared, &flags, &id);
    Trace("shell_get_end", static_cast<uint32_t>(thumbnail_status));
    if (FAILED(thumbnail_status) || !shared) return false;
    HBITMAP bitmap = nullptr;
    if (FAILED(shared->GetSharedBitmap(&bitmap)) || !bitmap) return false;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    if (FAILED(shared->GetFormat(&alpha))) alpha = WTSAT_UNKNOWN;
    // Straight-alpha cache entries labelled ARGB were read as premultiplied
    // (washed-out pictures); shell_bitmap.h settles what the alpha means.
    std::vector<uint8_t> pbgra;
    UINT width = 0, height = 0, stride = 0;
    if (!ShellBitmapToPbgra(bitmap, alpha, pbgra, width, height, stride)) return false;
    ComPtr<IWICBitmap> source;
    return SUCCEEDED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
               stride, static_cast<UINT>(pbgra.size()), pbgra.data(), &source)) &&
        Pixels(factory, source.Get(), cap, out);
}
bool ReadPicture(IWICImagingFactory* factory, const std::wstring& path, UINT cap, Picture& out) {
    pulse::preview::SharedImageFile file;  // outlives the decoder
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    Trace("wic_decoder_begin");
    const HRESULT decoded = pulse::preview::CreateSharedDecoder(factory, path,
        WICDecodeMetadataCacheOnDemand, file, &decoder);
    Trace("wic_decoder_end", static_cast<uint32_t>(decoded));
    if (FAILED(decoded)) return false;
    Trace("wic_frame_begin");
    const HRESULT framed = decoder->GetFrame(0, &frame);
    Trace("wic_frame_end", static_cast<uint32_t>(framed));
    if (FAILED(framed)) return false;
    ComPtr<IWICBitmapSource> thumbnail;
    if (SUCCEEDED(frame->GetThumbnail(&thumbnail)) && Pixels(factory, thumbnail.Get(), cap, out)) return true;
    return Pixels(factory, frame.Get(), cap, out);
}
fs::path CacheDirectory() {
    wchar_t root[32768];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    if (!count || count >= std::size(root)) return {};
    return fs::path(root) / L"Pulse" / L"FolderThumbnails";
}
struct CacheHeader { uint64_t magic = 0x32544d4854465055ull, signature = 0, created = 0; uint32_t size = 0, bytes = 0; };
uint64_t Now() { FILETIME time{}; GetSystemTimeAsFileTime(&time); return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
bool LoadCache(const fs::path& file, uint64_t signature, UINT cap, std::vector<uint8_t>& out, bool& negative) {
    std::ifstream stream(file, std::ios::binary); CacheHeader h;
    if (!stream.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != CacheHeader{}.magic ||
        h.signature != signature || h.size != cap || (h.bytes && h.bytes != cap * cap * 4)) return false;
    if (!h.bytes) { negative = true; return Now() >= h.created && Now() - h.created < 10ull * 10000000; }
    out.resize(h.bytes);
    return bool(stream.read(reinterpret_cast<char*>(out.data()), h.bytes));
}
void SaveCache(const fs::path& file, uint64_t signature, UINT cap, const std::vector<uint8_t>& out) {
    Trace("cache_save_begin", out.size());
    std::error_code ec; fs::create_directories(file.parent_path(), ec); if (ec) return;
    auto temporary = file; temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    CacheHeader h; h.signature = signature; h.created = Now(); h.size = cap; h.bytes = static_cast<uint32_t>(out.size());
    { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char*>(&h), sizeof(h));
      if (!out.empty()) stream.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
      if (!stream) { stream.close(); fs::remove(temporary, ec); return; } }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) fs::remove(temporary, ec);
    Trace("cache_write_end");
    struct Old { fs::path path; fs::file_time_type time; uint64_t size; };
    std::vector<Old> files; uint64_t total = 0;
    for (fs::directory_iterator it(file.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != L".ftc" && it->path().extension() != L".tmp") continue;
        const auto size = it->file_size(ec); if (ec) break;
        const auto time = it->last_write_time(ec); if (ec) break;
        total += size; files.push_back({it->path(), time, size});
    }
    std::sort(files.begin(), files.end(), [](const Old& a, const Old& b) { return a.time < b.time; });
    size_t remaining = files.size();
    for (const auto& old : files) {
        if (total <= kCacheLimit && remaining <= 2048) break;
        if (fs::remove(old.path, ec)) { total -= old.size; --remaining; }
    }
    Trace("cache_save_end", files.size());
}
void Rounded(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    const float d = radius * 2;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
}
void TypeLabel(Gdiplus::Graphics& g, const std::wstring& label, float width, const Gdiplus::Color& color) {
    // GDI+ named-font construction enumerates fonts on a fresh host. Render this
    // tiny opaque header with GDI grayscale antialiasing, then composite normally.
    struct LabelSurface {
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = nullptr;
        HFONT font = nullptr;
        HGDIOBJ old_bitmap = nullptr, old_font = nullptr;
        ~LabelSurface() {
            if (old_font) SelectObject(dc, old_font);
            if (old_bitmap) SelectObject(dc, old_bitmap);
            if (font) DeleteObject(font);
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
        }
    } surface;
    if (!surface.dc) return;
    const INT bitmap_width = static_cast<INT>(width * 2);
    constexpr INT bitmap_height = 60;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap_width; info.bmiHeader.biHeight = -bitmap_height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* memory = nullptr;
    surface.bitmap = CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    if (!surface.bitmap || !memory) return;
    surface.old_bitmap = SelectObject(surface.dc, surface.bitmap);
    auto* pixels = static_cast<uint32_t*>(memory);
    const uint32_t background = 0xff000000u | (uint32_t(color.GetR()) << 16) |
        (uint32_t(color.GetG()) << 8) | color.GetB();
    std::fill_n(pixels, size_t(bitmap_width) * bitmap_height, background);
    Trace("font_begin");
    surface.font = CreateFontW(-32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    Trace("font_end", surface.font != nullptr);
    if (!surface.font) return;
    surface.old_font = SelectObject(surface.dc, surface.font);
    SetBkMode(surface.dc, TRANSPARENT); SetTextColor(surface.dc, RGB(255, 255, 255));
    RECT rect{14, 10, bitmap_width, bitmap_height};
    Trace("text_begin");
    DrawTextW(surface.dc, label.c_str(), static_cast<int>(label.size()), &rect, DT_SINGLELINE | DT_TOP | DT_NOPREFIX);
    GdiFlush();
    Trace("text_end");
    // GDI does not preserve alpha. This is an opaque colored header, so every
    // pixel must have full alpha, including the grayscale-antialiased glyphs.
    for (size_t i = 0; i < size_t(bitmap_width) * bitmap_height; ++i) pixels[i] |= 0xff000000u;
    Gdiplus::Bitmap image(bitmap_width, bitmap_height, bitmap_width * 4, PixelFormat32bppPARGB,
        static_cast<BYTE*>(memory));
    g.DrawImage(&image, Gdiplus::RectF(3, 3, width, 30), 0.0f, 0.0f,
        static_cast<float>(bitmap_width), static_cast<float>(bitmap_height), Gdiplus::UnitPixel);
}
void Card(Gdiplus::Graphics& g, Picture& pic, float x, float y, float w, float h, float angle) {
    const auto state = g.Save();
    g.TranslateTransform(x+w/2, y+h); g.RotateTransform(angle); g.TranslateTransform(-w/2,-h);
    Gdiplus::GraphicsPath shadow, paper; Rounded(shadow,0,2,w,h,4); Rounded(paper,0,0,w,h,4);
    Gdiplus::SolidBrush shade(Gdiplus::Color(65,0,0,0)), white(Gdiplus::Color(255,255,255));
    g.FillPath(&shade,&shadow); g.FillPath(&white,&paper);
    const float border = 3;
    if (!pic.data.empty()) {
        Gdiplus::Bitmap image(static_cast<INT>(pic.width), static_cast<INT>(pic.height),
            static_cast<INT>(pic.width * 4), PixelFormat32bppPARGB, pic.data.data());
        const float dw = w-border*2, dh = h-border*2;
        const float scale = std::max(dw / static_cast<float>(pic.width), dh / static_cast<float>(pic.height));
        const float sw = dw / scale, sh = dh / scale;
        g.DrawImage(&image, Gdiplus::RectF(border,border,dw,dh), (static_cast<float>(pic.width)-sw)/2, (static_cast<float>(pic.height)-sh)/2, sw,sh,Gdiplus::UnitPixel);
    } else {
        const uint64_t color = HashText(1469598103934665603ull, pic.label);
        const Gdiplus::Color accent_color(255, BYTE(45+color%65), BYTE(100+(color>>8)%70), BYTE(145+(color>>16)%70));
        Gdiplus::SolidBrush accent(accent_color);
        g.FillRectangle(&accent, border,border,w-border*2,30.0f);
        TypeLabel(g, pic.label, w-border*2, accent_color);
        Gdiplus::SolidBrush line(Gdiplus::Color(220,224,229));
        for (int i=0; i<4; ++i) g.FillRectangle(&line,12.0f,43.0f+static_cast<float>(i)*12.0f,w-24.0f-(i==3?25.0f:0),3.0f);
    }
    g.Restore(state);
}
// Rounded convex polygon; each corner is a quadratic curve emitted as a cubic.
void RoundedPolygon(Gdiplus::GraphicsPath& path, const Gdiplus::PointF* points, int count, float radius) {
    const auto Toward = [radius](const Gdiplus::PointF& from, const Gdiplus::PointF& corner) {
        const float dx = from.X - corner.X, dy = from.Y - corner.Y;
        const float length = std::sqrt(dx * dx + dy * dy);
        return Gdiplus::PointF(corner.X + dx / length * radius, corner.Y + dy / length * radius);
    };
    Gdiplus::PointF previous_end;
    for (int i = 0; i < count; ++i) {
        const auto& corner = points[i];
        const auto start = Toward(points[(i + count - 1) % count], corner);
        const auto end = Toward(points[(i + 1) % count], corner);
        if (i) path.AddLine(previous_end, start);
        path.AddBezier(start,
            Gdiplus::PointF(start.X + (corner.X - start.X) * 2 / 3, start.Y + (corner.Y - start.Y) * 2 / 3),
            Gdiplus::PointF(end.X + (corner.X - end.X) * 2 / 3, end.Y + (corner.Y - end.Y) * 2 / 3), end);
        previous_end = end;
    }
    path.CloseFigure();
}
// Upright card sized to the picture: 3:4 to 16:10 inside max_w x max_h (border included).
void FitCard(const Picture& pic, float max_w, float max_h, float& w, float& h) {
    constexpr float border = 6;
    const float ratio = pic.data.empty() ? 124.0f / 104.0f :
        std::clamp(static_cast<float>(pic.width) / static_cast<float>(pic.height), 0.75f, 1.6f);
    float iw = max_w - border, ih = iw / ratio;
    if (ih > max_h - border) { ih = max_h - border; iw = ih * ratio; }
    w = iw + border; h = ih + border;
}
bool Render(std::vector<Picture>& pictures, UINT cap, bool single, std::vector<uint8_t>& out) {
    GdiSession session; if (!session.token) return false;
    out.assign(size_t(cap)*cap*4, 0);
    Gdiplus::Bitmap bitmap(static_cast<INT>(cap),static_cast<INT>(cap),static_cast<INT>(cap*4),PixelFormat32bppPARGB,out.data());
    Gdiplus::Graphics g(&bitmap); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.ScaleTransform(static_cast<float>(cap)/256.0f, static_cast<float>(cap)/256.0f);
    // Ground shadow: a radial falloff, since GDI+ has no blur.
    Gdiplus::GraphicsPath ground; ground.AddEllipse(10.0f, 212.0f, 236.0f, 24.0f);
    Gdiplus::PathGradientBrush ground_brush(&ground);
    ground_brush.SetCenterColor(Gdiplus::Color(56,0,0,0));
    Gdiplus::Color ground_edge(0,0,0,0); INT edge_count = 1;
    ground_brush.SetSurroundColors(&ground_edge, &edge_count);
    g.FillPath(&ground_brush,&ground);
    Gdiplus::GraphicsPath back;
    back.AddArc(26,50,24,24,180,90); back.AddLine(38,50,92,50);
    back.AddBezier(92.0f,50.0f,96.0f,50.0f,99.0f,51.4f,102.0f,54.0f); back.AddLine(102,54,114,64);
    back.AddLine(114,64,218,64); back.AddArc(206,64,24,24,270,90);
    back.AddLine(230,76,230,214); back.AddLine(230,214,26,214); back.CloseFigure();
    Gdiplus::LinearGradientBrush back_brush(Gdiplus::Point(0,50),Gdiplus::Point(0,214),Gdiplus::Color(233,174,43),Gdiplus::Color(210,147,15));
    g.FillPath(&back_brush,&back);
    // Cards stand upright; their bottoms tuck 8 units behind the front leaf.
    const float top = single ? 146.0f : 152.0f, bottom = top + 8.0f;
    if (!single) {
        if (pictures.size()>2) Card(g,pictures[2],120,bottom-22-124,96,124,0);
        if (pictures.size()>1) Card(g,pictures[1],40,bottom-12-124,96,124,0);
    }
    float w = 0, h = 0;
    FitCard(pictures[0], single ? 176.0f : 150.0f, single ? 132.0f : 128.0f, w, h);
    Card(g,pictures[0],128-w/2,bottom-h,w,h,0);
    Gdiplus::LinearGradientBrush pocket(Gdiplus::PointF(0,top-30),Gdiplus::PointF(0,top+1),
        Gdiplus::Color(0,110,60,0),Gdiplus::Color(87,110,60,0));
    g.FillRectangle(&pocket,26.0f,top-30,204.0f,31.0f);
    // Front leaf opened toward the viewer: the top edge is wider than the bottom.
    const Gdiplus::PointF leaf[] = {{10,top},{246,top},{230,222},{26,222}};
    Gdiplus::GraphicsPath front; RoundedPolygon(front,leaf,4,11);
    Gdiplus::LinearGradientBrush front_brush(Gdiplus::PointF(0,top-0.5f),Gdiplus::PointF(0,222.5f),
        Gdiplus::Color(255,235,176),Gdiplus::Color(246,180,42));
    const Gdiplus::Color front_colors[] = {Gdiplus::Color(255,235,176),Gdiplus::Color(255,218,110),Gdiplus::Color(246,180,42)};
    const Gdiplus::REAL front_stops[] = {0.0f,0.07f,1.0f};
    front_brush.SetInterpolationColors(front_colors,front_stops,3);
    g.FillPath(&front_brush,&front);
    Gdiplus::Pen highlight(Gdiplus::Color(242,255,250,228),1.4f); g.DrawLine(&highlight,21.0f,top+1.4f,235.0f,top+1.4f);
    Gdiplus::Pen outline(Gdiplus::Color(46,150,90,0),1.0f); g.DrawPath(&outline,&front);
    g.Flush(Gdiplus::FlushIntentionSync); return g.GetLastStatus() == Gdiplus::Ok;
}
} // namespace

bool DecodeFolderThumbnail(const std::wstring& path, UINT pixels, std::vector<uint8_t>& bgra,
    UINT& width, UINT& height, UINT& stride, bool single, bool force_refresh) {
    SetLastError(ERROR_SUCCESS);
    bool timed_out = false;
    struct ResultError {
        bool& timed_out;
        ~ResultError() { SetLastError(timed_out ? ERROR_TIMEOUT : ERROR_SUCCESS); }
    } result_error{timed_out};
    DiagnosticTrace trace;
    Trace("request_begin", HashText(1469598103934665603ull, path));
    const auto Timeout = [&]() { Trace("budget_timeout"); timed_out = true; return false; };
    bgra.clear(); width = height = stride = 0;
    WIN32_FILE_ATTRIBUTE_DATA directory{};
    Trace("local_folder_begin");
    if (!LocalFolder(path, directory)) return false;
    Trace("local_folder_end");
    const UINT cap = std::clamp(pixels, 32u, 512u);
    [[maybe_unused]] unsigned completed_pictures = 0;
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
    const ULONGLONG deadline = GetTickCount64() + test_budget_ms;
#else
    const ULONGLONG deadline = GetTickCount64() + 450;
#endif
    const auto Expired = [&]() {
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
        if (completed_pictures >= test_completed_pictures) return true;
#endif
        return GetTickCount64() >= deadline;
    };
    std::vector<Entry> entries;
    FindHandle find{INVALID_HANDLE_VALUE};
    WIN32_FIND_DATAW data{};
    Trace("enumerate_begin");
    find.value = FindFirstFileExW((fs::path(path)/L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find.value == INVALID_HANDLE_VALUE) return false;
    unsigned visited = 0;
    do {
        if (++visited > 200) break;
        if (Expired()) return Timeout();
        if (data.dwFileAttributes & (kSkip | FILE_ATTRIBUTE_DIRECTORY)) continue;
        entries.push_back({data.cFileName, data, Rank(data.cFileName)});
    } while (FindNextFileW(find.value, &data));
    Trace("enumerate_end", entries.size());
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return CompareStringOrdinal(a.name.c_str(),-1,b.name.c_str(),-1,TRUE) == CSTR_LESS_THAN;
    });
    uint64_t key = HashText(1469598103934665603ull, path);
    key = HashBytes(key, &cap, sizeof(cap)); key = HashBytes(key, &single, sizeof(single));
    constexpr unsigned style = 6; key = HashBytes(key, &style, sizeof(style));
    uint64_t signature = HashBytes(key, &directory.ftLastWriteTime, sizeof(FILETIME));
    for (const auto& entry : entries) {
        signature = HashText(signature, entry.name);
        signature = HashBytes(signature, &entry.data.ftLastWriteTime, sizeof(FILETIME));
        signature = HashBytes(signature, &entry.data.nFileSizeHigh, sizeof(DWORD));
        signature = HashBytes(signature, &entry.data.nFileSizeLow, sizeof(DWORD));
    }
    const auto root = CacheDirectory();
    const auto cache_file = root / (std::to_wstring(key)+L".ftc");
    bool negative = false;
    Trace("cache_load_begin");
    if (!force_refresh && !root.empty() && LoadCache(cache_file, signature, cap, bgra, negative)) {
        Trace("cache_load_hit", negative);
        if (negative) return false;
        width=height=cap; stride=cap*4; return true;
    }
    Trace("cache_load_miss");
    bgra.clear();
    ComPtr<IWICImagingFactory> factory;
    Trace("wic_factory_begin");
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) return false;
    Trace("wic_factory_end");
    std::vector<Picture> pictures;
    const size_t count = single ? 1 : 3;
    bool video_extraction_attempted = false;
    unsigned candidate_index = 0;
    for (const auto& entry : entries) {
        if (pictures.size() >= count) break;
        if (Expired()) return Timeout();
        Trace("candidate_begin", (uint64_t(candidate_index++) << 32) | static_cast<unsigned>(entry.rank));
        Picture picture;
        const auto file = (fs::path(path)/entry.name).wstring();
        if (entry.rank < 2) {
            bool made = !force_refresh && CachedPicture(factory.Get(),file,cap,picture);
            if (!made && Expired()) return Timeout();
            if (!made && entry.rank == 0 && !entry.data.nFileSizeHigh && entry.data.nFileSizeLow <= 32*1024*1024)
                made = ReadPicture(factory.Get(),file,cap,picture);
            if (!made && Expired()) return Timeout();
            if (!made && entry.rank == 1 && !video_extraction_attempted) {
                video_extraction_attempted = true;
                made = CachedPicture(factory.Get(),file,cap,picture,true,force_refresh);
            }
            if (!made && Expired()) return Timeout();
            if (!made) continue;
        } else {
            picture.label = Extension(entry.name); if (!picture.label.empty()) picture.label.erase(0,1);
            if (picture.label.empty()) picture.label=L"FILE";
            picture.label.resize(std::min(size_t(5),picture.label.size()));
            std::transform(picture.label.begin(),picture.label.end(),picture.label.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
        }
        pictures.push_back(std::move(picture));
        ++completed_pictures;
        Trace("candidate_complete", pictures.size());
    }
    Trace("render_begin", pictures.size());
    const bool made = !pictures.empty() && Render(pictures,cap,single,bgra);
    Trace("render_end", made);
    if (!made) bgra.clear();
    if (!root.empty()) SaveCache(cache_file,signature,cap,bgra);
    if (made) { width=height=cap; stride=cap*4; }
    return made;
}
#ifdef PULSE_FOLDER_THUMBNAIL_TESTING
void SetFolderThumbnailBudgetForTest(DWORD budget_ms, unsigned completed_pictures) {
    test_budget_ms = budget_ms;
    test_completed_pictures = completed_pictures;
}
#endif
} // namespace pulse::preview
