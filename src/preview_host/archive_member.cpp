// archive_member.cpp - see archive_member.h.
#include "archive_member.h"
#include "decode_budget.h"
#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

struct archive;
struct archive_entry;

constexpr int kArchiveOk = 0;
constexpr int kArchiveEof = 1;
constexpr int kArchiveWarn = -20;
// Pictures only; anything bigger is not worth a tile.
constexpr size_t kMaxMemberBytes = 48u << 20;
constexpr UINT kMaxPictureEdge = 16384;
// Grid covers: a smaller member cap, and a bounded scan so a pack of thousands
// of photos (or a solid 7z) never stalls a tile.
constexpr size_t kMaxCoverBytes = 24u << 20;
constexpr size_t kMaxCoverMembers = 4000;
constexpr ULONGLONG kCoverScanMs = 900;

struct Api {
    HMODULE module = nullptr;
    archive* (*read_new)() = nullptr;
    int (*support_format_zip_seekable)(archive*) = nullptr;
    int (*support_format_all)(archive*) = nullptr;
    int (*support_filter_all)(archive*) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*, size_t) = nullptr;
    int (*next_header)(archive*, archive_entry**) = nullptr;
    int64_t (*read_data)(archive*, void*, size_t) = nullptr;  // la_ssize_t on x64
    int (*read_free)(archive*) = nullptr;
    const wchar_t* (*pathname_w)(archive_entry*) = nullptr;
    const char* (*pathname_utf8)(archive_entry*) = nullptr;
    int64_t (*entry_size)(archive_entry*) = nullptr;
    int (*size_is_set)(archive_entry*) = nullptr;
    bool ready = false;

    Api() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        ready = load(read_new, "archive_read_new") &&
            load(support_format_zip_seekable, "archive_read_support_format_zip_seekable") &&
            load(support_format_all, "archive_read_support_format_all") &&
            load(support_filter_all, "archive_read_support_filter_all") &&
            load(open_filename_w, "archive_read_open_filename_w") &&
            load(next_header, "archive_read_next_header") &&
            load(read_data, "archive_read_data") &&
            load(read_free, "archive_read_free") &&
            load(pathname_w, "archive_entry_pathname_w") &&
            load(pathname_utf8, "archive_entry_pathname_utf8") &&
            load(entry_size, "archive_entry_size") &&
            load(size_is_set, "archive_entry_size_is_set");
    }
};

Api& GetApi() {
    static Api api;
    return api;
}

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

// '/' separators, no leading "/" or "./" parts, ASCII lower case.
std::wstring Key(std::wstring_view name) {
    std::wstring out(name);
    std::replace(out.begin(), out.end(), L'\\', L'/');
    for (;;) {
        if (out.starts_with(L"./")) out.erase(0, 2);
        else if (out.starts_with(L"/")) out.erase(0, 1);
        else break;
    }
    for (wchar_t& c : out) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return out;
}

std::wstring EntryName(const Api& api, archive_entry* entry) {
    if (const wchar_t* wide = api.pathname_w(entry)) return wide;
    const char* utf8 = api.pathname_utf8(entry);
    if (!utf8) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    return out;
}

bool LooksLikeZip(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    unsigned char magic[4]{};
    DWORD got = 0;
    const bool read = ReadFile(file, magic, sizeof(magic), &got, nullptr) && got == sizeof(magic);
    CloseHandle(file);
    return read && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
}

// Opens `path` for a forward scan: ZIP seekably (its central directory),
// anything else through every libarchive format and filter.
bool OpenForScan(Api& api, archive* a, const std::wstring& path) {
    if (LooksLikeZip(path)) {
        api.support_format_zip_seekable(a);
    } else {
        api.support_filter_all(a);
        api.support_format_all(a);
    }
    return api.open_filename_w(a, path.c_str(), 64 * 1024) == kArchiveOk;
}

// Reads the current member; false (and nothing kept) on a read error or when
// it grows past max_bytes.
bool ReadCurrent(Api& api, archive* a, size_t max_bytes, std::vector<unsigned char>& data) {
    data.clear();
    unsigned char buffer[64 * 1024];
    for (;;) {
        const int64_t got = api.read_data(a, buffer, sizeof(buffer));
        if (got == 0) return !data.empty();
        if (got < 0 || data.size() + static_cast<size_t>(got) > max_bytes) {
            data.clear();
            return false;
        }
        data.insert(data.end(), buffer, buffer + got);
    }
}

