#include "shelf_snippets.h"
#include "drop_staging.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../fs/fs_snapshot.h"
#include "../ui/drag_drop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <unordered_set>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;

std::atomic<uint64_t> g_snippet_seq{1};

CLIPFORMAT FmtPng() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG"));
    return cf;
}
CLIPFORMAT FmtMozUrl() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"text/x-moz-url"));
    return cf;
}
CLIPFORMAT FmtUrlW() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
    return cf;
}
CLIPFORMAT FmtUrlA() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocator"));
    return cf;
}
CLIPFORMAT FmtHtml() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format"));
    return cf;
}
CLIPFORMAT FmtShellIdList() {
    static CLIPFORMAT cf = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    return cf;
}

bool HasFormat(IDataObject* obj, CLIPFORMAT cf) {
    if (!obj || !cf) return false;
    FORMATETC fmt{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
    return obj->QueryGetData(&fmt) == S_OK;
}

std::wstring Trim(std::wstring_view sv) {
    size_t a = 0, b = sv.size();
    while (a < b && iswspace(sv[a])) ++a;
    while (b > a && iswspace(sv[b - 1])) --b;
    return std::wstring(sv.substr(a, b - a));
}

bool LooksLikeSingleUrl(const std::wstring& s) {
    if (!(s.starts_with(L"https://") || s.starts_with(L"http://"))) return false;
    if (s.size() < 10) return false;
    for (wchar_t c : s) {
        if (c <= 0x20 || c == 0x7f || iswspace(c)) return false;
    }
    return true;
}

std::wstring ExtractDomain(const std::wstring& url) {
    size_t p = url.find(L"://");
    p = (p == std::wstring::npos) ? 0 : p + 3;
    size_t end = url.find_first_of(L"/?#:", p);
    std::wstring host = url.substr(p, end == std::wstring::npos ? std::wstring::npos : end - p);
    if (host.starts_with(L"www.") && host.size() > 4) host.erase(0, 4);
    return host;
}

std::wstring SanitizeLeaf(std::wstring_view raw, const std::wstring& fallback) {
    std::wstring out;
    out.reserve(raw.size());
    bool prev_space = false;
    for (wchar_t c : raw) {
        if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
            if (!prev_space && !out.empty()) {
                out.push_back(L'_');
                prev_space = true;
            }
            continue;
        }
        if (iswspace(c)) {
            if (!prev_space && !out.empty()) {
                out.push_back(L' ');
                prev_space = true;
            }
            continue;
        }
        out.push_back(c);
        prev_space = false;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.' || out.back() == L'_'))
        out.pop_back();
    if (out.size() > 40) out.resize(40);
    if (out.empty()) out = fallback;
    // Guard DOS device names.
    static constexpr const wchar_t* kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4"
    };
    for (const wchar_t* r : kReserved) {
        if (_wcsicmp(out.c_str(), r) == 0) {
            out = L"_" + out;
            break;
        }
    }
    return out;
}

std::wstring UniqueFileInDir(const std::wstring& dir, const std::wstring& stem, const wchar_t* ext) {
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring base_dir = dir;
    if (!base_dir.empty() && base_dir.back() != L'\\') base_dir.push_back(L'\\');
    std::wstring candidate = base_dir + stem + ext;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    for (int k = 2; k < 10000; ++k) {
        candidate = base_dir + stem + L" (" + std::to_wstring(k) + L")" + ext;
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return base_dir + stem + L"_" + std::to_wstring(g_snippet_seq.fetch_add(1)) + ext;
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t rem = len;
    bool ok = true;
    while (rem > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(rem, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        p += written;
        rem -= written;
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    if (text.empty()) return WriteAllBytes(path, "", 0);
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        utf8.data(), n, nullptr, nullptr);
    return WriteAllBytes(path, utf8.data(), utf8.size());
}

bool ReadStgBytes(IDataObject* obj, CLIPFORMAT cf, std::vector<uint8_t>& out) {
    out.clear();
    if (!obj || !cf) return false;
    FORMATETC fmt{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
    STGMEDIUM med{};
    if (FAILED(obj->GetData(&fmt, &med))) return false;
    bool ok = false;
    if (med.tymed == TYMED_HGLOBAL && med.hGlobal) {
        const SIZE_T sz = GlobalSize(med.hGlobal);
        if (sz > 0 && sz <= (64u << 20)) {
            if (const void* ptr = GlobalLock(med.hGlobal)) {
                out.assign(static_cast<const uint8_t*>(ptr), static_cast<const uint8_t*>(ptr) + sz);
                GlobalUnlock(med.hGlobal);
                ok = true;
            }
        }
    } else if (med.tymed == TYMED_ISTREAM && med.pstm) {
        uint8_t buf[16384];
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = med.pstm->Read(buf, sizeof(buf), &got);
            if (FAILED(hr) || got > sizeof(buf)) { out.clear(); break; }
            if (got == 0) break;
            if (out.size() + got > (64u << 20)) { out.clear(); break; }
            out.insert(out.end(), buf, buf + got);
            ok = true;
        }
    }
    ReleaseStgMedium(&med);
    return ok && !out.empty();
}

bool EncodeBgraToPng(const std::wstring& path, UINT w, UINT h, const uint8_t* bgra, UINT stride) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) || !factory)
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pf))) return false;
    if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra))) ||
        FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        return false;
    return true;
}

