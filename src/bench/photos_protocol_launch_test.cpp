#include "../ops/photos_protocol_launch.h"
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <vector>

namespace {
using namespace pulse::ops;
int failures = 0, launches = 0;
std::wstring file_app = kPhotosAppId, protocol_app = kPhotosAppId, expected_uri;
HRESULT association_error = S_OK;
DWORD launch_error = ERROR_SUCCESS;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
HRESULT WINAPI Association(ASSOCF flags, ASSOCSTR kind, LPCWSTR association, LPCWSTR extra, LPWSTR out, DWORD* size) {
    Check(kind == ASSOCSTR_APPID && !extra, "query application identity without hardcoded ProgID or executable");
    if (FAILED(association_error)) return association_error;
    const bool protocol = (flags & ASSOCF_IS_PROTOCOL) != 0;
    Check(protocol ? wcscmp(association, L"ms-photos") == 0 : wcscmp(association, L".png") == 0, "query extension and protocol separately");
    const auto& value = protocol ? protocol_app : file_app;
    const DWORD required = static_cast<DWORD>(value.size() + 1);
    if (!out || *size < required) { *size = required; return S_FALSE; }
    wcscpy_s(out, *size, value.c_str()); *size = required; return S_OK;
}
BOOL WINAPI Execute(SHELLEXECUTEINFOW* info) {
    ++launches;
    Check(info && !info->lpVerb && !info->lpParameters && info->lpFile == expected_uri &&
        (info->fMask & (SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI)) == (SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI),
        "launch exact encoded URI once with default protocol verb and no implicit dialog");
    SetLastError(launch_error);
    return launch_error == ERROR_SUCCESS;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--probe-association") == 0) {
        const wchar_t* extension = argc == 3 ? argv[2] : L".png";
        std::wstring file, protocol;
        const HRESULT first = QueryPhotosAssociation(extension, false, file);
        const HRESULT second = QueryPhotosAssociation(L"ms-photos", true, protocol);
        std::wprintf(L"file_appid=%ls hr=%08lX\nprotocol_appid=%ls hr=%08lX\n", file.c_str(), static_cast<unsigned long>(first), protocol.c_str(), static_cast<unsigned long>(second));
        return FAILED(first) || FAILED(second) ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--launch") == 0) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 2;
        HWND owner = CreateWindowExW(0, L"STATIC", L"Photos protocol probe", WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        const auto result = LaunchPhotosProtocolIfDefault(argv[2], owner);
        std::printf("attempted=%u launched=%u hr=%08lX\n", result.attempted, result.launched, static_cast<unsigned long>(result.error));
        if (owner) DestroyWindow(owner);
        CoUninitialize();
        return result.launched ? 0 : 1;
    }
    if (argc != 1) return 2;
    std::wstring uri;
    Check(BuildPhotosViewerUri(L"C:\\图\\😀 #&%?+.png", uri) == S_OK &&
        uri == L"ms-photos:viewer?fileName=C%3A%5C%E5%9B%BE%5C%F0%9F%98%80%20%23%26%25%3F%2B.png",
        "UTF8 percent encoding preserves Chinese emoji spaces and URI delimiters");
    Check(BuildPhotosViewerUri(L"\\\\server\\share\\a.png", uri) == S_OK &&
        uri == L"ms-photos:viewer?fileName=%5C%5Cserver%5Cshare%5Ca.png", "UNC absolute path encoding");
    Check(BuildPhotosViewerUri(L"\\\\?\\C:\\a.png", uri) == S_OK && uri == L"ms-photos:viewer?fileName=C%3A%5Ca.png", "safe extended local normalization");
    Check(BuildPhotosViewerUri(L"\\\\?\\UNC\\server\\share\\a.png", uri) == S_OK, "safe extended UNC normalization");
    for (const auto* invalid : {L"a.png", L"C:a.png", L"\\a.png", L"\\\\.\\a.png", L"\\\\?\\GLOBALROOT\\a.png",
             L"C:\\a.png:stream", L"C:\\NUL.png", L"C:\\bad.\\a.png", L"C:\\..\\a.png", L"C:\\a.txt", L"\\\\server\\a.png"})
        Check(FAILED(BuildPhotosViewerUri(invalid, uri)) && uri.empty(), "unsafe or unsupported path cannot become protocol launch");
    std::wstring invalid_utf16 = L"C:\\"; invalid_utf16.push_back(0xd800); invalid_utf16 += L".png";
    Check(FAILED(BuildPhotosViewerUri(invalid_utf16, uri)), "unpaired UTF16 surrogate rejected");
    namespace fs = std::filesystem;
    const auto root = fs::absolute(L"bench_data") / (L"photos-protocol-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    const auto path = (root / L"test.png").wstring();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { RemoveDirectoryW(root.c_str()); return 2; }
    CloseHandle(file);
    BuildPhotosViewerUri(path, expected_uri);
    file_app = L"Other.App";
    auto result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(!result.attempted && launches == 0, "other default image application never launches Photos protocol");
    file_app = kPhotosAppId; protocol_app = L"Other.Protocol";
    result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(!result.attempted && launches == 0, "different protocol handler never launches");
    protocol_app = kPhotosAppId; association_error = E_ACCESSDENIED;
    result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(!result.attempted && result.error == E_ACCESSDENIED && launches == 0, "association failure preserved without launching");
    association_error = S_OK;
    result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(result.attempted && result.launched && result.error == S_OK && launches == 1, "matching defaults launch once");
    launch_error = ERROR_ACCESS_DENIED;
    result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(result.attempted && !result.launched && result.error == E_ACCESSDENIED && launches == 2, "failed launch is not retried by helper");
    launch_error = ERROR_CANCELLED;
    result = LaunchPhotosProtocolIfDefault(path, nullptr, Association, Execute);
    Check(result.attempted && !result.launched && result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && launches == 3, "cancellation preserved exactly without relaunch");
    Check(DeleteFileW(path.c_str()) && RemoveDirectoryW(root.c_str()), "remove owned metadata fixture");
    return failures ? 1 : 0;
}
