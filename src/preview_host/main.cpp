#include "../ipc/preview_protocol.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shellapi.h>
#include <thumbcache.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

using namespace pulse;
using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kRecallOnOpen = 0x00040000; // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr DWORD kRecallOnData = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
constexpr DWORD kPinned = 0x00080000; // FILE_ATTRIBUTE_PINNED

bool IsOfflinePlaceholder(DWORD attrs) {
    return (attrs & (kRecallOnOpen | kRecallOnData)) && !(attrs & kPinned);
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

// Shell APIs (SHCreateItemFromParsingName, SHGetPropertyStoreFromParsingName)
// reject \\?\ extended paths; the fs layer hands them out for long-path support.
std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

bool IsOneOf(std::wstring_view extension,
             std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

bool IsKnownText(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
        L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
        L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
        L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
        L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
        L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties"
    });
}

bool IsDirectImage(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
        L".webp", L".heic", L".ico"
    });
}

bool IsKnownShellPreview(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".pdf", L".doc", L".docx", L".xls",
        L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp", L".mp4",
        L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v", L".dwg",
        L".dxf", L".step", L".stp", L".iges", L".igs"
    });
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

bool DecodeText(const std::vector<uint8_t>& bytes, std::wstring& text) {
    if (bytes.empty()) { text.clear(); return true; }
    size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        offset = 2;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
    } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        offset = 2;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((bytes[i] << 8) | bytes[i + 1]));
    } else {
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            offset = 3;
        const char* raw = reinterpret_cast<const char*>(bytes.data() + offset);
        const int raw_size = static_cast<int>(bytes.size() - offset);
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, raw_size,
                                        nullptr, 0);
        UINT code_page = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (chars <= 0) {
            code_page = CP_ACP;
            flags = 0;
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
    if (text.size() > kMaxChars) text.resize(kMaxChars);
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
                   std::wstring& text, uint32_t& bytes_read, bool& truncated) {
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
    if (LooksBinary(bytes) || !DecodeText(bytes, text)) return false;
    kind = ipc::PreviewContentKind::Text;
    bytes_read = static_cast<uint32_t>(bytes.size());
    truncated = file_size > bytes.size();
    return true;
}

struct PreviewPropertyValue { std::wstring label, value; };

void AddProperty(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* label,
                 std::vector<PreviewPropertyValue>& out) {
    if (!store || out.size() >= 6) return;
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY && value.vt != VT_NULL) {
        PWSTR formatted = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &formatted)) &&
            formatted && *formatted) {
            out.push_back({ label, formatted });
        }
        CoTaskMemFree(formatted);
    }
    PropVariantClear(&value);
}

bool ReadUintProperty(IPropertyStore* store, REFPROPERTYKEY key, uint32_t& value) {
    if (!store) return false;
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_UI4) { value = pv.uintVal; ok = true; }
        else if (pv.vt == VT_I4 && pv.lVal > 0) { value = static_cast<uint32_t>(pv.lVal); ok = true; }
    }
    PropVariantClear(&pv);
    return ok;
}

std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path) {
    std::vector<PreviewPropertyValue> out;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store)))) return out;
    const std::wstring extension = ExtensionOf(path);
    if (IsOneOf(extension, {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic"})) {
        AddProperty(store.Get(), PKEY_Image_Dimensions, L"尺寸", out);
        AddProperty(store.Get(), PKEY_Photo_DateTaken, L"拍摄时间", out);
        AddProperty(store.Get(), PKEY_Photo_CameraModel, L"相机", out);
    } else if (IsOneOf(extension, {L".mp4", L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        uint32_t width = 0, height = 0;
        if (out.size() < 6 && ReadUintProperty(store.Get(), PKEY_Video_FrameWidth, width) &&
            ReadUintProperty(store.Get(), PKEY_Video_FrameHeight, height) &&
            width > 0 && height > 0) {
            wchar_t dims[64];
            swprintf_s(dims, L"%u x %u", width, height);
            out.push_back({ L"分辨率", dims });
        }
        AddProperty(store.Get(), PKEY_Video_FrameRate, L"帧率", out);
        AddProperty(store.Get(), PKEY_Video_Compression, L"编码格式", out);
    } else if (IsOneOf(extension, {L".mp3", L".wav", L".flac", L".m4a", L".aac"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        AddProperty(store.Get(), PKEY_Music_Artist, L"艺术家", out);
        AddProperty(store.Get(), PKEY_Music_AlbumTitle, L"专辑", out);
    } else if (IsOneOf(extension, {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp"})) {
        AddProperty(store.Get(), PKEY_Title, L"标题", out);
        AddProperty(store.Get(), PKEY_Author, L"作者", out);
        AddProperty(store.Get(), PKEY_Document_PageCount, L"页数", out);
    }
    return out;
}

bool WriteString(HANDLE pipe, const std::wstring& value) {
    const uint32_t chars = static_cast<uint32_t>(value.size());
    return ipc::WriteAll(pipe, &chars, sizeof(chars)) &&
        (chars == 0 || ipc::WriteAll(pipe, value.data(), chars * sizeof(wchar_t)));
}

} // namespace

static bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                        std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride) {
    if (IsOfflinePlaceholder(attrs)) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0) return false;

    IWICBitmapSource* source = frame.Get();
    const UINT longest = (std::max)(sourceWidth, sourceHeight);
    if (longest > pixels) {
        const double ratio = static_cast<double>(pixels) / longest;
        const UINT scaledWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * ratio + 0.5));
        const UINT scaledHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * ratio + 0.5));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), scaledWidth, scaledHeight,
                                      WICBitmapInterpolationModeFant))) return false;
        source = scaler.Get();
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

