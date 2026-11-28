#include "legacy_hook_host.h"
#include <algorithm>
#include <cstddef>
#include <commctrl.h>
#include <utility>
#include <cstring>
#include <dlgs.h>

namespace pulse::shell_bridge {
namespace {
constexpr wchar_t kProperty[] = L"Pulse.LegacyHookHost.Private";
UINT AttachMessage() { static const UINT message = RegisterWindowMessageW(L"Pulse.Picker.AttachLegacyChild.v1"); return message; }
constexpr LRESULT kAttached = 0x504c4348L;
struct EmptyTemplate { DLGTEMPLATE dialog; WORD menu, window_class, title; };

// Empty child dialog template matching native comdlg32 Explorer sub-dialog
// (0x44000444 = WS_CHILD | WS_CLIPCHILDREN | DS_CONTROL | DS_SETFONT | DS_3DLOOK).
std::vector<BYTE> BuildEmptyExplorerChildTemplate() {
    std::vector<BYTE> out;
    auto push_word = [&](WORD v) {
        const auto* p = reinterpret_cast<const BYTE*>(&v);
        out.insert(out.end(), p, p + 2);
    };
    auto push_dword = [&](DWORD v) {
        const auto* p = reinterpret_cast<const BYTE*>(&v);
        out.insert(out.end(), p, p + 4);
    };
    push_word(1);          // dlgVer
    push_word(0xFFFF);     // signature (DLGTEMPLATEEX)
    push_dword(0);         // helpID
    push_dword(0);         // exStyle
    push_dword(WS_CHILD | WS_CLIPCHILDREN | DS_CONTROL | DS_SETFONT | DS_3DLOOK);
    push_word(0);          // cDlgItems
    push_word(0);          // x
    push_word(0);          // y
    push_word(200);        // cx
    push_word(40);         // cy
    push_word(0);          // menu
    push_word(0);          // windowClass
    push_word(0);          // title
    push_word(9);          // pointsize
    push_word(400);        // weight
    out.push_back(0);      // italic
    out.push_back(1);      // charset (DEFAULT_CHARSET)
    const wchar_t font[] = L"MS Shell Dlg";
    for (wchar_t ch : font) push_word(static_cast<WORD>(ch));
    while (out.size() % 4 != 0) out.push_back(0);
    return out;
}

std::wstring TrimLabel(std::wstring text) {
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L':' || text.back() == L'\uFF1A'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\t')) ++start;
    if (start) text.erase(0, start);
    return text;
}

std::wstring WindowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0 || len > 4096) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return buf;
}

// Photoshop Save for Web's Explorer child dialog already contains these four
// controls when the hook's WM_INITDIALOG runs (0x0BBB/0x0BB9 Settings,
// 0x0BBC/0x0BBA Slices). Pre-create them when absent so the hook populates them.
void CreateMirroredStubControls(HWND dialog) noexcept {
    if (!dialog || GetDlgItem(dialog, 0x0BB9) || GetDlgItem(dialog, 0x0BBA)) return;
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    const auto make = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y, int cx, int cy, int id) {
        HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
            MulDiv(x, dpi, 144), MulDiv(y, dpi, 144),
            MulDiv(cx, dpi, 144), MulDiv(cy, dpi, 144),
            dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (control) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
    };
    make(L"Static", zh ? L"\u8BBE\u7F6E\uFF1A" : L"Settings:", SS_LEFT, 2, 10, 140, 19, 0x0BBB);
    make(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 144, 5, 369, 120, 0x0BB9);
    make(L"Static", zh ? L"\u5207\u7247\uFF1A" : L"Slices:", SS_LEFT, 2, 48, 140, 19, 0x0BBC);
    make(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 144, 43, 369, 120, 0x0BBA);
}
}
LegacyHookHost::LegacyHookHost(OPENFILENAMEW* input) noexcept : input_(input),
    hook_(input && (input->Flags & OFN_ENABLEHOOK) ? input->lpfnHook : nullptr), thread_(GetCurrentThreadId()) {}
