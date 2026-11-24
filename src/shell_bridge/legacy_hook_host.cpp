#include "legacy_hook_host.h"
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
}
LegacyHookHost::LegacyHookHost(OPENFILENAMEW* input) noexcept : input_(input),
    hook_(input && (input->Flags & OFN_ENABLEHOOK) ? input->lpfnHook : nullptr), thread_(GetCurrentThreadId()) {}
LegacyHookHost::LegacyHookHost(OPENFILENAMEA* input) noexcept : input_(input),
    hook_(input && (input->Flags & OFN_ENABLEHOOK) ? input->lpfnHook : nullptr), thread_(GetCurrentThreadId()), ansi_(true) {}
LegacyHookHost::~LegacyHookHost() {
    if (child_ && GetCurrentThreadId() == thread_) DestroyWindow(child_);
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
    return static_cast<INT_PTR>(answer);
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
HRESULT LegacyHookHost::Prepare() {
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
    if(!data || !bytes || bytes>1024*1024)return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
    const auto view=std::span(static_cast<const BYTE*>(data),bytes);
    if(!AppendTemplateSupported(view))return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
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
