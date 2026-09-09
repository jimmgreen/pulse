#include "update_transport.h"
#include "pulse_version.h"
#include <winhttp.h>
#include <array>

namespace pulse::app {
namespace {
struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
}

bool ReadUpdateResponse(std::wstring_view url, uint64_t maximum_bytes,
                        const std::atomic<bool>& cancelled,
                        const std::function<bool(const void*, DWORD)>& consume,
                        UpdateError& category, DWORD& error) {
    category = UpdateError::Network;
    error = ERROR_SUCCESS;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength ||
        parts.dwUserNameLength || parts.dwPasswordLength) {
        category = UpdateError::InsecureUrl;
        error = ERROR_WINHTTP_INVALID_URL;
        return false;
    }
    if (cancelled) { error = ERROR_CANCELLED; return false; }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    HttpHandle session{WinHttpOpen(L"Pulse Update/" PULSE_VERSION_STRING,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) { error = GetLastError(); return false; }
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    HttpHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) { error = GetLastError(); return false; }
    HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) { error = GetLastError(); return false; }
    // GitHub release assets redirect to its CDN. Never follow an HTTPS downgrade.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD redirects = 5;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects))) {
        error = GetLastError();
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    if (!WinHttpSendRequest(request.value, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) {
        error = GetLastError();
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        category = UpdateError::HttpStatus;
        error = status ? status : GetLastError();
        return false;
    }
    uint64_t received = 0;
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        if (cancelled) { error = ERROR_CANCELLED; return false; }
        if (GetTickCount64() >= deadline) { error = ERROR_TIMEOUT; return false; }
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            error = GetLastError();
            return false;
        }
        if (!read) return true;
        if (read > maximum_bytes - received) {
            category = UpdateError::ResponseTooLarge;
            error = ERROR_FILE_TOO_LARGE;
            return false;
        }
        received += read;
        if (!consume(buffer.data(), read)) {
            error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
            return false;
        }
    }
}
}
