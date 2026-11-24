#include "server_address.h"

#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace pulse::app {
namespace {

std::wstring Trim(std::wstring_view text) {
    size_t first = 0, last = text.size();
    while (first < last && (iswspace(text[first]) || text[first] == L'"')) ++first;
    while (last > first && (iswspace(text[last - 1]) || text[last - 1] == L'"')) --last;
    return std::wstring(text.substr(first, last - first));
}

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// %XX sequences are UTF-8 bytes; anything malformed is kept literally.
std::wstring PercentDecode(const std::wstring& text) {
    std::string bytes;
    std::wstring out;
    const auto flush = [&] {
        if (bytes.empty()) return;
        const int n = static_cast<int>(bytes.size());
        std::wstring wide(static_cast<size_t>(n), L'\0');
        int written = 0;
        for (int i = 0; i < n;) {  // minimal UTF-8 decoder (no Win32 so tests stay pure)
            const unsigned char b = static_cast<unsigned char>(bytes[static_cast<size_t>(i)]);
            uint32_t cp = 0xFFFD;
            int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
            if (i + len > n) len = 1;
            if (len == 1) cp = b < 0x80 ? b : 0xFFFD;
            else {
                cp = b & (0xFF >> (len + 1));
                bool ok = true;
                for (int k = 1; k < len; ++k) {
                    const unsigned char c = static_cast<unsigned char>(bytes[static_cast<size_t>(i + k)]);
                    if ((c & 0xC0) != 0x80) { ok = false; break; }
                    cp = (cp << 6) | (c & 0x3F);
                }
                if (!ok) { cp = 0xFFFD; len = 1; }
            }
            i += len;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                wide[static_cast<size_t>(written++)] = static_cast<wchar_t>(0xD800 + (cp >> 10));
                if (static_cast<size_t>(written) >= wide.size()) wide.resize(wide.size() + 1);
                wide[static_cast<size_t>(written++)] = static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            } else {
                wide[static_cast<size_t>(written++)] = static_cast<wchar_t>(cp);
            }
        }
        wide.resize(static_cast<size_t>(written));
        out += wide;
        bytes.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'%' && i + 2 < text.size() && HexValue(text[i + 1]) >= 0 && HexValue(text[i + 2]) >= 0) {
            bytes.push_back(static_cast<char>(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
            i += 2;
            continue;
        }
        flush();
        out += text[i];
    }
    flush();
    return out;
}

bool IsIPv4(std::wstring_view host) {
    int parts = 0, digits = 0, value = 0;
    for (wchar_t c : host) {
        if (c == L'.') {
            if (!digits) return false;
            ++parts; digits = 0; value = 0;
            continue;
        }
        if (c < L'0' || c > L'9' || ++digits > 3) return false;
        value = value * 10 + (c - L'0');
        if (value > 255) return false;
    }
    return digits && parts == 3;
}

// Host names: letters, digits, '-', '_', '.', and non-ASCII (NetBIOS names
// can be CJK). Anything with path or URL punctuation is not a host.
bool ValidHost(std::wstring_view host) {
    if (host.empty() || host.size() > 253 || host.front() == L'.' || host.back() == L'.') return false;
    for (wchar_t c : host) {
        if (c >= 0x80) continue;
        if (iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'@') continue;
        return false;
    }
    return true;
}

// [fe80::1%3] -> fe80--1s3.ipv6-literal.net (the UNC form Windows accepts).
bool Ipv6Literal(std::wstring& host) {
    if (host.size() < 3 || host.front() != L'[' || host.back() != L']') return false;
    std::wstring inner = host.substr(1, host.size() - 2);
    for (auto& c : inner) {
        if (c == L':') c = L'-';
        else if (c == L'%') c = L's';
        else if (!iswxdigit(c) && c != L'.') return false;
    }
    host = inner + L".ipv6-literal.net";
    return true;
}

// Splits "host/share/more" (separators already '\\') into the result.
ServerAddress FromParts(std::wstring host, const std::wstring& rest) {
    ServerAddress out;
    if (!Ipv6Literal(host)) {
        // smb://host:445/ -> host; other ports cannot be expressed in UNC.
        if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
            const std::wstring port = host.substr(colon + 1);
            if (port != L"445" && port != L"139") return out;
            host.resize(colon);
        }
    }
    if (!ValidHost(host)) return out;
    std::vector<std::wstring> parts;
    std::wstring part;
    for (wchar_t c : rest) {
        if (c == L'\\') {
            if (!part.empty()) parts.push_back(std::move(part));
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(std::move(part));
    for (const auto& p : parts) {
        if (p == L"." || p == L"..") return out;
        if (p.find_first_of(L"<>:\"|?*") != std::wstring::npos) return out;
    }
    out.host = host;
    out.unc = L"\\\\" + host;
    for (const auto& p : parts) out.unc += L"\\" + p;
    out.kind = parts.empty() ? ServerAddressKind::Server : ServerAddressKind::Share;
    return out;
}

void SplitHost(const std::wstring& path, std::wstring& host, std::wstring& rest) {
    const size_t cut = path.find(L'\\');
    host = path.substr(0, cut);
    rest = cut == std::wstring::npos ? std::wstring() : path.substr(cut);
}

std::wstring Slashes(std::wstring text) {
    for (auto& c : text) if (c == L'/') c = L'\\';
    return text;
}

constexpr std::wstring_view kUnsupported[] = {L"ftps", L"sftp", L"afp", L"nfs", L"ssh", L"webdav", L"dav"};

bool ValidPort(std::wstring_view port) {
    if (port.empty() || port.size() > 5) return false;
    uint32_t value = 0;
    for (wchar_t c : port) {
        if (c < L'0' || c > L'9') return false;
        value = value * 10u + static_cast<uint32_t>(c - L'0');
    }
    return value >= 1u && value <= 65535u;
}

std::wstring EncodeExplorerUrlPart(std::wstring_view s) {
    std::wstring encoded;
    encoded.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L' ') encoded += L"%20";
        else if (c == L',') encoded += L"%2C";
        else if (c == L'"') encoded += L"%22";
        else encoded += c;
    }
    return encoded;
}

}  // namespace

