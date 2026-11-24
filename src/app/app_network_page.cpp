// app_network_page.cpp - see app_network_page.h.
#include "app_network_page.h"

#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "app_network_locations.h"
#include "app_sidebar_refresh.h"
#include "context_menu.h"
#include "lan_discovery.h"
#include "network_locations.h"
#include "network_recent.h"
#include "selection_caps.h"
#include "server_address.h"
#include "session.h"
#include "window_registry.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"
#include "../ui/confirm_dialog.h"
#include "../ui/connect_server_dialog.h"
#include "../ui/fluent_menu.h"

#include <winnetwk.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse {

namespace {
constexpr DWORD kLanBudgetMs = 2500;
constexpr ULONGLONG kPendingPinMs = 2ull * 60 * 1000;  // 连接后固定: the listing must open within 2 min

struct LanScan {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    app::LanDiscoveryResult result;
    bool reused = false;  // same network as the cached answer: nothing was sent
    bool ready = false;
};
}  // namespace

struct NetworkPageLoad {
    app::NetworkRecentList recent;
    bool recent_loaded = false;
    std::shared_ptr<LanScan> scan;  // in flight (UI thread only)
    std::vector<app::LanDevice> devices;
    std::wstring network_key;
    ULONGLONG scanned_at = 0;
    bool scanned = false;
    bool public_network = false;
    bool offline = false;
    std::wstring network_name;     // INetwork::GetName, for the header note
    uint8_t network_category = 0;  // 0 unknown, 1 private, 2 domain, 3 public
    // 连接后固定 without mapping: pinned when its listing opens (RecordNetworkVisit).
    std::wstring pending_pin;
    ULONGLONG pending_pin_at = 0;
    // Rail badge: device keys (lower-case \\host) the user has seen on the page.
    std::vector<std::wstring> seen;
    bool seen_loaded = false;
};

namespace {
NetworkPageLoad& Page(AppState& s) {
    if (!s.networkPage) s.networkPage = std::make_shared<NetworkPageLoad>();
    return *s.networkPage;
}

std::wstring RecentFile() {
    const std::wstring dir = app::GetPulseDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\network_recent.json";
}

app::NetworkRecentList& Recent(AppState& s) {
    NetworkPageLoad& page = Page(s);
    if (!page.recent_loaded) {
        page.recent_loaded = true;
        std::wstring text;
        // A few hundred bytes in the local data folder, read once.
        if (const std::wstring file = RecentFile(); !file.empty() && ReadUtf8File(file, text))
            page.recent.FromJson(text);
    }
    return page.recent;
}

void SaveRecent(AppState& s) {
    const std::wstring file = RecentFile();
    if (file.empty()) return;
    s.worker.EnqueueIo([file, json = Recent(s).ToJson()] { WriteUtf8FileAtomic(file, json); });
}

ULONGLONG LanCacheMs(const AppState& s) {
    switch (s.appPrefs.lan_cache_mode) {
    case 1: return 60ull * 60 * 1000;
    case 2: return ~0ull;  // until Pulse closes
    default: return 10ull * 60 * 1000;
    }
}

std::wstring SeenFile() {
    const std::wstring dir = app::GetPulseDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\network_seen.txt";
}

std::wstring DeviceKey(const app::LanDevice& device) {
    std::wstring key = app::LanDeviceUnc(device);
    for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
    return key;
}

std::vector<std::wstring>& Seen(AppState& s) {
    NetworkPageLoad& page = Page(s);
    if (!page.seen_loaded) {
        page.seen_loaded = true;
        std::wstring text;
        if (const std::wstring file = SeenFile(); !file.empty() && ReadUtf8File(file, text)) {
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find(L'\n', start);
                if (end == std::wstring::npos) end = text.size();
                std::wstring line = text.substr(start, end - start);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (!line.empty()) page.seen.push_back(std::move(line));
                start = end + 1;
            }
        }
    }
    return page.seen;
}

