#pragma once

// A caller-owned append template, not an Adobe dialog or Photoshop integration.
#include <array>
#include <iterator>
#include <strsafe.h>

namespace pulse::bench::save_web {
using namespace pulse::shell_bridge;
struct State {
    std::wstring expected;
    bool cancel = false, inputs_match = true, veto_survived = false;
    unsigned init = 0, accepted_notifications = 0, poll = 0, destroyed = 0;
    std::array<unsigned, 2> changes{};
};

inline void Submit(HWND child, int cmd) {
    auto parent = GetParent(child);
    if (auto button = GetDlgItem(parent, cmd)) PostMessageW(button, BM_CLICK, 0, 0);
    else PostMessageW(parent, WM_COMMAND, cmd, 0);
}

inline UINT_PTR CALLBACK SaveHook(HWND child, UINT msg, WPARAM wp, LPARAM lp) {
    constexpr UINT_PTR drive = 91, after_veto = 92;
    if (msg == WM_INITDIALOG) {
        auto& s = *reinterpret_cast<State*>(reinterpret_cast<OPENFILENAMEW*>(lp)->lCustData);
        SetWindowLongPtrW(child, DWLP_USER, reinterpret_cast<LONG_PTR>(&s));
        ++s.init;
        for (int i = 0; i < 2; ++i) {
            auto combo = GetDlgItem(child, 1101 + i);
            auto add = [&](const wchar_t* item) {
                return SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            };
            s.inputs_match &= combo && add(i ? L"All slices" : L"Default") == 0 &&
                add(i ? L"Selected slices" : L"Custom") == 1 &&
                SendMessageW(combo, CB_SETCURSEL, 0, 0) == 0;
        }
        return 0;
    }
    auto state = reinterpret_cast<State*>(GetWindowLongPtrW(child, DWLP_USER));
    if (!state) return 0;
    auto& s = *state;
    if (msg == WM_COMMAND && HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) >= 1101 && LOWORD(wp) <= 1102)
        ++s.changes[LOWORD(wp) - 1101];
    if (msg == WM_DESTROY) ++s.destroyed;
    if (msg == WM_TIMER && wp == drive) {
        wchar_t value[2048]{};
        auto parent = GetParent(child);
        SendMessageW(parent, CDM_GETFILEPATH, std::size(value), reinterpret_cast<LPARAM>(value));
        if (s.expected != value || !IsWindowVisible(parent)) {
            if (++s.poll < 40) return 0;
            s.inputs_match = false;
            KillTimer(child, drive);
            Submit(child, IDCANCEL);
            return 0;
        }
        KillTimer(child, drive);
        for (int id : {1101, 1102}) {
            auto combo = GetDlgItem(child, id);
            s.inputs_match &= !!IsWindowVisible(combo) && !!IsWindowEnabled(combo);
            SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 1);
            SendMessageW(combo, WM_KEYUP, VK_DOWN, 1);
        }
        Submit(child, IDOK);
    }
    if (msg == WM_TIMER && wp == after_veto) {
        KillTimer(child, after_veto);
        s.veto_survived = !!IsWindowVisible(GetParent(child));
        Submit(child, s.cancel ? IDCANCEL : IDOK);
    }
    if (msg == WM_NOTIFY) {
        auto hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->code == CDN_INITDONE) SetTimer(child, drive, 100, nullptr);
        if (hdr->code == CDN_FILEOK) {
            ++s.accepted_notifications;
            auto ofn = reinterpret_cast<OFNOTIFYW*>(lp)->lpOFN;
            s.inputs_match &= ofn && ofn->lpstrFile && s.expected == ofn->lpstrFile;
            for (int i = 0; i < 2; ++i) {
                wchar_t selected[32]{};
                GetWindowTextW(GetDlgItem(child, 1101 + i), selected, static_cast<int>(std::size(selected)));
                s.inputs_match &= wcscmp(selected, i ? L"Selected slices" : L"Custom") == 0;
            }
            if (s.accepted_notifications == 1) {
                SetTimer(child, after_veto, 150, nullptr);
                SetWindowLongPtrW(child, DWLP_MSGRESULT, 1);
                return 1;
            }
        }
    }
    return 0;
}

