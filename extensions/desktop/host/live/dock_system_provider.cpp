// dock_system_provider.cpp - Live Windows system status & tray provider for Liquid Glass Dock .dk-sys.
#include "host/live/dock_system_provider.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <propsys.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <imm.h>
#include <powrprof.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <dwmapi.h>
#include <wlanapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace pulse::desktop::live {
namespace {

constexpr UINT kImcGetConversionMode = 0x0001;
constexpr UINT kImcSetConversionMode = 0x0002;
constexpr UINT kImcGetOpenStatus = 0x0005;
constexpr UINT kImcSetOpenStatus = 0x0006;

// Inline kPkeyDeviceFriendlyName & standard Windows power scheme GUIDs (zero initguid linkage dependency).
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr GUID kGuidPowerSaver = {
    0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
constexpr GUID kGuidBalanced = {
    0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr GUID kGuidHighPerf = {
    0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};

// Standard Windows undocumented-but-stable IPolicyConfig interface used by audio switchers on Win10/11.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

constexpr CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring BaseNameNoExt(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

std::wstring QueryExePathForPid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = ARRAYSIZE(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) {
        out.assign(buf, len);
    }
    CloseHandle(proc);
    return out;
}

uint32_t HashAccentForKey(const std::wstring& key) noexcept {
    static constexpr uint32_t kPalette[] = {
        0xFF0A84FFu, 0xFF30D158u, 0xFFFF453Au, 0xFFBF5AF2u,
        0xFFFF9F0Au, 0xFF64D2FFu, 0xFF5E5CE6u, 0xFFFF375Fu,
    };
    uint32_t h = 2166136261u;
    for (wchar_t ch : key) {
        h ^= static_cast<uint32_t>(ch);
        h *= 16777619u;
    }
    return kPalette[h % std::size(kPalette)];
}

int ParseUnreadBadge(const std::wstring& text) noexcept {
    const size_t open = text.find_last_of(L"(（[");
    if (open == std::wstring::npos || open + 1 >= text.size()) return 0;
    int val = 0;
    bool any = false;
    for (size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] >= L'0' && text[i] <= L'9') {
            val = val * 10 + (text[i] - L'0');
            any = true;
            if (val > 999) return 999;
        } else if (text[i] == L')' || text[i] == L'）' || text[i] == L']' || text[i] == L'+') {
            break;
        } else {
            return 0;
        }
    }
    return any ? val : 0;
}

// ---------------------------------------------------------------------------
// 1. Core Audio (Master Volume, Mute, Output Device List)
// ---------------------------------------------------------------------------
bool QueryCoreAudio(bool include_devices,
                    bool& out_available,
                    int& out_vol_pct,
                    bool& out_muted,
                    std::wstring& out_default_name,
                    std::vector<DockAudioDevice>& out_devices) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        out_available = false;
        return false;
    }
    ComPtr<IMMDevice> def_dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def_dev)) || !def_dev) {
        out_available = false;
        return false;
    }
    out_available = true;

    std::wstring def_id;
    LPWSTR raw_def_id = nullptr;
    if (SUCCEEDED(def_dev->GetId(&raw_def_id)) && raw_def_id) {
        def_id = raw_def_id;
        CoTaskMemFree(raw_def_id);
    }

    auto read_friendly_name = [](IMMDevice* dev) -> std::wstring {
        if (!dev) return {};
        ComPtr<IPropertyStore> store;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
        PROPVARIANT pv{};
        PropVariantInit(&pv);
        std::wstring res;
        if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) res = pv.pwszVal;
        }
        PropVariantClear(&pv);
        return res;
    };
    out_default_name = read_friendly_name(def_dev.Get());

    ComPtr<IAudioEndpointVolume> epv;
    if (SUCCEEDED(def_dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(epv.GetAddressOf()))) &&
        epv) {
        float scalar = 0.5f;
        BOOL is_muted = FALSE;
        if (SUCCEEDED(epv->GetMasterVolumeLevelScalar(&scalar))) {
            out_vol_pct = std::clamp(static_cast<int>(std::lround(scalar * 100.0f)), 0, 100);
        }
        if (SUCCEEDED(epv->GetMute(&is_muted))) {
            out_muted = (is_muted != FALSE);
        }
    }

    if (include_devices) {
        out_devices.clear();
        ComPtr<IMMDeviceCollection> col;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT count = 0;
            col->GetCount(&count);
            for (UINT i = 0; i < count && out_devices.size() < 8; ++i) {
                ComPtr<IMMDevice> dev;
                if (FAILED(col->Item(i, &dev)) || !dev) continue;
                LPWSTR id_w = nullptr;
                std::wstring id;
                if (SUCCEEDED(dev->GetId(&id_w)) && id_w) {
                    id = id_w;
                    CoTaskMemFree(id_w);
                }
                std::wstring name = read_friendly_name(dev.Get());
                if (name.empty()) continue;
                out_devices.push_back({id, name, !def_id.empty() && id == def_id});
            }
        }
        if (out_devices.empty() && !out_default_name.empty()) {
            out_devices.push_back({def_id, out_default_name, true});
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2. Dynamic wlanapi.dll (Wi-Fi state + available SSIDs + connect/disconnect)
// ---------------------------------------------------------------------------
struct WlanApi {
    HMODULE mod = nullptr;
    decltype(&WlanOpenHandle) open_handle = nullptr;
    decltype(&WlanCloseHandle) close_handle = nullptr;
    decltype(&WlanEnumInterfaces) enum_interfaces = nullptr;
    decltype(&WlanQueryInterface) query_interface = nullptr;
    decltype(&WlanGetAvailableNetworkList) get_networks = nullptr;
    decltype(&WlanConnect) connect = nullptr;
    decltype(&WlanDisconnect) disconnect = nullptr;
    decltype(&WlanFreeMemory) free_mem = nullptr;

    WlanApi() {
        mod = LoadLibraryW(L"wlanapi.dll");
        if (!mod) return;
        open_handle = reinterpret_cast<decltype(&WlanOpenHandle)>(GetProcAddress(mod, "WlanOpenHandle"));
        close_handle = reinterpret_cast<decltype(&WlanCloseHandle)>(GetProcAddress(mod, "WlanCloseHandle"));
        enum_interfaces = reinterpret_cast<decltype(&WlanEnumInterfaces)>(GetProcAddress(mod, "WlanEnumInterfaces"));
        query_interface = reinterpret_cast<decltype(&WlanQueryInterface)>(GetProcAddress(mod, "WlanQueryInterface"));
        get_networks = reinterpret_cast<decltype(&WlanGetAvailableNetworkList)>(GetProcAddress(mod, "WlanGetAvailableNetworkList"));
        connect = reinterpret_cast<decltype(&WlanConnect)>(GetProcAddress(mod, "WlanConnect"));
        disconnect = reinterpret_cast<decltype(&WlanDisconnect)>(GetProcAddress(mod, "WlanDisconnect"));
        free_mem = reinterpret_cast<decltype(&WlanFreeMemory)>(GetProcAddress(mod, "WlanFreeMemory"));
    }
    bool Ready() const noexcept {
        return open_handle && close_handle && enum_interfaces && query_interface && get_networks && free_mem;
    }
};

const WlanApi& GetWlanApi() {
    static const WlanApi api;
    return api;
}

std::wstring SsidToWide(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) return {};
    const char* raw = reinterpret_cast<const char*>(ssid.ucSSID);
    const int n = MultiByteToWideChar(CP_UTF8, 0, raw, static_cast<int>(ssid.uSSIDLength), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw, static_cast<int>(ssid.uSSIDLength), out.data(), n);
    return out;
}

int SignalToBars(int pct) noexcept {
    if (pct <= 0) return 0;
    if (pct < 38) return 1;
    if (pct < 72) return 2;
    return 3;
}

