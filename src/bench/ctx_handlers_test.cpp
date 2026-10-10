#include "../shell_host/ctx_handlers.h"
#include "../ipc/ctx_menu_util.h"

#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

bool passed = true;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    passed = passed && ok;
}

class DynamicMenu final : public IContextMenu3 {
public:
    HMENU submenu = nullptr;
    bool initialized = false;
    bool invoked = false;
    bool populate = true;
    bool plain_send_to = false;   // placeholder row without a flyout
    bool slow_before = false;     // a slow flyout above Send to
    HMENU slow = nullptr;
    bool slow_initialized = false;
    bool mnemonics = false;       // labels carry "&X" access keys

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IContextMenu &&
            iid != IID_IContextMenu2 && iid != IID_IContextMenu3) return E_NOINTERFACE;
        *out = static_cast<IContextMenu3*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT, UINT first, UINT, UINT) override {
        first_ = first;
        submenu = plain_send_to ? nullptr : CreatePopupMenu();
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_STRING | (submenu ? MIIM_SUBMENU : 0u);
        item.wID = first;
        item.dwTypeData = const_cast<wchar_t*>(mnemonics ? L"Se&nd to" : L"Send to");
        item.hSubMenu = submenu;
        if (!InsertMenuItemW(menu, 1, TRUE, &item)) return E_FAIL;
        sendto_pos_ = 1;
        if (mnemonics) {
            AppendMenuW(menu, MF_STRING, first + 4, L"\x7F16\x8F91(&E)\tCtrl+E");
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 5);
        }
        if (slow_before) {
            slow = CreatePopupMenu();
            MENUITEMINFOW s{sizeof(s)};
            s.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU;
            s.wID = first + 2;
            s.dwTypeData = const_cast<wchar_t*>(L"Slow handler");
            s.hSubMenu = slow;
            if (!InsertMenuItemW(menu, 0, TRUE, &s)) return E_FAIL;
            sendto_pos_ = 2;
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 4);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 2);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        invoked = info && IS_INTRESOURCE(info->lpVerb) && LOWORD(info->lpVerb) == 1;
        return invoked ? S_OK : E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR offset, UINT flags, UINT*, CHAR* out,
                                               UINT count) override {
        if (flags != GCS_VERBW || !out) return E_NOTIMPL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(out), count,
                        offset == 0 ? L"sendto" : offset == 2 ? L"slowverb" : L"sendtarget") == 0
            ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT message, WPARAM wparam, LPARAM lparam) override {
        if (message == WM_INITMENUPOPUP && slow && reinterpret_cast<HMENU>(wparam) == slow) {
            Sleep(150); // longer than the shared nested-flyout budget
            if (!slow_initialized) AppendMenuW(slow, MF_STRING, first_ + 3, L"Slow item");
            slow_initialized = true;
            return S_OK;
        }
        if (message != WM_INITMENUPOPUP || reinterpret_cast<HMENU>(wparam) != submenu ||
            LOWORD(lparam) != sendto_pos_ || HIWORD(lparam) != FALSE) return E_INVALIDARG;
        if (!initialized && populate)
            AppendMenuW(submenu, MF_STRING, first_ + 1, mnemonics ? L"Test &destination" : L"Test destination");
        initialized = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT message, WPARAM wparam, LPARAM lparam,
                                           LRESULT* result) override {
        if (result) *result = 0;
        return HandleMenuMsg(message, wparam, lparam);
    }

private:
    ULONG refs_ = 1;
    UINT first_ = 0;
    UINT sendto_pos_ = 1;
};

void TestDynamicMenu(bool version3, bool send_to_only = false, bool empty = false) {
    DynamicMenu menu;
    menu.populate = !empty;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = version3 ? &menu : nullptr;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    if (send_to_only) slot.send_to_title = L"Send to";
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    if (send_to_only) AppendMenuW(slot.hmenu, MF_STRING, 102, L"Unrelated command");
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    Check(menu.initialized, version3 ? "IContextMenu3 initializes a normal submenu" :
                                     "IContextMenu2 initializes a normal submenu");
    const bool children = items.size() == 2 && items[0].has_children && items[0].id == 0 &&
                          items[1].child && items[1].id == 101;
    if (empty) {
        Check(items.size() == 1 && !items.front().enabled && !items.front().has_children,
              "empty Send to is disabled instead of an inert clickable parent");
    } else {
        Check(children, send_to_only ? "Send to excludes unrelated default Shell commands" :
                                     "dynamic Send to is a flyout with its destination command");
    }
    if (children) {
        CMINVOKECOMMANDINFO info{sizeof(info)};
        info.lpVerb = MAKEINTRESOURCEA(items[1].id - slot.id_first);
        Check(SUCCEEDED(menu.InvokeCommand(&info)) && menu.invoked,
              "destination keeps the handler command offset");
    }
    DestroyMenu(slot.hmenu);
}

