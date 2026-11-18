#include "../common/windows_compat.h"
#include "ui_renderer.h"
// Keep MF header feature gates consistent with the Windows 8.1 app target
// (same as video_preview.cpp).
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x06030000
#include "preview_format_catalog.h"
#include "video_preview.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/image_pack_protocol.h"
#include "../common/ffmpeg_tool.h"
#include "../common/preview_extensions.h"
#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <thread>

namespace pulse::ui {
namespace {

const wchar_t* Pick(const wchar_t* zh, const wchar_t* en) { return l10n::Pick(zh, en); }
using l10n::Get;
using l10n::StringId;

template <size_t N>
void AppendTable(std::vector<std::wstring>& out, const std::wstring_view (&table)[N]) {
    for (const auto& extension : table) out.emplace_back(extension.substr(1));
}

bool Contains(const std::vector<std::wstring>& list, const std::wstring& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::vector<PreviewFormatGroup> BuildGroups() {
    namespace f = preview::formats;
    std::vector<PreviewFormatGroup> groups;
    std::vector<std::wstring> used;
    auto add = [&](const wchar_t* zh, const wchar_t* en, const wchar_t* note_zh,
                   const wchar_t* note_en, std::vector<std::wstring> extensions) {
        PreviewFormatGroup group{Pick(zh, en), Pick(note_zh, note_en), {}};
        for (auto& extension : extensions) {
            if (Contains(used, extension)) continue;
            used.push_back(extension);
            group.extensions.push_back(std::move(extension));
        }
        if (!group.extensions.empty()) groups.push_back(std::move(group));
    };

    std::vector<std::wstring> images;
    AppendTable(images, f::kImage);
    AppendTable(images, f::kVector);
    AppendTable(images, f::kPsd);
    AppendTable(images, f::kMetaFile);
    AppendTable(images, imgpack::kExtensions);
    add(L"图片", L"Images",
        L"动图受预览限额限制；TIFF 可翻页；SVG 不支持的内容显示源码；图像增强包补充格式支持",
        L"Animation has preview limits; TIFF supports pages; unsupported SVG shows source; the image pack adds formats",
        std::move(images));

    std::vector<std::wstring> raw;
    AppendTable(raw, f::kRaw);
    add(L"RAW 相机照片", L"Camera RAW",
        L"优先使用系统解码；RAW 增强包补充支持，列表只读取内嵌预览，兼容性取决于相机型号",
        L"System codecs first; the RAW pack adds support. Grid previews use embedded images; compatibility depends on the camera model",
        std::move(raw));

    std::vector<std::wstring> documents;
    AppendTable(documents, f::kPdfRaster);
    for (const wchar_t* extension : {L"md", L"docx", L"epub", L"ipynb"}) documents.emplace_back(extension);
    add(L"文档", L"Documents",
        L"PDF 分页显示；DOCX 为正文阅读转换，非原版排版；EPUB 保留无法读取的章节及原因",
        L"PDF pages; DOCX is a reading conversion, not original layout; unavailable EPUB chapters keep their place and explanation",
        std::move(documents));

    std::vector<std::wstring> data;
    for (const wchar_t* extension : {L"csv", L"tsv", L"xlsx", L"xlsm", L"json", L"xml", L"yaml", L"yml",
                                     L"toml", L"ini", L"cfg", L"conf", L"properties"})
        data.emplace_back(extension);
    add(L"表格与数据", L"Tables & data",
        L"CSV / TSV 为表格；XLSX / XLSM 按需读取所选工作表；JSON / XML 为树",
        L"CSV / TSV tables; XLSX / XLSM load the selected worksheet on demand; JSON / XML trees", std::move(data));

    std::vector<std::wstring> text;
    AppendTable(text, f::kText);
    add(L"代码与文本", L"Code & text", L"语法高亮", L"Syntax highlighting", std::move(text));

    // These are playback candidates, not a guarantee of installed system support.
    std::vector<std::wstring> media;
    AppendTable(media, f::kVideo);
    AppendTable(media, f::kAudio);
    AppendTable(media, ffmpeg::kVideoExtensions);
    AppendTable(media, ffmpeg::kAudioExtensions);
    add(L"音视频", L"Audio & video",
        L"由系统解码器播放；媒体增强包或已有 FFmpeg 补充解码支持",
        L"Playback uses system codecs; the media pack or your FFmpeg adds decoding support",
        std::move(media));

    std::vector<std::wstring> archives;
    AppendTable(archives, f::kArchive);
    const auto& archive_note = l10n::Get(l10n::StringId::ArchivePluginRequired);
    add(L"压缩包与镜像", L"Archives & images", archive_note.c_str(), archive_note.c_str(), std::move(archives));

    std::vector<std::wstring> fonts;
    AppendTable(fonts, f::kFont);
    add(L"字体", L"Fonts", L"显示样张，无需安装",
        L"Specimen page without installing", std::move(fonts));
    // Keep media adjacent to images without changing extension deduplication.
    std::rotate(groups.begin() + 1, groups.begin() + 5, groups.begin() + 6);
    return groups;
}

struct GroupCache {
    std::mutex mutex;
    l10n::Language language = l10n::Language::System;
    bool built = false;
    std::vector<PreviewFormatGroup> groups;
};
GroupCache& Cache() { static GroupCache cache; return cache; }

bool HasPackage(std::initializer_list<const wchar_t*> prefixes) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT,
            L"Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
            0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t name[256];
    for (DWORD i = 0; !found; ++i) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        for (const wchar_t* prefix : prefixes)
            if (_wcsnicmp(name, prefix, wcslen(prefix)) == 0) { found = true; break; }
    }
    RegCloseKey(key);
    return found;
}

// Media Foundation subtype GUIDs built from their FOURCC so no newer SDK
// gate or mfuuid.lib is needed.
constexpr GUID VideoSubtype(DWORD fourcc) {
    return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}
constexpr DWORD FourCc(char a, char b, char c, char d) {
    return static_cast<DWORD>(static_cast<unsigned char>(a)) |
           (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
}

// Bit 0: HEVC decoder, bit 1: AV1 decoder (any software, hardware or Store MFT).
unsigned VideoDecoders() {
    HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!plat) return 0;
    unsigned mask = 0;
    using Startup = HRESULT (WINAPI*)(ULONG, DWORD);
    using Shutdown = HRESULT (WINAPI*)();
    using EnumEx = HRESULT (WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                     const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
    const auto startup = reinterpret_cast<Startup>(GetProcAddress(plat, "MFStartup"));
    const auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(plat, "MFShutdown"));
    const auto enumerate = reinterpret_cast<EnumEx>(GetProcAddress(plat, "MFTEnumEx"));
    if (startup && shutdown && enumerate && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE))) {
        constexpr GUID kVideoDecoderCategory{0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
        const GUID video_major = VideoSubtype(FourCc('v', 'i', 'd', 's'));
        const GUID subtypes[] = {VideoSubtype(FourCc('H', 'E', 'V', 'C')), VideoSubtype(FourCc('A', 'V', '0', '1'))};
        for (unsigned i = 0; i < 2; ++i) {
            const MFT_REGISTER_TYPE_INFO input{video_major, subtypes[i]};
            IMFActivate** found = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(enumerate(kVideoDecoderCategory, MFT_ENUM_FLAG_ALL, &input, nullptr, &found, &count))) {
                if (count > 0) mask |= 1u << i;
                for (UINT32 k = 0; k < count; ++k)
                    if (found[k]) found[k]->Release();
                CoTaskMemFree(found);
            }
        }
        shutdown();
    }
    FreeLibrary(plat);
    return mask;
}

