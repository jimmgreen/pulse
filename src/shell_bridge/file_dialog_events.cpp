#include "file_dialog_events.h"

#include <atomic>
#include <new>
#include <wrl/client.h>

namespace pulse::shell_bridge::detail {
namespace {
using Microsoft::WRL::ComPtr;
class EventProxy final : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    EventProxy(std::shared_ptr<FileDialogEventOwner> owner, IFileDialogEvents* sink)
        : owner_(std::move(owner)), sink_(sink) { sink_.As(&control_sink_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents)
            *output = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents && control_sink_)
            *output = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override {
        return Call([](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnFileOk(owner); });
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem* folder) override {
        return Call([&](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnFolderChanging(owner, folder); });
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override {
        return Call([](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnFolderChange(owner); });
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override {
        return Call([](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnSelectionChange(owner); });
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem* item, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = FDESVR_DEFAULT;
        return Call([&](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnShareViolation(owner, item, response); });
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override {
        return Call([](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnTypeChange(owner); });
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = FDEOR_DEFAULT;
        return Call([&](IFileDialogEvents* sink, IFileDialog* owner) { return sink->OnOverwrite(owner, item, response); });
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD id, DWORD item) override {
        return ControlCall([&](IFileDialogControlEvents* sink, IFileDialogCustomize* owner) { return sink->OnItemSelected(owner, id, item); });
    }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize*, DWORD id) override {
        return ControlCall([&](IFileDialogControlEvents* sink, IFileDialogCustomize* owner) { return sink->OnButtonClicked(owner, id); });
    }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD id, BOOL checked) override {
        return ControlCall([&](IFileDialogControlEvents* sink, IFileDialogCustomize* owner) { return sink->OnCheckButtonToggled(owner, id, checked); });
    }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD id) override {
        return ControlCall([&](IFileDialogControlEvents* sink, IFileDialogCustomize* owner) { return sink->OnControlActivating(owner, id); });
    }
private:
    template<class F> HRESULT ControlCall(F&& invoke) noexcept {
        return Call([&](IFileDialogEvents*, IFileDialog* owner) {
            ComPtr<IFileDialogControlEvents> sink = control_sink_;
            if (!sink) return E_NOINTERFACE;
            ComPtr<IFileDialogCustomize> customize;
            const HRESULT hr = owner->QueryInterface(IID_PPV_ARGS(&customize));
            return FAILED(hr) ? hr : invoke(sink.Get(), customize.Get());
        });
    }
    template<class F> HRESULT Call(F&& invoke) noexcept {
        // A callback may Unadvise and release the proxy, owner or sink. Hold all
        // three until invocation unwinds; release owner before this self guard.
        AddRef();
        struct Hold { EventProxy* self; ~Hold() { self->Release(); } } hold{this};
        try {
            if (GetCurrentThreadId() != owner_->thread) return RPC_E_WRONG_THREAD;
            ComPtr<IFileDialog> owner;
            {
                std::lock_guard lock(owner_->mutex);
                if (!owner_->dialog) return CO_E_OBJNOTCONNECTED;
                owner = owner_->dialog;
            }
            ComPtr<IFileDialogEvents> sink = sink_;
            return invoke(sink.Get(), owner.Get());
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<FileDialogEventOwner> owner_;
    ComPtr<IFileDialogEvents> sink_;
    ComPtr<IFileDialogControlEvents> control_sink_;
};
}

HRESULT CreateFileDialogEventProxy(std::shared_ptr<FileDialogEventOwner> owner, IFileDialogEvents* sink,
                                  IFileDialogEvents** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!owner || !sink) return E_INVALIDARG;
    try { *output = new EventProxy(std::move(owner), sink); return S_OK; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