void QueryWifiState(DockSystemSnapshot& snap) {
    const auto& api = GetWlanApi();
    snap.wifi_available = false;
    snap.wifi_enabled = false;
    snap.wifi_connected = false;
    snap.wifi_ssid.clear();
    snap.wifi_bars = 0;
    snap.wifi_signal_pct = 0;
    snap.wifi_networks.clear();
    if (!api.Ready()) return;

    DWORD negotiated = 0;
    HANDLE client = nullptr;
    if (api.open_handle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS || !client) return;

    PWLAN_INTERFACE_INFO_LIST ilist = nullptr;
    if (api.enum_interfaces(client, nullptr, &ilist) == ERROR_SUCCESS && ilist && ilist->dwNumberOfItems > 0) {
        snap.wifi_available = true;
        // Prefer connected interface, otherwise first interface.
        DWORD chosen = 0;
        for (DWORD i = 0; i < ilist->dwNumberOfItems; ++i) {
            if (ilist->InterfaceInfo[i].isState == wlan_interface_state_connected) {
                chosen = i;
                break;
            }
        }
        const auto& iface = ilist->InterfaceInfo[chosen];
        snap.wifi_enabled = (iface.isState != wlan_interface_state_not_ready);

        // Query radio state if available
        DWORD data_sz = 0;
        PVOID data_ptr = nullptr;
        WLAN_OPCODE_VALUE_TYPE op_type{};
        if (api.query_interface(client, &iface.InterfaceGuid, wlan_intf_opcode_radio_state,
                                nullptr, &data_sz, &data_ptr, &op_type) == ERROR_SUCCESS && data_ptr) {
            const auto* rs = static_cast<const WLAN_RADIO_STATE*>(data_ptr);
            bool any_on = false;
            for (DWORD r = 0; r < rs->dwNumberOfPhys && r < WLAN_MAX_PHY_INDEX; ++r) {
                if (rs->PhyRadioState[r].dot11SoftwareRadioState == dot11_radio_state_on &&
                    rs->PhyRadioState[r].dot11HardwareRadioState == dot11_radio_state_on) {
                    any_on = true;
                }
            }
            snap.wifi_enabled = any_on;
            api.free_mem(data_ptr);
        }

        if (iface.isState == wlan_interface_state_connected) {
            data_sz = 0;
            data_ptr = nullptr;
            if (api.query_interface(client, &iface.InterfaceGuid, wlan_intf_opcode_current_connection,
                                    nullptr, &data_sz, &data_ptr, &op_type) == ERROR_SUCCESS && data_ptr) {
                const auto* conn = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data_ptr);
                snap.wifi_connected = true;
                snap.wifi_enabled = true;
                snap.wifi_ssid = SsidToWide(conn->wlanAssociationAttributes.dot11Ssid);
                snap.wifi_signal_pct = std::clamp<int>(static_cast<int>(conn->wlanAssociationAttributes.wlanSignalQuality), 0, 100);
                snap.wifi_bars = SignalToBars(snap.wifi_signal_pct);
                api.free_mem(data_ptr);
            }
        }

        PWLAN_AVAILABLE_NETWORK_LIST nlist = nullptr;
        if (api.get_networks(client, &iface.InterfaceGuid, 0, nullptr, &nlist) == ERROR_SUCCESS && nlist) {
            std::unordered_map<std::wstring, size_t> seen;
            for (DWORD i = 0; i < nlist->dwNumberOfItems; ++i) {
                const auto& net = nlist->Network[i];
                const std::wstring ssid = SsidToWide(net.dot11Ssid);
                if (ssid.empty()) continue;
                const int pct = std::clamp<int>(static_cast<int>(net.wlanSignalQuality), 0, 100);
                const bool conn = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0 ||
                                  (snap.wifi_connected && ssid == snap.wifi_ssid);
                const bool has_prof = (net.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
                const bool sec = (net.bSecurityEnabled != FALSE);
                auto it = seen.find(ssid);
                if (it == seen.end()) {
                    seen[ssid] = snap.wifi_networks.size();
                    DockWifiNetwork item;
                    item.ssid = ssid;
                    item.signal_pct = pct;
                    item.bars = SignalToBars(pct);
                    item.secured = sec;
                    item.connected = conn;
                    item.has_profile = has_prof;
                    if (has_prof && net.strProfileName[0]) item.profile_name = net.strProfileName;
                    snap.wifi_networks.push_back(std::move(item));
                } else {
                    auto& ex = snap.wifi_networks[it->second];
                    if (conn) ex.connected = true;
                    if (pct > ex.signal_pct) {
                        ex.signal_pct = pct;
                        ex.bars = SignalToBars(pct);
                    }
                    if (has_prof && !ex.has_profile) {
                        ex.has_profile = true;
                        if (net.strProfileName[0]) ex.profile_name = net.strProfileName;
                    }
                }
            }
            api.free_mem(nlist);
            std::sort(snap.wifi_networks.begin(), snap.wifi_networks.end(),
                      [](const DockWifiNetwork& a, const DockWifiNetwork& b) {
                          if (a.connected != b.connected) return a.connected > b.connected;
                          if (a.has_profile != b.has_profile) return a.has_profile > b.has_profile;
                          return a.signal_pct > b.signal_pct;
                      });
            if (snap.wifi_networks.size() > 7) snap.wifi_networks.resize(7);
        }
    }
    if (ilist) api.free_mem(ilist);
    api.close_handle(client, nullptr);
}

bool IsVirtualOrTunnelAdapter(const std::wstring& friendly, const std::wstring& desc) noexcept {
    const std::wstring combined = ToLower(friendly + L" " + desc);
    static constexpr const wchar_t* kVirtualTokens[] = {
        L"tailscale", L"wintun", L"meta tunnel", L"flclash", L"clash",
        L"tap-windows", L"tap-win", L"hyper-v", L"vethernet", L"vmware",
        L"virtualbox", L"vbox", L"virtual", L"loopback", L"bluetooth",
        L"zerotier", L"wireguard", L"openvpn", L"npcap", L"vpn",
        L"tunnel", L"teredo", L"isatap", L"6to4", L"wan miniport",
        L"ras async", L"kernel debug", L"radmin", L"hamachi", L"sing-box",
    };
    for (const wchar_t* tok : kVirtualTokens) {
        if (combined.find(tok) != std::wstring::npos) return true;
    }
    return false;
}

std::wstring FormatLinkSpeed(uint64_t bps) {
    if (bps == 0 || bps == UINT64_MAX) return {};
    wchar_t buf[32]{};
    if (bps >= 1000000000ull) {
        const double gbps = static_cast<double>(bps) / 1000000000.0;
        const double rounded = std::round(gbps);
        if (std::fabs(gbps - rounded) < 0.05) {
            std::swprintf(buf, ARRAYSIZE(buf), L"%d Gbps", static_cast<int>(rounded));
        } else {
            std::swprintf(buf, ARRAYSIZE(buf), L"%.1f Gbps", gbps);
        }
        return buf;
    }
    if (bps >= 1000000ull) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%llu Mbps", static_cast<unsigned long long>(bps / 1000000ull));
        return buf;
    }
    std::swprintf(buf, ARRAYSIZE(buf), L"%llu Kbps", static_cast<unsigned long long>(std::max<uint64_t>(1, bps / 1000ull)));
    return buf;
}

void QueryEthernetAndNetworkKind(DockSystemSnapshot& snap) {
    snap.ethernet_connected = false;
    snap.ethernet_name.clear();
    snap.ethernet_desc.clear();
    snap.ethernet_speed.clear();
    snap.ethernet_ipv4.clear();

    // Query Windows default route interface index to break ties when both Ethernet and Wi-Fi are up.
    DWORD best_if = 0;
    const bool has_best_if = (GetBestInterface(0x08080808u /* 8.8.8.8 */, &best_if) == NO_ERROR);

    ULONG buf_len = 24u * 1024u;
    std::vector<uint8_t> buf(buf_len);
    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                     nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW && buf_len > 0 && buf_len <= 2u * 1024u * 1024u) {
        buf.resize(buf_len);
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                   nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &buf_len);
    }

    DWORD chosen_eth_if = 0;
    int best_score = -1;
    if (ret == NO_ERROR) {
        for (auto* aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); aa != nullptr; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            if (aa->IfType != IF_TYPE_ETHERNET_CSMACD) continue;

            const std::wstring friendly = aa->FriendlyName ? std::wstring(aa->FriendlyName) : std::wstring();
            const std::wstring desc = aa->Description ? std::wstring(aa->Description) : std::wstring();
            if (IsVirtualOrTunnelAdapter(friendly, desc)) continue;

            std::wstring ipv4;
            bool non_apipa = false;
            for (auto* ua = aa->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);
                wchar_t ipbuf[46]{};
                if (InetNtopW(AF_INET, &sin->sin_addr, ipbuf, ARRAYSIZE(ipbuf))) {
                    ipv4 = ipbuf;
                    const uint8_t b0 = sin->sin_addr.S_un.S_un_b.s_b1;
                    const uint8_t b1 = sin->sin_addr.S_un.S_un_b.s_b2;
                    non_apipa = !(b0 == 169 && b1 == 254) && !(b0 == 127);
                    if (non_apipa) break;
                }
            }
            if (ipv4.empty()) continue;

            const bool has_gw = (aa->FirstGatewayAddress != nullptr);
            int score = 10;
            if (non_apipa) score += 20;
            if (has_gw) score += 40;
            if (has_best_if && aa->IfIndex == best_if) score += 100;
            if (aa->TransmitLinkSpeed >= 1000000000ull && aa->TransmitLinkSpeed != UINT64_MAX) score += 5;

            if (score > best_score) {
                best_score = score;
                chosen_eth_if = aa->IfIndex;
                snap.ethernet_connected = true;
                snap.ethernet_name = friendly.empty() ? L"以太网" : friendly;
                snap.ethernet_desc = desc;
                snap.ethernet_speed = FormatLinkSpeed(aa->TransmitLinkSpeed);
                snap.ethernet_ipv4 = ipv4;
            }
        }
    }

    if (snap.ethernet_connected && !snap.wifi_connected) {
        snap.network_kind = DockNetworkKind::Ethernet;
    } else if (snap.wifi_connected && !snap.ethernet_connected) {
        snap.network_kind = DockNetworkKind::Wifi;
    } else if (snap.ethernet_connected && snap.wifi_connected) {
        snap.network_kind = (has_best_if && chosen_eth_if == best_if)
            ? DockNetworkKind::Ethernet : DockNetworkKind::Wifi;
    } else {
        snap.network_kind = DockNetworkKind::Disconnected;
    }
}

// ---------------------------------------------------------------------------
// 3. Dynamic BluetoothAPIs.dll (Radio + Paired/Connected Devices + Batteries)
// ---------------------------------------------------------------------------
struct BthRadioFindParams { DWORD dwSize; };
struct BthDeviceSearchParams {
    DWORD dwSize;
    BOOL fReturnAuthenticated;
    BOOL fReturnRemembered;
    BOOL fReturnUnknown;
    BOOL fReturnConnected;
    BOOL fIssueInquiry;
    UCHAR cTimeoutMultiplier;
    HANDLE hRadio;
};
struct BthDeviceInfo {
    DWORD dwSize;
    uint64_t Address;
    ULONG ulClassofDevice;
    BOOL fConnected;
    BOOL fRemembered;
    BOOL fAuthenticated;
    SYSTEMTIME stLastSeen;
    SYSTEMTIME stLastUsed;
    WCHAR szName[248];
};

uint8_t ClassifyBtKind(ULONG cod, const std::wstring& name) noexcept {
    const std::wstring lo = ToLower(name);
    if (lo.find(L"airpod") != std::wstring::npos || lo.find(L"bud") != std::wstring::npos ||
        lo.find(L"head") != std::wstring::npos || lo.find(L"wh-") != std::wstring::npos ||
        lo.find(L"ear") != std::wstring::npos || lo.find(L"audio") != std::wstring::npos ||
        lo.find(L"耳机") != std::wstring::npos || lo.find(L"音箱") != std::wstring::npos) return 1;
    if (lo.find(L"mouse") != std::wstring::npos || lo.find(L"mx ") != std::wstring::npos ||
        lo.find(L"鼠标") != std::wstring::npos) return 2;
    if (lo.find(L"key") != std::wstring::npos || lo.find(L"kbd") != std::wstring::npos ||
        lo.find(L"键盘") != std::wstring::npos) return 3;
    if (lo.find(L"xbox") != std::wstring::npos || lo.find(L"dual") != std::wstring::npos ||
        lo.find(L"controller") != std::wstring::npos || lo.find(L"手柄") != std::wstring::npos) return 4;
    const ULONG major = (cod >> 8) & 0x1Fu;
    if (major == 0x04) return 1; // Audio/Video
    if (major == 0x05) {         // Peripheral
        const ULONG minor = (cod >> 6) & 0x03u;
        if (minor == 0x01) return 3;
        if (minor == 0x02) return 2;
        return 4;
    }
    return 0;
}

