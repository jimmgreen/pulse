#include "update_checker.h"

#include "pulse_update_config.h"
#include "pulse_version.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>
#include <vector>

namespace pulse::app {
namespace {

constexpr size_t kMaximumManifestBytes = 1024 * 1024;

class FlatJsonParser {
public:
    explicit FlatJsonParser(std::string_view input) : input_(input) {}

    bool Parse(UpdateManifest& manifest) {
        SkipSpace();
        if (!Take('{')) return false;
        bool seen_schema = false;
        bool seen_version = false;
        bool seen_build = false;
        bool seen_page = false;
        bool seen_hash = false;
        bool seen_signature = false;
        SkipSpace();
        if (Take('}')) return false;
        while (position_ < input_.size()) {
            std::string key;
            if (!String(key) || !TakeAfterSpace(':')) return false;
            if (key == "schema") {
                if (seen_schema || !Unsigned(manifest.schema)) return false;
                seen_schema = true;
            } else if (key == "version") {
                if (seen_version || !StringAfterSpace(manifest.version)) return false;
                seen_version = true;
            } else if (key == "minimum_windows_build") {
                if (seen_build || !Unsigned(manifest.minimum_windows_build)) return false;
                seen_build = true;
            } else if (key == "download_page") {
                if (seen_page || !StringAfterSpace(manifest.download_page)) return false;
                seen_page = true;
            } else if (key == "installer_sha256") {
                if (seen_hash || !StringAfterSpace(manifest.installer_sha256)) return false;
                seen_hash = true;
            } else if (key == "signature") {
                if (seen_signature || !StringAfterSpace(manifest.signature)) return false;
                seen_signature = true;
            } else {
                return false;
            }
            SkipSpace();
            if (Take('}')) break;
            if (!Take(',')) return false;
            SkipSpace();
        }
        SkipSpace();
        return position_ == input_.size() && seen_schema && seen_version && seen_build &&
               seen_page && seen_hash && seen_signature;
    }

private:
    void SkipSpace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }

    bool Take(char value) {
        if (position_ >= input_.size() || input_[position_] != value) return false;
        ++position_;
        return true;
    }

    bool TakeAfterSpace(char value) {
        SkipSpace();
        return Take(value);
    }

    bool StringAfterSpace(std::string& output) {
        SkipSpace();
        return String(output);
    }

    bool String(std::string& output) {
        output.clear();
        if (!Take('"')) return false;
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return true;
            if (value < 0x20 || value >= 0x80) return false;
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            if (escaped == '"' || escaped == '\\' || escaped == '/') output.push_back(escaped);
            else if (escaped == 'b') output.push_back('\b');
            else if (escaped == 'f') output.push_back('\f');
            else if (escaped == 'n') output.push_back('\n');
            else if (escaped == 'r') output.push_back('\r');
            else if (escaped == 't') output.push_back('\t');
            else return false;
        }
        return false;
    }

    bool Unsigned(uint32_t& output) {
        SkipSpace();
        const size_t start = position_;
        while (position_ < input_.size() && input_[position_] >= '0' &&
               input_[position_] <= '9') ++position_;
        if (start == position_ || (position_ - start > 1 && input_[start] == '0')) return false;
        unsigned long long value = 0;
        const auto parsed = std::from_chars(input_.data() + start,
                                             input_.data() + position_, value);
        if (parsed.ec != std::errc{} || value > std::numeric_limits<uint32_t>::max())
            return false;
        output = static_cast<uint32_t>(value);
        return true;
    }

    std::string_view input_;
    size_t position_ = 0;
};

bool IsHex(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    });
}

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool DecodeHex(std::string_view input, std::vector<uint8_t>& output) {
    if ((input.size() & 1u) != 0 || !IsHex(input)) return false;
    output.resize(input.size() / 2);
    for (size_t i = 0; i < output.size(); ++i) {
        output[i] = static_cast<uint8_t>((HexDigit(input[i * 2]) << 4) |
                                          HexDigit(input[i * 2 + 1]));
    }
    return true;
}

