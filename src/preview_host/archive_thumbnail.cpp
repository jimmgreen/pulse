// archive_thumbnail.cpp — see archive_thumbnail.h.
#include "archive_thumbnail.h"
#include "decode_budget.h"
#include "../common/preview_extensions.h"
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulse::preview {
namespace {

struct archive;
struct archive_entry;

constexpr int kArchiveOk = 0;
constexpr int kArchiveEof = 1;
constexpr int kArchiveWarn = -20;

constexpr uint64_t kMaxMemberBytes = 24ull * 1024ull * 1024ull;  // refuse an oversized cover
constexpr size_t kMaxMembers = 4000;                              // stop scanning a huge archive
constexpr size_t kMaxNameBytes = 4096;
constexpr ULONGLONG kBudgetMs = 900;

struct LibArchiveApi {
    HMODULE module = nullptr;
    archive* (*read_new)() = nullptr;
    int (*support_format_zip_seekable)(archive*) = nullptr;
    int (*support_format_7zip)(archive*) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*, size_t) = nullptr;
    int (*next_header)(archive*, archive_entry**) = nullptr;
    int64_t (*read_data)(archive*, void*, size_t) = nullptr;  // la_ssize_t on x64
    int (*read_free)(archive*) = nullptr;
    const char* (*pathname_utf8)(archive_entry*) = nullptr;
    const wchar_t* (*pathname_w)(archive_entry*) = nullptr;
    int64_t (*entry_size)(archive_entry*) = nullptr;
    int (*size_is_set)(archive_entry*) = nullptr;
    unsigned short (*filetype)(archive_entry*) = nullptr;  // __LA_MODE_T on Windows
    bool ready = false;

    LibArchiveApi() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        ready = load(read_new, "archive_read_new") &&
            load(support_format_zip_seekable, "archive_read_support_format_zip_seekable") &&
            load(support_format_7zip, "archive_read_support_format_7zip") &&
            load(open_filename_w, "archive_read_open_filename_w") &&
            load(next_header, "archive_read_next_header") &&
            load(read_data, "archive_read_data") &&
            load(read_free, "archive_read_free") &&
            load(pathname_utf8, "archive_entry_pathname_utf8") &&
            load(pathname_w, "archive_entry_pathname_w") &&
            load(entry_size, "archive_entry_size") &&
            load(size_is_set, "archive_entry_size_is_set") &&
            load(filetype, "archive_entry_filetype");
    }
};

std::wstring LowerExtension(std::wstring_view name) {
    const size_t slash = name.find_last_of(L"\\/");
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) return {};
    std::wstring ext(name.substr(dot));
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsCoverPicture(std::wstring_view name) {
    const std::wstring ext = LowerExtension(name);
    return IsOneOf(ext, {L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".tif",
                         L".tiff", L".heic", L".heif", L".avif", L".ico"});
}

// 0: a picture literally named cover / folder / preview / thumb (as good as
// it gets), 1: any other picture, -1: not a picture.
int CoverRank(std::wstring_view name) {
    if (!IsCoverPicture(name)) return -1;
    const size_t slash = name.find_last_of(L"\\/");
    std::wstring stem(name.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem.erase(dot);
    for (wchar_t& c : stem) c = static_cast<wchar_t>(std::towlower(c));
    if (stem == L"cover" || stem == L"folder" || stem == L"preview" || stem == L"thumb") return 0;
    return 1;
}

bool WideName(LibArchiveApi& api, archive_entry* entry, std::wstring& name) {
    if (const char* utf8 = api.pathname_utf8(entry)) {
        const std::string_view bytes(utf8);
        if (bytes.empty() || bytes.size() > kMaxNameBytes) return false;
        wchar_t buffer[512]{};
        const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                              buffer, static_cast<int>(std::size(buffer) - 1));
        if (chars <= 0) return false;
        name.assign(buffer, static_cast<size_t>(chars));
        return true;
    }
    if (const wchar_t* wide = api.pathname_w(entry)) {
        name.assign(wide);
        return !name.empty() && name.size() <= kMaxNameBytes;
    }
    return false;
}

