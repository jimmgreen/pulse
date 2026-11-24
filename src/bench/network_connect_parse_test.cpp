// Focused tests for the network page's pure pieces: server address parsing,
// the recent-visit list and LAN device merging.
#include "../app/lan_device.h"
#include "../app/network_recent.h"
#include "../app/server_address.h"
#include "../fs/wsl_path.h"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const char* what) {
    if (ok) ++g_passes;
    else { ++g_failures; std::printf("FAIL: %s\n", what); }
}

using pulse::app::ParseServerAddress;
using pulse::app::ServerAddressKind;

void Expect(const wchar_t* text, ServerAddressKind kind, const wchar_t* unc, const char* what, bool bare = false) {
    const auto parsed = ParseServerAddress(text, bare);
    Check(parsed.kind == kind && parsed.unc == unc, what);
}

void ParseTests() {
    using K = ServerAddressKind;
    Expect(L"\\\\ds920\\share", K::Share, L"\\\\ds920\\share", "unc share");
    Expect(L"  \\\\ds920\\share\\  ", K::Share, L"\\\\ds920\\share", "trim and trailing separator");
    Expect(L"\\\\ds920", K::Server, L"\\\\ds920", "unc server");
    Expect(L"//ds920/share/a", K::Share, L"\\\\ds920\\share\\a", "forward slashes");
    Expect(L"\\\\?\\UNC\\ds920\\share", K::Share, L"\\\\ds920\\share", "long unc prefix");
    Expect(L"\\\\ds920\\\\share", K::Share, L"\\\\ds920\\share", "duplicate separators");
    Expect(L"smb://ds920.local/共享资料", K::Share, L"\\\\ds920.local\\共享资料", "smb url");
    Expect(L"SMB://ds920/%E5%85%B1%E4%BA%AB", K::Share, L"\\\\ds920\\共享", "smb url percent-decoded utf-8");
    Expect(L"smb://ds920:445/s", K::Share, L"\\\\ds920\\s", "smb default port");
    Expect(L"smb://ds920:8445/s", K::Invalid, L"", "smb custom port cannot be unc");
    Expect(L"smb://ds920", K::Server, L"\\\\ds920", "smb server only");
    Expect(L"smb:///share", K::Invalid, L"", "smb without host");
    Expect(L"192.168.1.20", K::Server, L"\\\\192.168.1.20", "bare ipv4");
    Expect(L"192.168.1.20\\share", K::Share, L"\\\\192.168.1.20\\share", "bare ipv4 with share");
    Expect(L"192.168.1.256", K::Invalid, L"", "not an ipv4");
    Expect(L"nas", K::Invalid, L"", "bare host needs connect flow");
    Expect(L"nas", K::Server, L"\\\\nas", "bare host in connect flow", true);
    Expect(L"nas/share", K::Share, L"\\\\nas\\share", "bare host share in connect flow", true);
    Expect(L"C:\\Windows", K::Invalid, L"", "local path", true);
    Expect(L"shell:Downloads", K::Invalid, L"", "shell path", true);
    Expect(L"\\\\?\\C:\\x", K::Invalid, L"", "device path");
    Expect(L"\\\\ds 920\\s", K::Invalid, L"", "space in host");
    Expect(L"\\\\ds920\\s\\..\\x", K::Invalid, L"", "dot-dot segment");
    Expect(L"\\\\[fe80::1%3]\\s", K::Share, L"\\\\fe80--1s3.ipv6-literal.net\\s", "ipv6 literal");
    Expect(L"https://dav.example.com/files", K::WebDav, L"\\\\dav.example.com@SSL\\files", "webdav https");
    Expect(L"http://nas:5005", K::WebDav, L"\\\\nas@5005\\DavWWWRoot", "webdav http port");
    Expect(L"", K::Invalid, L"", "empty");

    const auto user = ParseServerAddress(L"smb://alice:secret@ds920/home");
    Check(user.kind == K::Share && user.unc == L"\\\\ds920\\home" && user.user == L"alice", "user kept, password dropped");
    const auto ftp = ParseServerAddress(L"ftp://files.example.com/pub");
    Check(ftp.kind == K::Ftp && ftp.scheme == L"ftp" && ftp.host == L"files.example.com" &&
          ftp.url == L"ftp://files.example.com/pub", "ftp routed for explorer");
    const auto ftp_root = ParseServerAddress(L"FTP://192.168.1.20");
    Check(ftp_root.kind == K::Ftp && ftp_root.url == L"ftp://192.168.1.20/", "ftp root normalized");
    const auto ftp_user = ParseServerAddress(L"ftp://alice:secret@nas.local:2121/pub,1/a b");
    Check(ftp_user.kind == K::Ftp && ftp_user.user == L"alice" && ftp_user.host == L"nas.local" &&
          ftp_user.url == L"ftp://alice:secret@nas.local:2121/pub%2C1/a%20b",
          "ftp encodes commas and spaces in explorer url");
    const auto ftp_v6 = ParseServerAddress(L"ftp://[::1]:2121/dir");
    Check(ftp_v6.kind == K::Ftp && ftp_v6.host == L"[::1]" && ftp_v6.url == L"ftp://[::1]:2121/dir",
          "ftp ipv6 host and port");
    Check(ParseServerAddress(L"ftp://").kind == K::Invalid, "empty ftp host invalid");
    Check(ParseServerAddress(L"ftp:///pub").kind == K::Invalid, "missing ftp host invalid");
    Check(ParseServerAddress(L"ftp://host:0/x").kind == K::Invalid, "ftp port 0 invalid");
    Check(ParseServerAddress(L"ftp://host:70000/x").kind == K::Invalid, "ftp port > 65535 invalid");
    const auto ftps = ParseServerAddress(L"ftps://files.example.com/pub");
    Check(ftps.kind == K::Unsupported && ftps.scheme == L"ftps", "ftps stays unsupported");
    const auto sftp = ParseServerAddress(L"sftp://files.example.com/pub");
    Check(sftp.kind == K::Unsupported && sftp.scheme == L"sftp", "sftp stays unsupported");
    Check(ParseServerAddress(L"mailto://x").kind == K::Invalid, "unknown scheme is not a server");

    Check(pulse::app::IsServerUrlText(L"smb://x/y"), "url text smb");
    Check(pulse::app::IsServerUrlText(L"ftp://files.example.com/pub"), "url text ftp");
    Check(pulse::app::IsServerUrlText(L"10.0.0.2\\share"), "url text ipv4");
    Check(!pulse::app::IsServerUrlText(L"\\\\x\\y"), "plain unc handled elsewhere");
    Check(!pulse::app::IsServerUrlText(L"Downloads"), "folder name");
    Check(!pulse::app::IsServerUrlText(L"https://example.com"), "web url stays a web url");

    Check(pulse::app::UncShareRoot(L"\\\\h\\s\\a\\b") == L"\\\\h\\s", "share root deep");
    Check(pulse::app::UncShareRoot(L"\\\\h\\") == L"\\\\h", "server root");
    Check(pulse::app::UncShareRoot(L"\\\\?\\UNC\\h\\s\\a") == L"\\\\h\\s", "share root long prefix");
    Check(pulse::app::UncShareRoot(L"C:\\a").empty(), "share root local");
}