bool DecodeBase64(std::string_view input, std::vector<uint8_t>& output) {
    if (input.empty() || input.size() > 1024) return false;
    DWORD size = 0;
    if (!CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr))
        return false;
    output.resize(size);
    return CryptStringToBinaryA(input.data(), static_cast<DWORD>(input.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, output.data(), &size, nullptr, nullptr) != FALSE;
}

bool HashSha256(std::string_view input, std::array<uint8_t, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD bytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<uint8_t> object(object_size);
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size,
        nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest.data(),
                                   static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool VerifySignature(const UpdateManifest& manifest, std::string_view public_key_hex) {
    std::vector<uint8_t> point;
    std::vector<uint8_t> signature;
    if (public_key_hex.size() != 130 || !DecodeHex(public_key_hex, point) ||
        point.size() != 65 || point[0] != 0x04 ||
        !DecodeBase64(manifest.signature, signature) || signature.size() != 64)
        return false;

    struct PublicBlob {
        BCRYPT_ECCKEY_BLOB header;
        uint8_t coordinates[64];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::copy(point.begin() + 1, point.end(), blob.coordinates);

    std::array<uint8_t, 32> digest{};
    if (!HashSha256(CanonicalUpdatePayload(manifest), digest)) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                           nullptr, 0) >= 0;
    if (ok) ok = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB,
        &key, reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0) >= 0;
    if (ok) ok = BCryptVerifySignature(key, nullptr, digest.data(),
        static_cast<ULONG>(digest.size()), signature.data(),
        static_cast<ULONG>(signature.size()), 0) >= 0;
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool ParseVersion(std::string_view input, std::array<uint32_t, 3>& output) {
    size_t start = 0;
    for (size_t part = 0; part < output.size(); ++part) {
        const size_t end = part + 1 == output.size() ? input.size() : input.find('.', start);
        if (end == std::string_view::npos || end == start ||
            (end - start > 1 && input[start] == '0')) return false;
        uint32_t value = 0;
        const auto parsed = std::from_chars(input.data() + start, input.data() + end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + end) return false;
        output[part] = value;
        start = end + 1;
    }
    return start == input.size() + 1;
}

bool IsHttpsUrl(std::string_view input) {
    if (!input.starts_with("https://") || input.size() > 2048) return false;
    if (input.find_first_of("\r\n\t \\") != std::string_view::npos) return false;
    const std::wstring wide(input.begin(), input.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    return WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) &&
           parts.nScheme == INTERNET_SCHEME_HTTPS && parts.dwHostNameLength > 0 &&
           parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0;
}

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), output.data(), size) != size) return {};
    return output;
}

uint32_t CurrentWindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtl_get_version || rtl_get_version(&version) < 0) return 0;
    return version.dwBuildNumber;
}

struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};

bool DownloadManifest(std::wstring_view url, std::string& document,
                      UpdateError& category, DWORD& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength) {
        category = UpdateError::InsecureUrl;
        error = ERROR_WINHTTP_INVALID_URL;
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    HttpHandle session{WinHttpOpen(L"Pulse Update/" PULSE_VERSION_STRING,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) { error = GetLastError(); return false; }
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    HttpHandle connection{WinHttpConnect(session.value, host.c_str(),
                                          parts.nPort, 0)};
    if (!connection.value) { error = GetLastError(); return false; }
    HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) { error = GetLastError(); return false; }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) {
        error = GetLastError();
        return false;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
            &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        category = UpdateError::HttpStatus;
        error = status ? status : GetLastError();
        return false;
    }
    document.clear();
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(),
                static_cast<DWORD>(buffer.size()), &read)) {
            error = GetLastError();
            return false;
        }
        if (!read) break;
        if (document.size() + read > kMaximumManifestBytes) {
            category = UpdateError::ResponseTooLarge;
            error = ERROR_FILE_TOO_LARGE;
            return false;
        }
        document.append(buffer.data(), read);
    }
    return true;
}

