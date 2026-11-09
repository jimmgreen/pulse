#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>

namespace pulse::ops {
inline constexpr wchar_t kPhotosAppId[] = L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App";
using PhotosAssociationQuery = HRESULT (WINAPI*)(ASSOCF, ASSOCSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD*);
using PhotosShellExecute = BOOL (WINAPI*)(SHELLEXECUTEINFOW*);
struct PhotosProtocolResult { bool attempted = false, launched = false; HRESULT error = S_FALSE; };
HRESULT QueryPhotosAssociation(const std::wstring& association, bool protocol, std::wstring& app_id,
    PhotosAssociationQuery query = AssocQueryStringW) noexcept;
// Pure path validation/encoding; does not read the image or enumerate its directory.
HRESULT BuildPhotosViewerUri(const std::wstring& path, std::wstring& uri) noexcept;
// Background worker only. Null owner is permitted; non-null owners must be valid.
PhotosProtocolResult LaunchPhotosProtocolIfDefault(const std::wstring& path, HWND owner,
    PhotosAssociationQuery query = AssocQueryStringW, PhotosShellExecute execute = ShellExecuteExW) noexcept;
}
