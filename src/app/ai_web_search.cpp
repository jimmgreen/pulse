// ai_web_search.cpp - Web search and SSRF-guarded webpage fetching (ai_web_search.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "ai_web_search.h"

#include "../common/json_value.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace pulse::app::ai {
namespace {

constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
constexpr size_t kMaxResponseBytes = 2 * 1024 * 1024;
constexpr int kMaxRedirects = 5;

struct WinHttpHandle {
    HINTERNET value = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : value(h) {}
    ~WinHttpHandle() {
        if (value) WinHttpCloseHandle(value);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = AsciiLower(c);
    return out;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (AsciiLower(s[i]) != AsciiLower(prefix[i])) return false;
    return true;
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
        if (AsciiLower(s[off + i]) != AsciiLower(suffix[i])) return false;
    return true;
}

size_t FindNoCase(std::string_view hay, std::string_view needle, size_t from = 0) {
    if (needle.empty() || from >= hay.size() || needle.size() > hay.size() - from) return std::string_view::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && AsciiLower(hay[i + j]) == AsciiLower(needle[j])) ++j;
        if (j == needle.size()) return i;
    }
    return std::string_view::npos;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return std::string(s.substr(a, b - a));
}

std::string CollapseSpaces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= ' ') {
            if (!out.empty()) space = true;
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

bool HasCjk(std::string_view utf8) {
    for (size_t i = 0; i + 2 < utf8.size(); ++i) {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        const unsigned char b1 = static_cast<unsigned char>(utf8[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(utf8[i + 2]);
        if ((b0 & 0xF0) == 0xE0 && (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            if ((cp >= 0x4E00u && cp <= 0x9FFFu) || (cp >= 0x3400u && cp <= 0x4DBFu)) return true;
        }
    }
    return false;
}

void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return;
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string UrlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = HexDigit(s[i + 1]);
            const int lo = HexDigit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

std::string Base64UrlDecode(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

struct ParsedUrl {
    bool secure = true;
    std::string scheme;
    std::string host;
    std::wstring whost;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path_and_query;
};

bool ParseUrlComponents(std::string_view url, ParsedUrl& out) {
    if (url.empty() || url.size() > 2048) return false;
    const std::wstring wide = json::Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[2048]{};
    wchar_t user[64]{};
    wchar_t pass[64]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    parts.lpszUserName = user;
    parts.dwUserNameLength = ARRAYSIZE(user);
    parts.lpszPassword = pass;
    parts.dwPasswordLength = ARRAYSIZE(pass);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) return false;
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    if (parts.dwUserNameLength > 0 || parts.dwPasswordLength > 0) return false;
    if (parts.dwHostNameLength == 0) return false;

    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.scheme = out.secure ? "https" : "http";
    out.whost.assign(host, parts.dwHostNameLength);
    out.host = LowerCopy(json::Utf8(out.whost));
    out.port = parts.nPort;
    out.path_and_query.assign(path, parts.dwUrlPathLength);
    if (out.path_and_query.empty()) out.path_and_query = L"/";
    if (parts.dwExtraInfoLength > 0) out.path_and_query.append(extra, parts.dwExtraInfoLength);
    return true;
}

std::string StripIpv6Brackets(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        return std::string(host.substr(1, host.size() - 2));
    return std::string(host);
}

bool IsBlockedHostLiteral(std::string_view host_in) {
    const std::string host = LowerCopy(StripIpv6Brackets(host_in));
    if (host.empty()) return true;
    if (host == "localhost" || EndsWithNoCase(host, ".localhost") || EndsWithNoCase(host, ".local") ||
        EndsWithNoCase(host, ".internal") || EndsWithNoCase(host, ".home.arpa"))
        return true;

    IN_ADDR v4{};
    if (InetPtonA(AF_INET, host.c_str(), &v4) == 1)
        return IsBlockedIpv4(ntohl(v4.S_un.S_addr));

    IN6_ADDR v6{};
    if (InetPtonA(AF_INET6, host.c_str(), &v6) == 1)
        return IsBlockedIpv6(v6.u.Byte);

    // Reject single-integer / octal / hex IPv4 shorthands (e.g. 2130706433, 0x7f000001, 0177.0.0.1).
    bool all_numeric_or_hex = true;
    for (char c : host) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == 'x' || c == '.')) {
            all_numeric_or_hex = false;
            break;
        }
    }
    if (all_numeric_or_hex && host.find_first_of("0123456789") != std::string::npos) {
        // Parse dotted or single-number IPv4 conservatively.
        if (host.find('.') == std::string::npos) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(host.c_str(), &end, 0);
            if (end && *end == '\0' && v <= 0xFFFFFFFFull)
                return IsBlockedIpv4(static_cast<uint32_t>(v));
        } else {
            uint32_t parts[4]{};
            int count = 0;
            size_t pos = 0;
            bool ok = true;
            while (pos < host.size() && count < 4) {
                size_t dot = host.find('.', pos);
                if (dot == std::string::npos) dot = host.size();
                std::string seg = host.substr(pos, dot - pos);
                if (seg.empty()) { ok = false; break; }
                char* end = nullptr;
                const unsigned long long v = std::strtoull(seg.c_str(), &end, 0);
                if (!end || *end != '\0' || v > 0xFFFFFFFFull) { ok = false; break; }
                parts[count++] = static_cast<uint32_t>(v);
                pos = dot < host.size() ? dot + 1 : dot;
            }
            if (ok && count >= 2 && count <= 4) {
                uint32_t ip = 0;
                if (count == 2 && parts[0] <= 255 && parts[1] <= 0xFFFFFFu)
                    ip = (parts[0] << 24) | parts[1];
                else if (count == 3 && parts[0] <= 255 && parts[1] <= 255 && parts[2] <= 0xFFFFu)
                    ip = (parts[0] << 24) | (parts[1] << 16) | parts[2];
                else if (count == 4 && parts[0] <= 255 && parts[1] <= 255 && parts[2] <= 255 && parts[3] <= 255)
                    ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
                if (ip != 0 || parts[0] == 0) return IsBlockedIpv4(ip);
            }
        }
    }
    return false;
}

