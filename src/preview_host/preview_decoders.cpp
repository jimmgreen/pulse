#include "preview_decoders.h"
#include "gif_decoder.h"
#include "../ipc/preview_protocol.h"
#include "../common/preview_extensions.h"
#include "archive_listing.h"
#include "content_sniff.h"
#include "docx_document.h"
#include "epub_document.h"
#include "dwg_thumb.h"
#include "folder_listing.h"
#include "markdown_document.h"
#include "notebook_document.h"
#include "font_raster.h"
#include "image_frames.h"
#include "gif_frames.h"
#include "image_pack.h"
#include "raw_pack.h"
#include "archive_pack.h"
#include "media_pack.h"
#include "metafile_raster.h"
#include "office_doc_model.h"
#include "office_sketch.h"
#include "pdf_raster.h"
#include "pe_document.h"
#include "pe_dependencies.h"
#include "preview_file_utils.h"
#include "psd_raster.h"
#include "svg_raster.h"
#include "table_document.h"
#include "tree_document.h"
#include "zip_entry.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
// The Media Foundation headers of this SDK do not build at the project's
// NTDDI_WIN10_NI (MFVideoSphericalFormat); audio_waveform.cpp sets 8.1 too.
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma pop_macro("NTDDI_VERSION")
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Media Foundation's GUIDs only; its DLLs are loaded on first use (see Mf()).
#pragma comment(lib, "mfuuid.lib")

using namespace pulse;
using Microsoft::WRL::ComPtr;
using preview::ExtensionOf;
using preview::IsOfflinePlaceholder;
using preview::ShellPath;

namespace {

bool IsOneOf(std::wstring_view extension,
             std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

bool IsKnownText(std::wstring_view extension) {
    return pulse::preview::IsTextExtension(extension);
}

bool IsDirectImage(std::wstring_view extension) {
    return pulse::preview::IsImageExtension(extension);
}

bool IsKnownShellPreview(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".pdf", L".doc", L".docx", L".xls",
        L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp", L".mp4",
        L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v", L".dwg",
        L".dxf", L".step", L".stp", L".iges", L".igs",
        // Office variants and WPS / OpenDocument: shell provider when one is
        // installed, otherwise the sketch.
        L".docm", L".xlsm", L".pptm", L".ppsx", L".pps", L".wps", L".et", L".dps", L".odg"
    });
}

// Formats outside the built-in lists (PSD, AI, RAW, fonts...) get a thumbnail
// when a shell handler is registered for them, exactly as Explorer decides:
// IThumbnailProvider, or the legacy IExtractImage many third-party packs use.
// Looked up once per extension; the host serves requests on one thread.
bool HasShellThumbnailHandler(const std::wstring& extension) {
    static std::unordered_map<std::wstring, bool> cache;
    if (extension.empty()) return false;
    if (const auto it = cache.find(extension); it != cache.end()) return it->second;
    static constexpr const wchar_t* kHandlers[] = {
        L"{e357fccd-a995-4576-b01f-234630154e96}", // IThumbnailProvider
        L"{BB2E617C-0920-11d1-9A0B-00C04FC2D6C1}", // IExtractImage
    };
    bool found = false;
    for (const wchar_t* handler : kHandlers) {
        wchar_t clsid[128]{};
        DWORD chars = ARRAYSIZE(clsid);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION,
                                        extension.c_str(), handler, clsid, &chars)) &&
            clsid[0] != L'\0') {
            found = true;
            break;
        }
    }
    cache.emplace(extension, found);
    return found;
}

bool ReadPrefix(const std::wstring& path, DWORD limit, std::vector<uint8_t>& bytes,
                uint64_t& file_size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size) != 0;
    file_size = sized ? static_cast<uint64_t>(size.QuadPart) : 0;
    bytes.resize(static_cast<size_t>((std::min)(file_size, static_cast<uint64_t>(limit))));
    DWORD read = 0;
    const bool ok = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0);
    CloseHandle(file);
    if (!ok) { bytes.clear(); return false; }
    bytes.resize(read);
    return true;
}

bool LooksBinary(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                             (bytes[0] == 0xFE && bytes[1] == 0xFF))) return false;
    size_t controls = 0;
    for (uint8_t c : bytes) {
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++controls;
    }
    return !bytes.empty() && controls * 20 > bytes.size();
}

bool DecodeText(const std::vector<uint8_t>& bytes, std::wstring& text, bool& truncated,
                ipc::PreviewTextEncoding* encoding = nullptr) {
    if (encoding) *encoding = ipc::PreviewTextEncoding::Utf8;
    if (bytes.empty()) { text.clear(); return true; }
    size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        if (encoding) *encoding = ipc::PreviewTextEncoding::Utf16Le;
        offset = 2;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
    } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        if (encoding) *encoding = ipc::PreviewTextEncoding::Utf16Be;
        offset = 2;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((bytes[i] << 8) | bytes[i + 1]));
    } else {
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            offset = 3;
            if (encoding) *encoding = ipc::PreviewTextEncoding::Utf8Bom;
        }
        const char* raw = reinterpret_cast<const char*>(bytes.data() + offset);
        int raw_size = static_cast<int>(bytes.size() - offset);
        if (truncated && raw_size > 0) {
            // A byte budget may cut a valid UTF-8 character. Exclude only an
            // incomplete final sequence; the remaining prefix still undergoes
            // strict validation before choosing UTF-8 over the system encoding.
            int lead = raw_size - 1;
            while (lead > 0 && (static_cast<unsigned char>(raw[lead]) & 0xC0) == 0x80) --lead;
            const auto c = static_cast<unsigned char>(raw[lead]);
            const int width = c >= 0xC2 && c <= 0xDF ? 2 :
                c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 1;
            if (raw_size - lead < width) raw_size = lead;
        }
        if (!raw_size) { text.clear(); return true; }
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, raw_size,
                                        nullptr, 0);
        UINT code_page = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (chars <= 0) {
            code_page = CP_ACP;
            flags = 0;
            if (encoding) *encoding = ipc::PreviewTextEncoding::Ansi;
            chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
        }
        if (chars <= 0) return false;
        text.resize(chars);
        MultiByteToWideChar(code_page, flags, raw, raw_size, text.data(), chars);
    }
    for (wchar_t& c : text) {
        if (c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t') c = L'\xFFFD';
    }
    constexpr size_t kMaxChars = 12000;
    if (text.size() > kMaxChars) {
        text.resize(kMaxChars);
        truncated = true;
    }
    if (truncated && !text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF)
        text.pop_back();
    return true;
}

std::wstring MakeHex(const std::vector<uint8_t>& bytes) {
    std::wstring out;
    wchar_t line[128]{};
    for (size_t base = 0; base < bytes.size(); base += 16) {
        int pos = swprintf_s(line, L"%08llX  ", static_cast<unsigned long long>(base));
        for (size_t i = 0; i < 16; ++i) {
            if (base + i < bytes.size())
                pos += swprintf_s(line + pos, std::size(line) - pos, L"%02X ", bytes[base + i]);
            else
                pos += swprintf_s(line + pos, std::size(line) - pos, L"   ");
        }
        pos += swprintf_s(line + pos, std::size(line) - pos, L" ");
        for (size_t i = 0; i < 16 && base + i < bytes.size(); ++i) {
            const uint8_t c = bytes[base + i];
            line[pos++] = c >= 0x20 && c < 0x7F ? static_cast<wchar_t>(c) : L'.';
        }
        line[pos++] = L'\n';
        line[pos] = 0;
        out += line;
    }
    return out;
}

bool MakeTextOrHex(const std::wstring& path, DWORD attrs, ipc::PreviewContentKind& kind,
                   std::wstring& text, uint32_t& bytes_read, bool& truncated,
                   ipc::PreviewTextEncoding* encoding = nullptr) {
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) || IsOfflinePlaceholder(attrs)) return false;
    const bool known_text = IsKnownText(ExtensionOf(path));
    std::vector<uint8_t> bytes;
    uint64_t file_size = 0;
    const DWORD initial = known_text ? 32u * 1024u : 256u;
    if (!ReadPrefix(path, initial, bytes, file_size)) return false;
    if (!known_text && LooksBinary(bytes)) {
        kind = ipc::PreviewContentKind::Hex;
        text = MakeHex(bytes);
        bytes_read = static_cast<uint32_t>(bytes.size());
        truncated = file_size > bytes.size();
        return true;
    }
    if (!known_text && file_size > bytes.size()) {
        if (!ReadPrefix(path, 32u * 1024u, bytes, file_size)) return false;
    }
    truncated = file_size > bytes.size();
    if (LooksBinary(bytes) || !DecodeText(bytes, text, truncated, encoding)) return false;
    kind = ipc::PreviewContentKind::Text;
    bytes_read = static_cast<uint32_t>(bytes.size());
    return true;
}

} // namespace

static bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                        std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
                        UINT& source_width, UINT& source_height,
                        const std::vector<unsigned char>* embedded = nullptr,
                        uint32_t frame_index = 0, uint32_t* frame_count = nullptr) {
    if (IsOfflinePlaceholder(attrs)) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapFlipRotator> rotation, reflection;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICStream> stream;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    if (embedded) {
        if (embedded->empty() || embedded->size() > MAXDWORD || FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(embedded->data()), static_cast<DWORD>(embedded->size()))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    } else if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    if (frame_count) {
        UINT count = 0;
        if (FAILED(decoder->GetFrameCount(&count)) || !count) return false;
        *frame_count = count;
        frame_index = (std::min)(frame_index, count - 1);
    }
    if (FAILED(decoder->GetFrame(frame_index, &frame))) return false;

    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0) return false;
    IWICBitmapSource* source = frame.Get();
    unsigned orientation = 1;
    GUID container{};
    if (SUCCEEDED(decoder->GetContainerFormat(&container)) &&
        (IsEqualGUID(container, GUID_ContainerFormatJpeg) || IsEqualGUID(container, GUID_ContainerFormatTiff))) {
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
            PROPVARIANT value{};
            const auto key = IsEqualGUID(container, GUID_ContainerFormatJpeg)
                ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
            if (SUCCEEDED(metadata->GetMetadataByName(key, &value)) && value.vt == VT_UI2 &&
                value.uiVal >= 1 && value.uiVal <= 8) orientation = value.uiVal;
            PropVariantClear(&value);
        }
    }
    // Scale first, straight off the frame: the JPEG decoder then decodes at
    // 1/2, 1/4 or 1/8 (IWICBitmapSourceTransform) instead of full size. A
    // flip-rotator in front of the scaler made every scaler pass re-decode
    // the whole frame, so a 12 MP portrait phone photo (Exif orientation 6)
    // took over 30 s and held up every preview queued behind it.
    const bool quarter_turn = orientation >= 5;
    source_width = quarter_turn ? sourceHeight : sourceWidth;
    source_height = quarter_turn ? sourceWidth : sourceHeight;
    const UINT longest = (std::max)(sourceWidth, sourceHeight);
    if (longest > pixels) {
        const double ratio = static_cast<double>(pixels) / longest;
        const UINT scaledWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * ratio + 0.5));
        const UINT scaledHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * ratio + 0.5));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source, scaledWidth, scaledHeight,
                                      WICBitmapInterpolationModeFant))) return false;
        source = scaler.Get();
    }
    ComPtr<IWICBitmap> upright;
    if (orientation != 1) {
        // Turned in memory, on the already small picture.
        WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
        switch (orientation) {
        case 2: transform = WICBitmapTransformFlipHorizontal; break;
        case 3: transform = WICBitmapTransformRotate180; break;
        case 4: transform = WICBitmapTransformFlipVertical; break;
        case 5: case 6: case 7: transform = WICBitmapTransformRotate90; break;
        case 8: transform = WICBitmapTransformRotate270; break;
        }
        if (FAILED(factory->CreateBitmapFromSource(source, WICBitmapCacheOnLoad, &upright))) return false;
        source = upright.Get();
        if (FAILED(factory->CreateBitmapFlipRotator(&rotation)) ||
            FAILED(rotation->Initialize(source, transform))) return false;
        source = rotation.Get();
        // Make transform order explicit for the two diagonal reflections.
        if (orientation == 5 || orientation == 7) {
            if (FAILED(factory->CreateBitmapFlipRotator(&reflection)) ||
                FAILED(reflection->Initialize(source, orientation == 5
                    ? WICBitmapTransformFlipHorizontal : WICBitmapTransformFlipVertical))) return false;
            source = reflection.Get();
        }
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return false;
    stride = width * 4;
    out.resize(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()),
                                     out.data()))) {
        out.clear();
        return false;
    }
    return true;
}

