#include "ctx_handlers.h"
#include "packaged_ctx_handlers.h"

#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include "../ipc/ctx_menu_util.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

using pulse::ipc::CleanMenuText;
using pulse::ipc::MenuMnemonic;
using pulse::ipc::IsBuiltinContextVerb;
using pulse::ipc::IsDisabledHandler;
using pulse::ipc::IsDefaultMenuExtraVerb;
using pulse::ipc::IsOpenWithSubmenuVerb;
using pulse::ipc::KeepFlyoutParentWithoutLeaves;
using pulse::ipc::kMaxSubmenuChildren;
using pulse::ipc::ToLowerVerb;

namespace pulse::shell {
namespace {

std::wstring ToParsingPath(std::wstring path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

// shell32!CTaskbandPin ({90AA3A4E-1CBA-4233-B8BB-535773D48449}) suppresses the
// "taskbarpin" verb during QueryContextMenu unless shell32!IsProcessAnExplorer
// returns 1. IsProcessAnExplorer begins with `mov eax, dword ptr [rip+disp32]`
// (8B 05 <disp32>) reading its cached process-type flag (1 = Explorer).
// The flag is process-wide, so this only ever runs inside pulse_shell, and
// lazily from QueryOneHandler: binaries that merely link this file (tests,
// the parity probe) keep an unpatched shell32.
#if defined(_M_X64) && !defined(_M_ARM64EC)
// The decoded RIP-relative target must be an aligned DWORD in a writable,
// non-executable section of the module that owns the function; anything else
// means the prologue is not the one described above and nothing is written.
bool IsWritableDataOf(HMODULE module, const void* p) noexcept {
    const auto addr = reinterpret_cast<uintptr_t>(p);
    if (addr % alignof(LONG) != 0) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(base) + sec->VirtualAddress;
        const uintptr_t last = first + (std::max)(sec->Misc.VirtualSize, sec->SizeOfRawData);
        if (addr < first || addr + sizeof(LONG) > last) continue;
        return (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0 &&
               (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0;
    }
    return false;
}
#endif

void EnableTaskbandPinInShell32() noexcept {
#if defined(_M_X64) && !defined(_M_ARM64EC)
    static std::once_flag once;
    std::call_once(once, []() noexcept {
        __try {
            HMODULE hShell32 = GetModuleHandleW(L"shell32.dll");
            if (!hShell32) hShell32 = LoadLibraryW(L"shell32.dll");
            if (!hShell32) return;
            const auto* fn = reinterpret_cast<const uint8_t*>(
                GetProcAddress(hShell32, "IsProcessAnExplorer"));
            if (!fn) {
                fn = reinterpret_cast<const uint8_t*>(
                    GetProcAddress(hShell32, MAKEINTRESOURCEA(680)));
            }
            if (!fn) return;
            // A forwarded export resolves into another module; validate the
            // flag against the module that actually owns the code.
            HMODULE owner = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(fn), &owner) ||
                !owner)
                return;
            for (size_t i = 0; i < 48; ++i) {
                if (i > 0 && (fn[i - 1] == 0x48 || fn[i - 1] == 0x4C)) continue;
                const bool mov_eax = (fn[i] == 0x8B && fn[i + 1] == 0x05 &&
                                      fn[i + 6] == 0x85 && fn[i + 7] == 0xC0);
                const bool mov_ecx = (fn[i] == 0x8B && fn[i + 1] == 0x0D &&
                                      fn[i + 6] == 0x85 && fn[i + 7] == 0xC9);
                if (!mov_eax && !mov_ecx) continue;
                int32_t disp = 0;
                std::memcpy(&disp, fn + i + 2, sizeof(disp));
                const void* target = fn + i + 6 + disp;
                if (IsWritableDataOf(owner, target))
                    InterlockedExchange(const_cast<LONG*>(static_cast<const LONG*>(target)), 1);
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
#endif
}

// {68C6A1B9-DE39-42C3-8D28-BF40A5126541}
constexpr GUID kIID_ICallingProcessInfo = {
    0x68c6a1b9, 0xde39, 0x42c3, {0x8d, 0x28, 0xbf, 0x40, 0xa5, 0x12, 0x65, 0x41}
};
// {A5C8D635-B4ED-452B-8109-9501781096D1}
constexpr GUID kCLSID_TaskbarPinManager = {
    0xa5c8d635, 0xb4ed, 0x452b, {0x81, 0x09, 0x95, 0x01, 0x78, 0x10, 0x96, 0xd1}
};
// {D75F625F-6DF9-4874-970D-17CBF846F00D}
constexpr GUID kIID_ITaskbarPinManager = {
    0xd75f625f, 0x6df9, 0x4874, {0x97, 0x0d, 0x17, 0xcb, 0xf8, 0x46, 0xf0, 0x0d}
};

struct ICallingProcessInfoLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OpenCallerProcessHandle(
        DWORD desiredAccess, HANDLE* phProcess) = 0;
};

struct IServerCallTargetLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsTargetObject(
        IUnknown* punkServer, BOOL* pbIsTarget) = 0;
};

struct ITaskbarPinManagerLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Method3() = 0;
    virtual HRESULT STDMETHODCALLTYPE PinPidl(PCIDLIST_ABSOLUTE pidl, int caller) = 0;
};

bool IsComCallContextIid(REFIID riid) noexcept {
    static const IID* const kIids[] = {
        &IID_IServerSecurity, &IID_ICancelMethodCalls, &IID_ICallFactory,
        &IID_IClientSecurity, &IID_IRpcOptions, &IID_IMarshal, &IID_IMarshal2,
        &IID_INoMarshal, &IID_IAgileObject, &IID_IStdMarshalInfo,
        &IID_IExternalConnection, &IID_IDispatch,
    };
    for (const IID* iid : kIids)
        if (IsEqualIID(riid, *iid)) return true;
    return false;
}

class ExplorerCallContext final : public ICallingProcessInfoLocal,
                                  public IServerCallTargetLocal {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_ICallingProcessInfo)) {
            *ppv = static_cast<ICallingProcessInfoLocal*>(this);
            return S_OK;
        }
        // While this object is the call context, CoImpersonateClient,
        // CoQueryClientBlanket, CoTestCancel, ... query it for COM's own
        // interfaces and call slots the 4-entry IServerCallTarget vtable does
        // not have. Its real IID is not public, so every other IID still gets
        // it, as TaskbarPinManager expects.
        if (IsComCallContextIid(riid)) return E_NOINTERFACE;
        *ppv = static_cast<IServerCallTargetLocal*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE OpenCallerProcessHandle(
        DWORD desiredAccess, HANDLE* phProcess) override {
        if (!phProcess) return E_POINTER;
        *phProcess = nullptr;
        DWORD pid = 0;
        if (HWND shellWnd = GetShellWindow()) {
            GetWindowThreadProcessId(shellWnd, &pid);
        }
        if (!pid) {
            if (HWND trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr)) {
                GetWindowThreadProcessId(trayWnd, &pid);
            }
        }
        if (!pid) return E_ACCESSDENIED;
        HANDLE hProc = OpenProcess(desiredAccess, FALSE, pid);
        if (!hProc) return HRESULT_FROM_WIN32(GetLastError());
        *phProcess = hProc;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsTargetObject(
        IUnknown* /*punkServer*/, BOOL* pbIsTarget) override {
        if (!pbIsTarget) return E_POINTER;
        *pbIsTarget = TRUE;
        return S_OK;
    }
};

std::wstring ClsidText(const CLSID& clsid) {
    wchar_t buf[64]{};
    if (StringFromGUID2(clsid, buf, ARRAYSIZE(buf)) <= 0) return {};
    return ToLowerVerb(buf);
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return {};
    struct LANGANDCODEPAGE { WORD language; WORD codepage; };
    LANGANDCODEPAGE* translate = nullptr;
    UINT translate_bytes = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translate), &translate_bytes) ||
        !translate || translate_bytes < sizeof(LANGANDCODEPAGE))
        return {};
    wchar_t query[64]{};
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translate[0].language, translate[0].codepage);
    wchar_t* desc = nullptr;
    UINT desc_bytes = 0;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&desc), &desc_bytes) ||
        !desc || desc[0] == 0)
        return {};
    return desc;
}