void QueryBluetoothState(DockSystemSnapshot& snap, const BatteryPowerSnapshot& bat) {
    snap.bt_available = false;
    snap.bt_enabled = false;
    snap.bt_devices.clear();

    using FindFirstRadioFn = HANDLE(WINAPI*)(const BthRadioFindParams*, HANDLE*);
    using FindRadioCloseFn = BOOL(WINAPI*)(HANDLE);
    using FindFirstDevFn = HANDLE(WINAPI*)(const BthDeviceSearchParams*, BthDeviceInfo*);
    using FindNextDevFn = BOOL(WINAPI*)(HANDLE, BthDeviceInfo*);
    using FindDevCloseFn = BOOL(WINAPI*)(HANDLE);

    static HMODULE mod = LoadLibraryW(L"BluetoothAPIs.dll");
    static auto fn_first_radio = mod ? reinterpret_cast<FindFirstRadioFn>(GetProcAddress(mod, "BluetoothFindFirstRadio")) : nullptr;
    static auto fn_close_radio = mod ? reinterpret_cast<FindRadioCloseFn>(GetProcAddress(mod, "BluetoothFindRadioClose")) : nullptr;
    static auto fn_first_dev = mod ? reinterpret_cast<FindFirstDevFn>(GetProcAddress(mod, "BluetoothFindFirstDevice")) : nullptr;
    static auto fn_next_dev = mod ? reinterpret_cast<FindNextDevFn>(GetProcAddress(mod, "BluetoothFindNextDevice")) : nullptr;
    static auto fn_close_dev = mod ? reinterpret_cast<FindDevCloseFn>(GetProcAddress(mod, "BluetoothFindDeviceClose")) : nullptr;

    std::unordered_map<std::wstring, BatteryDeviceInfo> bat_by_name;
    for (const auto& bd : bat.bt_devices) {
        bat_by_name[ToLower(bd.name)] = bd;
    }

    if (fn_first_radio && fn_close_radio) {
        BthRadioFindParams rp{sizeof(rp)};
        HANDLE radio = nullptr;
        HANDLE hfind = fn_first_radio(&rp, &radio);
        if (hfind) {
            snap.bt_available = true;
            snap.bt_enabled = true;
            fn_close_radio(hfind);
            if (radio) {
                if (fn_first_dev && fn_next_dev && fn_close_dev) {
                    BthDeviceSearchParams sp{};
                    sp.dwSize = sizeof(sp);
                    sp.fReturnAuthenticated = TRUE;
                    sp.fReturnRemembered = TRUE;
                    sp.fReturnConnected = TRUE;
                    sp.fReturnUnknown = FALSE;
                    sp.fIssueInquiry = FALSE;
                    sp.hRadio = radio;
                    BthDeviceInfo di{};
                    di.dwSize = sizeof(di);
                    HANDLE dfind = fn_first_dev(&sp, &di);
                    if (dfind) {
                        std::set<std::wstring> seen;
                        do {
                            std::wstring nm = di.szName;
                            if (nm.empty()) continue;
                            const std::wstring lo = ToLower(nm);
                            if (seen.count(lo)) continue;
                            seen.insert(lo);

                            DockBluetoothDevice dev{};
                            dev.name = nm;
                            dev.connected = (di.fConnected != FALSE);
                            dev.address = di.Address;
                            dev.kind = ClassifyBtKind(di.ulClassofDevice, nm);
                            if (auto it = bat_by_name.find(lo); it != bat_by_name.end()) {
                                dev.battery_pct = it->second.percent;
                                if (it->second.kind != 0) dev.kind = it->second.kind;
                            } else {
                                // Fuzzy substring match against SetupAPI BT battery entries
                                for (const auto& [bname, binfo] : bat_by_name) {
                                    if (bname.find(lo) != std::wstring::npos || lo.find(bname) != std::wstring::npos) {
                                        dev.battery_pct = binfo.percent;
                                        if (binfo.kind != 0) dev.kind = binfo.kind;
                                        break;
                                    }
                                }
                            }
                            snap.bt_devices.push_back(std::move(dev));
                        } while (fn_next_dev(dfind, &di));
                        fn_close_dev(dfind);
                    }
                }
                CloseHandle(radio);
            }
        }
    }

    // Merge any BLE battery devices reported by SetupAPI that weren't in classic BT list.
    for (const auto& bd : bat.bt_devices) {
        const std::wstring lo = ToLower(bd.name);
        const bool exists = std::any_of(snap.bt_devices.begin(), snap.bt_devices.end(),
                                        [&](const DockBluetoothDevice& d) { return ToLower(d.name) == lo; });
        if (!exists) {
            snap.bt_available = true;
            snap.bt_enabled = true;
            snap.bt_devices.push_back({bd.name, bd.percent, bd.kind, true, 0});
        }
    }

    std::sort(snap.bt_devices.begin(), snap.bt_devices.end(),
              [](const DockBluetoothDevice& a, const DockBluetoothDevice& b) {
                  if (a.connected != b.connected) return a.connected > b.connected;
                  if ((a.battery_pct >= 0) != (b.battery_pct >= 0)) return a.battery_pct >= 0;
                  return a.name < b.name;
              });
    if (snap.bt_devices.size() > 6) snap.bt_devices.resize(6);
}

// ---------------------------------------------------------------------------
// 4. Battery & Power Scheme (PowrProf.dll dynamic)
// ---------------------------------------------------------------------------
uint8_t QueryActivePowerMode(std::wstring& out_name) {
    using GetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
    static HMODULE mod = LoadLibraryW(L"PowrProf.dll");
    static auto fn_get = mod ? reinterpret_cast<GetActiveSchemeFn>(GetProcAddress(mod, "PowerGetActiveScheme")) : nullptr;
    if (!fn_get) return 1;
    GUID* active = nullptr;
    if (fn_get(nullptr, &active) != ERROR_SUCCESS || !active) return 1;
    uint8_t mode = 1;
    if (IsEqualGUID(*active, kGuidPowerSaver)) {
        mode = 0;
        if (out_name.empty()) out_name = L"节能";
    } else if (IsEqualGUID(*active, kGuidHighPerf)) {
        mode = 2;
        if (out_name.empty()) out_name = L"性能";
    } else {
        mode = 1;
        if (out_name.empty()) out_name = L"平衡";
    }
    LocalFree(active);
    return mode;
}

// ---------------------------------------------------------------------------
// 5. IME & Keyboard Layouts
// ---------------------------------------------------------------------------
void QueryImeState(HWND fg_hint, DockSystemSnapshot& snap) {
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    if (!fg || fg_pid == GetCurrentProcessId()) {
        if (fg_hint && IsWindow(fg_hint)) fg = fg_hint;
    }
    DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    HKL active_hkl = GetKeyboardLayout(tid);
    if (!active_hkl) active_hkl = GetKeyboardLayout(0);

    const WORD lang_id = LOWORD(reinterpret_cast<uintptr_t>(active_hkl));
    const WORD primary = PRIMARYLANGID(lang_id);

    bool is_chinese_mode = false;
    std::wstring badge = L"英";
    if (primary == LANG_CHINESE) {
        is_chinese_mode = true;
        // Query real IME conversion status from the foreground IME window (timeout-guarded)
        HWND ime_wnd = fg ? ImmGetDefaultIMEWnd(fg) : nullptr;
        if (ime_wnd) {
            DWORD_PTR open_status = 0;
            if (SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcGetOpenStatus, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &open_status) != 0) {
                if (open_status == 0) {
                    is_chinese_mode = false;
                } else {
                    DWORD_PTR conv_mode = 0;
                    if (SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcGetConversionMode, 0,
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &conv_mode) != 0) {
                        is_chinese_mode = ((conv_mode & IME_CMODE_NATIVE) != 0);
                    }
                }
            }
        }
        badge = is_chinese_mode ? L"中" : L"英";
    } else if (primary == LANG_JAPANESE) {
        badge = L"日";
    } else if (primary == LANG_KOREAN) {
        badge = L"한";
    } else {
        badge = L"英";
    }

    snap.ime_chinese = is_chinese_mode;
    snap.ime_badge = badge;

    snap.ime_layouts.clear();
    HKL hkls[16]{};
    const int cnt = GetKeyboardLayoutList(ARRAYSIZE(hkls), hkls);
    for (int i = 0; i < cnt; ++i) {
        const HKL h = hkls[i];
        const WORD lid = LOWORD(reinterpret_cast<uintptr_t>(h));
        const WORD pri = PRIMARYLANGID(lid);
        DockImeLayout item{};
        item.hkl_raw = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
        item.lang_id = lid;
        item.active = (h == active_hkl);
        if (pri == LANG_CHINESE) {
            item.short_code = L"中";
            item.name = (SUBLANGID(lid) == SUBLANG_CHINESE_TRADITIONAL) ? L"中文 (繁体)" : L"微软拼音";
            item.sub = (SUBLANGID(lid) == SUBLANG_CHINESE_TRADITIONAL) ? L"繁体中文" : L"简体中文 · 输入法";
        } else if (pri == LANG_ENGLISH) {
            item.short_code = L"英";
            item.name = L"英语 (美国)";
            item.sub = L"美式键盘";
        } else if (pri == LANG_JAPANESE) {
            item.short_code = L"日";
            item.name = L"日语 (IME)";
            item.sub = L"Microsoft IME";
        } else if (pri == LANG_KOREAN) {
            item.short_code = L"한";
            item.name = L"韩语 (IME)";
            item.sub = L"Microsoft IME";
        } else {
            wchar_t lname[64]{};
            if (LCIDToLocaleName(MAKELCID(lid, SORT_DEFAULT), lname, ARRAYSIZE(lname), 0) > 0) {
                wchar_t disp[96]{};
                if (GetLocaleInfoEx(lname, LOCALE_SLOCALIZEDDISPLAYNAME, disp, ARRAYSIZE(disp)) > 0) {
                    item.name = disp;
                } else {
                    item.name = lname;
                }
                item.short_code = std::wstring(lname).substr(0, 2);
            } else {
                item.short_code = L"EN";
                item.name = L"键盘布局";
            }
            item.sub = L"系统键盘";
        }
        snap.ime_layouts.push_back(std::move(item));
    }
}

