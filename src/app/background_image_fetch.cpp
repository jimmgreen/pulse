// background_image_fetch.cpp — see background_image_fetch.h.
#include "background_image_fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <limits>
#include <string>

namespace pulse::app {
namespace {

constexpr DWORD kConnectTimeoutMs = 10000;
constexpr DWORD kIoTimeoutMs = 10000;
constexpr ULONGLONG kTotalDeadlineMs = 20000;
constexpr DWORD kMaxRedirects = 5;
constexpr size_t kMaxJsonDepth = 64;

struct HttpHandle {
    HINTERNET value = nullptr;

    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

bool IsDigit(char value) { return value >= '0' && value <= '9'; }

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void AppendUtf8(uint32_t code, std::string& out) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

struct JsonValue {
    enum class Type : uint8_t { Null, Boolean, Number, String, Array, Object };

    Type type = Type::Null;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

// A complete minimal parser: pointer lookup must not depend on key order, so
// object members are collected rather than matched while scanning.
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool Parse(JsonValue& output) {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            position_ = 3;
        }
        SkipSpace();
        if (!Value(output, 0)) return false;
        SkipSpace();
        return position_ == text_.size();
    }

private:
    void SkipSpace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool Take(char expected) {
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool Literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    bool Value(JsonValue& output, size_t depth) {
        if (depth > kMaxJsonDepth) return false;
        SkipSpace();
        if (position_ >= text_.size()) return false;
        switch (text_[position_]) {
            case '{': return Object(output, depth);
            case '[': return Array(output, depth);
            case '"':
                output.type = JsonValue::Type::String;
                return String(output.string);
            case 't':
                if (!Literal("true")) return false;
                output.type = JsonValue::Type::Boolean;
                return true;
            case 'f':
                if (!Literal("false")) return false;
                output.type = JsonValue::Type::Boolean;
                return true;
            case 'n':
                if (!Literal("null")) return false;
                output.type = JsonValue::Type::Null;
                return true;
            default: return Number(output);
        }
    }

    bool Object(JsonValue& output, size_t depth) {
        if (!Take('{')) return false;
        output.type = JsonValue::Type::Object;
        SkipSpace();
        if (Take('}')) return true;
        for (;;) {
            SkipSpace();
            std::string key;
            if (!String(key)) return false;
            SkipSpace();
            if (!Take(':')) return false;
            JsonValue value;
            if (!Value(value, depth + 1)) return false;
            output.object.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            if (Take('}')) return true;
            if (!Take(',')) return false;
        }
    }

    bool Array(JsonValue& output, size_t depth) {
        if (!Take('[')) return false;
        output.type = JsonValue::Type::Array;
        SkipSpace();
        if (Take(']')) return true;
        for (;;) {
            JsonValue value;
            if (!Value(value, depth + 1)) return false;
            output.array.push_back(std::move(value));
            SkipSpace();
            if (Take(']')) return true;
            if (!Take(',')) return false;
        }
    }

    bool Number(JsonValue& output) {
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') {
            ++position_;
        } else if (IsDigit(text_[position_])) {
            while (position_ < text_.size() && IsDigit(text_[position_])) ++position_;
        } else {
            return false;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            if (position_ >= text_.size() || !IsDigit(text_[position_])) return false;
            while (position_ < text_.size() && IsDigit(text_[position_])) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            if (position_ >= text_.size() || !IsDigit(text_[position_])) return false;
            while (position_ < text_.size() && IsDigit(text_[position_])) ++position_;
        }
        if (position_ == start) return false;
        output.type = JsonValue::Type::Number;
        return true;
    }

