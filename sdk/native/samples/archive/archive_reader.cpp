#include "archive_reader.h"
#define NOMINMAX
#include <windows.h>
#include <propidl.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <set>
#include <cwctype>

// Minimal public 7-Zip COM ABI, from ip7z/7zip CPP/7zip/{IDecl.h,IStream.h,
// Archive/IArchive.h,PropID.h}. GUID group 3 is streams, group 6 archives.
// Only public CreateObject/GetHandlerProperty2/GetNumberOfFormats exports are used.
extern "C" __declspec(dllimport) HRESULT WINAPI CreateObject(const GUID*, const GUID*, void**);
extern "C" __declspec(dllimport) HRESULT WINAPI GetHandlerProperty2(unsigned, PROPID, PROPVARIANT*);
extern "C" __declspec(dllimport) HRESULT WINAPI GetNumberOfFormats(unsigned*);

namespace archive_reader {
namespace {
constexpr GUID Guid(unsigned char group, unsigned char id) {
    return {0x23170F69, 0x40C1, 0x278A, {0, 0, 0, group, 0, id, 0, 0}};
}
constexpr GUID kSequential = Guid(3, 1), kStream = Guid(3, 3), kArchive = Guid(6, 0x60), kOpen = Guid(6, 0x10);
struct Sequential : IUnknown { virtual HRESULT STDMETHODCALLTYPE Read(void*, unsigned, unsigned*) noexcept = 0; };
struct Stream : Sequential { virtual HRESULT STDMETHODCALLTYPE Seek(int64_t, unsigned, uint64_t*) noexcept = 0; };
struct OpenCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(const uint64_t*, const uint64_t*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t*, const uint64_t*) noexcept = 0;
};
struct Archive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(Stream*, const uint64_t*, OpenCallback*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(unsigned*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(unsigned, PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const unsigned*, unsigned, int, IUnknown*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(unsigned*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(unsigned, BSTR*, PROPID*, VARTYPE*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(unsigned*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(unsigned, BSTR*, PROPID*, VARTYPE*) noexcept = 0;
};
struct Variant {
    PROPVARIANT value{};
    ~Variant() { PropVariantClear(&value); }
};
bool Cancelled(const std::function<bool()>& callback) noexcept {
    try { return callback && callback(); } catch (...) { return true; }
}
class FileStream final : public Stream {
    std::atomic<ULONG> refs_{1};
    HANDLE file_;
    const std::function<bool()>& cancel_;
public:
    FileStream(HANDLE file, const std::function<bool()>& cancel) : file_(file), cancel_(cancel) {}
    ~FileStream() { CloseHandle(file_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kStream && id != kSequential) return E_NOINTERFACE;
        *out = static_cast<Stream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE Read(void* data, unsigned size, unsigned* read) noexcept override {
        if (read) *read = 0;
        if (Cancelled(cancel_)) return E_ABORT;
        DWORD done = 0;
        if (!ReadFile(file_, data, std::min(size, 1024u * 1024u), &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read) *read = done;
        return Cancelled(cancel_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(int64_t offset, unsigned origin, uint64_t* position) noexcept override {
        if (Cancelled(cancel_)) return E_ABORT;
        if (origin > FILE_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER move{}, result{}; move.QuadPart = offset;
        if (!SetFilePointerEx(file_, move, &result, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (position) *position = static_cast<uint64_t>(result.QuadPart);
        return S_OK;
    }
};
class Progress final : public OpenCallback {
    std::atomic<ULONG> refs_{1};
    const std::function<bool()>& cancel_;
public:
    explicit Progress(const std::function<bool()>& cancel) : cancel_(cancel) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kOpen) return E_NOINTERFACE;
        *out = static_cast<OpenCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT STDMETHODCALLTYPE SetTotal(const uint64_t*, const uint64_t*) noexcept override { return Cancelled(cancel_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t*, const uint64_t*) noexcept override { return Cancelled(cancel_) ? E_ABORT : S_OK; }
};
template<class T> struct ComRelease { void operator()(T* value) const { if (value) value->Release(); } };
struct ArchiveRelease { void operator()(Archive* value) const { if (value) { value->Close(); value->Release(); } } };
std::string Utf8(BSTR text) {
    const unsigned length = text ? SysStringLen(text) : 0;
    if (length > 4096) throw std::length_error("Archive item path exceeds 4096 UTF-8 bytes");
    if (!length) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (!bytes || bytes > 4096) throw std::length_error("Archive item path is invalid or exceeds 4096 UTF-8 bytes");
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), bytes, nullptr, nullptr);
    if (result.find('\0') != std::string::npos) throw std::runtime_error("Archive item path contains NUL");
    return result;
}
uint64_t Number(const PROPVARIANT& value) {
    if (value.vt == VT_UI8) return value.uhVal.QuadPart;
    if (value.vt == VT_UI4) return value.ulVal;
    if (value.vt == VT_EMPTY) return 0;
    throw std::runtime_error("Unexpected archive numeric property type");
}
Result Failure(Status status, const char* message) { Result result; result.status = status; result.incomplete = status == Status::LimitReached; result.error = message; return result; }
}

bool ValidExtractionPath(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' || path.front() == '\\') return false;
    size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find_first_of("/\\", start);
        auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for (unsigned char c : part) if (c < 32 || std::string_view(":*?\"<>|").find(c) != std::string_view::npos) return false;
        auto base = part.substr(0, part.find('.'));
        for (char& c : base) if (c >= 'a' && c <= 'z') c -= 32;
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' && base[3] <= '9')) return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}
namespace {
struct OutStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Write(const void*, unsigned, unsigned*) noexcept = 0; };
struct ExtractCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(uint64_t) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(unsigned, OutStream**, int) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(int) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(int) noexcept = 0;
};
class Output final : public OutStream {
    std::atomic<ULONG> refs_{1}; HANDLE file_; uint64_t remaining_;
    const std::function<bool()>& cancel_;
public:
    bool failed = false;
    ExtractionProgress& progress_;
    std::function<HRESULT()> notify_;
    Output(HANDLE f, uint64_t size, const std::function<bool()>& c,
        ExtractionProgress& progress, std::function<HRESULT()> notify)
        : file_(f), remaining_(size), cancel_(c), progress_(progress), notify_(std::move(notify)) {}
    ~Output() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    bool Finish() { const bool ok = !failed && remaining_ == 0 && FlushFileBuffers(file_); CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; return ok; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != Guid(3, 2)) return E_NOINTERFACE;
        *out = static_cast<OutStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Write(const void* data, unsigned size, unsigned* written) noexcept override {
        if (written) *written = 0;
        if (Cancelled(cancel_)) return E_ABORT;
        if (size > remaining_ || progress_.completed_bytes > progress_.total_bytes ||
            size > progress_.total_bytes - progress_.completed_bytes) { failed = true; return E_FAIL; }
        DWORD done = 0;
        if (!WriteFile(file_, data, std::min(size, 1024u * 1024u), &done, nullptr)) { failed = true; return E_FAIL; }
        remaining_ -= done;
        progress_.completed_bytes += done;
        if (written) *written = done;
        try {
            const auto status = notify_();
            if (status != S_OK) return status;
        } catch (...) { failed = true; return E_FAIL; }
        return Cancelled(cancel_) ? E_ABORT : S_OK;
    }
};
class Extraction final : public ExtractCallback {
    std::atomic<ULONG> refs_{1}; std::filesystem::path root_;
    const std::vector<Entry>& entries_; const std::function<bool()>& cancel_;
    const std::function<void(const ExtractionProgress&)>& notify_;
    std::vector<HANDLE> guards_; std::set<std::wstring> guarded_;
    std::unique_ptr<Output, ComRelease<Output>> output_;
    ExtractionProgress progress_;
    bool Guard(const std::filesystem::path& path) {
        if (guarded_.contains(path.wstring())) return true;
        HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(h); return false; }
        try { guards_.push_back(h); }
        catch (...) { CloseHandle(h); throw; }
        // Once tracked, Extraction owns the handle even if indexing allocates.
        guarded_.insert(path.wstring());
        return true;
    }
    HRESULT Notify() noexcept { try { if (notify_) notify_(progress_); } catch (...) { failed = true; return E_ABORT; } return Cancelled(cancel_) ? E_ABORT : S_OK; }
public:
    bool failed = false;
    Extraction(const std::wstring& root, const std::vector<Entry>& entries, const std::function<bool()>& cancel,
        const std::function<void(const ExtractionProgress&)>& notify, uint64_t total)
        : root_(root), entries_(entries), cancel_(cancel), notify_(notify) { progress_.total_bytes = total; progress_.total_items = entries.size(); }
    ~Extraction() { output_.reset(); for (HANDLE h : guards_) CloseHandle(h); }
    bool Initialize() {
        if (!root_.is_absolute()) return false;
        auto path = root_.root_path();
        for (const auto& component : root_.relative_path()) {
            if (component == L".." || component == L".") return false;
            path /= component;
            if (path.native().size() == 6 && path.native().back() == L':') path += L"\\";
            if (!Guard(path)) return false;
        }
        return std::filesystem::is_empty(root_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != Guid(6, 0x20) && id != Guid(0, 5)) return E_NOINTERFACE;
        *out = static_cast<ExtractCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetTotal(uint64_t) noexcept override { return Notify(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const uint64_t*) noexcept override {
        // 7-Zip processing progress is not the number of bytes written to staging.
        return Notify();
    }
    HRESULT STDMETHODCALLTYPE GetStream(unsigned index, OutStream** out, int mode) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        try {
            output_.reset(); if (Cancelled(cancel_)) return E_ABORT;
            if (index >= entries_.size() || mode != 0) return E_FAIL;
            const auto& entry = entries_[index]; progress_.path = entry.path;
            if (Notify() != S_OK) return E_ABORT;
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entry.path.data(), static_cast<int>(entry.path.size()), nullptr, 0);
            if (!count) return E_FAIL;
            std::wstring relative(count, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, entry.path.data(), static_cast<int>(entry.path.size()), relative.data(), count);
            std::replace(relative.begin(), relative.end(), L'/', L'\\');
            auto path = root_; const std::filesystem::path rel(relative);
            for (auto at = rel.begin(); at != rel.end(); ++at) {
                path /= *at; auto next = at; ++next;
                if (next != rel.end() || entry.dir) {
                    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return E_FAIL;
                    if (!Guard(path)) return E_FAIL;
                }
            }
            if (entry.dir) return S_OK;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) return E_FAIL;
            try { output_.reset(new Output(h, entry.size, cancel_, progress_, [this] { return Notify(); })); } catch (...) { CloseHandle(h); throw; }
            output_->AddRef(); *out = output_.get(); return S_OK;
        } catch (...) { failed = true; return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(int) noexcept override { return Cancelled(cancel_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(int result) noexcept override {
        if (result != 0) failed = true;
        if (output_) { if (!output_->Finish()) failed = true; output_.reset(); }
        ++progress_.completed_items;
        return failed ? E_FAIL : Notify();
    }
};
}

static Result Process(const std::wstring& path, const std::function<bool()>& cancel,
    const std::wstring* staging, const std::function<void(const ExtractionProgress&)>& notify, uint64_t max_bytes) {
    if (Cancelled(cancel)) return Failure(Status::Cancelled, "Cancelled");
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return Failure(Status::IoError, "Invalid archive path");
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Failure(Status::IoError, "Cannot open archive file");
    std::unique_ptr<FileStream, ComRelease<FileStream>> stream;
    try { stream.reset(new FileStream(handle, cancel)); } catch (...) { CloseHandle(handle); return Failure(Status::IoError, "Cannot allocate archive stream"); }
    try {
        std::unique_ptr<Progress, ComRelease<Progress>> progress(new Progress(cancel));
        unsigned formats = 0;
        if (GetNumberOfFormats(&formats) != S_OK || formats > 256) return Failure(Status::Unsupported, "7-Zip format discovery failed");
        for (unsigned format = 0; format < formats; ++format) {
            if (Cancelled(cancel)) return Failure(Status::Cancelled, "Cancelled");
            Variant name, cls;
            if (GetHandlerProperty2(format, 0, &name.value) != S_OK || name.value.vt != VT_BSTR) continue;
            const std::string label = Utf8(name.value.bstrVal);
            if (label != "7z" && label != "Rar" && label != "Rar5") continue;
            if (GetHandlerProperty2(format, 1, &cls.value) != S_OK || cls.value.vt != VT_BSTR || SysStringByteLen(cls.value.bstrVal) != sizeof(GUID))
                return Failure(Status::InvalidArchive, "Invalid 7-Zip handler class");
            GUID class_id{}; std::memcpy(&class_id, cls.value.bstrVal, sizeof(class_id));
            Archive* raw = nullptr;
            if (CreateObject(&class_id, &kArchive, reinterpret_cast<void**>(&raw)) != S_OK || !raw)
                return Failure(Status::Unsupported, "Cannot create 7-Zip archive handler");
            std::unique_ptr<Archive, ArchiveRelease> archive(raw);
            if (stream->Seek(0, FILE_BEGIN, nullptr) != S_OK) return Failure(Cancelled(cancel) ? Status::Cancelled : Status::IoError, "Cannot seek archive stream");
            const uint64_t search_limit = 0;
            const auto opened = archive->Open(stream.get(), &search_limit, progress.get());
            if (Cancelled(cancel) || opened == E_ABORT) return Failure(Status::Cancelled, "Cancelled");
            if (opened == S_FALSE) continue;
            if (opened != S_OK) return Failure(Status::InvalidArchive, "Archive headers are unreadable, encrypted or damaged");
            for (const PROPID flag : {71u, 72u}) {
                Variant diagnostic;
                if (archive->GetArchiveProperty(flag, &diagnostic.value) != S_OK || Number(diagnostic.value) != 0)
                    return Failure(Status::InvalidArchive, "Archive reports damaged or incomplete headers");
            }
            Result result; result.status = Status::Ok; result.format = label;
            unsigned count = 0;
            if (archive->GetNumberOfItems(&count) != S_OK) return Failure(Status::InvalidArchive, "Cannot read archive item count");
            const unsigned bounded = std::min(count, 4096u);
            result.entries.reserve(bounded);
            size_t path_bytes = 0;
            for (unsigned i = 0; i < bounded; ++i) {
                if (Cancelled(cancel)) return Failure(Status::Cancelled, "Cancelled");
                Entry entry;
                for (const PROPID property : {3u, 6u, 7u, 8u, 12u, 15u}) {
                    Variant value;
                    if (archive->GetProperty(i, property, &value.value) != S_OK) return Failure(Status::InvalidArchive, "Cannot read archive item metadata");
                    switch (property) {
                    case 3:
                        if (value.value.vt != VT_BSTR) return Failure(Status::InvalidArchive, "Archive item has no path");
                        entry.path = Utf8(value.value.bstrVal); break;
                    case 6: case 15:
                        if (value.value.vt != VT_BOOL && value.value.vt != VT_EMPTY) return Failure(Status::InvalidArchive, "Invalid archive boolean property");
                        (property == 6 ? entry.dir : entry.encrypted) = value.value.vt == VT_BOOL && value.value.boolVal != VARIANT_FALSE; break;
                    case 7: entry.size = Number(value.value); break;
                    case 8: entry.packed = Number(value.value); break;
                    case 12:
                        if (value.value.vt == VT_FILETIME) entry.modified = (uint64_t(value.value.filetime.dwHighDateTime) << 32) | value.value.filetime.dwLowDateTime;
                        else if (value.value.vt != VT_EMPTY) return Failure(Status::InvalidArchive, "Invalid archive timestamp");
                        break;
                    }
                }
                path_bytes += entry.path.size();
                if (path_bytes > 4 * 1024 * 1024) { result.incomplete = true; break; }
                result.entries.push_back(std::move(entry));
            }
            result.incomplete = result.incomplete || count > bounded;
            if (result.incomplete) { result.status = Status::LimitReached; result.error = "Archive listing reached the item or path budget"; }
            if (staging && result.status == Status::Ok) {
                uint64_t total = 0;
                for (unsigned i = 0; i < count; ++i) {
                    const auto& entry = result.entries[i];
                    if (!ValidExtractionPath(entry.path) || entry.encrypted)
                        return Failure(Status::InvalidArchive, "Unsafe or encrypted archive entry");
                    for (PROPID property : {9u, 21u, 53u, 54u, 89u, 90u}) {
                        Variant value;
                        if (archive->GetProperty(i, property, &value.value) != S_OK)
                            return Failure(Status::InvalidArchive, "Cannot inspect archive link metadata");
                        if ((property == 9 && ((Number(value.value) & FILE_ATTRIBUTE_REPARSE_POINT) || ((Number(value.value) >> 16) & 0170000) == 0120000)) ||
                            (property == 53 && (Number(value.value) & 0170000) == 0120000) ||
                            (property == 21 && value.value.vt == VT_BOOL && value.value.boolVal) ||
                            ((property == 54 || property == 89 || property == 90) && value.value.vt != VT_EMPTY))
                            return Failure(Status::InvalidArchive, "Archive links and anti-items are not supported");
                    }
                    if (!entry.dir) {
                        if (entry.size > max_bytes - total) return Failure(Status::LimitReached, "Extraction byte budget exceeded");
                        total += entry.size;
                    }
                }
                std::unique_ptr<Extraction, ComRelease<Extraction>> extraction(new Extraction(*staging, result.entries, cancel, notify, total));
                if (!extraction->Initialize()) return Failure(Status::IoError, "Staging must be an existing empty non-reparse directory");
                const auto status = archive->Extract(nullptr, UINT32_MAX, 0, extraction.get());
                if (Cancelled(cancel) || status == E_ABORT) return Failure(Status::Cancelled, "Cancelled");
                if (status != S_OK || extraction->failed) return Failure(Status::InvalidArchive, "Archive extraction or output validation failed");
            }
            return result;
        }
        return Failure(Status::Unsupported, "Not a supported 7z or RAR archive");
    } catch (const std::length_error& error) { return Failure(Status::LimitReached, error.what()); }
    catch (const std::exception& error) { return Failure(Status::InvalidArchive, error.what()); }
}

Result Read(const std::wstring& path, const std::function<bool()>& cancel) {
    return Process(path, cancel, nullptr, {}, 0);
}
Result Extract(const std::wstring& path, const std::wstring& staging,
    const std::function<bool()>& cancel, const std::function<void(const ExtractionProgress&)>& progress, uint64_t max_bytes) {
    return Process(path, cancel, &staging, progress, max_bytes);
}
}