std::wstring HandlerName(HKEY handlers, const wchar_t* subkey, const CLSID& clsid) {
    if (subkey && subkey[0] && subkey[0] != L'{') return subkey;
    wchar_t server[MAX_PATH]{};
    DWORD server_bytes = sizeof(server);
    std::wstring inproc = L"CLSID\\";
    inproc += ClsidText(clsid);
    inproc += L"\\InprocServer32";
    if (RegGetValueW(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, server,
                     &server_bytes) == ERROR_SUCCESS && server[0]) {
        wchar_t expanded[MAX_PATH]{};
        if (ExpandEnvironmentStringsW(server, expanded, ARRAYSIZE(expanded))) {
            auto desc = FileDescription(expanded);
            if (!desc.empty()) return desc;
        }
    }
    wchar_t named[256]{};
    DWORD named_bytes = sizeof(named);
    if (handlers && subkey &&
        RegGetValueW(handlers, subkey, nullptr, RRF_RT_REG_SZ, nullptr, named,
                     &named_bytes) == ERROR_SUCCESS && named[0] && named[0] != L'{')
        return named;
    return subkey && subkey[0] ? std::wstring(subkey) : ClsidText(clsid);
}

void AddHandlersFromKey(HKEY root, const wchar_t* relative,
                        std::vector<CtxHandlerDesc>& out,
                        std::unordered_set<std::wstring>& seen,
                        const std::vector<std::wstring>& disabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, relative, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD name_cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_cch, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        wchar_t value[128]{};
        DWORD value_bytes = sizeof(value);
        const wchar_t* guid_text = name;
        if (RegGetValueW(key, name, nullptr, RRF_RT_REG_SZ, nullptr, value,
                         &value_bytes) == ERROR_SUCCESS && value[0] == L'{')
            guid_text = value;
        CLSID clsid{};
        if (FAILED(CLSIDFromString(guid_text, &clsid))) continue;
        const std::wstring text = ClsidText(clsid);
        if (text.empty() || !seen.insert(text).second) continue;
        if (IsDisabledHandler(text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = clsid;
        desc.clsid_text = text;
        desc.name = HandlerName(key, name, clsid);
        out.push_back(std::move(desc));
    }
    RegCloseKey(key);
}

// Packaged (MSIX / sparse) verbs whose manifest item types match the target.
void AddPackagedHandlers(bool background, const std::wstring& path,
                         std::vector<CtxHandlerDesc>& out,
                         std::unordered_set<std::wstring>& seen,
                         const std::vector<std::wstring>& disabled) {
    const std::vector<std::wstring> types = PackagedItemTypesFor(background, path);
    for (const auto& verb : PackagedContextMenuVerbs()) {
        if (!PackagedVerbMatches(verb, types)) continue;
        if (!seen.insert(verb.clsid_text).second) continue;
        if (IsDisabledHandler(verb.clsid_text, disabled)) continue;
        CtxHandlerDesc desc;
        desc.clsid = verb.clsid;
        desc.clsid_text = verb.clsid_text;
        desc.name = verb.name;
        desc.explorer_command = true;
        out.push_back(std::move(desc));
    }
}

std::wstring ExtensionOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return ToLowerVerb(name.substr(dot));
}

// The ProgID Explorer itself resolves for the type. Reading HKCR\.ext (or
// UserChoice) by hand diverges from it: on the reporting machine .dwg had an
// empty UserChoice and HKCR\.dwg = AutoCAD.Drawing.26, yet the shell (and its
// menu) used CADFile, so Pulse loaded the wrong verbs and handlers.
std::wstring ProgIdForExt(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t progid[256]{};
    DWORD cch = ARRAYSIZE(progid);
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, ext.c_str(), nullptr,
                                    progid, &cch)) && progid[0] &&
        _wcsicmp(progid, ext.c_str()) != 0)
        return progid;
    DWORD bytes = sizeof(progid);
    progid[0] = 0;
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                     progid, &bytes) != ERROR_SUCCESS || !progid[0])
        return {};
    return progid;
}

std::wstring PerceivedType(const std::wstring& ext) {
    if (ext.empty()) return {};
    wchar_t type[128]{};
    DWORD bytes = sizeof(type);
    if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), L"PerceivedType", RRF_RT_REG_SZ,
                     nullptr, type, &bytes) != ERROR_SUCCESS || !type[0])
        return {};
    return type;
}