bool ResolveAndCheckPublicHost(const std::wstring& host, std::string& err_code, std::string& err_msg) {
    EnsureWinsock();
    const std::string utf8_host = StripIpv6Brackets(json::Utf8(host));
    if (IsBlockedHostLiteral(utf8_host)) {
        err_code = "blocked_host";
        err_msg = "Refused to access a local or private network address.";
        return false;
    }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    const std::wstring clean = json::Wide(utf8_host);
    if (GetAddrInfoW(clean.c_str(), nullptr, &hints, &res) != 0 || !res) {
        // Let WinHTTP surface its own proxy/DNS resolution if system uses a proxy;
        // only direct local literals were rejected above.
        return true;
    }
    bool blocked = false;
    for (const ADDRINFOW* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof(sockaddr_in)) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            if (IsBlockedIpv4(ntohl(sin->sin_addr.S_un.S_addr))) {
                blocked = true;
                break;
            }
        } else if (p->ai_family == AF_INET6 && p->ai_addrlen >= sizeof(sockaddr_in6)) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
            if (IsBlockedIpv6(sin6->sin6_addr.u.Byte)) {
                blocked = true;
                break;
            }
        }
    }
    FreeAddrInfoW(res);
    if (blocked) {
        err_code = "blocked_ip";
        err_msg = "Target hostname resolves to a private or loopback IP address.";
        return false;
    }
    return true;
}

struct HttpResult {
    bool ok = false;
    int status = 0;
    DWORD win32_error = 0;
    std::string content_type;
    std::string location;
    std::string body;
};

std::string QueryHeaderUtf8(HINTERNET req, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 16384) return {};
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return json::Utf8(buf);
}

HttpResult DoHttpRequest(const ParsedUrl& url, const wchar_t* method, const std::wstring& extra_headers,
                         const std::string* post_body, DWORD connect_ms, DWORD receive_ms, bool allow_redirects,
                         size_t max_bytes, const std::atomic<bool>& cancelled) {
    HttpResult res;
    if (cancelled.load()) {
        res.win32_error = ERROR_CANCELLED;
        return res;
    }
    WinHttpHandle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value)
        session.value = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        res.win32_error = GetLastError();
        return res;
    }
    WinHttpSetTimeouts(session.value, static_cast<int>(connect_ms), static_cast<int>(connect_ms), 10000,
                       static_cast<int>(receive_ms));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.value, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
#endif
    WinHttpHandle conn(WinHttpConnect(session.value, url.whost.c_str(), url.port, 0));
    if (!conn.value) {
        res.win32_error = GetLastError();
        return res;
    }
    WinHttpHandle req(WinHttpOpenRequest(conn.value, method, url.path_and_query.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req.value) {
        res.win32_error = GetLastError();
        return res;
    }
    if (!allow_redirects) {
        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req.value, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    }
    const void* payload = post_body && !post_body->empty() ? post_body->data() : WINHTTP_NO_REQUEST_DATA;
    const DWORD payload_len = post_body ? static_cast<DWORD>(post_body->size()) : 0;
    const BOOL sent = WinHttpSendRequest(req.value,
                                         extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str(),
                                         extra_headers.empty() ? 0 : static_cast<DWORD>(extra_headers.size()),
                                         const_cast<void*>(payload), payload_len, payload_len, 0);
    if (!sent || cancelled.load() || !WinHttpReceiveResponse(req.value, nullptr)) {
        res.win32_error = cancelled.load() ? ERROR_CANCELLED : GetLastError();
        return res;
    }
    DWORD status = 0, st_size = sizeof(status);
    WinHttpQueryHeaders(req.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &st_size, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);
    res.content_type = QueryHeaderUtf8(req.value, WINHTTP_QUERY_CONTENT_TYPE);
    if (res.status >= 300 && res.status < 400) res.location = QueryHeaderUtf8(req.value, WINHTTP_QUERY_LOCATION);

    std::vector<char> buf(16 * 1024);
    for (;;) {
        if (cancelled.load()) {
            res.win32_error = ERROR_CANCELLED;
            return res;
        }
        DWORD read = 0;
        if (!WinHttpReadData(req.value, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
            res.win32_error = GetLastError();
            return res;
        }
        if (read == 0) break;
        const size_t room = max_bytes > res.body.size() ? max_bytes - res.body.size() : 0;
        if (room > 0) res.body.append(buf.data(), (std::min)(static_cast<size_t>(read), room));
        if (res.body.size() >= max_bytes) break;
    }
    res.ok = true;
    return res;
}

std::string ResolveRedirect(const ParsedUrl& base, std::string_view loc) {
    const std::string l = Trim(loc);
    if (l.empty()) return {};
    if (StartsWithNoCase(l, "http://") || StartsWithNoCase(l, "https://")) return l;
    if (l.size() >= 2 && l[0] == '/' && l[1] == '/') return base.scheme + ":" + l;
    std::string origin = base.scheme + "://" + base.host;
    const bool def_port = (base.secure && base.port == 443) || (!base.secure && base.port == 80);
    if (!def_port) origin += ":" + std::to_string(base.port);
    if (l[0] == '/') return origin + l;
    std::string cur = json::Utf8(base.path_and_query);
    if (const size_t q = cur.find('?'); q != std::string::npos) cur.resize(q);
    const size_t slash = cur.rfind('/');
    cur = slash == std::string::npos ? "/" : cur.substr(0, slash + 1);
    return origin + cur + l;
}

std::string DetectCharset(std::string_view content_type, std::string_view body) {
    auto extract = [](std::string_view text) -> std::string {
        const size_t at = FindNoCase(text, "charset=");
        if (at == std::string_view::npos) return {};
        size_t i = at + 8;
        while (i < text.size() && (text[i] == ' ' || text[i] == '"' || text[i] == '\'')) ++i;
        size_t j = i;
        while (j < text.size()) {
            const char c = text[j];
            if (c == ';' || c == '"' || c == '\'' || c == '>' || c == '/' || static_cast<unsigned char>(c) <= ' ')
                break;
            ++j;
        }
        return LowerCopy(text.substr(i, j - i));
    };
    if (std::string cs = extract(content_type); !cs.empty()) return cs;
    return extract(body.substr(0, (std::min)(body.size(), static_cast<size_t>(4096))));
}

std::string ConvertToUtf8(std::string_view raw, std::string_view charset) {
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        return std::string(raw.substr(3));
    }
    UINT cp = CP_UTF8;
    if (charset == "gbk" || charset == "gb2312" || charset == "gb18030" || charset == "x-gbk") cp = 54936;
    else if (charset == "big5") cp = 950;
    else if (charset == "shift_jis" || charset == "shift-jis" || charset == "sjis") cp = 932;
    else if (charset == "euc-kr" || charset == "ks_c_5601-1987") cp = 949;
    else if (charset == "iso-8859-1" || charset == "latin1" || charset == "windows-1252") cp = 1252;
    if (cp == CP_UTF8) return std::string(raw);

    int wlen = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0 && cp == 54936) {
        cp = 936;
        wlen = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    }
    if (wlen <= 0) return std::string(raw);
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), wlen);
    return json::Utf8(wide);
}