LegacyHookHost::LegacyHookHost(OPENFILENAMEA* input) noexcept : input_(input),
    hook_(input && (input->Flags & OFN_ENABLEHOOK) ? input->lpfnHook : nullptr), thread_(GetCurrentThreadId()), ansi_(true) {}
LegacyHookHost::~LegacyHookHost() {
    if (child_ && GetCurrentThreadId() == thread_) DestroyWindow(child_);
    filter_combo_ = nullptr;
    if (parent_ && GetCurrentThreadId() == thread_) {
        HWND owned = owns_parent_ ? parent_ : nullptr;
        owns_parent_ = false;
        RemoveWindowSubclass(parent_, ParentProc, reinterpret_cast<UINT_PTR>(this));
        parent_ = nullptr;
        if (owned && IsWindow(owned)) DestroyWindow(owned);
    }
}
INT_PTR CALLBACK LegacyHookHost::DialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LegacyHookHost*>(GetPropW(window, kProperty));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<LegacyHookHost*>(lp);
        if (!self || !SetPropW(window, kProperty, self)) return FALSE;
        self->child_ = window;
        if (self->mirrored_) CreateMirroredStubControls(window);
        lp = reinterpret_cast<LPARAM>(self->input_);
    }
    if (!self) return FALSE;
    const UINT_PTR answer = self->hook_ ? self->hook_(window, message, wp, lp) : 0;
    if (message == WM_NOTIFY && self->notifying_) self->notification_return_ = answer;
    if (message == WM_NCDESTROY) {
        if (self->attached_ && IsWindow(self->parent_)) SendMessageW(self->parent_, AttachMessage(), 0, 0);
        self->attached_ = false;
        RemovePropW(window, kProperty);
        self->child_ = nullptr;
    }
    // Mirrored Explorer hook sub-dialogs must return TRUE from WM_INITDIALOG so the
    // dialog manager keeps the child window alive even when the hook returns 0.
    if (message == WM_INITDIALOG && self->mirrored_) return TRUE;
    return static_cast<INT_PTR>(answer);
}
bool LegacyHookHost::IsEmptyTemplate(std::span<const BYTE> bytes) noexcept {
    if (bytes.size() < 18) return false;
    WORD first = 0, signature = 0;
    std::memcpy(&first, bytes.data(), 2);
    std::memcpy(&signature, bytes.data() + 2, 2);
    WORD count = 0;
    if (signature == 0xFFFF) {
        if (first != 1 || bytes.size() < 18) return false;
        std::memcpy(&count, bytes.data() + 16, 2);
    } else {
        std::memcpy(&count, bytes.data() + 8, 2);
    }
    return count == 0;
}
bool LegacyHookHost::AppendTemplateSupported(std::span<const BYTE> bytes) noexcept {
    struct Reader {
        std::span<const BYTE> bytes; size_t at=0;
        bool skip(size_t n) {if(n>bytes.size()-at)return false;at+=n;return true;}
        bool word(WORD& out) {if(bytes.size()-at<2)return false;std::memcpy(&out,bytes.data()+at,2);at+=2;return true;}
        bool dword(DWORD& out) {if(bytes.size()-at<4)return false;std::memcpy(&out,bytes.data()+at,4);at+=4;return true;}
        bool field() {WORD value=0;if(!word(value))return false;if(value==0xffff)return skip(2);while(value)if(!word(value))return false;return true;}
    } r{bytes};
    if(bytes.size()<18 || bytes.size()>1024*1024)return false;
    WORD first=0,signature=0;r.word(first);r.word(signature);r.at=0;
    const bool extended=signature==0xffff;
    DWORD style=0;WORD count=0;
    if(extended){if(first!=1 || !r.skip(12) || !r.dword(style) || !r.word(count) || !r.skip(8))return false;}
    else{if(!r.dword(style)||!r.skip(4)||!r.word(count)||!r.skip(8))return false;}
    if(!(style&WS_CHILD) || count>256 || !r.field() || !r.field() || !r.field())return false;
    if(style&DS_SETFONT){if(!r.skip(extended?6:2)||!r.field())return false;}
    for(WORD i=0;i<count;++i){
        if(!r.skip((4-r.at%4)%4))return false;
        DWORD id=0;
        if(extended){if(!r.skip(20)||!r.dword(id))return false;}
        else{WORD short_id=0;if(!r.skip(16)||!r.word(short_id))return false;id=short_id;}
        if(id==stc32 || !r.field() || !r.field())return false;
        WORD creation=0;if(!r.word(creation)||creation)return false;
    }
    return true;
}
HRESULT LegacyHookHost::Prepare(bool allow_empty_template_fallback) {
    if(prepared_)return S_OK;
    if(!input_ || GetCurrentThreadId()!=thread_)return E_INVALIDARG;
    const auto* wide=static_cast<OPENFILENAMEW*>(input_);
    const auto* narrow=static_cast<OPENFILENAMEA*>(input_);
    const DWORD flags=ansi_?narrow->Flags:wide->Flags;
    const HINSTANCE instance=ansi_?narrow->hInstance:wide->hInstance;
    if(!(flags&(OFN_ENABLETEMPLATE|OFN_ENABLETEMPLATEHANDLE))){prepared_=true;return S_OK;}
    const void* data=nullptr;size_t bytes=0;HGLOBAL locked=nullptr;
    if(flags&OFN_ENABLETEMPLATEHANDLE){
        const auto memory=reinterpret_cast<HGLOBAL>(instance);bytes=GlobalSize(memory);
        if(bytes && bytes<=1024*1024){data=GlobalLock(memory);if(data)locked=memory;}
    }else{
        const HRSRC resource=ansi_?FindResourceA(instance,narrow->lpTemplateName,MAKEINTRESOURCEA(5)):FindResourceW(instance,wide->lpTemplateName,RT_DIALOG);
        if(resource){bytes=SizeofResource(instance,resource);const HGLOBAL loaded=LoadResource(instance,resource);if(loaded)data=LockResource(loaded);}
    }
    struct Unlock { HGLOBAL value; ~Unlock(){if(value)GlobalUnlock(value);} } unlock{locked};
    if(!data || !bytes || bytes>1024*1024) {
        if (allow_empty_template_fallback && (flags & OFN_ENABLETEMPLATEHANDLE) && (flags & OFN_ENABLEHOOK) && hook_) {
            template_ = BuildEmptyExplorerChildTemplate();
            prepared_ = true;
            return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
    }
    const auto view=std::span(static_cast<const BYTE*>(data),bytes);
    if(!AppendTemplateSupported(view)) {
        if (allow_empty_template_fallback && (flags & OFN_ENABLETEMPLATEHANDLE) && (flags & OFN_ENABLEHOOK) && hook_ && IsEmptyTemplate(view)) {
            template_ = BuildEmptyExplorerChildTemplate();
            prepared_ = true;
            return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    try {template_.assign(view.begin(),view.end());}catch(...){return E_OUTOFMEMORY;}
    prepared_=true;return S_OK;
}
LRESULT CALLBACK LegacyHookHost::ParentProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<LegacyHookHost*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ParentProc, id);
        self->parent_ = nullptr;
        self->owns_parent_ = false;
    } else if (message == WM_CLOSE && self->owns_parent_) {
        if (self->parent_message_) {
            try { self->parent_message_(WM_CLOSE, wp, lp); } catch (...) {}
        }
        return 0;
    } else if (message >= CDM_FIRST && message <= CDM_SETDEFEXT && self->parent_message_) {
        try { return self->parent_message_(message, wp, lp); } catch (...) { return -1; }
    }
    return DefSubclassProc(window, message, wp, lp);
}
HRESULT LegacyHookHost::Create(HWND parent, ParentMessage message) {
    if (!input_ || child_ || GetCurrentThreadId() != thread_ || !parent || !IsWindow(parent))
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    const auto* wide = static_cast<OPENFILENAMEW*>(input_);
    const auto* narrow = static_cast<OPENFILENAMEA*>(input_);
    const DWORD flags = ansi_ ? narrow->Flags : wide->Flags;
    const HINSTANCE instance = ansi_ ? narrow->hInstance : wide->hInstance;
    if ((flags & OFN_ENABLEHOOK) && !hook_) return E_INVALIDARG;
    const bool has_template = (flags & (OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) != 0;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(parent, &pid);
    const bool same_thread = tid == thread_ && pid == GetCurrentProcessId();
    HWND host_parent = parent;
    bool created_proxy = false;
    if (!same_thread) {
        // Templates must visibly attach to a same-thread in-process host window.
        // Hook-only dialogs only need a caller-thread parent for CDM_* messages.
        if (has_template) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        RECT rc{0, 0, 800, 600};
        GetWindowRect(parent, &rc);
        const int width = rc.right > rc.left ? (rc.right - rc.left) : 800;
        const int height = rc.bottom > rc.top ? (rc.bottom - rc.top) : 600;
        host_parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
            rc.left, rc.top, width, height, nullptr, nullptr, nullptr, nullptr);
        if (!host_parent) {
            const DWORD err = GetLastError();
            return HRESULT_FROM_WIN32(err ? err : ERROR_INVALID_WINDOW_HANDLE);
        }
        created_proxy = true;
    }
    struct ProxyGuard {
        LegacyHookHost* host;
        HWND proxy;
        bool committed = false;
        ~ProxyGuard() {
            if (committed || !proxy) return;
            if (host->parent_ == proxy) {
                RemoveWindowSubclass(proxy, ParentProc, reinterpret_cast<UINT_PTR>(host));
                host->parent_ = nullptr;
                host->owns_parent_ = false;
            }
            if (IsWindow(proxy)) DestroyWindow(proxy);
        }
    } guard{this, created_proxy ? host_parent : nullptr};
    parent_message_ = std::move(message);
    parent_ = host_parent;
    owns_parent_ = created_proxy;
    if (parent_message_ && !SetWindowSubclass(host_parent, ParentProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
        parent_ = nullptr; owns_parent_ = false; return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    if (!prepared_) { const HRESULT ready=Prepare(); if(FAILED(ready)) return ready; }
    EmptyTemplate empty{};
    empty.dialog.style = WS_CHILD | DS_CONTROL;
    const DLGTEMPLATE* layout = template_.empty() ? &empty.dialog : reinterpret_cast<const DLGTEMPLATE*>(template_.data());
    const HWND result = ansi_ ? CreateDialogIndirectParamA(instance, layout, host_parent, DialogProc, reinterpret_cast<LPARAM>(this)) :
        CreateDialogIndirectParamW(instance, layout, host_parent, DialogProc, reinterpret_cast<LPARAM>(this));
    if (!result || result != child_ || !IsWindow(result)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (has_template) {
        if (GetParent(result) != host_parent || !(GetWindowLongPtrW(result, GWL_STYLE) & WS_CHILD) ||
            SendMessageW(host_parent, AttachMessage(), reinterpret_cast<WPARAM>(result), 0) != kAttached) {
            DestroyWindow(result); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        parent_ = host_parent; attached_ = true;
        ShowWindow(result, SW_SHOWNOACTIVATE);
    }
    guard.committed = true;
    return S_OK;
}

HRESULT LegacyHookHost::CreateMirrored(ParentMessage message, const std::vector<std::wstring>& filter_labels,
                                       UINT filter_index_1based) {
    if (!input_ || child_ || GetCurrentThreadId() != thread_ || !hook_) return E_INVALIDARG;
    if (!prepared_) {
        const HRESULT ready = Prepare(true);
        if (FAILED(ready)) return ready;
    }
    const int host_w = 774;
    const int host_h = 86;
    // Off-screen yet WS_VISIBLE: Photoshop's Save for Web hook only populates its
    // Settings / Slices ComboBoxes once the sub-dialog is visible.
    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
                                 WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                                 -20000, -20000, host_w, host_h,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!popup) {
        const DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err ? err : ERROR_INVALID_WINDOW_HANDLE);
    }
    struct PopupGuard {
        LegacyHookHost* host;
        HWND popup;
        bool committed = false;
        ~PopupGuard() {
            if (committed || !popup) return;
            if (host->parent_ == popup) {
                RemoveWindowSubclass(popup, ParentProc, reinterpret_cast<UINT_PTR>(host));
                host->parent_ = nullptr;
                host->owns_parent_ = false;
                host->filter_combo_ = nullptr;
            }
            if (IsWindow(popup)) DestroyWindow(popup);
        }
    } guard{this, popup};

    // Provide a standard cmb1 (0x470) filter ComboBox stub inside the parent so hooks
    // querying GetDlgItem(GetParent(hDlg), cmb1) see the current Save-as-type index.
    filter_combo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | CBS_DROPDOWNLIST,
                                    0, 0, 200, 120,
                                    popup, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(cmb1)),
                                    GetModuleHandleW(nullptr), nullptr);
    if (filter_combo_) {
        for (const auto& label : filter_labels) {
            SendMessageW(filter_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SyncFilterIndex(filter_index_1based);
    }

    parent_message_ = std::move(message);
    parent_ = popup;
    owns_parent_ = true;
    mirrored_ = true;
    if (parent_message_ && !SetWindowSubclass(popup, ParentProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
        parent_ = nullptr;
        owns_parent_ = false;
        mirrored_ = false;
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    const std::vector<BYTE> fallback = template_.empty() ? BuildEmptyExplorerChildTemplate() : std::vector<BYTE>{};
    const DLGTEMPLATE* layout = reinterpret_cast<const DLGTEMPLATE*>(template_.empty() ? fallback.data() : template_.data());
    const HINSTANCE mod = GetModuleHandleW(nullptr);
    const HWND result = ansi_
        ? CreateDialogIndirectParamA(mod, layout, popup, DialogProc, reinterpret_cast<LPARAM>(this))
        : CreateDialogIndirectParamW(mod, layout, popup, DialogProc, reinterpret_cast<LPARAM>(this));
    if (!result || result != child_ || !IsWindow(result)) {
        mirrored_ = false;
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    SetWindowPos(result, nullptr, 0, 0, host_w, host_h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(result, SW_SHOWNOACTIVATE);
    SendMessageW(result, WM_SIZE, SIZE_RESTORED, MAKELPARAM(host_w, host_h));
    guard.committed = true;
    return S_OK;
}

void LegacyHookHost::SyncFilterIndex(UINT filter_index_1based) {
    if (!filter_combo_ || !IsWindow(filter_combo_)) return;
    const LRESULT count = SendMessageW(filter_combo_, CB_GETCOUNT, 0, 0);
    if (count <= 0) return;
    const LRESULT zero_based = filter_index_1based > 0 ? static_cast<LRESULT>(filter_index_1based - 1) : 0;
    SendMessageW(filter_combo_, CB_SETCURSEL, static_cast<WPARAM>(std::clamp<LRESULT>(zero_based, 0, count - 1)), 0);
    if (child_ && IsWindow(child_)) {
        SendMessageW(child_, WM_COMMAND, MAKEWPARAM(cmb1, CBN_SELCHANGE), reinterpret_cast<LPARAM>(filter_combo_));
    }
}

std::vector<MirroredLegacyCombo> LegacyHookHost::ReadMirroredCombos() const {
    std::vector<MirroredLegacyCombo> out;
    if (!child_ || !IsWindow(child_) || GetCurrentThreadId() != thread_) return out;

    // Drain any queued initialization messages posted to the off-screen sub-dialog.
    MSG msg{};
    for (int pumped = 0; pumped < 16 && PeekMessageW(&msg, child_, 0, 0, PM_REMOVE); ++pumped) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    struct ChildInfo {
        HWND hwnd = nullptr;
        int id = 0;
        std::wstring cls;
        std::wstring text;
    };
    std::vector<ChildInfo> children;
    EnumChildWindows(child_, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<ChildInfo>*>(lp);
        if (GetParent(hwnd) != GetAncestor(hwnd, GA_PARENT)) return TRUE;
        wchar_t cls[64]{};
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        list->push_back({hwnd, GetDlgCtrlID(hwnd), cls, WindowText(hwnd)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&children));

    std::wstring pending_label;
    for (const auto& ch : children) {
        if (GetParent(ch.hwnd) != child_) continue;
        if (_wcsicmp(ch.cls.c_str(), L"Static") == 0) {
            const std::wstring trimmed = TrimLabel(ch.text);
            if (!trimmed.empty()) pending_label = trimmed;
            continue;
        }
        if (_wcsicmp(ch.cls.c_str(), L"ComboBox") != 0) continue;
        MirroredLegacyCombo combo;
        combo.hwnd = ch.hwnd;
        combo.control_id = ch.id;
        std::wstring explicit_label;
        if (ch.id == 0x0BB9) {
            if (HWND st = GetDlgItem(child_, 0x0BBB)) explicit_label = TrimLabel(WindowText(st));
        } else if (ch.id == 0x0BBA) {
            if (HWND st = GetDlgItem(child_, 0x0BBC)) explicit_label = TrimLabel(WindowText(st));
        }
        combo.label = !explicit_label.empty() ? std::move(explicit_label) : pending_label;
        pending_label.clear();
        combo.enabled = IsWindowEnabled(ch.hwnd) != FALSE;
        const LRESULT count = SendMessageW(ch.hwnd, CB_GETCOUNT, 0, 0);
        if (count <= 0 || count > 256) continue;
        combo.items.reserve(static_cast<size_t>(count));
        for (LRESULT i = 0; i < count; ++i) {
            const LRESULT len = SendMessageW(ch.hwnd, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
            if (len == CB_ERR || len < 0 || len > 4096) {
                combo.items.emplace_back();
                continue;
            }
            std::wstring item(static_cast<size_t>(len) + 1, L'\0');
            const LRESULT copied = SendMessageW(ch.hwnd, CB_GETLBTEXT, static_cast<WPARAM>(i),
                                                reinterpret_cast<LPARAM>(item.data()));
            item.resize(copied != CB_ERR && copied >= 0 ? static_cast<size_t>(copied) : 0);
            combo.items.push_back(std::move(item));
        }
        const LRESULT sel = SendMessageW(ch.hwnd, CB_GETCURSEL, 0, 0);
        combo.selected = (sel != CB_ERR && sel >= 0 && sel < static_cast<LRESULT>(combo.items.size()))
            ? static_cast<int>(sel) : -1;
        out.push_back(std::move(combo));
    }
    return out;
}

bool LegacyHookHost::SelectMirroredCombo(size_t index, int item_index) {
    const auto combos = ReadMirroredCombos();
    if (index >= combos.size() || !child_ || !IsWindow(child_)) return false;
    const auto& target = combos[index];
    if (!target.hwnd || !IsWindow(target.hwnd)) return false;
    if (item_index < 0 || item_index >= static_cast<int>(target.items.size())) return false;
    SendMessageW(target.hwnd, CB_SETCURSEL, static_cast<WPARAM>(item_index), 0);
    SendMessageW(child_, WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(target.control_id), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(target.hwnd));
    return true;
}
bool LegacyHookHost::Notify(UINT code) {
    if (!child_ || GetCurrentThreadId() != thread_ || notifying_) return false;
    notification_return_ = 0;
    SetWindowLongPtrW(child_, DWLP_MSGRESULT, 0);
    notifying_ = true;
    if (ansi_) {
        auto* input = static_cast<OPENFILENAMEA*>(input_);
        OFNOTIFYA event{{GetParent(child_), 0, code}, input, input->lpstrFile};
        SendMessageA(child_, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&event));
    } else {
        auto* input = static_cast<OPENFILENAMEW*>(input_);
        OFNOTIFYW event{{GetParent(child_), 0, code}, input, input->lpstrFile};
        SendMessageW(child_, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&event));
    }
    notifying_ = false;
    return child_ && notification_return_ != 0 && GetWindowLongPtrW(child_, DWLP_MSGRESULT) != 0;
}
bool LegacyHookHost::FileOk() {
    if (Notify(CDN_FILEOK)) return true;
    if (!child_ || !hook_) return false;
    // Explorer dialogs retain this registered compatibility notification after
    // CDN_FILEOK unless the latter vetoed the candidate.
    const UINT message = RegisterWindowMessageW(FILEOKSTRINGW);
    return message && hook_(child_, message, 0, reinterpret_cast<LPARAM>(input_)) != 0;
}

}