static bool MetadataUInt(IWICMetadataQueryReader* reader, const wchar_t* name,
                         uint32_t& value) {
    if (!reader) return false;
    PROPVARIANT pv{}; PropVariantInit(&pv);
    const HRESULT hr = reader->GetMetadataByName(name, &pv);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        if (pv.vt == VT_UI1) value = pv.bVal;
        else if (pv.vt == VT_UI2) value = pv.uiVal;
        else if (pv.vt == VT_UI4) value = pv.ulVal;
        else if (pv.vt == VT_I4 && pv.lVal >= 0) value = static_cast<uint32_t>(pv.lVal);
        else ok = false;
    }
    PropVariantClear(&pv);
    return ok;
}

static void AlphaBlendPbgra(uint8_t* dst, const uint8_t* src) {
    const uint32_t sa = src[3];
    if (sa == 255) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; return; }
    if (sa == 0) return;
    const uint32_t inv = 255u - sa;
    dst[0] = static_cast<uint8_t>(src[0] + (dst[0] * inv + 127u) / 255u);
    dst[1] = static_cast<uint8_t>(src[1] + (dst[1] * inv + 127u) / 255u);
    dst[2] = static_cast<uint8_t>(src[2] + (dst[2] * inv + 127u) / 255u);
    dst[3] = static_cast<uint8_t>(sa + (dst[3] * inv + 127u) / 255u);
}

static bool HbitmapToBgra(HBITMAP bitmap, bool own, std::vector<uint8_t>& out,
                          UINT& width, UINT& height, UINT& stride) {
    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory)))) {
        if (own) DeleteObject(bitmap);
        return false;
    }
    const WICBitmapAlphaChannelOption options[] = {
        WICBitmapUsePremultipliedAlpha, WICBitmapIgnoreAlpha
    };
    bool ok = false;
    for (const auto option : options) {
        ComPtr<IWICBitmap> source;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateBitmapFromHBITMAP(bitmap, nullptr, option, &source)))
            continue;
        if (FAILED(wicFactory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
            continue;
        stride = width * 4;
        out.resize(static_cast<size_t>(stride) * height);
        if (SUCCEEDED(converter->CopyPixels(nullptr, stride,
                static_cast<UINT>(out.size()), out.data()))) {
            ok = true;
            break;
        }
        out.clear();
    }
    if (own) DeleteObject(bitmap);
    if (!ok) out.clear();
    return ok;
}

static bool FromThumbnailProvider(IShellItem* item, UINT pixels, HBITMAP& bitmap) {
    ComPtr<IThumbnailProvider> provider;
    if (FAILED(item->BindToHandler(nullptr, BHID_ThumbnailHandler, IID_PPV_ARGS(&provider))))
        return false;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    return SUCCEEDED(provider->GetThumbnail(pixels, &bitmap, &alpha)) && bitmap;
}

static bool FromThumbnailCache(IShellItem* item, UINT pixels, bool cache_only, HBITMAP& bitmap) {
    ComPtr<IThumbnailCache> cache;
    if (FAILED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&cache))))
        return false;
    const WTS_FLAGS flags = cache_only
        ? static_cast<WTS_FLAGS>(WTS_INCACHEONLY | WTS_SCALETOREQUESTEDSIZE)
        : static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_SCALETOREQUESTEDSIZE);
    ComPtr<ISharedBitmap> shared;
    WTS_CACHEFLAGS cacheFlags{};
    if (FAILED(cache->GetThumbnail(item, pixels, flags, &shared, &cacheFlags, nullptr)) || !shared)
        return false;
    HBITMAP shared_bitmap = nullptr;
    if (FAILED(shared->GetSharedBitmap(&shared_bitmap)) || !shared_bitmap) return false;
    bitmap = static_cast<HBITMAP>(CopyImage(shared_bitmap, IMAGE_BITMAP, 0, 0, 0));
    return bitmap != nullptr;
}

// cache_only: answer from Explorer's thumbnail cache or not at all - never
// runs a provider, so it cannot stall the host.
static bool MakeShellThumbnail(const std::wstring& path, DWORD attrs, UINT pixels,
                               std::vector<uint8_t>& out, UINT& width, UINT& height,
                               UINT& stride, bool cache_only = false) {
    if (IsOfflinePlaceholder(attrs)) return false;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(shell_path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return false;
    const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    HBITMAP bitmap = nullptr;

    if (isDirectory) {
        ComPtr<IShellItemImageFactory> factory;
        if (FAILED(item.As(&factory))) return false;
        const SIZE size{static_cast<LONG>(pixels), static_cast<LONG>(pixels)};
        if (FAILED(factory->GetImage(size,
                SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap)) || !bitmap)
            return false;
        return HbitmapToBgra(bitmap, true, out, width, height, stride);
    }

    if (!FromThumbnailCache(item.Get(), pixels, true, bitmap) || !bitmap)
        bitmap = nullptr;
    if (cache_only) {
        if (!bitmap) return false;
        return HbitmapToBgra(bitmap, true, out, width, height, stride);
    }
    if (!bitmap && !FromThumbnailProvider(item.Get(), pixels, bitmap))
        bitmap = nullptr;
    if (!bitmap && (!FromThumbnailCache(item.Get(), pixels, false, bitmap) || !bitmap))
        bitmap = nullptr;
    if (!bitmap) {
        ComPtr<IShellItemImageFactory> factory;
        if (SUCCEEDED(item.As(&factory))) {
            const SIZE size{static_cast<LONG>(pixels), static_cast<LONG>(pixels)};
            const SIIGBF flags = static_cast<SIIGBF>(
                SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY);
            if (FAILED(factory->GetImage(size, flags, &bitmap))) bitmap = nullptr;
        }
    }
    if (!bitmap) return false;
    return HbitmapToBgra(bitmap, true, out, width, height, stride);
}

// ---- Content decoders -------------------------------------------------------
// One entry per format family, tried in kDecoders order (see preview_router.h).
// accepts() holds the cheap checks; run() returns Next when the file could not
// be read natively so the shell thumbnail and text/hex paths still get it.

using preview::DecodeRequest;
using preview::DecodeResult;
using preview::DecodeStep;

static DecodeStep MadeBitmap(DecodeResult& r) {
    r.kind = ipc::PreviewContentKind::Bitmap;
    return DecodeStep::Made;
}

static bool ShellThumbnailInto(const DecodeRequest& q, DecodeResult& r) {
    return MakeShellThumbnail(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height,
                              r.stride);
}

static bool CachedShellThumbnailInto(const DecodeRequest& q, DecodeResult& r) {
    return MakeShellThumbnail(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height,
                              r.stride, true);
}

static void ClearBitmap(DecodeResult& r) {
    r.pixels.clear();
    r.width = r.height = r.stride = r.source_width = r.source_height = 0;
}

static bool DwgHeaderThumbnailInto(const DecodeRequest& q, DecodeResult& r) {
    ClearBitmap(r);
    return preview::ExtractDwgThumbnail(q.path, q.cap, r.pixels, r.width, r.height,
        r.stride, r.source_width, r.source_height, nullptr);
}

static bool TextOrHexInto(const DecodeRequest& q, DecodeResult& r) {
    return MakeTextOrHex(q.path, q.request.attrs, r.kind, r.text, r.bytes_read, r.truncated,
                         &r.text_encoding);
}

// ---- Image extras: animated WebP / APNG, icon sizes, missing codecs --------

static bool ReadFileBytes(const std::wstring& path, size_t max_bytes, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
              static_cast<uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>((std::min<size_t>)(out.size() - done, 1u << 24));
            ok = ReadFile(file, out.data() + done, want, &n, nullptr) && n > 0;
            done += n;
        }
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

// PBGRA canvas -> the reply, shrunk to fit the requested size.
static bool FitCanvas(IWICImagingFactory* factory, std::vector<uint8_t>& canvas, UINT cw, UINT ch,
                      UINT pixels, std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride) {
    const UINT longest = (std::max)(cw, ch);
    if (longest <= pixels) {
        out = canvas;
        width = cw; height = ch; stride = cw * 4;
        return true;
    }
    const double ratio = static_cast<double>(pixels) / longest;
    width = (std::max)(1u, static_cast<UINT>(cw * ratio + 0.5));
    height = (std::max)(1u, static_cast<UINT>(ch * ratio + 0.5));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppPBGRA, cw * 4,
                                               static_cast<UINT>(canvas.size()), canvas.data(), &bitmap)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeFant)))
        return false;
    stride = width * 4;
    out.resize(static_cast<size_t>(stride) * height);
    return SUCCEEDED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data()));
}

static bool FramePixels(IWICImagingFactory* factory, IWICBitmapSource* frame, std::vector<uint8_t>& pixels,
                        UINT& width, UINT& height) {
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 16384 || height > 16384)
        return false;
    pixels.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

// Animated WebP: the system decoder (Windows 10 1809+) hands out whole
// composed frames with their duration. false for still pictures.
static bool DecodeWebpFrame(const std::wstring& path, UINT pixels, uint32_t frame_index, DecodeResult& r,
                            uint32_t& frame_count, uint32_t& delay_ms, uint32_t& loop_count) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || count < 2) return false;
    frame_index = (std::min)(frame_index, count - 1);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(frame_index, &frame))) return false;
    std::vector<uint8_t> canvas;
    UINT cw = 0, ch = 0;
    if (!FramePixels(factory.Get(), frame.Get(), canvas, cw, ch)) return false;
    delay_ms = 100;
    ComPtr<IWICMetadataQueryReader> reader;
    uint32_t value = 0;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&reader)) && reader &&
        MetadataUInt(reader.Get(), L"/ANMF/FrameDuration", value) && value > 10)
        delay_ms = (std::min)(value, 60000u);
    loop_count = 0;
    ComPtr<IWICMetadataQueryReader> top;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&top)) && top && MetadataUInt(top.Get(), L"/ANIM/LoopCount", value))
        loop_count = value;
    frame_count = count;
    r.source_width = cw;
    r.source_height = ch;
    return FitCanvas(factory.Get(), canvas, cw, ch, pixels, r.pixels, r.width, r.height, r.stride);
}