std::string StripTags(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            const size_t gt = html.find('>', i + 1);
            if (gt == std::string_view::npos) break;
            const std::string_view inside = html.substr(i + 1, gt - i - 1);
            size_t k = 0;
            while (k < inside.size() && (static_cast<unsigned char>(inside[k]) <= ' ' || inside[k] == '/')) ++k;
            size_t e = k;
            while (e < inside.size() && std::isalnum(static_cast<unsigned char>(inside[e]))) ++e;
            const std::string tag = LowerCopy(inside.substr(k, e - k));
            if (tag == "br" || tag == "p" || tag == "div" || tag == "li" || tag == "tr" || tag == "td" || tag == "th")
                out.push_back(' ');
            i = gt + 1;
            continue;
        }
        out.push_back(html[i++]);
    }
    return CollapseSpaces(DecodeHtmlEntities(out));
}

// Extracts an attribute value (href="...") from a single HTML opening tag.
std::string ExtractAttr(std::string_view tag, std::string_view attr_name) {
    size_t pos = 0;
    while (pos < tag.size()) {
        const size_t at = FindNoCase(tag, attr_name, pos);
        if (at == std::string_view::npos) return {};
        if (at > 0 && std::isalnum(static_cast<unsigned char>(tag[at - 1]))) {
            pos = at + attr_name.size();
            continue;
        }
        size_t i = at + attr_name.size();
        while (i < tag.size() && static_cast<unsigned char>(tag[i]) <= ' ') ++i;
        if (i >= tag.size() || tag[i] != '=') {
            pos = i;
            continue;
        }
        ++i;
        while (i < tag.size() && static_cast<unsigned char>(tag[i]) <= ' ') ++i;
        if (i >= tag.size()) return {};
        if (tag[i] == '"' || tag[i] == '\'') {
            const char q = tag[i++];
            const size_t end = tag.find(q, i);
            if (end == std::string_view::npos) return {};
            return DecodeHtmlEntities(tag.substr(i, end - i));
        }
        size_t end = i;
        while (end < tag.size() && static_cast<unsigned char>(tag[end]) > ' ' && tag[end] != '>') ++end;
        return DecodeHtmlEntities(tag.substr(i, end - i));
    }
    return {};
}