uint64_t NowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring Leaf(const std::wstring& unc) {
    std::wstring path = unc;
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    const size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

bool Listed(const std::vector<fs::DirEntry>& entries, const std::wstring& path) {
    return std::any_of(entries.begin(), entries.end(), [&](const fs::DirEntry& e) {
        return app::SameNetworkLocation(e.full_path, path);
    });
}

std::wstring DeviceTypeText(const app::LanDevice& device) {
    std::wstring text = device.model.empty() ? l10n::Get(l10n::StringId::LanDeviceGeneric) : device.model;
    const std::wstring address = !device.ip.empty() ? device.ip : device.host;
    if (!address.empty()) text += L" \u00B7 " + address;
    std::wstring via;
    if (device.sources & app::kLanSourceMdns) via = L"mDNS";
    if (device.sources & app::kLanSourceWsd) via += via.empty() ? L"WSD" : L" + WSD";
    if (!via.empty()) text += L" \u00B7 " + via;
    return text;
}

void RunCommandLine(AppState& s, const wchar_t* exe, const wchar_t* args) {
    wchar_t system[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (!n || n >= ARRAYSIZE(system)) return;
    s.ops.OpenProgramIn(std::wstring(system) + L"\\" + exe, std::wstring(), args);
}

std::wstring DisplayFtpUrl(const app::ServerAddress& address) {
    if (!address.url.starts_with(L"ftp://")) return address.url;
    const size_t slash = address.url.find(L'/', 6);
    const size_t at = address.url.find(L'@', 6);
    if (at == std::wstring::npos || (slash != std::wstring::npos && at >= slash)) return address.url;
    const std::wstring prefix = address.user.empty() ? L"ftp://" : L"ftp://" + address.user + L"@";
    return prefix + address.url.substr(at + 1);
}

void OpenFtpInExplorer(AppState& s, const std::wstring& url) {
    if (url.empty()) return;
    wchar_t windows_dir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windows_dir, ARRAYSIZE(windows_dir));
    if (!n || n >= ARRAYSIZE(windows_dir)) return;
    s.ops.OpenProgramIn(std::wstring(windows_dir) + L"\\explorer.exe",
                        std::wstring(), L"\"" + url + L"\"");
}

void ExplainAddress(AppState& s, const app::ServerAddress& address) {
    std::wstring message;
    if (address.kind == app::ServerAddressKind::Unsupported || address.kind == app::ServerAddressKind::WebDav) {
        wchar_t text[512]{};
        swprintf_s(text, l10n::Get(l10n::StringId::ServerAddressUnsupportedFormat).c_str(),
                   address.scheme.empty() ? L"?" : address.scheme.c_str());
        message = text;
    } else {
        message = l10n::Get(l10n::StringId::ServerAddressInvalid);
    }
    s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::ConnectServer), std::move(message));
}

void SaveSeen(AppState& s) {
    const std::wstring file = SeenFile();
    if (file.empty()) return;
    std::wstring text;
    const auto& seen = Seen(s);
    // The last 200 devices are plenty for one PC's networks.
    for (size_t i = seen.size() > 200 ? seen.size() - 200 : 0; i < seen.size(); ++i) text += seen[i] + L"\r\n";
    s.worker.EnqueueIo([file, text] { WriteUtf8FileAtomic(file, text); });
}

// Free drive letters for 同时映射为驱动器, Z: downward like Explorer: not in
// use and not remembered for a disconnected mapping. Local registry reads only.
std::vector<wchar_t> FreeDriveLetters(size_t limit) {
    std::vector<wchar_t> letters;
    const DWORD used = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D' && letters.size() < limit; --letter) {
        if (used & (1u << (letter - L'A'))) continue;
        const wchar_t local[3] = {letter, L':', 0};
        wchar_t remote[MAX_PATH]{};
        DWORD size = ARRAYSIZE(remote);
        const DWORD state = WNetGetConnectionW(local, remote, &size);
        if (state == NO_ERROR || state == ERROR_CONNECTION_UNAVAIL || state == ERROR_MORE_DATA) continue;
        letters.push_back(letter);
    }
    return letters;
}

void PinAndProbe(AppState& s, const std::wstring& unc) {
    if (unc.empty() || s.places.FindNetwork(unc) >= 0) return;
    s.places.PinNetwork(unc, L"");
    RequestUncProbe(s, unc);
}

bool AnyWindowShowsNetworkPage(AppState& s) {
    bool shown = false;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        ForEachPane(window, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath) shown = true;
        });
    });
    return shown;
}
}  // namespace

bool IsConnectServerPage(const std::wstring& path) {
    return path == kNetworkLocationsPath || app::IsUncServerRoot(path);
}