void RecentTests() {
    pulse::app::NetworkRecentList list;
    Check(!list.Touch(L"C:\\x", 1), "local path not recorded");
    Check(list.Touch(L"\\\\ds920\\share\\deep\\folder", 10), "deep path recorded");
    Check(list.Items().size() == 1 && list.Items()[0].path == L"\\\\ds920\\share", "recorded as share root");
    list.Touch(L"\\\\other", 20);
    list.Touch(L"\\\\DS920\\SHARE", 30);
    Check(list.Items().size() == 2 && list.Items()[0].last_visit == 30, "dedupe moves to front");
    for (int i = 0; i < 30; ++i) list.Touch(L"\\\\h" + std::to_wstring(i), 100 + static_cast<uint64_t>(i));
    Check(list.Items().size() == pulse::app::NetworkRecentList::kCap, "capped");
    Check(list.Items()[0].path == L"\\\\h29", "newest first");

    pulse::app::NetworkRecentList round;
    Check(round.FromJson(list.ToJson()), "round trip parses");
    Check(round.Items().size() == list.Items().size() && round.Items()[0].path == L"\\\\h29" &&
          round.Items()[0].last_visit == 129, "round trip keeps order and time");
    Check(round.Remove(L"\\\\H29\\"), "remove is case and separator insensitive");
    Check(round.Items()[0].path == L"\\\\h28", "removed");

    pulse::app::NetworkRecentList bad;
    Check(!bad.FromJson(L"not json"), "garbage rejected");
    Check(bad.FromJson(L"{\"items\":[{\"path\":\"C:\\\\x\"},{\"path\":\"\\\\\\\\a\\\\b\",\"t\":\"5\"},{\"path\":\"\\\\\\\\A\\\\B\"}]}"),
          "mixed list parses");
    Check(bad.Items().size() == 1 && bad.Items()[0].last_visit == 5, "bad and duplicate members skipped");
}