// ---------------------------------------------------------------------------
// 6. Real Windows Tray Icon Discovery (ToolbarWindow32 + UIA + Resident Apps)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct TrayButtonData64 {
    uint64_t hwnd;
    uint32_t uID;
    uint32_t uCallbackMessage;
    uint32_t reserved[2];
    uint64_t hIcon;
};
#pragma pack(pop)

void ScanExplorerToolbarTray(HWND tb_hwnd, std::vector<DockTrayApp>& out, std::set<std::wstring>& seen_keys) {
    if (!tb_hwnd || !IsWindow(tb_hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(tb_hwnd, &pid);
    if (pid == 0) return;

    const LRESULT count = SendMessageW(tb_hwnd, TB_BUTTONCOUNT, 0, 0);
    if (count <= 0 || count > 128) return;

    HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!proc) return;
    void* remote_buf = VirtualAllocEx(proc, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_buf) {
        CloseHandle(proc);
        return;
    }

    for (LRESULT i = 0; i < count; ++i) {
        if (!SendMessageW(tb_hwnd, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(remote_buf))) {
            continue;
        }
        TBBUTTON btn{};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(proc, remote_buf, &btn, sizeof(btn), &rd) || rd != sizeof(btn)) continue;
        if ((btn.fsState & TBSTATE_HIDDEN) != 0) continue;

        TrayButtonData64 tdata{};
        if (btn.dwData && ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(btn.dwData), &tdata, sizeof(tdata), &rd) &&
            rd == sizeof(tdata)) {
            HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(tdata.hwnd));
            if (!target || !IsWindow(target)) continue;

            std::wstring tip;
            if (btn.iString > 0xFFFF) {
                wchar_t tbuf[192]{};
                if (ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(btn.iString), tbuf, sizeof(tbuf) - sizeof(wchar_t), &rd)) {
                    tbuf[ARRAYSIZE(tbuf) - 1] = L'\0';
                    tip = tbuf;
                }
            }
            if (const size_t nl = tip.find_first_of(L"\r\n"); nl != std::wstring::npos) tip.resize(nl);

            DWORD tpid = 0;
            GetWindowThreadProcessId(target, &tpid);
            const std::wstring exe = QueryExePathForPid(tpid);
            const std::wstring base = ToLower(BaseNameNoExt(exe));
            if (base == L"explorer" || base == L"shellexperiencehost" || base == L"pulse_desktop_host") continue;

            if (tip.empty()) tip = BaseNameNoExt(exe);
            if (tip.empty()) continue;

            const std::wstring key = !exe.empty() ? ToLower(exe) : (L"tray:" + ToLower(tip));
            if (seen_keys.count(key)) continue;
            seen_keys.insert(key);

            DockTrayApp app{};
            app.key = key;
            app.title = tip;
            app.exe_path = exe;
            app.hwnd = target;
            app.callback_msg = tdata.uCallbackMessage;
            app.icon_uid = tdata.uID;
            app.pid = tpid;
            app.badge_count = ParseUnreadBadge(tip);
            app.accent_argb = HashAccentForKey(key);
            app.from_toolbar = true;
            out.push_back(std::move(app));
        }
    }

    VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);
    CloseHandle(proc);
}

struct NotifyIconRegEntry {
    std::wstring subkey;
    std::wstring exe_path;
    std::wstring initial_tooltip;
    bool promoted = false;
};

struct NotifyIconRegIndex {
    std::unordered_map<std::wstring, NotifyIconRegEntry> by_subkey;
    std::vector<NotifyIconRegEntry> all;
};

bool IsInvisibleTitleChar(wchar_t ch) noexcept {
    if (ch <= 0x0020 || ch == 0x00A0 || ch == 0xFEFF || ch == 0x3000) return true;
    // Unicode directional marks & zero-width formatting characters common in Win11 XAML UIA names
    if ((ch >= 0x200B && ch <= 0x200F) ||
        (ch >= 0x202A && ch <= 0x202E) ||
        (ch >= 0x2060 && ch <= 0x206F)) {
        return true;
    }
    return false;
}

std::wstring TrimSanitizeTitle(std::wstring_view raw) {
    // Take first non-empty line and strip invisible directional marks & outer whitespace.
    size_t start = 0;
    size_t end = raw.size();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r' || raw[i] == L'\n') {
            end = i;
            break;
        }
    }
    std::wstring out;
    out.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        const wchar_t ch = raw[i];
        if ((ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x202A && ch <= 0x202E) ||
            (ch >= 0x2060 && ch <= 0x206F) ||
            ch == 0xFEFF) {
            continue;
        }
        if (ch < 0x0020) continue;
        out.push_back(ch);
    }
    while (!out.empty() && IsInvisibleTitleChar(out.front())) out.erase(out.begin());
    while (!out.empty() && IsInvisibleTitleChar(out.back())) out.pop_back();
    return out;
}

std::wstring ExpandKnownFolderExePath(std::wstring_view raw) {
    if (raw.empty()) return {};
    std::wstring s(raw);
    // Win11 NotifyIconSettings prefixes paths with "{KNOWN_FOLDER_GUID}\subpath\app.exe".
    if (s.size() >= 38 && s.front() == L'{' && s[37] == L'}') {
        const std::wstring guid_str = s.substr(0, 38);
        GUID fid{};
        if (SUCCEEDED(CLSIDFromString(guid_str.c_str(), &fid))) {
            PWSTR folder = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(fid, 0, nullptr, &folder)) && folder) {
                std::wstring resolved(folder);
                CoTaskMemFree(folder);
                std::wstring rest = s.substr(38);
                if (!resolved.empty() && resolved.back() == L'\\' && !rest.empty() && rest.front() == L'\\') {
                    rest.erase(rest.begin());
                }
                return resolved + rest;
            }
        }
    }
    if (s.find(L'%') != std::wstring::npos) {
        wchar_t exp[MAX_PATH * 2]{};
        const DWORD n = ExpandEnvironmentStringsW(s.c_str(), exp, ARRAYSIZE(exp));
        if (n > 0 && n < ARRAYSIZE(exp)) return exp;
    }
    return s;
}

bool FileExistsLocal(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

NotifyIconRegIndex LoadNotifyIconRegIndex() {
    NotifyIconRegIndex idx;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS || !root) {
        return idx;
    }
    wchar_t subname[128]{};
    for (DWORD i = 0; i < 512; ++i) {
        DWORD sublen = ARRAYSIZE(subname);
        if (RegEnumKeyExW(root, i, subname, &sublen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sk = nullptr;
        if (RegOpenKeyExW(root, subname, 0, KEY_READ, &sk) != ERROR_SUCCESS || !sk) continue;

        auto read_str = [&](const wchar_t* val) -> std::wstring {
            wchar_t buf[MAX_PATH * 2]{};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(sk, val, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ) && cb >= sizeof(wchar_t)) {
                buf[ARRAYSIZE(buf) - 1] = L'\0';
                return buf;
            }
            return {};
        };
        DWORD promoted = 0;
        DWORD ptype = 0, pcb = sizeof(promoted);
        const bool has_promoted =
            (RegQueryValueExW(sk, L"IsPromoted", nullptr, &ptype, reinterpret_cast<LPBYTE>(&promoted), &pcb) == ERROR_SUCCESS &&
             ptype == REG_DWORD && promoted != 0);
        const std::wstring raw_exe = read_str(L"ExecutablePath");
        const std::wstring raw_tip = read_str(L"InitialTooltip");
        RegCloseKey(sk);

        const std::wstring exp_exe = ExpandKnownFolderExePath(raw_exe);
        if (exp_exe.empty() && raw_tip.empty()) continue;

        NotifyIconRegEntry entry{};
        entry.subkey = subname;
        entry.exe_path = exp_exe;
        entry.initial_tooltip = TrimSanitizeTitle(raw_tip);
        entry.promoted = has_promoted;
        idx.by_subkey[entry.subkey] = entry;
        idx.all.push_back(std::move(entry));
    }
    RegCloseKey(root);
    return idx;
}

std::wstring ExtractTrailingNumericId(std::wstring_view aid) {
    // Matches Win11 UIA AutomationId patterns such as "NotifyItemIcon:13949164571351438169"
    // or "NotifyItemIcon_13949164571351438169".
    size_t end = aid.size();
    while (end > 0 && (aid[end - 1] < L'0' || aid[end - 1] > L'9')) --end;
    size_t start = end;
    while (start > 0 && aid[start - 1] >= L'0' && aid[start - 1] <= L'9') --start;
    if (end > start && (end - start) >= 6) {
        return std::wstring(aid.substr(start, end - start));
    }
    return {};
}

// Top-level windows every GUI process owns that are never "the app": IME / text-service windows,
// tooltips, GDI+ / COM / DDE helpers and the tray-icon message windows of common frameworks.
constexpr const wchar_t* kNonAppWindowClasses[] = {
    L"IME", L"MSCTFIME UI", L"tooltips_class32", L"GDI+ Hook Window Class", L"GDI+ Window",
    L"OleMainThreadWndClass", L"CicMarshalWndClass", L"DDEMLMom", L"DDEMLEvent", L"SystemTray_Main",
    L"Chrome_SystemMessageWindow", L"Base_PowerMessageWindow", L"QTrayIconMessageWindowClass",
    L"ThumbnailDeviceHelperWnd", L"UserAdapterWindowClass", L"crashpad_SessionEndWatcher",
    L"Shell_TrayWnd", L"WorkerW", L"Progman", L"NotifyIconOverflowWindow"};

// 3 = on screen, 2 = minimized, 1 = hidden main window (an app closed to the tray), 0 = not an app window.
int ScoreTrayAppWindow(HWND h) noexcept {
    if (!h || !IsWindow(h) || GetWindow(h, GW_OWNER) != nullptr) return 0;
    wchar_t cls[96]{};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    for (const wchar_t* c : kNonAppWindowClasses) {
        if (_wcsicmp(cls, c) == 0) return 0;
    }
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0) return 0;
    if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) return 0;
    if ((ex & WS_EX_NOACTIVATE) != 0) return 0;
    const bool iconic = IsIconic(h) != FALSE;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    RECT r{};
    if (iconic && GetWindowPlacement(h, &wp)) r = wp.rcNormalPosition;
    else GetWindowRect(h, &r);
    if (r.right - r.left < 120 || r.bottom - r.top < 80) return 0;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (IsWindowVisible(h) && !cloaked) return iconic ? 2 : 3;
    // Hidden: only something that looks like the app's main frame (titled, captioned or a popup frame).
    wchar_t title[4]{};
    if (GetWindowTextW(h, title, ARRAYSIZE(title)) <= 0) return 0;
    if ((style & WS_CAPTION) != WS_CAPTION && (style & WS_POPUP) == 0) return 0;
    return 1;
}

