// thumbnail_disk_cache.cpp - see thumbnail_disk_cache.h.
#include "thumbnail_disk_cache.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>

namespace pulse::ui {
namespace {

constexpr uint32_t kMagic = 0x31435450;  // "PTC1"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kCodecJpeg = 1;
constexpr uint16_t kCodecPng = 2;
constexpr uint32_t kMaxEdge = 4096;
constexpr uint64_t kMaxEntryBytes = 8ull << 20;
constexpr uint64_t kTouchAfter = 24ull * 3600ull * 10000000ull;  // FILETIME units

#pragma pack(push, 1)
struct EntryHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t codec;
    uint32_t width, height;
    uint32_t source_width, source_height;
    uint32_t duration_ms;
    uint32_t pixels;
    uint64_t modified, size;
    uint32_t path_chars;
    uint32_t payload_bytes;
};
#pragma pack(pop)

// The worker threads of the grid cache have no apartment of their own.
struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

IWICImagingFactory* Wic() {
    thread_local ComApartment apartment;
    thread_local ComPtr<IWICImagingFactory> factory;
    if (!factory.get())
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory.get();
}

uint64_t FileTimeValue(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// remote: the file lives on another machine; reading it again costs more
// than any decode, so it is kept however quick the host was.
bool Eligible(const std::wstring& path, DWORD attrs, bool& remote) {
    remote = false;
    if (attrs & FILE_ATTRIBUTE_ENCRYPTED) return false;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        remote = path.compare(0, 4, L"\\\\?\\") != 0 && path.compare(0, 4, L"\\\\.\\") != 0;
        return remote;
    }
    if (path.size() < 3 || path[1] != L':') return false;
    const wchar_t root[] = {path[0], L':', L'\\', 0};
    switch (GetDriveTypeW(root)) {
    case DRIVE_FIXED: return true;
    case DRIVE_REMOTE: remote = true; return true;
    default: return false;  // removable, optical, RAM disks, unknown
    }
}

// Always slow somewhere: shell or FFmpeg posters, HEIF-family and RAW
// decoders, layered and paged documents.
bool AlwaysKept(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.size() - dot > 6) return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    static const wchar_t* const kSlow[] = {
        L"mp4", L"m4v", L"mkv", L"mov", L"avi", L"wmv", L"webm", L"flv", L"mpg", L"mpeg", L"vob",
        L"ts", L"mts", L"m2ts", L"3gp", L"3g2", L"ogv", L"asf", L"rm", L"rmvb",
        L"heic", L"heif", L"hif", L"avif", L"jxl", L"psd", L"psb", L"pdf", L"tif", L"tiff",
        L"cr2", L"cr3", L"nef", L"arw", L"dng", L"orf", L"rw2", L"raf", L"pef", L"srw", L"rwl", L"srf", L"sr2"};
    for (const wchar_t* slow : kSlow)
        if (ext == slow) return true;
    return false;
}

uint64_t KeyHash(const ThumbnailStoreKey& key) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    for (wchar_t c : key.path) {
        const wchar_t lower = static_cast<wchar_t>(std::towlower(c));
        mix(&lower, sizeof(lower));
    }
    mix(&key.size, sizeof(key.size));
    mix(&key.modified, sizeof(key.modified));
    mix(&key.pixels, sizeof(key.pixels));
    // Version only formats whose rendering changed, leaving photo/video caches warm.
    const auto dot = key.path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring extension = key.path.substr(dot);
        for (auto& c : extension) c = static_cast<wchar_t>(std::towlower(c));
        // WIC frame ordinals differ from ICO/CUR directory ordinals. Entries
        // made before container-frame decoding can contain enlarged small icons.
        if (extension == L".ico" || extension == L".cur") {
            constexpr uint32_t icon_decode_policy = 1;
            mix(&icon_decode_policy, sizeof(icon_decode_policy));
        }
        for (const auto* office : {L".docx", L".docm", L".doc", L".rtf", L".wps",
             L".xlsx", L".xlsm", L".xls", L".et", L".pptx", L".pptm", L".ppsx",
             L".ppt", L".pps", L".dps", L".odt", L".ods", L".odp", L".odg"}) {
            if (extension != office) continue;
            // Older Office sketches baked a second application badge into the page.
            constexpr uint32_t office_style = 2;
            mix(&office_style, sizeof(office_style));
            break;
        }
    }
    return hash;
}