// Access keys survive the HMENU walk: header, flyout child and flat row keep
// the raw label's "&X" while their visible text drops the marker.
void TestMnemonics() {
    DynamicMenu menu;
    menu.mnemonics = true;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    const pulse::shell::CtxItemOut* header = nullptr;
    const pulse::shell::CtxItemOut* child = nullptr;
    const pulse::shell::CtxItemOut* flat = nullptr;
    for (const auto& item : items) {
        if (item.has_children) header = &item;
        else if (item.child) child = &item;
        else flat = &item;
    }
    Check(header && header->text == L"Send to" && header->mnemonic == L'N',
          "flyout header keeps its access key, text drops the marker");
    Check(child && child->text == L"Test destination" && child->mnemonic == L'D',
          "flyout child keeps its access key");
    Check(flat && flat->text == L"\x7F16\x8F91(E)" && flat->mnemonic == L'E',
          "flat CJK row keeps (E) text and access key, shortcut suffix dropped");
    DestroyMenu(slot.hmenu);
}

void TestSendToAfterSlowFlyout() {
    DynamicMenu menu;
    menu.slow_before = true;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    bool flyout = false;
    for (size_t i = 0; i + 1 < items.size(); ++i)
        if (items[i].verb == L"sendto" && items[i].has_children && items[i + 1].child &&
            items[i + 1].id == 101) flyout = true;
    Check(menu.slow_initialized && menu.initialized && flyout,
          "Send to still fills after a slow flyout used up the shared budget (#77)");
    DestroyMenu(slot.hmenu);
}

void TestPlainSendToPlaceholder() {
    DynamicMenu menu;
    menu.plain_send_to = true;
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    slot.send_to_title = L"Send to";
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    Check(items.size() == 1 && items[0].text == L"Send to" && !items[0].enabled &&
              !items[0].has_children,
          "a Send to row without destinations is disabled, never an inert command (#77)");
    DestroyMenu(slot.hmenu);
}

void TestInstalledSendTo(const wchar_t* fixture = nullptr) {
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring path = fixture ? std::wstring(fixture) : std::wstring(windows) + L"\\win.ini";
    pulse::shell::CtxHandlerDesc handler;
    handler.clsid_text = L"{7BA4C740-9E81-11CF-99D3-00AA004AE837}";
    handler.name = L"SendTo";
    CLSIDFromString(handler.clsid_text.c_str(), &handler.clsid);
    pulse::shell::CtxBind bind;
    Check(pulse::shell::BindCtxSelection({path}, false, bind), "bind read-only Send to fixture");
    pulse::shell::CtxHandlerSlot slot;
    const HRESULT hr = pulse::shell::QueryOneHandler(handler, bind, nullptr, 100, 355,
                                                    CMF_NORMAL, slot);
    Check(SUCCEEDED(hr), "query installed Send to handler");
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    size_t children = 0;
    bool isolated = true;
    for (const auto& item : items) {
        if (item.child && item.enabled) ++children;
        if ((!item.child && item.verb != L"sendto" && item.verb != L"link") ||
            (item.id && (item.id < slot.id_first || item.id > slot.id_last))) isolated = false;
    }
    std::printf("[INFO] installed Send to: %zu rows, %zu enabled destinations\n", items.size(), children);
    Check(!items.empty() && items.front().has_children && children > 0,
          "installed Send to exposes destinations instead of an inert parent");
    Check(isolated, "Send to exposes only its own group and allocated command IDs");
    pulse::shell::ReleaseHandlerSlot(slot);
}