static bool MakeShellThumbnail(const std::wstring& path, DWORD attrs, UINT pixels,
                               std::vector<uint8_t>& out, UINT& width, UINT& height,
                               UINT& stride) {
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    if (__argc < 2) return 2;
    const DWORD owner = wcstoul(__wargv[1], nullptr, 10);
    const std::wstring pipeName = ipc::PreviewPipeName(owner);
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64*1024, 64*1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 4; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        ipc::PreviewRequest req;
        if (!ipc::ReadAll(pipe, &req, sizeof(req)) || req.magic!=ipc::kPreviewMagic ||
            req.path_chars==0 || req.path_chars>32768) break;
        std::wstring path(req.path_chars, L'\0');
        if (!ipc::ReadAll(pipe, path.data(), req.path_chars*sizeof(wchar_t))) break;
        std::vector<uint8_t> pixels;
        std::wstring previewText;
        std::wstring errorText;
        std::vector<PreviewPropertyValue> properties;
        UINT w=0,h=0,stride=0;
        ipc::PreviewResponse response{};
        response.request_id=req.request_id;
        response.generation=req.generation;

        bool made = false;
        if (req.kind == ipc::PreviewRequestKind::Properties) {
            if (!IsOfflinePlaceholder(req.attrs)) properties = ReadProperties(path);
            response.property_count = static_cast<uint32_t>(properties.size());
            response.status = 0;
            made = true;
        } else {
            const std::wstring extension = ExtensionOf(path);
            bool truncated = false;
            uint32_t bytesRead = 0;
            const bool isDirectory = (req.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (IsDirectImage(extension)) {
                made = DecodeImage(path, req.attrs, std::clamp(req.pixel_size, 32u, 512u),
                                   pixels, w, h, stride);
                if (made) response.kind = ipc::PreviewContentKind::Bitmap;
                else errorText = L"image-decode-failed";
            } else if (isDirectory || IsKnownShellPreview(extension)) {
                made = MakeShellThumbnail(path, req.attrs,
                                          std::clamp(req.pixel_size, 32u, 512u),
                                          pixels, w, h, stride);
                if (made) response.kind = ipc::PreviewContentKind::Bitmap;
                else errorText = L"provider-failed";
            } else {
                made = MakeTextOrHex(path, req.attrs, response.kind, previewText,
                                     bytesRead, truncated);
                if (!made) errorText = L"content-read-failed";
            }
            response.status = made ? 0 : 1;
            if (!made) {
                response.kind = ipc::PreviewContentKind::Unsupported;
                if (IsOfflinePlaceholder(req.attrs)) errorText = L"offline-placeholder";
                else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                    errorText = L"path-unavailable";
            }
            response.text_chars = static_cast<uint32_t>(previewText.size());
            response.error_chars = static_cast<uint32_t>(errorText.size());
            response.bytes_read = bytesRead;
            if (truncated) response.flags |= ipc::kPreviewFlagTruncated;
        }
        response.width=w; response.height=h; response.stride=stride;
        HANDLE mapping=nullptr; void* view=nullptr; std::wstring mappingName;
        if (made && response.kind == ipc::PreviewContentKind::Bitmap) {
            mappingName=L"Local\\PulsePreviewMap-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                        std::to_wstring(req.request_id)+L"-"+std::to_wstring(GetTickCount64());
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
                static_cast<DWORD>(pixels.size()),mappingName.c_str());
            if (mapping) view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,pixels.size());
            if (!view) { response.status=2; mappingName.clear(); }
            else memcpy(view,pixels.data(),pixels.size());
        }
        response.mapping_chars=static_cast<uint32_t>(mappingName.size());
        bool ok=ipc::WriteAll(pipe,&response,sizeof(response));
        if (ok && !mappingName.empty()) ok=ipc::WriteAll(pipe,mappingName.data(),
            response.mapping_chars*sizeof(wchar_t));
        if (ok && !previewText.empty())
            ok=ipc::WriteAll(pipe,previewText.data(),response.text_chars*sizeof(wchar_t));
        if (ok && !errorText.empty())
            ok=ipc::WriteAll(pipe,errorText.data(),response.error_chars*sizeof(wchar_t));
        for (const auto& property : properties) {
            if (!ok) break;
            ok = WriteString(pipe, property.label) && WriteString(pipe, property.value);
        }
        if (ok && mapping) { unsigned char ack=0; ok=ipc::ReadAll(pipe,&ack,1); }
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping);
        if (!ok) break;
    }
    CoUninitialize(); DisconnectNamedPipe(pipe); CloseHandle(pipe); return 0;
}