// Truncates UTF-8 string cleanly at `max_chars` Unicode code points.
std::string TruncateUtf8Chars(const std::string& s, size_t max_chars, bool* truncated) {
    size_t chars = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (chars >= max_chars) {
            if (truncated) *truncated = true;
            return s.substr(0, i);
        }
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((b & 0xE0) == 0xC0) step = 2;
        else if ((b & 0xF0) == 0xE0) step = 3;
        else if ((b & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++chars;
    }
    if (truncated) *truncated = false;
    return s;
}

WebSearchResponse SearchDuckDuckGo(std::string_view query, size_t max_results, const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "ddg";
    const std::string url = "https://html.duckduckgo.com/html/?q=" + UrlEncode(query);
    ParsedUrl parsed;
    if (!ParseUrlComponents(url, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Failed to build DuckDuckGo query URL.";
        return resp;
    }
    const std::wstring headers =
        L"Accept: text/html,application/xhtml+xml\r\nAccept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n";
    const HttpResult http = DoHttpRequest(parsed, L"GET", headers, nullptr, 3500, 7000, true, kMaxResponseBytes, cancelled);
    if (!http.ok) {
        resp.error_code = http.win32_error == ERROR_CANCELLED ? "cancelled" : "network";
        resp.error_message = "DuckDuckGo request failed (Win32 " + std::to_string(http.win32_error) + ").";
        return resp;
    }
    if (http.status != 200) {
        resp.error_code = "http_" + std::to_string(http.status);
        resp.error_message = "DuckDuckGo returned HTTP " + std::to_string(http.status) + ".";
        return resp;
    }
    resp.results = ParseDuckDuckGoHtml(http.body, max_results);
    resp.ok = !resp.results.empty();
    if (!resp.ok) {
        resp.error_code = "empty_results";
        resp.error_message = "DuckDuckGo returned no parseable search results.";
    }
    return resp;
}

WebSearchResponse SearchBing(std::string_view query, size_t max_results, const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "bing";
    std::string url = "https://www.bing.com/search?q=" + UrlEncode(query);
    if (HasCjk(query)) url += "&mkt=zh-CN&setlang=zh-hans";
    ParsedUrl parsed;
    if (!ParseUrlComponents(url, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Failed to build Bing query URL.";
        return resp;
    }
    const std::wstring headers =
        L"Accept: text/html,application/xhtml+xml\r\nAccept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n";
    const HttpResult http = DoHttpRequest(parsed, L"GET", headers, nullptr, 4000, 8000, true, kMaxResponseBytes, cancelled);
    if (!http.ok) {
        resp.error_code = http.win32_error == ERROR_CANCELLED ? "cancelled" : "network";
        resp.error_message = "Bing request failed (Win32 " + std::to_string(http.win32_error) + ").";
        return resp;
    }
    if (http.status != 200) {
        resp.error_code = "http_" + std::to_string(http.status);
        resp.error_message = "Bing returned HTTP " + std::to_string(http.status) + ".";
        return resp;
    }
    resp.results = ParseBingHtml(http.body, max_results);
    resp.ok = !resp.results.empty();
    if (!resp.ok) {
        resp.error_code = "empty_results";
        resp.error_message = "Bing returned no parseable search results.";
    }
    return resp;
}

WebSearchResponse SearchTavily(std::string_view query, size_t max_results, const WebSearchConfig& cfg,
                               const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "tavily";
    if (cfg.api_key.empty()) {
        resp.error_code = "no_api_key";
        resp.error_message = "Tavily search requires an API key.";
        return resp;
    }
    ParsedUrl parsed;
    const std::string ep = cfg.endpoint.empty() ? "https://api.tavily.com/search" : cfg.endpoint;
    if (!ParseUrlComponents(ep, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Invalid Tavily endpoint URL.";
        return resp;
    }
    json::Value req = json::Value::Object();
    req.set("api_key", json::Utf8(cfg.api_key));
    req.set("query", std::string(query));
    req.set("max_results", static_cast<int64_t>(max_results));
    req.set("include_answer", true);
    std::string body = json::Serialize(req);
    const HttpResult http = DoHttpRequest(parsed, L"POST", L"Content-Type: application/json\r\nAccept: application/json\r\n",
                                          &body, 5000, 12000, true, kMaxResponseBytes, cancelled);
    SecureZeroMemory(body.data(), body.size());
    if (!http.ok || http.status != 200) {
        resp.error_code = !http.ok ? "network" : ("http_" + std::to_string(http.status));
        resp.error_message = "Tavily search failed.";
        return resp;
    }
    json::Value v;
    if (!json::Parse(http.body, v) || !v.is_object()) {
        resp.error_code = "bad_json";
        resp.error_message = "Tavily returned invalid JSON.";
        return resp;
    }
    resp.answer = v.get_string("answer");
    if (const json::Value* arr = v.find("results"); arr && arr->is_array()) {
        for (const json::Value& item : arr->items()) {
            if (resp.results.size() >= max_results) break;
            WebSearchItem r;
            r.title = Trim(item.get_string("title"));
            r.url = Trim(item.get_string("url"));
            r.snippet = CollapseSpaces(item.get_string("content"));
            r.source = ExtractUrlHost(r.url);
            if (!r.url.empty() && !r.title.empty()) resp.results.push_back(std::move(r));
        }
    }
    resp.ok = !resp.results.empty() || !resp.answer.empty();
    return resp;
}

WebSearchResponse SearchBocha(std::string_view query, size_t max_results, const WebSearchConfig& cfg,
                              const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "bocha";
    if (cfg.api_key.empty()) {
        resp.error_code = "no_api_key";
        resp.error_message = "Bocha search requires an API key.";
        return resp;
    }
    ParsedUrl parsed;
    const std::string ep = cfg.endpoint.empty() ? "https://api.bochaai.com/v1/web-search" : cfg.endpoint;
    if (!ParseUrlComponents(ep, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Invalid Bocha endpoint URL.";
        return resp;
    }
    json::Value req = json::Value::Object();
    req.set("query", std::string(query));
    req.set("count", static_cast<int64_t>(max_results));
    req.set("summary", true);
    const std::string body = json::Serialize(req);
    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\nAuthorization: Bearer " +
                           cfg.api_key + L"\r\n";
    const HttpResult http = DoHttpRequest(parsed, L"POST", headers, &body, 5000, 12000, true, kMaxResponseBytes, cancelled);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!http.ok || http.status != 200) {
        resp.error_code = !http.ok ? "network" : ("http_" + std::to_string(http.status));
        resp.error_message = "Bocha search failed.";
        return resp;
    }
    json::Value v;
    if (!json::Parse(http.body, v) || !v.is_object()) {
        resp.error_code = "bad_json";
        resp.error_message = "Bocha returned invalid JSON.";
        return resp;
    }
    const json::Value* data = v.find("data");
    const json::Value* wp = data && data->is_object() ? data->find("webPages") : nullptr;
    const json::Value* vals = wp && wp->is_object() ? wp->find("value") : nullptr;
    if (vals && vals->is_array()) {
        for (const json::Value& item : vals->items()) {
            if (resp.results.size() >= max_results) break;
            WebSearchItem r;
            r.title = Trim(item.get_string("name"));
            r.url = Trim(item.get_string("url"));
            r.snippet = CollapseSpaces(item.get_string("summary"));
            if (r.snippet.empty()) r.snippet = CollapseSpaces(item.get_string("snippet"));
            r.source = ExtractUrlHost(r.url);
            if (!r.url.empty() && !r.title.empty()) resp.results.push_back(std::move(r));
        }
    }
    resp.ok = !resp.results.empty();
    return resp;
}

WebSearchResponse SearchSerper(std::string_view query, size_t max_results, const WebSearchConfig& cfg,
                               const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "serper";
    if (cfg.api_key.empty()) {
        resp.error_code = "no_api_key";
        resp.error_message = "Serper search requires an API key.";
        return resp;
    }
    ParsedUrl parsed;
    const std::string ep = cfg.endpoint.empty() ? "https://google.serper.dev/search" : cfg.endpoint;
    if (!ParseUrlComponents(ep, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Invalid Serper endpoint URL.";
        return resp;
    }
    json::Value req = json::Value::Object();
    req.set("q", std::string(query));
    req.set("num", static_cast<int64_t>(max_results));
    const std::string body = json::Serialize(req);
    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\nX-API-KEY: " +
                           cfg.api_key + L"\r\n";
    const HttpResult http = DoHttpRequest(parsed, L"POST", headers, &body, 5000, 12000, true, kMaxResponseBytes, cancelled);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!http.ok || http.status != 200) {
        resp.error_code = !http.ok ? "network" : ("http_" + std::to_string(http.status));
        resp.error_message = "Serper search failed.";
        return resp;
    }
    json::Value v;
    if (!json::Parse(http.body, v) || !v.is_object()) {
        resp.error_code = "bad_json";
        resp.error_message = "Serper returned invalid JSON.";
        return resp;
    }
    if (const json::Value* arr = v.find("organic"); arr && arr->is_array()) {
        for (const json::Value& item : arr->items()) {
            if (resp.results.size() >= max_results) break;
            WebSearchItem r;
            r.title = Trim(item.get_string("title"));
            r.url = Trim(item.get_string("link"));
            r.snippet = CollapseSpaces(item.get_string("snippet"));
            r.source = ExtractUrlHost(r.url);
            if (!r.url.empty() && !r.title.empty()) resp.results.push_back(std::move(r));
        }
    }
    resp.ok = !resp.results.empty();
    return resp;
}

WebSearchResponse SearchSearxng(std::string_view query, size_t max_results, const WebSearchConfig& cfg,
                                const std::atomic<bool>& cancelled) {
    WebSearchResponse resp;
    resp.engine = "searxng";
    if (cfg.endpoint.empty()) {
        resp.error_code = "no_endpoint";
        resp.error_message = "SearXNG requires a configured endpoint URL.";
        return resp;
    }
    std::string base = cfg.endpoint;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string url = base + "/search?q=" + UrlEncode(query) + "&format=json";
    ParsedUrl parsed;
    if (!ParseUrlComponents(url, parsed)) {
        resp.error_code = "bad_url";
        resp.error_message = "Invalid SearXNG endpoint URL.";
        return resp;
    }
    const HttpResult http = DoHttpRequest(parsed, L"GET", L"Accept: application/json\r\n", nullptr, 5000, 10000, true,
                                          kMaxResponseBytes, cancelled);
    if (!http.ok || http.status != 200) {
        resp.error_code = !http.ok ? "network" : ("http_" + std::to_string(http.status));
        resp.error_message = "SearXNG search failed.";
        return resp;
    }
    json::Value v;
    if (!json::Parse(http.body, v) || !v.is_object()) {
        resp.error_code = "bad_json";
        resp.error_message = "SearXNG returned invalid JSON.";
        return resp;
    }
    if (const json::Value* arr = v.find("results"); arr && arr->is_array()) {
        for (const json::Value& item : arr->items()) {
            if (resp.results.size() >= max_results) break;
            WebSearchItem r;
            r.title = Trim(item.get_string("title"));
            r.url = Trim(item.get_string("url"));
            r.snippet = CollapseSpaces(item.get_string("content"));
            r.source = ExtractUrlHost(r.url);
            if (!r.url.empty() && !r.title.empty()) resp.results.push_back(std::move(r));
        }
    }
    resp.ok = !resp.results.empty();
    return resp;
}

} // namespace

bool IsBlockedIpv4(uint32_t ip) {
    const uint32_t a = (ip >> 24) & 0xFFu;
    const uint32_t b = (ip >> 16) & 0xFFu;
    if (a == 0u || a == 10u || a == 127u) return true;
    if (a == 100u && (b >= 64u && b <= 127u)) return true;    // CGNAT 100.64.0.0/10
    if (a == 169u && b == 254u) return true;                  // link-local / cloud metadata
    if (a == 172u && (b >= 16u && b <= 31u)) return true;     // RFC1918
    if (a == 192u && b == 168u) return true;                  // RFC1918
    if (a == 192u && b == 0u && (((ip >> 8) & 0xFFu) == 0u || ((ip >> 8) & 0xFFu) == 2u)) return true;
    // Note: 198.18.0.0/15 is intentionally NOT blocked because TUN/Fake-IP
    // proxies (Clash, Mihomo, Sing-box) map public internet domains into it.
    if (a >= 224u) return true;                               // multicast / reserved / broadcast
    return false;
}

bool IsBlockedIpv6(const uint8_t bytes[16]) {
    bool all_zero = true;
    for (int i = 0; i < 15; ++i)
        if (bytes[i] != 0) all_zero = false;
    if (all_zero && (bytes[15] == 0 || bytes[15] == 1)) return true;   // :: or ::1
    if ((bytes[0] & 0xFEu) == 0xFCu) return true;                      // fc00::/7 unique local
    if (bytes[0] == 0xFEu && (bytes[1] & 0xC0u) == 0x80u) return true; // fe80::/10 link-local
    if (bytes[0] == 0xFFu) return true;                                // ff00::/8 multicast
    // IPv4-mapped ::ffff:a.b.c.d
    bool mapped = true;
    for (int i = 0; i < 10; ++i)
        if (bytes[i] != 0) mapped = false;
    if (mapped && bytes[10] == 0xFFu && bytes[11] == 0xFFu) {
        const uint32_t v4 = (static_cast<uint32_t>(bytes[12]) << 24) | (static_cast<uint32_t>(bytes[13]) << 16) |
                            (static_cast<uint32_t>(bytes[14]) << 8) | static_cast<uint32_t>(bytes[15]);
        return IsBlockedIpv4(v4);
    }
    return false;
}

bool ValidatePublicHttpUrl(std::string_view url, std::string* error_code, std::string* error_message) {
    auto fail = [&](const char* code, const char* msg) {
        if (error_code) *error_code = code;
        if (error_message) *error_message = msg;
        return false;
    };
    const std::string clean = Trim(url);
    if (clean.empty() || clean.size() > 2048) return fail("invalid_url", "URL is empty or too long.");
    for (unsigned char c : clean)
        if (c <= ' ') return fail("invalid_url", "URL contains whitespace or control characters.");
    if (!StartsWithNoCase(clean, "http://") && !StartsWithNoCase(clean, "https://"))
        return fail("unsupported_scheme", "Only http:// and https:// URLs are allowed.");
    ParsedUrl parsed;
    if (!ParseUrlComponents(clean, parsed))
        return fail("invalid_url", "URL is malformed or contains embedded credentials.");
    if (IsBlockedHostLiteral(parsed.host))
        return fail("blocked_host", "Refused to access a local or private network address.");
    return true;
}

std::string ExtractUrlHost(std::string_view url) {
    ParsedUrl parsed;
    return ParseUrlComponents(url, parsed) ? parsed.host : std::string{};
}

std::string DecodeHtmlEntities(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            out.push_back(input[i]);
            continue;
        }
        const size_t semi = input.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        const std::string_view ent = input.substr(i + 1, semi - i - 1);
        bool decoded = true;
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos" || ent == "#39" || ent == "#x27") out.push_back('\'');
        else if (ent == "nbsp" || ent == "ensp" || ent == "emsp" || ent == "thinsp") out.push_back(' ');
        else if (ent == "mdash") out += "\xE2\x80\x94";
        else if (ent == "ndash") out += "\xE2\x80\x93";
        else if (ent == "hellip") out += "\xE2\x80\xA6";
        else if (ent == "middot") out += "\xC2\xB7";
        else if (ent == "copy") out += "\xC2\xA9";
        else if (ent == "reg") out += "\xC2\xAE";
        else if (ent == "laquo") out += "\xC2\xAB";
        else if (ent == "raquo") out += "\xC2\xBB";
        else if (ent.size() >= 2 && ent[0] == '#') {
            uint32_t cp = 0;
            bool ok = false;
            if (ent.size() >= 3 && (ent[1] == 'x' || ent[1] == 'X')) {
                ok = true;
                for (size_t k = 2; k < ent.size(); ++k) {
                    const int d = HexDigit(ent[k]);
                    if (d < 0) { ok = false; break; }
                    cp = (cp << 4) | static_cast<uint32_t>(d);
                    if (cp > 0x10FFFFu) { ok = false; break; }
                }
            } else {
                ok = true;
                for (size_t k = 1; k < ent.size(); ++k) {
                    if (ent[k] < '0' || ent[k] > '9') { ok = false; break; }
                    cp = cp * 10u + static_cast<uint32_t>(ent[k] - '0');
                    if (cp > 0x10FFFFu) { ok = false; break; }
                }
            }
            if (ok && cp > 0) AppendUtf8Codepoint(out, cp);
            else decoded = false;
        } else {
            decoded = false;
        }
        if (decoded) i = semi;
        else out.push_back('&');
    }
    return out;
}

std::string DecodeDuckDuckGoRedirectUrl(std::string_view href) {
    std::string s = DecodeHtmlEntities(Trim(href));
    if (s.rfind("//", 0) == 0) s = "https:" + s;
    const size_t uddg = FindNoCase(s, "uddg=");
    if (uddg != std::string_view::npos) {
        size_t end = s.find('&', uddg + 5);
        if (end == std::string::npos) end = s.size();
        s = UrlDecode(std::string_view(s).substr(uddg + 5, end - (uddg + 5)));
    }
    if (!StartsWithNoCase(s, "http://") && !StartsWithNoCase(s, "https://")) return {};
    if (FindNoCase(s, "duckduckgo.com/y.js") != std::string_view::npos) return {};
    return s;
}

std::string DecodeBingRedirectUrl(std::string_view href) {
    std::string s = DecodeHtmlEntities(Trim(href));
    if (FindNoCase(s, "bing.com/ck/a") != std::string_view::npos) {
        size_t pos = FindNoCase(s, "&u=a1");
        if (pos == std::string_view::npos) pos = FindNoCase(s, "?u=a1");
        if (pos != std::string_view::npos) {
            pos += 5;
            size_t end = s.find('&', pos);
            if (end == std::string::npos) end = s.size();
            const std::string decoded = Base64UrlDecode(std::string_view(s).substr(pos, end - pos));
            if (StartsWithNoCase(decoded, "http://") || StartsWithNoCase(decoded, "https://")) return decoded;
        }
        return {};
    }
    if (!StartsWithNoCase(s, "http://") && !StartsWithNoCase(s, "https://")) return {};
    return s;
}

std::vector<WebSearchItem> ParseDuckDuckGoHtml(std::string_view html, size_t max_results) {
    std::vector<WebSearchItem> out;
    std::unordered_set<std::string> seen;
    size_t pos = 0;
    while (pos < html.size() && out.size() < max_results) {
        const size_t cls = FindNoCase(html, "result__a", pos);
        if (cls == std::string_view::npos) break;
        const size_t open = html.rfind('<', cls);
        const size_t close = html.find('>', cls);
        if (open == std::string_view::npos || close == std::string_view::npos) break;
        const size_t end_a = FindNoCase(html, "</a>", close);
        if (end_a == std::string_view::npos) break;

        const std::string_view open_tag = html.substr(open, close - open + 1);
        const std::string url = DecodeDuckDuckGoRedirectUrl(ExtractAttr(open_tag, "href"));
        const std::string title = StripTags(html.substr(close + 1, end_a - (close + 1)));

        const size_t next_res = FindNoCase(html, "result__a", end_a + 4);
        const size_t chunk_end = next_res == std::string_view::npos ? html.size() : next_res;
        std::string snippet;
        const size_t sn_cls = FindNoCase(html, "result__snippet", end_a + 4);
        if (sn_cls != std::string_view::npos && sn_cls < chunk_end) {
            const size_t sn_gt = html.find('>', sn_cls);
            if (sn_gt != std::string_view::npos && sn_gt < chunk_end) {
                size_t sn_end = FindNoCase(html, "</a>", sn_gt);
                if (sn_end == std::string_view::npos || sn_end > chunk_end)
                    sn_end = FindNoCase(html, "</div>", sn_gt);
                if (sn_end != std::string_view::npos && sn_end <= chunk_end)
                    snippet = StripTags(html.substr(sn_gt + 1, sn_end - (sn_gt + 1)));
            }
        }
        if (!url.empty() && !title.empty() && seen.insert(url).second) {
            WebSearchItem item;
            item.title = title;
            item.url = url;
            item.snippet = snippet;
            item.source = ExtractUrlHost(url);
            out.push_back(std::move(item));
        }
        pos = end_a + 4;
    }
    return out;
}

std::vector<WebSearchItem> ParseBingHtml(std::string_view html, size_t max_results) {
    std::vector<WebSearchItem> out;
    std::unordered_set<std::string> seen;
    size_t pos = 0;
    while (pos < html.size() && out.size() < max_results) {
        const size_t li = FindNoCase(html, "<li class=\"b_algo\"", pos);
        if (li == std::string_view::npos) break;
        const size_t next_li = FindNoCase(html, "<li class=\"b_algo\"", li + 18);
        size_t end_li = FindNoCase(html, "</li>", li + 18);
        if (end_li == std::string_view::npos || (next_li != std::string_view::npos && end_li > next_li))
            end_li = next_li == std::string_view::npos ? html.size() : next_li;
        const std::string_view chunk = html.substr(li, end_li - li);

        const size_t h2 = FindNoCase(chunk, "<h2");
        if (h2 != std::string_view::npos) {
            const size_t a_open = FindNoCase(chunk, "<a", h2);
            const size_t a_gt = a_open != std::string_view::npos ? chunk.find('>', a_open) : std::string_view::npos;
            const size_t a_end = a_gt != std::string_view::npos ? FindNoCase(chunk, "</a>", a_gt) : std::string_view::npos;
            if (a_end != std::string_view::npos) {
                const std::string url = DecodeBingRedirectUrl(ExtractAttr(chunk.substr(a_open, a_gt - a_open + 1), "href"));
                const std::string title = StripTags(chunk.substr(a_gt + 1, a_end - (a_gt + 1)));
                std::string snippet;
                size_t p_from = FindNoCase(chunk, "b_caption", a_end);
                if (p_from == std::string_view::npos) p_from = a_end;
                const size_t p_open = FindNoCase(chunk, "<p", p_from);
                const size_t p_gt = p_open != std::string_view::npos ? chunk.find('>', p_open) : std::string_view::npos;
                const size_t p_end = p_gt != std::string_view::npos ? FindNoCase(chunk, "</p>", p_gt) : std::string_view::npos;
                if (p_end != std::string_view::npos)
                    snippet = StripTags(chunk.substr(p_gt + 1, p_end - (p_gt + 1)));
                if (!url.empty() && !title.empty() && seen.insert(url).second) {
                    WebSearchItem item;
                    item.title = title;
                    item.url = url;
                    item.snippet = snippet;
                    item.source = ExtractUrlHost(url);
                    out.push_back(std::move(item));
                }
            }
        }
        pos = end_li;
    }
    return out;
}

std::string HtmlToReadableText(std::string_view html, std::string* title_out, size_t max_chars, bool* truncated) {
    if (title_out) {
        title_out->clear();
        const size_t t0 = FindNoCase(html, "<title");
        const size_t t_gt = t0 != std::string_view::npos ? html.find('>', t0) : std::string_view::npos;
        const size_t t1 = t_gt != std::string_view::npos ? FindNoCase(html, "</title>", t_gt) : std::string_view::npos;
        if (t1 != std::string_view::npos)
            *title_out = StripTags(html.substr(t_gt + 1, t1 - (t_gt + 1)));
    }

    auto extract_slice = [&](std::string_view src) -> std::string {
        std::string raw;
        raw.reserve(src.size() / 2);
        size_t i = 0;
        while (i < src.size()) {
            if (src[i] == '<') {
                if (i + 3 < src.size() && src.substr(i, 4) == "<!--") {
                    const size_t end = src.find("-->", i + 4);
                    i = end == std::string_view::npos ? src.size() : end + 3;
                    continue;
                }
                const size_t gt = src.find('>', i + 1);
                if (gt == std::string_view::npos) break;
                const std::string_view inside = src.substr(i + 1, gt - i - 1);
                size_t k = 0;
                while (k < inside.size() && static_cast<unsigned char>(inside[k]) <= ' ') ++k;
                const bool closing = k < inside.size() && inside[k] == '/';
                if (closing) ++k;
                size_t name_end = k;
                while (name_end < inside.size() &&
                       (std::isalnum(static_cast<unsigned char>(inside[name_end])) || inside[name_end] == '-'))
                    ++name_end;
                const std::string tag = LowerCopy(inside.substr(k, name_end - k));

                if (!closing && (tag == "script" || tag == "style" || tag == "noscript" || tag == "svg" ||
                                 tag == "template" || tag == "iframe" || tag == "head")) {
                    const std::string closer = "</" + tag;
                    const size_t end = FindNoCase(src, closer, gt + 1);
                    if (end == std::string_view::npos) {
                        i = src.size();
                    } else {
                        const size_t end_gt = src.find('>', end + closer.size());
                        i = end_gt == std::string_view::npos ? src.size() : end_gt + 1;
                    }
                    continue;
                }
                if (tag == "br" || tag == "hr") {
                    raw.push_back('\n');
                } else if (!closing && (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4")) {
                    raw += "\n\n";
                    const int lvl = tag[1] - '0';
                    for (int h = 0; h < lvl; ++h) raw.push_back('#');
                    raw.push_back(' ');
                } else if (!closing && tag == "li") {
                    raw += "\n- ";
                } else if (tag == "p" || tag == "div" || tag == "section" || tag == "article" || tag == "main" ||
                           tag == "tr" || tag == "blockquote" || tag == "pre" || tag == "ul" || tag == "ol" ||
                           tag == "table" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
                           tag == "h5" || tag == "h6") {
                    raw.push_back('\n');
                } else if (tag == "td" || tag == "th") {
                    raw += closing ? " | " : " ";
                }
                i = gt + 1;
                continue;
            }
            raw.push_back(src[i++]);
        }

        const std::string decoded = DecodeHtmlEntities(raw);
        std::string clean;
        clean.reserve(decoded.size());
        bool prev_space = false;
        int blank_run = 0;
        for (char c : decoded) {
            if (c == '\r') continue;
            if (c == '\n') {
                while (!clean.empty() && clean.back() == ' ') clean.pop_back();
                prev_space = false;
                if (!clean.empty() && blank_run < 2) {
                    clean.push_back('\n');
                    ++blank_run;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) <= ' ') {
                if (!clean.empty() && clean.back() != '\n') prev_space = true;
                continue;
            }
            if (prev_space) {
                clean.push_back(' ');
                prev_space = false;
            }
            blank_run = 0;
            clean.push_back(c);
        }
        return Trim(clean);
    };

    // Prefer <main> or <article> when it carries substantive content.
    for (const char* region : {"<main", "<article"}) {
        const size_t r0 = FindNoCase(html, region);
        if (r0 == std::string_view::npos) continue;
        const size_t r_gt = html.find('>', r0);
        const std::string closer = std::string("</") + (region + 1) + ">";
        const size_t r1 = r_gt != std::string_view::npos ? FindNoCase(html, closer, r_gt) : std::string_view::npos;
        if (r1 != std::string_view::npos && r1 > r_gt) {
            const std::string slice = extract_slice(html.substr(r_gt + 1, r1 - (r_gt + 1)));
            if (slice.size() >= 200) return TruncateUtf8Chars(slice, max_chars, truncated);
        }
    }
    return TruncateUtf8Chars(extract_slice(html), max_chars, truncated);
}

WebSearchResponse WebSearch(std::string_view query, size_t max_results, WebSearchConfig config,
                            const std::atomic<bool>& cancelled) {
    struct KeyWipe {
        std::wstring& k;
        ~KeyWipe() {
            if (!k.empty()) SecureZeroMemory(k.data(), k.size() * sizeof(wchar_t));
        }
    } wipe{config.api_key};

    const std::string q = Trim(query);
    if (q.empty()) {
        WebSearchResponse r;
        r.error_code = "empty_query";
        r.error_message = "Search query must not be empty.";
        return r;
    }
    const size_t limit = std::clamp<size_t>(max_results, 1, 10);
    const std::string prov = LowerCopy(config.provider);

    if (prov == "tavily") return SearchTavily(q, limit, config, cancelled);
    if (prov == "bocha") return SearchBocha(q, limit, config, cancelled);
    if (prov == "serper") return SearchSerper(q, limit, config, cancelled);
    if (prov == "searxng") return SearchSearxng(q, limit, config, cancelled);
    if (prov == "ddg") return SearchDuckDuckGo(q, limit, cancelled);
    if (prov == "bing") return SearchBing(q, limit, cancelled);

    // "auto": use configured SearXNG endpoint if set, else try DuckDuckGo HTML
    // first for highest relevance and fall back cleanly to Bing HTML.
    if (!config.endpoint.empty()) {
        WebSearchResponse sx = SearchSearxng(q, limit, config, cancelled);
        if (sx.ok || cancelled.load()) return sx;
    }
    WebSearchResponse ddg = SearchDuckDuckGo(q, limit, cancelled);
    if (ddg.ok || cancelled.load()) return ddg;

    WebSearchResponse bing = SearchBing(q, limit, cancelled);
    if (bing.ok) return bing;
    return bing.error_message.empty() ? ddg : bing;
}

WebFetchResponse FetchWebUrl(std::string_view url, size_t max_chars, const std::atomic<bool>& cancelled) {
    WebFetchResponse out;
    const size_t limit = std::clamp<size_t>(max_chars, 500, 24000);
    std::string current = Trim(url);

    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (cancelled.load()) {
            out.error_code = "cancelled";
            out.error_message = "Request cancelled.";
            return out;
        }
        if (!ValidatePublicHttpUrl(current, &out.error_code, &out.error_message)) return out;
        ParsedUrl parsed;
        if (!ParseUrlComponents(current, parsed)) {
            out.error_code = "invalid_url";
            out.error_message = "Invalid URL.";
            return out;
        }
        if (!ResolveAndCheckPublicHost(parsed.whost, out.error_code, out.error_message)) return out;

        const std::wstring headers =
            L"Accept: text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.5\r\n"
            L"Accept-Language: zh-CN,zh;q=0.9,en;q=0.8\r\n";
        const HttpResult http = DoHttpRequest(parsed, L"GET", headers, nullptr, 6000, 12000, false, kMaxResponseBytes,
                                              cancelled);
        if (!http.ok) {
            out.error_code = http.win32_error == ERROR_CANCELLED ? "cancelled" : "network";
            out.error_message = "Failed to fetch URL (Win32 " + std::to_string(http.win32_error) + ").";
            return out;
        }
        out.http_status = http.status;
        if ((http.status == 301 || http.status == 302 || http.status == 303 || http.status == 307 ||
             http.status == 308) &&
            !http.location.empty()) {
            if (hop == kMaxRedirects) {
                out.error_code = "too_many_redirects";
                out.error_message = "Too many HTTP redirects.";
                return out;
            }
            current = ResolveRedirect(parsed, http.location);
            continue;
        }
        out.url = current;
        out.content_type = http.content_type;
        if (http.status < 200 || http.status >= 300) {
            out.error_code = "http_" + std::to_string(http.status);
            out.error_message = "Server returned HTTP " + std::to_string(http.status) + ".";
            return out;
        }
        const std::string ct = LowerCopy(http.content_type);
        const bool is_html = ct.find("html") != std::string::npos;
        const bool is_text = ct.empty() || StartsWithNoCase(ct, "text/") || ct.find("json") != std::string::npos ||
                             ct.find("xml") != std::string::npos || ct.find("javascript") != std::string::npos;
        if (!is_text) {
            out.error_code = "unsupported_content_type";
            out.error_message = "Unsupported non-text content type: " + http.content_type;
            return out;
        }
        const std::string charset = DetectCharset(http.content_type, http.body);
        const std::string utf8 = ConvertToUtf8(http.body, charset);
        if (is_html || (ct.empty() && FindNoCase(utf8.substr(0, (std::min)(utf8.size(), static_cast<size_t>(512))),
                                                 "<html") != std::string_view::npos)) {
            out.text = HtmlToReadableText(utf8, &out.title, limit, &out.truncated);
        } else {
            out.text = TruncateUtf8Chars(Trim(utf8), limit, &out.truncated);
        }
        out.ok = true;
        return out;
    }
    return out;
}

} // namespace pulse::app::ai