void BringUpTrayAppWindow(HWND h) noexcept {
    if (!h || !IsWindow(h)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid) AllowSetForegroundWindow(pid);
    if (!IsWindowVisible(h)) ShowWindowAsync(h, SW_SHOW);
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    SetForegroundWindow(h);
}

// Windows 11 draws the notification area in XAML, so there is no callback to post to: invoke the
// tray button through UI Automation instead, which Explorer turns into the same click it would send.
bool InvokeWindows11TrayButton(const DockTrayApp& app) noexcept {
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) || !uia) {
        return false;
    }
    VARIANT v_btn{};
    v_btn.vt = VT_I4;
    v_btn.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v_btn, &cond)) || !cond) return false;
    const std::wstring want_title = ToLower(app.title);
    const HWND hosts[] = {FindWindowW(L"Shell_TrayWnd", nullptr),
                          FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)};
    for (HWND host : hosts) {
        if (!host) continue;
        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(host, &root)) || !root) continue;
        ComPtr<IUIAutomationElementArray> arr;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) continue;
        int len = 0;
        arr->get_Length(&len);
        ComPtr<IUIAutomationElement> by_name;
        for (int i = 0; i < len; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(arr->GetElement(i, &el)) || !el) continue;
            BSTR auto_id = nullptr;
            el->get_CurrentAutomationId(&auto_id);
            const std::wstring aid = auto_id ? std::wstring(auto_id) : std::wstring();
            if (auto_id) SysFreeString(auto_id);
            if (aid.find(L"NotifyItemIcon") == std::wstring::npos) continue;
            bool match = !app.uia_id.empty() && aid == app.uia_id;
            if (!match && !by_name && want_title.size() >= 2) {
                BSTR bname = nullptr;
                el->get_CurrentName(&bname);
                const std::wstring name = bname ? ToLower(TrimSanitizeTitle(bname)) : std::wstring();
                if (bname) SysFreeString(bname);
                if (!name.empty() && (name == want_title || name.rfind(want_title, 0) == 0 ||
                                      want_title.rfind(name, 0) == 0)) {
                    by_name = el;
                }
            }
            if (!match) continue;
            by_name = el;
            break;
        }
        if (!by_name) continue;
        ComPtr<IUIAutomationInvokePattern> inv;
        if (SUCCEEDED(by_name->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv &&
            SUCCEEDED(inv->Invoke())) {
            return true;
        }
    }
    return false;
}

void ScanWindows11TrayViaUia(const NotifyIconRegIndex& reg_idx,
                             std::vector<DockTrayApp>& out,
                             std::set<std::wstring>& seen_keys) {
    HWND tray_wnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray_wnd) return;
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) || !uia) return;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(tray_wnd, &root)) || !root) return;

    VARIANT v_btn{};
    v_btn.vt = VT_I4;
    v_btn.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v_btn, &cond)) || !cond) return;

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) return;

    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len && out.size() < 20; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;

        BSTR auto_id = nullptr;
        el->get_CurrentAutomationId(&auto_id);
        std::wstring aid = auto_id ? std::wstring(auto_id) : std::wstring();
        if (auto_id) SysFreeString(auto_id);

        // Windows 11 notification area app icons carry AutomationId starting with "NotifyItemIcon".
        const bool is_notify_item = (aid.find(L"NotifyItemIcon") != std::wstring::npos);
        if (!is_notify_item) continue;

        BSTR bname = nullptr;
        el->get_CurrentName(&bname);
        std::wstring name = bname ? TrimSanitizeTitle(bname) : std::wstring();
        if (bname) SysFreeString(bname);
        if (name.empty()) continue;

        const std::wstring key = L"uia:" + ToLower(name);
        if (seen_keys.count(key)) continue;
        seen_keys.insert(key);

        DockTrayApp app{};
        app.key = key;
        app.title = name;
        app.badge_count = ParseUnreadBadge(name);
        app.accent_argb = HashAccentForKey(key);
        app.uia_id = aid;

        // 1) Direct Win11 AutomationId -> NotifyIconSettings registry lookup (exact real exe path!)
        const std::wstring num_id = ExtractTrailingNumericId(aid);
        if (!num_id.empty()) {
            if (auto it = reg_idx.by_subkey.find(num_id); it != reg_idx.by_subkey.end() &&
                FileExistsLocal(it->second.exe_path)) {
                app.exe_path = it->second.exe_path;
            }
        }
        // 2) Fallback tooltip lookup in NotifyIconSettings
        if (app.exe_path.empty()) {
            const std::wstring lo_name = ToLower(name);
            for (const auto& re : reg_idx.all) {
                if (re.initial_tooltip.empty() || !FileExistsLocal(re.exe_path)) continue;
                const std::wstring lo_tip = ToLower(re.initial_tooltip);
                if (lo_tip == lo_name || lo_name.rfind(lo_tip, 0) == 0 || lo_tip.rfind(lo_name, 0) == 0) {
                    app.exe_path = re.exe_path;
                    break;
                }
            }
        }
        out.push_back(std::move(app));
    }
}

struct RunningProcSnapshot {
    DWORD pid = 0;
    std::wstring exe_lower;
    std::wstring base_lower;
    std::wstring full_path;
};

// The app's own name from its version resource ("CC Switch", "Tailscale"), or "" when it has none.
std::wstring FileDescriptionOf(const std::wstring& exe) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size == 0 || size > (1u << 20)) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    struct LangCp { WORD lang, cp; }* langs = nullptr;
    UINT langs_len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &langs_len) ||
        !langs || langs_len < sizeof(LangCp))
        return {};
    wchar_t sub[64]{};
    std::swprintf(sub, ARRAYSIZE(sub), L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* desc = nullptr;
    UINT desc_len = 0;
    if (!VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&desc), &desc_len) || !desc || desc_len == 0) return {};
    return TrimSanitizeTitle(desc);
}