ServerAddress ParseServerAddress(std::wstring_view input, bool allow_bare_host) {
    std::wstring text = Trim(input);
    ServerAddress out;
    if (text.empty()) return out;
    for (wchar_t c : text) if (c < 0x20 || c == L'"') return out;

    if (const size_t sep = text.find(L"://"); sep != std::wstring::npos && sep > 0) {
        const std::wstring scheme = Lower(text.substr(0, sep));
        for (wchar_t c : scheme) if (!iswalnum(c) && c != L'+' && c != L'-' && c != L'.') return out;
        std::wstring rest = text.substr(sep + 3);
        if (const size_t cut = rest.find_first_of(L"?#"); cut != std::wstring::npos) rest.resize(cut);
        if (scheme == L"ftp") {
            const size_t slash = rest.find_first_of(L"/\\");
            const std::wstring raw_authority = slash == std::wstring::npos ? rest : rest.substr(0, slash);
            std::wstring raw_path = slash == std::wstring::npos ? std::wstring() : rest.substr(slash);
            std::wstring raw_userinfo, host_port = raw_authority, user;
            bool has_userinfo = false;
            if (const size_t at = raw_authority.rfind(L'@'); at != std::wstring::npos) {
                has_userinfo = true;
                raw_userinfo = raw_authority.substr(0, at);
                if (raw_userinfo.empty() || raw_userinfo.find_first_of(L"<>|") != std::wstring::npos) return out;
                user = PercentDecode(raw_userinfo);
                if (const size_t colon = user.find(L':'); colon != std::wstring::npos) user.resize(colon);
                host_port = raw_authority.substr(at + 1);
            }
            if (host_port.empty()) return out;
            std::wstring host_only, port;
            if (host_port.front() == L'[') {
                const size_t close = host_port.find(L']');
                if (close == std::wstring::npos) return out;
                const std::wstring bracketed = host_port.substr(0, close + 1);
                std::wstring v6 = bracketed;
                if (!Ipv6Literal(v6)) return out;
                host_only = bracketed;
                if (close + 1 < host_port.size()) {
                    if (host_port[close + 1] != L':') return out;
                    port = host_port.substr(close + 2);
                    if (!ValidPort(port)) return out;
                }
            } else {
                const size_t colon = host_port.find(L':');
                const std::wstring raw_host = colon == std::wstring::npos ? host_port : host_port.substr(0, colon);
                if (colon != std::wstring::npos) {
                    port = host_port.substr(colon + 1);
                    if (!ValidPort(port)) return out;
                }
                host_only = PercentDecode(raw_host);
                if (!ValidHost(host_only) || host_only.find(L'@') != std::wstring::npos) return out;
            }
            if (raw_path.find_first_of(L"<>|\"") != std::wstring::npos) return out;
            for (auto& c : raw_path) if (c == L'\\') c = L'/';
            if (raw_path.empty()) raw_path = L"/";
            out.kind = ServerAddressKind::Ftp;
            out.host = host_only;
            out.scheme = L"ftp";
            out.user = std::move(user);
            out.url = L"ftp://" + (has_userinfo ? EncodeExplorerUrlPart(raw_userinfo) + L"@" : std::wstring()) +
                host_only + (port.empty() ? std::wstring() : L":" + port) + EncodeExplorerUrlPart(raw_path);
            return out;
        }
        const size_t slash = rest.find(L'/');
        std::wstring authority = rest.substr(0, slash);
        std::wstring path = slash == std::wstring::npos ? std::wstring() : rest.substr(slash);
        std::wstring user;
        if (const size_t at = authority.rfind(L'@'); at != std::wstring::npos) {
            user = PercentDecode(authority.substr(0, at));
            if (const size_t colon = user.find(L':'); colon != std::wstring::npos) user.resize(colon);  // never keep a password
            authority = authority.substr(at + 1);
        }
        authority = PercentDecode(authority);
        path = Slashes(PercentDecode(path));
        if (scheme == L"smb" || scheme == L"cifs") {
            out = FromParts(authority, path);
            if (out.kind != ServerAddressKind::Invalid) out.user = user;
        } else if (scheme == L"http" || scheme == L"https") {
            std::wstring host = authority, port;
            if (host.empty() || host.front() != L'[') {
                if (const size_t colon = host.find(L':'); colon != std::wstring::npos) {
                    port = host.substr(colon + 1);
                    host.resize(colon);
                }
            }
            for (wchar_t c : port) if (c < L'0' || c > L'9') return ServerAddress{};
            ServerAddress parts = FromParts(host, path);
            if (parts.kind == ServerAddressKind::Invalid) return parts;
            std::wstring tail = parts.unc.substr(2 + parts.host.size());
            std::wstring decorated = parts.host;
            if (scheme == L"https") decorated += L"@SSL";
            if (!port.empty() && port != (scheme == L"https" ? L"443" : L"80")) decorated += L"@" + port;
            out.kind = ServerAddressKind::WebDav;
            out.host = parts.host;
            out.unc = L"\\\\" + decorated + (tail.empty() ? std::wstring(L"\\DavWWWRoot") : tail);
            out.user = user;
        } else {
            bool known = false;
            for (auto s : kUnsupported) known |= scheme == s;
            if (!known) return out;  // mailto:, file:, ... are not servers
            out.kind = ServerAddressKind::Unsupported;
            out.host = authority;
        }
        out.scheme = scheme;
        return out;
    }

    text = Slashes(std::move(text));
    if (text.size() >= 8 && Lower(text.substr(0, 8)) == L"\\\\?\\unc\\") text = L"\\\\" + text.substr(8);
    if (text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\') {
        if (text.size() >= 3 && text[2] == L'?') return out;  // \\?\C:\ and other device paths
        std::wstring host, rest;
        SplitHost(text.substr(2), host, rest);
        return FromParts(host, rest);
    }
    if (text.front() == L'\\') return out;
    if (text.size() >= 2 && text[1] == L':') return out;  // C:\..., shell:...
    std::wstring host, rest;
    SplitHost(text, host, rest);
    if (IsIPv4(host) || (allow_bare_host && ValidHost(host) && host.find(L'@') == std::wstring::npos))
        return FromParts(host, rest);
    return out;
}