namespace {
struct ServerCandidate { std::wstring path, name, hint; };

// Pins, LAN devices, recent visits: what 连接服务器 offers for `raw`.
std::vector<ServerCandidate> ServerCandidates(AppState& s, const std::wstring& raw) {
    std::wstring needle = raw;
    while (!needle.empty() && (iswspace(needle.front()) || needle.front() == L'\\' || needle.front() == L'/'))
        needle.erase(needle.begin());
    if (needle.rfind(L"smb:", 0) == 0) {
        needle.erase(0, 4);
        while (!needle.empty() && needle.front() == L'/') needle.erase(needle.begin());
    }
    for (auto& c : needle) if (c == L'/') c = L'\\';
    needle = Lower(needle);
    std::vector<ServerCandidate> candidates;
    const auto add = [&](const std::wstring& path, const std::wstring& name, const std::wstring& hint) {
        if (path.empty()) return;
        for (const auto& c : candidates) if (app::SameNetworkLocation(c.path, path)) return;
        const std::wstring bare = Lower(path.size() > 2 ? path.substr(2) : path);
        if (!needle.empty() && bare.rfind(needle, 0) != 0 && Lower(name).find(needle) == std::wstring::npos) return;
        candidates.push_back({path, name, hint});
    };
    for (const auto& pin : s.places.networks)
        add(app::NetworkLocationKey(pin.unc), pin.name, l10n::Get(l10n::StringId::GroupNetworkPinned));
    NetworkPageLoad& page = Page(s);
    for (const auto& device : page.devices)
        add(app::LanDeviceUnc(device), device.name, l10n::Get(l10n::StringId::GroupNetworkDevices));
    if (s.appPrefs.network_recent_enabled)
        for (const auto& item : Recent(s).Items())
            add(item.path, std::wstring(), l10n::Get(l10n::StringId::GroupNetworkRecent));
    return candidates;
}

ui::ConnectServerPreview PreviewServerAddress(const std::wstring& text) {
    ui::ConnectServerPreview preview;
    std::wstring trimmed = text;
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    if (trimmed.empty() || trimmed == L"\\\\" || trimmed == L"\\") return preview;
    const app::ServerAddress address = app::ParseServerAddress(trimmed, true);
    wchar_t line[512]{};
    switch (address.kind) {
    case app::ServerAddressKind::Server:
        swprintf_s(line, l10n::Get(l10n::StringId::ConnectServerHostFormat).c_str(), address.unc.c_str());
        preview.ok = true;
        break;
    case app::ServerAddressKind::Share:
        swprintf_s(line, l10n::Get(l10n::StringId::ConnectServerUnderstoodFormat).c_str(), address.unc.c_str());
        preview.ok = true;
        preview.can_map = app::UncShareRoot(address.unc) == address.unc;
        break;
    case app::ServerAddressKind::Ftp:
        swprintf_s(line, l10n::Get(l10n::StringId::ConnectServerFtpFormat).c_str(),
                   DisplayFtpUrl(address).c_str());
        preview.ok = true;
        preview.can_pin = false;
        preview.can_map = false;
        break;
    case app::ServerAddressKind::WebDav:
    case app::ServerAddressKind::Unsupported:
        swprintf_s(line, l10n::Get(l10n::StringId::ServerAddressUnsupportedFormat).c_str(),
                   address.scheme.empty() ? L"?" : address.scheme.c_str());
        break;
    default:
        wcscpy_s(line, l10n::Get(l10n::StringId::ServerAddressInvalid).c_str());
        break;
    }
    preview.text = line;
    return preview;
}

void StartMapDrive(AppState& s, const std::wstring& unc, wchar_t letter, bool pin) {
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, unc, letter, pin] {
        const wchar_t local[3] = {letter, L':', 0};
        NETRESOURCEW resource{};
        resource.dwType = RESOURCETYPE_DISK;
        resource.lpLocalName = const_cast<wchar_t*>(local);
        resource.lpRemoteName = const_cast<wchar_t*>(unc.c_str());
        // Saved credentials first; Windows' own prompt only when the server asks.
        auto result = std::make_unique<NetworkMapResult>();
        result->error = WNetAddConnection3W(hwnd, &resource, nullptr, nullptr,
                                            CONNECT_INTERACTIVE | CONNECT_UPDATE_PROFILE);
        result->letter = letter;
        result->unc = unc;
        result->pin = pin;
        if (PostMessageW(hwnd, WM_NETWORK_MAP_DONE, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    }).detach();
}
}  // namespace