// Read the current member, sized and bounds-checked by the caller. A member
// that yields more bytes than its declared size is refused rather than decoded
// from a truncated prefix. libarchive skips the remaining member bytes on the
// next next_header(), so this does not need to drain them.
bool ReadCurrentMember(LibArchiveApi& api, archive* a, uint64_t size, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(size), 0);
    size_t filled = 0;
    unsigned char buffer[64 * 1024];
    while (filled < out.size()) {
        const int64_t got = api.read_data(a, buffer, sizeof(buffer));
        if (got == 0) break;
        if (got < 0) { out.clear(); return false; }
        const size_t take = (std::min)(static_cast<size_t>(got), out.size() - filled);
        std::copy(buffer, buffer + take, out.begin() + filled);
        filled += take;
        if (static_cast<uint64_t>(got) > take) { out.clear(); return false; }
    }
    if (filled != out.size()) { out.clear(); return false; }
    return true;
}

// Decode bytes straight into premultiplied BGRA, scaled so the longest edge is
// at most `pixels`. The stream decoder reads from memory; no temp file.
bool DecodeBytesToBgra(const std::vector<uint8_t>& bytes, UINT pixels,
                       std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride) {
    if (bytes.empty() || bytes.size() > MAXDWORD) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight)
        return false;
    // The member is size-capped, but WIC still decodes a large PNG/GIF at full
    // size; charge the shared budget for the real source pixels before any
    // scaler/converter allocates.
    if (decode_budget_session)
        RequireDecodeMemory(DecodeEstimate(uint64_t(sourceWidth) * sourceHeight, 20, pixels));
    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
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
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height) return false;
    stride = width * 4;
    out.resize(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data()))) {
        out.clear();
        width = height = stride = 0;
        return false;
    }
    return true;
}

} // namespace

bool MakeArchiveThumbnail(const std::wstring& path, UINT requested_pixels,
                          std::vector<uint8_t>& bgra, UINT& width, UINT& height, UINT& stride,
                          std::wstring* error) {
    bgra.clear();
    width = height = stride = 0;
    if (error) error->clear();
    const UINT pixels = std::clamp(requested_pixels, 32u, 512u);
    static LibArchiveApi api;
    if (!api.ready) {
        if (error) *error = L"libarchive-unavailable";
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = L"archive-unavailable";
        return false;
    }
    archive* a = api.read_new();
    if (!a) return false;
    struct Owner { LibArchiveApi& api; archive* a; ~Owner() { api.read_free(a); } } owner{api, a};
    api.support_format_zip_seekable(a);
    api.support_format_7zip(a);
    if (api.open_filename_w(a, path.c_str(), 64 * 1024) != kArchiveOk) {
        if (error) *error = L"archive-open-failed";
        return false;
    }
    const ULONGLONG started = GetTickCount64();
    // At most two members are inflated: the first picture seen, and a named
    // cover if one turns up later. Everything else is skipped by libarchive
    // itself, so a pack holding hundreds of photos never stalls the host.
    std::vector<uint8_t> best;
    std::vector<uint8_t> fallback;  // previous plain picture, if a named cover replaced it
    size_t members = 0;
    for (;;) {
        if (GetTickCount64() - started > kBudgetMs) break;
        if (++members > kMaxMembers) break;
        archive_entry* entry = nullptr;
        const int r = api.next_header(a, &entry);
        if (r == kArchiveEof) break;
        if (r < kArchiveWarn || !entry) break;
        // Directories and links have no cover; only regular files are read.
        if ((api.filetype(entry) & 0170000) != 0100000) continue;
        std::wstring name;
        if (!WideName(api, entry, name)) continue;
        if (name.front() == L'/' || name.front() == L'\\') continue;  // absolute name
        const int rank = CoverRank(name);
        if (rank < 0) continue;  // next_header() skips its data
        if (rank == 1 && !best.empty()) continue;  // already have a plain picture
        if (!api.size_is_set(entry) || api.entry_size(entry) <= 0 ||
            static_cast<uint64_t>(api.entry_size(entry)) > kMaxMemberBytes) continue;
        std::vector<uint8_t> candidate;
        if (!ReadCurrentMember(api, a, static_cast<uint64_t>(api.entry_size(entry)), candidate))
            continue;
        if (rank == 0) {
            if (!best.empty()) fallback = best;  // keep the plain picture in case this cover is broken
            best = std::move(candidate);
        } else if (best.empty()) {
            best = std::move(candidate);
        }
    }
    if (best.empty()) {
        if (error) *error = L"archive-no-cover";
        return false;
    }
    if (!DecodeBytesToBgra(best, pixels, bgra, width, height, stride)) {
        bgra.clear();
        width = height = stride = 0;
        if (!fallback.empty() && DecodeBytesToBgra(fallback, pixels, bgra, width, height, stride))
            return true;
        if (error) *error = L"archive-cover-decode-failed";
        return false;
    }
    return true;
}

} // namespace pulse::preview
