#include "engine.h"
#include "sevenzip_com.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace engine {
namespace {
using namespace sevenzip;
namespace fs = std::filesystem;

constexpr size_t kMaxCreateItems = 1000000;
constexpr unsigned kMaxNestDepth = 2;

bool IsCancelled(const Cancelled& cancelled) noexcept {
    try { return cancelled && cancelled(); } catch (...) { return true; }
}
std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}
std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring LongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.size() < 240) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
uint64_t Number(const PROPVARIANT& value) {
    switch (value.vt) {
    case VT_UI8: return value.uhVal.QuadPart;
    case VT_UI4: return value.ulVal;
    case VT_UI2: return value.uiVal;
    case VT_UI1: return value.bVal;
    case VT_I8: return static_cast<uint64_t>(value.hVal.QuadPart);
    case VT_I4: return static_cast<uint64_t>(value.lVal);
    default: return 0;
    }
}
bool Bool(const PROPVARIANT& value) { return value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE; }
std::wstring String(const PROPVARIANT& value) { return value.vt == VT_BSTR && value.bstrVal ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) : std::wstring{}; }

// ---- handlers ---------------------------------------------------------------
struct Handler {
    std::wstring name, lower;
    GUID cls{};
    std::vector<std::wstring> extensions;  // lowercase, no dot
    bool update = false;
};
const std::vector<Handler>& Handlers() {
    static std::once_flag once;
    static std::vector<Handler> handlers;
    std::call_once(once, [] {
        unsigned count = 0;
        if (GetNumberOfFormats(&count) != S_OK || count > 512) return;
        for (unsigned i = 0; i < count; ++i) {
            Variant name, cls, ext, update;
            if (GetHandlerProperty2(i, kHandlerName, &name.value) != S_OK || name.value.vt != VT_BSTR) continue;
            if (GetHandlerProperty2(i, kHandlerClassID, &cls.value) != S_OK || cls.value.vt != VT_BSTR ||
                SysStringByteLen(cls.value.bstrVal) != sizeof(GUID)) continue;
            Handler handler;
            handler.name = String(name.value);
            handler.lower = Lower(handler.name);
            std::memcpy(&handler.cls, cls.value.bstrVal, sizeof(GUID));
            if (GetHandlerProperty2(i, kHandlerExtension, &ext.value) == S_OK) {
                std::wstring list = Lower(String(ext.value));
                size_t start = 0;
                while (start < list.size()) {
                    const auto end = list.find(L' ', start);
                    auto item = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                    if (!item.empty()) handler.extensions.push_back(item);
                    if (end == std::wstring::npos) break;
                    start = end + 1;
                }
            }
            if (GetHandlerProperty2(i, kHandlerUpdate, &update.value) == S_OK) handler.update = Bool(update.value);
            handlers.push_back(std::move(handler));
        }
    });
    return handlers;
}
const Handler* FindHandler(std::wstring_view lower_name) {
    for (const auto& handler : Handlers()) if (handler.lower == lower_name) return &handler;
    return nullptr;
}
// Single-stream wrappers: their one item may be an archive itself.
bool IsWrapper(const std::wstring& lower) {
    return lower == L"gzip" || lower == L"bzip2" || lower == L"xz" || lower == L"lzma" || lower == L"z" ||
           lower == L"split" || lower == L"zstd" || lower == L"lz4" || lower == L"lzip";
}
std::wstring Extension(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    const auto slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    return Lower(name.substr(dot + 1));
}
std::vector<const Handler*> Candidates(const std::wstring& file_name) {
    std::vector<const Handler*> list;
    const auto add = [&](const Handler* handler) {
        if (handler && std::find(list.begin(), list.end(), handler) == list.end()) list.push_back(handler);
    };
    const auto ext = Extension(file_name);
    if (!ext.empty() && std::all_of(ext.begin(), ext.end(), iswdigit)) add(FindHandler(L"split"));
    for (const auto& handler : Handlers())
        if (!ext.empty() && std::find(handler.extensions.begin(), handler.extensions.end(), ext) != handler.extensions.end()) add(&handler);
    for (const auto* name : {L"7z", L"zip", L"rar5", L"rar", L"tar", L"gzip", L"xz", L"bzip2", L"iso", L"udf", L"cab", L"wim", L"arj", L"lzh"})
        add(FindHandler(name));
    return list;
}