void RunConnectServerDialog(AppState& s) {
    RequestLanDiscovery(s, false);  // the suggestions list devices as they arrive
    ui::ConnectServerDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::ConnectServer);
    spec.label = l10n::Get(l10n::StringId::ConnectServerAddressLabel);
    spec.confirm_text = l10n::Get(l10n::StringId::ConnectButton);
    spec.initial_text = L"\\\\";
    spec.pin = true;
    spec.free_letters = FreeDriveLetters(5);
    spec.preview = PreviewServerAddress;
    spec.suggestions = [&s](const std::wstring& text) {
        std::vector<ui::ConnectServerSuggestion> rows;
        for (const auto& c : ServerCandidates(s, text)) {
            const std::wstring hint = c.name.empty() || c.name == Leaf(c.path) ? c.hint : c.name + L" \u00B7 " + c.hint;
            rows.push_back({c.path, hint});
            if (rows.size() >= 4) break;
        }
        return rows;
    };
    const auto result = ui::ShowConnectServerDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (!result.accepted) return;
    const app::ServerAddress address = app::ParseServerAddress(result.text, true);
    if (address.kind == app::ServerAddressKind::Ftp) {
        OpenFtpInExplorer(s, address.url);
        return;
    }
    if (address.kind != app::ServerAddressKind::Server && address.kind != app::ServerAddressKind::Share) {
        ExplainAddress(s, address);
        return;
    }
    if (result.map && result.letter) {
        StartMapDrive(s, address.unc, result.letter, result.pin);
        return;
    }
    NetworkPageLoad& page = Page(s);
    page.pending_pin = result.pin ? address.unc : std::wstring();
    page.pending_pin_at = GetTickCount64();
    NavigateTo(s, address.unc);
}

void ApplyNetworkMapDone(AppState& s, const NetworkMapResult& result) {
    if (result.error != NO_ERROR) {
        ShowMapDriveError(s, result.error);
        return;
    }
    if (result.pin) PinAndProbe(s, result.unc);
    RefreshPath(s, L"", RefreshReason::Explicit);  // This PC
    RequestSidebarRefresh(s);
    RequestNetworkLocations(s);  // 网络驱动器 on the network page
    NavigateTo(s, result.unc);
}

void ShowMapDriveError(AppState& s, DWORD error) {
    if (error == ERROR_CANCELLED || error == static_cast<DWORD>(-1)) return;
    ui::ConfirmDialogSpec spec;
    spec.tone = ui::ConfirmTone::Warning;
    const std::wstring title = l10n::Get(l10n::StringId::CouldNotMapNetworkDrive);
    spec.title = title.c_str();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    const std::wstring message = text ? text : l10n::Get(l10n::StringId::WindowsCouldNotOpenTheNetworkDriveDialog);
    if (text) LocalFree(text);
    spec.message = message.c_str();
    wchar_t code[64]{};
    swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), error);
    spec.note = code;
    const std::wstring close = l10n::Get(l10n::StringId::Close);
    spec.confirm_text = close.c_str();
    (void)ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
}

std::wstring SelectedNetworkPin(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != kNetworkLocationsPath || tab->SelectedCount() != 1 || !tab->snapshot) return {};
    const int index = tab->selected_index;
    if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount()) return {};
    const fs::DirEntry e = tab->EntryAt(static_cast<size_t>(index));
    return s.places.FindNetwork(e.full_path) >= 0 ? e.full_path : std::wstring();
}

void RenameNetworkPin(AppState& s, const std::wstring& unc) {
    const int index = s.places.FindNetwork(unc);
    if (index < 0) return;
    ui::ConnectServerDialogSpec spec;
    spec.rename = true;
    spec.title = l10n::Get(l10n::StringId::NetworkRenameTitle);
    spec.label = l10n::Get(l10n::StringId::NetworkRenameLabel);
    spec.note = l10n::Get(l10n::StringId::NetworkRenameNote);
    spec.confirm_text = l10n::Get(l10n::StringId::Rename);
    spec.initial_text = s.places.networks[static_cast<size_t>(index)].name;
    const auto result = ui::ShowConnectServerDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (!result.accepted) return;
    std::wstring name = result.text;
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (!s.places.RenameNetwork(unc, name)) return;
    RefreshNetworkPages(s);
    app::ForEachWindowSharingCore(s, [](AppState& window) { InvalidateRect(window.hwnd, nullptr, FALSE); });
}

void BeginConnectServer(AppState& s) {
    ShowAddressEditor(s);
    if (!s.addressEditing || !s.hwndAddressEdit) return;
    s.addressConnectMode = true;
    SetWindowTextW(s.hwndAddressEdit, L"\\\\");
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 2, 2);
    UpdateAddressSuggestions(s);
}