bool WriteDibBytesToPng(const std::wstring& path, const uint8_t* data, size_t size) {
    if (!data || size < sizeof(BITMAPINFOHEADER)) return false;
    const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(data);
    if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > size) return false;
    const int w = bih->biWidth;
    const int raw_h = bih->biHeight;
    if (raw_h < -16384 || raw_h > 16384 || bih->biPlanes != 1) return false;
    const int h = std::abs(raw_h);
    const bool bottom_up = raw_h > 0;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    const WORD bpp = bih->biBitCount;
    if (bpp != 24 && bpp != 32) return false;
    size_t offset = bih->biSize;
    if (bih->biCompression == BI_BITFIELDS) {
        const bool embedded = bih->biSize >= 52;
        if (bpp != 32 || (!embedded && bih->biSize != sizeof(BITMAPINFOHEADER)) || size < 52)
            return false;
        DWORD masks[3]{};
        std::memcpy(masks, data + sizeof(BITMAPINFOHEADER), sizeof(masks));
        if (masks[0] != 0xff0000 || masks[1] != 0xff00 || masks[2] != 0xff) return false;
        if (!embedded) offset += sizeof(masks);
    }
    else if (bih->biCompression != BI_RGB) return false;
    if (bih->biClrUsed > 0) offset += static_cast<size_t>(bih->biClrUsed) * sizeof(RGBQUAD);
    const size_t src_stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    if (offset + src_stride * static_cast<size_t>(h) > size) return false;
    const uint8_t* pixels = data + offset;
    const UINT dst_stride = static_cast<UINT>(w) * 4u;
    std::vector<uint8_t> bgra(static_cast<size_t>(dst_stride) * static_cast<size_t>(h));
    bool any_alpha = false;
    for (int y = 0; y < h; ++y) {
        const int sy = bottom_up ? (h - 1 - y) : y;
        const uint8_t* sr = pixels + static_cast<size_t>(sy) * src_stride;
        uint8_t* dr = bgra.data() + static_cast<size_t>(y) * dst_stride;
        if (bpp == 32) {
            std::memcpy(dr, sr, dst_stride);
            for (int x = 0; x < w; ++x) {
                if (dr[x * 4 + 3] != 0) any_alpha = true;
            }
        } else {
            for (int x = 0; x < w; ++x) {
                dr[x * 4 + 0] = sr[x * 3 + 0];
                dr[x * 4 + 1] = sr[x * 3 + 1];
                dr[x * 4 + 2] = sr[x * 3 + 2];
                dr[x * 4 + 3] = 255;
            }
            any_alpha = true;
        }
    }
    // Many Win32 apps place BI_RGB 32bpp DIBs with all alpha bytes = 0.
    if (!any_alpha) {
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    }
    return EncodeBgraToPng(path, static_cast<UINT>(w), static_cast<UINT>(h), bgra.data(), dst_stride);
}

std::wstring FillOne(std::wstring fmt, const wchar_t* key, const std::wstring& val) {
    if (const size_t at = fmt.find(key); at != std::wstring::npos)
        fmt.replace(at, wcslen(key), val);
    return fmt;
}