    bool String(std::string& output) {
        if (!Take('"')) return false;
        while (position_ < text_.size()) {
            const unsigned char value = static_cast<unsigned char>(text_[position_++]);
            if (value == '"') return true;
            if (value < 0x20) return false;
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!Hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') {
                            return false;
                        }
                        position_ += 2;
                        uint32_t low = 0;
                        if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false;
                    }
                    AppendUtf8(code, output);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool Hex4(uint32_t& output) {
        if (position_ + 4 > text_.size()) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexValue(text_[position_ + static_cast<size_t>(i)]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        position_ += 4;
        output = value;
        return true;
    }

    std::string_view text_;
    size_t position_ = 0;
};

bool DecodePointerToken(std::string_view token, std::string& output) {
    output.clear();
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '~') {
            output.push_back(token[i]);
            continue;
        }
        if (i + 1 >= token.size()) return false;
        const char escaped = token[++i];
        if (escaped == '0') output.push_back('~');
        else if (escaped == '1') output.push_back('/');
        else return false;
    }
    return true;
}

bool ParseArrayIndex(std::string_view token, size_t& index) {
    if (token.empty() || (token.size() > 1 && token[0] == '0')) return false;
    size_t value = 0;
    for (const char character : token) {
        if (!IsDigit(character)) return false;
        const size_t digit = static_cast<size_t>(character - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    index = value;
    return true;
}

const JsonValue* FindMember(const JsonValue& object, std::string_view key) {
    const JsonValue* found = nullptr;
    for (const auto& member : object.object) {
        if (member.first == key) found = &member.second;
    }
    return found;
}

bool HeaderExceedsLimit(HINTERNET request, size_t limit) {
    wchar_t text[32]{};
    DWORD bytes = static_cast<DWORD>(sizeof(text));
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, text, &bytes, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - L'0');
        if (value > limit) return true;
    }
    return value > limit;
}

// One budget covers the whole request. Redirects are followed inside a single
// WinHttpSendRequest call, so each hop is bounded by re-arming the session
// timeouts with only the time that is left.
struct RequestDeadline {
    ULONGLONG end = GetTickCount64() + kTotalDeadlineMs;

    DWORD Remaining() const {
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return 0;
        const ULONGLONG left = end - now;
        return left > static_cast<ULONGLONG>(MAXDWORD) ? MAXDWORD
                                                      : static_cast<DWORD>(left);
    }

    bool Expired() const { return GetTickCount64() >= end; }
};

// Narrows the handle's connect/send/receive timeouts to the remaining budget, so
// one slow call cannot spend time the rest of the request still needs.
bool ArmWinHttpTimeouts(HINTERNET handle, const RequestDeadline& deadline,
                        uint32_t& error) {
    const DWORD remaining = deadline.Remaining();
    if (remaining == 0) {
        error = ERROR_TIMEOUT;
        return false;
    }
    const DWORD connect = remaining < kConnectTimeoutMs ? remaining : kConnectTimeoutMs;
    const DWORD transfer = remaining < kIoTimeoutMs ? remaining : kIoTimeoutMs;
    if (!WinHttpSetTimeouts(handle, static_cast<int>(connect), static_cast<int>(connect),
            static_cast<int>(transfer), static_cast<int>(transfer))) {
        error = static_cast<uint32_t>(GetLastError());
        return false;
    }
    return true;
}

// A call that ran into the shared budget reports the timeout instead of the
// error of the last hop, so a slow server stays distinguishable.
uint32_t DeadlineAwareError(uint32_t error, const RequestDeadline& deadline) {
    return deadline.Expired() ? static_cast<uint32_t>(ERROR_TIMEOUT) : error;
}

}  // namespace

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), output.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return output;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), output.data(), length) != length) {
        return {};
    }
    return output;
}

JsonPointerStatus JsonPointerString(std::string_view json, std::wstring_view pointer,
                                    std::string& out) {
    out.clear();
    JsonValue root;
    if (!JsonParser(json).Parse(root)) return JsonPointerStatus::InvalidJson;
    const std::string path = WideToUtf8(pointer);
    if (!pointer.empty() && path.empty()) return JsonPointerStatus::NotFound;
    if (path.empty()) {
        if (root.type != JsonValue::Type::String) return JsonPointerStatus::NotString;
        out = root.string;
        return JsonPointerStatus::Found;
    }
    if (path[0] != '/') return JsonPointerStatus::NotFound;
    const JsonValue* current = &root;
    size_t start = 1;
    for (;;) {
        const size_t slash = path.find('/', start);
        const std::string_view raw = std::string_view(path).substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        std::string token;
        if (!DecodePointerToken(raw, token)) return JsonPointerStatus::NotFound;
        if (current->type == JsonValue::Type::Object) {
            current = FindMember(*current, token);
            if (!current) return JsonPointerStatus::NotFound;
        } else if (current->type == JsonValue::Type::Array) {
            size_t index = 0;
            if (!ParseArrayIndex(token, index) || index >= current->array.size()) {
                return JsonPointerStatus::NotFound;
            }
            current = &current->array[index];
        } else {
            return JsonPointerStatus::NotFound;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (current->type != JsonValue::Type::String) return JsonPointerStatus::NotString;
    out = current->string;
    return JsonPointerStatus::Found;
}

BackgroundImageFormat SniffBackgroundImage(const uint8_t* data, size_t size) noexcept {
    if (!data) return BackgroundImageFormat::Unknown;
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return BackgroundImageFormat::Png;
    }
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return BackgroundImageFormat::Jpeg;
    }
    if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return BackgroundImageFormat::Gif;
    }
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return BackgroundImageFormat::Bmp;
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return BackgroundImageFormat::Webp;
    }
    if (size >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A))) {
        return BackgroundImageFormat::Tiff;
    }
    return BackgroundImageFormat::Unknown;
}