void ShowConnectServerMenu(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto anchor = s.renderer.NewCommandRect(static_cast<float>(s.compositor.Width()));
    POINT pt{static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.bottom)};
    ClientToScreen(s.hwnd, &pt);
    const auto row = [](int command, l10n::StringId text, const wchar_t* glyph) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = l10n::Get(text);
        item.glyph = glyph;
        return item;
    };
    std::vector<ui::FluentMenuItem> items;
    items.push_back(row(app::CmdConnectServer, l10n::StringId::ConnectServerMenu, L"\xE968"));
    items.push_back(row(app::CmdMapNetworkDrive, l10n::StringId::MapNetworkDrive, L"\xE8CE"));
    items.push_back(row(app::CmdAddNetworkPlace, l10n::StringId::NetworkAddWindowsPlace, L"\xE710"));
    items.back().separator_after = true;
    items.push_back(row(app::CmdLanRescan, l10n::StringId::NetworkRescan, L"\xE72C"));
    items.push_back(row(app::CmdNetworkSharingSettings, l10n::StringId::NetworkSharingSettings, L"\xE713"));
    {
        POINT tl{static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.top)};
        POINT br{static_cast<LONG>(anchor.right), static_cast<LONG>(anchor.bottom)};
        ClientToScreen(s.hwnd, &tl);
        ClientToScreen(s.hwnd, &br);
        s.menu->SetMorphFrom(RECT{tl.x, tl.y, br.x, br.y});
    }
    const int cmd = s.menu->TrackPopup(pt, std::move(items));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

bool HandleNetworkPageCommand(AppState& s, int cmd) {
    switch (cmd) {
    case app::CmdConnectServer: RunConnectServerDialog(s); return true;
    case app::CmdForgetNetworkRecent: ForgetSelectedNetworkRecent(s); return true;
    case app::CmdLanRescan: RequestLanDiscovery(s, true); return true;
    case app::CmdRenameNetwork:
        if (const std::wstring pin = SelectedNetworkPin(s); !pin.empty()) RenameNetworkPin(s, pin);
        return true;
    case app::CmdNetworkClearRecent:
        Recent(s).Clear();
        SaveRecent(s);
        RefreshNetworkPages(s);
        return true;
    case app::CmdNetworkHiddenShares: HandleNetworkSettingToggle(s, 42); return true;
    case app::CmdNetworkDiscoverySettings: OpenSettingsTab(s, 12); return true;
    // The Windows wizard creates a NetHood shortcut; the page re-reads them.
    case app::CmdAddNetworkPlace: RunCommandLine(s, L"rundll32.exe", L"shwebsvc.dll,AddNetPlaceRunDll"); return true;
    case app::CmdNetworkSharingSettings:
        RunCommandLine(s, L"control.exe", L"/name Microsoft.NetworkAndSharingCenter /page Advanced");
        return true;
    default: return false;
    }
}

bool OpenServerAddressText(AppState& s, const std::wstring& text) {
    const bool connect = s.addressConnectMode;
    const bool url = app::IsServerUrlText(text) || text.find(L"://") != std::wstring::npos;
    if (!connect && !app::IsServerUrlText(text)) return false;
    const app::ServerAddress address = app::ParseServerAddress(text, connect);
    switch (address.kind) {
    case app::ServerAddressKind::Server:
    case app::ServerAddressKind::Share:
        NavigateTo(s, address.unc);
        return true;
    case app::ServerAddressKind::Ftp:
        OpenFtpInExplorer(s, address.url);
        return true;
    case app::ServerAddressKind::WebDav:
    case app::ServerAddressKind::Unsupported:
        ExplainAddress(s, address);
        return true;
    default:
        if (!url) return false;  // connect mode, but a local path: just go there
        ExplainAddress(s, address);
        return true;
    }
}

bool FillServerSuggestions(AppState& s, const std::wstring& raw) {
    std::wstring text = raw;
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    const bool unc = text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\';
    if (!unc && !s.addressConnectMode) return false;
    if (!unc && (text.find_first_of(L":%") != std::wstring::npos)) return false;
    // "\\" alone: look for devices now (a recent answer is reused), so the
    // list fills in while the user types the rest.
    if (unc && text.find_first_not_of(L"\\/", 2) == std::wstring::npos) RequestLanDiscovery(s, false);
    std::vector<ServerCandidate> candidates = ServerCandidates(s, text);
    if (candidates.empty()) return unc;  // a typed UNC path: no alias rows either
    s.addressSuggestHeader = l10n::Get(l10n::StringId::ConnectServerHint);
    for (auto& c : candidates) {
        if (s.addressSuggest.size() >= 8) break;
        std::wstring hint = c.name.empty() || c.name == Leaf(c.path) ? c.hint : c.name + L" \u00B7 " + c.hint;
        std::wstring label = c.path;
        s.addressSuggest.push_back({std::move(label), std::move(c.path), std::move(hint), false});
    }
    s.addressSuggestSel = -1;  // Enter opens what was typed; arrows pick a row
    return true;
}