// For a Key()ed member name: 0 = a picture named cover / folder / preview /
// thumb, 1 = any other picture, -1 = not a cover candidate (no picture
// extension, or macOS resource-fork debris such as __MACOSX/._cover.jpg).
int CoverRank(std::wstring_view key) {
    if (key.starts_with(L"__macosx/")) return -1;
    const size_t slash = key.find_last_of(L'/');
    const std::wstring_view base = key.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
    const size_t dot = base.find_last_of(L'.');
    if (base.starts_with(L"._") || dot == std::wstring_view::npos || dot == 0) return -1;
    const std::wstring_view ext = base.substr(dot);
    static constexpr std::wstring_view kPictures[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".tif", L".tiff",
        L".heic", L".heif", L".avif"};
    if (std::find(std::begin(kPictures), std::end(kPictures), ext) == std::end(kPictures)) return -1;
    const std::wstring_view stem = base.substr(0, dot);
    return stem == L"cover" || stem == L"folder" || stem == L"preview" || stem == L"thumb" ? 0 : 1;
}

// Encoded picture bytes -> 32bpp premultiplied BGRA, long side at most
// max_edge (0 = 256; clamped to 16..1024). The outputs are only written on
// success. charge_budget charges the preview host's decode budget for the full
// source size before WIC allocates (grid covers run under it).
bool DecodePicture(const std::vector<unsigned char>& encoded, UINT max_edge, bool charge_budget,
                   std::vector<uint8_t>& pixels, UINT& width, UINT& height, UINT& stride,
                   UINT& source_width, UINT& source_height, std::wstring* error) {
    if (encoded.empty() || encoded.size() > MAXDWORD) { SetError(error, L"entry-read-failed"); return false; }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(encoded.data(), static_cast<UINT>(encoded.size())));
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT fw = 0, fh = 0;
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&fw, &fh)) ||
        !fw || !fh || fw > kMaxPictureEdge || fh > kMaxPictureEdge) {
        SetError(error, L"archive-member-not-picture");
        return false;
    }
    if (charge_budget && decode_budget_session)
        RequireDecodeMemory(DecodeEstimate(uint64_t(fw) * fh, 20, max_edge ? max_edge : 256u));
    const UINT edge = (std::clamp)(max_edge ? max_edge : 256u, 16u, 1024u);
    const double k = (std::min)(1.0, static_cast<double>(edge) / (std::max)(fw, fh));
    const UINT tw = (std::max)(1u, static_cast<UINT>(fw * k + 0.5));
    const UINT th = (std::max)(1u, static_cast<UINT>(fh * k + 0.5));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        SetError(error, L"archive-member-decode-failed");
        return false;
    }
    std::vector<uint8_t> out(static_cast<size_t>(tw) * th * 4);
    if (FAILED(converter->CopyPixels(nullptr, tw * 4, static_cast<UINT>(out.size()), out.data()))) {
        SetError(error, L"archive-member-decode-failed");
        return false;
    }
    pixels.swap(out);
    width = tw;
    height = th;
    stride = tw * 4;
    source_width = fw;
    source_height = fh;
    return true;
}

} // namespace

bool SplitArchiveMemberPath(std::wstring_view spec, std::wstring& archive, std::wstring& member) {
    const size_t bar = spec.find(L'|');
    if (bar == std::wstring_view::npos || bar == 0 || bar + 1 >= spec.size()) return false;
    archive.assign(spec.substr(0, bar));
    member.assign(spec.substr(bar + 1));
    return true;
}