// Scan chunk headers without reading large ancillary payloads before IDAT.
static bool HasApngControl(const std::wstring& path, uint32_t& declared_frames) {
    declared_frames = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint8_t head[8]{};
    DWORD n = 0;
    bool found = false;
    const auto be32 = [](const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) |
        (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; };
    if (ReadFile(file, head, 8, &n, nullptr) && n == 8 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G' &&
        head[4] == 13 && head[5] == 10 && head[6] == 26 && head[7] == 10) {
        for (size_t chunks = 0; chunks < 8192; ++chunks) {
            if (!ReadFile(file, head, 8, &n, nullptr) || n != 8) break;
            const uint32_t length = be32(head);
            const std::string_view type(reinterpret_cast<const char*>(head + 4), 4);
            if (type == "acTL") {
                found = true;  // Malformed animation control still requires an explicit fallback warning.
                uint8_t control[8]{};
                if (length == 8 && ReadFile(file, control, 8, &n, nullptr) && n == 8) {
                    declared_frames = be32(control);
                }
                break;
            }
            if (type == "IDAT" || type == "IEND") break;
            LARGE_INTEGER skip{};
            skip.QuadPart = static_cast<LONGLONG>(length) + 4;
            if (!SetFilePointerEx(file, skip, nullptr, FILE_CURRENT)) break;
        }
    }
    CloseHandle(file);
    return found;
}

// APNG: frames are rebuilt as standalone PNGs for WIC and composed here. The
// canvas of the last request is kept, so playing forward costs one frame.
namespace {
struct ApngState {
    std::mutex lock;
    std::wstring path;
    uint64_t size = 0, modified = 0;
    std::vector<uint8_t> file;
    preview::ApngInfo info;
    bool valid = false;
    std::wstring failure_reason;
    uint32_t next = 0;             // frames composed into canvas
    std::vector<uint8_t> canvas;   // what frame next-1 shows
    std::vector<uint8_t> saved;    // canvas before frame next-1 (dispose 2)
};
ApngState g_apng;
}

static bool DecodeApngFrame(const std::wstring& path, UINT pixels, uint32_t frame_index, DecodeResult& r,
                            uint32_t& frame_count, uint32_t& delay_ms, uint32_t& loop_count) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const uint64_t modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                              data.ftLastWriteTime.dwLowDateTime;
    std::lock_guard guard(g_apng.lock);
    ApngState& st = g_apng;
    const auto publish = [&](bool fallback) {
        const auto& reason = st.failure_reason.empty() ? st.info.incomplete_reason : st.failure_reason;
        const uint32_t loaded = fallback ? 1u : static_cast<uint32_t>(st.info.frames.size());
        r.text = preview::MakeImageFramesPayload(fallback ? L"static-fallback" : L"animation", loaded,
            st.info.declared_frames, fallback ? 0 : (std::min)(frame_index, loaded - 1), reason);
        if (!reason.empty()) {
            r.error = L"image-preview-incomplete:" + reason;
            r.truncated = true;
        }
    };
    if (st.path != path || st.size != size || st.modified != modified) {
        st.path = path; st.size = size; st.modified = modified;
        st.valid = false; st.next = 0;
        st.canvas.clear(); st.saved.clear(); st.info = {};
        st.failure_reason.clear();
        uint32_t declared = 0;
        if (!HasApngControl(path, declared)) return false;
        st.info.declared_frames = declared;
        if (size > 96u * 1024u * 1024u) st.failure_reason = L"file-limit";
        else if (!ReadFileBytes(path, 96u * 1024u * 1024u, st.file)) st.failure_reason = L"read-failed";
        else if (!preview::ParseApng(st.file, st.info)) st.failure_reason =
            st.info.incomplete_reason.empty() ? L"malformed" : st.info.incomplete_reason;
        else if (static_cast<uint64_t>(st.info.width) * st.info.height > 4096u * 4096u)
            st.failure_reason = L"canvas-limit";
        if (!st.failure_reason.empty()) {
            if (!st.info.declared_frames) st.info.declared_frames = declared;
            st.file.clear();
            st.file.shrink_to_fit();
        } else st.valid = true;
    }
    if (!st.valid) {
        if (!st.failure_reason.empty()) publish(true);
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        st.failure_reason = L"decode-failed";
        publish(true);
        return false;
    }
    const auto& frames = st.info.frames;
    const UINT cw = st.info.width, ch = st.info.height;
    frame_index = (std::min)(frame_index, static_cast<uint32_t>(frames.size() - 1));
    if (st.next == 0 || frame_index + 1 < st.next) {
        st.next = 0;
        st.canvas.assign(static_cast<size_t>(cw) * ch * 4, 0);
        st.saved.clear();
    }
    auto clear_rect = [&](const preview::ApngFrame& f) {
        for (uint32_t y = f.y; y < f.y + f.height; ++y)
            std::fill(st.canvas.begin() + (static_cast<size_t>(y) * cw + f.x) * 4,
                      st.canvas.begin() + (static_cast<size_t>(y) * cw + f.x + f.width) * 4, uint8_t{0});
    };
    while (st.next <= frame_index) {
        if (st.next > 0) {
            const preview::ApngFrame& prev = frames[st.next - 1];
            if (prev.dispose == 1) clear_rect(prev);
            else if (prev.dispose == 2 && st.saved.size() == st.canvas.size()) st.canvas = st.saved;
        }
        const preview::ApngFrame& f = frames[st.next];
        if (f.dispose == 2) st.saved = st.canvas;
        const std::vector<uint8_t> png = preview::BuildApngFramePng(st.file, st.info, st.next);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        std::vector<uint8_t> px;
        UINT fw = 0, fh = 0;
        if (png.empty() || FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || !FramePixels(factory.Get(), frame.Get(), px, fw, fh)) {
            st.next = 0;  // start over next time
            st.failure_reason = L"decode-failed";
            st.valid = false;
            publish(true);
            return false;
        }
        const UINT w = (std::min)(fw, f.width), h = (std::min)(fh, f.height);
        for (UINT y = 0; y < h; ++y) {
            uint8_t* dst = &st.canvas[(static_cast<size_t>(f.y + y) * cw + f.x) * 4];
            const uint8_t* src = &px[static_cast<size_t>(y) * fw * 4];
            if (f.blend == 0) std::copy(src, src + static_cast<size_t>(w) * 4, dst);
            else for (UINT x = 0; x < w; ++x) AlphaBlendPbgra(dst + x * 4, src + x * 4);
        }
        ++st.next;
    }
    frame_count = static_cast<uint32_t>(frames.size());
    delay_ms = frames[frame_index].delay_ms;
    loop_count = st.info.plays;
    r.source_width = cw;
    r.source_height = ch;
    if (!FitCanvas(factory.Get(), st.canvas, cw, ch, pixels, r.pixels, r.width, r.height, r.stride)) {
        frame_count = 1; delay_ms = loop_count = 0;
        st.failure_reason = L"decode-failed";
        publish(true);
        return false;
    }
    st.failure_reason.clear();
    publish(false);
    return true;
}

// .ico / .cur: one directory entry (frame_index 0 = the largest, k = entry
// k-1) and the list of sizes as text for the window's size pills.
static bool DecodeIconEntry(const std::wstring& path, UINT pixels, uint32_t frame_index, DecodeResult& r) {
    std::vector<uint8_t> file;
    std::vector<preview::IconEntry> entries;
    bool cursor = false;
    if (!ReadFileBytes(path, 32u * 1024u * 1024u, file) || !preview::ParseIconDirectory(file, entries, cursor))
        return false;
    const size_t selected = frame_index == 0 ? preview::DefaultIconEntry(entries)
                                             : (std::min<size_t>)(frame_index - 1, entries.size() - 1);
    if (cursor) file[2] = 1;  // WIC's icon decoder reads cursors once they claim to be icons
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    std::vector<uint8_t> canvas;
    UINT cw = 0, ch = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(file.data(), static_cast<DWORD>(file.size()))) ||
        FAILED(factory->CreateDecoder(GUID_ContainerFormatIco, nullptr, &decoder)) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) ||
        FAILED(decoder->GetFrame(entries[selected].frame, &frame)) ||
        !FramePixels(factory.Get(), frame.Get(), canvas, cw, ch))
        return false;
    r.source_width = cw;
    r.source_height = ch;
    if (!FitCanvas(factory.Get(), canvas, cw, ch, pixels, r.pixels, r.width, r.height, r.stride)) return false;
    r.text = preview::MakeIconSizesPayload(entries, selected);
    return true;
}

// HEIF / AVIF without their Store extensions: names the missing piece for the
// window's codec card ("heif", "hevc" or "av1"), empty when unknown.
static std::wstring MissingImageCodec(const std::wstring& path, const std::wstring& extension) {
    const bool heif = extension == L".heic" || extension == L".heif" || extension == L".hif";
    const bool avif = extension == L".avif";
    if (!heif && !avif) return {};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return {};
    const HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder);
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) return avif ? L"av1" : L"heif";
    // The HEIF container opened but its pictures did not decode: HEVC is the
    // separate extension HEIC photos need.
    if (SUCCEEDED(hr) && heif) return L"hevc";
    return {};
}

// Image preview pack (HEIC / AVIF / JPEG XL without the Store extensions,
// OpenEXR, Radiance HDR, QOI). False without the pack.
// ---- HEIF embedded thumbnail -------------------------------------------------
// Phones store a small picture next to the photo (iPhone: 320 x 240, as an
// item the 'thmb' reference points at). WIC hands it out through
// GetThumbnail in about 60 ms, where decoding the 12 MP photo itself took
// 1.7 s and a 48 MP one over 4 s. GetThumbnail on a file without one decodes
// the whole photo, so the reference is looked for first.
constexpr UINT kHeifThumbnailEdge = 320;

static bool IsHeifExtension(std::wstring_view extension) {
    return IsOneOf(extension, {L".heic", L".heif", L".hif", L".avif"});
}

// The children of a box held in `bytes`: calls `visit(type, payload, end)`
// until it returns true.
template <typename Visit>
static bool EachBox(const std::vector<uint8_t>& bytes, size_t pos, size_t limit, Visit visit) {
    for (int boxes = 0; boxes < 1024 && pos + 8 <= limit; ++boxes) {
        uint64_t size = (uint64_t(bytes[pos]) << 24) | (uint64_t(bytes[pos + 1]) << 16) |
                        (uint64_t(bytes[pos + 2]) << 8) | bytes[pos + 3];
        size_t payload = pos + 8;
        if (size == 1) {
            if (pos + 16 > limit) return false;
            size = 0;
            for (int i = 0; i < 8; ++i) size = (size << 8) | bytes[pos + 8 + i];
            payload = pos + 16;
        } else if (size == 0) {
            size = limit - pos;
        }
        if (size < payload - pos) return false;
        const size_t end = size > limit - pos ? limit : pos + static_cast<size_t>(size);
        if (visit(reinterpret_cast<const char*>(&bytes[pos + 4]), payload, end)) return true;
        if (size > limit - pos) return false;
        pos = end;
    }
    return false;
}

static bool HeifHasThumbnail(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    uint64_t file_size = 0;
    if (!ReadPrefix(path, 256u * 1024u, bytes, file_size)) return false;
    return EachBox(bytes, 0, bytes.size(), [&](const char* type, size_t payload, size_t end) {
        if (memcmp(type, "meta", 4) != 0) return false;
        return EachBox(bytes, payload + 4, end, [&](const char* child, size_t iref, size_t iref_end) {
            if (memcmp(child, "iref", 4) != 0) return false;
            return EachBox(bytes, iref + 4, iref_end, [](const char* reference, size_t, size_t) {
                return memcmp(reference, "thmb", 4) == 0;
            });
        });
    });
}

static bool HeifThumbnailInto(const DecodeRequest& q, UINT cap, DecodeResult& r) {
    if (!HeifHasThumbnail(q.path)) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> thumbnail;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(q.path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetThumbnail(&thumbnail)))
        return false;
    std::vector<uint8_t> canvas;
    UINT width = 0, height = 0, full_w = 0, full_h = 0;
    if (!FramePixels(factory.Get(), thumbnail.Get(), canvas, width, height) || width > 1024 || height > 1024)
        return false;
    ClearBitmap(r);
    if (!FitCanvas(factory.Get(), canvas, width, height, cap, r.pixels, r.width, r.height, r.stride)) {
        ClearBitmap(r);
        return false;
    }
    // The photo's own size (already turned upright by the decoder) for the facts.
    if (SUCCEEDED(frame->GetSize(&full_w, &full_h))) { r.source_width = full_w; r.source_height = full_h; }
    return true;
}