void RecordNetworkVisit(AppState& s, const std::wstring& path) {
    if (s.networkPage && !s.networkPage->pending_pin.empty()) {
        NetworkPageLoad& page = *s.networkPage;
        const std::wstring root = app::UncShareRoot(path);
        if (GetTickCount64() - page.pending_pin_at > kPendingPinMs) {
            page.pending_pin.clear();
        } else if (app::SameNetworkLocation(page.pending_pin, root.empty() ? path : root)) {
            const std::wstring pin = std::move(page.pending_pin);
            page.pending_pin.clear();
            PinAndProbe(s, pin);
            RefreshNetworkPages(s);
        }
    }
    if (!s.appPrefs.network_recent_enabled) return;
    if (!Recent(s).Touch(path, NowTicks())) return;
    SaveRecent(s);
}

bool SelectionHasNetworkRecent(const app::Tab& tab) {
    if (tab.current_path != kNetworkLocationsPath || !tab.snapshot) return false;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        if (tab.EntryAt(static_cast<size_t>(index)).net_group == 4) return true;
    }
    return false;
}

bool ForgetSelectedNetworkRecent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !SelectionHasNetworkRecent(*tab)) return false;
    bool changed = false;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount()) continue;
        const fs::DirEntry e = tab->EntryAt(static_cast<size_t>(index));
        if (e.net_group == 4) changed = Recent(s).Remove(e.full_path) || changed;
    }
    if (!changed) return false;
    SaveRecent(s);
    RefreshNetworkPages(s);
    return true;
}

void AppendNetworkPageEntries(AppState& s, std::vector<fs::DirEntry>& entries) {
    NetworkPageLoad& page = Page(s);
    for (const auto& device : page.devices) {
        const std::wstring unc = app::LanDeviceUnc(device);
        if (unc.empty() || Listed(entries, unc)) continue;
        fs::DirEntry entry;
        entry.name = device.name.empty() ? unc.substr(2) : device.name;
        entry.full_path = unc;
        entry.is_dir = true;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.net_group = 3;
        entry.net_kind = app::LanDeviceKind(device);
        entry.change_type_text = DeviceTypeText(device);
        entries.push_back(std::move(entry));
    }
    if (!s.appPrefs.network_recent_enabled) return;
    for (const auto& item : Recent(s).Items()) {
        if (Listed(entries, item.path)) continue;
        fs::DirEntry entry;
        entry.name = Leaf(item.path);
        entry.full_path = item.path;
        entry.is_dir = true;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.mtime.dwLowDateTime = static_cast<DWORD>(item.last_visit);
        entry.mtime.dwHighDateTime = static_cast<DWORD>(item.last_visit >> 32);
        entry.net_group = 4;
        entry.change_type_text = item.path;
        entries.push_back(std::move(entry));
    }
}

std::wstring NetworkPageStatus(AppState& s, const std::vector<fs::DirEntry>& entries) {
    int counts[5]{};
    for (const auto& e : entries) if (e.net_group < 5) ++counts[e.net_group];
    std::wstring text;
    const auto part = [&](l10n::StringId format, int count) {
        if (count <= 0) return;
        wchar_t buffer[128]{};
        swprintf_s(buffer, l10n::Get(format).c_str(), count);
        if (!text.empty()) text += L" \u00B7 ";
        text += buffer;
    };
    part(l10n::StringId::NetworkStatusPinnedFormat, counts[1]);
    part(l10n::StringId::NetworkStatusDrivesFormat, counts[2]);
    part(l10n::StringId::NetworkStatusRecentFormat, counts[4]);
    // With hints on, the LAN state is the hint at the right (NetworkLanHint).
    if (s.appPrefs.show_hints) return text;
    const NetworkPageLoad& page = Page(s);
    std::wstring lan;
    if (page.scan) lan = l10n::Get(l10n::StringId::LanDiscoveryScanning);
    else if (!s.appPrefs.lan_discovery && !page.scanned) lan = l10n::Get(l10n::StringId::LanDiscoveryDisabled);
    else if (page.offline) lan = l10n::Get(l10n::StringId::LanDiscoveryOffline);
    else if (page.public_network) lan = l10n::Get(l10n::StringId::LanDiscoveryPublic);
    else if (page.scanned && page.devices.empty()) lan = l10n::Get(l10n::StringId::LanDiscoveryNone);
    else if (page.scanned) {
        wchar_t buffer[128]{};
        swprintf_s(buffer, l10n::Get(l10n::StringId::LanDiscoveryFoundFormat).c_str(), static_cast<int>(page.devices.size()));
        lan = buffer;
    }
    if (!lan.empty()) text += (text.empty() ? L"" : L" \u00B7 ") + lan;
    return text;
}