inline int Run(int argc, wchar_t** argv) {
    const bool interactive = argc > 0 && std::wstring_view(argv[argc-1]) == L"--interactive";
    if (interactive) --argc;
    const bool native = argc == 1 && std::wstring_view(argv[0]) == L"--native";
    if (!native && !(argc == 2 && std::wstring_view(argv[0]) == L"--ui")) { printf("invalid arguments argc=%d\n", argc); return 2; }
    auto desktop_name = L"PulseSaveTemplate-" + std::to_wstring(GetCurrentProcessId());
    if (!interactive) {
        auto desk = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desk || !SetThreadDesktop(desk)) { printf("isolated desktop error=%lu\n", GetLastError()); return 2; }
    }
    printf("desktop=%s\n", interactive ? "interactive" : "isolated");
    wchar_t base[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, base)) { printf("temp path error=%lu\n", GetLastError()); return 2; }
    const std::wstring directory = std::wstring(base) + desktop_name;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { printf("create fixture error=%lu\n", GetLastError()); return 2; }
    std::shared_ptr<const FileDialogInprocessProvider> provider;
    if (!native && FAILED(LoadFileDialogUiProvider(argv[1], provider, true))) {
        RemoveDirectoryW(directory.c_str()); printf("provider load failed\n"); return 2;
    }
    unsigned failures = 0;
    for (bool cancel : {false, true}) {
        const wchar_t* leaf = cancel ? L"cancel.html" : L"save.html";
        State s;
        s.expected = directory + L"\\" + leaf;
        s.cancel = cancel;
        unsigned seen = 0;
        LegacyDialogFactory factory = [&](bool save, IFileDialog** out) -> HRESULT {
            if (!save || !provider) return E_UNEXPECTED;
            Microsoft::WRL::ComPtr<IUnknown> system;
            HRESULT hr = CreateSystemLegacyDialog(true, &system);
            if (FAILED(hr)) return hr;
            FileDialogBridgeServices services;
            services.inprocess_provider = provider;
            services.on_native_reason = [](const char* reason, const IID*) {
                printf("wrapper=%s\n", reason);
            };
            services.save_access_policy = AccessPolicy::LocalSaveV1;
            services.validation_verified = [&](const ContractDecision& d) {
                if (!RuntimeLocalSaveContractCertified(d) || !d.request) return false;
                const auto* r = std::get_if<ipc::picker::Request>(&d.request->body);
                return r && r->operation == ipc::picker::Operation::SaveFile &&
                    r->initial_directory == std::u16string(directory.begin(), directory.end()) &&
                    r->filename == std::u16string(leaf, leaf + wcslen(leaf));
            };
            services.observe_show = [&](bool begin, const FileDialogShowObservation& o) {
                if (begin) return;
                printf("show fallback=%d started=%d shown=%d hr=%08lX reason=%u stage=%u\n",
                    o.native_fallback, o.provider_started, o.ui_shown.value_or(false),
                    static_cast<unsigned long>(o.result), static_cast<unsigned>(o.contract_reason),
                    static_cast<unsigned>(o.fallback));
                seen += !o.native_fallback && o.provider_started && o.ui_shown.value_or(false);
            };
            return WrapNativeFileDialog(DialogFactory::Save, system.Get(), services, IID_IFileDialog,
                reinterpret_cast<void**>(out));
        };
        wchar_t name[2048]{};
        StringCchCopyW(name, std::size(name), leaf);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hInstance = GetModuleHandleW(nullptr);
        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_ENABLEHOOK | OFN_ENABLETEMPLATE |
            OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_DONTADDTORECENT;
        ofn.lpTemplateName = MAKEINTRESOURCEW(102);
        ofn.lpfnHook = SaveHook;
        ofn.lCustData = reinterpret_cast<LPARAM>(&s);
        ofn.lpstrFile = name;
        ofn.nMaxFile = static_cast<DWORD>(std::size(name));
        ofn.lpstrInitialDir = directory.c_str();
        BOOL result = FALSE;
        bool consumed = true;
        if (native) result = GetSaveFileNameW(&ofn);
        else consumed = RunLegacyComDialog(&ofn, true, factory, result, [](const LegacyComDiagnostic& d) {
            printf("legacy=%s hr=%08lX\n", d.reason, static_cast<unsigned long>(d.error));
        }, true);
        const DWORD error = result ? 0 : native ? CommDlgExtendedError() : legacy_dialog_error::Query(&CommDlgExtendedError);
        const bool ok = consumed && result == !cancel && !error && (cancel || name == s.expected) &&
            s.init == 1 && s.destroyed == 1 && s.changes[0] > 0 && s.changes[1] > 0 && s.inputs_match &&
            s.veto_survived && s.accepted_notifications == (cancel ? 1u : 2u) && (native || seen == 1);
        printf("[%s] route=%s next=%s settings_changes=%u slice_changes=%u selected_values_and_path=%d "
            "visible_after_veto=%d fileok=%u init=%u destroy=%u consumed=%d result=%d error=%lu provider_shows=%u\n",
            ok ? "PASS" : "FAIL", native ? "win32" : "pulse", cancel ? "cancel" : "accept", s.changes[0],
            s.changes[1], s.accepted_notifications != 0 && s.inputs_match, s.veto_survived, s.accepted_notifications, s.init, s.destroyed,
            consumed, result, error, seen);
        failures += !ok;
    }
    if (!RemoveDirectoryW(directory.c_str())) { printf("cleanup error=%lu\n", GetLastError()); ++failures; }
    printf("failures=%u\n", failures);
    return failures ? 1 : 0;
}
}
