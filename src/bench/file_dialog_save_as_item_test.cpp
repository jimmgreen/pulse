#include <windows.h>
#include <shlobj.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <functional>
#include <thread>
#include "../shell_bridge/save_as_item_model.h"
#include "../shell_bridge/file_dialog_properties.h"

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* name) { ++checks; if (!okay) ++failures; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", name); std::fflush(stdout); }
HRESULT NativeSave(IFileSaveDialog** output) {
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static const auto module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    const HRESULT hr = entry ? entry(CLSID_FileSaveDialog, IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    return FAILED(hr) ? hr : factory->CreateInstance(nullptr, IID_PPV_ARGS(output));
}
struct Items {
    fs::path root, folder_a, folder_b, file_a, file_b;
    ComPtr<IShellItem> item_a, item_b, directory_a, directory_b;
};
int PathKind(IShellItem* item, const Items& items) {
    if (!item) return 0;
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return -1;
    const fs::path path(name ? name : L""); CoTaskMemFree(name);
    if (path == items.folder_a) return 1;
    if (path == items.folder_b) return 2;
    if (path == items.file_a) return 3;
    if (path == items.file_b) return 4;
    return 9;
}
int NameKind(const wchar_t* name, const Items& items) {
    if (!name || !*name) return 0;
    if (name == items.file_a.filename().wstring()) return 1;
    if (name == items.file_b.filename().wstring()) return 2;
    if (std::wstring_view(name) == L"override.txt") return 3;
    return 9;
}
void Observe(IFileSaveDialog* dialog, const Items& items, const char* label) {
    ComPtr<IShellItem> folder, result;
    PWSTR filename = nullptr;
    ComPtr<IPropertyStore> properties;
    const HRESULT folder_hr = dialog->GetFolder(&folder), name_hr = dialog->GetFileName(&filename),
        result_hr = dialog->GetResult(&result), properties_hr = dialog->GetProperties(&properties);
    DWORD count = 0;
    const HRESULT count_hr = properties ? properties->GetCount(&count) : E_PENDING;
    PROPVARIANT title{};
    const HRESULT title_hr = properties ? properties->GetValue(PKEY_Title, &title) : E_PENDING;
    const bool seed = title_hr == S_OK && title.vt == VT_LPWSTR && title.pwszVal && std::wstring_view(title.pwszVal) == L"owned-title-seed";
    std::printf("OBSERVE case=%s folder_hr=%08lX folder_kind=%d name_hr=%08lX name_kind=%d result_hr=%08lX result_present=%u properties_hr=%08lX properties_count_hr=%08lX properties_count=%lu title_hr=%08lX title_vt=%u title_seed=%u\n",
        label, static_cast<unsigned long>(folder_hr), PathKind(folder.Get(), items), static_cast<unsigned long>(name_hr), NameKind(filename, items),
        static_cast<unsigned long>(result_hr), result ? 1u : 0u, static_cast<unsigned long>(properties_hr),
        static_cast<unsigned long>(count_hr), count, static_cast<unsigned long>(title_hr), static_cast<unsigned>(title.vt), seed ? 1u : 0u);
    CoTaskMemFree(filename); PropVariantClear(&title); std::fflush(stdout);
}
void Call(const char* name, HRESULT result) {
    std::printf("CALL method=%s hr=%08lX\n", name, static_cast<unsigned long>(result)); std::fflush(stdout);
}
struct ItemCounts { std::atomic<ULONG> references{1}; unsigned names = 0, parents = 0, attributes = 0, queries = 0; bool alive = true; };
class CountedItem final : public IShellItem {
    ComPtr<IShellItem> actual_;
    ItemCounts& counts_;
    HRESULT name_error_;
public:
    std::function<void()> on_name;
    std::function<void()> on_release;
    CountedItem(IShellItem* actual, ItemCounts& counts, HRESULT name_error = S_OK) : actual_(actual), counts_(counts), name_error_(name_error) {}
    void FailNames(HRESULT error) { name_error_ = error; }
    ~CountedItem() { counts_.alive = false; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        ++counts_.queries;
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IShellItem) return E_NOINTERFACE;
        *output = static_cast<IShellItem*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++counts_.references; }
    ULONG STDMETHODCALLTYPE Release() override { if (on_release) on_release(); const auto value = --counts_.references; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx* context, REFGUID handler, REFIID iid, void** output) override { return actual_->BindToHandler(context, handler, iid, output); }
    HRESULT STDMETHODCALLTYPE GetParent(IShellItem** output) override { ++counts_.parents; return actual_->GetParent(output); }
    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN kind, PWSTR* output) override {
        if (on_name) on_name();
        ++counts_.names; if (name_error_ != S_OK) { if (output) *output = nullptr; return name_error_; }
        return actual_->GetDisplayName(kind, output);
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF mask, SFGAOF* output) override { ++counts_.attributes; return actual_->GetAttributes(mask, output); }
    HRESULT STDMETHODCALLTYPE Compare(IShellItem* other, SICHINTF hint, int* order) override { return actual_->Compare(other, hint, order); }
};
void NativeCases(const Items& items) {
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        ComPtr<IFileSaveDialog> dialog;
        const HRESULT created = NativeSave(&dialog);
        Check(created == S_OK && dialog, "System32 factory creates unwrapped native Save dialog");
        if (!dialog) continue;
        Call("SetOptions", dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_OVERWRITEPROMPT));
        const char* label = "fresh";
        switch (scenario) {
        case 0: break;
        case 1: label = "item_A"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); break;
        case 2: label = "name_then_item"; Call("SetFileName", dialog->SetFileName(L"override.txt")); Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); break;
        case 3: label = "item_then_name"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetFileName", dialog->SetFileName(L"override.txt")); break;
        case 4: label = "folder_B_then_item_A"; Call("SetFolder(B)", dialog->SetFolder(items.directory_b.Get())); Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); break;
        case 5: label = "item_A_then_folder_B"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetFolder(B)", dialog->SetFolder(items.directory_b.Get())); break;
        case 6: label = "default_B_then_item_A"; Call("SetDefaultFolder(B)", dialog->SetDefaultFolder(items.directory_b.Get())); Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); break;
        case 7: label = "item_A_then_default_B"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetDefaultFolder(B)", dialog->SetDefaultFolder(items.directory_b.Get())); break;
        case 8: label = "item_A_then_item_B"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetSaveAsItem(B)", dialog->SetSaveAsItem(items.item_b.Get())); break;
        case 9: label = "directory_item"; Call("SetSaveAsItem(folder)", dialog->SetSaveAsItem(items.directory_a.Get())); break;
        case 10: label = "item_then_null"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetSaveAsItem(null)", dialog->SetSaveAsItem(nullptr)); break;
        case 11: label = "folder_B_item_A_null"; Call("SetFolder(B)", dialog->SetFolder(items.directory_b.Get())); Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetSaveAsItem(null)", dialog->SetSaveAsItem(nullptr)); break;
        case 12: label = "default_B_item_A_null"; Call("SetDefaultFolder(B)", dialog->SetDefaultFolder(items.directory_b.Get())); Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetSaveAsItem(null)", dialog->SetSaveAsItem(nullptr)); break;
        case 13: label = "item_A_folder_B_null"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetFolder(B)", dialog->SetFolder(items.directory_b.Get())); Call("SetSaveAsItem(null)", dialog->SetSaveAsItem(nullptr)); break;
        case 14: label = "item_A_name_override_null"; Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get())); Call("SetFileName", dialog->SetFileName(L"override.txt")); Call("SetSaveAsItem(null)", dialog->SetSaveAsItem(nullptr)); break;
        }
        Observe(dialog.Get(), items, label);
    }
    {
        ItemCounts counts;
        ComPtr<IFileSaveDialog> dialog; Check(NativeSave(&dialog) == S_OK, "create native success-failure-replacement fixture");
        ComPtr<IShellItem> failure; failure.Attach(new CountedItem(items.item_b.Get(), counts, E_ACCESSDENIED));
        Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get()));
        const HRESULT failed = dialog->SetSaveAsItem(failure.Get());
        Call("SetSaveAsItem(failing_B)", failed);
        Observe(dialog.Get(), items, "valid_A_then_failed_B");
        Call("SetSaveAsItem(B)", dialog->SetSaveAsItem(items.item_b.Get()));
        Observe(dialog.Get(), items, "failed_B_then_valid_B");
        failure.Reset(); dialog.Reset();
        Check(failed == E_ACCESSDENIED && !counts.alive, "native failed setter HRESULT and retained identity survive until replacement/release");
    }
    for (const HRESULT name_error : {S_OK, E_ACCESSDENIED}) {
        ItemCounts counts;
        ComPtr<IFileSaveDialog> dialog; Check(NativeSave(&dialog) == S_OK, "create counted-item native fixture");
        ComPtr<IShellItem> item; item.Attach(new CountedItem(items.item_a.Get(), counts, name_error));
        const HRESULT setter = dialog->SetSaveAsItem(item.Get());
        std::printf("REFCOUNT name_error=%08lX setter_hr=%08lX references=%lu names=%u parents=%u attributes=%u queries=%u\n", static_cast<unsigned long>(name_error), static_cast<unsigned long>(setter), counts.references.load(), counts.names, counts.parents, counts.attributes, counts.queries);
        Observe(dialog.Get(), items, name_error == S_OK ? "counted_item" : "failing_name_item");
        item.Reset();
        std::printf("REFCOUNT caller_released references=%lu alive=%u\n", counts.references.load(), counts.alive ? 1u : 0u);
        dialog.Reset();
        Check(!counts.alive && !counts.references.load(), "native fixture releases retained caller item before cleanup");
    }
    {
        ComPtr<IFileSaveDialog> dialog; Check(NativeSave(&dialog) == S_OK, "create native explicit property fixture");
        ComPtr<IPropertyStore> store; Check(PSCreateMemoryPropertyStore(IID_PPV_ARGS(&store)) == S_OK, "create owned memory property request");
        PROPVARIANT title{}; InitPropVariantFromString(L"owned-title-seed", &title); store->SetValue(PKEY_Title, title); store->Commit(); PropVariantClear(&title);
        Call("SetProperties(seed)", dialog->SetProperties(store.Get()));
        Call("SetSaveAsItem(A)", dialog->SetSaveAsItem(items.item_a.Get()));
        Observe(dialog.Get(), items, "properties_without_collection");
        ComPtr<IPropertyDescriptionList> list;
        const HRESULT described = PSGetPropertyDescriptionListFromString(L"prop:System.Title;System.Comment", IID_PPV_ARGS(&list));
        Call("PropertyDescriptionList", described);
        if (described == S_OK) Call("SetCollectedProperties", dialog->SetCollectedProperties(list.Get(), FALSE));
        Observe(dialog.Get(), items, "properties_with_collection");
    }
}
struct CancelState { IFileSaveDialog* dialog = nullptr; ULONGLONG begin = 0; bool visible = false, timed_out = false; HRESULT closed = E_PENDING; };
CancelState* active_cancel = nullptr;
void CALLBACK CancelTick(HWND, UINT, UINT_PTR, DWORD) {
    auto* state = active_cancel;
    if (!state || state->closed == S_OK) return;
    ComPtr<IOleWindow> window; HWND hwnd = nullptr; DWORD pid = 0;
    if (state->dialog->QueryInterface(IID_PPV_ARGS(&window)) == S_OK && window->GetWindow(&hwnd) == S_OK &&
        hwnd && IsWindowVisible(hwnd) && GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId()) state->visible = true;
    const auto elapsed = GetTickCount64() - state->begin;
    if ((state->visible && elapsed >= 250) || elapsed >= 10000) {
        state->timed_out = !state->visible;
        state->closed = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
void NativeCancelCases(const Items& items) {
    ComPtr<IFileSaveDialog> dialog;
    Check(NativeSave(&dialog) == S_OK, "create uniquely owned native cancellation dialog");
    GUID guid{}; Check(CoCreateGuid(&guid) == S_OK, "allocate fresh native client GUID");
    wchar_t guid_text[40]{}; StringFromGUID2(guid, guid_text, 40);
    bool journaled = false;
    { std::ofstream journal(fs::absolute(L"bench_data/dialog-contracts-20261110/save-as-item-owned-guids.tsv"), std::ios::app);
        journal << "SaveAsItem\tSave\t"; for (const auto c : std::wstring_view(guid_text)) journal << static_cast<char>(c);
        journal << "\tallocated-before-SetClientGuid\n";
        journal.flush(); journaled = journal.good();
    }
    Check(journaled, "journal owned client GUID before native state can be persisted");
    if (!journaled) return;
    std::printf("CLIENT_GUID pid=%lu value=%ls\n", GetCurrentProcessId(), guid_text);
    Check(dialog->SetClientGuid(guid) == S_OK, "isolate native persistence under fresh owned client GUID");
    Check(dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_OVERWRITEPROMPT) == S_OK &&
        dialog->SetSaveAsItem(items.item_a.Get()) == S_OK, "seed real filesystem identity for repeated native cancellation");
    for (unsigned show = 1; show <= 2; ++show) {
        const auto title = L"Pulse SaveAsItem native cancel " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(show);
        dialog->SetTitle(title.c_str());
        CancelState state{dialog.Get(), GetTickCount64()}; active_cancel = &state;
        const auto timer = SetTimer(nullptr, 0, 25, CancelTick);
        const HRESULT shown = timer ? dialog->Show(nullptr) : E_FAIL;
        if (timer) KillTimer(nullptr, timer);
        active_cancel = nullptr;
        ComPtr<IShellItem> result;
        const HRESULT result_hr = dialog->GetResult(&result);
        std::printf("CANCEL pid=%lu show=%u visible=%u timeout=%u close_hr=%08lX show_hr=%08lX result_hr=%08lX result_present=%u\n",
            GetCurrentProcessId(), show, state.visible ? 1u : 0u, state.timed_out ? 1u : 0u, static_cast<unsigned long>(state.closed),
            static_cast<unsigned long>(shown), static_cast<unsigned long>(result_hr), result ? 1u : 0u);
        Check(state.visible && !state.timed_out && state.closed == S_OK && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(result_hr) && !result,
            "real visible native cancellation returns no accepted result");
        Observe(dialog.Get(), items, show == 1 ? "after_cancel_1" : "after_cancel_2");
    }
    HRESULT cleared = dialog->ClearClientData();
    std::printf("CLEANUP original_hr=%08lX\n", static_cast<unsigned long>(cleared));
    if (FAILED(cleared)) {
        ComPtr<IFileSaveDialog> fresh;
        HRESULT retry = NativeSave(&fresh);
        if (SUCCEEDED(retry)) retry = fresh->SetClientGuid(guid);
        if (SUCCEEDED(retry)) retry = fresh->ClearClientData();
        std::printf("CLEANUP fresh_same_guid_hr=%08lX\n", static_cast<unsigned long>(retry));
        cleared = retry;
    }
    Check(cleared == S_OK, "clear only fresh owned native client GUID state");
}
struct AcceptState { IFileSaveDialog* dialog = nullptr; ULONGLONG begin = 0; bool visible = false, submitted = false, timed_out = false; };
AcceptState* active_accept = nullptr;
void CALLBACK AcceptTick(HWND, UINT, UINT_PTR, DWORD) {
    auto* state = active_accept;
    if (!state) return;
    ComPtr<IOleWindow> window; HWND hwnd = nullptr; DWORD pid = 0;
    if (state->dialog->QueryInterface(IID_PPV_ARGS(&window)) == S_OK && window->GetWindow(&hwnd) == S_OK && hwnd &&
        IsWindowVisible(hwnd) && GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId()) {
        state->visible = true;
        if (!state->submitted && GetTickCount64() - state->begin >= 350) {
            ComPtr<IPropertyStore> store; const HRESULT hr = state->dialog->GetProperties(&store);
            DWORD count = 0; const HRESULT counted = store ? store->GetCount(&count) : E_PENDING;
            std::printf("ACCEPT_METADATA phase=visible properties_hr=%08lX count_hr=%08lX count=%lu\n",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(counted), count);
            PWSTR before = nullptr; const HRESULT before_hr = state->dialog->GetFileName(&before);
            const bool before_expected = before_hr == S_OK && before && std::wstring_view(before) == L"oracle-new.txt";
            CoTaskMemFree(before);
            const HRESULT set = state->dialog->SetFileName(L"oracle-new.txt");
            PWSTR after = nullptr; const HRESULT after_hr = state->dialog->GetFileName(&after);
            const bool after_expected = after_hr == S_OK && after && std::wstring_view(after) == L"oracle-new.txt";
            CoTaskMemFree(after);
            std::printf("ACCEPT_DRIVER before_hr=%08lX before_exact=%u visible_set_hr=%08lX after_hr=%08lX after_exact=%u\n",
                static_cast<unsigned long>(before_hr), before_expected ? 1u : 0u, static_cast<unsigned long>(set), static_cast<unsigned long>(after_hr), after_expected ? 1u : 0u);
            const HWND button = GetDlgItem(hwnd, IDOK);
            if (set == S_OK && after_expected && button && IsWindowEnabled(button)) state->submitted = PostMessageW(hwnd, WM_COMMAND,
                MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(button)) != FALSE;
        }
    }
    if (GetTickCount64() - state->begin >= 10000) { state->timed_out = true; state->dialog->Close(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
}
void NativeAcceptCase(const Items& items, bool invalid_null_child = false) {
    ComPtr<IFileSaveDialog> dialog; Check(NativeSave(&dialog) == S_OK, "create System32 native acceptance oracle");
    GUID guid{}; Check(CoCreateGuid(&guid) == S_OK, "allocate owned acceptance client GUID");
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    { std::ofstream journal(fs::absolute(L"bench_data/dialog-contracts-20261110/save-as-item-owned-guids.tsv"), std::ios::app);
        journal << "SaveAsItem\tSave\t"; for (const auto c : std::wstring_view(text)) journal << static_cast<char>(c);
        journal << "\tallocated-before-SetClientGuid\n"; journal.flush();
        Check(journal.good(), "journal acceptance GUID before native persistence"); if (!journal.good()) return; }
    std::printf("CLIENT_GUID pid=%lu value=%ls\n", GetCurrentProcessId(), text);
    Check(dialog->SetClientGuid(guid) == S_OK && dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT) == S_OK &&
        dialog->SetSaveAsItem(items.item_a.Get()) == S_OK && dialog->SetFileName(L"oracle-new.txt") == S_OK, "seed owned FS source then distinct nonexisting accepted name without metadata setters");
    const auto title = L"Pulse SaveAsItem native acceptance " + std::to_wstring(GetCurrentProcessId()); dialog->SetTitle(title.c_str());
    Observe(dialog.Get(), items, "before_accept");
    AcceptState state{dialog.Get(), GetTickCount64()}; active_accept = &state;
    const auto timer = SetTimer(nullptr, 0, 25, AcceptTick);
    const HRESULT shown = timer ? dialog->Show(nullptr) : E_FAIL;
    if (timer) KillTimer(nullptr, timer); active_accept = nullptr;
    ComPtr<IShellItem> result; const HRESULT result_hr = dialog->GetResult(&result);
    PWSTR result_path = nullptr; const HRESULT path_hr = result ? result->GetDisplayName(SIGDN_FILESYSPATH, &result_path) : E_PENDING;
    const fs::path accepted_path(result_path ? result_path : L"");
    const bool exact = path_hr == S_OK && result_path && accepted_path == items.folder_a / L"oracle-new.txt";
    const auto relative = accepted_path.lexically_relative(items.root);
    const bool owned = !relative.empty() && !relative.is_absolute() && *relative.begin() != L"..";
    if (owned) std::printf("ACCEPT_OWNED_RELATIVE value=%ls\n", relative.c_str());
    else std::printf("ACCEPT_OWNED_RELATIVE outside_owned_root=1\n");
    CoTaskMemFree(result_path);
    std::printf("ACCEPT pid=%lu visible=%u submitted=%u timeout=%u show_hr=%08lX result_hr=%08lX exact_owned_target=%u\n", GetCurrentProcessId(),
        state.visible ? 1u : 0u, state.submitted ? 1u : 0u, state.timed_out ? 1u : 0u, static_cast<unsigned long>(shown), static_cast<unsigned long>(result_hr), exact ? 1u : 0u);
    Check(state.visible && state.submitted && !state.timed_out && shown == S_OK && result_hr == S_OK && exact, "native UI actually accepts exact distinct owned target");
    Observe(dialog.Get(), items, "after_accept");
    ComPtr<IPropertyStore> store; const HRESULT properties = dialog->GetProperties(&store); DWORD count = 0;
    const HRESULT counted = store ? store->GetCount(&count) : E_PENDING;
    std::printf("ACCEPT_METADATA phase=accepted properties_hr=%08lX count_hr=%08lX count=%lu explicit_metadata_setters=0\n", static_cast<unsigned long>(properties), static_cast<unsigned long>(counted), count);
    if (counted == S_OK && count <= 128) for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key{}; PROPVARIANT value{}; const HRESULT key_hr = store->GetAt(i, &key), value_hr = SUCCEEDED(key_hr) ? store->GetValue(key, &value) : key_hr;
        wchar_t id[40]{}; StringFromGUID2(key.fmtid, id, 40);
        std::printf("ACCEPT_PROPERTY index=%lu key=%ls:%lu key_hr=%08lX value_hr=%08lX type=%u\n", i, id, key.pid, static_cast<unsigned long>(key_hr), static_cast<unsigned long>(value_hr), static_cast<unsigned>(value.vt)); PropVariantClear(&value);
    }
    ComPtr<IPropertyStore> empty; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&empty));
    Call("ApplyProperties(owned_B,empty)", dialog->ApplyProperties(items.item_b.Get(), empty.Get(), nullptr, nullptr));
    if (invalid_null_child) {
        std::printf("INVALID_INPUT_CHILD method=ApplyProperties property_store=null native_may_terminate=1\n"); std::fflush(stdout);
        Call("ApplyProperties(owned_B,null)", dialog->ApplyProperties(items.item_b.Get(), nullptr, nullptr, nullptr));
    }
    Check(GetFileAttributesW((items.folder_a / L"oracle-new.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "native Save selection does not itself create target document");
    HRESULT cleared = dialog->ClearClientData(); std::printf("CLEANUP original_hr=%08lX\n", static_cast<unsigned long>(cleared));
    if (FAILED(cleared)) { ComPtr<IFileSaveDialog> fresh; HRESULT retry = NativeSave(&fresh); if (SUCCEEDED(retry)) retry = fresh->SetClientGuid(guid);
        if (SUCCEEDED(retry)) retry = fresh->ClearClientData(); std::printf("CLEANUP fresh_same_guid_hr=%08lX\n", static_cast<unsigned long>(retry)); cleared = retry; }
    Check(cleared == S_OK, "clear exact owned acceptance GUID with fresh native retry when needed");
}
int CleanupOwnedJournal() {
    std::ifstream journal(fs::absolute(L"bench_data/dialog-contracts-20261110/save-as-item-owned-guids.tsv"));
    std::string line; unsigned entries = 0;
    while (std::getline(journal, line)) {
        constexpr std::string_view prefix = "SaveAsItem\tSave\t", suffix = "\tallocated-before-SetClientGuid";
        if (!line.starts_with(prefix) || !line.ends_with(suffix)) { Check(false, "cleanup journal has exact owned allocation marker"); continue; }
        const auto value = line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());
        const std::wstring text(value.begin(), value.end()); GUID guid{};
        HRESULT result = CLSIDFromString(text.c_str(), &guid);
        if (result == S_OK) { ComPtr<IFileSaveDialog> fresh; result = NativeSave(&fresh);
            if (result == S_OK) result = fresh->SetClientGuid(guid); if (result == S_OK) result = fresh->ClearClientData(); }
        std::printf("OWNED_JOURNAL_CLEANUP guid=%ls fresh_hr=%08lX\n", text.c_str(), static_cast<unsigned long>(result));
        Check(result == S_OK, "fresh native object clears only journaled owned GUID"); ++entries;
    }
    Check(entries != 0, "cleanup journal contains an owned preallocated GUID");
    std::printf("SAVE_AS_ITEM_JOURNAL entries=%u failures=%u show_calls=0\n", entries, failures); return failures ? 1 : 0;
}
class FailingPropertyStore final : public IPropertyStore {
    std::atomic<ULONG> references_{1};
    ComPtr<IPropertyStore> source_;
public:
    unsigned successful_reads = 0;
    explicit FailingPropertyStore(IPropertyStore* source) : source_(source) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IPropertyStore) return E_NOINTERFACE;
        *output = static_cast<IPropertyStore*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override { if (!count) return E_POINTER; *count = 2; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        if (index > 1) return E_INVALIDARG;
        *key = index ? PKEY_Comment : PKEY_Title; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (IsEqualPropertyKey(key, PKEY_Comment)) return E_ACCESSDENIED;
        ++successful_reads; return source_->GetValue(key, value);
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
};
bool TitleEquals(IPropertyStore* store, const wchar_t* expected) {
    PROPVARIANT value{};
    const HRESULT hr = store ? store->GetValue(PKEY_Title, &value) : E_POINTER;
    const bool matches = hr == S_OK && value.vt == VT_LPWSTR && value.pwszVal && std::wstring_view(value.pwszVal) == expected;
    PropVariantClear(&value); return matches;
}
void ModelCases(const Items& items) {
    using namespace pulse::shell_bridge;
    ComPtr<IFileSaveDialog> native; Check(NativeSave(&native) == S_OK, "create native mirror for model assertions");
    SaveAsItemModel model;
    SaveAsItemSeed seed; seed.filename = L"unchanged";
    Check(model.BuildFilesystemSeed(false, &seed) == E_PENDING && seed.filename == L"unchanged", "uninitialized model cannot publish an initial or accepted result");
    Check(native->SetSaveAsItem(items.item_a.Get()) == S_OK && model.ObserveSetSaveAsItem(items.item_a.Get(), native.Get(), S_OK) == S_OK,
        "capture real filesystem SaveAsItem after native success");
    SaveAsItemSnapshot snapshot;
    Check(model.Snapshot(&snapshot) == S_OK && snapshot.original_item.Get() == items.item_a.Get() && snapshot.source_path == items.file_a.wstring() &&
        snapshot.source_parent_path == items.folder_a.wstring() && snapshot.initial_directory == items.folder_a.wstring() &&
        snapshot.initial_filename == items.file_a.filename().wstring(), "model owns source Shell identity, parent and native effective initial fields");
    Check(model.BuildFilesystemSeed(false, &seed) == S_OK && seed.original_item.Get() == items.item_a.Get(), "plain filesystem seed retains original identity beyond its filename");
    const auto generation = model.Generation();
    Check(model.BuildFilesystemSeed(true, &seed) == E_NOTIMPL && model.Generation() == generation, "explicit metadata contract never silently becomes a filename-only seed");
    {
        ComPtr<IPropertyStore> source; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&source));
        PROPVARIANT title{}; InitPropVariantFromString(L"combined-owned-title", &title);
        source->SetValue(PKEY_Title, title); PropVariantClear(&title); source->Commit();
        FileDialogProperties metadata;
        Check(metadata.SetStore(source.Get()) == S_OK, "combined seed metadata enters existing owned property model");
        ComPtr<IPropertyStore> metadata_snapshot;
        Check(metadata.Snapshot(&metadata_snapshot) == S_OK && model.BuildFilesystemSeed(true, &seed, metadata_snapshot.Get()) == S_OK,
            "filesystem initial-item seed admits a real owned metadata snapshot");
        metadata_snapshot.Reset();
        InitPropVariantFromString(L"caller-mutated-title", &title); source->SetValue(PKEY_Title, title); PropVariantClear(&title);
        Check(seed.original_item.Get() == items.item_a.Get() && TitleEquals(seed.metadata_snapshot.Get(), L"combined-owned-title") &&
            model.Generation() == generation, "combined seed retains Shell identity and detached store without aliasing caller edits");
        Check(model.BuildFilesystemSeed(false, &seed) == S_OK && !seed.metadata_snapshot,
            "plain seed does not retain metadata from an earlier combination");
    }
    Check(native->SetFileName(L"override.txt") == S_OK && model.RefreshAfterSetter(native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.initial_filename == L"override.txt" && snapshot.original_item.Get() == items.item_a.Get(), "later filename overrides initial text without dropping source identity");
    Check(native->SetFolder(items.directory_b.Get()) == S_OK && model.RefreshAfterSetter(native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.initial_directory == items.folder_b.wstring() && snapshot.source_parent_path == items.folder_a.wstring(), "later explicit folder changes destination, not source identity");
    Check(native->SetDefaultFolder(items.directory_a.Get()) == S_OK && model.RefreshAfterSetter(native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.initial_directory == items.folder_b.wstring(), "later default folder respects native explicit-folder precedence");
    Check(native->SetSaveAsItem(items.item_b.Get()) == S_OK && model.ObserveSetSaveAsItem(items.item_b.Get(), native.Get(), S_OK) == S_OK &&
        model.Snapshot(&snapshot) == S_OK && snapshot.original_item.Get() == items.item_b.Get() && snapshot.initial_filename == items.file_b.filename().wstring(),
        "replacement recaptures identity and full native initial state");
    {
        ItemCounts counts; ComPtr<IShellItem> failing; failing.Attach(new CountedItem(items.item_a.Get(), counts, E_ACCESSDENIED));
        const auto before = model.Generation();
        const HRESULT rejected = native->SetSaveAsItem(failing.Get());
        Check(rejected == E_ACCESSDENIED && model.ObserveSetSaveAsItem(failing.Get(), native.Get(), rejected) == rejected && model.Generation() == before &&
            model.BuildFilesystemSeed(false, &seed) == rejected, "failed native setter invalidates seed instead of reusing now-stale prior fields");
        Check(native->SetSaveAsItem(items.item_a.Get()) == S_OK, "replace retained failing fixture before its lifetime counters expire");
    }
    Check(native->SetSaveAsItem(items.item_a.Get()) == S_OK && model.ObserveSetSaveAsItem(items.item_a.Get(), native.Get(), S_OK) == S_OK,
        "successful full recapture restores model validity without changing wrapper failure gates");
    Check(native->SetSaveAsItem(nullptr) == S_OK && model.ObserveSetSaveAsItem(nullptr, native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.cleared && !snapshot.original_item && !snapshot.filename_set && snapshot.filename_result == E_FAIL && snapshot.initial_directory == items.folder_a.wstring(),
        "null clears source and filename while retaining native default-folder resolution");
    Check(model.BuildFilesystemSeed(false, &seed) == S_OK && seed.cleared && !seed.filename_set && !seed.original_item,
        "clear seed does not expose previous source as an accepted result");
    Check(native->SetFolder(items.directory_b.Get()) == S_OK && model.RefreshAfterSetter(native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.initial_directory == items.folder_b.wstring() && !snapshot.filename_set, "folder setter after clear mirrors native no-name state");
    Check(native->SetSaveAsItem(nullptr) == S_OK && model.ObserveSetSaveAsItem(nullptr, native.Get(), S_OK) == S_OK && model.Snapshot(&snapshot) == S_OK &&
        snapshot.initial_directory == items.folder_a.wstring(), "later clear removes explicit folder and resolves retained default, not last-wins fiction");
    Check(native->SetSaveAsItem(items.directory_a.Get()) == S_OK && model.ObserveSetSaveAsItem(items.directory_a.Get(), native.Get(), S_OK) == E_NOTIMPL &&
        model.BuildFilesystemSeed(false, &seed) == E_NOTIMPL, "native-accepted folder item keeps native route outside regular-file subset");
    {
        ItemCounts counts; ComPtr<IShellItem> item; auto* counted = new CountedItem(items.item_a.Get(), counts); item.Attach(counted);
        ComPtr<IFileSaveDialog> mirror; NativeSave(&mirror); SaveAsItemModel owned;
        Check(mirror->SetSaveAsItem(item.Get()) == S_OK && owned.ObserveSetSaveAsItem(item.Get(), mirror.Get(), S_OK) == S_OK, "capture counted caller identity independently of native mirror ownership");
        item.Reset(); mirror.Reset();
        Check(counts.alive && counts.references.load() == 1, "model retains exactly its own original Shell reference after caller and native release");
        Check(owned.Reset() == S_OK && !counts.alive && !counts.references.load(), "reset releases model-owned original identity");
    }
    {
        ItemCounts counts; ComPtr<IShellItem> item; auto* counted = new CountedItem(items.item_a.Get(), counts); item.Attach(counted);
        ComPtr<IFileSaveDialog> mirror; NativeSave(&mirror); SaveAsItemModel atomic;
        mirror->SetSaveAsItem(item.Get()); counted->FailNames(E_ACCESSDENIED);
        Check(atomic.ObserveSetSaveAsItem(item.Get(), mirror.Get(), S_OK) == E_ACCESSDENIED && atomic.Generation() == 0 &&
            atomic.BuildFilesystemSeed(false, &seed) == E_ACCESSDENIED, "capture failure after successful native mutation publishes no partial state");
    }
    {
        ItemCounts counts; ComPtr<IShellItem> item; auto* counted = new CountedItem(items.item_a.Get(), counts); item.Attach(counted);
        ComPtr<IFileSaveDialog> mirror; NativeSave(&mirror); SaveAsItemModel reentrant;
        mirror->SetSaveAsItem(item.Get());
        HRESULT nested = S_OK;
        counted->on_name = [&] { nested = reentrant.ObserveSetSaveAsItem(items.item_b.Get(), mirror.Get(), S_OK); };
        const HRESULT captured = reentrant.ObserveSetSaveAsItem(item.Get(), mirror.Get(), S_OK);
        counted->on_name = {};
        Check(nested == HRESULT_FROM_WIN32(ERROR_BUSY) && captured == HRESULT_FROM_WIN32(ERROR_RETRY) && reentrant.Generation() == 0,
            "caller item reentry cancels outer capture without publishing a stale replacement");
    }
    {
        HRESULT result = S_OK;
        std::thread other([&] { result = model.RefreshAfterSetter(nullptr, S_OK); }); other.join();
        Check(result == RPC_E_WRONG_THREAD, "capture remains on the creating caller apartment thread");
    }
    for (const bool reset : {false, true}) {
        ItemCounts counts; ComPtr<IShellItem> item; auto* counted = new CountedItem(items.item_a.Get(), counts); item.Attach(counted);
        ComPtr<IFileSaveDialog> first, second; NativeSave(&first); NativeSave(&second); SaveAsItemModel reentrant;
        first->SetSaveAsItem(item.Get()); second->SetSaveAsItem(items.item_b.Get());
        reentrant.ObserveSetSaveAsItem(item.Get(), first.Get(), S_OK); item.Reset(); first.Reset();
        const auto before = reentrant.Generation(); HRESULT nested = S_OK;
        counted->on_release = [&] { nested = reentrant.ObserveSetSaveAsItem(items.item_b.Get(), second.Get(), S_OK); };
        const HRESULT outer = reset ? reentrant.Reset() : reentrant.ObserveSetSaveAsItem(items.item_b.Get(), second.Get(), S_OK);
        Check(nested == HRESULT_FROM_WIN32(ERROR_BUSY) && outer == HRESULT_FROM_WIN32(ERROR_RETRY) && !counts.alive &&
            (reset || reentrant.Generation() == before) && reentrant.BuildFilesystemSeed(false, &seed) == HRESULT_FROM_WIN32(ERROR_RETRY),
            reset ? "caller Release reentry during reset cannot publish an unexpected new seed" : "caller Release reentry during replacement invalidates capture without advancing generation");
    }
    {
        ComPtr<IPropertyStore> store; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&store));
        PROPVARIANT value{}; InitPropVariantFromString(L"owned-title-seed", &value); store->SetValue(PKEY_Title, value); PropVariantClear(&value); store->Commit();
        FileDialogProperties requests;
        Check(requests.SetStore(store.Get()) == S_OK, "existing bounded property model copies explicit metadata request");
        InitPropVariantFromString(L"changed-source", &value); store->SetValue(PKEY_Title, value); PropVariantClear(&value); store->Commit();
        ComPtr<IPropertyStore> copy; requests.Snapshot(&copy);
        Check(TitleEquals(copy.Get(), L"owned-title-seed"), "metadata request owns a deep copy rather than caller live store");
        ComPtr<IPropertyStore> failing; auto* failure = new FailingPropertyStore(store.Get()); failing.Attach(failure);
        const HRESULT captured = requests.SetStore(failing.Get());
        copy.Reset(); const HRESULT read = requests.Snapshot(&copy);
        Check(captured == E_ACCESSDENIED && failure->successful_reads == 1 && read == S_OK && TitleEquals(copy.Get(), L"owned-title-seed"),
            "property failure after partial copy is atomic and preserves previous explicit request");
        Check(requests.SetStore(nullptr) == S_OK && requests.Snapshot(&copy) == S_OK, "explicit metadata clear remains an owned empty request");
        DWORD count = 1; Check(copy->GetCount(&count) == S_OK && count == 0, "cleared metadata never leaks earlier title");
    }
    ComPtr<IShellItem> result;
    Check(native->GetResult(&result) == E_UNEXPECTED && !result, "capturing an initial source never synthesizes native GetResult before acceptance");
    ComPtr<IPropertyStore> empty; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&empty));
    Check(native->ApplyProperties(items.item_b.Get(), empty.Get(), nullptr, nullptr) == S_OK,
        "native empty-property apply helper also works without native Show acceptance");
}
void NameOnlyBaseline(const Items& items) {
    ComPtr<IFileSaveDialog> correct, projected; NativeSave(&correct); NativeSave(&projected);
    correct->SetFolder(items.directory_b.Get()); projected->SetFolder(items.directory_b.Get());
    correct->SetSaveAsItem(items.item_a.Get()); projected->SetFileName(items.file_a.filename().c_str());
    ComPtr<IShellItem> expected_folder, projected_folder;
    correct->GetFolder(&expected_folder); projected->GetFolder(&projected_folder);
    const int expected = PathKind(expected_folder.Get(), items), actual = PathKind(projected_folder.Get(), items);
    std::printf("NAME_ONLY_BASELINE native_folder=%d projected_folder=%d equivalent=%u hypothetical_projection_not_Pulse=1\n", expected, actual, expected == actual ? 1u : 0u);
    Check(expected == actual, "hypothetical filename-only projection must preserve native SaveAsItem folder semantics");
}
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::printf("COM_INITIALIZE hr=%08lX\n", static_cast<unsigned long>(initialized)); return 2; }
    if (argc == 2 && std::string_view(argv[1]) == "--cleanup-owned-journal") { const int result = CleanupOwnedJournal(); CoUninitialize(); return result; }
    Items items;
    items.root = fs::absolute(L"bench_data/dialog-contracts-20261110") / (L"saveas-native-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(items.root.c_str(), nullptr)) { CoUninitialize(); return 2; }
    items.folder_a = items.root / L"a"; items.folder_b = items.root / L"b";
    items.file_a = items.folder_a / L"source-a.txt"; items.file_b = items.folder_b / L"source-b.txt";
    Check(CreateDirectoryW(items.folder_a.c_str(), nullptr) && CreateDirectoryW(items.folder_b.c_str(), nullptr), "create isolated owned folders");
    { std::ofstream(items.file_a) << "owned-A"; std::ofstream(items.file_b) << "owned-B"; }
    Check(SHCreateItemFromParsingName(items.file_a.c_str(), nullptr, IID_PPV_ARGS(&items.item_a)) == S_OK &&
        SHCreateItemFromParsingName(items.file_b.c_str(), nullptr, IID_PPV_ARGS(&items.item_b)) == S_OK &&
        SHCreateItemFromParsingName(items.folder_a.c_str(), nullptr, IID_PPV_ARGS(&items.directory_a)) == S_OK &&
        SHCreateItemFromParsingName(items.folder_b.c_str(), nullptr, IID_PPV_ARGS(&items.directory_b)) == S_OK, "create real owned filesystem Shell identities");
    if (!failures) {
        if (argc == 2 && std::string_view(argv[1]) == "--model-only") ModelCases(items);
        else if (argc == 2 && std::string_view(argv[1]) == "--native-cancel") NativeCancelCases(items);
        else if (argc == 2 && std::string_view(argv[1]) == "--native-accept") NativeAcceptCase(items);
        else if (argc == 2 && std::string_view(argv[1]) == "--native-accept-invalid-null-child") NativeAcceptCase(items, true);
        else if (argc == 2 && std::string_view(argv[1]) == "--name-only-baseline") NameOnlyBaseline(items);
        else { NativeCases(items); ModelCases(items); }
    }
    { std::ifstream a(items.file_a), b(items.file_b); std::string first, second;
        std::getline(a, first); std::getline(b, second);
        Check(first == "owned-A" && second == "owned-B", "native initial-item and model probes leave both owned file payloads unchanged"); }
    items.item_a.Reset(); items.item_b.Reset(); items.directory_a.Reset(); items.directory_b.Reset();
    Check(DeleteFileW(items.file_a.c_str()) && DeleteFileW(items.file_b.c_str()) && RemoveDirectoryW(items.folder_a.c_str()) &&
        RemoveDirectoryW(items.folder_b.c_str()) && RemoveDirectoryW(items.root.c_str()), "clean only isolated owned filesystem fixtures");
    CoUninitialize();
    const unsigned shows = argc == 2 && std::string_view(argv[1]) == "--native-cancel" ? 2 : argc == 2 && std::string_view(argv[1]).starts_with("--native-accept") ? 1 : 0;
    std::printf("SAVE_AS_ITEM_NATIVE checks=%u failures=%u show_calls=%u provider_takeover_verified=0\n", checks, failures, shows);
    return failures ? 1 : 0;
}