// Fake handler whose rows (offset -> verb) are scripted; one optional flyout
// is filled on WM_INITMENUPOPUP like Explorer's 打开方式 / 发送到.
class ScriptedMenu final : public IContextMenu3 {
public:
    struct Row {
        const wchar_t* text;
        const wchar_t* verb;
        bool flyout = false;
        bool disabled = false;
    };
    std::vector<Row> rows;
    std::vector<Row> flyout_rows;
    // Filled later from a message posted to a hidden window, like Explorer's
    // 发送到 / 包含到库中; `async_replaces` drops the placeholder rows first.
    std::vector<Row> async_rows;
    bool async_replaces = false;
    HMENU flyout = nullptr;
    UINT flyout_pos = 0;

    ScriptedMenu() = default;
    ScriptedMenu(const ScriptedMenu&) = delete;
    ScriptedMenu& operator=(const ScriptedMenu&) = delete;
    ~ScriptedMenu() {
        if (window_) DestroyWindow(window_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IContextMenu &&
            iid != IID_IContextMenu2 && iid != IID_IContextMenu3) return E_NOINTERFACE;
        *out = static_cast<IContextMenu3*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU menu, UINT, UINT first, UINT, UINT) override {
        first_ = first;
        for (UINT i = 0; i < rows.size(); ++i) {
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_ID | MIIM_STRING;
            item.wID = first + i;
            item.dwTypeData = const_cast<wchar_t*>(rows[i].text);
            if (rows[i].flyout) {
                flyout = CreatePopupMenu();
                flyout_pos = i;
                item.fMask |= MIIM_SUBMENU;
                item.hSubMenu = flyout;
            }
            if (!InsertMenuItemW(menu, i, TRUE, &item)) return E_FAIL;
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 64);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR offset, UINT flags, UINT*, CHAR* out,
                                               UINT count) override {
        if (flags != GCS_VERBW || !out) return E_NOTIMPL;
        const wchar_t* verb = nullptr;
        if (offset < rows.size()) verb = rows[offset].verb;
        else if (offset >= 32 && offset - 32 < flyout_rows.size()) verb = flyout_rows[offset - 32].verb;
        if (!verb || !*verb) return E_FAIL;
        return wcscpy_s(reinterpret_cast<wchar_t*>(out), count, verb) == 0 ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg(UINT message, WPARAM wparam, LPARAM) override {
        if (message != WM_INITMENUPOPUP || reinterpret_cast<HMENU>(wparam) != flyout) return S_OK;
        if (GetMenuItemCount(flyout) == 0) {
            for (UINT i = 0; i < flyout_rows.size(); ++i)
                AppendMenuW(flyout, MF_STRING | (flyout_rows[i].disabled ? MF_GRAYED : 0),
                            first_ + 32 + i, flyout_rows[i].text);
            if (!async_rows.empty()) PostFill();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleMenuMsg2(UINT message, WPARAM wparam, LPARAM lparam,
                                           LRESULT* result) override {
        if (result) *result = 0;
        return HandleMenuMsg(message, wparam, lparam);
    }

private:
    static LRESULT CALLBACK FillProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ScriptedMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_APP + 1 && self) {
            if (self->async_replaces)
                while (GetMenuItemCount(self->flyout) > 0) DeleteMenu(self->flyout, 0, MF_BYPOSITION);
            for (UINT i = 0; i < self->async_rows.size(); ++i)
                AppendMenuW(self->flyout, MF_STRING, self->first_ + 48 + i, self->async_rows[i].text);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void PostFill() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FillProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseCtxTestFill";
        RegisterClassW(&wc);
        if (!window_) {
            window_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                      nullptr, wc.hInstance, nullptr);
            SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }
        PostMessageW(window_, WM_APP + 1, 0, 0);
    }

    ULONG refs_ = 1;
    UINT first_ = 0;
    HWND window_ = nullptr;
};

std::vector<pulse::shell::CtxItemOut> CollectScripted(
    ScriptedMenu& menu, const wchar_t* send_to_title = nullptr, int send_to_expected = 0,
    std::vector<std::wstring> default_menu_verbs = {}, bool default_menu_only = false) {
    pulse::shell::CtxHandlerSlot slot;
    slot.menu = &menu;
    slot.menu2 = &menu;
    slot.menu3 = &menu;
    slot.hmenu = CreatePopupMenu();
    slot.id_first = 100;
    slot.id_last = 355;
    if (send_to_title) slot.send_to_title = send_to_title;
    slot.send_to_expected = send_to_expected;
    slot.default_menu_verbs = std::move(default_menu_verbs);
    slot.default_menu_only = default_menu_only;
    menu.QueryContextMenu(slot.hmenu, 0, slot.id_first, slot.id_last, CMF_NORMAL);
    std::vector<pulse::shell::CtxItemOut> items;
    pulse::shell::CollectHandlerItems(slot, false, items);
    DestroyMenu(slot.hmenu);
    return items;
}

// Explorer's 打开方式 flyout is kept whole — apps plus 选择其他应用, whose verb
// is the builtin "openas" — instead of being dropped for a registry guess.
void TestOpenWithFlyoutKept() {
    ScriptedMenu menu;
    menu.rows = {{L"Open wit&h", L"openas", true}};
    menu.flyout_rows = {{L"CAD Viewer", L""}, {L"&Choose another app", L"openas"}};
    const auto items = CollectScripted(menu);
    Check(items.size() == 3 && items[0].has_children && items[0].verb == L"openas" &&
              items[1].child && items[1].text == L"CAD Viewer" && items[1].id == 132 &&
              items[2].child && items[2].verb == L"openas" && items[2].id == 133,
          "Open with flyout keeps its apps and the Choose another app picker");
    ScriptedMenu empty;
    empty.rows = {{L"Open with", L"openas", true}};
    Check(CollectScripted(empty).empty(),
          "an Open with flyout without apps adds nothing over the static row");
}

// The SendTo slot (whole default Shell menu) also feeds 创建快捷方式 — only
// that menu produces it — enabled and invoked through the same slot.
void TestDefaultMenuExtras() {
    ScriptedMenu menu;
    menu.rows = {{L"Unrelated", L"unrelated"}, {L"Create &shortcut", L"link"},
                 {L"Send to", L"sendto", true}, {L"Other", L""}};
    menu.flyout_rows = {{L"Desktop", L""}};
    const auto items = CollectScripted(menu, L"Send to");
    bool link = false, unrelated = false, send_to = false;
    for (const auto& item : items) {
        if (item.verb == L"link") link = item.enabled && item.id == 101 && !item.child;
        if (item.text == L"Unrelated" || item.text == L"Other") unrelated = true;
        if (item.has_children && item.verb == L"sendto") send_to = true;
    }
    Check(link && send_to && !unrelated,
          "default menu feeds Create shortcut next to Send to, nothing else");
}

// A plain file loads only Explorer's file keys: its ProgID, SystemFile-
// Associations entries, * and AllFilesystemObjects — never Folder / Drive.
void TestFileHandlerKeys() {
    const auto keys = pulse::shell::CtxHandlerKeysForFile(L"CADFile", L".dwg", L"");
    const bool exact = keys.size() == 4 &&
        keys[0] == L"CADFile\\shellex\\ContextMenuHandlers" &&
        keys[1] == L"SystemFileAssociations\\.dwg\\shellex\\ContextMenuHandlers" &&
        keys[2] == L"*\\shellex\\ContextMenuHandlers" &&
        keys[3] == L"AllFilesystemObjects\\shellex\\ContextMenuHandlers";
    Check(exact, "file handler keys follow Explorer's association order");
    const auto image = pulse::shell::CtxHandlerKeysForFile(L"", L".png", L"image");
    bool folderish = false;
    for (const auto& key : image)
        if (key.find(L"Folder") == 0 || key.find(L"Directory") == 0 || key.find(L"Drive") == 0)
            folderish = true;
    Check(image.size() == 4 && image[1] == L"SystemFileAssociations\\image\\shellex\\ContextMenuHandlers" &&
              !folderish,
          "PerceivedType keys are included and folder/drive keys never are");
}
int CountChildren(const std::vector<pulse::shell::CtxItemOut>& items) {
    int n = 0;
    for (const auto& item : items)
        if (item.child) ++n;
    return n;
}

// Explorer's 发送到 lists 文档 / 压缩文件夹 / 邮件收件人 / 桌面快捷方式 from
// posted messages after WM_INITMENUPOPUP; the worker waits for the SendTo
// folder's count, keeps the host's thread messages queued, and a flyout that
// is already complete is not waited on.
void TestSendToFillsAsync() {
    ScriptedMenu menu;
    menu.rows = {{L"Send to", L"sendto", true}};
    menu.flyout_rows = {{L"Bluetooth", L""}};
    menu.async_rows = {{L"Desktop", L""}, {L"Documents", L""}, {L"Mail recipient", L""}};
    PostThreadMessageW(GetCurrentThreadId(), WM_APP + 10, 7, 0);
    const ULONGLONG start = GetTickCount64();
    const auto items = CollectScripted(menu, L"Send to", 4);
    const ULONGLONG took = GetTickCount64() - start;
    MSG held{};
    const bool kept = PeekMessageW(&held, nullptr, WM_APP + 10, WM_APP + 10, PM_REMOVE) &&
                      !held.hwnd && held.wParam == 7;
    Check(CountChildren(items) == 4 && took < 1000,
          "Send to waits for its asynchronously listed destinations");
    Check(kept, "pumping a flyout keeps the host's worker thread messages");

    ScriptedMenu quick;
    quick.rows = {{L"Send to", L"sendto", true}};
    quick.flyout_rows = {{L"Bluetooth", L""}};
    quick.async_rows = {{L"Late", L""}};
    Check(CountChildren(CollectScripted(quick, L"Send to", 0)) == 1,
          "a flyout with real rows and no target is not waited on");
    MSG drain{};
    while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&drain);
}