void RequestLanDiscovery(AppState& s, bool force) {
    NetworkPageLoad& page = Page(s);
    if (page.scan) return;  // one look at a time
    if (!force && !s.appPrefs.lan_discovery) return;  // 自动发现 off: only 重新发现 / F5 look
    const bool fresh = !force && page.scanned && GetTickCount64() - page.scanned_at < LanCacheMs(s);
    auto scan = std::make_shared<LanScan>();
    page.scan = scan;
    const HWND hwnd = ServiceWindow(s);
    const std::wstring cached_key = fresh ? page.network_key : std::wstring();
    const bool isolated = s.isolatedTest;  // tests never send multicast
    s.worker.EnqueueIo([scan, hwnd, cached_key, fresh, isolated] {
        app::LanDiscoveryResult result;
        bool reused = false;
        if (isolated) {
            result.offline = true;
        } else if (fresh) {
            app::ReadLanNetworkIdentity(result);
            reused = !result.network_key.empty() && result.network_key == cached_key;
        }
        if (!isolated && !reused && !scan->cancelled)
            result = app::RunLanDiscovery([scan] { return scan->cancelled.load(); }, kLanBudgetMs);
        if (scan->cancelled) return;
        {
            std::lock_guard lock(scan->mutex);
            scan->result = std::move(result);
            scan->reused = reused;
            scan->ready = true;
        }
        if (hwnd) PostMessageW(hwnd, WM_LAN_DISCOVERY, 0, 0);
    });
    RefreshNetworkPages(s);  // 正在发现局域网设备…
}

void ApplyLanDiscovery(AppState& s) {
    if (!s.networkPage || !s.networkPage->scan) return;
    NetworkPageLoad& page = *s.networkPage;
    const auto scan = page.scan;
    {
        std::lock_guard lock(scan->mutex);
        if (!scan->ready) return;
        page.scan.reset();
        if (scan->result.cancelled) return;
        page.network_name = scan->result.network_name;
        page.network_category = scan->result.network_category;
        if (!scan->reused) {
            page.devices = std::move(scan->result.devices);
            page.network_key = scan->result.network_key;
            page.public_network = scan->result.public_network;
            page.offline = scan->result.offline;
            page.scanned_at = GetTickCount64();
            page.scanned = true;
        }
    }
    RefreshNetworkPages(s);
}

void CancelLanDiscovery(AppState& s) {
    if (s.networkPage && s.networkPage->scan) s.networkPage->scan->cancelled = true;
}

bool HandleNetworkSettingToggle(AppState& s, int index) {
    auto& prefs = s.appPrefs;
    switch (index) {
    case 40:
        prefs.lan_discovery = !prefs.lan_discovery;
        if (!prefs.lan_discovery) CancelLanDiscovery(s);
        break;
    case 41:
        prefs.network_recent_enabled = !prefs.network_recent_enabled;
        break;
    case 42:
        prefs.show_hidden_shares = !prefs.show_hidden_shares;
        ApplyNetworkPrefs(s);
        // Open share lists re-read with or without the $ shares.
        app::ForEachWindowSharingCore(s, [](AppState& window) {
            ForEachPane(window, [&](app::Pane& pane) {
                if (auto* tab = pane.ActiveTab(); tab && app::IsUncServerRoot(tab->current_path))
                    RefreshPath(window, tab->current_path, RefreshReason::Explicit);
            });
        });
        break;
    case 46: {
        // 清除网络记录: recent visits, found devices and the badge's memory.
        NetworkPageLoad& page = Page(s);
        CancelLanDiscovery(s);
        Recent(s).Clear();
        SaveRecent(s);
        page.devices.clear();
        page.scanned = false;
        page.scanned_at = 0;
        page.network_key.clear();
        Seen(s).clear();
        SaveSeen(s);
        s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::SidebarNetworkLocations),
                                  l10n::Get(l10n::StringId::NetworkHistoryCleared), false);
        RefreshNetworkPages(s);
        return true;
    }
    default:
        return false;
    }
    prefs.Save();
    RefreshNetworkPages(s);
    return true;
}