void SupplementAndEnrichTrayApps(const NotifyIconRegIndex& reg_idx,
                                 std::vector<DockTrayApp>& out,
                                 std::set<std::wstring>& seen_keys) {
    struct KnownTrayProc {
        const wchar_t* exe_lower;
        const wchar_t* display;
        const wchar_t* alias_lower;
        uint32_t accent;
    };
    static constexpr KnownTrayProc kKnown[] = {
        {L"weixin.exe",       L"微信",         L"微信",         0xFF07C160u},
        {L"wechat.exe",       L"微信",         L"wechat",       0xFF07C160u},
        {L"qq.exe",           L"QQ",           L"qq",           0xFF12B7F5u},
        {L"tim.exe",          L"TIM",          L"tim",          0xFF12B7F5u},
        {L"cloudmusic.exe",   L"网易云音乐",    L"网易云",       0xFFE60026u},
        {L"qqmusic.exe",      L"QQ 音乐",      L"qq音乐",       0xFF31C27Cu},
        {L"spotify.exe",      L"Spotify",      L"spotify",      0xFF1DB954u},
        {L"steam.exe",        L"Steam",        L"steam",        0xFF1B2838u},
        {L"discord.exe",      L"Discord",      L"discord",      0xFF5865F2u},
        {L"telegram.exe",     L"Telegram",     L"telegram",     0xFF2AABEEu},
        {L"dingtalk.exe",     L"钉钉",         L"钉钉",         0xFF0089FFu},
        {L"feishu.exe",       L"飞书",         L"飞书",         0xFF3370FFu},
        {L"lark.exe",         L"飞书",         L"lark",         0xFF3370FFu},
        {L"powertoys.exe",    L"PowerToys",    L"powertoys",    0xFF0078D4u},
        {L"everything.exe",   L"Everything",   L"everything",   0xFFFF8C00u},
        {L"flclash.exe",      L"FlClash",      L"flclash",      0xFF3B82F6u},
        {L"clash-verge.exe",  L"Clash Verge",  L"clash",        0xFF3B82F6u},
        {L"mihomo.exe",       L"Mihomo",       L"mihomo",       0xFF3B82F6u},
        {L"snipaste.exe",     L"Snipaste",     L"snipaste",     0xFF00B4D8u},
        {L"obs64.exe",        L"OBS Studio",   L"obs",          0xFF302E31u},
        {L"onedrive.exe",     L"OneDrive",     L"onedrive",     0xFF0078D4u},
        {L"baidunetdisk.exe", L"百度网盘",     L"百度网盘",     0xFF06A7FFu},
        {L"alist.exe",        L"AList",        L"alist",        0xFF2563EBu},
        {L"pulse.exe",        L"Pulse",        L"pulse",        0xFF3B82F6u},
    };

    std::vector<RunningProcSnapshot> procs;
    DWORD own_session = 0;
    const bool own_session_known = ProcessIdToSessionId(GetCurrentProcessId(), &own_session) != FALSE;
    HANDLE snap_h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap_h != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        if (Process32FirstW(snap_h, &pe)) {
            do {
                const std::wstring exe_lo = ToLower(pe.szExeFile);
                if (exe_lo.empty() || exe_lo == L"svchost.exe" || exe_lo == L"conhost.exe" ||
                    exe_lo == L"runtimebroker.exe" || exe_lo == L"pulse_desktop_host.exe") {
                    continue;
                }
                if (DWORD session = 0; own_session_known && ProcessIdToSessionId(pe.th32ProcessID, &session) &&
                                       session != own_session) {
                    continue;
                }
                RunningProcSnapshot rp{};
                rp.pid = pe.th32ProcessID;
                rp.exe_lower = exe_lo;
                rp.base_lower = BaseNameNoExt(exe_lo);
                procs.push_back(std::move(rp));
            } while (Process32NextW(snap_h, &pe));
        }
        CloseHandle(snap_h);
    }

    auto registry_path_for = [&](const std::wstring& exe_lower) -> std::wstring {
        for (const auto& re : reg_idx.all) {
            if (ToLower(BaseNameNoExt(re.exe_path)) + L".exe" == exe_lower && FileExistsLocal(re.exe_path)) {
                return re.exe_path;
            }
        }
        return {};
    };
    auto resolve_proc_path = [&](RunningProcSnapshot& rp) -> const std::wstring& {
        if (rp.full_path.empty()) rp.full_path = QueryExePathForPid(rp.pid);
        if (rp.full_path.empty()) rp.full_path = registry_path_for(rp.exe_lower);
        return rp.full_path;
    };

    // 1) Enrich UIA-discovered tray icons with real exe_path, pid, and brand fallback accent
    //    BEFORE supplementing so WeChat / QQ / FlClash / Everything / Pulse ALWAYS load real icons!
    for (auto& app : out) {
        const std::wstring lo_title = ToLower(app.title);
        // Match known Chinese/English aliases (e.g. "微信" -> Weixin.exe/WeChat.exe, "QQ: 11044..." -> QQ.exe)
        if (app.exe_path.empty() || app.pid == 0) {
            for (const auto& k : kKnown) {
                const std::wstring disp_lo = ToLower(k.display);
                const bool name_match =
                    lo_title == disp_lo ||
                    lo_title.rfind(disp_lo + L":", 0) == 0 ||
                    lo_title.rfind(disp_lo + L"：", 0) == 0 ||
                    lo_title.rfind(disp_lo + L" ", 0) == 0 ||
                    lo_title.rfind(disp_lo + L"(", 0) == 0 ||
                    lo_title.find(k.alias_lower) != std::wstring::npos;
                if (!name_match) continue;

                app.accent_argb = k.accent;
                for (auto& rp : procs) {
                    if (rp.exe_lower != k.exe_lower) continue;
                    const std::wstring& full = resolve_proc_path(rp);
                    if (app.pid == 0) app.pid = rp.pid;
                    if (app.exe_path.empty() && !full.empty()) {
                        app.exe_path = full;
                        break;
                    }
                }
                // Fallback to NotifyIconSettings promoted exe if process is elevated (OpenProcess denied)
                if (app.exe_path.empty()) {
                    for (const auto& re : reg_idx.all) {
                        if (ToLower(BaseNameNoExt(re.exe_path)) + L".exe" == k.exe_lower &&
                            FileExistsLocal(re.exe_path)) {
                            app.exe_path = re.exe_path;
                            break;
                        }
                    }
                }
                if (!app.exe_path.empty()) break;
            }
        }
        // Generic basename token match against running processes (e.g. "Snipaste", "FlClash", "Clash Verge", etc.)
        if (app.exe_path.empty()) {
            for (auto& rp : procs) {
                if (rp.base_lower.size() < 3) continue;
                if (lo_title == rp.base_lower || lo_title.find(rp.base_lower) != std::wstring::npos) {
                    const std::wstring& full = resolve_proc_path(rp);
                    if (!full.empty()) {
                        app.exe_path = full;
                        if (app.pid == 0) app.pid = rp.pid;
                        break;
                    }
                }
            }
        }
        // Backfill pid when exe_path came from NotifyIconSettings so click-to-activate finds the window
        if (app.pid == 0 && !app.exe_path.empty()) {
            const std::wstring want_base = ToLower(BaseNameNoExt(app.exe_path));
            for (auto& rp : procs) {
                if (rp.base_lower == want_base) {
                    app.pid = rp.pid;
                    break;
                }
            }
        }
    }

    // 2) Supplement with running resident apps that Windows 11 keeps hidden inside XAML overflow.
    for (auto& rp : procs) {
        if (out.size() >= 20) break;
        for (const auto& k : kKnown) {
            if (rp.exe_lower != k.exe_lower) continue;
            const std::wstring& full = resolve_proc_path(rp);
            const std::wstring key = !full.empty() ? ToLower(full) : (L"proc:" + rp.exe_lower);
            const std::wstring disp_lo = ToLower(k.display);
            const bool dup = seen_keys.count(key) ||
                             std::any_of(out.begin(), out.end(), [&](const DockTrayApp& e) {
                                 if (!full.empty() && !e.exe_path.empty() && ToLower(e.exe_path) == ToLower(full)) return true;
                                 const std::wstring et = ToLower(e.title);
                                 return et == disp_lo || et.find(disp_lo) != std::wstring::npos;
                             });
            if (dup) break;
            seen_keys.insert(key);

            DockTrayApp app{};
            app.key = key;
            app.title = k.display;
            app.exe_path = full;
            app.pid = rp.pid;
            app.accent_argb = k.accent;
            out.push_back(std::move(app));
            break;
        }
    }

    // 2b) Every other running app that has registered a notification icon (NotifyIconSettings). Windows 11
    //     keeps most of them in the overflow flyout, which neither the toolbar nor the UIA scan can see.
    //     A registered path counts only when that exact file is running (several builds of one app may be
    //     registered); a process whose path is unreadable (elevated) matches its registered path by name.
    {
        std::set<std::wstring> running_paths, unreadable_bases;
        for (auto& rp : procs) {
            if (rp.full_path.empty()) rp.full_path = QueryExePathForPid(rp.pid);
            if (!rp.full_path.empty()) running_paths.insert(ToLower(rp.full_path));
            else unreadable_bases.insert(rp.exe_lower);
        }
        std::set<std::wstring> taken_bases;
        for (const auto& re : reg_idx.all) {
            if (out.size() >= 20) break;
            if (re.exe_path.empty()) continue;
            const std::wstring path_lo = ToLower(re.exe_path);
            const std::wstring exe_lo = ToLower(BaseNameNoExt(re.exe_path)) + L".exe";
            if (exe_lo == L"explorer.exe" || exe_lo == L"pulse_desktop_host.exe") continue;
            const bool exact = running_paths.count(path_lo) != 0;
            const bool by_name = !exact && unreadable_bases.count(exe_lo) && !taken_bases.count(exe_lo) &&
                                 FileExistsLocal(re.exe_path);
            if (!exact && !by_name) continue;
            const bool dup = seen_keys.count(path_lo) ||
                             std::any_of(out.begin(), out.end(), [&](const DockTrayApp& e) {
                                 return !e.exe_path.empty() && ToLower(e.exe_path) == path_lo;
                             });
            if (dup) continue;
            if (by_name) taken_bases.insert(exe_lo);
            seen_keys.insert(path_lo);

            DockTrayApp app{};
            app.key = path_lo;
            app.exe_path = re.exe_path;
            app.title = re.initial_tooltip;
            if (app.title.empty()) app.title = FileDescriptionOf(re.exe_path);
            if (app.title.empty()) app.title = BaseNameNoExt(re.exe_path);
            app.badge_count = ParseUnreadBadge(app.title);
            app.accent_argb = HashAccentForKey(path_lo);
            for (const auto& rp : procs) {
                if (ToLower(rp.full_path) == path_lo || (by_name && rp.exe_lower == exe_lo)) {
                    app.pid = rp.pid;
                    break;
                }
            }
            out.push_back(std::move(app));
        }
    }

    // 3) Match main window HWND, unread title badge, and fallback window-title -> exe_path for any remaining app.
    if (!out.empty()) {
        struct WinMatchCtx { std::vector<DockTrayApp>* apps; } ctx{&out};
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<WinMatchCtx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == 0) return TRUE;
            wchar_t title[160]{};
            const int tlen = GetWindowTextW(h, title, ARRAYSIZE(title));
            const std::wstring wtitle = (tlen > 0) ? TrimSanitizeTitle(title) : std::wstring();
            const std::wstring lo_wtitle = ToLower(wtitle);
            for (auto& a : *c->apps) {
                if (a.pid != 0 && a.pid == pid) {
                    if (!wtitle.empty()) {
                        const int b = ParseUnreadBadge(wtitle);
                        if (b > a.badge_count) a.badge_count = b;
                    }
                    // Only a real app frame: every process also owns hidden "Default IME" / helper
                    // windows with titles, and restoring one of those shows nothing.
                    if (!a.hwnd && ScoreTrayAppWindow(h) > 0) {
                        a.hwnd = h;
                    }
                } else if (a.exe_path.empty() && !lo_wtitle.empty() && lo_wtitle.size() >= 2) {
                    const std::wstring lo_at = ToLower(a.title);
                    if (lo_wtitle == lo_at || lo_at.rfind(lo_wtitle, 0) == 0 || lo_wtitle.rfind(lo_at, 0) == 0) {
                        const std::wstring exe = QueryExePathForPid(pid);
                        if (!exe.empty()) {
                            a.exe_path = exe;
                            if (a.pid == 0) a.pid = pid;
                            if (!a.hwnd && ScoreTrayAppWindow(h) > 0) a.hwnd = h;
                        }
                    }
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }
}

