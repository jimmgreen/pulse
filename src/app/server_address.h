// server_address.h - What the user typed to reach a server: \\host\share,
// //host/share, smb://user@host/share, a bare IP, or (in the connect flow) a
// bare host name. Pure string work so the address bar and tests share it; no
// name resolution, no network access.
#pragma once
#include <string>
#include <string_view>

namespace pulse::app {

enum class ServerAddressKind {
    Invalid,      // empty, a local path, or a malformed host
    Server,       // \\host (the share list)
    Share,        // \\host\share[\more]
    Ftp,          // ftp://host[:port][/path] (opened in File Explorer)
    WebDav,       // http(s)://: unc holds the WebClient form, not opened in P1
    Unsupported,  // ftps://, afp://, nfs://, sftp://, ...
};

struct ServerAddress {
    ServerAddressKind kind = ServerAddressKind::Invalid;
    std::wstring unc;     // normalized \\host[\share...] (no trailing separator)
    std::wstring host;    // host as it appears in unc
    std::wstring scheme;  // lower-case scheme for URL input ("smb", "ftp", ...)
    std::wstring user;    // smb:// or ftp:// user@host: dropped password, kept for display/sign-in
    std::wstring url;     // normalized ftp://... URL for Explorer
};

// `allow_bare_host`: "nas" or "nas\share" count as a server (the connect
// flow). The address bar passes false so folder names and aliases keep
// working there; bare IPv4 addresses ("192.168.1.20\share") are always
// accepted.
ServerAddress ParseServerAddress(std::wstring_view text, bool allow_bare_host = false);

// True when the text is clearly meant as a server address in the address bar:
// a URL scheme Pulse knows (smb, cifs, http(s), ftp, ...), or a bare IPv4 host.
// Plain \\host paths are already handled by navigation and return false.
bool IsServerUrlText(std::wstring_view text);

// "\\host\share\a\b" -> "\\host\share"; "\\host\" -> "\\host"; else empty.
// Accepts the \\?\UNC\ prefix.
std::wstring UncShareRoot(std::wstring_view path);

} // namespace pulse::app