bool HasAlpha(const StoredThumbnail& thumb) {
    for (size_t i = 3; i < thumb.pixels.size(); i += 4)
        if (thumb.pixels[i] != 0xFF) return true;
    return false;
}

bool Encode(const StoredThumbnail& thumb, bool png, std::vector<uint8_t>& out) {
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IStream> memory;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(wic->CreateBitmapFromMemory(thumb.width, thumb.height, GUID_WICPixelFormat32bppPBGRA,
            thumb.width * 4, static_cast<UINT>(thumb.pixels.size()),
            const_cast<BYTE*>(thumb.pixels.data()), &bitmap)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory)) ||
        FAILED(wic->CreateEncoder(png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(memory.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
    if (!png && options.get()) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality{};
        quality.vt = VT_R4;
        quality.fltVal = 0.9f;
        options->Write(1, &option, &quality);
    }
    WICPixelFormatGUID format = png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(frame->Initialize(options.get())) ||
        FAILED(frame->SetSize(thumb.width, thumb.height)) ||
        FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bitmap.get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(memory->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > kMaxEntryBytes ||
        FAILED(GetHGlobalFromStream(memory.get(), &global))) return false;
    const auto* bytes = static_cast<const uint8_t*>(GlobalLock(global));
    if (!bytes) return false;
    out.assign(bytes, bytes + static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(global);
    return !out.empty();
}

bool Decode(const std::vector<uint8_t>& entry, const ThumbnailStoreKey& key, StoredThumbnail& out) {
    EntryHeader header{};
    std::memcpy(&header, entry.data(), sizeof(header));
    const size_t path_bytes = static_cast<size_t>(header.path_chars) * sizeof(wchar_t);
    if (header.magic != kMagic || header.version != kVersion ||
        (header.codec != kCodecJpeg && header.codec != kCodecPng) ||
        header.pixels != key.pixels || header.modified != key.modified || header.size != key.size ||
        header.path_chars != key.path.size() || !header.width || !header.height ||
        header.width > kMaxEdge || header.height > kMaxEdge ||
        sizeof(header) + path_bytes + header.payload_bytes != entry.size()) return false;
    std::wstring stored_path(header.path_chars, L'\0');
    std::memcpy(stored_path.data(), entry.data() + sizeof(header), path_bytes);
    if (_wcsicmp(stored_path.c_str(), key.path.c_str()) != 0) return false;
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(entry.data() + sizeof(header) + path_bytes),
                                            header.payload_bytes)) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) ||
        width != header.width || height != header.height) return false;
    out.pixels.resize(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.pixels.size()),
                                     out.pixels.data()))) return false;
    out.width = width;
    out.height = height;
    out.source_width = header.source_width;
    out.source_height = header.source_height;
    out.duration_ms = header.duration_ms;
    return true;
}

// Calls visit(path, data) for every file in the two-level layout.
template <typename Visit>
void ForEachEntry(const std::wstring& root, Visit&& visit) {
    WIN32_FIND_DATAW dir{};
    HANDLE dirs = FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &dir, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (dirs == INVALID_HANDLE_VALUE) return;
    do {
        if (!(dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(dir.cFileName) != 2) continue;
        const std::wstring folder = root + L"\\" + dir.cFileName;
        WIN32_FIND_DATAW file{};
        HANDLE files = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &file, FindExSearchNameMatch,
                                        nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (files == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                visit(folder + L"\\" + file.cFileName, file);
        } while (FindNextFileW(files, &file));
        FindClose(files);
    } while (FindNextFileW(dirs, &dir));
    FindClose(dirs);
}

uint64_t FileBytes(const WIN32_FIND_DATAW& data) {
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool IsTemporary(const std::wstring& path) {
    return path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".tmp") == 0;
}

// The grid cache of every window reads this store.
const bool g_registered = (ThumbnailCache::SetGridStore(&ThumbnailDiskCache::Shared()), true);

} // namespace

ThumbnailDiskCache& ThumbnailDiskCache::Shared() {
    // Persistence is best effort. Keep this process-wide service alive until
    // process termination so closing the UI never joins unbounded disk I/O.
    // The writer owns its requests and uses only this service and thread-local WIC.
    static auto* cache = new ThumbnailDiskCache;
    return *cache;
}

