// interactive_scope.h — Where an operation that is not all plain local paths
// can reach, for lane admission (ops_manager_interactive.inl).
//
// Plain local paths are compared by name (interactive_name_reservation.h).
// What cannot be proven by name widens instead of blocking everything: a local
// path with an unprovable name stands for its whole volume, a network path for
// the network (one group: a server reached under two names, or a share and a
// mapped drive of it, is never written twice at once). A network name that is
// this computer (localhost, its own names or addresses, WSL) or anything else
// unknown stands for everything.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winnetwk.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace pulse::ops {

inline std::wstring InteractiveLower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}

// "\\server\share\..." -> "server" (lowercase); empty when not a UNC path.
inline std::wstring InteractiveUncServer(const std::wstring& path) {
    if (path.size() < 3 || path[0] != L'\\' || path[1] != L'\\') return {};
    const size_t end = path.find(L'\\', 2);
    return InteractiveLower(path.substr(2, end == std::wstring::npos ? std::wstring::npos : end - 2));
}

// This computer under a network name: its writes land on local volumes.
inline bool InteractiveIsLocalServer(const std::wstring& server_in) {
    std::wstring server = server_in;
    if (server.size() > 2 && server.front() == L'[' && server.back() == L']') server = server.substr(1, server.size() - 2);
    if (server.empty() || server == L"." || server == L"?" || server == L"localhost" || server == L"::1" ||
        server == L"0:0:0:0:0:0:0:1" || server.starts_with(L"127.") || server.starts_with(L"wsl") ||
        server.ends_with(L".ipv6-literal.net") || server == L"globalroot")
        return true;
    // Own names and addresses, refreshed at most every 30 s.
    static std::mutex mutex;
    static std::set<std::wstring> own;
    static ULONGLONG refreshed = 0;
    std::lock_guard lock(mutex);
    const ULONGLONG now = GetTickCount64();
    if (refreshed == 0 || now - refreshed > 30000) {
        own.clear();
        refreshed = now;
        for (const auto kind : {ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
            wchar_t name[256]{};
            DWORD size = static_cast<DWORD>(std::size(name));
            if (GetComputerNameExW(kind, name, &size) && size) own.insert(InteractiveLower(name));
        }
        ULONG bytes = 16 * 1024;
        std::vector<unsigned char> buffer(bytes);
        auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &bytes);
        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(bytes);
            list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &bytes);
        }
        if (result == NO_ERROR) {
            for (auto* adapter = list; adapter; adapter = adapter->Next)
                for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                    wchar_t text[64]{};
                    const auto* sa = address->Address.lpSockaddr;
                    const void* raw = sa->sa_family == AF_INET
                        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr)
                        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr);
                    if (InetNtopW(sa->sa_family, raw, text, std::size(text))) own.insert(InteractiveLower(text));
                }
        }
    }
    if (own.contains(server)) return true;
    // A short name of this computer's DNS name ("pc" for "pc.lan").
    const auto dot = server.find(L'.');
    return dot != std::wstring::npos && own.contains(server.substr(0, dot));
}

// What a lane's footprint check knows about each drive during one admission.
class InteractiveDriveCache {
public:
    // \\?\Volume{...}\ of a local drive letter; empty when unknown.
    const std::wstring& Volume(wchar_t letter) {
        letter = static_cast<wchar_t>(std::towupper(letter));
        auto [it, added] = volumes_.try_emplace(letter);
        if (added) {
            const wchar_t root[]{letter, L':', L'\\', L'\0'};
            wchar_t volume[MAX_PATH]{};
            if (GetVolumeNameForVolumeMountPointW(root, volume, MAX_PATH)) it->second = InteractiveLower(volume);
        }
        return it->second;
    }
    UINT Type(wchar_t letter) {
        letter = static_cast<wchar_t>(std::towupper(letter));
        auto [it, added] = types_.try_emplace(letter, DRIVE_UNKNOWN);
        if (added) {
            const wchar_t root[]{letter, L':', L'\\', L'\0'};
            it->second = GetDriveTypeW(root);
        }
        return it->second;
    }
    // The server a mapped network drive leads to; empty when unknown.
    const std::wstring& MappedServer(wchar_t letter) {
        letter = static_cast<wchar_t>(std::towupper(letter));
        auto [it, added] = servers_.try_emplace(letter);
        if (added) {
            const wchar_t local[]{letter, L':', L'\0'};
            wchar_t remote[1024]{};
            DWORD size = static_cast<DWORD>(std::size(remote));
            if (WNetGetConnectionW(local, remote, &size) == NO_ERROR) it->second = InteractiveUncServer(remote);
        }
        return it->second;
    }
    // Volume of a footprint parent ("X:\..." or "\\?\Volume{...}\..."); empty when unknown.
    std::wstring VolumeOfParent(const std::wstring& parent) {
        if (parent.size() > 11 && _wcsnicmp(parent.c_str(), L"\\\\?\\Volume{", 11) == 0) {
            const size_t end = parent.find(L'\\', 11);
            return end == std::wstring::npos ? std::wstring() : InteractiveLower(parent.substr(0, end + 1));
        }
        if (parent.size() >= 3 && parent[1] == L':' && parent[2] == L'\\') return Volume(parent[0]);
        return {};
    }

private:
    std::map<wchar_t, std::wstring> volumes_, servers_;
    std::map<wchar_t, UINT> types_;
};

}  // namespace pulse::ops