void QueryTrayApps(const std::vector<std::wstring>& pinned_keys, std::vector<DockTrayApp>& out) {
    out.clear();
    std::set<std::wstring> seen_keys;
    const NotifyIconRegIndex reg_idx = LoadNotifyIconRegIndex();

    // 1. Classic Shell_TrayWnd -> TrayNotifyWnd -> SysPager -> ToolbarWindow32
    if (HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        if (HWND notify = FindWindowExW(shell, nullptr, L"TrayNotifyWnd", nullptr)) {
            if (HWND pager = FindWindowExW(notify, nullptr, L"SysPager", nullptr)) {
                if (HWND tb = FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr)) {
                    ScanExplorerToolbarTray(tb, out, seen_keys);
                }
            }
            if (HWND tb = FindWindowExW(notify, nullptr, L"ToolbarWindow32", nullptr)) {
                ScanExplorerToolbarTray(tb, out, seen_keys);
            }
        }
    }
    // 2. Classic overflow tray window
    if (HWND ovf = FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
        if (HWND tb = FindWindowExW(ovf, nullptr, L"ToolbarWindow32", nullptr)) {
            ScanExplorerToolbarTray(tb, out, seen_keys);
        }
    }
    // 3. Windows 11 XAML notification area icons via UIA + NotifyIconSettings registry resolution
    ScanWindows11TrayViaUia(reg_idx, out, seen_keys);
    // 4. Enrich discovered UIA icons with real exe_path/pid/hwnd & supplement resident background apps
    SupplementAndEnrichTrayApps(reg_idx, out, seen_keys);

    // Apply user pinned tray keys; if none saved yet, auto-pin up to 2 discovered apps cleanly.
    std::set<std::wstring> pin_set;
    for (const auto& k : pinned_keys) {
        if (!k.empty()) pin_set.insert(ToLower(k));
    }
    if (!pin_set.empty()) {
        for (auto& a : out) {
            if (pin_set.count(ToLower(a.key)) || pin_set.count(ToLower(a.title)) ||
                (!a.exe_path.empty() && pin_set.count(ToLower(a.exe_path)))) {
                a.pinned = true;
            }
        }
    } else {
        int auto_pinned = 0;
        for (auto& a : out) {
            if (auto_pinned < 2) {
                a.pinned = true;
                ++auto_pinned;
            }
        }
    }
}

void FormatClockStrings(bool clock_24h, std::wstring& out_time, std::wstring& out_date) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t tb[16]{};
    if (clock_24h) {
        std::swprintf(tb, ARRAYSIZE(tb), L"%02u:%02u", st.wHour, st.wMinute);
    } else {
        const unsigned h12 = (st.wHour % 12u == 0u) ? 12u : (st.wHour % 12u);
        std::swprintf(tb, ARRAYSIZE(tb), L"%u:%02u", h12, st.wMinute);
    }
    out_time = tb;
    wchar_t db[16]{};
    std::swprintf(db, ARRAYSIZE(db), L"%u/%u", st.wMonth, st.wDay);
    out_date = db;
}

} // namespace

// ---------------------------------------------------------------------------
// Direct Control Helpers
// ---------------------------------------------------------------------------
std::wstring SanitizeTrayTitle(std::wstring_view raw) {
    return TrimSanitizeTitle(raw);
}

std::wstring ExpandNotifyIconExecutablePath(std::wstring_view raw) {
    return ExpandKnownFolderExePath(raw);
}

bool SetMasterAudioVolume(int pct) noexcept {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioEndpointVolume> epv;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) && en &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(epv.GetAddressOf()))) && epv) {
            const float scalar = static_cast<float>(std::clamp(pct, 0, 100)) / 100.0f;
            ok = SUCCEEDED(epv->SetMasterVolumeLevelScalar(scalar, nullptr));
            if (ok && pct > 0) epv->SetMute(FALSE, nullptr);
        }
    }
    if (ci == S_OK || ci == S_FALSE) CoUninitialize();
    return ok;
}

bool StepMasterAudioVolume(int delta_pct, int& out_pct, bool& out_muted) noexcept {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioEndpointVolume> epv;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) && en &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(epv.GetAddressOf()))) && epv) {
            float cur = 0.5f;
            BOOL m = FALSE;
            epv->GetMasterVolumeLevelScalar(&cur);
            epv->GetMute(&m);
            const int next = std::clamp(static_cast<int>(std::lround(cur * 100.0f)) + delta_pct, 0, 100);
            if (SUCCEEDED(epv->SetMasterVolumeLevelScalar(static_cast<float>(next) / 100.0f, nullptr))) {
                if (next > 0 && m) {
                    epv->SetMute(FALSE, nullptr);
                    m = FALSE;
                }
                out_pct = next;
                out_muted = (m != FALSE) || (next == 0);
                ok = true;
            }
        }
    }
    if (ci == S_OK || ci == S_FALSE) CoUninitialize();
    return ok;
}

bool ToggleMasterAudioMute(int& out_pct, bool& out_muted) noexcept {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioEndpointVolume> epv;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) && en &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(epv.GetAddressOf()))) && epv) {
            float cur = 0.5f;
            BOOL m = FALSE;
            epv->GetMasterVolumeLevelScalar(&cur);
            epv->GetMute(&m);
            const BOOL next_m = m ? FALSE : TRUE;
            if (SUCCEEDED(epv->SetMute(next_m, nullptr))) {
                out_pct = std::clamp(static_cast<int>(std::lround(cur * 100.0f)), 0, 100);
                out_muted = (next_m != FALSE);
                ok = true;
            }
        }
    }
    if (ci == S_OK || ci == S_FALSE) CoUninitialize();
    return ok;
}

bool SetDefaultAudioOutputDevice(const std::wstring& device_id) noexcept {
    if (device_id.empty()) return false;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IPolicyConfig> cfg;
        if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cfg))) && cfg) {
            const HRESULT r0 = cfg->SetDefaultEndpoint(device_id.c_str(), eConsole);
            cfg->SetDefaultEndpoint(device_id.c_str(), eMultimedia);
            cfg->SetDefaultEndpoint(device_id.c_str(), eCommunications);
            ok = SUCCEEDED(r0);
        }
    }
    if (ci == S_OK || ci == S_FALSE) CoUninitialize();
    return ok;
}

bool SetSystemPowerMode(uint8_t mode) noexcept {
    using SetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);
    static HMODULE mod = LoadLibraryW(L"PowrProf.dll");
    static auto fn_set = mod ? reinterpret_cast<SetActiveSchemeFn>(GetProcAddress(mod, "PowerSetActiveScheme")) : nullptr;
    if (!fn_set) return false;
    const GUID* target = (mode == 0) ? &kGuidPowerSaver
                       : (mode == 2) ? &kGuidHighPerf
                                     : &kGuidBalanced;
    return fn_set(nullptr, target) == ERROR_SUCCESS;
}

bool ToggleImeChineseEnglish(HWND target_hwnd) noexcept {
    HWND tgt = (target_hwnd && IsWindow(target_hwnd)) ? target_hwnd : GetForegroundWindow();
    if (!tgt) return false;

    const DWORD tid = GetWindowThreadProcessId(tgt, nullptr);
    const HKL hkl = GetKeyboardLayout(tid);
    const WORD pri = PRIMARYLANGID(LOWORD(reinterpret_cast<uintptr_t>(hkl)));
    if (pri == LANG_CHINESE) {
        if (HWND ime_wnd = ImmGetDefaultIMEWnd(tgt)) {
            DWORD_PTR open_status = 0;
            if (SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcGetOpenStatus, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &open_status) != 0) {
                if (open_status == 0) {
                    SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcSetOpenStatus, 1,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, nullptr);
                    return true;
                }
                DWORD_PTR conv = 0;
                if (SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcGetConversionMode, 0,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &conv) != 0) {
                    const WPARAM next_conv = (conv & IME_CMODE_NATIVE)
                        ? static_cast<WPARAM>(conv & ~static_cast<DWORD_PTR>(IME_CMODE_NATIVE))
                        : static_cast<WPARAM>(conv | IME_CMODE_NATIVE);
                    SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, kImcSetConversionMode, next_conv,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, nullptr);
                    return true;
                }
            }
        }
    }

    // Fallback: cycle between installed keyboard layouts on target window
    HKL hkls[16]{};
    const int n = GetKeyboardLayoutList(ARRAYSIZE(hkls), hkls);
    if (n >= 2) {
        int cur = 0;
        for (int i = 0; i < n; ++i) {
            if (hkls[i] == hkl) { cur = i; break; }
        }
        const HKL next = hkls[(cur + 1) % n];
        PostMessageW(tgt, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(next));
        return true;
    }
    return false;
}

bool SwitchKeyboardLayout(HWND target_hwnd, uint64_t hkl_raw) noexcept {
    if (hkl_raw == 0) return false;
    HWND tgt = (target_hwnd && IsWindow(target_hwnd)) ? target_hwnd : GetForegroundWindow();
    if (!tgt) return false;
    PostMessageW(tgt, WM_INPUTLANGCHANGEREQUEST, 0, static_cast<LPARAM>(hkl_raw));
    return true;
}

bool ConnectWifiNetwork(const std::wstring& ssid) noexcept {
    if (ssid.empty()) return false;
    const auto& api = GetWlanApi();
    if (!api.Ready() || !api.connect) {
        OpenWindowsSettingsUri(L"ms-settings:network-wifi");
        return false;
    }
    DWORD neg = 0;
    HANDLE client = nullptr;
    if (api.open_handle(2, nullptr, &neg, &client) != ERROR_SUCCESS || !client) return false;
    bool ok = false;
    PWLAN_INTERFACE_INFO_LIST ilist = nullptr;
    if (api.enum_interfaces(client, nullptr, &ilist) == ERROR_SUCCESS && ilist && ilist->dwNumberOfItems > 0) {
        const GUID& gid = ilist->InterfaceInfo[0].InterfaceGuid;
        WLAN_CONNECTION_PARAMETERS params{};
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = ssid.c_str();
        params.dot11BssType = dot11_BSS_type_any;
        if (api.connect(client, &gid, &params, nullptr) == ERROR_SUCCESS) {
            ok = true;
        }
    }
    if (ilist) api.free_mem(ilist);
    api.close_handle(client, nullptr);
    if (!ok) OpenWindowsSettingsUri(L"ms-settings:network-wifi");
    return ok;
}