bool SafeGetVerbW(IContextMenu* menu, UINT offset, wchar_t* buf, UINT cch) {
    __try {
        return SUCCEEDED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                reinterpret_cast<CHAR*>(buf), cch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT SafeQueryContextMenu(IContextMenu* menu, HMENU hmenu, UINT index,
                             UINT id_first, UINT id_last, UINT flags) {
    __try {
        return menu->QueryContextMenu(hmenu, index, id_first, id_last, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

std::wstring CtxVerbOf(IContextMenu* menu, UINT id, UINT id_first) {
    if (id < id_first) return {};
    wchar_t buf[128]{};
    if (SafeGetVerbW(menu, id - id_first, buf, ARRAYSIZE(buf) - 1)) return buf;
    return {};
}

// The row's MIIM_BITMAP as premultiplied BGRA, the way Explorer paints it
// next to the label. Handlers hand out 32bpp ARGB bitmaps (premultiplied per
// the Vista menu guidance; straight alpha is premultiplied here), older ones
// opaque DDBs. HBMMENU_* stock ids and owner-draw callbacks carry no pixels.
pulse::common::MenuIconRef MenuItemIcon(HMENU menu, UINT pos) {
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_BITMAP;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii) || !mii.hbmpItem ||
        mii.hbmpItem == HBMMENU_CALLBACK ||
        reinterpret_cast<ULONG_PTR>(mii.hbmpItem) <=
            reinterpret_cast<ULONG_PTR>(HBMMENU_POPUP_MINIMIZE))
        return nullptr;
    BITMAP info{};
    if (!GetObjectW(mii.hbmpItem, sizeof(info), &info) || info.bmWidth <= 0 ||
        info.bmHeight <= 0 ||
        info.bmWidth > static_cast<LONG>(pulse::common::kMaxMenuIconEdge) ||
        info.bmHeight > static_cast<LONG>(pulse::common::kMaxMenuIconEdge))
        return nullptr;
    auto pixels = std::make_shared<pulse::common::MenuIconPixels>();
    pixels->width = static_cast<uint32_t>(info.bmWidth);
    pixels->height = static_cast<uint32_t>(info.bmHeight);
    pixels->bgra.resize(static_cast<size_t>(pixels->width) * pixels->height * 4u);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    const int lines = GetDIBits(dc, mii.hbmpItem, 0, static_cast<UINT>(info.bmHeight),
                                pixels->bgra.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines != info.bmHeight) return nullptr;
    auto& bgra = pixels->bgra;
    bool any_alpha = false, straight = false;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const uint8_t a = bgra[i + 3];
        if (a) any_alpha = true;
        if (bgra[i] > a || bgra[i + 1] > a || bgra[i + 2] > a) straight = true;
    }
    if (info.bmBitsPixel != 32 || !any_alpha) {
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
            const uint32_t a = bgra[i + 3];
            for (size_t c = 0; c < 3; ++c)
                bgra[i + c] = static_cast<uint8_t>((bgra[i + c] * a + 127u) / 255u);
        }
    }
    return pixels;
}

// Visible row text; the raw label's access key ("&X") goes to *mnemonic.
std::wstring MenuItemText(HMENU menu, UINT pos, wchar_t* mnemonic = nullptr) {
    if (mnemonic) *mnemonic = 0;
    wchar_t buf[512]{};
    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = buf;
    mii.cch = ARRAYSIZE(buf) - 1;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mii)) return {};
    if (mnemonic) *mnemonic = MenuMnemonic(buf);
    return CleanMenuText(buf);
}

constexpr ULONGLONG kNestedFlyoutBudgetMs = 80;
constexpr CLSID kSendToHandler = {0x7BA4C740, 0x9E81, 0x11CF,
                                 {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

void InitMenuPopup(IContextMenu2* menu2, IContextMenu3* menu3, HMENU submenu, UINT pos) {
    if (!submenu) return;
    __try {
        if (menu3) {
            LRESULT ignored = 0;
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                  MAKELPARAM(pos, FALSE), &ignored);
        } else if (menu2) {
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu),
                                 MAKELPARAM(pos, FALSE));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 发送到 has been seen to fill in 310-440 ms on a cold host (95-160 ms warm);
// Explorer's own rows can arrive 300+ ms apart, so a quiet gap below that
// would cut the list short.
constexpr ULONGLONG kSendToFillMs = 1500;
constexpr ULONGLONG kSendToQuietMs = 500;
// 包含到库中 replaces 正在检索库... in ~110 ms cold, ~16 ms warm.
constexpr ULONGLONG kPlaceholderFillMs = 300;

void SafeDispatch(const MSG* msg) {
    __try {
        TranslateMessage(msg);
        DispatchMessageW(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Some flyouts fill from messages posted to their own hidden windows, so the
// worker gives them its queue until `done` or `deadline`. Thread messages (the
// host's worker commands) are set aside and posted back afterwards.
template <typename Done>
void PumpUntil(Done done, ULONGLONG deadline) {
    std::vector<MSG> held;
    bool quit = false;
    WPARAM quit_code = 0;
    while (!done()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const ULONGLONG left = deadline - now;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left < 10 ? left : 10),
                                  QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quit_code = msg.wParam;
            } else if (!msg.hwnd) {
                held.push_back(msg);
            } else {
                SafeDispatch(&msg);
            }
        }
    }
    for (const MSG& msg : held)
        PostThreadMessageW(GetCurrentThreadId(), msg.message, msg.wParam, msg.lParam);
    if (quit) PostQuitMessage(static_cast<int>(quit_code));
}

// A flyout showing only greyed rows (正在检索库..., 正在搜索设备...) is still
// being filled.
bool OnlyPlaceholderRows(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    int rows = 0;
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) return false;
        if (mii.fType & MFT_SEPARATOR) continue;
        if (mii.hSubMenu || !(mii.fState & (MFS_DISABLED | MFS_GRAYED))) return false;
        ++rows;
    }
    return rows > 0;
}

// True when the flyout was given time to fill.
bool WaitForFlyoutFill(HMENU submenu, int fill_target) {
    if (fill_target > 0) {
        int last = GetMenuItemCount(submenu);
        ULONGLONG changed = GetTickCount64();
        PumpUntil([&] {
            const int count = GetMenuItemCount(submenu);
            const ULONGLONG now = GetTickCount64();
            if (count != last) {
                last = count;
                changed = now;
            }
            return count >= fill_target || now - changed >= kSendToQuietMs;
        }, GetTickCount64() + kSendToFillMs);
        return true;
    }
    if (OnlyPlaceholderRows(submenu)) {
        const int before = GetMenuItemCount(submenu);
        PumpUntil([&] {
            return GetMenuItemCount(submenu) != before || !OnlyPlaceholderRows(submenu);
        }, GetTickCount64() + kPlaceholderFillMs);
        return true;
    }
    return false;
}

// Entries Explorer lists under 发送到 (hidden files such as desktop.ini are not).
int CountSendToTargets() {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_SendTo, 0, nullptr, &pidl)) || !pidl) return 0;
    int count = 0;
    IShellFolder* folder = nullptr;
    if (SUCCEEDED(SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder))) && folder) {
        IEnumIDList* items = nullptr;
        if (folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items) == S_OK &&
            items) {
            PITEMID_CHILD child = nullptr;
            while (items->Next(1, &child, nullptr) == S_OK) {
                ++count;
                CoTaskMemFree(child);
            }
            items->Release();
        }
        folder->Release();
    }
    CoTaskMemFree(pidl);
    return count;
}

bool ContainsVerb(const std::vector<std::wstring>& verbs, const std::wstring& verb) {
    if (verb.empty()) return false;
    for (const auto& v : verbs)
        if (_wcsicmp(v.c_str(), verb.c_str()) == 0) return true;
    return false;
}