const std::wstring& ThumbnailDiskCache::Root() {
    std::call_once(root_once_, [this] {
        wchar_t value[MAX_PATH]{};
        const DWORD forced = GetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", value, MAX_PATH);
        if (forced > 0 && forced < MAX_PATH) {
            if (_wcsicmp(value, L"off") != 0) root_ = value;
            return;
        }
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, MAX_PATH);
        if (length > 0 && length < MAX_PATH) root_ = std::wstring(value) + L"\\Pulse\\thumbs";
    });
    return root_;
}

std::wstring ThumbnailDiskCache::EntryPath(const ThumbnailStoreKey& key) const {
    wchar_t name[40]{};
    const uint64_t hash = KeyHash(key);
    swprintf_s(name, L"\\%02x\\%016llx.pt", static_cast<unsigned>(hash >> 56),
               static_cast<unsigned long long>(hash));
    return root_ + name;
}

bool ThumbnailDiskCache::Load(const ThumbnailStoreKey& key, StoredThumbnail& out) {
    bool remote = false;
    if (Root().empty() || !Eligible(key.path, key.attrs, remote)) return false;
    const std::wstring file = EntryPath(key);
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> entry;
    LARGE_INTEGER length{};
    bool read = GetFileSizeEx(handle, &length) && length.QuadPart > static_cast<LONGLONG>(sizeof(EntryHeader)) &&
                static_cast<uint64_t>(length.QuadPart) <= kMaxEntryBytes;
    if (read) {
        entry.resize(static_cast<size_t>(length.QuadPart));
        DWORD got = 0;
        read = ReadFile(handle, entry.data(), static_cast<DWORD>(entry.size()), &got, nullptr) &&
               got == entry.size();
    }
    if (read) {
        // Last write time is the LRU clock (NTFS last-access updates are
        // usually off); refreshed at most daily so hits stay read-only.
        FILETIME written{}, now{};
        GetSystemTimeAsFileTime(&now);
        if (GetFileTime(handle, nullptr, nullptr, &written) &&
            FileTimeValue(now) > FileTimeValue(written) + kTouchAfter)
            SetFileTime(handle, nullptr, nullptr, &now);
    }
    CloseHandle(handle);
    if (!read) return false;
    if (Decode(entry, key, out)) return true;
    // Damaged, or another file's entry under the same hash.
    DeleteFileW(file.c_str());
    return false;
}

ThumbnailDiskCache::~ThumbnailDiskCache() {
    { std::lock_guard lock(writer_mutex_); stopping_ = true; writes_.clear(); }
    writer_cv_.notify_one();
    if (writer_.joinable()) writer_.join();
}

void ThumbnailDiskCache::Save(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb) {
    constexpr size_t kWriterBudget = 16u << 20;
    const size_t bytes = thumb.pixels.size();
    if (!bytes || bytes > kWriterBudget || key.path.size() > 32767 ||
        !thumb.width || !thumb.height || thumb.width > kMaxEdge || thumb.height > kMaxEdge ||
        bytes != static_cast<size_t>(thumb.width) * thumb.height * 4) return;
    std::lock_guard lock(writer_mutex_);
    if (stopping_ || writes_.size() >= 64 || bytes > kWriterBudget - writer_bytes_) return;
    try {
        if (!writer_.joinable()) writer_ = std::thread([this] { WriterLoop(); });
        writes_.push_back({key.path, key.attrs, key.pixels, key.modified, key.size, key.keep_always, decode_ms, thumb});
        writer_bytes_ += bytes;
    } catch (...) { return; }
    writer_cv_.notify_one();
}

bool ThumbnailDiskCache::FlushForTesting(uint32_t timeout_ms) {
    std::unique_lock lock(writer_mutex_);
    return writer_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this] { return writer_bytes_ == 0; });
}