bool IsServerUrlText(std::wstring_view input) {
    const std::wstring text = Trim(input);
    if (const size_t sep = text.find(L"://"); sep != std::wstring::npos && sep > 0) {
        const std::wstring scheme = Lower(text.substr(0, sep));
        if (scheme == L"smb" || scheme == L"cifs" || scheme == L"ftp") return true;
        for (auto s : kUnsupported) if (scheme == s) return true;
        return false;
    }
    const std::wstring slashed = Slashes(text);
    const size_t cut = slashed.find(L'\\');
    return IsIPv4(std::wstring_view(slashed).substr(0, cut));
}

std::wstring UncShareRoot(std::wstring_view input) {
    std::wstring path = Slashes(std::wstring(input));
    if (path.size() >= 8 && Lower(path.substr(0, 8)) == L"\\\\?\\unc\\") path = L"\\\\" + path.substr(8);
    if (path.size() < 3 || path[0] != L'\\' || path[1] != L'\\' || path[2] == L'\\' || path[2] == L'?') return {};
    const size_t host_end = path.find(L'\\', 2);
    if (host_end == std::wstring::npos) return path;
    const size_t share_end = path.find(L'\\', host_end + 1);
    std::wstring root = path.substr(0, share_end);
    while (root.size() > 2 && root.back() == L'\\') root.pop_back();
    return root;
}

} // namespace pulse::app