UpdateResult CheckConfiguredManifest() {
    UpdateResult result;
#if PULSE_UPDATE_ENABLED
    std::string document;
    UpdateError category = UpdateError::Network;
    DWORD error = ERROR_SUCCESS;
    if (!DownloadManifest(PULSE_UPDATE_MANIFEST_URL, document, category, error)) {
        result.error = category;
        result.diagnostic_code = error;
        return result;
    }
    return ValidateUpdateManifest(document, PULSE_UPDATE_PUBLIC_KEY_HEX,
                                  PULSE_VERSION_STRING_A,
                                  CurrentWindowsBuild());
#else
    result.error = UpdateError::Disabled;
    return result;
#endif
}

} // namespace

std::string CanonicalUpdatePayload(const UpdateManifest& manifest) {
    return "schema=" + std::to_string(manifest.schema) +
        "\nversion=" + manifest.version +
        "\nminimum_windows_build=" + std::to_string(manifest.minimum_windows_build) +
        "\ndownload_page=" + manifest.download_page +
        "\ninstaller_sha256=" + manifest.installer_sha256 + "\n";
}

bool ParseUpdateManifest(std::string_view document, UpdateManifest& manifest) {
    manifest = {};
    if (document.empty() || document.size() > kMaximumManifestBytes) return false;
    return FlatJsonParser(document).Parse(manifest);
}

UpdateResult ValidateUpdateManifest(std::string_view document,
                                    std::string_view public_key_hex,
                                    std::string_view current_version,
                                    uint32_t current_windows_build) {
    UpdateResult result;
    UpdateManifest manifest;
    std::array<uint32_t, 3> available{};
    std::array<uint32_t, 3> current{};
    if (!ParseUpdateManifest(document, manifest) || manifest.schema != 1 ||
        !ParseVersion(manifest.version, available) || !ParseVersion(current_version, current) ||
        manifest.minimum_windows_build < 10240 || manifest.installer_sha256.size() != 64 ||
        !IsHex(manifest.installer_sha256)) {
        result.error = UpdateError::InvalidManifest;
        return result;
    }
    if (!IsHttpsUrl(manifest.download_page)) {
        result.error = UpdateError::InsecureUrl;
        return result;
    }
    if (!VerifySignature(manifest, public_key_hex)) {
        result.error = UpdateError::InvalidSignature;
        return result;
    }
    result.version = Utf8ToWide(manifest.version);
    result.installer_sha256 = Utf8ToWide(manifest.installer_sha256);
    if (available <= current) return result;
    if (!current_windows_build || current_windows_build < manifest.minimum_windows_build) {
        result.error = UpdateError::UnsupportedWindows;
        return result;
    }
    result.download_page = Utf8ToWide(manifest.download_page);
    if (result.download_page.empty()) {
        result.error = UpdateError::InvalidManifest;
        return result;
    }
    result.update_available = true;
    return result;
}

UpdateChecker::~UpdateChecker() {
    Stop();
}

bool UpdateChecker::Enabled() noexcept {
    return PULSE_UPDATE_ENABLED != 0;
}

bool UpdateChecker::CheckAsync(HWND notify, UINT message) {
    if (!Enabled() || !notify || !message || checking_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_result_ = false;
        result_ = {};
    }
    try {
        worker_ = std::thread([this, notify, message] {
            UpdateResult result = CheckConfiguredManifest();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(result);
                has_result_ = true;
            }
            checking_ = false;
            if (!stopping_) PostMessageW(notify, message, 0, 0);
        });
    } catch (...) {
        checking_ = false;
        return false;
    }
    return true;
}

bool UpdateChecker::TakeResult(UpdateResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_result_) return false;
    result = std::move(result_);
    result_ = {};
    has_result_ = false;
    return true;
}

void UpdateChecker::Stop() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
    checking_ = false;
}

} // namespace pulse::app
