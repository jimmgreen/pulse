// sevenzip_com.h - the public 7-Zip COM ABI used by this extension, from
// ip7z/7zip CPP/7zip/{IDecl.h,IStream.h,IPassword.h,Archive/IArchive.h,PropID.h}.
// Only the documented DLL exports CreateObject / GetHandlerProperty2 /
// GetNumberOfFormats are used.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <propidl.h>
#include <oleauto.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace sevenzip {
namespace dyn {
struct Api {
    HMODULE sevenzip = nullptr;
    HMODULE oleaut = nullptr;
    HMODULE ole = nullptr;
    DWORD load_error = 0;
    HRESULT(WINAPI* create_object)(const GUID*, const GUID*, void**) = nullptr;
    HRESULT(WINAPI* get_handler_property2)(unsigned, PROPID, PROPVARIANT*) = nullptr;
    HRESULT(WINAPI* get_number_of_formats)(unsigned*) = nullptr;
    BSTR(WINAPI* sys_alloc_string)(const OLECHAR*) = nullptr;
    void(WINAPI* sys_free_string)(BSTR) = nullptr;
    UINT(WINAPI* sys_string_len)(BSTR) = nullptr;
    UINT(WINAPI* sys_string_byte_len)(BSTR) = nullptr;
    HRESULT(WINAPI* prop_variant_clear)(PROPVARIANT*) = nullptr;
};

inline void AnchorSymbol() noexcept {}

inline const Api& Get() noexcept {
    static std::once_flag once;
    static Api api{};
    std::call_once(once, []() noexcept {
        api.oleaut = LoadLibraryExW(L"oleaut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (api.oleaut) {
            api.sys_alloc_string = reinterpret_cast<decltype(api.sys_alloc_string)>(GetProcAddress(api.oleaut, "SysAllocString"));
            api.sys_free_string = reinterpret_cast<decltype(api.sys_free_string)>(GetProcAddress(api.oleaut, "SysFreeString"));
            api.sys_string_len = reinterpret_cast<decltype(api.sys_string_len)>(GetProcAddress(api.oleaut, "SysStringLen"));
            api.sys_string_byte_len = reinterpret_cast<decltype(api.sys_string_byte_len)>(GetProcAddress(api.oleaut, "SysStringByteLen"));
        }
        api.ole = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (api.ole) {
            api.prop_variant_clear = reinterpret_cast<decltype(api.prop_variant_clear)>(GetProcAddress(api.ole, "PropVariantClear"));
        }
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&AnchorSymbol), &self) || !self) {
            api.load_error = GetLastError();
            return;
        }
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD len = 0;
        for (;;) {
            len = GetModuleFileNameW(self, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (len == 0) {
                api.load_error = GetLastError();
                return;
            }
            if (len < buffer.size()) {
                buffer.resize(len);
                break;
            }
            if (buffer.size() >= 32768) {
                api.load_error = ERROR_FILENAME_EXCED_RANGE;
                return;
            }
            buffer.resize(buffer.size() * 2);
        }
        const auto dll_path = std::filesystem::path(buffer).parent_path() / L"7z.dll";
        api.sevenzip = LoadLibraryExW(dll_path.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!api.sevenzip) {
            api.load_error = GetLastError();
            return;
        }
        api.create_object = reinterpret_cast<decltype(api.create_object)>(GetProcAddress(api.sevenzip, "CreateObject"));
        api.get_handler_property2 = reinterpret_cast<decltype(api.get_handler_property2)>(GetProcAddress(api.sevenzip, "GetHandlerProperty2"));
        api.get_number_of_formats = reinterpret_cast<decltype(api.get_number_of_formats)>(GetProcAddress(api.sevenzip, "GetNumberOfFormats"));
        if (!api.create_object || !api.get_handler_property2 || !api.get_number_of_formats) {
            api.load_error = ERROR_PROC_NOT_FOUND;
        }
    });
    return api;
}

inline bool IsLoaded() noexcept {
    const auto& api = Get();
    return api.sevenzip && api.create_object && api.get_handler_property2 && api.get_number_of_formats;
}
inline DWORD LoadError() noexcept {
    return Get().load_error;
}
inline HRESULT CreateObject(const GUID* cls, const GUID* iid, void** out) noexcept {
    const auto& api = Get();
    return api.create_object ? api.create_object(cls, iid, out) : E_NOTIMPL;
}
inline HRESULT GetHandlerProperty2(unsigned index, PROPID prop, PROPVARIANT* value) noexcept {
    const auto& api = Get();
    return api.get_handler_property2 ? api.get_handler_property2(index, prop, value) : E_NOTIMPL;
}
inline HRESULT GetNumberOfFormats(unsigned* count) noexcept {
    const auto& api = Get();
    return api.get_number_of_formats ? api.get_number_of_formats(count) : E_NOTIMPL;
}
inline BSTR AllocBstr(const wchar_t* text) noexcept {
    const auto& api = Get();
    return api.sys_alloc_string ? api.sys_alloc_string(text ? text : L"") : nullptr;
}
inline UINT BstrLen(BSTR value) noexcept {
    const auto& api = Get();
    return value && api.sys_string_len ? api.sys_string_len(value) : 0;
}
inline UINT BstrByteLen(BSTR value) noexcept {
    const auto& api = Get();
    return value && api.sys_string_byte_len ? api.sys_string_byte_len(value) : 0;
}
inline HRESULT ClearPropVariant(PROPVARIANT* value) noexcept {
    if (!value) return E_POINTER;
    const auto& api = Get();
    if (api.prop_variant_clear) return api.prop_variant_clear(value);
    if (value->vt == VT_BSTR && value->bstrVal && api.sys_free_string) {
        api.sys_free_string(value->bstrVal);
    }
    PropVariantInit(value);
    return S_OK;
}
} // namespace dyn
using UInt32 = unsigned int;
using UInt64 = unsigned long long;
using Int32 = int;
using Int64 = long long;