void ThumbnailDiskCache::WriterLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        WriteRequest request;
        {
            std::unique_lock lock(writer_mutex_);
            writer_cv_.wait(lock, [this] { return stopping_ || !writes_.empty(); });
            if (stopping_) break;
            request = std::move(writes_.front());
            writes_.pop_front();
        }
        try { SaveNow({request.path, request.attrs, request.pixels, request.modified, request.size, request.keep_always}, request.decode_ms, request.thumb); } catch (...) {}
        { std::lock_guard lock(writer_mutex_); writer_bytes_ -= request.thumb.pixels.size(); }
        writer_cv_.notify_all();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void ThumbnailDiskCache::SaveNow(const ThumbnailStoreKey& key, uint32_t decode_ms, const StoredThumbnail& thumb) {
    bool remote = false;
    if (Root().empty() || !Eligible(key.path, key.attrs, remote)) return;
    if (decode_ms < kMinDecodeMs && !remote && !key.keep_always && !AlwaysKept(key.path)) return;
    if (!thumb.width || !thumb.height || thumb.width > kMaxEdge || thumb.height > kMaxEdge ||
        thumb.pixels.size() != static_cast<size_t>(thumb.width) * thumb.height * 4 ||
        key.path.size() > 32767) return;
    const bool png = HasAlpha(thumb);
    std::vector<uint8_t> payload;
    if (!Encode(thumb, png, payload)) return;
    EntryHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.codec = png ? kCodecPng : kCodecJpeg;
    header.width = thumb.width;
    header.height = thumb.height;
    header.source_width = thumb.source_width;
    header.source_height = thumb.source_height;
    header.duration_ms = thumb.duration_ms;
    header.pixels = key.pixels;
    header.modified = key.modified;
    header.size = key.size;
    header.path_chars = static_cast<uint32_t>(key.path.size());
    header.payload_bytes = static_cast<uint32_t>(payload.size());
    const size_t path_bytes = key.path.size() * sizeof(wchar_t);
    std::vector<uint8_t> entry(sizeof(header) + path_bytes + payload.size());
    std::memcpy(entry.data(), &header, sizeof(header));
    std::memcpy(entry.data() + sizeof(header), key.path.data(), path_bytes);
    std::memcpy(entry.data() + sizeof(header) + path_bytes, payload.data(), payload.size());

    const std::wstring file = EntryPath(key);
    const std::wstring folder = file.substr(0, file.find_last_of(L'\\'));
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES)
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const std::wstring temporary = file + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    const bool written = WriteFile(handle, entry.data(), static_cast<DWORD>(entry.size()), &wrote, nullptr) &&
                         wrote == entry.size();
    CloseHandle(handle);
    WIN32_FILE_ATTRIBUTE_DATA old{};
    const uint64_t replaced = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &old)
        ? (static_cast<uint64_t>(old.nFileSizeHigh) << 32) | old.nFileSizeLow : 0;
    if (!written || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return;
    }
    std::lock_guard lock(mutex_);
    if (!scanned_) ScanLocked();  // counts the entry just written
    else bytes_ = bytes_ + entry.size() - (std::min)(replaced, bytes_ + entry.size());
    if (bytes_ > kBudgetBytes) TrimLocked();
}

void ThumbnailDiskCache::ScanLocked() {
    scanned_ = true;
    bytes_ = 0;
    ForEachEntry(root_, [&](const std::wstring&, const WIN32_FIND_DATAW& data) { bytes_ += FileBytes(data); });
}

void ThumbnailDiskCache::TrimLocked() {
    struct Entry {
        uint64_t written;
        uint64_t bytes;
        std::wstring path;
    };
    std::vector<Entry> entries;
    FILETIME now_time{};
    GetSystemTimeAsFileTime(&now_time);
    const uint64_t now = FileTimeValue(now_time);
    uint64_t total = 0;
    ForEachEntry(root_, [&](const std::wstring& path, const WIN32_FIND_DATAW& data) {
        const uint64_t written = FileTimeValue(data.ftLastWriteTime);
        // A write that never finished (crash, power loss); recent ones may
        // still be on their way in from another Pulse process.
        if (IsTemporary(path)) {
            if (now > written + 600ull * 10000000ull) DeleteFileW(path.c_str());
            return;
        }
        total += FileBytes(data);
        entries.push_back({written, FileBytes(data), path});
    });
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.written < b.written; });
    // Down to 80 % so the next trim is a while away.
    const uint64_t target = kBudgetBytes / 5 * 4;
    for (const Entry& entry : entries) {
        if (total <= target) break;
        if (DeleteFileW(entry.path.c_str())) total -= entry.bytes;
    }
    bytes_ = total;
}

void ThumbnailDiskCache::Clear() {
    if (Root().empty()) return;
    std::lock_guard lock(mutex_);
    uint64_t left = 0;
    ForEachEntry(root_, [&](const std::wstring& path, const WIN32_FIND_DATAW& data) {
        if (!DeleteFileW(path.c_str()) && !IsTemporary(path)) left += FileBytes(data);
    });
    scanned_ = true;
    bytes_ = left;
}

uint64_t ThumbnailDiskCache::BytesOnDisk() {
    if (Root().empty()) return 0;
    std::lock_guard lock(mutex_);
    if (!scanned_) ScanLocked();
    return bytes_;
}

} // namespace pulse::ui