void CollectSubmenuLeaves(IContextMenu* menu, IContextMenu2* menu2, IContextMenu3* menu3,
                          HMENU submenu, UINT pos, bool background, UINT id_first,
                          bool parent_enabled, int depth, ULONGLONG deadline,
                          std::vector<CtxItemOut>& kids, bool open_with = false,
                          int fill_target = 0) {
    if (GetTickCount64() < deadline) {
        InitMenuPopup(menu2, menu3, submenu, pos);
        // Off the first-paint path: these workers stream in a later partial.
        // Rows that arrived late get the usual budget for their own flyouts.
        if (depth == 0 && WaitForFlyoutFill(submenu, fill_target))
            deadline = GetTickCount64() + kNestedFlyoutBudgetMs;
    }
    const int sub_count = GetMenuItemCount(submenu);
    for (int j = 0; j < sub_count && static_cast<int>(kids.size()) < kMaxSubmenuChildren; ++j) {
        MENUITEMINFOW sub{ sizeof(sub) };
        sub.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(submenu, j, TRUE, &sub)) continue;
        if (sub.fType & MFT_SEPARATOR) continue;
        if (sub.hSubMenu) {
            if (depth >= 1) continue;
            const bool nested_enabled =
                parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
            CollectSubmenuLeaves(menu, menu2, menu3, sub.hSubMenu, static_cast<UINT>(j),
                                 background, id_first, nested_enabled, depth + 1, deadline,
                                 kids, open_with);
            continue;
        }
        wchar_t child_mnemonic = 0;
        const std::wstring child_text =
            MenuItemText(submenu, static_cast<UINT>(j), &child_mnemonic);
        if (child_text.empty()) continue;
        CtxItemOut item;
        item.id = sub.wID;
        item.enabled = parent_enabled && !(sub.fState & (MFS_DISABLED | MFS_GRAYED));
        item.child = true;
        item.verb = CtxVerbOf(menu, sub.wID, id_first);
        // 选择其他应用 carries the builtin "openas" verb but belongs to the flyout.
        if (!open_with && IsBuiltinContextVerb(item.verb, background)) continue;
        item.text = child_text;
        item.mnemonic = child_mnemonic;
        item.icon = MenuItemIcon(submenu, static_cast<UINT>(j));
        kids.push_back(std::move(item));
    }
}

} // namespace

CtxBind::~CtxBind() {
    if (data) data->Release();
    if (folder) CoTaskMemFree(folder);
    if (assoc) RegCloseKey(assoc);
}

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept {
    if (slot.hmenu) DestroyMenu(slot.hmenu);
    if (slot.menu3) slot.menu3->Release();
    if (slot.menu2) slot.menu2->Release();
    if (slot.menu) slot.menu->Release();
    slot.hmenu = nullptr;
    slot.menu3 = nullptr;
    slot.menu2 = nullptr;
    slot.menu = nullptr;
}

std::vector<std::wstring> CtxHandlerKeysForFile(const std::wstring& progid,
                                               const std::wstring& ext,
                                               const std::wstring& perceived) {
    // Explorer's order: the type's ProgID, its SystemFileAssociations entries,
    // then every file (*) and every file-system object.
    constexpr wchar_t kHandlers[] = L"\\shellex\\ContextMenuHandlers";
    std::vector<std::wstring> keys;
    if (!progid.empty()) keys.push_back(progid + kHandlers);
    if (!ext.empty()) keys.push_back(L"SystemFileAssociations\\" + ext + kHandlers);
    if (!perceived.empty())
        keys.push_back(L"SystemFileAssociations\\" + perceived + kHandlers);
    keys.push_back(std::wstring(L"*") + kHandlers);
    keys.push_back(std::wstring(L"AllFilesystemObjects") + kHandlers);
    return keys;
}

std::vector<std::wstring> CtxHandlerKeysForLocation(bool drive_root) {
    constexpr wchar_t kHandlers[] = L"\\shellex\\ContextMenuHandlers";
    if (drive_root)
        return { std::wstring(L"Drive") + kHandlers, std::wstring(L"Folder") + kHandlers };
    return { std::wstring(L"Directory") + kHandlers, std::wstring(L"Folder") + kHandlers,
             std::wstring(L"AllFilesystemObjects") + kHandlers };
}

std::vector<std::wstring> ConditionalShellVerbs(const std::vector<std::wstring>& classes) {
    std::vector<std::wstring> verbs;
    for (const auto& cls : classes) {
        HKEY shell = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (cls + L"\\shell").c_str(), 0, KEY_READ, &shell) !=
            ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[256]{};
            DWORD cch = ARRAYSIZE(name);
            if (RegEnumKeyExW(shell, i, name, &cch, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS)
                break;
            if (RegGetValueW(shell, name, L"AppliesTo", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                             nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
                !ContainsVerb(verbs, name))
                verbs.emplace_back(name);
        }
        RegCloseKey(shell);
    }
    return verbs;
}

HRESULT InitContextMenuExtension(IUnknown* extension, const CtxBind& bind, IContextMenu** menu) {
    if (!menu) return E_POINTER;
    *menu = nullptr;
    if (!extension) return E_INVALIDARG;
    IShellExtInit* init = nullptr;
    if (SUCCEEDED(extension->QueryInterface(IID_PPV_ARGS(&init))) && init) {
        // Called anyway, WorkFolders (E_INVALIDARG), Portable Devices and
        // Library Location (E_FAIL) still add 立即同步 / 以便携式设备方式打开 /
        // 包含到库中 that Explorer never shows; Open With fails on a drive.
        const HRESULT hr = init->Initialize(bind.folder, bind.data, bind.assoc);
        init->Release();
        if (FAILED(hr)) return hr;
    }
    const HRESULT hr = extension->QueryInterface(IID_PPV_ARGS(menu));
    return SUCCEEDED(hr) && !*menu ? E_NOINTERFACE : hr;
}