std::atomic<unsigned> g_codecs{0};
std::atomic<bool> g_probe_running{false};
std::atomic<bool> g_probe_again{false};

unsigned ProbeCodecs() {
    const unsigned decoders = VideoDecoders();
    unsigned mask = kPreviewCodecsDetected;
    if (HasPackage({L"Microsoft.HEIFImageExtension_"})) mask |= 1u << 0;
    if ((decoders & 1u) || HasPackage({L"Microsoft.HEVCVideoExtension_", L"Microsoft.HEVCVideoExtensions_"}))
        mask |= 1u << 1;
    if ((decoders & 2u) || HasPackage({L"Microsoft.AV1VideoExtension_"})) mask |= 1u << 2;
    if (HasPackage({L"Microsoft.WebpImageExtension_"})) mask |= 1u << 3;
    return mask;
}

// MFTEnumEx can pump COM messages. Run on the UI thread from BuildVm, it let a
// title-bar WM_NCHITTEST re-enter BuildVm and start another probe before the
// first one finished, until the stack overflowed. One worker, never re-entered.
void RunCodecProbe(HWND notify) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        g_codecs.store(ProbeCodecs());
        if (notify) InvalidateRect(notify, nullptr, FALSE);
        if (g_probe_again.exchange(false)) continue;
        g_probe_running.store(false);
        // A refresh may have arrived between the check above and the store.
        if (!g_probe_again.exchange(false) || g_probe_running.exchange(true)) break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