std::wstring ShortTextTitle(const std::wstring& text, bool from_clipboard) {
    std::wstring collapsed;
    collapsed.reserve(24);
    bool sp = false;
    for (wchar_t c : text) {
        if (iswspace(c)) {
            if (!sp && !collapsed.empty()) { collapsed.push_back(L' '); sp = true; }
        } else {
            collapsed.push_back(c);
            sp = false;
            if (collapsed.size() >= 12) break;
        }
    }
    if (collapsed.empty()) collapsed = L"Text";
    return FillOne(l10n::Get(from_clipboard ? StringId::ShelfSnippetClipFormat
                                            : StringId::ShelfSnippetExcerptFormat),
                   L"{text}", collapsed);
}

bool MakeLinkSnippet(const std::wstring& shelf_dir, const std::wstring& url_in,
                     const std::wstring& title_in, bool from_clipboard,
                     ShelfContentItem& item) {
    const std::wstring url = Trim(url_in);
    if (!LooksLikeSingleUrl(url) || url.size() > 16384) return false;
    const std::wstring domain = ExtractDomain(url);
    std::wstring title = Trim(title_in);
    if (title.empty() || title == url) title = domain.empty() ? L"Link" : domain;
    const std::wstring stem = SanitizeLeaf(title, L"Link");
    const std::wstring file = UniqueFileInDir(shelf_dir, stem, L".url");
    const std::wstring body = L"[InternetShortcut]\r\nURL=" + url + L"\r\n";
    if (!WriteUtf8File(file, body)) return false;
    item.path = file;
    item.kind = TrayContentKind::Link;
    item.display_name = stem;
    item.domain = domain.empty() ? stem : domain;
    item.source_label = l10n::Get(from_clipboard ? StringId::ShelfSourceClipboard
                                                 : StringId::ShelfSourceBrowser);
    item.owned_snippet = true;
    return true;
}

bool MakeTextSnippet(const std::wstring& shelf_dir, const std::wstring& text_in,
                     bool from_clipboard, ShelfContentItem& item) {
    const std::wstring trimmed = Trim(text_in);
    if (trimmed.empty()) return false;
    if (LooksLikeSingleUrl(trimmed))
        return MakeLinkSnippet(shelf_dir, trimmed, {}, from_clipboard, item);
    const std::wstring title = ShortTextTitle(trimmed, from_clipboard);
    const std::wstring stem = SanitizeLeaf(title, L"Snippet");
    const std::wstring file = UniqueFileInDir(shelf_dir, stem, L".txt");
    if (!WriteUtf8File(file, text_in)) return false;
    item.path = file;
    item.kind = TrayContentKind::Text;
    item.display_name = title;
    item.source_label = l10n::Get(from_clipboard ? StringId::ShelfSourceClipboard
                                                 : StringId::ShelfSourceBrowser);
    item.owned_snippet = true;
    return true;
}

bool IsImagePath(const std::wstring& p) {
    const size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = p.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
           ext == L".webp" || ext == L".gif" || ext == L".bmp";
}

std::wstring WStringFromBytesUtf16(const std::vector<uint8_t>& b) {
    const size_t chars = b.size() / sizeof(wchar_t);
    std::wstring s(reinterpret_cast<const wchar_t*>(b.data()), chars);
    if (const size_t nul = s.find(L'\0'); nul != std::wstring::npos) s.resize(nul);
    return s;
}

std::wstring WStringFromBytesAnsiOrUtf8(const std::vector<uint8_t>& b) {
    size_t len = 0;
    while (len < b.size() && b[len] != 0) ++len;
    if (len == 0) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   reinterpret_cast<const char*>(b.data()), static_cast<int>(len), nullptr, 0);
    UINT cp = CP_UTF8;
    if (wlen <= 0) {
        cp = CP_ACP;
        wlen = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(b.data()),
                                   static_cast<int>(len), nullptr, 0);
    }
    if (wlen <= 0) return {};
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(b.data()),
                        static_cast<int>(len), out.data(), wlen);
    return out;
}