namespace {

// HKCR class names behind handler keys ("Drive\\shellex\\..." -> "Drive").
std::vector<std::wstring> ClassesOf(const std::vector<std::wstring>& handler_keys) {
    std::vector<std::wstring> classes;
    for (const auto& key : handler_keys) {
        const size_t at = key.find(L"\\shellex\\");
        if (at != std::wstring::npos) classes.push_back(key.substr(0, at));
    }
    return classes;
}

void AttachDefaultMenuVerbs(std::vector<CtxHandlerDesc>& handlers,
                            const std::vector<std::wstring>& handler_keys) {
    for (auto& desc : handlers) {
        if (desc.clsid != kSendToHandler) continue;
        desc.default_menu_verbs = ConditionalShellVerbs(ClassesOf(handler_keys));
        return;
    }
}

// 创建快捷方式 / 格式化 / 旋转 come only from the default Shell menu, which the
// SendTo slot builds. Explorer shows them whether or not the SendTo extension
// is registered, but the slot existed only when the SendTo entry under
// AllFilesystemObjects\shellex\ContextMenuHandlers parsed: a "右键管理" tool that turned 发送到 off
// (key removed or its value prefixed) took 创建快捷方式 along on a user's
// Windows 10. Without a registered SendTo, a default-menu-only slot (no 发送到
// rows) still supplies them. One the user disabled in Pulse stays off: hiding
// 发送到 hides its slot (main.cpp).
void EnsureDefaultMenuSlot(std::vector<CtxHandlerDesc>& out,
                           std::unordered_set<std::wstring>& seen,
                           const std::vector<std::wstring>& disabled) {
    for (const auto& desc : out)
        if (desc.clsid == kSendToHandler) return;
    const std::wstring text = ClsidText(kSendToHandler);
    if (text.empty() || IsDisabledHandler(text, disabled) || !seen.insert(text).second) return;
    CtxHandlerDesc desc;
    desc.clsid = kSendToHandler;
    desc.clsid_text = text;
    desc.name = HandlerName(nullptr, nullptr, kSendToHandler);
    desc.default_menu_only = true;
    out.push_back(std::move(desc));
}

} // namespace

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids) {
    std::vector<CtxHandlerDesc> out;
    std::unordered_set<std::wstring> seen;
    auto add = [&](const wchar_t* relative) {
        AddHandlersFromKey(HKEY_CLASSES_ROOT, relative, out, seen, disabled_clsids);
        const std::wstring key(relative);
        if (background && key != L"Directory\\Background\\shellex\\ContextMenuHandlers") return;
        const auto suffix = key.find(L"\\shellex\\");
        if (suffix == std::wstring::npos) return;
        HKEY shell = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, (key.substr(0, suffix) + L"\\shell").c_str(),
                          0, KEY_READ, &shell) != ERROR_SUCCESS) return;
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]{};
            DWORD length = ARRAYSIZE(name);
            if (RegEnumKeyExW(shell, index, name, &length, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) break;
            wchar_t value[64]{};
            DWORD bytes = sizeof(value);
            if (RegGetValueW(shell, name, L"ExplorerCommandHandler", RRF_RT_REG_SZ,
                             nullptr, value, &bytes) != ERROR_SUCCESS) continue;
            CtxHandlerDesc desc;
            if (FAILED(CLSIDFromString(value, &desc.clsid))) continue;
            const auto present = [&](const wchar_t* field) {
                return RegGetValueW(shell, name, field, RRF_RT_ANY, nullptr, nullptr, nullptr)
                    == ERROR_SUCCESS;
            };
            if (present(L"LegacyDisable") || present(L"ProgrammaticAccessOnly") ||
                present(L"Extended") || present(L"AppliesTo")) continue;
            desc.clsid_text = ClsidText(desc.clsid);
            if (IsDisabledHandler(desc.clsid_text, disabled_clsids) ||
                !seen.insert(desc.clsid_text).second) continue;
            desc.name = name;
            desc.explorer_command = true;
            out.push_back(std::move(desc));
        }
        RegCloseKey(shell);
    };
    if (background) {
        add(L"Directory\\Background\\shellex\\ContextMenuHandlers");
        add(L"Directory\\shellex\\ContextMenuHandlers");
        add(L"Folder\\shellex\\ContextMenuHandlers");
        add(L"LibraryFolder\\Background\\shellex\\ContextMenuHandlers");
        add(L"Drive\\shellex\\ContextMenuHandlers");
        add(L"*\\shellex\\ContextMenuHandlers");
        AddPackagedHandlers(true, path, out, seen, disabled_clsids);
        return out;
    }
    const std::wstring ext = ExtensionOf(path);
    const DWORD attr = GetFileAttributesW(ToParsingPath(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // A plain file: only the keys Explorer's association array holds for
        // it. Folder / Directory / Drive extensions do not apply; loading them
        // cost a worker each (PintoStartScreen ~125 ms) and showed rows such as
        // 包含到库中 or 以便携式设备方式打开 that Explorer never offers on a file.
        const auto keys = CtxHandlerKeysForFile(ProgIdForExt(ext), ext, PerceivedType(ext));
        for (const auto& key : keys) add(key.c_str());
        EnsureDefaultMenuSlot(out, seen, disabled_clsids);
        AttachDefaultMenuVerbs(out, keys);
        AddPackagedHandlers(false, path, out, seen, disabled_clsids);
        return out;
    }
    if (attr != INVALID_FILE_ATTRIBUTES) {
        // A folder or a drive root: Explorer's keys for it, never * (files
        // only) — on a drive root those added VS / Git / ToDesk / 内网通 / 泛泰快传 rows
        // Explorer does not show.
        const std::wstring parsing = ToParsingPath(path);
        const bool drive_root = PathIsRootW(parsing.c_str()) && !PathIsUNCW(parsing.c_str());
        const auto keys = CtxHandlerKeysForLocation(drive_root);
        for (const auto& key : keys) add(key.c_str());
        if (drive_root) {
            // No 发送到 on a drive, but its default-menu slot still supplies
            // 创建快捷方式 / 格式化 / BitLocker.
            std::vector<CtxHandlerDesc> objects;
            std::unordered_set<std::wstring> objects_seen = seen;
            AddHandlersFromKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
                               objects, objects_seen, disabled_clsids);
            for (auto& desc : objects) {
                if (desc.clsid != kSendToHandler) continue;
                desc.default_menu_only = true;
                seen.insert(desc.clsid_text);
                out.push_back(std::move(desc));
            }
        }
        EnsureDefaultMenuSlot(out, seen, disabled_clsids);
        AttachDefaultMenuVerbs(out, keys);
        AddPackagedHandlers(false, path, out, seen, disabled_clsids);
        return out;
    }
    add(L"*\\shellex\\ContextMenuHandlers");
    add(L"AllFilesystemObjects\\shellex\\ContextMenuHandlers");
    add(L"Folder\\shellex\\ContextMenuHandlers");
    add(L"Directory\\shellex\\ContextMenuHandlers");
    add(L"Drive\\shellex\\ContextMenuHandlers");
    if (!ext.empty()) {
        const std::wstring assoc = L"SystemFileAssociations\\" + ext +
            L"\\shellex\\ContextMenuHandlers";
        add(assoc.c_str());
        const std::wstring progid = ProgIdForExt(ext);
        if (!progid.empty()) {
            const std::wstring key = progid + L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
        const std::wstring perceived = PerceivedType(ext);
        if (!perceived.empty()) {
            const std::wstring key = L"SystemFileAssociations\\" + perceived +
                L"\\shellex\\ContextMenuHandlers";
            add(key.c_str());
        }
    }
    AddPackagedHandlers(false, path, out, seen, disabled_clsids);
    return out;
}

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out) {
    if (paths.empty()) return false;
    if (background) {
        if (FAILED(pulse::shell_id::ParseId(ToParsingPath(paths.front()), &out.folder)) || !out.folder)
            return false;
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(out.folder, IID_PPV_ARGS(&folder))) && folder) {
            folder->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
            folder->Release();
        }
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\Background", 0, KEY_READ, &out.assoc);
        return true;
    }

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(pulse::shell_id::ParseId(ToParsingPath(p), &pidl)) && pidl)
            pidls.push_back(pidl);
    }
    if (pidls.empty()) return false;
    IShellItemArray* array = nullptr;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(
        static_cast<UINT>(pidls.size()),
        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &array);
    if (SUCCEEDED(hr) && array) {
        array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&out.data));
        IShellItem* first = nullptr;
        if (SUCCEEDED(array->GetItemAt(0, &first)) && first) {
            IShellItem* parent = nullptr;
            if (SUCCEEDED(first->GetParent(&parent)) && parent) {
                SHGetIDListFromObject(parent, &out.folder);
                parent->Release();
            }
            first->Release();
        }
        array->Release();
    }
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    const std::wstring ext = ExtensionOf(paths.front());
    const std::wstring progid = ProgIdForExt(ext);
    if (!progid.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, progid.c_str(), 0, KEY_READ, &out.assoc);
    else if (!ext.empty())
        RegOpenKeyExW(HKEY_CLASSES_ROOT, ext.c_str(), 0, KEY_READ, &out.assoc);
    else
        RegOpenKeyExW(HKEY_CLASSES_ROOT, L"*", 0, KEY_READ, &out.assoc);
    return out.data != nullptr;
}

HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot, std::atomic<int>* phase) {
    EnableTaskbandPinInShell32();
    slot = {};
    slot.id_first = id_first;
    slot.id_last = id_last;
    slot.clsid = handler.clsid_text;
    slot.name = handler.name;
    slot.default_menu_verbs = handler.default_menu_verbs;
    slot.default_menu_only = handler.default_menu_only;

    HRESULT hr = E_FAIL;
    if (handler.explorer_command) {
        // Packaged verbs are IExplorerCommand servers, usually out of process
        // (dllhost surrogate); the selection is the bound data object, which is
        // the folder itself for a background click.
        IExplorerCommand* command = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                              IID_PPV_ARGS(&command));
        if (FAILED(hr) || !command) return FAILED(hr) ? hr : E_FAIL;
        IShellItemArray* items = nullptr;
        if (bind.data)
            SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&items));
        // Explorer's view site: verbs such as PowerToys New+ read the folder
        // to work in from it, not from the items (#58-6).
        IUnknown* site = nullptr;
        if (bind.folder) CreateFolderViewSite(bind.folder, &site);
        if (phase) phase->store(1);
        hr = items ? CreateExplorerCommandMenu(command, items, &slot.menu, site) : E_FAIL;
        if (site) site->Release();
        if (items) items->Release();
        command->Release();
    } else {
        IUnknown* unk = nullptr;
        hr = CoCreateInstance(handler.clsid, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IUnknown, reinterpret_cast<void**>(&unk));
        if (FAILED(hr) || !unk) return FAILED(hr) ? hr : E_FAIL;

        hr = InitContextMenuExtension(unk, bind, &slot.menu);
        unk->Release();
    }
    if (FAILED(hr) || !slot.menu) {
        slot.menu = nullptr;
        return FAILED(hr) ? hr : E_FAIL;
    }

    slot.hmenu = CreatePopupMenu();
    if (!slot.hmenu) {
        slot.menu->Release();
        slot.menu = nullptr;
        return E_OUTOFMEMORY;
    }
    (void)owner;
    if (phase) phase->store(1);
    hr = SafeQueryContextMenu(slot.menu, slot.hmenu, 0, id_first, id_last, qcm_flags);
    if (FAILED(hr)) {
        ReleaseHandlerSlot(slot);
        return hr;
    }
    if (handler.clsid == kSendToHandler && bind.data) {
        // The standalone SendTo extension exposes only a placeholder on modern
        // Windows. The default Shell menu supplies the services that populate
        // and invoke its destinations. Keep the CLSID-provided localized title
        // to isolate that submenu without hardcoding any display language.
        // Prefer the placeholder flyout. Some Windows builds add the
        // placeholder as a plain row instead; it used to slip through as an
        // inert "Send to" that did nothing when clicked (#77).
        int plain_row = -1;
        for (int i = 0; i < GetMenuItemCount(slot.hmenu); ++i) {
            if (GetSubMenu(slot.hmenu, i)) {
                slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(i));
                break;
            }
            MENUITEMINFOW row{ sizeof(row) };
            row.fMask = MIIM_FTYPE;
            if (plain_row < 0 && GetMenuItemInfoW(slot.hmenu, i, TRUE, &row) &&
                !(row.fType & MFT_SEPARATOR) &&
                !MenuItemText(slot.hmenu, static_cast<UINT>(i)).empty())
                plain_row = i;
        }
        if (slot.send_to_title.empty() && plain_row >= 0)
            slot.send_to_title = MenuItemText(slot.hmenu, static_cast<UINT>(plain_row));
        if (!slot.send_to_title.empty()) {
            if (!slot.default_menu_only) slot.send_to_expected = CountSendToTargets();
            IShellItemArray* selection = nullptr;
            IContextMenu* native = nullptr;
            HRESULT native_hr = SHCreateShellItemArrayFromDataObject(bind.data, IID_PPV_ARGS(&selection));
            if (SUCCEEDED(native_hr)) {
                native_hr = selection->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&native));
                selection->Release();
            }
            HMENU native_popup = native ? CreatePopupMenu() : nullptr;
            if (native_popup) {
                // No CMF_SYNCCASCADEMENU: synchronously initializing every
                // cascade turns this worker into a serial shadow query of the
                // whole default menu, making it structurally the slowest
                // worker on machines with legacy extensions (#77 regression).
                // CollectHandlerItems fills the Send to flyout through
                // InitMenuPopup with its own budget instead.
                native_hr = SafeQueryContextMenu(native, native_popup, 0, id_first, id_last,
                                                 qcm_flags);
                if (SUCCEEDED(native_hr)) {
                    ReleaseHandlerSlot(slot);
                    slot.menu = native;
                    slot.hmenu = native_popup;
                    native = nullptr;
                    native_popup = nullptr;
                }
            }
            if (native_popup) DestroyMenu(native_popup);
            if (native) native->Release();
        }
    }
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu3));
    slot.menu->QueryInterface(IID_PPV_ARGS(&slot.menu2));
    return S_OK;
}

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out) {
    if (!slot.menu || !slot.hmenu) return;
    if (!slot.send_to_title.empty()) InitMenuPopup(slot.menu2, slot.menu3, slot.hmenu, 0);
    const int count = GetMenuItemCount(slot.hmenu);
    const ULONGLONG deadline = GetTickCount64() + kNestedFlyoutBudgetMs;
    bool pending_separator = false;
    auto push = [&](CtxItemOut item) {
        if (item.text.empty()) return;
        item.clsid = slot.clsid;
        item.handler = slot.name;
        if (pending_separator && !out.empty()) {
            out.back().separator_after = true;
            pending_separator = false;
        }
        out.push_back(std::move(item));
    };
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(slot.hmenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            if (!slot.send_to_title.empty()) continue;
            if (!out.empty()) pending_separator = true;
            continue;
        }
        const bool enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        wchar_t title_mnemonic = 0;
        const std::wstring title =
            MenuItemText(slot.hmenu, static_cast<UINT>(i), &title_mnemonic);
        // The SendTo slot holds the whole default Shell menu; besides 发送到 it
        // also feeds the few rows only that menu produces (创建快捷方式).
        std::wstring extra_verb;
        if (!slot.send_to_title.empty() && !mii.hSubMenu)
            extra_verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
        const bool default_extra = IsDefaultMenuExtraVerb(extra_verb) ||
                                   ContainsVerb(slot.default_menu_verbs, extra_verb);
        const bool send_to_row = !slot.send_to_title.empty() &&
            (title == slot.send_to_title ||
             (mii.hSubMenu &&
              pulse::ipc::ToLowerVerb(CtxVerbOf(slot.menu, mii.wID, slot.id_first)) == L"sendto"));
        if (!slot.send_to_title.empty() && !default_extra && !send_to_row) continue;
        // A drive's menu has no 发送到; its slot only feeds the default extras.
        if (slot.default_menu_only && !default_extra) continue;
        if (mii.hSubMenu) {
            const std::wstring parent_verb = slot.send_to_title.empty()
                ? CtxVerbOf(slot.menu, mii.wID, slot.id_first) : L"sendto";
            const bool open_with = slot.send_to_title.empty() &&
                                   IsOpenWithSubmenuVerb(parent_verb);
            if (!open_with && IsBuiltinContextVerb(parent_verb, background)) continue;
            const std::wstring& parent_text = title;
            if (parent_text.empty()) continue;
            // Send to fills itself on WM_INITMENUPOPUP and sits near the end of
            // the default menu, so slower flyouts above it could use up the
            // shared budget and leave it empty (#77). It gets its own, and so
            // does 打开方式, which also enumerates its apps on first open.
            const bool send_to = !slot.send_to_title.empty() ||
                                 pulse::ipc::ToLowerVerb(parent_verb) == L"sendto";
            std::vector<CtxItemOut> kids;
            CollectSubmenuLeaves(slot.menu, slot.menu2, slot.menu3, mii.hSubMenu,
                                 static_cast<UINT>(i), background, slot.id_first, enabled, 0,
                                 send_to || open_with ? GetTickCount64() + kNestedFlyoutBudgetMs
                                                      : deadline,
                                 kids, open_with, send_to ? slot.send_to_expected : 0);
            // An empty 打开方式 adds nothing over Pulse's own 打开方式… row.
            if (kids.empty() && open_with) continue;
            if (kids.empty()) {
                if (!KeepFlyoutParentWithoutLeaves(mii.wID, slot.id_first, slot.id_last))
                    continue;
                CtxItemOut item;
                item.id = mii.wID;
                // An empty Send to flyout is not an invokable destination.
                item.enabled = enabled && !send_to;
                item.verb = parent_verb;
                item.text = parent_text;
                item.mnemonic = title_mnemonic;
                item.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
                push(std::move(item));
                continue;
            }
            CtxItemOut header;
            header.id = 0;
            header.enabled = enabled;
            header.has_children = true;
            header.verb = parent_verb;
            header.text = parent_text;
            header.mnemonic = title_mnemonic;
            header.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
            push(std::move(header));
            for (auto& k : kids) {
                k.clsid = slot.clsid;
                k.handler = slot.name;
                out.push_back(std::move(k));
            }
            continue;
        }
        if (mii.wID < slot.id_first || mii.wID > slot.id_last) continue;
        const std::wstring verb = CtxVerbOf(slot.menu, mii.wID, slot.id_first);
        if (IsBuiltinContextVerb(verb, background) &&
            !(slot.shell_namespace && _wcsicmp(verb.c_str(), L"pintohome") == 0)) continue;
        CtxItemOut item;
        item.id = mii.wID;
        // A Send to row without its flyout cannot send anywhere (#77).
        item.enabled = enabled && (slot.send_to_title.empty() || default_extra);
        item.verb = verb;
        item.text = MenuItemText(slot.hmenu, static_cast<UINT>(i), &item.mnemonic);
        item.icon = MenuItemIcon(slot.hmenu, static_cast<UINT>(i));
        push(std::move(item));
    }
}

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text) {
    if (id != 0) {
        for (const auto& item : items)
            if (item.id == id) return item.id;
    }
    if (!verb.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.verb == verb) return item.id;
    }
    if (!text.empty()) {
        for (const auto& item : items)
            if (!item.has_children && item.id != 0 && item.text == text) return item.id;
    }
    return 0;
}