float ChipWidth(const std::wstring& text, float scale) {
    return (6.6f * static_cast<float>(text.size()) + 16.0f) * scale;
}

// Rough width of a note line in the small UI font (CJK is about twice as wide).
float EstimateWidth(const std::wstring& text, float scale) {
    float width = 0;
    for (const wchar_t c : text) width += c >= 0x2E80 ? 12.5f : 6.6f;
    return width * scale;
}

} // namespace

const std::vector<PreviewFormatGroup>& PreviewFormatGroups() {
    auto& cache = Cache();
    std::lock_guard lock(cache.mutex);
    const l10n::Language language = l10n::effective_language();
    if (!cache.built || cache.language != language) {
        cache.groups = BuildGroups();
        cache.language = language;
        cache.built = true;
    }
    return cache.groups;
}

size_t PreviewFormatCount() {
    size_t count = 0;
    for (const auto& group : PreviewFormatGroups()) count += group.extensions.size();
    return count;
}

PreviewFormatState PreviewFormatSupport(const WindowViewModel& vm, size_t group, size_t index) {
    const auto& groups = PreviewFormatGroups();
    if (group >= groups.size() || index >= groups[group].extensions.size()) return {};
    const std::wstring ext = L"." + groups[group].extensions[index];
    // Ambiguous .ts is listed once in Code & text, where it needs no media pack.
    if (preview::IsTextExtension(ext)) return {};
    const bool image_pack = vm.settings_pack_images_installed && vm.settings_pack_images_enabled;
    const bool media_pack = vm.settings_pack_ffmpeg_enabled &&
        (vm.settings_pack_use_custom ? vm.settings_pack_ffmpeg == 2 : vm.settings_pack_media_installed);
    if (imgpack::IsImagePackExtension(ext)) {
        const bool heif = preview::IsOneOf(ext, {L".heic", L".heif", L".hif"});
        const bool avif = ext == L".avif";
        const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
        const bool system = detected && (heif ? (vm.settings_preview_codecs & 3u) == 3u :
                                         avif && (vm.settings_preview_codecs & 4u));
        return {heif || avif ? PreviewFormatSource::PackOrSystem : PreviewFormatSource::Pack,
                image_pack || system};
    }
    if (preview::IsFamilyExtension(preview::PreviewFamily::Raw, ext))
        return {PreviewFormatSource::PackOrSystem, vm.settings_pack_raw_installed && vm.settings_pack_raw_enabled};
    if (preview::IsArchiveExtension(ext) && !preview::IsOneOf(ext, {L".zip", L".tar", L".iso", L".udf"}))
        return {PreviewFormatSource::Plugin, Contains(vm.settings_native_archive_extensions, ext)};
    const auto in = [&](const auto& table) { return std::find(std::begin(table), std::end(table), ext) != std::end(table); };
    if ((in(ffmpeg::kVideoExtensions) || in(ffmpeg::kAudioExtensions)) &&
        !preview::IsVideoExtension(ext) && !preview::IsAudioExtension(ext))
        return {PreviewFormatSource::Pack, media_pack};
    return {};
}