// ---- streams ----------------------------------------------------------------
class FileIn final : public IInStream, public RefCounted<FileIn> {
    HANDLE file_;
    const Cancelled& cancelled_;
public:
    FileIn(HANDLE file, const Cancelled& cancelled) : file_(file), cancelled_(cancelled) {}
    ~FileIn() override { CloseHandle(file_); }
    static FileIn* Open(const std::wstring& path, const Cancelled& cancelled) {
        HANDLE file = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return file == INVALID_HANDLE_VALUE ? nullptr : new FileIn(file, cancelled);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kIID_InStream && id != kIID_SequentialIn) return E_NOINTERFACE;
        *out = static_cast<IInStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* read) noexcept override {
        if (read) *read = 0;
        if (IsCancelled(cancelled_)) return E_ABORT;
        DWORD done = 0;
        if (!ReadFile(file_, data, std::min<UInt32>(size, 4u << 20), &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read) *read = done;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* position) noexcept override {
        if (origin > FILE_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER move{}, result{};
        move.QuadPart = offset;
        if (!SetFilePointerEx(file_, move, &result, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (position) *position = static_cast<UInt64>(result.QuadPart);
        return S_OK;
    }
};

// Plain file output; also the base of one volume.
class FileOut final : public IOutStream, public RefCounted<FileOut> {
    HANDLE file_;
    const Cancelled& cancelled_;
public:
    FileOut(HANDLE file, const Cancelled& cancelled) : file_(file), cancelled_(cancelled) {}
    ~FileOut() override { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    bool Close() { const bool ok = FlushFileBuffers(file_) != FALSE; CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; return ok; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kIID_OutStream && id != kIID_SequentialOut) return E_NOINTERFACE;
        *out = static_cast<IOutStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* written) noexcept override {
        if (written) *written = 0;
        if (IsCancelled(cancelled_)) return E_ABORT;
        DWORD done = 0;
        if (!WriteFile(file_, data, size, &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (written) *written = done;
        return done == size ? S_OK : STG_E_WRITEFAULT;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* position) noexcept override {
        if (origin > FILE_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER move{}, result{};
        move.QuadPart = offset;
        if (!SetFilePointerEx(file_, move, &result, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (position) *position = static_cast<UInt64>(result.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 size) noexcept override {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return SetFileInformationByHandle(file_, FileEndOfFileInfo, &info, sizeof(info)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
};

// name.001, name.002 ... of `volume` bytes each; seekable like one stream.
class VolumesOut final : public IOutStream, public RefCounted<VolumesOut> {
    std::wstring base_;
    uint64_t volume_, position_ = 0, length_ = 0;
    std::vector<HANDLE> files_;
    const Cancelled& cancelled_;
    HANDLE Volume(size_t index) {
        while (files_.size() <= index) {
            wchar_t suffix[16];
            swprintf_s(suffix, L".%03zu", files_.size() + 1);
            HANDLE file = CreateFileW(LongPath(base_ + suffix).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
            files_.push_back(file);
        }
        return files_[index];
    }
public:
    std::vector<std::wstring> names;
    VolumesOut(std::wstring base, uint64_t volume, const Cancelled& cancelled)
        : base_(std::move(base)), volume_(volume), cancelled_(cancelled) {}
    ~VolumesOut() override { for (HANDLE file : files_) CloseHandle(file); }
    bool Close() {
        bool ok = true;
        names.clear();
        for (size_t i = 0; i < files_.size(); ++i) {
            ok = FlushFileBuffers(files_[i]) && ok;
            CloseHandle(files_[i]);
            wchar_t suffix[16];
            swprintf_s(suffix, L".%03zu", i + 1);
            names.push_back(base_ + suffix);
        }
        files_.clear();
        return ok;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kIID_OutStream && id != kIID_SequentialOut) return E_NOINTERFACE;
        *out = static_cast<IOutStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* written) noexcept override {
        if (written) *written = 0;
        if (IsCancelled(cancelled_)) return E_ABORT;
        const auto* bytes = static_cast<const uint8_t*>(data);
        UInt32 total = 0;
        while (total < size) {
            const size_t index = static_cast<size_t>(position_ / volume_);
            const uint64_t offset = position_ % volume_;
            const UInt32 chunk = static_cast<UInt32>(std::min<uint64_t>(size - total, volume_ - offset));
            HANDLE file = Volume(index);
            if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
            LARGE_INTEGER move{};
            move.QuadPart = static_cast<LONGLONG>(offset);
            DWORD done = 0;
            if (!SetFilePointerEx(file, move, nullptr, FILE_BEGIN) || !WriteFile(file, bytes + total, chunk, &done, nullptr) || done != chunk)
                return HRESULT_FROM_WIN32(GetLastError());
            total += chunk;
            position_ += chunk;
            length_ = std::max(length_, position_);
        }
        if (written) *written = total;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* position) noexcept override {
        const Int64 base = origin == FILE_BEGIN ? 0 : origin == FILE_CURRENT ? static_cast<Int64>(position_)
                         : origin == FILE_END ? static_cast<Int64>(length_) : -1;
        if (base < 0 || base + offset < 0) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<uint64_t>(base + offset);
        if (position) *position = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 size) noexcept override {
        length_ = size;
        const size_t keep = size == 0 ? 1 : static_cast<size_t>((size + volume_ - 1) / volume_);
        for (size_t i = 0; i < files_.size(); ++i) {
            FILE_END_OF_FILE_INFO info{};
            info.EndOfFile.QuadPart = i + 1 < keep ? static_cast<LONGLONG>(volume_)
                : i + 1 == keep ? static_cast<LONGLONG>(size - volume_ * i) : 0;
            if (!SetFileInformationByHandle(files_[i], FileEndOfFileInfo, &info, sizeof(info))) return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }
};

// ---- opening ----------------------------------------------------------------
struct PasswordState {
    const AskPassword* ask = nullptr;
    std::wstring value;
    bool known = false, asked = false, refused = false;
    HRESULT Get(BSTR* out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!known) {
            asked = true;
            std::wstring entered;
            if (!ask || !*ask || !(*ask)(false, entered)) { refused = true; return E_ABORT; }
            value = std::move(entered);
            known = true;
        }
        *out = SysAllocString(value.c_str());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
};

class OpenCallback final : public IArchiveOpenCallback, public IArchiveOpenVolumeCallback,
                           public ICryptoGetTextPassword, public RefCounted<OpenCallback> {
    fs::path folder_;
    std::wstring name_;
    const Cancelled& cancelled_;
    PasswordState& password_;
public:
    OpenCallback(const fs::path& path, const Cancelled& cancelled, PasswordState& password)
        : folder_(path.parent_path()), name_(path.filename().wstring()), cancelled_(cancelled), password_(password) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == kIID_OpenCallback) *out = static_cast<IArchiveOpenCallback*>(this);
        else if (id == kIID_OpenVolumeCallback) *out = static_cast<IArchiveOpenVolumeCallback*>(this);
        else if (id == kIID_GetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) noexcept override { return IsCancelled(cancelled_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) noexcept override { return IsCancelled(cancelled_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* value) noexcept override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (id == kpidName) {
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(name_.c_str());
            return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, IInStream** stream) noexcept override {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        if (!name || wcschr(name, L'\\') || wcschr(name, L'/') || wcsstr(name, L"..")) return S_FALSE;
        try {
            auto* file = FileIn::Open((folder_ / name).wstring(), cancelled_);
            if (!file) return S_FALSE;
            name_ = name;
            *stream = file;
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* out) noexcept override {
        try { return password_.Get(out); } catch (...) { return E_FAIL; }
    }
};

struct ArchiveRelease { void operator()(IInArchive* value) const { if (value) { value->Close(); value->Release(); } } };
using ArchivePtr = std::unique_ptr<IInArchive, ArchiveRelease>;

class TempFolder {
    fs::path path_;
public:
    TempFolder() = default;
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    ~TempFolder() { std::error_code ec; if (!path_.empty()) fs::remove_all(path_, ec); }
    // In `parent`, else PULSE_7ZIP_TEMP, else the user's temporary folder. When that is refused
    // (security software may block writes to %TEMP%), a hidden folder in `fallback` is used.
    const fs::path& Create(const fs::path& parent = {}, const fs::path& fallback = {}) {
        fs::path root = parent;
        if (root.empty()) {
            wchar_t buffer[MAX_PATH + 1]{};
            const DWORD length = GetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", buffer, MAX_PATH);
            if (!length || length >= MAX_PATH) GetTempPathW(MAX_PATH, buffer);
            root = buffer;
        }
        DWORD error = TryCreate(root);
        if (path_.empty() && parent.empty() && !fallback.empty()) {
            error = TryCreate(fallback);
            if (!path_.empty()) SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_HIDDEN);
        }
        if (path_.empty()) throw fs::filesystem_error("cannot create a temporary folder", root,
            std::error_code(static_cast<int>(error), std::system_category()));
        return path_;
    }
private:
    DWORD TryCreate(const fs::path& root) {
        DWORD error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 100 && path_.empty(); ++attempt) {
            auto candidate = root / (L".PulseZip-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                               std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(candidate.c_str(), nullptr)) path_ = candidate;
            else if ((error = GetLastError()) != ERROR_ALREADY_EXISTS) break;
        }
        return error;
    }
};

struct Opened {
    ArchivePtr archive;
    std::wstring format;
    bool nested = false;
    std::vector<std::unique_ptr<TempFolder>> temps;  // outer archives' extracted inner files
    std::vector<ArchivePtr> outers;
};

// Extracts item 0 of a wrapper format into `target` (used for nesting).
class SingleOut final : public IArchiveExtractCallback, public ICryptoGetTextPassword, public RefCounted<SingleOut> {
    std::wstring target_;
    const Cancelled& cancelled_;
    PasswordState& password_;
public:
    Int32 result = kOpOk;
    SingleOut(std::wstring target, const Cancelled& cancelled, PasswordState& password)
        : target_(std::move(target)), cancelled_(cancelled), password_(password) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == kIID_ExtractCallback || id == kIID_Progress) *out = static_cast<IArchiveExtractCallback*>(this);
        else if (id == kIID_GetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return IsCancelled(cancelled_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept override { return IsCancelled(cancelled_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32, ISequentialOutStream** out, Int32 mode) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (mode != kAskExtract) return S_OK;
        HANDLE file = CreateFileW(LongPath(target_).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        try { *out = new FileOut(file, cancelled_); } catch (...) { CloseHandle(file); return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 value) noexcept override { result = value; return S_OK; }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* out) noexcept override {
        try { return password_.Get(out); } catch (...) { return E_FAIL; }
    }
};

ArchivePtr Create(const Handler& handler) {
    IInArchive* raw = nullptr;
    if (CreateObject(&handler.cls, &kIID_InArchive, reinterpret_cast<void**>(&raw)) != S_OK || !raw) return nullptr;
    return ArchivePtr(raw);
}

Status OpenFile(const fs::path& path, const Cancelled& cancelled, PasswordState& password, Opened& out, unsigned depth) {
    std::unique_ptr<FileIn, ComRelease<FileIn>> stream(FileIn::Open(path.wstring(), cancelled));
    if (!stream) return Status::IoError;
    for (const Handler* handler : Candidates(path.filename().wstring())) {
        if (IsCancelled(cancelled)) return Status::Cancelled;
        auto archive = Create(*handler);
        if (!archive) continue;
        if (stream->Seek(0, FILE_BEGIN, nullptr) != S_OK) return Status::IoError;
        std::unique_ptr<OpenCallback, ComRelease<OpenCallback>> callback(new OpenCallback(path, cancelled, password));
        const UInt64 scan = 1u << 20;
        const HRESULT opened = archive->Open(stream.get(), &scan, callback.get());
        if (IsCancelled(cancelled) || (opened == E_ABORT && !password.refused)) return Status::Cancelled;
        if (password.refused) return Status::NeedPassword;
        if (opened != S_OK) {
            if (password.asked && password.known && (handler->lower == L"7z" || handler->lower.rfind(L"rar", 0) == 0))
                return Status::WrongPassword;  // header encryption: a wrong password makes headers unreadable
            continue;
        }
        UInt32 count = 0;
        archive->GetNumberOfItems(&count);
        if (depth < kMaxNestDepth && IsWrapper(handler->lower) && count == 1) {
            Variant inner_name;
            archive->GetProperty(0, kpidPath, &inner_name.value);
            std::wstring name = String(inner_name.value);
            if (name.empty()) {
                name = path.stem().wstring();
                if (handler->lower == L"gzip" && Lower(path.extension().wstring()) == L".tgz") name += L".tar";
            }
            const auto inner_ext = Extension(name);
            const bool archive_inside = handler->lower == L"split" || inner_ext == L"tar" ||
                std::any_of(Handlers().begin(), Handlers().end(), [&](const Handler& h) {
                    return !IsWrapper(h.lower) && std::find(h.extensions.begin(), h.extensions.end(), inner_ext) != h.extensions.end();
                });
            if (archive_inside && SafeRelativePath(name) && name.find(L'\\') == std::wstring::npos && name.find(L'/') == std::wstring::npos) {
                auto temp = std::make_unique<TempFolder>();
                const auto target = temp->Create({}, path.parent_path()) / name;
                std::unique_ptr<SingleOut, ComRelease<SingleOut>> single(new SingleOut(target.wstring(), cancelled, password));
                const UInt32 index = 0;
                const HRESULT extracted = archive->Extract(&index, 1, kAskExtract, single.get());
                if (IsCancelled(cancelled)) return Status::Cancelled;
                if (extracted == S_OK && single->result == kOpOk) {
                    Opened inner;
                    const auto status = OpenFile(target, cancelled, password, inner, depth + 1);
                    if (status == Status::Ok) {
                        out = std::move(inner);
                        out.nested = true;
                        out.temps.push_back(std::move(temp));
                        out.outers.push_back(std::move(archive));
                        return Status::Ok;
                    }
                    if (status == Status::Cancelled || status == Status::NeedPassword || status == Status::WrongPassword) return status;
                }
            }
        }
        out.archive = std::move(archive);
        out.format = handler->name;
        return Status::Ok;
    }
    return Status::Unsupported;
}

std::string StatusText(Status status) {
    switch (status) {
    case Status::Ok: return {};
    case Status::Cancelled: return "Cancelled";
    case Status::NeedPassword: return "A password is required";
    case Status::WrongPassword: return "Wrong password";
    case Status::Unsupported: return "Not an archive 7-Zip can open";
    case Status::IoError: return "Cannot read the archive";
    case Status::LimitReached: return "Archive is too large to list";
    case Status::InvalidInput: return "Invalid input";
    default: return "Archive is damaged";
    }
}

bool ReadEntry(IInArchive* archive, UInt32 index, Entry& entry) {
    Variant path, dir, size, packed, mtime, attrib, encrypted, posix, symlink, hardlink, copylink, anti;
    archive->GetProperty(index, kpidPath, &path.value);
    archive->GetProperty(index, kpidIsDir, &dir.value);
    archive->GetProperty(index, kpidSize, &size.value);
    archive->GetProperty(index, kpidPackSize, &packed.value);
    archive->GetProperty(index, kpidMTime, &mtime.value);
    archive->GetProperty(index, kpidAttrib, &attrib.value);
    archive->GetProperty(index, kpidEncrypted, &encrypted.value);
    archive->GetProperty(index, kpidPosixAttrib, &posix.value);
    archive->GetProperty(index, kpidSymLink, &symlink.value);
    archive->GetProperty(index, kpidHardLink, &hardlink.value);
    archive->GetProperty(index, kpidCopyLink, &copylink.value);
    archive->GetProperty(index, kpidIsAnti, &anti.value);
    entry.path = String(path.value);
    std::replace(entry.path.begin(), entry.path.end(), L'/', L'\\');
    entry.dir = Bool(dir.value);
    entry.size = Number(size.value);
    entry.packed = Number(packed.value);
    if (mtime.value.vt == VT_FILETIME)
        entry.modified = (uint64_t(mtime.value.filetime.dwHighDateTime) << 32) | mtime.value.filetime.dwLowDateTime;
    const uint64_t attributes = Number(attrib.value);
    entry.attributes = static_cast<uint32_t>(attributes & 0xFFFF);
    entry.encrypted = Bool(encrypted.value);
    const bool unix_link = ((attributes & 0x8000) && ((attributes >> 16) & 0170000) == 0120000) ||
                           (posix.value.vt != VT_EMPTY && (Number(posix.value) & 0170000) == 0120000);
    entry.link = unix_link || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || symlink.value.vt == VT_BSTR ||
                 hardlink.value.vt == VT_BSTR || copylink.value.vt == VT_BSTR;
    return !Bool(anti.value);
}

// ---- extraction ---------------------------------------------------------------
class ItemOut final : public ISequentialOutStream, public RefCounted<ItemOut> {
    HANDLE file_;
    const Cancelled& cancelled_;
    uint64_t& written_;
public:
    ItemOut(HANDLE file, const Cancelled& cancelled, uint64_t& written) : file_(file), cancelled_(cancelled), written_(written) {}
    ~ItemOut() override { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    HANDLE Handle() const { return file_; }
    void Close() { if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; } }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kIID_SequentialOut) return E_NOINTERFACE;
        *out = static_cast<ISequentialOutStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* written) noexcept override {
        if (written) *written = 0;
        if (IsCancelled(cancelled_)) return E_ABORT;
        DWORD done = 0;
        if (!WriteFile(file_, data, size, &done, nullptr) || done != size) return HRESULT_FROM_WIN32(GetLastError());
        written_ += done;
        if (written) *written = done;
        return S_OK;
    }
};

class Extraction final : public IArchiveExtractCallback, public ICryptoGetTextPassword, public RefCounted<Extraction> {
    fs::path root_;
    const std::vector<Entry>& entries_;
    const std::vector<bool>& wanted_;
    bool test_;
    const Cancelled& cancelled_;
    const Report& report_;
    PasswordState& password_;
    Progress progress_;
    std::unique_ptr<ItemOut, ComRelease<ItemOut>> out_;
    UInt32 current_ = 0;
    uint64_t written_ = 0;
    struct DirTime { fs::path path; FILETIME time; };
    std::vector<DirTime> dirs_;
    void Notify() { try { if (report_) report_(progress_); } catch (...) {} }
    bool MakeDirs(const fs::path& path) {
        std::error_code ec;
        fs::create_directories(LongPath(path.wstring()), ec);
        return !ec || fs::is_directory(LongPath(path.wstring()));
    }
public:
    Int32 worst = kOpOk;
    bool wrong_password = false, failed = false;
    uint64_t files = 0;
    Extraction(fs::path root, const std::vector<Entry>& entries, const std::vector<bool>& wanted, bool test,
               const Cancelled& cancelled, const Report& report, PasswordState& password, uint64_t total, uint64_t items)
        : root_(std::move(root)), entries_(entries), wanted_(wanted), test_(test), cancelled_(cancelled),
          report_(report), password_(password) {
        progress_.total_bytes = total;
        progress_.total_items = items;
    }
    uint64_t Written() const { return written_; }
    void FinishDirs() {
        for (auto it = dirs_.rbegin(); it != dirs_.rend(); ++it) {
            HANDLE dir = CreateFileW(LongPath(it->path.wstring()).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (dir != INVALID_HANDLE_VALUE) { SetFileTime(dir, nullptr, nullptr, &it->time); CloseHandle(dir); }
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == kIID_ExtractCallback || id == kIID_Progress) *out = static_cast<IArchiveExtractCallback*>(this);
        else if (id == kIID_GetTextPassword) *out = static_cast<ICryptoGetTextPassword*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept override {
        if (total) progress_.total_bytes = total;
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* value) noexcept override {
        if (value) progress_.completed_bytes = std::min<uint64_t>(*value, progress_.total_bytes ? progress_.total_bytes : *value);
        Notify();
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream** out, Int32 mode) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        try {
            out_.reset();
            current_ = index;
            if (IsCancelled(cancelled_)) return E_ABORT;
            if (index >= entries_.size() || !wanted_[index] || mode != kAskExtract) return S_OK;
            const auto& entry = entries_[index];
            progress_.current = entry.path;
            Notify();
            if (test_) return S_OK;
            const fs::path path = root_ / entry.path;
            if (entry.dir) {
                if (!MakeDirs(path)) { failed = true; return E_FAIL; }
                if (entry.modified) {
                    FILETIME time{static_cast<DWORD>(entry.modified), static_cast<DWORD>(entry.modified >> 32)};
                    dirs_.push_back({path, time});
                }
                return S_OK;
            }
            if (!MakeDirs(path.parent_path())) { failed = true; return E_FAIL; }
            HANDLE file = CreateFileW(LongPath(path.wstring()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) { failed = true; return HRESULT_FROM_WIN32(GetLastError()); }
            out_.reset(new ItemOut(file, cancelled_, written_));
            out_->AddRef();
            *out = out_.get();
            return S_OK;
        } catch (...) { failed = true; return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return IsCancelled(cancelled_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        const bool tracked = current_ < entries_.size() && wanted_[current_];
        const auto& entry = tracked ? entries_[current_] : Entry{};
        if (out_) {
            if (entry.modified) {
                FILETIME time{static_cast<DWORD>(entry.modified), static_cast<DWORD>(entry.modified >> 32)};
                SetFileTime(out_->Handle(), nullptr, nullptr, &time);
            }
            out_->Close();
            const DWORD keep = entry.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
            if (keep && result == kOpOk) SetFileAttributesW(LongPath((root_ / entry.path).wstring()).c_str(), keep);
            out_.reset();
        }
        if (tracked) {
            if (result == kOpWrongPassword || (entry.encrypted && (result == kOpDataError || result == kOpCrcError)))
                wrong_password = true;
            if (result != kOpOk && worst == kOpOk) worst = result;
            if (!entry.dir && result == kOpOk) ++files;
            ++progress_.completed_items;
            Notify();
        }
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* out) noexcept override {
        try { return password_.Get(out); } catch (...) { return E_FAIL; }
    }
};

// ---- creation ---------------------------------------------------------------
struct Item {
    std::wstring path, name;
    bool dir = false;
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attributes = 0;
};
void Collect(const std::wstring& source, const std::wstring& name, unsigned depth, std::vector<Item>& items,
             std::set<std::wstring>& names, uint64_t& total, uint64_t& skipped, const Cancelled& cancelled) {
    if (IsCancelled(cancelled)) throw std::runtime_error("cancelled");
    if (depth > 128 || items.size() >= kMaxCreateItems) throw std::length_error("too many items");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(LongPath(source).c_str(), GetFileExInfoStandard, &data)) throw std::runtime_error("cannot read " + Narrow(source));
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++skipped; return; }  // links and junctions are not followed
    Item item{source, name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
              (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, data.ftLastWriteTime, data.dwFileAttributes};
    if (item.dir) item.size = 0;
    if (!names.insert(Lower(item.name)).second) throw std::runtime_error("two items would share the name " + Narrow(name));
    total += item.size;
    items.push_back(item);
    if (!item.dir) return;
    std::vector<std::wstring> children;
    WIN32_FIND_DATAW find{};
    HANDLE handle = FindFirstFileExW(LongPath(source + L"\\*").c_str(), FindExInfoBasic, &find, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(find.cFileName, L".") && wcscmp(find.cFileName, L"..")) children.emplace_back(find.cFileName);
    } while (FindNextFileW(handle, &find));
    FindClose(handle);
    std::sort(children.begin(), children.end());
    for (const auto& child : children)
        Collect(source + L"\\" + child, name + L"\\" + child, depth + 1, items, names, total, skipped, cancelled);
}

class ItemIn final : public ISequentialInStream, public RefCounted<ItemIn> {
    HANDLE file_;
    const Cancelled& cancelled_;
public:
    ItemIn(HANDLE file, const Cancelled& cancelled) : file_(file), cancelled_(cancelled) {}
    ~ItemIn() override { CloseHandle(file_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != kIID_SequentialIn) return E_NOINTERFACE;
        *out = static_cast<ISequentialInStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* read) noexcept override {
        if (read) *read = 0;
        if (IsCancelled(cancelled_)) return E_ABORT;
        DWORD done = 0;
        if (!ReadFile(file_, data, std::min<UInt32>(size, 4u << 20), &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read) *read = done;
        return S_OK;
    }
};

class Update final : public IArchiveUpdateCallback, public ICryptoGetTextPassword2, public RefCounted<Update> {
    const std::vector<Item>& items_;
    const std::wstring& password_;
    const Cancelled& cancelled_;
    const Report& report_;
    Progress progress_;
public:
    Update(const std::vector<Item>& items, const std::wstring& password, uint64_t total, const Cancelled& cancelled, const Report& report)
        : items_(items), password_(password), cancelled_(cancelled), report_(report) {
        progress_.total_bytes = total;
        progress_.total_items = items.size();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == kIID_UpdateCallback || id == kIID_Progress) *out = static_cast<IArchiveUpdateCallback*>(this);
        else if (id == kIID_GetTextPassword2) *out = static_cast<ICryptoGetTextPassword2*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return DoAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return DoRelease(); }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept override {
        if (total) progress_.total_bytes = total;
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* value) noexcept override {
        if (value) progress_.completed_bytes = std::min<uint64_t>(*value, progress_.total_bytes ? progress_.total_bytes : *value);
        try { if (report_) report_(progress_); } catch (...) {}
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 index, Int32* data, Int32* props, UInt32* archive_index) noexcept override {
        if (index >= items_.size()) return E_INVALIDARG;
        if (data) *data = 1;
        if (props) *props = 1;
        if (archive_index) *archive_index = UINT32_MAX;
        return IsCancelled(cancelled_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID id, PROPVARIANT* value) noexcept override {
        if (!value || index >= items_.size()) return E_INVALIDARG;
        PropVariantInit(value);
        const auto& item = items_[index];
        switch (id) {
        case kpidPath: value->vt = VT_BSTR; value->bstrVal = SysAllocString(item.name.c_str()); return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        case kpidIsDir: value->vt = VT_BOOL; value->boolVal = item.dir ? VARIANT_TRUE : VARIANT_FALSE; break;
        case kpidSize: value->vt = VT_UI8; value->uhVal.QuadPart = item.size; break;
        case kpidMTime: value->vt = VT_FILETIME; value->filetime = item.modified; break;
        case kpidAttrib: value->vt = VT_UI4; value->ulVal = item.attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_REPARSE_POINT); break;
        case kpidIsAnti: value->vt = VT_BOOL; value->boolVal = VARIANT_FALSE; break;
        default: break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialInStream** stream) noexcept override {
        if (!stream || index >= items_.size()) return E_INVALIDARG;
        *stream = nullptr;
        const auto& item = items_[index];
        progress_.current = item.name;
        if (item.dir) return S_OK;
        HANDLE file = CreateFileW(LongPath(item.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        try { *stream = new ItemIn(file, cancelled_); } catch (...) { CloseHandle(file); return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        ++progress_.completed_items;
        return result == kOpOk && !IsCancelled(cancelled_) ? S_OK : E_ABORT;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32* defined, BSTR* password) noexcept override {
        if (!defined || !password) return E_POINTER;
        *defined = password_.empty() ? 0 : 1;
        *password = SysAllocString(password_.c_str());
        return *password ? S_OK : E_OUTOFMEMORY;
    }
};

struct Prop {
    std::wstring name;
    PROPVARIANT value{};
};
Prop Text(const wchar_t* name, const wchar_t* text) {
    Prop prop{name};
    prop.value.vt = VT_BSTR;
    prop.value.bstrVal = SysAllocString(text);
    return prop;
}
Prop Uint(const wchar_t* name, UInt32 number) { Prop prop{name}; prop.value.vt = VT_UI4; prop.value.ulVal = number; return prop; }
Prop Flag(const wchar_t* name, bool on) { Prop prop{name}; prop.value.vt = VT_BOOL; prop.value.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE; return prop; }

// One archive of `handler_name` from `items` into `target` (or volumes of it).
Status Write(const std::wstring& handler_name, const std::vector<Item>& items, uint64_t total, std::vector<Prop> props,
             const std::wstring& password, const std::wstring& target, uint64_t volume, std::vector<std::wstring>& outputs,
             std::string& error, const Cancelled& cancelled, const Report& report) {
    const Handler* handler = FindHandler(Lower(handler_name));
    if (!handler || !handler->update) { error = "7-Zip cannot write this format"; return Status::Unsupported; }
    IOutArchive* raw = nullptr;
    if (CreateObject(&handler->cls, &kIID_OutArchive, reinterpret_cast<void**>(&raw)) != S_OK || !raw) {
        error = "cannot create the archive writer";
        return Status::Unsupported;
    }
    std::unique_ptr<IOutArchive, ComRelease<IOutArchive>> archive(raw);
    if (!props.empty()) {
        ISetProperties* set_raw = nullptr;
        if (archive->QueryInterface(kIID_SetProperties, reinterpret_cast<void**>(&set_raw)) == S_OK && set_raw) {
            std::unique_ptr<ISetProperties, ComRelease<ISetProperties>> set(set_raw);
            std::vector<const wchar_t*> names;
            std::vector<PROPVARIANT> values;
            for (auto& prop : props) { names.push_back(prop.name.c_str()); values.push_back(prop.value); }
            const HRESULT accepted = set->SetProperties(names.data(), values.data(), static_cast<UInt32>(names.size()));
            for (auto& prop : props) PropVariantClear(&prop.value);
            if (FAILED(accepted)) { error = "the format rejected these compression options"; return Status::InvalidInput; }
        }
    }
    std::unique_ptr<Update, ComRelease<Update>> callback(new Update(items, password, total, cancelled, report));
    HRESULT updated = E_FAIL;
    bool closed = false;
    if (volume) {
        std::unique_ptr<VolumesOut, ComRelease<VolumesOut>> out(new VolumesOut(target, volume, cancelled));
        updated = archive->UpdateItems(out.get(), static_cast<UInt32>(items.size()), callback.get());
        closed = out->Close();
        outputs.insert(outputs.end(), out->names.begin(), out->names.end());
    } else {
        HANDLE file = CreateFileW(LongPath(target).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { error = "cannot create the archive file"; return Status::IoError; }
        std::unique_ptr<FileOut, ComRelease<FileOut>> out(new FileOut(file, cancelled));
        updated = archive->UpdateItems(out.get(), static_cast<UInt32>(items.size()), callback.get());
        closed = out->Close();
        outputs.push_back(target);
    }
    if (IsCancelled(cancelled)) return Status::Cancelled;
    if (updated != S_OK || !closed) { error = "writing the archive failed"; return Status::IoError; }
    return Status::Ok;
}

// CRC-32 (IEEE).
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
std::string Hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}
struct Hasher {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ULONG length = 0;
    explicit Hasher(const wchar_t* id) {
        if (BCryptOpenAlgorithmProvider(&algorithm, id, nullptr, 0) != 0) { algorithm = nullptr; return; }
        ULONG got = 0;
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0);
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) hash = nullptr;
    }
    ~Hasher() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void Add(const uint8_t* data, ULONG size) { if (hash) BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0); }
    std::string Finish() {
        if (!hash || !length || length > 64) return {};
        uint8_t digest[64]{};
        return BCryptFinishHash(hash, digest, length, 0) == 0 ? Hex(digest, length) : std::string{};
    }
};
} // namespace

bool SafeRelativePath(const std::wstring& path) {
    if (path.empty() || path.size() > 32000 || path.front() == L'\\' || path.front() == L'/' ||
        (path.size() > 1 && path[1] == L':')) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find_first_of(L"\\/", start);
        auto part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (wchar_t c : part) if (c < 32 || std::wstring_view(L":*?\"<>|").find(c) != std::wstring_view::npos) return false;
        auto stem = part.substr(0, part.find(L'.'));
        for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) && iswdigit(stem[3]))) return false;
        if (end == std::wstring::npos) return true;
        start = end + 1;
    }
    return false;
}

std::vector<std::wstring> SupportedFormats() {
    std::vector<std::wstring> lines;
    for (const auto& handler : Handlers()) {
        std::wstring line = handler.name + L":";
        for (const auto& ext : handler.extensions) line += L" " + ext;
        if (handler.update) line += L" (+)";
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> ArchiveExtensions() {
    return {".7z", ".zip", ".rar", ".tar", ".gz", ".tgz", ".xz", ".txz", ".bz2", ".tbz2", ".tbz", ".lzma", ".z",
            ".iso", ".cab", ".wim", ".arj", ".lzh", ".lha", ".cpio", ".rpm", ".deb", ".001", ".zipx", ".jar",
            ".apk", ".msi", ".vhd", ".vhdx", ".dmg", ".udf", ".squashfs"};
}

Listing List(const std::wstring& path, const Cancelled& cancelled, const AskPassword& ask, size_t max_entries) {
    Listing result;
    try {
        PasswordState password;
        password.ask = &ask;
        Opened opened;
        result.status = OpenFile(path, cancelled, password, opened, 0);
        if (result.status != Status::Ok) { result.error = StatusText(result.status); return result; }
        result.format = opened.format;
        result.nested = opened.nested;
        UInt32 count = 0;
        opened.archive->GetNumberOfItems(&count);
        result.entries.reserve(std::min<size_t>(count, max_entries));
        for (UInt32 i = 0; i < count; ++i) {
            if (IsCancelled(cancelled)) { result.status = Status::Cancelled; return result; }
            if (result.entries.size() >= max_entries) { result.truncated = true; break; }
            Entry entry;
            if (ReadEntry(opened.archive.get(), i, entry)) result.entries.push_back(std::move(entry));
        }
        result.status = Status::Ok;
    } catch (const std::exception& error) { result.status = Status::InvalidArchive; result.error = error.what(); }
    return result;
}

ExtractResult Extract(const std::wstring& path, const std::wstring& destination, bool test_only,
                      const Cancelled& cancelled, const Report& report, const AskPassword& ask) {
    ExtractResult result;
    try {
        PasswordState password;
        password.ask = &ask;
        Opened opened;
        const auto status = OpenFile(path, cancelled, password, opened, 0);
        if (status != Status::Ok) { result.status = status; result.error = StatusText(status); return result; }
        UInt32 count = 0;
        opened.archive->GetNumberOfItems(&count);
        std::vector<Entry> entries(count);
        std::vector<bool> wanted(count, false);
        uint64_t total = 0, items = 0;
        for (UInt32 i = 0; i < count; ++i) {
            if (IsCancelled(cancelled)) { result.status = Status::Cancelled; return result; }
            if (!ReadEntry(opened.archive.get(), i, entries[i])) continue;
            auto& entry = entries[i];
            if (entry.path.empty()) entry.path = fs::path(path).stem().wstring();  // nameless single streams
            if (entry.link) { ++result.skipped_links; continue; }
            if (!SafeRelativePath(entry.path)) {
                result.status = Status::InvalidArchive;
                result.error = "The archive contains an unsafe path: " + Narrow(entry.path);
                return result;
            }
            wanted[i] = true;
            ++items;
            if (!entry.dir) total += entry.size;
        }
        if (!test_only) {
            std::error_code ec;
            if (!fs::is_directory(destination, ec) || !fs::is_empty(destination, ec)) {
                result.status = Status::InvalidInput;
                result.error = "Destination must be an existing empty folder";
                return result;
            }
        }
        std::vector<UInt32> indices;
        for (UInt32 i = 0; i < count; ++i) if (wanted[i]) indices.push_back(i);
        std::unique_ptr<Extraction, ComRelease<Extraction>> callback(
            new Extraction(destination, entries, wanted, test_only, cancelled, report, password, total, items));
        const HRESULT extracted = indices.empty() ? S_OK
            : opened.archive->Extract(indices.data(), static_cast<UInt32>(indices.size()), test_only ? 1 : 0, callback.get());
        callback->FinishDirs();
        result.files = callback->files;
        result.bytes = test_only ? total : callback->Written();
        if (password.refused) { result.status = Status::NeedPassword; result.error = StatusText(result.status); return result; }
        if (IsCancelled(cancelled) || extracted == E_ABORT) { result.status = Status::Cancelled; result.error = "Cancelled"; return result; }
        if (callback->wrong_password) { result.status = Status::WrongPassword; result.error = StatusText(result.status); return result; }
        if (extracted != S_OK || callback->failed || callback->worst != kOpOk) {
            result.status = Status::InvalidArchive;
            result.error = callback->worst == kOpUnsupportedMethod ? "The archive uses a compression method 7-Zip does not support"
                : callback->worst == kOpUnexpectedEnd ? "The archive ends unexpectedly (incomplete download or missing volume)"
                : callback->failed ? "A file could not be written" : "The archive is damaged (CRC or data error)";
            return result;
        }
        result.status = Status::Ok;
    } catch (const std::exception& error) { result.status = Status::IoError; result.error = error.what(); }
    return result;
}

std::wstring FormatExtension(Format format) {
    switch (format) {
    case Format::Zip: return L".zip";
    case Format::Tar: return L".tar";
    case Format::TarGz: return L".tar.gz";
    case Format::TarXz: return L".tar.xz";
    case Format::TarBz2: return L".tar.bz2";
    default: return L".7z";
    }
}

CreateResult Create(const std::vector<std::wstring>& sources, const std::wstring& destination, const std::wstring& name,
                    const CreateOptions& options, const Cancelled& cancelled, const Report& report) {
    CreateResult result;
    if (sources.empty() || name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || !SafeRelativePath(name) ||
        options.level > 9) {
        result.status = Status::InvalidInput;
        result.error = "Invalid archive name or options";
        return result;
    }
    try {
        std::vector<Item> items;
        std::set<std::wstring> names;
        uint64_t total = 0;
        for (const auto& source : sources) {
            const fs::path path(source);
            if (!path.is_absolute()) { result.status = Status::InvalidInput; result.error = "Sources must be absolute paths"; return result; }
            Collect(source, path.filename().wstring(), 0, items, names, total, result.skipped, cancelled);
        }
        if (items.empty()) { result.status = Status::InvalidInput; result.error = "Nothing to compress"; return result; }
        const fs::path target = fs::path(destination) / name;
        const bool tarred = options.format == Format::TarGz || options.format == Format::TarXz || options.format == Format::TarBz2;
        std::vector<Prop> props;
        std::wstring password;
        switch (options.format) {
        case Format::SevenZip:
            props.push_back(Uint(L"x", options.level));
            props.push_back(Flag(L"s", options.solid && options.level > 0));
            props.push_back(Flag(L"mt", true));
            if (!options.password.empty()) {
                password = options.password;
                props.push_back(Flag(L"he", options.encrypt_names));
            }
            break;
        case Format::Zip:
            props.push_back(Uint(L"x", options.level));
            props.push_back(Flag(L"mt", true));
            props.push_back(Flag(L"cu", true));  // UTF-8 names
            if (!options.password.empty()) {
                password = options.password;
                props.push_back(Text(L"em", options.zip_aes ? L"AES256" : L"ZipCrypto"));
            }
            break;
        default:
            break;
        }
        Status status;
        if (!tarred) {
            status = Write(options.format == Format::Zip ? L"zip" : options.format == Format::Tar ? L"tar" : L"7z", items, total,
                           std::move(props), password, target.wstring(), options.volume_size, result.outputs, result.error, cancelled, report);
        } else {
            // Two passes, like tar + gzip: the tar goes to a temporary file first.
            TempFolder temp;
            std::wstring tar_name = name;
            const auto dot = tar_name.find_last_of(L'.');
            tar_name = dot == std::wstring::npos ? tar_name + L".tar" : tar_name.substr(0, dot);
            if (Lower(fs::path(tar_name).extension().wstring()) != L".tar") tar_name += L".tar";
            const auto tar_path = (temp.Create(destination) / tar_name).wstring();
            SetFileAttributesW(LongPath(tar_path).c_str(), FILE_ATTRIBUTE_HIDDEN);
            std::vector<std::wstring> ignored;
            status = Write(L"tar", items, total, {}, {}, tar_path, 0, ignored, result.error, cancelled,
                           [&](const Progress& value) { Progress half = value; half.completed_bytes /= 2; if (report) report(half); });
            if (status == Status::Ok) {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                GetFileAttributesExW(LongPath(tar_path).c_str(), GetFileExInfoStandard, &data);
                Item tar{tar_path, tar_name, false, (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, data.ftLastWriteTime, FILE_ATTRIBUTE_NORMAL};
                std::vector<Item> single{tar};
                std::vector<Prop> level{Uint(L"x", options.level ? options.level : 1)};
                if (options.format != Format::TarBz2) level.push_back(Flag(L"mt", true));
                const wchar_t* handler = options.format == Format::TarGz ? L"gzip" : options.format == Format::TarXz ? L"xz" : L"bzip2";
                status = Write(handler, single, tar.size, std::move(level), {}, target.wstring(), options.volume_size, result.outputs,
                               result.error, cancelled, [&](const Progress& value) {
                                   Progress second = value;
                                   second.total_bytes = tar.size;
                                   second.completed_bytes = tar.size / 2 + value.completed_bytes / 2;
                                   if (report) report(second);
                               });
            }
        }
        result.status = status;
        if (status != Status::Ok) {
            for (const auto& output : result.outputs) DeleteFileW(LongPath(output).c_str());
            result.outputs.clear();
            if (result.error.empty()) result.error = StatusText(status);
            return result;
        }
        for (const auto& item : items) if (!item.dir) ++result.files;
        result.bytes = total;
    } catch (const std::length_error& error) { result.status = Status::LimitReached; result.error = error.what(); }
    catch (const std::exception& error) {
        result.status = IsCancelled(cancelled) ? Status::Cancelled : Status::IoError;
        result.error = error.what();
    }
    return result;
}

std::vector<HashResult> Hash(const std::vector<std::wstring>& files, const Cancelled& cancelled, const Report& report) {
    std::vector<HashResult> results;
    Progress progress;
    progress.total_items = files.size();
    for (const auto& file : files) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(LongPath(file).c_str(), GetFileExInfoStandard, &data) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            progress.total_bytes += (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    std::vector<uint8_t> buffer(1u << 20);
    for (const auto& file : files) {
        HashResult item;
        item.path = file;
        progress.current = fs::path(file).filename().wstring();
        HANDLE handle = CreateFileW(LongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { item.error = "cannot open"; results.push_back(std::move(item)); continue; }
        Hasher sha1(BCRYPT_SHA1_ALGORITHM), sha256(BCRYPT_SHA256_ALGORITHM);
        uint32_t crc = 0;
        for (;;) {
            if (IsCancelled(cancelled)) { CloseHandle(handle); return results; }
            DWORD read = 0;
            if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { item.error = "read failed"; break; }
            if (!read) break;
            crc = Crc32(crc, buffer.data(), read);
            sha1.Add(buffer.data(), read);
            sha256.Add(buffer.data(), read);
            item.size += read;
            progress.completed_bytes += read;
            if (report) report(progress);
        }
        CloseHandle(handle);
        if (item.error.empty()) {
            char text[9];
            sprintf_s(text, "%08x", crc);
            item.crc32 = text;
            item.sha1 = sha1.Finish();
            item.sha256 = sha256.Finish();
        }
        ++progress.completed_items;
        results.push_back(std::move(item));
    }
    return results;
}
} // namespace engine