// 包含到库中 shows only a greyed 正在检索库... until its list arrives.
void TestPlaceholderFlyoutFills() {
    ScriptedMenu menu;
    menu.rows = {{L"Include in library", L"", true}};
    menu.flyout_rows = {{L"Retrieving libraries...", L"", false, true}};
    menu.async_rows = {{L"Documents", L""}, {L"Music", L""}};
    menu.async_replaces = true;
    const auto items = CollectScripted(menu);
    bool placeholder = false;
    for (const auto& item : items)
        if (item.text == L"Retrieving libraries...") placeholder = true;
    Check(CountChildren(items) == 2 && !placeholder,
          "a placeholder-only flyout is given time to fill");
}

// Extension whose Initialize fails (WorkFolders, Portable Devices, Library
// Location on a file) — the default Shell menu drops it, so does Pulse.
class InitFailingExtension final : public IShellExtInit, public IContextMenu {
public:
    HRESULT init_result = E_FAIL;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IShellExtInit)
            *out = static_cast<IShellExtInit*>(this);
        else if (iid == IID_IContextMenu)
            *out = static_cast<IContextMenu*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) override {
        return init_result;
    }
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override {
        return E_NOTIMPL;
    }
    ULONG refs() const { return refs_; }

private:
    ULONG refs_ = 1;
};

