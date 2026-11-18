#pragma once

#include "../ipc/protocol.h"

#include <windows.h>
#include <atomic>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace pulse::shell {

struct CtxHandlerDesc {
    CLSID clsid{};
    std::wstring clsid_text;
    std::wstring name;
    bool explorer_command = false; // packaged IExplorerCommand verb (#48)
    // SendTo slot only. Registry verbs with an AppliesTo condition: the
    // default Shell menu evaluates those itself (启用 BitLocker on a drive), so
    // they are taken from it like 创建快捷方式.
    std::vector<std::wstring> default_menu_verbs;
    // Drive roots: Explorer's drive menu has no 发送到, but the default menu
    // still supplies 创建快捷方式 / 格式化 / BitLocker.
    bool default_menu_only = false;
};

struct CtxItemOut {
    uint32_t id = 0;
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;
    bool child = false;
    std::wstring verb;
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
    wchar_t mnemonic = 0;   // access key from the raw label ("&X"), 0 = none
    // The row's MIIM_BITMAP as premultiplied BGRA (what Explorer paints next
    // to the label); null when the handler set none.
    common::MenuIconRef icon;
    uint8_t depth = 0;          // preorder tree depth; legacy child rows default to 1
};

struct CtxHandlerSlot {
    IContextMenu* menu = nullptr;
    IContextMenu2* menu2 = nullptr;
    IContextMenu3* menu3 = nullptr;
    HMENU hmenu = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    std::wstring clsid;
    std::wstring name;
    // Localized title obtained from the SendTo CLSID, used to isolate its
    // submenu in the default Shell context menu (which has no canonical verb).
    std::wstring send_to_title;
    std::vector<std::wstring> default_menu_verbs; // see CtxHandlerDesc
    bool default_menu_only = false;
    // Destinations in the SendTo folder. Explorer's 发送到 lists them from
    // posted messages, so collection pumps until the flyout holds them all.
    int send_to_expected = 0;
    // A phone item's menu (the shell's own). Pulse has no 固定到快速访问 row
    // for it (its pins need a real path), so the native row stays.
    bool shell_namespace = false;
};

struct CtxBind {
    IDataObject* data = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    HKEY assoc = nullptr;
    ~CtxBind();
    CtxBind() = default;
    CtxBind(const CtxBind&) = delete;
    CtxBind& operator=(const CtxBind&) = delete;
};

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept;

// shellex keys (relative to HKCR) for a plain file, in Explorer's order.
// Folder / Directory / Drive extensions never apply to a file.
std::vector<std::wstring> CtxHandlerKeysForFile(const std::wstring& progid,
                                               const std::wstring& ext,
                                               const std::wstring& perceived);

// Handler keys of a file system folder or a drive root, in Explorer's order.
// A drive's menu reads Drive and Folder only; a folder's also Directory and
// AllFilesystemObjects. Neither reads * (files only).
std::vector<std::wstring> CtxHandlerKeysForLocation(bool drive_root);

// Verbs under HKCR\<class>\shell carrying an AppliesTo condition.
std::vector<std::wstring> ConditionalShellVerbs(const std::vector<std::wstring>& classes);

// IShellExtInit + IContextMenu for an extension that is already created.
// Like the default Shell menu, an extension whose Initialize fails is dropped.
HRESULT InitContextMenuExtension(IUnknown* extension, const CtxBind& bind, IContextMenu** menu);

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids);

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out);

// `phase`, when given, is set to 1 once the extension is created and
// initialized, right before its own QueryContextMenu runs.
HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot, std::atomic<int>* phase = nullptr);

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out);

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text);

// Native 发送到 (右键菜单优化方案 C5): the SendTo folder plus removable and
// optical drives, listed without the default Shell menu the SendTo slot has
// to build. That slot still runs: it supplies the default menu's own rows
// (创建快捷方式, ...) and checks the native list (MergeNativeSendTo).
struct SendToTarget {
    std::wstring text;
    PIDLIST_ABSOLUTE pidl = nullptr;
};
// shell32's own 发送到(&N) (string 30312, in the system language), cleaned like
// a menu label; empty when the string is missing (no native list then).
std::wstring NativeSendToTitle(wchar_t* mnemonic = nullptr);
// The folder's entries in its own order, then removable drives with media and
// optical drives. Free with FreeSendToTargets.
std::vector<SendToTarget> EnumerateSendToTargets();
void FreeSendToTargets(std::vector<SendToTarget>& targets) noexcept;
// The 发送到 header (id 0) and one child per target, ids id_first + 1 + index.
// Nothing without a title or a target.
void NativeSendToItems(const std::vector<SendToTarget>& targets, const std::wstring& title,
                       wchar_t mnemonic, UINT id_first, UINT id_last,
                       std::vector<CtxItemOut>& out);
// What SendTo does with a destination: drop the selection on it.
HRESULT DropOnSendToTarget(PCIDLIST_ABSOLUTE target, IDataObject* data);
// SendTo copies; Shift moves and Ctrl+Shift links (Ctrl alone still copies).
// The drag keys that ask a drop target for that, and the effect itself.
inline DWORD SendToDropKeys(bool shift, bool ctrl) {
    if (shift && ctrl) return MK_LBUTTON | MK_SHIFT | MK_CONTROL;
    if (shift) return MK_LBUTTON | MK_SHIFT;
    return MK_LBUTTON | MK_CONTROL;
}
inline DWORD SendToDropEffect(DWORD keys) {
    if ((keys & MK_SHIFT) && (keys & MK_CONTROL)) return DROPEFFECT_LINK;
    if (keys & MK_SHIFT) return DROPEFFECT_MOVE;
    return DROPEFFECT_COPY;
}
// The SendTo slot's rows next to a native list: its 发送到 row and the
// destinations the native list has are dropped; destinations only the slot
// found come first, so they extend the native flyout right before it; the
// default menu's own rows stay. The differences go to `slot_only` /
// `native_only` (the latter only when the slot's flyout was filled). Without
// native destinations the rows come back unchanged.
std::vector<CtxItemOut> MergeNativeSendTo(const std::vector<CtxItemOut>& native,
                                          const std::vector<CtxItemOut>& slot_rows,
                                          std::vector<std::wstring>* slot_only = nullptr,
                                          std::vector<std::wstring>* native_only = nullptr);

// Pins the given paths to the Windows taskbar via twinui's TaskbarPinManager
// and stores the first failure (or S_OK) in `result`. Returns false, leaving
// `result` untouched, when that manager is unavailable on this Windows build;
// the caller then falls back to the verb's own InvokeCommand.
bool InvokeTaskbandPin(const std::vector<std::wstring>& paths, HRESULT& result);

} // namespace pulse::shell