bool ReadArchiveMember(const std::wstring& path, std::wstring_view member, size_t max_bytes,
                       std::vector<unsigned char>& data, std::wstring* error) {
    data.clear();
    Api& api = GetApi();
    if (!api.ready) { SetError(error, L"libarchive-unavailable"); return false; }
    archive* a = api.read_new();
    if (!a) { SetError(error, L"libarchive-unavailable"); return false; }
    struct Free { Api& api; archive* a; ~Free() { api.read_free(a); } } release{api, a};
    if (LooksLikeZip(path)) {
        api.support_format_zip_seekable(a);
    } else {
        api.support_filter_all(a);
        api.support_format_all(a);
    }
    if (api.open_filename_w(a, path.c_str(), 64 * 1024) != kArchiveOk) {
        SetError(error, L"archive-open-failed");
        return false;
    }
    const std::wstring wanted = Key(member);
    if (wanted.empty()) { SetError(error, L"entry-not-found"); return false; }
    for (;;) {
        archive_entry* entry = nullptr;
        const int r = api.next_header(a, &entry);
        if (r == kArchiveEof) break;
        if (r < kArchiveWarn || !entry) { SetError(error, L"entry-read-failed"); return false; }
        if (Key(EntryName(api, entry)) != wanted) continue;
        if (api.size_is_set(entry) && api.entry_size(entry) >= 0 &&
            static_cast<uint64_t>(api.entry_size(entry)) > max_bytes) {
            SetError(error, L"entry-too-large");
            return false;
        }
        unsigned char buffer[64 * 1024];
        for (;;) {
            const int64_t got = api.read_data(a, buffer, sizeof(buffer));
            if (got == 0) return true;
            if (got < 0) { data.clear(); SetError(error, L"entry-read-failed"); return false; }
            if (data.size() + static_cast<size_t>(got) > max_bytes) {
                data.clear();
                SetError(error, L"entry-too-large");
                return false;
            }
            data.insert(data.end(), buffer, buffer + got);
        }
    }
    SetError(error, L"entry-not-found");
    return false;
}

bool DecodeArchiveMemberThumbnail(std::wstring_view spec, UINT max_edge, std::vector<uint8_t>& pixels,
                                  UINT& width, UINT& height, UINT& stride,
                                  UINT& source_width, UINT& source_height, std::wstring* error) {
    std::wstring archive_path, member;
    if (!SplitArchiveMemberPath(spec, archive_path, member)) { SetError(error, L"archive-member-path"); return false; }
    std::vector<unsigned char> encoded;
    if (!ReadArchiveMember(archive_path, member, kMaxMemberBytes, encoded, error)) return false;
    return DecodePicture(encoded, max_edge, false, pixels, width, height, stride, source_width, source_height, error);
}

bool DecodeArchiveCoverThumbnail(const std::wstring& path, UINT max_edge, std::vector<uint8_t>& pixels,
                                 UINT& width, UINT& height, UINT& stride, std::wstring* error) {
    Api& api = GetApi();
    if (!api.ready) { SetError(error, L"libarchive-unavailable"); return false; }
    archive* a = api.read_new();
    if (!a) { SetError(error, L"libarchive-unavailable"); return false; }
    struct Free { Api& api; archive* a; ~Free() { api.read_free(a); } } release{api, a};
    if (!OpenForScan(api, a, path)) { SetError(error, L"archive-open-failed"); return false; }
    // One forward pass that inflates at most two members: the first picture,
    // kept as a fallback, and a named cover, which ends the scan.
    std::vector<unsigned char> named, plain;
    const ULONGLONG started = GetTickCount64();
    for (size_t seen = 0; named.empty() && seen < kMaxCoverMembers; ++seen) {
        if (GetTickCount64() - started > kCoverScanMs) break;
        archive_entry* entry = nullptr;
        const int r = api.next_header(a, &entry);
        if (r == kArchiveEof || r < kArchiveWarn || !entry) break;
        const int rank = CoverRank(Key(EntryName(api, entry)));
        if (rank < 0 || (rank == 1 && !plain.empty())) continue;
        if (api.size_is_set(entry) && (api.entry_size(entry) <= 0 ||
                                       static_cast<uint64_t>(api.entry_size(entry)) > kMaxCoverBytes))
            continue;
        std::vector<unsigned char> data;
        if (ReadCurrent(api, a, kMaxCoverBytes, data)) (rank == 0 ? named : plain) = std::move(data);
    }
    UINT source_width = 0, source_height = 0;
    for (const std::vector<unsigned char>* data : {&named, &plain})
        if (!data->empty() && DecodePicture(*data, max_edge, true, pixels, width, height, stride,
                                            source_width, source_height, nullptr))
            return true;
    SetError(error, named.empty() && plain.empty() ? L"archive-no-cover" : L"archive-cover-decode-failed");
    return false;
}

} // namespace pulse::preview