static bool ImagePackInto(const DecodeRequest& q, UINT cap, DecodeResult& r) {
    if (!preview::IsImagePackExtension(q.extension) || !preview::ImagePackAvailable()) return false;
    preview::ImagePackFrame frame;
    if (!preview::ImagePackDecode(q.path, cap, q.grid, frame)) {
        ClearBitmap(r);
        return false;
    }
    r.pixels = std::move(frame.pixels);
    r.width = frame.width; r.height = frame.height; r.stride = frame.stride;
    r.source_width = frame.source_width; r.source_height = frame.source_height;
    return true;
}

// WIC images (animated GIF frames included).
static bool AcceptsImage(const DecodeRequest& q) { return IsDirectImage(q.extension); }
static DecodeStep RunImage(const DecodeRequest& q, DecodeResult& r) {
    uint32_t frame_count = 1, frame_delay = 0, loop_count = 0;
    const bool gif = q.extension == L".gif";
    const UINT cap = ipc::ClampPreviewPixelSize(q.request.pixel_size, gif);
    bool made = false;
    if (!q.offline && cap <= kHeifThumbnailEdge && IsHeifExtension(q.extension)) {
        // Grid thumbnails and the hover card's first picture: the embedded
        // thumbnail. The card asks for the full photo next, so a file
        // without one answers at once instead of decoding it twice.
        if (HeifThumbnailInto(q, cap, r)) return MadeBitmap(r);
        ClearBitmap(r);
        if (q.request.flags & ipc::kPreviewRequestFlagCard) {
            r.error = L"heif-no-thumbnail";
            return DecodeStep::Failed;
        }
    }
    if (!q.offline && (q.extension == L".tif" || q.extension == L".tiff")) {
        made = DecodeImage(q.path, q.request.attrs, cap, r.pixels, r.width, r.height,
            r.stride, r.source_width, r.source_height, nullptr, q.request.frame_index, &frame_count);
        if (made) r.text = preview::MakeImageFramesPayload(L"pages", frame_count, frame_count,
            (std::min)(q.request.frame_index, frame_count - 1));
        else {
            r.error = L"image-page-decode-failed";
            return DecodeStep::Failed;
        }
    } else if (gif) {
        if (q.offline) { r.error = L"gif-offline"; return DecodeStep::Failed; }
        preview::GifFrame frame;
        made = preview::DecodeGifCached(q.path, cap, q.request.frame_index, frame);
        if (made) {
            r.pixels = std::move(frame.pixels); r.width = frame.width; r.height = frame.height; r.stride = frame.stride;
            r.source_width = frame.source_width; r.source_height = frame.source_height;
            frame_count = frame.count; frame_delay = frame.delay; loop_count = frame.loops;
        } else {
            // Over the animation budget (a 37 MB screen recording, say): show
            // the first frame as a still instead of an empty preview.
            ClearBitmap(r);
            made = DecodeImage(q.path, q.request.attrs, cap, r.pixels, r.width, r.height,
                               r.stride, r.source_width, r.source_height);
            if (!made) { r.error = frame.error; return DecodeStep::Failed; }
        }
    } else if (!q.offline && !q.grid && q.extension == L".webp" &&
               DecodeWebpFrame(q.path, ipc::ClampPreviewPixelSize(q.request.pixel_size, true),
                               q.request.frame_index, r, frame_count, frame_delay, loop_count)) {
        made = true;
    } else if (!q.offline && !q.grid && q.extension == L".png" &&
               DecodeApngFrame(q.path, ipc::ClampPreviewPixelSize(q.request.pixel_size, true),
                               q.request.frame_index, r, frame_count, frame_delay, loop_count)) {
        made = true;
    } else if (!q.offline && (q.extension == L".ico" || q.extension == L".cur") &&
               DecodeIconEntry(q.path, cap, q.request.frame_index, r)) {
        made = true;
    } else {
        ClearBitmap(r);
        made = DecodeImage(q.path, q.request.attrs, cap, r.pixels, r.width, r.height,
                           r.stride, r.source_width, r.source_height);
    }
    r.frame_count = frame_count;
    r.frame_delay_ms = frame_delay;
    r.loop_count = loop_count;
    if (made) return MadeBitmap(r);
    if (!q.offline) {
        // WIC has no decoder for every format in the list on every machine:
        // AVIF/HEIC need the store extension. The image preview pack reads
        // them when it is installed; otherwise fall back to the shell
        // thumbnail - the path Explorer itself uses - before reporting the
        // preview as unavailable.
        ClearBitmap(r);
        if (ImagePackInto(q, cap, r)) return MadeBitmap(r);
        if (ShellThumbnailInto(q, r)) return MadeBitmap(r);
        const std::wstring codec = MissingImageCodec(q.path, q.extension);
        if (!codec.empty()) {
            r.error = L"image-codec-missing:" + codec;
            return DecodeStep::Failed;
        }
    }
    if (r.error.empty()) r.error = L"image-decode-failed";
    return DecodeStep::Failed;
}

// Formats outside the WIC image list that the image pack reads. JPEG XL tries
// Windows 11's Store extension first. A file that does not start like one of
// these (an ENVI .hdr header is plain text) keeps the text / hex chain; a real
// picture without the pack names the pack for Quick Look's install card.
static bool AcceptsImagePack(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && preview::IsImagePackOnlyExtension(q.extension);
}
static DecodeStep RunImagePack(const DecodeRequest& q, DecodeResult& r) {
    // 16 bytes decide before any decoder (or process) starts.
    if (!preview::LooksLikeImagePackFile(q.path)) return DecodeStep::Next;
    if (q.extension == L".jxl") {
        ClearBitmap(r);
        if (DecodeImage(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height, r.stride,
                        r.source_width, r.source_height)) return MadeBitmap(r);
        ClearBitmap(r);
    }
    if (ImagePackInto(q, q.cap, r)) return MadeBitmap(r);
    if (q.extension == L".jxl" && HasShellThumbnailHandler(q.extension) && ShellThumbnailInto(q, r))
        return MadeBitmap(r);
    ClearBitmap(r);
    if (q.grid) return DecodeStep::Next;
    r.error = preview::ImagePackAvailable() ? L"image-decode-failed" : L"image-pack-missing";
    return DecodeStep::Failed;
}

// RAW grid requests never develop sensor data; only an explicit Quick Look
// request may use the slower full decoder when there is no embedded preview.
static bool AcceptsRawPack(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
        preview::IsFamilyExtension(preview::PreviewFamily::Raw, q.extension);
}
static DecodeStep RunRawPack(const DecodeRequest& q, DecodeResult& r) {
    ClearBitmap(r);
    if (DecodeImage(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height, r.stride,
                    r.source_width, r.source_height)) return MadeBitmap(r);
    ClearBitmap(r);
    preview::ImagePackFrame frame;
    const bool full = !q.grid && (q.request.flags & ipc::kPreviewRequestFlagRichText);
    if (preview::RawPackDecode(q.path, q.cap, full, frame)) {
        r.pixels = std::move(frame.pixels);
        r.width = frame.width; r.height = frame.height; r.stride = frame.stride;
        r.source_width = frame.source_width; r.source_height = frame.source_height;
        return MadeBitmap(r);
    }
    // An installed shell provider may still have a cached embedded preview.
    return DecodeStep::Next;
}

static bool MakeEnhancedArchiveListing(const std::wstring& path, std::wstring& text,
                                       uint32_t& bytes_read, std::wstring* error, bool allow_pack) {
    if (preview::MakeArchiveListing(path, text, bytes_read, error)) return true;
    // Preserve deliberate native limits/cancellation rather than retrying work.
    if (error && (error->find(L"cancel") != std::wstring::npos ||
                  error->find(L"limit") != std::wstring::npos)) return false;
    // Archive contents are useful in the preview pane, not in grid thumbnails.
    if (!allow_pack || !preview::ArchivePackAvailable()) return false;
    return preview::ArchivePackListing(path, text, bytes_read, error);
}

// SVG through Direct2D; markup that does not render is shown as text.
static bool AcceptsVector(const DecodeRequest& q) {
    return preview::IsVectorExtension(q.extension);
}
static DecodeStep RunVector(const DecodeRequest& q, DecodeResult& r) {
    // Use the system SVG subset only after checking for unsupported content.
    if (!q.offline && preview::RasterizeSvgFile(q.path, q.cap, r.pixels, r.width, r.height,
            r.stride, r.source_width, r.source_height, &r.error))
        return MadeBitmap(r);
    if (q.offline) return DecodeStep::Failed;
    // Preserve the renderer's reason while showing source for restricted,
    // oversized or damaged documents.
    if (TextOrHexInto(q, r)) return DecodeStep::Made;
    r.error = L"svg-render-failed";
    return DecodeStep::Failed;
}

// WMF/EMF: WIC decodes them only when its codec is installed, and the shell has
// no thumbnail provider, so GDI renders them.
static bool AcceptsMetaFile(const DecodeRequest& q) {
    return preview::IsMetaFileExtension(q.extension);
}
static DecodeStep RunMetaFile(const DecodeRequest& q, DecodeResult& r) {
    if (!q.offline && preview::RasterizeMetaFile(q.path, q.cap, r.pixels, r.width, r.height,
            r.stride, r.source_width, r.source_height, &r.error))
        return MadeBitmap(r);
    return DecodeStep::Failed;
}

// PDF / PDF-compatible AI: PDFium renders the requested page (frame_index) at
// the requested size and reports the page count as frame_count with a zero
// frame delay, which tells the UI this is a paged document, not an animation
// (GIF delays are never below 20 ms). Legacy
// PostScript AI, encrypted PDFs or a missing DLL fall back to whatever shell
// thumbnail provider is registered.
static bool AcceptsPdf(const DecodeRequest& q) {
    return !q.is_directory && preview::IsPdfRasterExtension(q.extension);
}
static DecodeStep RunPdf(const DecodeRequest& q, DecodeResult& r) {
    bool made = false;
    if (!q.offline) {
        UINT pages = 0;
        made = preview::RasterizePdfFile(q.path, q.cap, r.pixels, r.width, r.height, r.stride,
                                         r.source_width, r.source_height, &r.error,
                                         q.request.frame_index, &pages, q.grid);
        if (!made && r.error == L"pdf-thumbnail-budget") return DecodeStep::Failed;
        if (made && pages > 1) {
            r.frame_count = pages;
            r.frame_delay_ms = 0;
        }
        if (!made) {
            r.pixels.clear();
            r.width = r.height = r.stride = r.source_width = r.source_height = 0;
            made = ShellThumbnailInto(q, r);
            if (made) r.error.clear();
        }
    }
    if (made) return MadeBitmap(r);
    if (r.error.empty()) r.error = L"provider-failed";
    return DecodeStep::Failed;
}

// Archives: contents tree instead of a hex dump. A file that is not a readable
// archive falls through to the entries below.
static bool AcceptsArchive(const DecodeRequest& q) {
    return !q.is_directory && preview::IsArchiveExtension(q.extension) && !q.offline;
}
static DecodeStep RunArchive(const DecodeRequest& q, DecodeResult& r) {
    if (!MakeEnhancedArchiveListing(q.path, r.text, r.bytes_read, &r.error, !q.grid))
        return DecodeStep::Next;
    r.kind = ipc::PreviewContentKind::Archive;
    return DecodeStep::Made;
}