constexpr GUID Guid(unsigned char group, unsigned char id) {
    return {0x23170F69, 0x40C1, 0x278A, {0, 0, 0, group, 0, id, 0, 0}};
}
inline constexpr GUID kIID_SequentialIn = Guid(3, 1), kIID_SequentialOut = Guid(3, 2);
inline constexpr GUID kIID_InStream = Guid(3, 3), kIID_OutStream = Guid(3, 4);
inline constexpr GUID kIID_Progress = Guid(0, 5);
inline constexpr GUID kIID_GetTextPassword = Guid(5, 0x10), kIID_GetTextPassword2 = Guid(5, 0x11);
inline constexpr GUID kIID_SetProperties = Guid(6, 0x03);
inline constexpr GUID kIID_OpenCallback = Guid(6, 0x10), kIID_ExtractCallback = Guid(6, 0x20);
inline constexpr GUID kIID_OpenVolumeCallback = Guid(6, 0x30), kIID_InArchiveGetStream = Guid(6, 0x40);
inline constexpr GUID kIID_InArchive = Guid(6, 0x60), kIID_UpdateCallback = Guid(6, 0x80);
inline constexpr GUID kIID_OutArchive = Guid(6, 0xA0);

enum : PROPID {
    kpidPath = 3, kpidName = 4, kpidIsDir = 6, kpidSize = 7, kpidPackSize = 8, kpidAttrib = 9,
    kpidCTime = 10, kpidATime = 11, kpidMTime = 12, kpidEncrypted = 15, kpidIsAnti = 21,
    kpidErrorFlags = 71, kpidPosixAttrib = 53, kpidSymLink = 54, kpidHardLink = 89, kpidCopyLink = 90,
};
enum : PROPID { kHandlerName = 0, kHandlerClassID = 1, kHandlerExtension = 2, kHandlerUpdate = 4 };
enum : Int32 { kAskExtract = 0, kAskTest = 1, kAskSkip = 2 };
enum : Int32 {
    kOpOk = 0, kOpUnsupportedMethod = 1, kOpDataError = 2, kOpCrcError = 3, kOpUnavailable = 4,
    kOpUnexpectedEnd = 5, kOpDataAfterEnd = 6, kOpIsNotArc = 7, kOpHeadersError = 8, kOpWrongPassword = 9,
};

struct ISequentialInStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Read(void*, UInt32, UInt32*) noexcept = 0; };
struct ISequentialOutStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Write(const void*, UInt32, UInt32*) noexcept = 0; };
struct IInStream : ISequentialInStream { virtual HRESULT STDMETHODCALLTYPE Seek(Int64, UInt32, UInt64*) noexcept = 0; };
struct IOutStream : ISequentialOutStream {
    virtual HRESULT STDMETHODCALLTYPE Seek(Int64, UInt32, UInt64*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSize(UInt64) noexcept = 0;
};
struct IProgress : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept = 0;
};
struct ICryptoGetTextPassword : IUnknown { virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR*) noexcept = 0; };
struct ICryptoGetTextPassword2 : IUnknown { virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32*, BSTR*) noexcept = 0; };
struct IArchiveOpenCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) noexcept = 0;
};
struct IArchiveOpenVolumeCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetProperty(PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(const wchar_t*, IInStream**) noexcept = 0;
};
struct IArchiveExtractCallback : IProgress {
    virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32, ISequentialOutStream**, Int32) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32) noexcept = 0;
};
struct IInArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(IInStream*, const UInt64*, IArchiveOpenCallback*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32, PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const UInt32*, UInt32, Int32, IArchiveExtractCallback*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32, BSTR*, PROPID*, VARTYPE*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32, BSTR*, PROPID*, VARTYPE*) noexcept = 0;
};
struct IInArchiveGetStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32, ISequentialInStream**) noexcept = 0; };
struct IArchiveUpdateCallback : IProgress {
    virtual HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32, Int32*, Int32*, UInt32*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32, PROPID, PROPVARIANT*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32, ISequentialInStream**) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32) noexcept = 0;
};
struct IOutArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UpdateItems(ISequentialOutStream*, UInt32, IArchiveUpdateCallback*) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileTimeType(UInt32*) noexcept = 0;
};
struct ISetProperties : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetProperties(const wchar_t* const*, const PROPVARIANT*, UInt32) noexcept = 0;
};

struct Variant {
    PROPVARIANT value{};
    Variant() { PropVariantInit(&value); }
    ~Variant() { dyn::ClearPropVariant(&value); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
};
template<class T> struct ComRelease { void operator()(T* value) const { if (value) value->Release(); } };

// Reference counting shared by the extension's COM objects.
template<class Derived> class RefCounted {
    std::atomic<ULONG> refs_{1};
protected:
    virtual ~RefCounted() = default;
public:
    ULONG DoAddRef() noexcept { return ++refs_; }
    ULONG DoRelease() noexcept { const auto n = --refs_; if (!n) delete this; return n; }
};
} // namespace sevenzip
