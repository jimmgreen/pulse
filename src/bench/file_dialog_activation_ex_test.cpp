#include "../shell_bridge/file_dialog_activation_ex.h"
#include "../shell_bridge/file_dialog_activation.h"

#include <iostream>
#include <string>
#include <algorithm>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name, HRESULT hr = S_OK) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    std::cout << '\n';
}
const IID unknown_iid = {0x45aedc31, 0x3584, 0x4af0, {0x8b, 0x42, 0xd2, 0x71, 0x87, 0x53, 0x2c, 0x90}};
struct Results {
    std::vector<MULTI_QI> values;
    Results(std::initializer_list<const IID*> iids) { for (auto* iid : iids) values.push_back({iid, nullptr, S_OK}); }
    ~Results() { for (auto& value : values) if (value.pItf) value.pItf->Release(); }
    Results(const Results&) = delete;
    Results& operator=(const Results&) = delete;
    DWORD Count() const { return static_cast<DWORD>(values.size()); }
};
ComPtr<IUnknown> Identity(IUnknown* object) {
    ComPtr<IUnknown> result;
    if (object) object->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
struct Probe {
    int ex_calls = 0, single_calls = 0, creations = 0;
    CLSID clsid{}; IUnknown* outer = nullptr; DWORD context = 0, count = 0;
    COSERVERINFO* server = nullptr; MULTI_QI* results = nullptr;
    HRESULT result = E_UNEXPECTED;
    bool fail = false, ex_to_single = false, single_to_ex = false, mixed_objects = false;
    HRESULT override_result = S_OK, override_item = S_OK;
    HRESULT incoming_hr = S_OK;
    FileDialogActivation* single_config = nullptr;
    FileDialogActivationEx* ex_config = nullptr;
    ComPtr<IUnknown> native_identity;
    std::vector<MULTI_QI> original_results; // Non-owning snapshots.
};
thread_local Probe* active = nullptr;
HRESULT WINAPI OriginalSingle(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
HRESULT WINAPI OriginalEx(REFCLSID clsid, IUnknown* outer, DWORD context, COSERVERINFO* server, DWORD count, MULTI_QI* results) {
    auto& probe = *active;
    if (results && count) probe.incoming_hr = results[0].hr;
    ++probe.ex_calls; probe.clsid = clsid; probe.outer = outer; probe.context = context;
    probe.server = server; probe.count = count; probe.results = results;
    HRESULT hr;
    if (probe.fail || !count || !results) {
        hr = probe.fail ? E_ACCESSDENIED : E_INVALIDARG;
        if (results) for (DWORD i = 0; i < count; ++i) { results[i].hr = hr; results[i].pItf = nullptr; }
    } else if (probe.ex_to_single) {
        ComPtr<IUnknown> object;
        hr = ActivateFileDialog(*probe.single_config, clsid, outer, context, IID_PPV_ARGS(&object));
        if (SUCCEEDED(hr)) {
            for (DWORD i = 0; i < count; ++i) results[i].hr = object->QueryInterface(*results[i].pIID, reinterpret_cast<void**>(&results[i].pItf));
        }
    } else {
        ++probe.creations;
        hr = ::CoCreateInstanceEx(clsid, outer, context, server, count, results);
    }
    if (SUCCEEDED(hr) && results && count) {
        for (DWORD i = 0; i < count; ++i) if (results[i].pItf) { probe.native_identity = Identity(results[i].pItf); break; }
        if (probe.mixed_objects && count > 1 && results[1].pItf) {
            ComPtr<IUnknown> different;
            ::CoCreateInstance(clsid, nullptr, context, *results[1].pIID, reinterpret_cast<void**>(different.GetAddressOf()));
            results[1].pItf->Release(); results[1].pItf = different.Detach();
        }
        if (hr == S_OK) {
            hr = probe.override_result;
            results[0].hr = probe.override_item;
        }
    }
    probe.original_results.clear();
    if (results) for (DWORD i = 0; i < count; ++i) probe.original_results.push_back(results[i]);
    return probe.result = hr;
}
HRESULT WINAPI OriginalSingle(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) {
    auto& probe = *active; ++probe.single_calls;
    if (probe.single_to_ex) {
        MULTI_QI result{&iid, nullptr, S_OK};
        const auto hr = ActivateFileDialogEx(*probe.ex_config, clsid, outer, context, nullptr, 1, &result);
        *output = result.pItf; return hr;
    }
    ++probe.creations;
    const auto hr = ::CoCreateInstance(clsid, outer, context, iid, output);
    if (SUCCEEDED(hr)) probe.native_identity = Identity(static_cast<IUnknown*>(*output));
    return hr;
}
FileDialogActivationEx Configuration() {
    FileDialogActivationEx config; config.enabled = true; config.original = OriginalEx; return config;
}
bool Unchanged(const Results& results, const Probe& probe) {
    if (results.values.size() != probe.original_results.size()) return false;
    for (size_t i = 0; i < results.values.size(); ++i) {
        const auto& a = results.values[i]; const auto& b = probe.original_results[i];
        if (a.pIID != b.pIID || a.pItf != b.pItf || a.hr != b.hr) return false;
    }
    return true;
}
bool Unified(const Results& results) {
    ComPtr<IUnknown> identity;
    for (const auto& result : results.values) {
        if (result.hr != S_OK || !result.pItf) return false;
        auto current = Identity(result.pItf);
        if (!current) return false;
        if (!identity) identity = current;
        else if (identity.Get() != current.Get()) return false;
    }
    return true;
}
struct ThrowCopy {
    std::shared_ptr<bool> fail;
    explicit ThrowCopy(std::shared_ptr<bool> value) : fail(std::move(value)) {}
    ThrowCopy(const ThrowCopy& other) : fail(other.fail) { if (*fail) throw std::bad_alloc(); }
    HRESULT operator()(IFileDialog*, HWND) const { return E_ABORT; }
};
void Success(DialogFactory factory) {
    Probe probe; active = &probe; auto config = Configuration();
    int shown = 0; bool same_native = false;
    auto token = std::make_shared<int>(1); std::weak_ptr<int> lifetime = token;
    config.services.show_native = [&, token](IFileDialog* object, HWND) {
        ++shown; same_native = Identity(object).Get() == probe.native_identity.Get(); return E_ABORT;
    };
    const auto& factory_iid = factory == DialogFactory::Open ? IID_IFileOpenDialog : IID_IFileSaveDialog;
    const auto& clsid = factory == DialogFactory::Open ? CLSID_FileOpenDialog : CLSID_FileSaveDialog;
    Results results{&IID_IUnknown, &factory_iid, &IID_IFileDialog, &IID_IModalWindow, &IID_IFileDialogCustomize, &IID_IFileDialog2, &IID_IUnknown};
    const auto hr = ActivateFileDialogEx(config, clsid, nullptr, CLSCTX_INPROC_SERVER, nullptr, results.Count(), results.values.data());
    Check(hr == S_OK && probe.ex_calls == 1 && probe.creations == 1, "multi-interface creation calls original exactly once", hr);
    Check(probe.clsid == clsid && !probe.outer && !probe.server && probe.context == CLSCTX_INPROC_SERVER &&
        probe.count == results.Count() && probe.results == results.values.data(), "original receives exact Ex arguments");
    Check(Unified(results) && Identity(results.values[0].pItf).Get() != probe.native_identity.Get(), "all interfaces and duplicate IIDs share one wrapper identity");
    bool metadata_preserved = true;
    for (size_t i = 0; i < results.values.size(); ++i) metadata_preserved = metadata_preserved &&
        results.values[i].pIID == probe.original_results[i].pIID && results.values[i].hr == probe.original_results[i].hr;
    Check(metadata_preserved, "successful wrapping preserves original IID pointers and per-item HRESULTs");
    ComPtr<IFileDialog> dialog; if (results.values[0].pItf) results.values[0].pItf->QueryInterface(IID_PPV_ARGS(&dialog));
    Check(dialog && !Unchanged(results, probe) && dialog->Show(nullptr) == E_ABORT && shown == 1 && same_native,
        "default gate uses same single original native object for Show");
    config.services = {}; token.reset();
    for (auto& value : results.values) { if (value.pItf) value.pItf->Release(); value.pItf = nullptr; }
    FILEOPENDIALOGOPTIONS options = 0;
    Check(dialog && !lifetime.expired() && SUCCEEDED(dialog->GetOptions(&options)), "remaining wrapper reference survives releasing all MULTI_QI outputs");
    dialog.Reset();
    Check(lifetime.expired(), "last output reference releases single wrapper and temporary staged references");
}
void SeededResultStatus() {
    for (const HRESULT seed : {E_NOINTERFACE, E_FAIL, S_FALSE}) {
        for (const bool save : {false, true}) {
            Probe probe; active = &probe; auto config = Configuration();
            const auto& iid = save ? IID_IFileSaveDialog : IID_IFileOpenDialog;
            Results results{&iid}; results.values[0].hr = seed;
            const HRESULT hr = ActivateFileDialogEx(config, save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, results.values.data());
            Check(probe.incoming_hr == seed && probe.ex_calls == 1, "seeded status reaches original unchanged exactly once");
            Check(hr == S_OK && results.values[0].hr == S_OK && results.values[0].pItf,
                "real native COM accepts and replaces seeded result HRESULT", hr);
            Check(results.values[0].pItf && Identity(results.values[0].pItf).Get() != probe.native_identity.Get(),
                "native successful object is wrapped despite prior result HRESULT");
        }
    }
}

void Diagnostics() {
    for (int mode = 0; mode < 7; ++mode) {
        Probe probe; active = &probe; auto config = Configuration();
        std::vector<std::string> observed;
        IID rejected{};
        config.services.on_native_reason = [&](const char* reason, const IID* iid) {
            observed.emplace_back(reason);
            if (iid) rejected = *iid;
            if (mode == 6) throw std::bad_alloc();
        };
        const IID* requested = mode == 1 ? &unknown_iid : &IID_IFileOpenDialog;
        Results results{requested};
        const DWORD context = mode == 2 ? CLSCTX_INPROC_SERVER | CLSCTX_DISABLE_AAA : CLSCTX_INPROC_SERVER;
        if (mode == 3) config.enabled = false;
        if (mode == 4) probe.override_result = S_FALSE;
        if (mode == 5) probe.override_item = S_FALSE;
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, context, nullptr, 1, results.values.data());
        Check(hr == probe.result && probe.ex_calls == 1, "diagnostic preserves original call count and HRESULT", hr);
        const char* expected[] = {"ex-wrapper-published", "ex-native-requested-interface", "ex-native-context",
            "ex-native-disabled", "ex-native-aggregate-result", "ex-native-output-state", "ex-wrapper-published"};
        Check(std::find(observed.begin(), observed.end(), expected[mode]) != observed.end(), "activation reports exact wrapping or bypass stage");
        if (mode == 1) Check(rejected == unknown_iid, "unsupported interface diagnostic retains requested IID");
        if (mode == 0 || mode == 6) {
            Check(results.values[0].pItf && Identity(results.values[0].pItf).Get() != probe.native_identity.Get(),
                "successful and throwing observers both preserve wrapped identity");
        } else Check(Unchanged(results, probe), "bypass diagnostics preserve every original output field");
    }
}

}
int main() {
    std::cout << std::unitbuf;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    SeededResultStatus();
    Diagnostics();
    Success(DialogFactory::Open); Success(DialogFactory::Save);
    {
        Probe probe; active = &probe; auto config = Configuration();
        Results partial{&IID_IFileOpenDialog, &unknown_iid};
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, partial.Count(), partial.values.data());
        Check(hr == CO_S_NOTALLINTERFACES && partial.values[0].hr == S_OK && partial.values[1].hr == E_NOINTERFACE &&
            Unchanged(partial, probe) && probe.creations == 1, "native partial success preserves every pointer HRESULT and aggregate status", hr);
        Results none{&unknown_iid, &IID_IClassFactory};
        const auto failed = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, none.Count(), none.values.data());
        Check(failed == E_NOINTERFACE && Unchanged(none, probe) && !none.values[0].pItf && !none.values[1].pItf,
            "all-interface native failure remains unchanged", failed);
    }
    {
        Probe probe; active = &probe; auto config = Configuration(); config.enabled = false;
        Results results{&IID_IUnknown, &IID_IFileOpenDialog};
        Check(ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, results.Count(), results.values.data()) == S_OK &&
            Unchanged(results, probe), "disabled Ex adapter returns native multi-interface object unchanged");
    }
    for (int mode = 0; mode < 5; ++mode) {
        Probe probe; active = &probe; auto config = Configuration();
        Results results{&IID_IUnknown, &IID_IFileOpenDialog};
        COSERVERINFO server{}; server.pwszName = const_cast<LPWSTR>(L"not-contacted-fixture");
        auto fail = std::make_shared<bool>(false);
        if (mode == 0) { config.services.show_native = ThrowCopy(fail); *fail = true; }
        if (mode == 1) probe.override_result = S_FALSE;
        if (mode == 2) probe.override_item = S_FALSE;
        if (mode == 3) probe.mixed_objects = true;
        if (mode == 4) probe.fail = true;
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            mode == 4 ? &server : nullptr, results.Count(), results.values.data());
        const char* labels[] = {"wrapper allocation failure rolls back every original interface atomically",
            "noncanonical aggregate success remains unchanged", "noncanonical item HRESULT remains unchanged",
            "different native object identities reject wrapping without partial commit", "server info reaches controlled original unchanged without remote activation"};
        Check(hr == probe.result && Unchanged(results, probe) && probe.ex_calls == 1 &&
            (mode != 4 || (probe.server == &server && probe.creations == 0 && hr == E_ACCESSDENIED)), labels[mode], hr);
    }
    {
        Probe probe; active = &probe; auto config = Configuration();
        Results results{&IID_IUnknown};
        const DWORD flags = CLSCTX_INPROC_SERVER | CLSCTX_DISABLE_AAA;
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, flags, nullptr, results.Count(), results.values.data());
        Check(hr == probe.result && probe.context == flags && Unchanged(results, probe), "extra context flags are forwarded without dropping or wrapping", hr);
        Results other{&IID_IUnknown};
        Check(ActivateFileDialogEx(config, CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, nullptr, other.Count(), other.values.data()) == S_OK &&
            Unchanged(other, probe), "unknown class is unchanged native activation");
        Results unknown{&IID_IUnknown, &IID_IOleWindow};
        const auto unknown_hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, unknown.Count(), unknown.values.data());
        Check(unknown_hr == probe.result && Unchanged(unknown, probe), "unmodeled interface combination never partially wraps", unknown_hr);
    }
    {
        Probe probe; active = &probe; auto config = Configuration();
        ComPtr<IUnknown> outer; ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&outer));
        Results results{&IID_IUnknown};
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, outer.Get(), CLSCTX_INPROC_SERVER, nullptr, results.Count(), results.values.data());
        Check(hr == probe.result && probe.outer == outer.Get() && Unchanged(results, probe), "aggregation retains native HRESULT and all fields", hr);
        Check(ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, 0, nullptr) == E_INVALIDARG &&
            probe.count == 0 && !probe.results, "invalid empty array forwarded once to original");
        config.original = nullptr;
        Results untouched{&IID_IUnknown};
        Check(ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, untouched.values.data()) == E_INVALIDARG &&
            !untouched.values[0].pItf && untouched.values[0].hr == S_OK, "missing original rejects without altering MULTI_QI input");
    }
    {
        Probe probe; active = &probe; auto config = Configuration();
        Results results{};
        for (int i = 0; i < 65; ++i) results.values.push_back({&IID_IUnknown, nullptr, S_OK});
        const auto hr = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, results.Count(), results.values.data());
        Check(hr == S_OK && probe.count == 65 && probe.ex_calls == 1 && Unchanged(results, probe),
            "oversized interface combination bypasses without truncating original request", hr);
    }
    for (int direction = 0; direction < 2; ++direction) {
        Probe probe; active = &probe;
        auto ex = Configuration(); FileDialogActivation single; single.enabled = true; single.original = OriginalSingle;
        probe.ex_config = &ex; probe.single_config = &single;
        int shows = 0; bool raw_native = false;
        auto show = [&](IFileDialog* object, HWND) { ++shows; raw_native = Identity(object).Get() == probe.native_identity.Get(); return E_ABORT; };
        ex.services.show_native = show; single.services.show_native = show;
        ComPtr<IFileDialog> dialog; HRESULT hr;
        if (direction == 0) {
            probe.ex_to_single = true;
            Results results{&IID_IUnknown, &IID_IFileOpenDialog};
            hr = ActivateFileDialogEx(ex, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, results.Count(), results.values.data());
            if (results.values[0].pItf) results.values[0].pItf->QueryInterface(IID_PPV_ARGS(&dialog));
            Check(Unified(results), "Ex to single nested activation preserves output identity");
        } else {
            probe.single_to_ex = true;
            hr = ActivateFileDialog(single, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        }
        Check(SUCCEEDED(hr) && probe.ex_calls == 1 && probe.single_calls == 1 && probe.creations == 1,
            direction == 0 ? "Ex original delegates to single entry with one native creation" : "single original delegates to Ex entry with one native creation", hr);
        Check(dialog && dialog->Show(nullptr) == E_ABORT && shows == 1 && raw_native,
            "shared TLS prevents cross-entry double wrapping in both directions");
    }
    {
        std::thread worker([] {
            const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Check(SUCCEEDED(hr), "initialize MTA Ex bypass fixture", hr);
            if (SUCCEEDED(hr)) {
                { Probe probe; active = &probe; auto config = Configuration(); Results results{&IID_IUnknown};
                  const auto created = ActivateFileDialogEx(config, CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, results.values.data());
                  Check(created == probe.result && Unchanged(results, probe), "MTA activation retains original multi-interface result", created); }
                CoUninitialize();
            }
        }); worker.join();
    }
    CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}