// PSD/PSB: large previews show the merged composite; grid thumbnails keep an
// installed shell provider, else the embedded JPEG thumbnail.
static bool AcceptsPsd(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && preview::IsPsdExtension(q.extension) &&
        (!q.grid || !HasShellThumbnailHandler(q.extension));
}
static DecodeStep RunPsd(const DecodeRequest& q, DecodeResult& r) {
    if (!preview::RasterizePsdFile(q.path, q.cap, q.grid, r.pixels, r.width, r.height,
            r.stride, r.source_width, r.source_height, nullptr))
        return DecodeStep::Next;
    return MadeBitmap(r);
}

// Fonts: specimen page for the details pane and Quick Look; grid thumbnails
// keep the Windows font thumbnail ("Abg").
static bool AcceptsFont(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && preview::IsFontExtension(q.extension) && !q.grid;
}
static DecodeStep RunFont(const DecodeRequest& q, DecodeResult& r) {
    if (!preview::RasterizeFontFile(q.path, q.cap, r.pixels, r.width, r.height, r.stride,
                                    nullptr))
        return DecodeStep::Next;
    return MadeBitmap(r);
}

// RTF: grid thumbnails get the first-page sketch; the details pane and Quick
// Look get the body text, not the markup the text view would otherwise show.
static bool RtfInto(const DecodeRequest& q, DecodeResult& r) {
    if (q.grid) {
        if (q.cap < preview::kOfficeSketchMinEdge) return false;
        ClearBitmap(r);
        if (!preview::RenderOfficeSketch(q.path, L".rtf", q.cap, r.pixels, r.width, r.height, r.stride,
                                         nullptr)) {
            ClearBitmap(r);
            return false;
        }
        r.kind = ipc::PreviewContentKind::Bitmap;
        return true;
    }
    std::wstring text;
    bool truncated = false;
    if (!preview::ReadRtfText(q.path, text, &truncated, nullptr)) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const uint64_t size = GetFileAttributesExW(q.path.c_str(), GetFileExInfoStandard, &data)
        ? (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow : 0;
    r.kind = ipc::PreviewContentKind::Text;
    r.text = std::move(text);
    r.truncated = truncated;
    r.bytes_read = static_cast<uint32_t>((std::min<uint64_t>)(size, UINT32_MAX));
    return true;
}
static bool AcceptsRtf(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && q.extension == L".rtf";
}
static DecodeStep RunRtf(const DecodeRequest& q, DecodeResult& r) {
    return RtfInto(q, r) ? DecodeStep::Made : DecodeStep::Next;
}

// Videos Windows cannot decode itself - HEVC / AV1 / VP9 without the Store
// extensions, FLV, RMVB, MPEG-TS, ProRes - once the FFmpeg preview pack is
// installed. The native path still runs first and is unchanged: Explorer's
// thumbnail cache and provider answer every file they can, and ffmpeg only
// starts for the ones they could not. Without the pack accepts() is false and
// the table behaves exactly as before.
static bool AcceptsMediaPack(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && preview::IsMediaPackVideoExtension(q.extension) &&
        preview::MediaPackAvailable();
}
// MPEG transport streams: Windows' video thumbnail provider was measured
// stalling for more than 8 s on a 22 s 720p MPEG-2 .ts/.m2ts (with or without
// the pack), while ffmpeg returns the frame in about 150 ms. For these only
// Explorer's existing cache entry is taken before ffmpeg; the provider is not
// asked at all.
static bool IsTransportStreamExtension(std::wstring_view extension) {
    return IsOneOf(extension, {L".ts", L".m2ts", L".mts", L".m2t"});
}
static DecodeStep RunMediaPack(const DecodeRequest& q, DecodeResult& r) {
    const bool shell_known = IsKnownShellPreview(q.extension);
    uint32_t known_duration_ms = 0;
    if (shell_known || HasShellThumbnailHandler(q.extension)) {
        const bool cache_only = IsTransportStreamExtension(q.extension);
        if (cache_only ? CachedShellThumbnailInto(q, r) : ShellThumbnailInto(q, r)) return MadeBitmap(r);
        ClearBitmap(r);
        // Windows may still know the playing time (the property handler
        // parses the container even when no decoder is installed).
        known_duration_ms = preview::ReadMediaDurationMs(q.path);
    }
    preview::MediaFrame frame;
    if (preview::MediaPackFrame(q.path, q.cap, q.grid, known_duration_ms, frame)) {
        r.pixels = std::move(frame.pixels);
        r.width = frame.width; r.height = frame.height; r.stride = frame.stride;
        r.source_width = frame.source_width; r.source_height = frame.source_height;
        r.duration_ms = frame.duration_ms ? frame.duration_ms : known_duration_ms;
        return MadeBitmap(r);
    }
    // What the shell-preview entry would have reported for a video, without
    // asking the provider a second time; other containers keep the old
    // content-sniff / text / hex chain.
    if (shell_known) {
        r.error = L"provider-failed";
        return DecodeStep::Failed;
    }
    return DecodeStep::Next;
}

// ---- Audio cover art ----------------------------------------------------------
// The picture a tagger embedded, read straight from the file: ID3v2 APIC/PIC
// (MP3, ADTS AAC, WAV / AIFF "id3 " chunks), MP4 covr atoms (M4A) and FLAC
// PICTURE blocks. Measured on real music folders this takes a few ms, where
// the shell provider took 250-450 ms to report a file without a cover.

static bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD bytes) {
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD got = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
        ReadFile(file, buffer, bytes, &got, nullptr) && got == bytes;
}
static uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static uint32_t Le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
static uint32_t Syncsafe(const uint8_t* p) {
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) |
           (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}
// ID3 unsynchronisation: every FF 00 stands for FF.
static void Resync(std::vector<uint8_t>& bytes) {
    size_t out = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[out++] = bytes[i];
        if (bytes[i] == 0xFF && i + 1 < bytes.size() && bytes[i + 1] == 0x00) ++i;
    }
    bytes.resize(out);
}
constexpr uint32_t kMaxCoverBytes = 32u * 1024u * 1024u;

// APIC / PIC body -> picture bytes; rank 2 for the front cover, 1 otherwise.
static bool Id3Picture(const std::vector<uint8_t>& body, int version,
                       std::vector<unsigned char>& picture, int& rank) {
    if (body.size() < 4) return false;
    const uint8_t encoding = body[0];
    size_t pos = 1;
    if (version == 2) {
        pos += 3;                                     // "JPG" / "PNG"
    } else {
        const size_t start = pos;
        while (pos < body.size() && body[pos]) ++pos; // MIME type
        if (pos - start == 3 && memcmp(&body[start], "-->", 3) == 0) return false;  // a link
        ++pos;
    }
    if (pos >= body.size()) return false;
    const uint8_t type = body[pos++];
    if (encoding == 1 || encoding == 2) {             // UTF-16 description, 00 00 ended
        while (pos + 1 < body.size() && (body[pos] || body[pos + 1])) pos += 2;
        pos += 2;
    } else {
        while (pos < body.size() && body[pos]) ++pos;
        ++pos;
    }
    if (pos >= body.size()) return false;
    picture.assign(body.begin() + static_cast<ptrdiff_t>(pos), body.end());
    rank = type == 3 ? 2 : 1;
    return picture.size() >= 16;
}

static bool Id3Cover(HANDLE file, uint64_t offset, std::vector<unsigned char>& out) {
    uint8_t header[10];
    if (!ReadAt(file, offset, header, sizeof(header)) || memcmp(header, "ID3", 3) != 0) return false;
    const int version = header[3];
    const uint8_t flags = header[5];
    const uint32_t size = Syncsafe(header + 6);
    if (version < 2 || version > 4 || !size || size > kMaxCoverBytes + 65536u) return false;
    std::vector<uint8_t> tag(size);
    if (!ReadAt(file, offset + 10, tag.data(), size)) return false;
    if ((flags & 0x80) && version < 4) Resync(tag);   // v2.4 marks it per frame
    size_t pos = 0;
    if ((flags & 0x40) && version >= 3 && tag.size() >= 4)
        pos = version == 3 ? 4 + Be32(tag.data()) : Syncsafe(tag.data());
    const size_t frame_header = version == 2 ? 6 : 10;
    int best = 0;
    while (pos + frame_header <= tag.size()) {
        const uint8_t* frame = &tag[pos];
        if (!frame[0]) break;                          // padding
        uint32_t frame_size = 0;
        uint16_t frame_flags = 0;
        if (version == 2) {
            frame_size = (uint32_t(frame[3]) << 16) | (uint32_t(frame[4]) << 8) | frame[5];
        } else {
            frame_size = version == 4 ? Syncsafe(frame + 4) : Be32(frame + 4);
            frame_flags = static_cast<uint16_t>((frame[8] << 8) | frame[9]);
        }
        if (frame_size > tag.size() - pos - frame_header) break;
        const bool picture = version == 2 ? memcmp(frame, "PIC", 3) == 0 : memcmp(frame, "APIC", 4) == 0;
        // Compressed or encrypted frames are skipped (v2.3: 0x80 / 0x40, v2.4: 0x08 / 0x04).
        const bool sealed = version == 3 ? (frame_flags & 0x00C0) != 0 :
                            version == 4 ? (frame_flags & 0x000C) != 0 : false;
        if (picture && !sealed) {
            std::vector<uint8_t> body(frame + frame_header, frame + frame_header + frame_size);
            if (version == 4 && (frame_flags & 0x0001) && body.size() >= 4)
                body.erase(body.begin(), body.begin() + 4);   // data length indicator
            if (version == 4 && (frame_flags & 0x0002)) Resync(body);
            std::vector<unsigned char> art;
            int rank = 0;
            if (Id3Picture(body, version, art, rank) && rank > best) {
                out = std::move(art);
                best = rank;
                if (rank == 2) break;
            }
        }
        pos += frame_header + frame_size;
    }
    return best > 0;
}

// RIFF (WAV, little-endian sizes) or FORM (AIFF, big-endian) "id3 " chunk.
static bool ChunkedId3Cover(HANDLE file, uint64_t file_size, bool big_endian,
                            std::vector<unsigned char>& out) {
    uint64_t pos = 12;
    for (int chunks = 0; chunks < 256 && pos + 8 <= file_size; ++chunks) {
        uint8_t header[8];
        if (!ReadAt(file, pos, header, sizeof(header))) return false;
        const uint32_t size = big_endian ? Be32(header + 4) : Le32(header + 4);
        if (_strnicmp(reinterpret_cast<const char*>(header), "id3 ", 4) == 0)
            return Id3Cover(file, pos + 8, out);
        pos += 8 + uint64_t(size) + (size & 1);
    }
    return false;
}

// The FLAC PICTURE structure (also base64'd into Ogg METADATA_BLOCK_PICTURE).
static bool FlacPicture(const uint8_t* block, size_t length, std::vector<unsigned char>& out) {
    if (length < 32) return false;
    size_t at = 4;                                                 // picture type
    const uint32_t mime = Be32(block + at); at += 4;
    if (mime > length - at - 4) return false;
    at += mime;
    const uint32_t description = Be32(block + at); at += 4;
    if (length - at < 20 || description > length - at - 20) return false;
    at += description + 16;                                        // width, height, depth, colours
    const uint32_t bytes = Be32(block + at); at += 4;
    if (bytes < 16 || bytes > length - at) return false;
    out.assign(block + at, block + at + bytes);
    return true;
}