void SetLanCacheMode(AppState& s, int mode) {
    if (mode < 0 || mode > 2 || s.appPrefs.lan_cache_mode == mode) return;
    s.appPrefs.lan_cache_mode = mode;
    s.appPrefs.Save();
}

void ApplyNetworkPrefs(AppState& s) {
    fs::SetShowHiddenShares(s.appPrefs.show_hidden_shares);
}

void AppendNetworkMoreItems(AppState& s, std::vector<ui::FluentMenuItem>& items) {
    const auto row = [](int command, l10n::StringId text, const wchar_t* glyph) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = l10n::Get(text);
        item.glyph = glyph;
        return item;
    };
    std::vector<ui::FluentMenuItem> own;
    own.push_back(row(app::CmdLanRescan, l10n::StringId::NetworkRescan, L"\xE72C"));
    own.push_back(row(app::CmdNetworkClearRecent, l10n::StringId::NetworkClearRecent, L"\xE81C"));
    own.back().enabled = s.appPrefs.network_recent_enabled && !Recent(s).Items().empty();
    own.push_back(row(app::CmdNetworkHiddenShares, l10n::StringId::NetworkShowHiddenShares, L"\xED1A"));
    own.back().toggle = true;
    own.back().checked = s.appPrefs.show_hidden_shares;
    own.push_back(row(app::CmdNetworkDiscoverySettings, l10n::StringId::NetworkDiscoverySettingsMenu, L"\xE713"));
    if (!items.empty()) own.back().separator_after = true;
    items.insert(items.begin(), own.begin(), own.end());
}

std::wstring NetworkLanHint(AppState& s) {
    const NetworkPageLoad& page = Page(s);
    if (page.scan) return l10n::Get(l10n::StringId::LanDiscoveryScanning);
    if (page.offline) return l10n::Get(l10n::StringId::LanDiscoveryOffline);
    if (page.public_network) return l10n::Get(l10n::StringId::LanDiscoveryPublic);
    if (!page.scanned) return s.appPrefs.lan_discovery ? std::wstring() : l10n::Get(l10n::StringId::LanDiscoveryDisabled);
    std::wstring text;
    if (page.devices.empty()) {
        text = l10n::Get(l10n::StringId::LanDiscoveryNone);
    } else {
        wchar_t buffer[128]{};
        swprintf_s(buffer, l10n::Get(l10n::StringId::LanDiscoveryFoundFormat).c_str(), static_cast<int>(page.devices.size()));
        text = buffer;
    }
    const ULONGLONG minutes = (GetTickCount64() - page.scanned_at) / 60000ull;
    text += L" \u00B7 ";
    if (minutes < 1) {
        text += l10n::Get(l10n::StringId::LanScannedJustNow);
    } else {
        wchar_t buffer[64]{};
        swprintf_s(buffer, l10n::Get(l10n::StringId::LanScannedMinutesFormat).c_str(), static_cast<int>(minutes));
        text += buffer;
    }
    return text;
}

int NetworkRailBadge(AppState& s) {
    if (!s.networkPage || s.networkPage->devices.empty()) return 0;
    NetworkPageLoad& page = *s.networkPage;
    auto& seen = Seen(s);
    const bool showing = AnyWindowShowsNetworkPage(s);
    int unseen = 0;
    bool added = false;
    for (const auto& device : page.devices) {
        const std::wstring key = DeviceKey(device);
        if (key.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        if (showing) {
            seen.push_back(key);
            added = true;
        } else {
            ++unseen;
        }
    }
    if (added) SaveSeen(s);
    return unseen;
}

std::wstring NetworkHeaderNote(AppState& s) {
    const NetworkPageLoad& page = Page(s);
    if (page.network_category == 0) return {};
    const l10n::StringId category = page.network_category == 2 ? l10n::StringId::NetworkCategoryDomain
        : page.network_category == 3 ? l10n::StringId::NetworkCategoryPublic : l10n::StringId::NetworkCategoryPrivate;
    return page.network_name.empty() ? l10n::Get(category) : page.network_name + L" \u00B7 " + l10n::Get(category);
}

void RefreshNetworkPages(AppState& s) {
    if (!AnyWindowShowsNetworkPage(s)) return;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        bool any = false;
        ForEachPane(window, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath) {
                FillNetworkLocationsView(window, *tab);
                any = true;
            }
        });
        if (any) InvalidateRect(window.hwnd, nullptr, FALSE);
    });
}

} // namespace pulse