bool InsideDir(const std::wstring& path, const std::wstring& dir) {
    if (path.empty() || dir.empty()) return false;
    std::wstring p = path::StripExtendedPathPrefix(fs::NormalizePath(path));
    std::wstring d = path::StripExtendedPathPrefix(fs::NormalizePath(dir));
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    if (p.size() <= d.size() || p[d.size()] != L'\\') return false;
    if (_wcsnicmp(p.c_str(), d.c_str(), d.size()) != 0) return false;
    // Direct child file only.
    return p.find(L'\\', d.size() + 1) == std::wstring::npos;
}

} // namespace

std::wstring DefaultShelfSnippetsDir() {
    wchar_t fixture[MAX_PATH]{};
    const auto len = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture, ARRAYSIZE(fixture));
    if (len > 0 && len < ARRAYSIZE(fixture)) {
        const auto dir = (std::filesystem::path(fixture) / L"shelf").wstring();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return ec ? std::wstring{} : dir;
    }
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)) && local) {
        dir = std::wstring(local) + L"\\Pulse\\shelf";
        CoTaskMemFree(local);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    return dir;
}

const wchar_t* SnippetMaterializedExt(TrayContentKind kind) noexcept {
    switch (kind) {
    case TrayContentKind::Pic: return L".png";
    case TrayContentKind::Text: return L".txt";
    case TrayContentKind::Link: return L".url";
    default: return L"";
    }
}

ShelfDropPayloadKind ProbeShelfDropPayload(IDataObject* obj) {
    if (!obj) return ShelfDropPayloadKind::None;
    // Delayed rendering on DragEnter can stall the source. Preview using only
    // format queries; inspect a possible temporary HDROP image on Drop.
    if (HasFormat(obj, FmtHtml()) && HasFormat(obj, FmtPng())) return ShelfDropPayloadKind::Content;
    if (HasFormat(obj, CF_HDROP) || HasFormat(obj, FmtShellIdList())) return ShelfDropPayloadKind::Files;
    if (HasFormat(obj, FmtPng()) || HasFormat(obj, CF_DIBV5) || HasFormat(obj, CF_DIB) ||
        HasFormat(obj, FmtMozUrl()) || HasFormat(obj, FmtUrlW()) || HasFormat(obj, FmtUrlA()) ||
        HasFormat(obj, CF_UNICODETEXT) || HasFormat(obj, CF_TEXT)) {
        return ShelfDropPayloadKind::Content;
    }
    return ShelfDropPayloadKind::None;
}