std::wstring NativeSendToTitle(wchar_t* mnemonic) {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) return {};
    const wchar_t* raw = nullptr;
    const int length = LoadStringW(shell32, 30312, reinterpret_cast<LPWSTR>(&raw), 0);
    if (length <= 0 || !raw) return {};
    const std::wstring text(raw, static_cast<size_t>(length));
    if (mnemonic) *mnemonic = MenuMnemonic(text);
    return CleanMenuText(text);
}

std::vector<SendToTarget> EnumerateSendToTargets() {
    std::vector<SendToTarget> out;
    PIDLIST_ABSOLUTE root = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_SendTo, 0, nullptr, &root)) && root) {
        IShellFolder* folder = nullptr;
        if (SUCCEEDED(SHBindToObject(nullptr, root, nullptr, IID_PPV_ARGS(&folder))) && folder) {
            std::vector<PITEMID_CHILD> children;
            IEnumIDList* items = nullptr;
            // Hidden entries (desktop.ini) are not destinations.
            if (folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items) == S_OK &&
                items) {
                PITEMID_CHILD child = nullptr;
                while (items->Next(1, &child, nullptr) == S_OK) children.push_back(child);
                items->Release();
            }
            std::stable_sort(children.begin(), children.end(),
                [folder](PCUITEMID_CHILD a, PCUITEMID_CHILD b) {
                    const HRESULT hr = folder->CompareIDs(0, a, b);
                    return SUCCEEDED(hr) && static_cast<short>(HRESULT_CODE(hr)) < 0;
                });
            for (PITEMID_CHILD child : children) {
                STRRET name{};
                wchar_t* text = nullptr;
                if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &name)) &&
                    SUCCEEDED(StrRetToStrW(&name, child, &text)) && text) {
                    SendToTarget target;
                    target.text = CleanMenuText(text);
                    target.pidl = ILCombine(root, child);
                    CoTaskMemFree(text);
                    if (target.pidl && !target.text.empty()) out.push_back(std::move(target));
                    else if (target.pidl) CoTaskMemFree(target.pidl);
                }
                CoTaskMemFree(child);
            }
            folder->Release();
        }
        CoTaskMemFree(root);
    }
    // No "insert a disk" prompt for an empty card reader.
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t drive_root[] = L"A:\\";
        drive_root[0] = static_cast<wchar_t>(L'A' + i);
        const UINT type = GetDriveTypeW(drive_root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_CDROM) continue;
        // Explorer hides empty removable drives; optical drives are listed
        // without media (and are not spun up here).
        if (type == DRIVE_REMOVABLE &&
            !GetVolumeInformationW(drive_root, nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0))
            continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(drive_root, nullptr, &pidl, 0, nullptr)) || !pidl) continue;
        wchar_t* name = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &name)) && name) {
            SendToTarget target;
            target.text = CleanMenuText(name);
            target.pidl = pidl;
            pidl = nullptr;
            CoTaskMemFree(name);
            if (!target.text.empty()) out.push_back(std::move(target));
            else CoTaskMemFree(target.pidl);
        }
        if (pidl) CoTaskMemFree(pidl);
    }
    SetThreadErrorMode(old_mode, nullptr);
    return out;
}