PreviewCodecInfo PreviewCodec(int index) {
    switch (index) {
    case 0: return {Get(StringId::HEIFImageExtensions).c_str(),
                    Get(StringId::HEICHEIFPhotosCommonOnIPhone).c_str(),
                    L"9PMMSR1CGPWG"};
    case 1: return {Get(StringId::HEVCVideoExtensions).c_str(),
                    Get(StringId::HEVCH265VideoAndDecodingHEICPhotos).c_str(),
                    L"9NMZLZ57R3T7"};
    case 2: return {Get(StringId::AV1VideoExtension).c_str(),
                    Get(StringId::AVIFImagesAndAV1Video).c_str(), L"9MVZQVXJBQ9V"};
    default: return {Get(StringId::WebPImageExtensions).c_str(),
                     Get(StringId::WebPImagesAndAnimations).c_str(), nullptr};
    }
}

unsigned DetectPreviewCodecs(bool refresh, HWND notify) {
    const unsigned mask = g_codecs.load();
    if ((mask & kPreviewCodecsDetected) && !refresh) return mask;
    if (g_probe_running.exchange(true)) {
        if (refresh) g_probe_again.store(true);
        return mask;
    }
    try {
        std::thread(RunCodecProbe, notify).detach();
    } catch (...) {
        g_probe_running.store(false); // no thread: report "not detected" and retry next paint
    }
    return mask;
}

bool OpenPreviewCodecStore(HWND owner, int index) {
    const auto info = PreviewCodec(index);
    if (!info.store_id) return false;
    const std::wstring url = std::wstring(L"ms-windows-store://pdp/?ProductId=") + info.store_id;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

float LayoutPreviewFormats(const D2D1_RECT_F& area, float scale,
                           std::vector<PreviewFormatChip>* chips,
                           std::vector<PreviewFormatRow>* rows,
                           const std::function<float(std::wstring_view, float)>& measure_note) {
    scale *= typography::UiFontScale();
    const auto& groups = PreviewFormatGroups();
    const float left = area.left + (area.right - area.left >= 608 * scale ? 54 : 24) * scale;
    const float right = area.right - 24 * scale;
    const bool stacked = right - left < 560 * scale;
    const float chips_left = stacked ? left : left + 234 * scale;
    const float note_right = stacked ? right : left + 210 * scale;
    const float chip_height = 22 * scale, gap = 6 * scale;
    const WindowViewModel source_model{};
    float y = area.top;
    for (size_t g = 0; g < groups.size(); ++g) {
        PreviewFormatRow row{};
        row.bounds = D2D1::RectF(area.left, y, area.right, y);
        const float top = y + 14 * scale;
        row.name = D2D1::RectF(left, top, note_right, top + chip_height);
        float description_bottom = row.name.bottom;
        if (!groups[g].note.empty()) {
            const float available = (std::max)(note_right - left, 1.0f);
            const float lines = (std::max)(1.0f, std::ceil(EstimateWidth(groups[g].note, scale) / available));
            row.note = D2D1::RectF(left, row.name.bottom + 3 * scale, note_right,
                                 row.name.bottom + 3 * scale + (measure_note
                                     ? measure_note(groups[g].note, available) : 18 * lines * scale));
            description_bottom = row.note.bottom;
        }
        float chip_y = stacked ? description_bottom + 10 * scale : top;
        bool any_band = false;
        for (const bool enhanced : {false, true}) {
            float x = chips_left;
            bool any = false;
            for (size_t i = 0; i < groups[g].extensions.size(); ++i) {
                const bool is_enhanced = PreviewFormatSupport(source_model, g, i).source != PreviewFormatSource::BuiltIn;
                if (is_enhanced != enhanced) continue;
                if (!any && any_band) chip_y += chip_height + 6 * scale;
                const float w = (std::min)(ChipWidth(groups[g].extensions[i], scale), right - chips_left);
                if (x > chips_left && x + w > right) {
                    x = chips_left;
                    chip_y += chip_height + 6 * scale;
                }
                if (chips) chips->push_back({D2D1::RectF(x, chip_y, x + w, chip_y + chip_height), g, i});
                x += w + gap;
                any = true;
            }
            any_band |= any;
        }
        y = (std::max)(description_bottom, chip_y + chip_height) + 14 * scale;
        row.bounds.bottom = y;
        if (rows) rows->push_back(row);
    }
    return y + 4 * scale - area.top;
}

} // namespace pulse::ui