void TestInitializeFailureDropsExtension() {
    pulse::shell::CtxBind bind;
    InitFailingExtension failing;
    IContextMenu* menu = reinterpret_cast<IContextMenu*>(1);
    const HRESULT hr = pulse::shell::InitContextMenuExtension(
        static_cast<IShellExtInit*>(&failing), bind, &menu);
    Check(hr == E_FAIL && !menu && failing.refs() == 1,
          "an extension whose Initialize fails is dropped");
    InitFailingExtension ok;
    ok.init_result = S_OK;
    const HRESULT ok_hr = pulse::shell::InitContextMenuExtension(
        static_cast<IShellExtInit*>(&ok), bind, &menu);
    Check(SUCCEEDED(ok_hr) && menu, "an initialized extension yields its menu");
    if (menu) menu->Release();
}

// A drive reads Drive + Folder; a folder Directory + Folder + AllFilesystem-
// Objects; neither reads * (files only).
void TestLocationHandlerKeys() {
    const auto drive = pulse::shell::CtxHandlerKeysForLocation(true);
    const auto folder = pulse::shell::CtxHandlerKeysForLocation(false);
    Check(drive.size() == 2 && drive[0] == L"Drive\\shellex\\ContextMenuHandlers" &&
              drive[1] == L"Folder\\shellex\\ContextMenuHandlers",
          "drive handler keys are Drive and Folder only");
    Check(folder.size() == 3 && folder[0] == L"Directory\\shellex\\ContextMenuHandlers" &&
              folder[1] == L"Folder\\shellex\\ContextMenuHandlers" &&
              folder[2] == L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
          "folder handler keys skip * and Drive");
}