static bool Base64Decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;                                             // line breaks
        bits = (bits << 6) | static_cast<uint32_t>(v);
        if (++count == 4) {
            out.push_back(static_cast<uint8_t>(bits >> 16));
            out.push_back(static_cast<uint8_t>(bits >> 8));
            out.push_back(static_cast<uint8_t>(bits));
            bits = 0; count = 0;
        }
    }
    if (count == 3) { out.push_back(static_cast<uint8_t>(bits >> 10)); out.push_back(static_cast<uint8_t>(bits >> 2)); }
    else if (count == 2) out.push_back(static_cast<uint8_t>(bits >> 4));
    return !out.empty();
}

// Ogg Vorbis / Opus: the comment packet (the stream's second) and its
// METADATA_BLOCK_PICTURE, or the older COVERART field.
static bool OggCover(HANDLE file, uint64_t file_size, std::vector<unsigned char>& out,
                     DWORD window = 256u * 1024u) {
    const DWORD limit = static_cast<DWORD>((std::min<uint64_t>)(file_size, window));
    std::vector<uint8_t> bytes(limit);
    if (!ReadAt(file, 0, bytes.data(), limit)) return false;
    std::vector<uint8_t> packet;
    int packets = 0;
    uint32_t serial = 0;
    bool first = true;
    size_t pos = 0;
    while (packets < 2 && pos + 27 <= bytes.size() && memcmp(&bytes[pos], "OggS", 4) == 0) {
        const uint32_t page_serial = Le32(&bytes[pos + 14]);
        const uint8_t segments = bytes[pos + 26];
        if (pos + 27 + segments > bytes.size()) break;            // cut by the window
        const uint8_t* table = &bytes[pos + 27];
        size_t data = pos + 27 + segments;
        if (first) { serial = page_serial; first = false; }
        bool cut = false;
        for (uint8_t s = 0; s < segments && packets < 2; ++s) {
            if (data + table[s] > bytes.size()) { cut = true; break; }
            if (page_serial == serial) {
                if (packets == 1) packet.insert(packet.end(), &bytes[data], &bytes[data] + table[s]);
                if (table[s] < 255) ++packets;                     // a packet ends here
            }
            data += table[s];
        }
        if (cut) break;
        size_t page_bytes = 27 + segments;
        for (uint8_t s = 0; s < segments; ++s) page_bytes += table[s];
        pos += page_bytes;
    }
    if (packets < 2) {
        // A large cover spreads the comment packet over many pages: read on.
        const DWORD more = kMaxCoverBytes + 65536u;
        return limit < file_size && window < more && OggCover(file, file_size, out, more);
    }
    size_t at = 0;
    if (packet.size() >= 7 && packet[0] == 3 && memcmp(&packet[1], "vorbis", 6) == 0) at = 7;
    else if (packet.size() >= 8 && memcmp(packet.data(), "OpusTags", 8) == 0) at = 8;
    else return false;
    if (at + 4 > packet.size()) return false;
    at += 4 + Le32(&packet[at]);                                   // vendor string
    if (at + 4 > packet.size()) return false;
    const uint32_t count = Le32(&packet[at]); at += 4;
    for (uint32_t i = 0; i < count && at + 4 <= packet.size(); ++i) {
        const uint32_t length = Le32(&packet[at]); at += 4;
        if (length > packet.size() - at) return false;
        const std::string_view field(reinterpret_cast<const char*>(&packet[at]), length);
        at += length;
        const size_t equals = field.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = field.substr(0, equals);
        const bool block = _strnicmp(key.data(), "METADATA_BLOCK_PICTURE", key.size()) == 0 && key.size() == 22;
        const bool legacy = _strnicmp(key.data(), "COVERART", key.size()) == 0 && key.size() == 8;
        if (!block && !legacy) continue;
        std::vector<uint8_t> decoded;
        if (!Base64Decode(field.substr(equals + 1), decoded)) continue;
        if (legacy) { out.assign(decoded.begin(), decoded.end()); return out.size() >= 16; }
        if (FlacPicture(decoded.data(), decoded.size(), out)) return true;
    }
    return false;
}

static bool FlacCover(HANDLE file, uint64_t offset, std::vector<unsigned char>& out) {
    uint8_t magic[4];
    if (!ReadAt(file, offset, magic, 4) || memcmp(magic, "fLaC", 4) != 0) return false;
    uint64_t pos = offset + 4;
    for (int blocks = 0; blocks < 128; ++blocks) {
        uint8_t header[4];
        if (!ReadAt(file, pos, header, 4)) return false;
        const bool last = (header[0] & 0x80) != 0;
        const uint32_t length = (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | header[3];
        if ((header[0] & 0x7F) == 6 && length >= 32 && length <= kMaxCoverBytes) {
            std::vector<uint8_t> block(length);
            if (!ReadAt(file, pos + 4, block.data(), length)) return false;
            return FlacPicture(block.data(), block.size(), out);
        }
        pos += 4 + length;
        if (last) break;
    }
    return false;
}

// One MP4 atom header at `pos`: its type, the payload start and the end.
static bool Mp4Atom(HANDLE file, uint64_t pos, uint64_t limit, char type[4],
                    uint64_t& payload, uint64_t& end) {
    if (pos + 8 > limit) return false;
    uint8_t header[16];
    if (!ReadAt(file, pos, header, 8)) return false;
    uint64_t size = Be32(header);
    memcpy(type, header + 4, 4);
    payload = pos + 8;
    if (size == 1) {
        if (pos + 16 > limit || !ReadAt(file, pos + 8, header + 8, 8)) return false;
        size = (uint64_t(Be32(header + 8)) << 32) | Be32(header + 12);
        payload = pos + 16;
    } else if (size == 0) {
        size = limit - pos;
    }
    if (size < payload - pos || pos + size > limit) return false;
    end = pos + size;
    return true;
}
static bool Mp4Child(HANDLE file, uint64_t start, uint64_t limit, const char* wanted,
                     uint64_t& payload, uint64_t& end) {
    uint64_t pos = start;
    for (int atoms = 0; atoms < 4096 && pos < limit; ++atoms) {
        char type[4];
        if (!Mp4Atom(file, pos, limit, type, payload, end)) return false;
        if (memcmp(type, wanted, 4) == 0) return true;
        pos = end;
    }
    return false;
}
static bool Mp4Cover(HANDLE file, uint64_t file_size, std::vector<unsigned char>& out) {
    uint64_t moov = 0, moov_end = 0, udta = 0, udta_end = 0, meta = 0, meta_end = 0;
    uint64_t ilst = 0, ilst_end = 0, covr = 0, covr_end = 0, data = 0, data_end = 0;
    if (!Mp4Child(file, 0, file_size, "moov", moov, moov_end) ||
        !Mp4Child(file, moov, moov_end, "udta", udta, udta_end) ||
        !Mp4Child(file, udta, udta_end, "meta", meta, meta_end)) return false;
    // iTunes writes meta as a full box (4 bytes of version / flags); QuickTime
    // files put the hdlr child right away.
    uint8_t peek[8];
    if (meta + 8 <= meta_end && ReadAt(file, meta, peek, 8) && memcmp(peek + 4, "hdlr", 4) != 0) meta += 4;
    if (!Mp4Child(file, meta, meta_end, "ilst", ilst, ilst_end) ||
        !Mp4Child(file, ilst, ilst_end, "covr", covr, covr_end) ||
        !Mp4Child(file, covr, covr_end, "data", data, data_end)) return false;
    // data: 4 bytes type indicator (13 JPEG, 14 PNG, 27 BMP), 4 bytes locale.
    if (data_end < data + 8 + 16 || data_end - data - 8 > kMaxCoverBytes) return false;
    out.resize(static_cast<size_t>(data_end - data - 8));
    return ReadAt(file, data + 8, out.data(), static_cast<DWORD>(out.size()));
}

// true: the container was understood (`parsed`) and had a picture.
static bool ReadAudioCover(const std::wstring& path, const std::wstring& extension,
                           std::vector<unsigned char>& out, bool& parsed) {
    parsed = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 16) return false;
    const uint64_t file_size = static_cast<uint64_t>(size.QuadPart);
    uint8_t head[12];
    if (!ReadAt(file, 0, head, sizeof(head))) return false;
    if (memcmp(head, "ID3", 3) == 0) {
        parsed = true;
        if (Id3Cover(file, 0, out)) return true;
        // FLAC (and the odd MP4) behind a stray ID3 tag.
        const uint64_t after = 10 + uint64_t(Syncsafe(head + 6)) + ((head[5] & 0x10) ? 10 : 0);
        return FlacCover(file, after, out);
    }
    if (memcmp(head, "fLaC", 4) == 0) { parsed = true; return FlacCover(file, 0, out); }
    if (memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
        parsed = true;
        return ChunkedId3Cover(file, file_size, false, out);
    }
    if (memcmp(head, "FORM", 4) == 0 && (memcmp(head + 8, "AIFF", 4) == 0 || memcmp(head + 8, "AIFC", 4) == 0)) {
        parsed = true;
        return ChunkedId3Cover(file, file_size, true, out);
    }
    if (memcmp(head + 4, "ftyp", 4) == 0) { parsed = true; return Mp4Cover(file, file_size, out); }
    if (memcmp(head, "OggS", 4) == 0) { parsed = true; return OggCover(file, file_size, out); }
    // Plain MPEG / ADTS frames without a tag have no picture to find.
    if (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0 && (extension == L".mp3" || extension == L".aac"))
        parsed = true;
    return false;
}

static bool AcceptsAudioCover(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && !q.grid && preview::IsAudioExtension(q.extension);
}
static DecodeStep RunAudioCover(const DecodeRequest& q, DecodeResult& r) {
    std::vector<unsigned char> art;
    bool parsed = false;
    if (ReadAudioCover(q.path, q.extension, art, parsed)) {
        ClearBitmap(r);
        if (DecodeImage(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height, r.stride,
                        r.source_width, r.source_height, &art)) return MadeBitmap(r);
        ClearBitmap(r);
    }
    // Quick Look keeps the old chain (shell thumbnail, then the hex view).
    if (!(q.request.flags & ipc::kPreviewRequestFlagCard)) return DecodeStep::Next;
    // The card: a container this parser does not read (WMA, Ogg) may still
    // carry art the shell can extract; one that was read has none.
    if (!parsed && ShellThumbnailInto(q, r)) return MadeBitmap(r);
    ClearBitmap(r);
    r.error = L"audio-no-cover";
    return DecodeStep::Failed;
}

// ---- Video poster through Media Foundation -----------------------------------
// Explorer's video thumbnail provider took 0.5-2.8 s per uncached file on the
// test machine; a source reader opens the file, seeks to a key frame and
// decodes that one frame in a fraction of that. Media Foundation is loaded on
// first use: Windows N editions ship without it, and grid thumbnails (cached
// by the shell) never need it.
struct MfApi {
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using CreateAttributes = HRESULT (WINAPI*)(IMFAttributes**, UINT32);
    using CreateType = HRESULT (WINAPI*)(IMFMediaType**);
    using CreateReader = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
    CreateAttributes create_attributes = nullptr;
    CreateType create_type = nullptr;
    CreateReader create_reader = nullptr;
    bool ready = false;
};
static const MfApi& Mf() {
    static MfApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!plat || !readwrite) return;   // kept loaded for the host's lifetime
        const auto startup = reinterpret_cast<MfApi::Startup>(GetProcAddress(plat, "MFStartup"));
        api.create_attributes = reinterpret_cast<MfApi::CreateAttributes>(GetProcAddress(plat, "MFCreateAttributes"));
        api.create_type = reinterpret_cast<MfApi::CreateType>(GetProcAddress(plat, "MFCreateMediaType"));
        api.create_reader = reinterpret_cast<MfApi::CreateReader>(
            GetProcAddress(readwrite, "MFCreateSourceReaderFromURL"));
        api.ready = startup && api.create_attributes && api.create_type && api.create_reader &&
            SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE));
    });
    return api;
}