void LanTests() {
    using pulse::app::LanDevice;
    Check(pulse::app::MdnsInstanceName(L"DS920._smb._tcp.local") == L"DS920", "instance name");
    Check(pulse::app::MdnsInstanceName(L"Mac\\032mini._smb._tcp.local") == L"Mac mini", "escaped space");
    Check(pulse::app::MdnsInstanceName(L"a\\.b._smb._tcp.local") == L"a.b", "escaped dot");
    Check(pulse::app::TxtValue({L"vers=1", L"MODEL=Xserve"}, L"model") == L"Xserve", "txt value");
    Check(pulse::app::TxtValue({L"modelx=1"}, L"model").empty(), "txt key must match fully");

    std::vector<LanDevice> list;
    Check(pulse::app::MergeLanDevice(list, LanDevice{L"DS920", L"ds920.local", L"", L"", pulse::app::kLanSourceMdns}), "add mdns");
    Check(pulse::app::MergeLanDevice(list, LanDevice{L"", L"DS920", L"192.168.1.20", L"DS920+", pulse::app::kLanSourceWsd}),
          "wsd answer merges by host");
    Check(list.size() == 1 && list[0].ip == L"192.168.1.20" && list[0].model == L"DS920+" &&
          list[0].sources == (pulse::app::kLanSourceMdns | pulse::app::kLanSourceWsd), "merged fields");
    Check(!pulse::app::MergeLanDevice(list, LanDevice{L"DS920", L"", L"192.168.1.20", L"", pulse::app::kLanSourceWsd}),
          "same answer again changes nothing");
    Check(pulse::app::MergeLanDevice(list, LanDevice{L"", L"", L"192.168.1.30", L"", pulse::app::kLanSourceWsd}), "ip-only device");
    Check(list.size() == 2 && list[1].name == L"192.168.1.30", "ip-only gets ip as name");
    Check(pulse::app::LanDeviceUnc(list[0]) == L"\\\\ds920.local", "unc prefers host");
    Check(pulse::app::LanDeviceUnc(list[1]) == L"\\\\192.168.1.30", "unc falls back to ip");
    Check(!pulse::app::MergeLanDevice(list, LanDevice{}), "empty answer ignored");

    using pulse::app::LanDeviceKind;
    Check(LanDeviceKind(LanDevice{L"DS920", L"ds920.local", L"", L"DS920+", 1}) == pulse::app::kLanKindNas, "synology model");
    Check(LanDeviceKind(LanDevice{L"Storage", L"", L"", L"TS-453D", 2}) == pulse::app::kLanKindNas, "qnap model");
    Check(LanDeviceKind(LanDevice{L"truenas", L"truenas.local", L"", L"", 1}) == pulse::app::kLanKindNas, "nas by name");
    Check(LanDeviceKind(LanDevice{L"Home-NAS", L"", L"192.168.1.5", L"", 2}) == pulse::app::kLanKindNas, "nas word");
    Check(LanDeviceKind(LanDevice{L"Mac mini", L"mac-mini.local", L"", L"Macmini9,1", 1}) == pulse::app::kLanKindMac, "mac model");
    Check(LanDeviceKind(LanDevice{L"Studio", L"", L"", L"iMac21,1", 1}) == pulse::app::kLanKindMac, "imac model");
    Check(LanDeviceKind(LanDevice{L"DESKTOP-7", L"DESKTOP-7", L"", L"", 2}) == pulse::app::kLanKindPc, "windows pc");
    Check(LanDeviceKind(LanDevice{L"dsp", L"", L"", L"DSP", 2}) == pulse::app::kLanKindPc, "ds without digit is not a nas");
}
void WslTests() {
    using pulse::fs::IsWslPath;
    Check(IsWslPath(L"\\\\wsl$\\Ubuntu\\home"), "wsl$ share");
    Check(IsWslPath(L"\\\\WSL.LocalHost\\Debian"), "wsl.localhost, any case");
    Check(IsWslPath(L"\\\\?\\UNC\\wsl.localhost\\Ubuntu"), "long UNC form");
    Check(IsWslPath(L"//wsl$/Ubuntu/etc"), "forward slashes");
    Check(IsWslPath(L"\\\\wsl$"), "bare server");
    Check(!IsWslPath(L"\\\\wsl\\share"), "a server named wsl is a server");
    Check(!IsWslPath(L"\\\\wsl.localhost.example\\x"), "longer host name");
    Check(!IsWslPath(L"\\\\ds920\\wsl$"), "share named wsl$");
    Check(!IsWslPath(L"C:\\wsl$"), "local folder");
    Check(!IsWslPath(L""), "empty");
}
}  // namespace

int main() {
    ParseTests();
    RecentTests();
    LanTests();
    WslTests();
    std::printf("network_connect_parse_test: %d passed, %d failed\n", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
