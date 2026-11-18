// app_network_page.h - The network page beyond the pins:
// 连接服务器 in the New button's slot and the address bar, 最近访问, and the
// 局域网设备 found by lan_discovery. Window-side; the data is shared through
// AppCore::networkPage so every window sees the same lists.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
namespace fs { struct DirEntry; }
namespace ui { struct FluentMenuItem; }
struct NetworkPageLoad;

// The network page and a server's share list put 连接服务器 where 新建 is.
bool IsConnectServerPage(const std::wstring& path);
// 连接服务器 (connect_server_dialog.h): address, 将被理解为, suggestions,
// 连接后固定 and 同时映射为驱动器. Mapping runs on its own thread (Windows may
// ask for credentials) and ends in ApplyNetworkMapDone.
void RunConnectServerDialog(AppState& s);
// The old address-bar flow, kept for typing "\\" there.
void BeginConnectServer(AppState& s);

// WM_NETWORK_MAP_DONE: LPARAM is a NetworkMapResult* (the receiver deletes it).
struct NetworkMapResult {
    DWORD error = 0;
    wchar_t letter = 0;
    std::wstring unc;
    bool pin = false;
};
void ApplyNetworkMapDone(AppState& s, const NetworkMapResult& result);
// 无法映射网络驱动器 with the Windows message and code; nothing for a cancel.
void ShowMapDriveError(AppState& s, DWORD error);

// 重命名 for a pinned network location: Pulse's display name only.
void RenameNetworkPin(AppState& s, const std::wstring& unc);
// The one selected pin on the network page, empty otherwise.
std::wstring SelectedNetworkPin(AppState& s);

// Settings → 文件操作 → 网络位置: toggles 40 自动发现, 41 记录最近访问,
// 42 显示隐藏共享 and button 46 清除网络记录. False for other indices.
bool HandleNetworkSettingToggle(AppState& s, int index);
// 发现结果保留: 0 10 min, 1 1 h, 2 this session.
void SetLanCacheMode(AppState& s, int mode);
// Pushes the prefs that live below the app (显示隐藏共享) to fs; at startup
// and after a settings import.
void ApplyNetworkPrefs(AppState& s);
// The network page's ⋯ items (重新发现 / 清除最近访问 / 显示隐藏共享 /
// 局域网发现设置…), put before the toolbar's own.
void AppendNetworkMoreItems(AppState& s, std::vector<ui::FluentMenuItem>& items);

// Status bar hint on the network page: "局域网 3 台设备 · 2 分钟前"; empty when
// there is nothing to say (the hint falls back to the idle one).
std::wstring NetworkLanHint(AppState& s);
// Rail badge on 网络位置: devices found but not seen yet. Devices count as
// seen while any window shows the network page (remembered in network_seen.txt).
int NetworkRailBadge(AppState& s);
// The pane header's note: "Home-5G · 专用网络", empty when unknown.
std::wstring NetworkHeaderNote(AppState& s);
// The button's ▾: 连接服务器… / 映射网络驱动器… / 添加 Windows 网络位置… /
// 重新发现局域网设备 / 网络和共享设置.
void ShowConnectServerMenu(AppState& s);
// Commands from the network page's menus (CmdConnectServer ...). False when
// `cmd` is not one of them.
bool HandleNetworkPageCommand(AppState& s, int cmd);

// Address bar text that names a server: smb:// (and other schemes, which get
// an explanation), a bare IP, or anything typed after 连接服务器. True when
// handled; false leaves the text to ordinary navigation.
bool OpenServerAddressText(AppState& s, const std::wstring& text);
// Server suggestions under the address bar for "\\..." (and in connect mode):
// pins, recent servers, LAN devices from the cache, mapped drives. Never
// touches the network. True when it owns the suggestion list.
bool FillServerSuggestions(AppState& s, const std::wstring& text);

// A UNC listing opened in a window: remember its share root (最近访问).
void RecordNetworkVisit(AppState& s, const std::wstring& path);
bool SelectionHasNetworkRecent(const app::Tab& tab);
bool ForgetSelectedNetworkRecent(AppState& s);

// The network page's rows after the pins and drives: LAN devices, then recent
// visits that are not already listed above.
void AppendNetworkPageEntries(AppState& s, std::vector<fs::DirEntry>& entries);
// Status bar summary for the network page (counts, discovery state).
std::wstring NetworkPageStatus(AppState& s, const std::vector<fs::DirEntry>& entries);

// Looks for LAN devices unless a recent answer for this network exists
// (10 min); `force` (F5, 重新发现) always looks. Nothing runs on Public networks.
void RequestLanDiscovery(AppState& s, bool force);
void ApplyLanDiscovery(AppState& s);
void CancelLanDiscovery(AppState& s);
// Rebuilds every window's network page from the current data.
void RefreshNetworkPages(AppState& s);
} // namespace pulse