void FreeSendToTargets(std::vector<SendToTarget>& targets) noexcept {
    for (auto& target : targets) {
        if (target.pidl) CoTaskMemFree(target.pidl);
        target.pidl = nullptr;
    }
    targets.clear();
}

void NativeSendToItems(const std::vector<SendToTarget>& targets, const std::wstring& title,
                       wchar_t mnemonic, UINT id_first, UINT id_last,
                       std::vector<CtxItemOut>& out) {
    if (title.empty() || targets.empty()) return;
    const std::wstring handler = title + L" (Pulse)";
    CtxItemOut header;
    header.has_children = true;
    header.verb = L"sendto";
    header.text = title;
    header.mnemonic = mnemonic;
    header.clsid = pulse::ipc::kNativeSendToClsid;
    header.handler = handler;
    out.push_back(std::move(header));
    UINT id = id_first + 1;
    for (const auto& target : targets) {
        if (id > id_last) break;
        CtxItemOut child;
        child.id = id++;
        child.child = true;
        child.text = target.text;
        child.clsid = pulse::ipc::kNativeSendToClsid;
        child.handler = handler;
        out.push_back(std::move(child));
    }
}

HRESULT DropOnSendToTarget(PCIDLIST_ABSOLUTE target, IDataObject* data) {
    if (!target || !data) return E_INVALIDARG;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(target, IID_PPV_ARGS(&parent), &child);
    if (FAILED(hr) || !parent) return FAILED(hr) ? hr : E_FAIL;
    IDropTarget* drop = nullptr;
    hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr,
                               reinterpret_cast<void**>(&drop));
    parent->Release();
    if (FAILED(hr) || !drop) return FAILED(hr) ? hr : E_FAIL;
    // As the SendTo extension does: copy, Shift moves, Ctrl+Shift links —
    // told to the target as the matching drag keys.
    const DWORD keys = SendToDropKeys(GetAsyncKeyState(VK_SHIFT) < 0,
                                      GetAsyncKeyState(VK_CONTROL) < 0);
    POINTL point{};
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    hr = drop->DragEnter(data, keys, point, &effect);
    if (SUCCEEDED(hr) && effect != DROPEFFECT_NONE) {
        effect = SendToDropEffect(keys);
        hr = drop->Drop(data, keys, point, &effect);
    } else {
        drop->DragLeave();
        if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    drop->Release();
    return hr;
}

std::vector<CtxItemOut> MergeNativeSendTo(const std::vector<CtxItemOut>& native,
                                          const std::vector<CtxItemOut>& slot_rows,
                                          std::vector<std::wstring>* slot_only,
                                          std::vector<std::wstring>* native_only) {
    std::unordered_set<std::wstring> native_texts;
    for (const auto& item : native)
        if (item.child) native_texts.insert(item.text);
    if (native_texts.empty()) return slot_rows;
    std::vector<CtxItemOut> extra;
    std::vector<CtxItemOut> rest;
    std::unordered_set<std::wstring> slot_texts;
    bool in_send_to = false;
    bool filled = false;
    for (const auto& item : slot_rows) {
        if (!item.child) {
            const bool send_to = pulse::ipc::ToLowerVerb(item.verb) == L"sendto";
            in_send_to = send_to && item.has_children;
            // The flyout header, or the inert row of an empty flyout.
            if (send_to) continue;
            rest.push_back(item);
            continue;
        }
        if (!in_send_to) {
            rest.push_back(item);
            continue;
        }
        filled = true;
        slot_texts.insert(item.text);
        if (native_texts.contains(item.text)) continue;
        if (slot_only) slot_only->push_back(item.text);
        CtxItemOut row = item;
        row.separator_after = false;
        extra.push_back(std::move(row));
    }
    if (native_only && filled) {
        for (const auto& item : native)
            if (item.child && !slot_texts.contains(item.text)) native_only->push_back(item.text);
    }
    extra.insert(extra.end(), rest.begin(), rest.end());
    return extra;
}

bool InvokeTaskbandPin(const std::vector<std::wstring>& paths, HRESULT& result) {
    if (paths.empty()) {
        result = E_INVALIDARG;
        return true;
    }
    // twinui.pcshell.dll and its TaskbarPinManager are missing or different on
    // older builds; report that so the caller falls back to InvokeCommand.
    HMODULE hTwin = GetModuleHandleW(L"twinui.pcshell.dll");
    if (!hTwin) hTwin = LoadLibraryExW(L"twinui.pcshell.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hTwin) return false;

    using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(hTwin, "DllGetClassObject")));
    if (!get_class_object) return false;

    IClassFactory* factory = nullptr;
    if (FAILED(get_class_object(kCLSID_TaskbarPinManager, IID_IClassFactory,
                                reinterpret_cast<void**>(&factory))) || !factory)
        return false;
    ITaskbarPinManagerLocal* manager = nullptr;
    const HRESULT create_hr = factory->CreateInstance(nullptr, kIID_ITaskbarPinManager,
                                                      reinterpret_cast<void**>(&manager));
    factory->Release();
    if (FAILED(create_hr) || !manager) return false;

    // The manager accepts the request only from Explorer, which it checks via
    // CoGetCallContext. COM keeps the context pointer without a reference, so
    // the object must outlive the restore below.
    ExplorerCallContext call_context;
    struct ContextSwitch {
        IUnknown* previous = nullptr;
        bool active = false;
        explicit ContextSwitch(IUnknown* next) {
            active = SUCCEEDED(CoSwitchCallContext(next, &previous));
        }
        ~ContextSwitch() {
            IUnknown* ours = nullptr;
            if (active) CoSwitchCallContext(previous, &ours);
        }
        ContextSwitch(const ContextSwitch&) = delete;
        ContextSwitch& operator=(const ContextSwitch&) = delete;
    };

    // The caller kind Explorer's CTaskbandPin passes for a context-menu pin.
    constexpr int kPinFromContextMenu = 4;
    HRESULT first_failure = S_OK;
    {
        const ContextSwitch context_switch(static_cast<ICallingProcessInfoLocal*>(&call_context));
        for (const auto& path : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            HRESULT hr = pulse::shell_id::ParseId(ToParsingPath(path), &pidl);
            if (SUCCEEDED(hr) && !pidl) hr = E_FAIL;
            if (SUCCEEDED(hr)) {
                hr = manager->PinPidl(pidl, kPinFromContextMenu);
                CoTaskMemFree(pidl);
            }
            if (FAILED(hr) && SUCCEEDED(first_failure)) first_failure = hr;
        }
    }
    manager->Release();
    result = first_failure;
    return true;
}

} // namespace pulse::shell