static bool MfVideoFrameInto(const DecodeRequest& q, DecodeResult& r) {
    const MfApi& mf = Mf();
    if (!mf.ready) return false;
    std::wstring url = q.path;
    if (url.rfind(L"\\\\?\\UNC\\", 0) == 0) url = L"\\\\" + url.substr(8);
    else if (url.rfind(L"\\\\?\\", 0) == 0) url = url.substr(4);
    ComPtr<IMFAttributes> attributes;
    if (FAILED(mf.create_attributes(&attributes, 1))) return false;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(mf.create_reader(url.c_str(), attributes.Get(), &reader))) return false;
    const DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    ComPtr<IMFMediaType> wanted;
    if (FAILED(reader->SetStreamSelection(video, TRUE)) || FAILED(mf.create_type(&wanted)) ||
        FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType(video, nullptr, wanted.Get()))) return false;
    PROPVARIANT value;
    PropVariantInit(&value);
    uint64_t duration = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &value)) && value.vt == VT_UI8)
        duration = value.uhVal.QuadPart;
    PropVariantClear(&value);
    // Past the fade-in most clips open with: a tenth in, at most 5 s. The
    // reader lands on the key frame before it, so this costs one GOP at most.
    if (duration >= 20000000ull) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>((std::min)(duration / 10, 50000000ull));
        reader->SetCurrentPosition(GUID_NULL, position);
    }
    ComPtr<IMFSample> sample;
    for (int tries = 0; tries < 16 && !sample; ++tries) {
        DWORD index = 0, flags = 0;
        LONGLONG timestamp = 0;
        if (FAILED(reader->ReadSample(video, 0, &index, &flags, &timestamp, &sample))) return false;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
    }
    ComPtr<IMFMediaType> actual;
    if (!sample || FAILED(reader->GetCurrentMediaType(video, &actual))) return false;
    UINT32 width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        !width || !height || width > 16384 || height > 16384) return false;
    UINT32 left = 0, top = 0, shown_w = width, shown_h = height;
    MFVideoArea area{};
    if (SUCCEEDED(actual->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                  sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
        UINT32(area.OffsetX.value + area.Area.cx) <= width && UINT32(area.OffsetY.value + area.Area.cy) <= height) {
        left = static_cast<UINT32>(area.OffsetX.value); top = static_cast<UINT32>(area.OffsetY.value);
        shown_w = static_cast<UINT32>(area.Area.cx); shown_h = static_cast<UINT32>(area.Area.cy);
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    std::vector<uint8_t> canvas(static_cast<size_t>(shown_w) * shown_h * 4);
    ComPtr<IMF2DBuffer> planar;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    bool locked2d = SUCCEEDED(buffer.As(&planar)) && SUCCEEDED(planar->Lock2D(&scan0, &pitch));
    BYTE* data = nullptr;
    DWORD length = 0;
    if (!locked2d) {
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        pitch = static_cast<LONG>(MFGetAttributeUINT32(actual.Get(), MF_MT_DEFAULT_STRIDE, width * 4));
        scan0 = pitch < 0 ? data + static_cast<size_t>(-pitch) * (height - 1) : data;
        if (static_cast<uint64_t>(pitch < 0 ? -pitch : pitch) * height > length) { buffer->Unlock(); return false; }
    }
    for (UINT32 y = 0; y < shown_h; ++y) {
        const BYTE* row = scan0 + static_cast<ptrdiff_t>(pitch) * static_cast<ptrdiff_t>(top + y) + left * 4;
        uint8_t* dst = canvas.data() + static_cast<size_t>(y) * shown_w * 4;
        memcpy(dst, row, static_cast<size_t>(shown_w) * 4);
        for (UINT32 x = 0; x < shown_w; ++x) dst[x * 4 + 3] = 0xFF;   // RGB32's X byte is not alpha
    }
    if (locked2d) planar->Unlock2D(); else buffer->Unlock();
    // Anamorphic video (DV, some broadcast captures): the display shape.
    UINT32 par_n = 1, par_d = 1;
    MFGetAttributeRatio(actual.Get(), MF_MT_PIXEL_ASPECT_RATIO, &par_n, &par_d);
    UINT32 display_w = shown_w;
    if (par_n && par_d && par_n != par_d)
        display_w = (std::max)(1u, static_cast<UINT32>(static_cast<uint64_t>(shown_w) * par_n / par_d));
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    ClearBitmap(r);
    if (display_w != shown_w) {
        // Stretch to the display width first, then fit the cap.
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICBitmapScaler> scaler;
        std::vector<uint8_t> wide(static_cast<size_t>(display_w) * shown_h * 4);
        if (FAILED(factory->CreateBitmapFromMemory(shown_w, shown_h, GUID_WICPixelFormat32bppPBGRA, shown_w * 4,
                static_cast<UINT>(canvas.size()), canvas.data(), &bitmap)) ||
            FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(bitmap.Get(), display_w, shown_h, WICBitmapInterpolationModeFant)) ||
            FAILED(scaler->CopyPixels(nullptr, display_w * 4, static_cast<UINT>(wide.size()), wide.data())))
            return false;
        canvas.swap(wide);
        shown_w = display_w;
    }
    if (!FitCanvas(factory.Get(), canvas, shown_w, shown_h, q.cap, r.pixels, r.width, r.height, r.stride)) {
        ClearBitmap(r);
        return false;
    }
    r.source_width = shown_w;
    r.source_height = shown_h;
    r.duration_ms = static_cast<uint32_t>(duration / 10000);
    return true;
}

// Details pane, Quick Look and the hover card: Explorer's cached thumbnail if
// there is one (instant), else the frame from Media Foundation. Formats it
// cannot read continue to the media pack and the shell provider as before.
static bool AcceptsVideoFrame(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && !q.grid && preview::IsVideoExtension(q.extension);
}
static DecodeStep RunVideoFrame(const DecodeRequest& q, DecodeResult& r) {
    if (CachedShellThumbnailInto(q, r)) return MadeBitmap(r);
    ClearBitmap(r);
    if (MfVideoFrameInto(q, r)) return MadeBitmap(r);
    ClearBitmap(r);
    return DecodeStep::Next;
}

// Folders and the formats Windows previews well (Office, video, CAD).
static bool AcceptsShellPreview(const DecodeRequest& q) {
    return q.is_directory || IsKnownShellPreview(q.extension);
}
static DecodeStep RunShellPreview(const DecodeRequest& q, DecodeResult& r) {
    // Grid thumbnails only, except for formats Windows cannot preview at all
    // (WPS, OpenDocument), where the sketch beats an empty details pane.
    const bool sketchable = !q.is_directory && !q.offline &&
        (q.grid || preview::IsSketchOnlyExtension(q.extension)) &&
        q.cap >= preview::kOfficeSketchMinEdge && preview::IsOfficeSketchExtension(q.extension);
    const bool dwg = !q.is_directory && !q.offline && q.extension == L".dwg";
    bool made = false;
    if (dwg && q.grid) {
        // Grid thumbnails must be stable more than sharp: a CAD provider may
        // take seconds or fail intermittently (licence checks, cold start),
        // which made thumbnails flip between picture and icon. Explorer's
        // cache is instant; the preview AutoCAD embeds in the header is a
        // plain file read; only files with neither wake the provider.
        made = CachedShellThumbnailInto(q, r);
        if (!made) made = DwgHeaderThumbnailInto(q, r);
        if (!made) {
            ClearBitmap(r);
            made = ShellThumbnailInto(q, r);
        }
        if (made) return MadeBitmap(r);
        r.error = L"provider-failed";
        return DecodeStep::Failed;
    }
    if (sketchable) {
        // OOXML previews remain available without an installed Office Shell provider.
        for (const char* member : {"docProps/thumbnail.jpeg", "docProps/thumbnail.jpg", "docProps/thumbnail.png"}) {
            std::vector<unsigned char> bytes;
            if (preview::ReadZipEntry(q.path, member, 8u * 1024u * 1024u, bytes, nullptr) &&
                DecodeImage(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height, r.stride,
                            r.source_width, r.source_height, &bytes) &&
                !preview::IsBlankThumbnail(r.pixels, r.width, r.height, r.stride)) {
                made = true;
                break;
            }
        }
    }
    if (!made) made = ShellThumbnailInto(q, r);
    if (made && sketchable && preview::IsBlankThumbnail(r.pixels, r.width, r.height, r.stride)) {
        // A blank embedded thumbnail (Mac Word writes all-white ones) says
        // less than the sketch below.
        made = false;
    }
    if (!made && dwg) {
        // Large previews: an installed CAD thumbnail provider renders
        // sharper, so it wins; without one, fall back to the small preview
        // AutoCAD embeds in the drawing header.
        made = DwgHeaderThumbnailInto(q, r);
    }
    if (!made && sketchable) {
        // Word / Excel files rarely embed a (non-blank) thumbnail; grid
        // thumbnails then get a sketch of the first page drawn from the
        // document's own text. source_width/height stay 0: the sketch has no
        // pixel size of its own to report.
        r.pixels.clear();
        r.width = r.height = r.stride = r.source_width = r.source_height = 0;
        made = preview::RenderOfficeSketch(q.path, q.extension, q.cap, r.pixels, r.width, r.height,
            r.stride, nullptr);
    }
    if (made) return MadeBitmap(r);
    r.error = L"provider-failed";
    return DecodeStep::Failed;
}

// Any other format with a registered shell thumbnail handler.
static bool AcceptsShellHandler(const DecodeRequest& q) {
    return !IsKnownText(q.extension) && !q.offline && HasShellThumbnailHandler(q.extension);
}
static DecodeStep RunShellHandler(const DecodeRequest& q, DecodeResult& r) {
    if (ShellThumbnailInto(q, r)) return MadeBitmap(r);
    // Handler present but it declined this file: keep the old view.
    if (TextOrHexInto(q, r)) return DecodeStep::Made;
    r.error = L"content-read-failed";
    return DecodeStep::Failed;
}

// Files whose extension names nothing we or the shell can preview - AutoCAD
// backups (.bak / .sv$), renamed fonts, images or PDFs - are recognised by
// their signature and previewed as what they really are. Unrecognised content
// continues to the text / hex view.
static bool AcceptsSniffed(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && !IsKnownText(q.extension) &&
        !preview::IsNativeExtension(q.extension) && !IsKnownShellPreview(q.extension) &&
        !preview::IsOfficeSketchExtension(q.extension) && !HasShellThumbnailHandler(q.extension);
}
static DecodeStep RunSniffed(const DecodeRequest& q, DecodeResult& r) {
    bool made = false;
    switch (preview::SniffContent(q.path)) {
    case preview::SniffedFormat::Dwg:
        if (DwgHeaderThumbnailInto(q, r)) return MadeBitmap(r);
        // A drawing saved without a preview: nothing readable to show, and a
        // hex dump of DWG says less than the file icon.
        ClearBitmap(r);
        r.error = L"dwg-no-preview";
        return DecodeStep::Failed;
    case preview::SniffedFormat::Font:
        made = preview::RasterizeFontFile(q.path, q.cap, r.pixels, r.width, r.height, r.stride, nullptr);
        break;
    case preview::SniffedFormat::Rtf:
        return RtfInto(q, r) ? DecodeStep::Made : DecodeStep::Next;
    case preview::SniffedFormat::Pdf:
        made = preview::RasterizePdfFile(q.path, q.cap, r.pixels, r.width, r.height, r.stride,
                                         r.source_width, r.source_height, &r.error, 0, nullptr, q.grid);
        if (!made && r.error == L"pdf-thumbnail-budget") return DecodeStep::Failed;
        break;
    case preview::SniffedFormat::Archive:
        if (!MakeEnhancedArchiveListing(q.path, r.text, r.bytes_read, &r.error, !q.grid)) {
            r.text.clear();
            r.bytes_read = 0;
            return DecodeStep::Next;
        }
        r.kind = ipc::PreviewContentKind::Archive;
        return DecodeStep::Made;
    case preview::SniffedFormat::Image:
        made = DecodeImage(q.path, q.request.attrs, q.cap, r.pixels, r.width, r.height, r.stride,
                           r.source_width, r.source_height);
        break;
    default:
        return DecodeStep::Next;
    }
    if (made) return MadeBitmap(r);
    ClearBitmap(r);
    return DecodeStep::Next;
}