bool ExtractShelfDropItems(IDataObject* obj, const std::wstring& shelf_dir,
                           std::vector<ShelfContentItem>& out, bool* out_is_content) {
    out.clear();
    if (out_is_content) *out_is_content = false;
    if (!obj) return false;

    std::vector<std::wstring> paths;
    if (ui::ExtractHDropPaths(obj, paths) || ui::ExtractShellItemIds(obj, paths)) {
        const std::wstring temp_dir = TempDirectory();
        const std::wstring stage_root = DropStageRoot();
        // Single temporary image from a browser drag: copy into shelf_dir as Pic.
        if (paths.size() == 1 && IsImagePath(paths[0]) && HasFormat(obj, FmtHtml()) &&
            IsTemporaryDropSource(paths[0], temp_dir, stage_root)) {
            std::filesystem::path src_p(paths[0]);
            std::wstring stem = SanitizeLeaf(src_p.stem().wstring(),
                                             l10n::Get(StringId::ShelfSnippetImageDefault));
            std::wstring ext = src_p.extension().wstring();
            if (ext.empty()) ext = L".png";
            const std::wstring dst = UniqueFileInDir(shelf_dir, stem, ext.c_str());
            if (CopyFileW(paths[0].c_str(), dst.c_str(), FALSE)) {
                ShelfContentItem item;
                item.path = dst;
                item.kind = TrayContentKind::Pic;
                item.display_name = stem;
                item.source_label = l10n::Get(StringId::ShelfSourceBrowser);
                item.owned_snippet = true;
                out.push_back(std::move(item));
                if (out_is_content) *out_is_content = true;
                return true;
            }
        }
        std::vector<std::wstring> staged;
        const auto res = StageDropSources(paths, temp_dir, stage_root, staged);
        const auto& use = (!res.error && !staged.empty()) ? staged : paths;
        for (const auto& p : use) {
            if (p.empty() || ui::IsShellItemId(p)) continue;
            ShelfContentItem item;
            item.path = fs::NormalizePath(p);
            item.kind = TrayContentKind::File;
            out.push_back(std::move(item));
        }
        return !out.empty();
    }

    if (out_is_content) *out_is_content = true;
    std::vector<uint8_t> bytes;
    // 1. Raw PNG image
    if (ReadStgBytes(obj, FmtPng(), bytes)) {
        const std::wstring stem = SanitizeLeaf(l10n::Get(StringId::ShelfSnippetImageDefault), L"Image");
        const std::wstring dst = UniqueFileInDir(shelf_dir, stem, L".png");
        if (WriteAllBytes(dst, bytes.data(), bytes.size())) {
            ShelfContentItem item;
            item.path = dst;
            item.kind = TrayContentKind::Pic;
            item.display_name = stem;
            item.source_label = l10n::Get(StringId::ShelfSourceBrowser);
            item.owned_snippet = true;
            out.push_back(std::move(item));
            return true;
        }
    }
    // 2. DIB / DIBV5 image
    if (ReadStgBytes(obj, CF_DIBV5, bytes) || ReadStgBytes(obj, CF_DIB, bytes)) {
        const std::wstring stem = SanitizeLeaf(l10n::Get(StringId::ShelfSnippetImageDefault), L"Image");
        const std::wstring dst = UniqueFileInDir(shelf_dir, stem, L".png");
        if (WriteDibBytesToPng(dst, bytes.data(), bytes.size())) {
            ShelfContentItem item;
            item.path = dst;
            item.kind = TrayContentKind::Pic;
            item.display_name = stem;
            item.source_label = l10n::Get(StringId::ShelfSourceBrowser);
            item.owned_snippet = true;
            out.push_back(std::move(item));
            return true;
        }
    }
    // 3. text/x-moz-url ("url\ntitle" UTF-16) — preserves link title from browsers!
    if (ReadStgBytes(obj, FmtMozUrl(), bytes)) {
        const std::wstring moz = WStringFromBytesUtf16(bytes);
        const size_t nl = moz.find(L'\n');
        std::wstring url = nl == std::wstring::npos ? moz : moz.substr(0, nl);
        std::wstring title = nl == std::wstring::npos ? std::wstring() : moz.substr(nl + 1);
        if (!url.empty() && url.back() == L'\r') url.pop_back();
        if (!title.empty() && title.back() == L'\r') title.pop_back();
        ShelfContentItem item;
        if (MakeLinkSnippet(shelf_dir, url, title, false, item)) {
            out.push_back(std::move(item));
            return true;
        }
    }
    // 4. UniformResourceLocatorW / A
    if (ReadStgBytes(obj, FmtUrlW(), bytes)) {
        ShelfContentItem item;
        if (MakeLinkSnippet(shelf_dir, WStringFromBytesUtf16(bytes), {}, false, item)) {
            out.push_back(std::move(item));
            return true;
        }
    }
    if (ReadStgBytes(obj, FmtUrlA(), bytes)) {
        ShelfContentItem item;
        if (MakeLinkSnippet(shelf_dir, WStringFromBytesAnsiOrUtf8(bytes), {}, false, item)) {
            out.push_back(std::move(item));
            return true;
        }
    }
    // 5. Unicode / ANSI text (auto-detects single URL vs text excerpt)
    if (ReadStgBytes(obj, CF_UNICODETEXT, bytes)) {
        ShelfContentItem item;
        if (MakeTextSnippet(shelf_dir, WStringFromBytesUtf16(bytes), false, item)) {
            out.push_back(std::move(item));
            return true;
        }
    }
    if (ReadStgBytes(obj, CF_TEXT, bytes)) {
        ShelfContentItem item;
        if (MakeTextSnippet(shelf_dir, WStringFromBytesAnsiOrUtf8(bytes), false, item)) {
            out.push_back(std::move(item));
            return true;
        }
    }
    return false;
}

