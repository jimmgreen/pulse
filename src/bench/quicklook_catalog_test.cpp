#include "../ui/preview_format_catalog.h"
#include "../ui/ui_renderer.h"
#include "../ui/typography.h"
#include "../common/preview_extensions.h"
#include "../common/image_pack_protocol.h"
#include "../common/ffmpeg_tool.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace {
using namespace pulse;
using namespace pulse::ui;
int failures = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
PreviewFormatState State(const WindowViewModel& vm, const wchar_t* extension) {
    const auto& groups = PreviewFormatGroups();
    for (size_t g = 0; g < groups.size(); ++g)
        for (size_t i = 0; i < groups[g].extensions.size(); ++i)
            if (groups[g].extensions[i] == extension) return PreviewFormatSupport(vm, g, i);
    Check(false, "requested extension is present in catalog");
    return {PreviewFormatSource::BuiltIn, false};
}
void CheckState(const WindowViewModel& vm, const wchar_t* extension,
                PreviewFormatSource source, bool available, const char* label) {
    const auto state = State(vm, extension);
    Check(state.source == source && state.available == available, label);
}
void TestSupport() {
    using Source = PreviewFormatSource;
    WindowViewModel vm;
    CheckState(vm, L"png", Source::BuiltIn, true, "builtin image needs no enhancement pack");
    CheckState(vm, L"pdf", Source::BuiltIn, true, "builtin document needs no enhancement pack");
    CheckState(vm, L"ts", Source::BuiltIn, true, "TypeScript in code group needs no media pack");
    CheckState(vm, L"jxl", Source::Pack, false, "JXL reports missing image pack");
    vm.settings_pack_images_installed = true;
    CheckState(vm, L"jxl", Source::Pack, true, "JXL reports installed enabled image pack");
    vm.settings_pack_images_enabled = false;
    CheckState(vm, L"jxl", Source::Pack, false, "JXL respects image pack disable switch");
    for (const auto* ext : {L"heic", L"heif", L"hif"}) {
        vm.settings_preview_codecs = kPreviewCodecsDetected | 1u;
        CheckState(vm, ext, Source::PackOrSystem, false, "HEIF container alone does not imply HEVC decoding");
        vm.settings_preview_codecs = kPreviewCodecsDetected | 2u;
        CheckState(vm, ext, Source::PackOrSystem, false, "HEVC decoder alone does not imply HEIF container support");
        vm.settings_preview_codecs = kPreviewCodecsDetected | 3u;
        CheckState(vm, ext, Source::PackOrSystem, true, "HEIF plus HEVC supplies system alternative while image pack disabled");
        vm.settings_preview_codecs = 3u;
        CheckState(vm, ext, Source::PackOrSystem, false, "unconfirmed detection bits do not imply codec availability");
    }
    vm.settings_preview_codecs = kPreviewCodecsDetected | 4u;
    CheckState(vm, L"avif", Source::PackOrSystem, true, "AV1 system extension supplies AVIF alternative");
    vm.settings_preview_codecs = kPreviewCodecsDetected;
    CheckState(vm, L"avif", Source::PackOrSystem, false, "AVIF without image pack or system codec remains conditional");
    vm.settings_pack_images_enabled = true;
    CheckState(vm, L"avif", Source::PackOrSystem, true, "image pack supplies AVIF without system codec");
    CheckState(vm, L"heic", Source::PackOrSystem, true, "image pack supplies HEIC without system codec");

    CheckState(vm, L"flv", Source::Pack, false, "extended media reports missing decoder pack");
    vm.settings_pack_media_installed = true;
    CheckState(vm, L"flv", Source::Pack, true, "installed media pack supplies extended video");
    CheckState(vm, L"ape", Source::Pack, true, "installed media pack supplies extended audio");
    vm.settings_pack_use_custom = true;
    vm.settings_pack_ffmpeg = 0;
    CheckState(vm, L"flv", Source::Pack, false, "invalid selected custom FFmpeg does not fall back to installed pack status");
    vm.settings_pack_media_installed = false;
    vm.settings_pack_ffmpeg = 2;
    CheckState(vm, L"flv", Source::Pack, true, "validated existing FFmpeg supplies extended video without installed pack");
    vm.settings_pack_ffmpeg_enabled = false;
    CheckState(vm, L"flv", Source::Pack, false, "disabled media switch also disables custom FFmpeg availability");
    vm.settings_pack_use_custom = false;
    vm.settings_pack_media_installed = true;
    CheckState(vm, L"ape", Source::Pack, false, "disabled media switch disables installed pack availability");
    CheckState(vm, L"mp4", Source::BuiltIn, true, "native media remains a playback candidate when enhancement disabled");

    // PackOrSystem with available=false means no confirmed pack in this VM.
    // It does not assert that this machine lacks a system RAW codec.
    CheckState(vm, L"cr3", Source::PackOrSystem, false, "RAW missing pack keeps conditional system source");
    vm.settings_pack_raw_installed = true;
    CheckState(vm, L"cr3", Source::PackOrSystem, true, "RAW installed enabled pack is available");
    vm.settings_pack_raw_enabled = false;
    CheckState(vm, L"cr3", Source::PackOrSystem, false, "RAW disabled pack preserves conditional system source");
    CheckState(vm, L"7z", Source::Plugin, false, "7z requires an available native provider");
    CheckState(vm, L"rar", Source::Plugin, false, "RAR requires an available native provider");
    vm.settings_native_archive_extensions = {L".7z"};
    CheckState(vm, L"7z", Source::Plugin, true, "registered 7z provider is available");
    CheckState(vm, L"rar", Source::Plugin, false, "7z provider does not imply RAR support");
    vm.settings_native_archive_extensions.push_back(L".rar");
    CheckState(vm, L"rar", Source::Plugin, true, "registered RAR provider is available");
    vm.settings_native_archive_extensions.clear();
    CheckState(vm, L"7z", Source::Plugin, false, "unloaded provider is unavailable");
    for (const auto* ext : {L"zip", L"tar", L"iso", L"udf"})
        CheckState(vm, ext, Source::BuiltIn, true, "builtin archive remains available without a native plugin");
}
void TestCatalog() {
    namespace f = preview::formats;
    std::set<std::wstring> expected;
    const auto append = [&](const auto& table) {
        for (const auto extension : table) expected.emplace(extension.substr(1));
    };
    append(f::kImage); append(f::kVector); append(f::kPsd); append(f::kMetaFile);
    append(imgpack::kExtensions); append(f::kRaw); append(f::kPdfRaster);
    append(f::kText); append(f::kVideo); append(f::kAudio);
    append(ffmpeg::kVideoExtensions); append(ffmpeg::kAudioExtensions);
    append(f::kArchive); append(f::kFont);
    for (const auto* extension : {L"md", L"docx", L"epub", L"ipynb", L"csv", L"tsv", L"xlsx", L"xlsm",
                                 L"json", L"xml", L"yaml", L"yml", L"toml", L"ini", L"cfg", L"conf", L"properties"})
        expected.emplace(extension);
    std::set<std::wstring> actual;
    size_t count = 0;
    for (const auto& group : PreviewFormatGroups()) {
        count += group.extensions.size();
        actual.insert(group.extensions.begin(), group.extensions.end());
    }
    Check(actual == expected, "catalog is exact union of decoder tables and rich viewer formats");
    Check(count == actual.size() && PreviewFormatCount() == expected.size(),
          "catalog count deduplicates overlaps across shared image media and text tables");
}
void TestLayout() {
    const int previous = typography::UiFontScalePercent();
    for (const int font : {100, 125}) {
        typography::SetUiFontScale(font);
        for (const float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
            for (const float width : {240.0f, 420.0f, 900.0f}) {
                const D2D1_RECT_F area{17, 23, 17 + width * dpi, 100000};
                std::vector<PreviewFormatChip> chips;
                std::vector<PreviewFormatRow> rows;
                const float height = LayoutPreviewFormats(area, dpi, &chips, &rows);
                bool inside = chips.size() == PreviewFormatCount() && rows.size() == PreviewFormatGroups().size();
                for (const auto& chip : chips) {
                    inside &= chip.group < rows.size() && chip.index < PreviewFormatGroups()[chip.group].extensions.size();
                    inside &= chip.rect.left >= area.left && chip.rect.right <= area.right + 0.01f &&
                        chip.rect.bottom > chip.rect.top && chip.rect.right > chip.rect.left &&
                        chip.rect.top >= rows[chip.group].bounds.top && chip.rect.bottom <= rows[chip.group].bounds.bottom;
                }
                for (size_t i = 1; i < chips.size(); ++i) {
                    const auto& a = chips[i - 1].rect; const auto& b = chips[i].rect;
                    inside &= a.bottom <= b.top + 0.01f || a.right <= b.left + 0.01f;
                }
                const WindowViewModel sources{};
                bool separated = true;
                for (size_t g = 0; g < rows.size(); ++g) {
                    float builtin_bottom = 0, enhanced_top = 100000;
                    for (const auto& chip : chips) if (chip.group == g) {
                        if (PreviewFormatSupport(sources, g, chip.index).source == PreviewFormatSource::BuiltIn)
                            builtin_bottom = (std::max)(builtin_bottom, chip.rect.bottom);
                        else enhanced_top = (std::min)(enhanced_top, chip.rect.top);
                        if (width == 900 && rows[g].note.bottom > rows[g].note.top)
                            separated &= rows[g].note.right < chip.rect.left;
                    }
                    separated &= builtin_bottom < enhanced_top;
                }
                Check(separated, "descriptions stay in left column and enhanced formats start after builtin rows");
                for (size_t i = 1; i < rows.size(); ++i) inside &= rows[i - 1].bounds.bottom <= rows[i].bounds.top;
                inside &= std::abs(height - LayoutPreviewFormats(area, dpi, nullptr, nullptr)) < 0.01f;
                inside &= !rows.empty() && rows.back().bounds.bottom <= area.top + height;
                Check(inside, "narrow/wide layout and 100/125 percent font across DPI keep chips inside rows without overlaps");
            }
        }
    }
    typography::SetUiFontScale(previous);
}
}
int main() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    TestSupport(); TestCatalog(); TestLayout();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    TestCatalog(); TestLayout();
    return failures ? 1 : 0;
}