const wchar_t* BackgroundImageExtension(BackgroundImageFormat format) noexcept {
    switch (format) {
        case BackgroundImageFormat::Png: return L"png";
        case BackgroundImageFormat::Jpeg: return L"jpg";
        case BackgroundImageFormat::Gif: return L"gif";
        case BackgroundImageFormat::Bmp: return L"bmp";
        case BackgroundImageFormat::Webp: return L"webp";
        case BackgroundImageFormat::Tiff: return L"tif";
        case BackgroundImageFormat::Unknown:
        default: return nullptr;
    }
}

bool IsHttpUrl(std::wstring_view url) noexcept {
    const auto matches = [&](std::wstring_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            wchar_t character = url[i];
            if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
            if (character != prefix[i]) return false;
        }
        return true;
    };
    return matches(L"http://") || matches(L"https://");
}

bool FetchBackgroundHttp(const BackgroundFetchRequest& request, BackgroundFetchResult& result) {
    result = BackgroundFetchResult{};
    if (request.max_bytes == 0 || !IsHttpUrl(request.url)) {
        result.error = ERROR_WINHTTP_INVALID_URL;
        return false;
    }

    const std::wstring& url = request.url;
    URL_COMPONENTS parts{};
    parts.dwStructSize = static_cast<DWORD>(sizeof(parts));
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = parts.dwUserNameLength = parts.dwPasswordLength =
            static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength) {
        result.error = ERROR_WINHTTP_INVALID_URL;
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path;
    if (parts.dwUrlPathLength) path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    // Session, redirect hops and the body read share this one budget.
    const RequestDeadline deadline;

    HttpHandle session{WinHttpOpen(L"Pulse Background/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) {
        result.error = static_cast<uint32_t>(GetLastError());
        return false;
    }
    uint32_t budget_error = 0;
    if (!ArmWinHttpTimeouts(session.value, deadline, budget_error)) {
        result.error = budget_error;
        return false;
    }

    HttpHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) {
        result.error = DeadlineAwareError(static_cast<uint32_t>(GetLastError()), deadline);
        return false;
    }

    if (deadline.Expired()) {
        result.error = ERROR_TIMEOUT;
        return false;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request_handle{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request_handle.value) {
        result.error = static_cast<uint32_t>(GetLastError());
        return false;
    }

    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD redirects = kMaxRedirects;
    if (!WinHttpSetOption(request_handle.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
            static_cast<DWORD>(sizeof(policy))) ||
        !WinHttpSetOption(request_handle.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
            &redirects, static_cast<DWORD>(sizeof(redirects)))) {
        result.error = static_cast<uint32_t>(GetLastError());
        return false;
    }

    if (!ArmWinHttpTimeouts(session.value, deadline, budget_error)) {
        result.error = budget_error;
        return false;
    }
    if (!WinHttpSendRequest(request_handle.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request_handle.value, nullptr)) {
        result.error = DeadlineAwareError(static_cast<uint32_t>(GetLastError()), deadline);
        return false;
    }
    if (deadline.Expired()) {
        result.error = ERROR_TIMEOUT;
        return false;
    }

    DWORD status = 0;
    DWORD status_size = static_cast<DWORD>(sizeof(status));
    if (!WinHttpQueryHeaders(request_handle.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        result.error = status ? static_cast<uint32_t>(status)
                              : static_cast<uint32_t>(GetLastError());
        return false;
    }
    if (HeaderExceedsLimit(request_handle.value, request.max_bytes)) {
        result.error = ERROR_FILE_TOO_LARGE;
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        // Re-arming per chunk bounds a stalled read by the budget that is left;
        // the outer deadline is therefore never outlived by a single wait.
        uint32_t read_budget_error = 0;
        if (!ArmWinHttpTimeouts(session.value, deadline, read_budget_error)) {
            result.error = read_budget_error;
            return false;
        }
        DWORD read = 0;
        if (!WinHttpReadData(request_handle.value, buffer.data(),
                static_cast<DWORD>(buffer.size()), &read)) {
            result.error = DeadlineAwareError(static_cast<uint32_t>(GetLastError()), deadline);
            return false;
        }
        if (read == 0) break;
        if (static_cast<size_t>(read) > request.max_bytes - result.body.size()) {
            result.error = ERROR_FILE_TOO_LARGE;
            return false;
        }
        result.body.insert(result.body.end(), buffer.data(),
            buffer.data() + static_cast<size_t>(read));
    }
    result.ok = true;
    return true;
}

}  // namespace pulse::app