bool ExtractClipboardShelfItems(HWND owner, const std::wstring& shelf_dir,
                                std::vector<ShelfContentItem>& out) {
    out.clear();
    if (!OpenClipboard(owner)) return false;

    // 1. CF_HDROP files on clipboard
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = static_cast<HDROP>(h);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring p(static_cast<size_t>(len) + 1, L'\0');
            if (DragQueryFileW(drop, i, p.data(), len + 1) == len) {
                p.resize(len);
                ShelfContentItem item;
                item.path = fs::NormalizePath(p);
                item.kind = TrayContentKind::File;
                out.push_back(std::move(item));
            }
        }
        if (!out.empty()) {
            CloseClipboard();
            return true;
        }
    }
    // 2. PNG on clipboard
    if (const CLIPFORMAT png = FmtPng(); png && IsClipboardFormatAvailable(png)) {
        if (HANDLE h = GetClipboardData(png)) {
            const SIZE_T sz = GlobalSize(h);
            if (sz > 0 && sz <= (64u << 20)) {
                if (const void* ptr = GlobalLock(h)) {
                    const std::wstring stem = SanitizeLeaf(l10n::Get(StringId::ShelfSnippetClipImage), L"Image");
                    const std::wstring dst = UniqueFileInDir(shelf_dir, stem, L".png");
                    const bool ok = WriteAllBytes(dst, ptr, sz);
                    GlobalUnlock(h);
                    if (ok) {
                        ShelfContentItem item;
                        item.path = dst;
                        item.kind = TrayContentKind::Pic;
                        item.display_name = stem;
                        item.source_label = l10n::Get(StringId::ShelfSourceClipboard);
                        item.owned_snippet = true;
                        out.push_back(std::move(item));
                        CloseClipboard();
                        return true;
                    }
                }
            }
        }
    }
    // 3. CF_DIBV5 / CF_DIB screenshot / copied image
    for (UINT cf : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
        if (!IsClipboardFormatAvailable(cf)) continue;
        if (HANDLE h = GetClipboardData(cf)) {
            const SIZE_T sz = GlobalSize(h);
            if (sz >= sizeof(BITMAPINFOHEADER) && sz <= (64u << 20)) {
                if (const auto* ptr = static_cast<const uint8_t*>(GlobalLock(h))) {
                    const std::wstring stem = SanitizeLeaf(l10n::Get(StringId::ShelfSnippetClipImage), L"Image");
                    const std::wstring dst = UniqueFileInDir(shelf_dir, stem, L".png");
                    const bool ok = WriteDibBytesToPng(dst, ptr, sz);
                    GlobalUnlock(h);
                    if (ok) {
                        ShelfContentItem item;
                        item.path = dst;
                        item.kind = TrayContentKind::Pic;
                        item.display_name = stem;
                        item.source_label = l10n::Get(StringId::ShelfSourceClipboard);
                        item.owned_snippet = true;
                        out.push_back(std::move(item));
                        CloseClipboard();
                        return true;
                    }
                }
            }
        }
    }
    // 4. CF_UNICODETEXT (URL or text snippet)
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            std::wstring text;
            if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
                const size_t max_chars = GlobalSize(h) / sizeof(wchar_t);
                size_t len = 0;
                while (len < max_chars && w[len] != L'\0') ++len;
                text.assign(w, len);
                GlobalUnlock(h);
            }
            ShelfContentItem item;
            if (!text.empty() && MakeTextSnippet(shelf_dir, text, true, item)) {
                out.push_back(std::move(item));
                CloseClipboard();
                return true;
            }
        }
    }
    CloseClipboard();
    return false;
}

void DeleteOwnedSnippetFile(const std::wstring& path, const std::wstring& shelf_dir) {
    if (!InsideDir(path, shelf_dir)) return;
    const std::wstring plain = path::StripExtendedPathPrefix(fs::NormalizePath(path));
    DeleteFileW(plain.c_str());
}

void SweepOrphanedShelfSnippets(const std::wstring& shelf_dir,
                                const std::vector<std::wstring>& referenced_paths) {
    if (shelf_dir.empty()) return;
    std::unordered_set<std::wstring> keep;
    keep.reserve(referenced_paths.size());
    for (const auto& p : referenced_paths) {
        if (!p.empty()) keep.insert(path::FoldedKey(fs::NormalizePath(p)));
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(shelf_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring p = fs::NormalizePath(entry.path().wstring());
        if (!keep.contains(path::FoldedKey(p))) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

} // namespace pulse::app
