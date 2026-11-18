#include "archive_writer.h"
#define NOMINMAX
#include <windows.h>
#include <propidl.h>
#include <oleauto.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <cwctype>

extern "C" __declspec(dllimport) HRESULT WINAPI CreateObject(const GUID*, const GUID*, void**);
extern "C" __declspec(dllimport) HRESULT WINAPI GetHandlerProperty2(unsigned, PROPID, PROPVARIANT*);
extern "C" __declspec(dllimport) HRESULT WINAPI GetNumberOfFormats(unsigned*);

namespace archive_writer {
namespace {
using UInt32 = unsigned long;
using UInt64 = unsigned long long;
using Int32 = long;
constexpr GUID Guid(unsigned char group, unsigned char id) {
    return {0x23170F69, 0x40C1, 0x278A, {0, 0, 0, group, 0, id, 0, 0}};
}
constexpr GUID kSequentialIn = Guid(3, 1), kSequentialOut = Guid(3, 2);
constexpr GUID kOutStream = Guid(3, 4);
constexpr GUID kOutArchive = Guid(6, 0xA0), kUpdateCallback = Guid(6, 0x80);
constexpr GUID kSetProperties = Guid(6, 3), kProgress = Guid(0, 5);
constexpr PROPID kPath = 3, kIsDir = 6, kSize = 7, kMTime = 12;
constexpr PROPID kAttrib = 9;
constexpr PROPID kIsAnti = 21;
constexpr uint64_t kMaxBytes = 4ull * 1024 * 1024 * 1024;
constexpr size_t kMaxItems = 4096;

struct SequentialIn : IUnknown { virtual HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG*) noexcept = 0; };
struct SequentialOut : IUnknown { virtual HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) noexcept = 0; };
struct OutStream : SequentialOut {
    virtual HRESULT STDMETHODCALLTYPE Seek(int64_t, UInt32, UInt64*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSize(UInt64) noexcept = 0;
};
struct OutArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UpdateItems(SequentialOut*, UInt32, IUnknown*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileTimeType(UInt32*) noexcept = 0;
};
struct ProgressCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept = 0;
};
struct UpdateCallback : ProgressCallback {
    virtual HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32, Int32*, Int32*, UInt32*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32, PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32, SequentialIn**) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32) noexcept = 0;
};
struct SetPropertiesIface : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetProperties(const wchar_t* const*, const PROPVARIANT*, UInt32) noexcept = 0;
};
template<class T> struct Release { void operator()(T* p) const { if (p) p->Release(); } };
struct Variant { PROPVARIANT value{}; ~Variant() { PropVariantClear(&value); } };
struct Item {
    std::wstring path, name;
    bool directory = false;
    uint64_t size = 0;
    FILETIME modified{};
    DWORD attributes = 0;
};
bool IsCancelled(const std::function<bool()>& fn) noexcept {
    try { return fn && fn(); } catch (...) { return true; }
}
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("invalid Unicode archive path");
    std::string out(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr))
        throw std::runtime_error("cannot convert archive path");
    return out;
}
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}
std::wstring LongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.size() < 240) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
void Collect(const std::wstring& source, const std::wstring& archiveName, unsigned depth,
             std::vector<Item>& items, std::set<std::wstring>& names, uint64_t& total,
             const std::function<bool()>& cancel) {
    if (IsCancelled(cancel)) throw std::runtime_error("cancelled");
    if (depth > 64 || items.size() >= kMaxItems) throw std::length_error("archive tree exceeds item or depth limit");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(LongPath(source).c_str(), GetFileExInfoStandard, &data))
        throw std::runtime_error("cannot inspect source path");
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("reparse points are not supported");
    Item item{source, archiveName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow, data.ftLastWriteTime, data.dwFileAttributes};
    if (item.directory) item.size = 0;
    const auto folded = Lower(item.name);
    if (!names.insert(folded).second) throw std::runtime_error("duplicate archive member path");
    if (!item.directory) {
        if (item.size > kMaxBytes - total) throw std::length_error("archive input exceeds 4 GiB limit");
        total += item.size;
        HANDLE h = CreateFileW(LongPath(source).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot inspect source file");
        BY_HANDLE_FILE_INFORMATION info{};
        const bool valid = GetFileInformationByHandle(h, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && info.nNumberOfLinks == 1;
        CloseHandle(h);
        if (!valid) throw std::runtime_error("hard links and reparse files are not supported");
    }
    items.push_back(item);
    if (!item.directory) return;
    std::vector<std::filesystem::path> children;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(LongPath(source + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
        throw std::runtime_error("cannot enumerate source directory");
    }
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) children.emplace_back(source + L"\\" + fd.cFileName);
    } while (FindNextFileW(find, &fd));
    const DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) throw std::runtime_error("source directory enumeration failed");
    std::sort(children.begin(), children.end());
    for (const auto& child : children)
        Collect(child.wstring(), archiveName + L"/" + child.filename().wstring(), depth + 1, items, names, total, cancel);
}
class Input final : public SequentialIn {
    std::atomic<ULONG> refs_{1};
    HANDLE file_;
    const std::function<bool()>& cancel_;
public:
    Input(const std::wstring& path, const std::function<bool()>& cancel) : cancel_(cancel) {
        file_ = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open source file");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file_, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            info.nNumberOfLinks != 1) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; throw std::runtime_error("source changed or became unsupported"); }
    }
    ~Input() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != kSequentialIn) return E_NOINTERFACE;
        *out = static_cast<SequentialIn*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) noexcept override {
        if (read) *read = 0;
        if (IsCancelled(cancel_)) return E_ABORT;
        DWORD done = 0;
        if (!ReadFile(file_, data, std::min<ULONG>(size, 1024 * 1024), &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read) *read = done;
        return IsCancelled(cancel_) ? E_ABORT : S_OK;
    }
};
class Output final : public OutStream {
    std::atomic<ULONG> refs_{1};
    HANDLE file_;
    const std::function<bool()>& cancel_;
public:
    Output(HANDLE file, const std::function<bool()>& cancel) : file_(file), cancel_(cancel) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != kSequentialOut && id != kOutStream) return E_NOINTERFACE;
        *out = static_cast<OutStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Seek(int64_t offset, UInt32 origin, UInt64* position) noexcept override {
        if (IsCancelled(cancel_)) return E_ABORT;
        if (origin > FILE_END) return STG_E_INVALIDFUNCTION;
        LARGE_INTEGER move{}, result{}; move.QuadPart = offset;
        if (!SetFilePointerEx(file_, move, &result, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (position) *position = static_cast<UInt64>(result.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 size) noexcept override {
        if (IsCancelled(cancel_)) return E_ABORT;
        if (size > INT64_MAX) return E_INVALIDARG;
        FILE_END_OF_FILE_INFO info{}; info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
        return SetFileInformationByHandle(file_, FileEndOfFileInfo, &info, sizeof(info)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* written) noexcept override {
        if (written) *written = 0;
        if (IsCancelled(cancel_)) return E_ABORT;
        DWORD done = 0;
        if (!WriteFile(file_, data, std::min<ULONG>(size, 1024 * 1024), &done, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (written) *written = done;
        return done == std::min<ULONG>(size, 1024 * 1024) ? S_OK : STG_E_WRITEFAULT;
    }
};
class Callback final : public UpdateCallback {
    std::atomic<ULONG> refs_{1};
    const std::vector<Item>& items_;
    const std::function<bool()>& cancel_;
    const std::function<void(const Progress&)>& notify_;
    uint64_t total_, completed_ = 0;
    uint32_t done_items_ = 0;
    std::wstring current_;
public:
    Callback(const std::vector<Item>& items, uint64_t total, const std::function<bool()>& cancel,
             const std::function<void(const Progress&)>& notify) : items_(items), cancel_(cancel), notify_(notify), total_(total) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != kUpdateCallback && id != kProgress) return E_NOINTERFACE;
        *out = static_cast<UpdateCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return IsCancelled(cancel_) ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* value) noexcept override {
        if (IsCancelled(cancel_)) return E_ABORT;
        if (value) completed_ = *value;
        try { if (notify_) notify_({ToUtf8(current_), completed_, total_, done_items_, static_cast<uint64_t>(items_.size())}); }
        catch (...) { return E_ABORT; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 index, Int32* data, Int32* props, UInt32* archiveIndex) noexcept override {
        if (!data || !props || !archiveIndex || index >= items_.size()) return E_INVALIDARG;
        *data = *props = 1; *archiveIndex = UINT32_MAX; current_ = items_[index].name; return IsCancelled(cancel_) ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID prop, PROPVARIANT* value) noexcept override {
        if (!value || index >= items_.size()) return E_INVALIDARG;
        PropVariantInit(value);
        const auto& item = items_[index];
        if (prop == kPath) { value->vt = VT_BSTR; value->bstrVal = SysAllocString(item.name.c_str()); if (!value->bstrVal) return E_OUTOFMEMORY; }
        else if (prop == kIsDir) { value->vt = VT_BOOL; value->boolVal = item.directory ? VARIANT_TRUE : VARIANT_FALSE; }
        else if (prop == kSize) { value->vt = VT_UI8; value->uhVal.QuadPart = item.size; }
        else if (prop == kMTime) { value->vt = VT_FILETIME; value->filetime = item.modified; }
        else if (prop == kAttrib) { value->vt = VT_UI4; value->ulVal = item.attributes; }
        else if (prop == kIsAnti) { value->vt = VT_BOOL; value->boolVal = VARIANT_FALSE; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, SequentialIn** stream) noexcept override {
        if (!stream || index >= items_.size()) return E_INVALIDARG;
        *stream = nullptr;
        if (items_[index].directory) return S_OK;
        try { *stream = new Input(items_[index].path, cancel_); current_ = items_[index].name; return S_OK; }
        catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        if (result != 0 || IsCancelled(cancel_)) return E_ABORT;
        ++done_items_;
        try { if (notify_) notify_({ToUtf8(current_), completed_, total_, done_items_, static_cast<uint64_t>(items_.size())}); }
        catch (...) { return E_ABORT; }
        return S_OK;
    }
};
Result Fail(Status status, const char* text) { Result r; r.status=status; r.error=text; return r; }
}

Result Create(const std::vector<std::wstring>& sources, const std::wstring& staging, const std::wstring& basename,
    Format format, unsigned level, const std::function<bool()>& cancelled, const std::function<void(const Progress&)>& notify) {
    if (sources.empty() || sources.size() > 256 || staging.empty() || basename.empty() ||
        basename == L"." || basename == L".." || basename.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return Fail(Status::InvalidInput, "invalid archive arguments");
    if (level > 9) return Fail(Status::InvalidInput, "compression level must be 0..9");
    if (IsCancelled(cancelled)) return Fail(Status::Cancelled, "cancelled");
    try {
        const std::filesystem::path stage(staging);
        if (!stage.is_absolute() || !std::filesystem::is_directory(stage) || std::filesystem::directory_iterator(stage) != std::filesystem::directory_iterator())
            return Fail(Status::InvalidInput, "staging directory must exist and be empty");
        std::vector<Item> items; items.reserve(128); std::set<std::wstring> names; uint64_t total=0;
        for (const auto& source : sources) {
            std::filesystem::path path(source);
            const bool extended_local = source.size() >= 7 && source.rfind(L"\\\\?\\", 0) == 0 &&
                ((source[4] >= L'A' && source[4] <= L'Z') || (source[4] >= L'a' && source[4] <= L'z')) &&
                source[5] == L':' && source[6] == L'\\';
            if (!path.is_absolute() || (source.rfind(L"\\",0)==0 && !extended_local))
                return Fail(Status::Unsupported, "only local absolute source paths are supported");
            Collect(source, path.filename().wstring(), 0, items, names, total, cancelled);
        }
        if (items.empty()) return Fail(Status::InvalidInput, "no archive entries");
        unsigned formatCount=0;
        if (GetNumberOfFormats(&formatCount)!=S_OK || formatCount>256) return Fail(Status::Unsupported,"7-Zip handler discovery failed");
        GUID classId{}; bool found=false;
        const wchar_t* wanted = format == Format::SevenZip ? L"7z" : L"zip";
        for (unsigned i=0;i<formatCount;++i) {
            Variant name, cls;
            if (GetHandlerProperty2(i,0,&name.value)!=S_OK || name.value.vt!=VT_BSTR) continue;
            if (_wcsicmp(name.value.bstrVal,wanted)!=0) continue;
            if (GetHandlerProperty2(i,1,&cls.value)!=S_OK || cls.value.vt!=VT_BSTR || SysStringByteLen(cls.value.bstrVal)!=sizeof(GUID))
                return Fail(Status::Unsupported,"invalid 7-Zip writer handler");
            std::memcpy(&classId,cls.value.bstrVal,sizeof(classId)); found=true; break;
        }
        if (!found) return Fail(Status::Unsupported,"requested archive format is unavailable");
        OutArchive* raw=nullptr;
        if (CreateObject(&classId,&kOutArchive,reinterpret_cast<void**>(&raw))!=S_OK || !raw)
            return Fail(Status::Unsupported,"cannot create archive writer");
        std::unique_ptr<OutArchive,Release<OutArchive>> archive(raw);
        SetPropertiesIface* propertyRaw=nullptr;
        if (archive->QueryInterface(kSetProperties,reinterpret_cast<void**>(&propertyRaw))==S_OK && propertyRaw) {
            std::unique_ptr<SetPropertiesIface,Release<SetPropertiesIface>> properties(propertyRaw);
            PROPVARIANT value{}; value.vt=VT_UI4; value.ulVal=level;
            const wchar_t* key=L"x";
            const HRESULT propertyResult = properties->SetProperties(&key,&value,1);
            if (FAILED(propertyResult))
                return Fail(Status::Unsupported,"archive compression level is not accepted by the format handler");
        }
        const auto outputPath=(stage/basename).wstring();
        HANDLE file=CreateFileW(LongPath(outputPath).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return Fail(Status::IoError,"cannot create archive output");
        bool success=false;
        {
            std::unique_ptr<Output,Release<Output>> output(new Output(file,cancelled));
            std::unique_ptr<Callback,Release<Callback>> callback(new Callback(items,total,cancelled,notify));
            const HRESULT hr=archive->UpdateItems(output.get(),static_cast<UInt32>(items.size()),callback.get());
            success=hr==S_OK&&!IsCancelled(cancelled);
            success = FlushFileBuffers(file) != FALSE && success;
        }
        CloseHandle(file);
        if (!success) { DeleteFileW(LongPath(outputPath).c_str()); return Fail(IsCancelled(cancelled)?Status::Cancelled:Status::IoError,"archive creation failed or was cancelled"); }
        Result result; result.status=Status::Ok; result.path=outputPath; result.bytes=total; result.items=items.size(); return result;
    } catch (const std::length_error& e) { return Fail(Status::LimitReached,e.what()); }
      catch (const std::exception& e) { return Fail(IsCancelled(cancelled)?Status::Cancelled:Status::IoError,e.what()); }
      catch (...) { return Fail(Status::IoError,"archive creation failed"); }
}
}
