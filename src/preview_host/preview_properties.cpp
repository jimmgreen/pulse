#include "preview_properties.h"
#include "preview_file_utils.h"
#include "video_codec.h"
#include "image_pack.h"
#include "raw_pack.h"
#include "media_pack.h"
#include "../common/preview_extensions.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace pulse;
using Microsoft::WRL::ComPtr;
using preview::PreviewPropertyValue;

namespace {

void AddProperty(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* label,
                 std::vector<PreviewPropertyValue>& out) {
    if (!store || out.size() >= 6) return;
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY && value.vt != VT_NULL) {
        PWSTR formatted = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &formatted)) &&
            formatted && *formatted) {
            const std::wstring display = IsEqualPropertyKey(key, PKEY_Video_Compression)
                ? preview::VideoCodecDisplayName(formatted) : std::wstring(formatted);
            if (!display.empty()) out.push_back({ label, display });
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

} // namespace

namespace pulse::preview {

static std::vector<PreviewPropertyValue> ReadShellProperties(const std::wstring& path) {
    std::vector<PreviewPropertyValue> out;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store)))) return out;
    return ReadPropertiesFromStore(path, store.Get());
}

std::vector<PreviewPropertyValue> ReadPropertiesFromStore(const std::wstring& path, IPropertyStore* store) {
    std::vector<PreviewPropertyValue> out;
    const std::wstring extension = ExtensionOf(path);
    if (IsImageExtension(extension)) {
        AddProperty(store, PKEY_Image_Dimensions, L"尺寸", out);
        AddProperty(store, PKEY_Photo_DateTaken, L"拍摄时间", out);
        AddProperty(store, PKEY_Photo_CameraModel, L"相机", out);
    } else if (IsVideoExtension(extension)) {
        AddProperty(store, PKEY_Media_Duration, L"时长", out);
        uint32_t width = 0, height = 0;
        if (out.size() < 6 && ReadUintProperty(store, PKEY_Video_FrameWidth, width) &&
            ReadUintProperty(store, PKEY_Video_FrameHeight, height) &&
            width > 0 && height > 0) {
            wchar_t dims[64];
            swprintf_s(dims, L"%u x %u", width, height);
            out.push_back({ L"分辨率", dims });
        }
        AddProperty(store, PKEY_Video_FrameRate, L"帧率", out);
        AddProperty(store, PKEY_Video_Compression, L"编码格式", out);
    } else if (IsAudioExtension(extension)) {
        AddProperty(store, PKEY_Title, L"标题", out);
        AddProperty(store, PKEY_Music_Artist, L"艺术家", out);
        AddProperty(store, PKEY_Music_AlbumTitle, L"专辑", out);
        AddProperty(store, PKEY_Media_Duration, L"时长", out);
        AddProperty(store, PKEY_Audio_EncodingBitrate, L"比特率", out);
        AddProperty(store, PKEY_Audio_SampleRate, L"采样率", out);
    } else if (IsOneOf(extension, {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp"})) {
        AddProperty(store, PKEY_Title, L"标题", out);
        AddProperty(store, PKEY_Author, L"作者", out);
        AddProperty(store, PKEY_Document_PageCount, L"页数", out);
    }
    return out;
}

std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path) {
    std::vector<PreviewPropertyValue> out = ReadShellProperties(path);
    // FFmpeg preview pack: fill in what Windows could not read - containers
    // it has no property handler for (FLV, RMVB, APE...) or streams it has no
    // decoder for. Files the shell describes fully never start ffprobe.
    constexpr size_t kMaxRows = 6;
    const std::wstring extension = ExtensionOf(path);
    const bool video = IsMediaPackVideoExtension(extension);
    if (out.size() < kMaxRows && (video || IsMediaPackAudioExtension(extension)) && MediaPackAvailable()) {
        auto has = [&](const wchar_t* label) {
            return std::any_of(out.begin(), out.end(), [&](const PreviewPropertyValue& row) { return row.label == label; });
        };
        if (!has(L"时长") || (video && !has(L"编码格式"))) MediaPackProperties(path, out, kMaxRows);
    }
    if (out.size() < kMaxRows && IsFamilyExtension(PreviewFamily::Raw, extension) && RawPackAvailable() &&
        std::none_of(out.begin(), out.end(), [](const PreviewPropertyValue& row) { return row.label == L"尺寸"; }))
        RawPackProperties(path, out, kMaxRows);
    // Image preview pack: pictures Windows has no property handler for
    // (AVIF, JPEG XL, EXR...). Reads headers only.
    if (out.size() < kMaxRows && IsImagePackExtension(extension) && ImagePackAvailable() &&
        std::none_of(out.begin(), out.end(), [](const PreviewPropertyValue& row) { return row.label == L"尺寸"; }))
        ImagePackProperties(path, out, kMaxRows);
    return out;
}

uint32_t ReadMediaDurationMs(const std::wstring& path) {
    if (!IsOneOf(ExtensionOf(path), {L".mp4", L".mkv", L".mov", L".avi", L".webm", L".wmv",
                                     L".m4v", L".mpg", L".mpeg", L".ts", L".mts", L".m2ts",
                                     L".3gp", L".flv"}))
        return 0;
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(ShellPath(path).c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store))) || !store) return 0;
    PROPVARIANT value{};
    PropVariantInit(&value);
    uint32_t ms = 0;
    // 100 ns units; clamp to what fits the response (about 49 days).
    if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &value)) && value.vt == VT_UI8)
        ms = static_cast<uint32_t>((std::min)(value.uhVal.QuadPart / 10000ull, 0xFFFFFFFFull));
    PropVariantClear(&value);
    return ms;
}

std::array<std::wstring, 3> ReadAudioMeta(const std::wstring& path) {
    std::array<std::wstring, 3> out;
    if (!IsAudioExtension(ExtensionOf(path))) return out;
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(ShellPath(path).c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store))) || !store)
        return out;
    // Format rather than read: the shell stores the artist as a string vector,
    // so a plain VT_LPWSTR path would silently drop every multi-artist tag.
    auto read = [&](REFPROPERTYKEY key, std::wstring& value) {
        PROPVARIANT pv{};
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt != VT_EMPTY && pv.vt != VT_NULL) {
            PWSTR formatted = nullptr;
            if (SUCCEEDED(PSFormatForDisplayAlloc(key, pv, PDFF_DEFAULT, &formatted)) && formatted) {
                std::wstring text(formatted);
                // Tag editors routinely leave trailing blanks behind, and a
                // column of them makes the rows look misaligned.
                const size_t end = text.find_last_not_of(L" \t\r\n");
                value = end == std::wstring::npos ? std::wstring() : text.substr(0, end + 1);
            }
            CoTaskMemFree(formatted);
        }
        PropVariantClear(&pv);
    };
    read(PKEY_Title, out[0]);
    read(PKEY_Music_Artist, out[1]);
    read(PKEY_Music_AlbumTitle, out[2]);
    return out;
}

} // namespace pulse::preview