// Last resort for everything: text, or a hex dump of binary data.
static bool AcceptsPe(const DecodeRequest& q) {
    return !q.grid && !q.offline && !q.is_directory &&
        preview::IsFamilyExtension(preview::PreviewFamily::Pe, q.extension);
}
static DecodeStep RunPe(const DecodeRequest& q, DecodeResult& r) {
    std::vector<uint8_t> bytes;
    // Bound both I/O and memory; oversized files keep the existing fallback.
    if (!ReadFileBytes(q.path, 64u * 1024u * 1024u, bytes) ||
        !preview::MakePeDocument(q.path, bytes, r.text, r.truncated)) return DecodeStep::Next;
    r.bytes_read = static_cast<uint32_t>(bytes.size());
    r.kind = ipc::PreviewContentKind::Text;
    return DecodeStep::Made;
}

static bool AcceptsAny(const DecodeRequest&) { return true; }
static DecodeStep RunTextOrHex(const DecodeRequest& q, DecodeResult& r) {
    if (TextOrHexInto(q, r)) return DecodeStep::Made;
    r.error = L"content-read-failed";
    return DecodeStep::Failed;
}

// Markdown in Quick Look: parsed here (untrusted input stays in the sandboxed
// host) into blocks the UI lays out. Elsewhere it stays plain text.
static bool AcceptsMarkdown(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           IsOneOf(q.extension, {L".md", L".markdown", L".mdown", L".mkd", L".mkdn"});
}
static DecodeStep RunMarkdown(const DecodeRequest& q, DecodeResult& r) {
    if (!TextOrHexInto(q, r) || r.kind != ipc::PreviewContentKind::Text) return DecodeStep::Next;
    std::wstring payload;
    if (preview::MakeMarkdownDocument(r.text, payload)) {
        r.text = std::move(payload);
        r.kind = ipc::PreviewContentKind::Markdown;
    }
    return DecodeStep::Made;
}

// CSV/TSV and XLSX in Quick Look: a grid payload. Needs the RichText flag
// (elsewhere CSV stays text and XLSX keeps its shell thumbnail/preview); a
// workbook that cannot be read falls through to the shell preview.
static bool AcceptsTable(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           (preview::IsTableExtension(q.extension) || preview::IsSpreadsheetExtension(q.extension));
}
static DecodeStep RunTable(const DecodeRequest& q, DecodeResult& r) {
    std::wstring payload;
    if (preview::IsSpreadsheetExtension(q.extension)) {
        uint32_t bytes = 0;
        if (!preview::MakeXlsxTable(q.path, payload, bytes, q.request.frame_index)) {
            r.error = L"sheet-read-failed";
            return DecodeStep::Failed;
        }
        r.bytes_read = bytes;
        r.truncated = false;
    } else {
        ipc::PreviewTextEncoding encoding = ipc::PreviewTextEncoding::Unknown;
        if (!preview::MakeCsvTable(q.path, q.extension, payload, r.bytes_read, r.truncated, encoding))
            return DecodeStep::Next;
        r.text_encoding = encoding;
    }
    r.text = std::move(payload);
    r.kind = ipc::PreviewContentKind::Table;
    return DecodeStep::Made;
}

// Jupyter notebooks in Quick Look: converted to the Markdown payload (cells,
// text outputs, pictures). Code is never run. Not a notebook: next decoder.
static bool AcceptsNotebook(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           preview::IsNotebookExtension(q.extension);
}
static DecodeStep RunNotebook(const DecodeRequest& q, DecodeResult& r) {
    std::wstring payload;
    ipc::PreviewTextEncoding encoding = ipc::PreviewTextEncoding::Unknown;
    if (!preview::MakeNotebookDocument(q.path, payload, r.bytes_read, r.truncated, encoding))
        return DecodeStep::Next;
    r.text_encoding = encoding;
    r.text = std::move(payload);
    r.kind = ipc::PreviewContentKind::Markdown;
    return DecodeStep::Made;
}

// JSON and XML in Quick Look: a node tree payload (a parse error still makes
// one, carrying the source). Binary plists and the like fall through.
static bool AcceptsTree(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           (preview::IsJsonExtension(q.extension) || preview::IsXmlExtension(q.extension));
}
static DecodeStep RunTree(const DecodeRequest& q, DecodeResult& r) {
    std::wstring payload;
    ipc::PreviewTextEncoding encoding = ipc::PreviewTextEncoding::Unknown;
    if (!preview::MakeTreeDocument(q.path, q.extension, payload, r.bytes_read, r.truncated, encoding))
        return DecodeStep::Next;
    r.text_encoding = encoding;
    r.text = std::move(payload);
    r.kind = ipc::PreviewContentKind::Tree;
    return DecodeStep::Made;
}

// DOCX and EPUB in Quick Look: converted to the Markdown block payload (EPUB
// with chapters and contents). A document that does not convert falls
// through to the shell preview and thumbnail providers.
static bool AcceptsDocx(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           preview::IsDocxExtension(q.extension);
}
static DecodeStep RunDocx(const DecodeRequest& q, DecodeResult& r) {
    std::wstring payload;
    if (!preview::MakeDocxDocument(q.path, payload, r.bytes_read, r.truncated)) return DecodeStep::Next;
    r.error = L"docx-reading";
    r.text = std::move(payload);
    r.kind = ipc::PreviewContentKind::Markdown;
    return DecodeStep::Made;
}
static bool AcceptsEpub(const DecodeRequest& q) {
    return !q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagRichText) != 0 &&
           preview::IsEpubExtension(q.extension);
}
static DecodeStep RunEpub(const DecodeRequest& q, DecodeResult& r) {
    std::wstring payload;
    if (!preview::MakeEpubDocument(q.path, payload, r.bytes_read, r.truncated)) return DecodeStep::Next;
    r.text = std::move(payload);
    r.kind = ipc::PreviewContentKind::Markdown;
    return DecodeStep::Made;
}

// Folders in Quick Look: a contents listing drawn by the archive tree view.
// Everywhere else (grid, details pane) they keep the shell icon below.
static bool AcceptsFolder(const DecodeRequest& q) {
    return q.is_directory && !q.offline &&
           (q.request.flags & ipc::kPreviewRequestFlagFolderListing) != 0;
}
static DecodeStep RunFolder(const DecodeRequest& q, DecodeResult& r) {
    const bool final_pass = q.request.frame_index != 0;
    if (!preview::MakeFolderListing(q.path, final_pass ? 3000u : 400u, final_pass, r.text))
        return DecodeStep::Next;
    r.kind = ipc::PreviewContentKind::Archive;
    r.bytes_read = 0;
    return DecodeStep::Made;
}

// Shortcuts (.lnk): the preview of the file they point to, as Explorer shows
// it. The link is read, never resolved (Resolve may search the disk or wake a
// network share); network targets, folders and programs keep the link icon.
static bool AcceptsShortcut(const DecodeRequest& q) {
    return !q.is_directory && !q.offline && q.extension == L".lnk";
}
static bool ShortcutTarget(const std::wstring& link, std::wstring& target) {
    ComPtr<IShellLinkW> shell_link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link))) ||
        FAILED(shell_link.As(&file)) || FAILED(file->Load(link.c_str(), STGM_READ)))
        return false;
    wchar_t raw[MAX_PATH]{};
    if (FAILED(shell_link->GetPath(raw, ARRAYSIZE(raw), nullptr, SLGP_RAWPATH)) || !raw[0]) return false;
    wchar_t expanded[MAX_PATH]{};
    const DWORD n = ExpandEnvironmentStringsW(raw, expanded, ARRAYSIZE(expanded));
    target = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : raw;
    return true;
}
static DecodeStep RunShortcut(const DecodeRequest& q, DecodeResult& r) {
    std::wstring target;
    if (!ShortcutTarget(q.path, target)) {
        r.error = L"shortcut-no-target";     // shell namespace targets (Control Panel, apps...)
        return DecodeStep::Failed;
    }
    if (PathIsNetworkPathW(target.c_str())) {
        r.error = L"shortcut-network-target";
        return DecodeStep::Failed;
    }
    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        r.error = L"shortcut-target-missing";
        return DecodeStep::Failed;
    }
    const std::wstring extension = ExtensionOf(target);
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 || IsOneOf(extension, {
            L".lnk", L".exe", L".dll", L".com", L".scr", L".msc", L".cpl", L".sys", L".url", L".appref-ms"})) {
        r.error = L"shortcut-to-program";
        return DecodeStep::Failed;
    }
    ipc::PreviewRequest inner = q.request;
    inner.attrs = attrs;
    const DecodeRequest target_request{inner, target, extension, false, IsOfflinePlaceholder(attrs), q.cap, q.grid};
    return preview::DecodeContent(target_request, r) ? DecodeStep::Made : DecodeStep::Failed;
}

// Order is behaviour: it is the order the formats were tested in before the
// table existed. New families go before "shell-preview" unless the shell
// provider must win.
static const preview::DecoderEntry kDecoders[] = {
    { "folder",          AcceptsFolder,       RunFolder },
    { "shortcut",        AcceptsShortcut,     RunShortcut },
    { "image",           AcceptsImage,        RunImage },
    { "image-pack",      AcceptsImagePack,    RunImagePack },
    { "raw-pack",        AcceptsRawPack,      RunRawPack },
    { "svg",             AcceptsVector,       RunVector },
    { "metafile",        AcceptsMetaFile,     RunMetaFile },
    { "pdf",             AcceptsPdf,          RunPdf },
    { "archive",         AcceptsArchive,      RunArchive },
    { "markdown",        AcceptsMarkdown,     RunMarkdown },
    { "table",           AcceptsTable,        RunTable },
    { "notebook",        AcceptsNotebook,     RunNotebook },
    { "tree",            AcceptsTree,         RunTree },
    { "docx",            AcceptsDocx,         RunDocx },
    { "epub",            AcceptsEpub,         RunEpub },
    { "psd",             AcceptsPsd,          RunPsd },
    { "font",            AcceptsFont,         RunFont },
    { "rtf",             AcceptsRtf,          RunRtf },
    { "audio-cover",     AcceptsAudioCover,   RunAudioCover },
    { "video-frame",     AcceptsVideoFrame,   RunVideoFrame },
    { "media-pack",      AcceptsMediaPack,    RunMediaPack },
    { "pe",              AcceptsPe,           RunPe },
    { "shell-preview",   AcceptsShellPreview, RunShellPreview },
    { "content-sniff",   AcceptsSniffed,      RunSniffed },
    { "shell-thumbnail", AcceptsShellHandler, RunShellHandler },
    { "text-or-hex",     AcceptsAny,          RunTextOrHex },
};

bool pulse::preview::DecodeContent(const DecodeRequest& request, DecodeResult& result) {
    return RunDecoders(kDecoders, request, result);
}