void TestOpenWithForPrograms() {
    Check(!pulse::ipc::OffersOpenWith(L".exe") && !pulse::ipc::OffersOpenWith(L".BAT") &&
              !pulse::ipc::OffersOpenWith(L".cmd") && !pulse::ipc::OffersOpenWith(L".com"),
          "programs and batch files get no Open with");
    Check(pulse::ipc::OffersOpenWith(L".msi") && pulse::ipc::OffersOpenWith(L".ps1") &&
              pulse::ipc::OffersOpenWith(L".dwg"),
          "installers, scripts and documents keep Open with");
}

// 格式化 / 旋转 / AppliesTo verbs (启用 BitLocker) come from the default menu;
// a drive's slot feeds only those, without 发送到.
void TestDefaultMenuConditionalVerbs() {
    ScriptedMenu menu;
    menu.rows = {{L"Format...", L"format"}, {L"Turn on BitLocker", L"encrypt-bde-elev"},
                 {L"Rotate right", L"rotate90"}, {L"Send to", L"sendto", true},
                 {L"Unrelated", L"unrelated"}};
    menu.flyout_rows = {{L"Desktop", L""}};
    const auto items = CollectScripted(menu, L"Send to", 0, {L"Encrypt-BDE-Elev"});
    bool format = false, bde = false, rotate = false, send_to = false, unrelated = false;
    for (const auto& item : items) {
        format |= item.verb == L"format";
        bde |= item.verb == L"encrypt-bde-elev" && item.enabled;
        rotate |= item.verb == L"rotate90";
        send_to |= item.has_children;
        unrelated |= item.text == L"Unrelated";
    }
    Check(format && bde && rotate && send_to && !unrelated,
          "default menu feeds Format, Rotate and AppliesTo verbs");
    ScriptedMenu drive;
    drive.rows = {{L"Format...", L"format"}, {L"Send to", L"sendto", true}};
    drive.flyout_rows = {{L"Desktop", L""}};
    const auto drive_items = CollectScripted(drive, L"Send to", 0, {}, true);
    Check(drive_items.size() == 1 && drive_items[0].verb == L"format",
          "a drive's default-menu slot adds no Send to");
}

void TestConditionalShellVerbsRegistry() {
    const wchar_t* cls = L"PulseCtxTest.Conditional";
    const std::wstring base = std::wstring(L"Software\\Classes\\") + cls + L"\\shell";
    auto make = [&](const wchar_t* verb, bool applies) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\" + verb).c_str(), 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        if (applies) {
            const wchar_t value[] = L"System.Volume.BitLockerProtection:=3";
            RegSetValueExW(key, L"AppliesTo", 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                           sizeof(value));
        }
        RegCloseKey(key);
    };
    make(L"conditional", true);
    make(L"plain", false);
    const auto verbs = pulse::shell::ConditionalShellVerbs({cls});
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + cls).c_str());
    Check(verbs.size() == 1 && verbs[0] == L"conditional",
          "only AppliesTo verbs are left to the default menu");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    TestDynamicMenu(false);
    TestDynamicMenu(true);
    TestDynamicMenu(true, true);
    TestDynamicMenu(true, true, true);
    TestDynamicMenu(true, false, true); // default menu: empty "sendto" flyout
    TestSendToAfterSlowFlyout();
    TestPlainSendToPlaceholder();
    TestMnemonics();
    TestOpenWithFlyoutKept();
    TestDefaultMenuExtras();
    TestFileHandlerKeys();
    TestSendToFillsAsync();
    TestPlaceholderFlyoutFills();
    TestInitializeFailureDropsExtension();
    TestLocationHandlerKeys();
    TestOpenWithForPrograms();
    TestDefaultMenuConditionalVerbs();
    TestConditionalShellVerbsRegistry();
    if (argc > 1 && wcscmp(argv[1], L"--sendto") == 0) TestInstalledSendTo(argc > 2 ? argv[2] : nullptr);
    CoUninitialize();
    return passed ? 0 : 1;
}
