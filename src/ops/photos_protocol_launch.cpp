#include "photos_protocol_launch.h"
#include "../common/preview_extensions.h"
#include <algorithm>
#include <vector>

namespace pulse::ops {
namespace {
HRESULT Normalize(const std::wstring& input, std::wstring& path, std::wstring& extension) {
    if (input.empty() || input.size() > 32767 || input.find(L'\0') != input.npos) return E_INVALIDARG;
    path = input;
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    size_t begin = 0;
    if (path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\') begin = 3;
    else if (path.rfind(L"\\\\", 0) == 0 && path.size() > 2 && path[2] != L'.' && path[2] != L'?') {
        const size_t server = path.find(L'\\', 2);
        const size_t share = server == path.npos ? path.npos : path.find(L'\\', server + 1);
        if (server == path.npos || server == 2 || share == path.npos || share == server + 1 || share + 1 >= path.size()) return E_INVALIDARG;
        begin = 2;
    } else return E_INVALIDARG;
    for (size_t i = begin; i < path.size(); ++i)
        if (path[i] < 32 || path[i] == L':' || path[i] == L'"' || path[i] == L'<' || path[i] == L'>' || path[i] == L'|' || path[i] == L'*') return E_INVALIDARG;
    // Stripping an extended prefix must not change Win32 path interpretation.
    for (size_t at = begin; at < path.size();) {
        const size_t end = path.find(L'\\', at);
        const auto part = path.substr(at, end == path.npos ? path.size() - at : end - at);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return E_INVALIDARG;
        auto stem = part.substr(0, part.find(L'.'));
        CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") && stem[3] >= L'1' && stem[3] <= L'9')) return E_INVALIDARG;
        if (end == path.npos) break;
        at = end + 1;
        if (at == path.size()) return E_INVALIDARG;
    }
    const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L'\\');
    if (dot == path.npos || dot < slash) return E_INVALIDARG;
    extension = path.substr(dot);
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
    return preview::IsImageExtension(extension) ? S_OK : E_INVALIDARG;
}
}
HRESULT QueryPhotosAssociation(const std::wstring& association, bool protocol, std::wstring& app_id,
    PhotosAssociationQuery query) noexcept {
    app_id.clear();
    if (!query) return E_POINTER;
    try {
        const ASSOCF flags = protocol ? ASSOCF_IS_PROTOCOL : ASSOCF_NONE;
        DWORD count = 0;
        HRESULT hr = query(flags, ASSOCSTR_APPID, association.c_str(), nullptr, nullptr, &count);
        if (hr != S_FALSE && hr != S_OK && hr != E_POINTER && hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) return hr;
        if (count < 2 || count > 32768) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::vector<wchar_t> value(count, L'\0');
        hr = query(flags, ASSOCSTR_APPID, association.c_str(), nullptr, value.data(), &count);
        if (hr != S_OK) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (count < 2 || count > value.size() || value[count - 1] != L'\0') return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        app_id.assign(value.data(), count - 1);
        if (app_id.find(L'\0') != app_id.npos) { app_id.clear(); return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT BuildPhotosViewerUri(const std::wstring& input, std::wstring& uri) noexcept {
    uri.clear();
    try {
        std::wstring path, extension;
        HRESULT hr = Normalize(input, path, extension);
        if (FAILED(hr)) return hr;
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
        if (!size) return HRESULT_FROM_WIN32(GetLastError());
        std::string utf8(size, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), utf8.data(), size, nullptr, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        uri = L"ms-photos:viewer?fileName=";
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (const unsigned char byte : utf8) {
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') uri.push_back(byte);
            else { uri.push_back(L'%'); uri.push_back(hex[byte >> 4]); uri.push_back(hex[byte & 15]); }
        }
        return S_OK;
    } catch (...) { uri.clear(); return E_OUTOFMEMORY; }
}
PhotosProtocolResult LaunchPhotosProtocolIfDefault(const std::wstring& input, HWND owner,
    PhotosAssociationQuery query, PhotosShellExecute execute) noexcept {
    PhotosProtocolResult result;
    try {
        if (!execute || !query || (owner && !IsWindow(owner))) { result.error = E_INVALIDARG; return result; }
        std::wstring path, extension, uri;
        result.error = Normalize(input, path, extension);
        if (FAILED(result.error)) return result;
        result.error = BuildPhotosViewerUri(path, uri);
        if (FAILED(result.error)) return result;
        std::wstring file_app, protocol_app;
        result.error = QueryPhotosAssociation(extension, false, file_app, query);
        if (FAILED(result.error)) return result;
        if (file_app != kPhotosAppId) { result.error = S_FALSE; return result; }
        result.error = QueryPhotosAssociation(L"ms-photos", true, protocol_app, query);
        if (FAILED(result.error)) return result;
        if (protocol_app != kPhotosAppId) { result.error = S_FALSE; return result; }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) { result.error = E_INVALIDARG; return result; }
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner; info.lpFile = uri.c_str(); info.nShow = SW_SHOWNORMAL;
        result.attempted = true;
        SetLastError(ERROR_SUCCESS);
        result.launched = execute(&info) != FALSE;
        const DWORD error = GetLastError();
        result.error = result.launched ? S_OK : HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    } catch (...) { result.error = E_OUTOFMEMORY; }
    return result;
}
}