bool DisconnectWifiNetwork() noexcept {
    const auto& api = GetWlanApi();
    if (!api.Ready() || !api.disconnect) return false;
    DWORD neg = 0;
    HANDLE client = nullptr;
    if (api.open_handle(2, nullptr, &neg, &client) != ERROR_SUCCESS || !client) return false;
    bool ok = false;
    PWLAN_INTERFACE_INFO_LIST ilist = nullptr;
    if (api.enum_interfaces(client, nullptr, &ilist) == ERROR_SUCCESS && ilist && ilist->dwNumberOfItems > 0) {
        for (DWORD i = 0; i < ilist->dwNumberOfItems; ++i) {
            if (ilist->InterfaceInfo[i].isState == wlan_interface_state_connected) {
                ok = (api.disconnect(client, &ilist->InterfaceInfo[i].InterfaceGuid, nullptr) == ERROR_SUCCESS);
                break;
            }
        }
    }
    if (ilist) api.free_mem(ilist);
    api.close_handle(client, nullptr);
    return ok;
}

bool ActivateDockTrayApp(const DockTrayApp& app, bool right_click, POINT screen_pt) noexcept {
    if (app.from_toolbar && app.hwnd && IsWindow(app.hwnd) && app.callback_msg != 0) {
        DWORD pid = 0;
        GetWindowThreadProcessId(app.hwnd, &pid);
        if (pid) AllowSetForegroundWindow(pid);
        const WPARAM wp = static_cast<WPARAM>(app.icon_uid);
        if (right_click) {
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_RBUTTONDOWN);
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_RBUTTONUP);
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_CONTEXTMENU);
        } else {
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_LBUTTONDOWN);
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_LBUTTONUP);
            PostMessageW(app.hwnd, app.callback_msg, wp, WM_LBUTTONDBLCLK);
        }
        (void)screen_pt;
        return true;
    }
    // Windows 11 (XAML tray, no callback to post to). Look the window up now rather than trusting the
    // snapshot, which is seconds old and used to hold whichever titled window came first — often the
    // hidden "Default IME" window, so the click "worked" but nothing appeared.
    DWORD pid = app.pid;
    if (!pid && app.hwnd && IsWindow(app.hwnd)) GetWindowThreadProcessId(app.hwnd, &pid);
    int score = 0;
    const HWND win = FindDockTrayAppWindow(pid, app.exe_path, &score);
    if (win && score >= 2) {
        BringUpTrayAppWindow(win);
        return true;
    }
    if (pid == 0 && app.exe_path.empty() && app.uia_id.empty() && app.title.empty()) return false;
    // Nothing on screen: let the app open itself the way its own tray icon click would, then fall back
    // to its hidden main window, then to starting it (single-instance apps show their window).
    AllowSetForegroundWindow(ASFW_ANY); // while this process still owns the click's foreground right
    const DockTrayApp copy = app;
    auto open_it = [copy, win, score]() {
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool done = InvokeWindows11TrayButton(copy);
        if (!done && win && score == 1 && IsWindow(win)) {
            BringUpTrayAppWindow(win);
            done = true;
        }
        if (!done && !copy.exe_path.empty()) {
            ShellExecuteW(nullptr, L"open", copy.exe_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (SUCCEEDED(ci)) CoUninitialize();
    };
    try {
        std::thread(open_it).detach();
    } catch (...) {
        open_it();
    }
    return true;
}

HWND FindDockTrayAppWindow(DWORD pid, const std::wstring& exe_path, int* out_score) noexcept {
    if (out_score) *out_score = 0;
    std::set<DWORD> pids;
    if (pid) pids.insert(pid);
    if (!exe_path.empty()) {
        // Tray icon and window can live in different processes of one app (launchers, Chromium hosts).
        const std::wstring want_path = ToLower(exe_path);
        const std::wstring want_exe = ToLower(BaseNameNoExt(exe_path)) + L".exe";
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (ToLower(pe.szExeFile) != want_exe || pe.th32ProcessID == GetCurrentProcessId()) continue;
                const std::wstring full = QueryExePathForPid(pe.th32ProcessID);
                if (full.empty() || ToLower(full) == want_path) pids.insert(pe.th32ProcessID);
            }
            CloseHandle(snap);
        }
    }
    if (pids.empty()) return nullptr;
    struct Ctx {
        const std::set<DWORD>* pids;
        HWND best = nullptr;
        int best_score = 0;
        long long best_area = 0;
    } ctx{&pids};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (!c->pids->count(wpid)) return TRUE;
        const int s = ScoreTrayAppWindow(h);
        if (s == 0) return TRUE;
        RECT r{};
        GetWindowRect(h, &r);
        const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
        // Higher score wins; on screen, the first in Z order (most recent) wins; hidden, the largest.
        if (s > c->best_score || (s == c->best_score && s == 1 && area > c->best_area)) {
            c->best = h;
            c->best_score = s;
            c->best_area = area;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (out_score) *out_score = ctx.best_score;
    return ctx.best;
}

void OpenWindowsSettingsUri(const wchar_t* uri) noexcept {
    if (!uri || !*uri) return;
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenWindowsNotificationCenter() noexcept {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'N';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'N';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// DockSystemMonitor Background Worker
// ---------------------------------------------------------------------------
struct DockSystemMonitor::Impl {
    HWND notify_hwnd = nullptr;
    UINT notify_msg = 0;
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> enabled{true};
    std::atomic<bool> clock_24h{true};
    std::atomic<bool> want_fast{true};
    std::atomic<bool> want_slow{true};
    std::atomic<HWND> fg_hint{nullptr};

    mutable std::mutex mu;
    std::condition_variable cv;
    std::vector<std::wstring> pinned_keys;
    DockSystemSnapshot snapshot;
    std::thread worker;

    void Run() {
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uint64_t last_slow_ms = 0;
        DockSystemSnapshot local{};
        while (!stop_flag.load(std::memory_order_acquire)) {
            if (!enabled.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(500), [&] {
                    return stop_flag.load(std::memory_order_acquire) || enabled.load(std::memory_order_acquire);
                });
                continue;
            }

            const uint64_t now_ms = GetTickCount64();
            const bool do_slow = want_slow.exchange(false, std::memory_order_acq_rel) ||
                                 (last_slow_ms == 0) || (now_ms - last_slow_ms >= 5000);
            want_fast.store(false, std::memory_order_release);

            DockSystemSnapshot next = local;
            next.valid = true;
            FormatClockStrings(clock_24h.load(std::memory_order_acquire), next.time_text, next.date_text);
            QueryImeState(fg_hint.load(std::memory_order_acquire), next);
            QueryCoreAudio(do_slow, next.audio_available, next.volume_pct, next.muted,
                           next.default_audio_name, next.audio_devices);

            if (do_slow) {
                last_slow_ms = now_ms;
                std::vector<std::wstring> pins;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    pins = pinned_keys;
                }
                QueryWifiState(next);
                QueryEthernetAndNetworkKind(next);
                const BatteryPowerSnapshot bat = SampleBatteryPower(true);
                next.has_battery = bat.has_battery;
                next.battery_pct = bat.battery_pct;
                next.battery_charging = bat.charging;
                next.ac_online = bat.ac_online;
                next.battery_remaining_min = bat.remaining_min;
                next.power_plan_name = bat.power_plan;
                next.power_mode = QueryActivePowerMode(next.power_plan_name);
                QueryBluetoothState(next, bat);
                QueryTrayApps(pins, next.tray_apps);
            }

            if (!(next == local)) {
                local = next;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    snapshot = local;
                }
                if (notify_hwnd && IsWindow(notify_hwnd)) {
                    PostMessageW(notify_hwnd, notify_msg, 0, 0);
                }
            }

            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(1000), [&] {
                return stop_flag.load(std::memory_order_acquire) ||
                       want_fast.load(std::memory_order_acquire) ||
                       want_slow.load(std::memory_order_acquire);
            });
        }
        if (SUCCEEDED(ci)) CoUninitialize();
    }
};

DockSystemMonitor::DockSystemMonitor() : impl_(std::make_unique<Impl>()) {}

DockSystemMonitor::~DockSystemMonitor() { Stop(); }

void DockSystemMonitor::Start(HWND notify_hwnd, UINT notify_msg) {
    Stop();
    impl_->notify_hwnd = notify_hwnd;
    impl_->notify_msg = notify_msg;
    impl_->stop_flag.store(false, std::memory_order_release);
    impl_->want_fast.store(true, std::memory_order_release);
    impl_->want_slow.store(true, std::memory_order_release);
    impl_->worker = std::thread([this] { impl_->Run(); });
}

void DockSystemMonitor::Stop() noexcept {
    if (!impl_) return;
    impl_->stop_flag.store(true, std::memory_order_release);
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void DockSystemMonitor::SetEnabled(bool enabled) noexcept {
    if (!impl_) return;
    impl_->enabled.store(enabled, std::memory_order_release);
    if (enabled) {
        impl_->want_fast.store(true, std::memory_order_release);
        impl_->cv.notify_all();
    }
}

void DockSystemMonitor::SetClock24h(bool clock_24h) noexcept {
    if (!impl_) return;
    if (impl_->clock_24h.exchange(clock_24h, std::memory_order_acq_rel) != clock_24h) {
        impl_->want_fast.store(true, std::memory_order_release);
        impl_->cv.notify_all();
    }
}

void DockSystemMonitor::SetPinnedTrayKeys(const std::vector<std::wstring>& keys) {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->pinned_keys = keys;
    }
    impl_->want_slow.store(true, std::memory_order_release);
    impl_->cv.notify_all();
}

void DockSystemMonitor::SetForegroundHint(HWND hwnd) noexcept {
    if (!impl_) return;
    impl_->fg_hint.store(hwnd, std::memory_order_release);
}

void DockSystemMonitor::RequestRefresh(bool include_slow) noexcept {
    if (!impl_) return;
    impl_->want_fast.store(true, std::memory_order_release);
    if (include_slow) impl_->want_slow.store(true, std::memory_order_release);
    impl_->cv.notify_all();
}

DockSystemSnapshot DockSystemMonitor::Snapshot() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->snapshot;
}

} // namespace pulse::desktop::live
