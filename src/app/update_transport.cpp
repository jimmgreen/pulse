#include "update_transport.h"
#include "pulse_version.h"
#include <winhttp.h>
#include <array>
#include <limits>

namespace pulse::app {
namespace {
struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
}

uint64_t ParseUpdateContentLength(std::wstring_view header) noexcept {
    uint64_t value = 0;
    if (header.empty()) return 0;
    for (const auto c : header) {
        if (c < L'0' || c > L'9') return 0;
        const auto digit = static_cast<uint64_t>(c - L'0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    return value;
}

bool ReadUpdateResponse(std::wstring_view url, uint64_t maximum_bytes,
                        const std::atomic<bool>& cancelled,
                        const std::function<bool(const void*, DWORD)>& consume,
                        UpdateError& category, DWORD& error) {
    return ReadUpdateResponseWithProgress(url, maximum_bytes, cancelled, consume, category, error, {});
}

bool ReadUpdateResponseWithProgress(std::wstring_view url, uint64_t maximum_bytes,
                        const std::atomic<bool>& cancelled,
                        const std::function<bool(const void*, DWORD)>& consume,
                        UpdateError& category, DWORD& error,
                        const UpdateDownloadProgress& progress) {
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
    wchar_t length_header[32]{};
    DWORD length_size = sizeof(length_header);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, length_header, &length_size, WINHTTP_NO_HEADER_INDEX))
        total = ParseUpdateContentLength(length_header);
    if (total > maximum_bytes) {
        category = UpdateError::ResponseTooLarge;
        error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    if (progress) progress(0, total);
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
        if (!consume(buffer.data(), read)) {
            category = UpdateError::LocalIo;
            error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
            return false;
        }
        received += read;
        // A contradictory/missing length must never produce a fabricated percentage.
        if (total && received > total) total = 0;
        if (progress) progress(received, total);
    }
}

std::wstring AcceleratedUpdateUrl(std::wstring_view url) {
    constexpr std::wstring_view prefix = L"https://github.com/";
    if (!url.starts_with(prefix) || url.find_first_of(L"?#\\") != url.npos) return {};
    auto path = url.substr(prefix.size());
    // Only public release asset URLs may be sent to the external accelerator.
    for (int i = 0; i < 2; ++i) {
        const auto slash = path.find(L'/');
        if (slash == path.npos || slash == 0) return {};
        const auto component = path.substr(0, slash);
        if (component == L"." || component == L".." ||
            component.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != component.npos)
            return {};
        path.remove_prefix(slash + 1);
    }
    if (path.starts_with(L"releases/latest/download/")) {
        path.remove_prefix(std::wstring_view(L"releases/latest/download/").size());
    } else if (path.starts_with(L"releases/download/")) {
        path.remove_prefix(std::wstring_view(L"releases/download/").size());
        const auto slash = path.find(L'/');
        if (slash == path.npos || slash == 0) return {};
        path.remove_prefix(slash + 1);
    } else return {};
    if (path.empty() || path.find(L'/') != path.npos) return {};
    return L"https://ghproxy.net/" + std::wstring(url);
}

namespace {
std::atomic<uint32_t> g_update_source{static_cast<uint32_t>(UpdateSource::Automatic)};
}

void SetUpdateSource(UpdateSource source) noexcept {
    const auto value = static_cast<uint32_t>(source);
    g_update_source.store(value < kUpdateSourceCount ? value : 0u, std::memory_order_relaxed);
}

UpdateSource CurrentUpdateSource() noexcept {
    return static_cast<UpdateSource>(g_update_source.load(std::memory_order_relaxed));
}

std::wstring UpdateSourceUrl(std::wstring_view url, UpdateSource source) {
    const auto accelerated = AcceleratedUpdateUrl(url);
    switch (source) {
        case UpdateSource::Mirror1: return accelerated;
        case UpdateSource::Mirror2:
            return accelerated.empty() ? std::wstring{} : L"https://gh-proxy.com/" + std::wstring(url);
        case UpdateSource::GitHub:
        case UpdateSource::Automatic:
        default: return std::wstring(url);
    }
}

std::vector<std::wstring> UpdateSourceUrls(std::wstring_view url, UpdateSource source) {
    // Custom hosts (not a public release asset) never use a public mirror.
    if (AcceleratedUpdateUrl(url).empty()) return {std::wstring(url)};
    if (source != UpdateSource::Automatic) return {UpdateSourceUrl(url, source)};
    return {UpdateSourceUrl(url, UpdateSource::Mirror1), UpdateSourceUrl(url, UpdateSource::Mirror2),
            std::wstring(url)};
}

bool ReadUpdateWithFallback(std::wstring_view url, uint64_t maximum_bytes,
                            const std::atomic<bool>& cancelled,
                            const std::function<bool()>& reset,
                            const std::function<bool(const void*, DWORD)>& consume,
                            UpdateError& category, DWORD& error,
                            const UpdateResponseReader& read,
                            const std::function<bool()>& verify) {
    // The most telling failure so far: a wrong file beats a refused connection.
    UpdateError mismatch = UpdateError::None;
    DWORD mismatch_error = ERROR_SUCCESS;
    for (const auto& source : UpdateSourceUrls(url, CurrentUpdateSource())) {
        if (source.empty()) continue;
        if (cancelled) { category = UpdateError::Network; error = ERROR_CANCELLED; return false; }
        if (!reset()) {
            category = UpdateError::LocalIo;
            error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
            return false;
        }
        if (read(source, maximum_bytes, cancelled, consume, category, error)) {
            if (!verify || verify()) {
                category = UpdateError::None;
                error = ERROR_SUCCESS;
                return true;
            }
            category = UpdateError::Mismatch;
            error = ERROR_INVALID_DATA;
        }
        if (cancelled || error == ERROR_CANCELLED) return false;
        if (category == UpdateError::ResponseTooLarge || category == UpdateError::Mismatch) {
            mismatch = category;
            mismatch_error = error;
            continue;
        }
        if (category != UpdateError::Network && category != UpdateError::HttpStatus) return false;
    }
    if (mismatch != UpdateError::None) {
        category = mismatch;
        error = mismatch_error;
    }
    return false;
}
}
