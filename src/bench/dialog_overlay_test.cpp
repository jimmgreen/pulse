#include "../common/dialog_bridge_state.h"
#include "../dialog_overlay/companion_bar.h"
#include "../dialog_overlay/dialog_classifier.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace overlay = pulse::dialog_overlay;
namespace state_ns = pulse::dialog_bridge_state;

namespace {

constexpr int kEditId = 1148;
constexpr int kCustomCheckId = 9001;

struct FixtureDialogState {
    bool with_custom_control = false;
    std::wstring result_file;
    std::wstring accepted_text;
    int exit_code = 0;
};

void WriteUtf8File(const std::wstring& path, const std::string& content) {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f << content;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

INT_PTR CALLBACK FixtureDlgProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<FixtureDialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<FixtureDialogState*>(lparam);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
        // Keep completely off-screen so automated tests never disturb the user's desktop.
        SetWindowPos(dlg, nullptr, -20000, -20000, 320, 160, SWP_NOZORDER | SWP_NOACTIVATE);

        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        10, 10, 260, 24, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        10, 44, 80, 24, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        100, 44, 80, 24, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                        GetModuleHandleW(nullptr), nullptr);

        if (st && st->with_custom_control) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"CtrlNotifySink";
            RegisterClassExW(&wc);
            HWND sink = CreateWindowExW(0, L"CtrlNotifySink", L"", WS_CHILD | WS_VISIBLE,
                                        10, 80, 240, 28, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (sink) {
                CreateWindowExW(0, L"BUTTON", L"App Custom Option",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                0, 0, 200, 24, sink,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomCheckId)),
                                GetModuleHandleW(nullptr), nullptr);
            }
        }
        // Signal readiness by writing our HWND into result_file + ".hwnd".
        if (st && !st->result_file.empty()) {
            WriteUtf8File(st->result_file + L".hwnd",
                          std::to_string(reinterpret_cast<uintptr_t>(dlg)));
        }
        return FALSE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDOK) {
            wchar_t buf[1024]{};
            GetDlgItemTextW(dlg, kEditId, buf, ARRAYSIZE(buf));
            if (st) {
                st->accepted_text = buf;
                st->exit_code = 100;
                WriteUtf8File(st->result_file, "OK:" + WideToUtf8(st->accepted_text));
            }
            EndDialog(dlg, 100);
            return TRUE;
        }
        if (id == IDCANCEL) {
            if (st) {
                st->exit_code = 200;
                WriteUtf8File(st->result_file, "CANCEL");
            }
            EndDialog(dlg, 200);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        if (st) {
            st->exit_code = 200;
            WriteUtf8File(st->result_file, "CANCEL");
        }
        EndDialog(dlg, 200);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

int RunChildDialogFixture(bool with_custom, const std::wstring& result_file) {
    // Build a minimal in-memory DLGTEMPLATE (#32770) — zero .rc resource dependency!
    alignas(4) struct {
        DLGTEMPLATE tmpl;
        WORD menu;
        WORD cls;
        WORD title;
    } mem{};
    mem.tmpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    mem.tmpl.dwExtendedStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    mem.tmpl.x = 0;
    mem.tmpl.y = 0;
    mem.tmpl.cx = 160;
    mem.tmpl.cy = 80;

    FixtureDialogState st{};
    st.with_custom_control = with_custom;
    st.result_file = result_file;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &mem.tmpl, nullptr,
                            &FixtureDlgProc, reinterpret_cast<LPARAM>(&st));
    return st.exit_code;
}

struct ChildDialog {
    PROCESS_INFORMATION pi{};
    HWND hwnd = nullptr;
    std::wstring result_file;

    ~ChildDialog() {
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 500) != WAIT_OBJECT_0) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        DeleteFileW(result_file.c_str());
        DeleteFileW((result_file + L".hwnd").c_str());
    }
};

bool SpawnFixtureDialog(const std::wstring& self_exe, const std::wstring& mode,
                        const std::wstring& result_file, ChildDialog& out) {
    out.result_file = result_file;
    DeleteFileW(result_file.c_str());
    DeleteFileW((result_file + L".hwnd").c_str());

    std::wstring cmd = L"\"" + self_exe + L"\" --dialog-fixture " + mode + L" \"" + result_file + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    if (!CreateProcessW(self_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &out.pi)) {
        return false;
    }

    const std::wstring hwnd_file = result_file + L".hwnd";
    for (int i = 0; i < 200; ++i) {
        std::ifstream f{std::filesystem::path(hwnd_file)};
        if (f.good()) {
            uintptr_t raw = 0;
            if (f >> raw && raw != 0) {
                HWND h = reinterpret_cast<HWND>(raw);
                if (IsWindow(h)) {
                    out.hwnd = h;
                    return true;
                }
            }
        }
        Sleep(15);
    }
    return false;
}

std::string ReadFileAll(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && _wcsicmp(argv[1], L"--dialog-fixture") == 0) {
        const bool custom = _wcsicmp(argv[2], L"custom") == 0;
        return RunChildDialogFixture(custom, argv[3]);
    }

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    wchar_t self_buf[32768]{};
    GetModuleFileNameW(nullptr, self_buf, ARRAYSIZE(self_buf));
    const std::wstring self_exe(self_buf);

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring base = std::wstring(temp) + L"pulse-overlay-test-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64());

    state_ns::ActiveDialogLedger ledger;
    check(ledger.OpenOrCreate(L"Local\\PulseOverlayTestLedger-" + std::to_wstring(GetCurrentProcessId())) == ERROR_SUCCESS,
          "ActiveDialogLedger opened for Layer 3 deduplication test");

    // 1. Standard #32770 classification, Layer 1/2 deduplication, and cross-process DriveNativeDialogNavigation.
    {
        ChildDialog child;
        check(SpawnFixtureDialog(self_exe, L"standard", base + L"-std.txt", child),
              "spawned off-screen standard #32770 child fixture");

        const auto info = overlay::ClassifyNativeDialog(child.hwnd, &ledger);
        check(info.kind == overlay::NativeDialogKind::LegacyFileDialog &&
                  info.filename_edit_hwnd != nullptr &&
                  info.ok_button_hwnd != nullptr &&
                  info.cancel_button_hwnd != nullptr &&
                  !info.has_custom_controls &&
                  info.mode_b_eligible,
              "ClassifyNativeDialog identifies standard #32770 controls and Mode B eligibility");

        // Layer 1/2 deduplication check.
        ledger.MarkActive(child.pi.dwProcessId, reinterpret_cast<uint64_t>(child.hwnd), 1);
        const auto dedup = overlay::ClassifyNativeDialog(child.hwnd, &ledger);
        check(dedup.kind == overlay::NativeDialogKind::AlreadyTakenOverByPulse,
              "ActiveDialogLedger prevents Layer 3 double-takeover when Layer 1/2 owns PID");
        ledger.ClearActive(child.pi.dwProcessId);

        // Drive navigation + submit via Edit(1148) + IDOK.
        check(overlay::DriveNativeDialogNavigation(info, L"C:\\TestTarget\\report.txt", true),
              "DriveNativeDialogNavigation injects path and clicks IDOK across processes");
        DWORD code = 0;
        const bool exited = WaitForSingleObject(child.pi.hProcess, 3000) == WAIT_OBJECT_0 &&
                            GetExitCodeProcess(child.pi.hProcess, &code);
        const std::string recorded = ReadFileAll(child.result_file);
        check(exited && code == 100 && recorded == "OK:C:\\TestTarget\\report.txt",
              "child #32770 received exact injected path and accepted cleanly");
    }

    // 2. Custom-control #32770 (CtrlNotifySink checkbox) -> strictly rejects Mode B!
    {
        ChildDialog child;
        check(SpawnFixtureDialog(self_exe, L"custom", base + L"-custom.txt", child),
              "spawned off-screen custom-control #32770 child fixture");

        const auto info = overlay::ClassifyNativeDialog(child.hwnd, &ledger);
        check(info.kind == overlay::NativeDialogKind::LegacyFileDialog &&
                  info.has_custom_controls &&
                  !info.mode_b_eligible,
              "ClassifyNativeDialog detects CtrlNotifySink custom control and disables Mode B");

        const auto refused = overlay::RunModeBProxy(info, [](std::wstring& out) {
            out = L"C:\\ShouldNotRun.txt";
            return true;
        });
        check(!refused.handled && !refused.accepted,
              "RunModeBProxy strictly refuses dialogs with custom controls (Mode A only)");

        check(overlay::CancelNativeDialog(info), "CancelNativeDialog cleanly closes custom fixture");
        DWORD code = 0;
        check(WaitForSingleObject(child.pi.hProcess, 3000) == WAIT_OBJECT_0 &&
                  GetExitCodeProcess(child.pi.hProcess, &code) && code == 200,
              "custom fixture exited cleanly with cancel status");
    }

    // 3. Mode B Proxy Accept & Cancel/Restore end-to-end on standard #32770.
    {
        ChildDialog child_ok;
        check(SpawnFixtureDialog(self_exe, L"standard", base + L"-modeb-ok.txt", child_ok),
              "spawned Mode B accept fixture");
        const auto info = overlay::ClassifyNativeDialog(child_ok.hwnd, &ledger);
        const auto outcome = overlay::RunModeBProxy(info, [](std::wstring& out) {
            out = L"C:\\ModeB\\picked.pdf";
            return true;
        });
        DWORD code = 0;
        const bool exited = WaitForSingleObject(child_ok.pi.hProcess, 3000) == WAIT_OBJECT_0 &&
                            GetExitCodeProcess(child_ok.pi.hProcess, &code);
        check(outcome.handled && outcome.accepted && exited && code == 100 &&
                  ReadFileAll(child_ok.result_file) == "OK:C:\\ModeB\\picked.pdf",
              "RunModeBProxy hides native, injects Pulse selection, and accepts cleanly");
    }
    {
        ChildDialog child_cancel;
        check(SpawnFixtureDialog(self_exe, L"standard", base + L"-modeb-cancel.txt", child_cancel),
              "spawned Mode B cancel fixture");
        const auto info = overlay::ClassifyNativeDialog(child_cancel.hwnd, &ledger);
        const auto outcome = overlay::RunModeBProxy(info, [](std::wstring&) {
            return false; // User cancelled Pulse dialog
        });
        DWORD code = 0;
        const bool exited = WaitForSingleObject(child_cancel.pi.hProcess, 3000) == WAIT_OBJECT_0 &&
                            GetExitCodeProcess(child_cancel.pi.hProcess, &code);
        check(outcome.handled && !outcome.accepted && outcome.restored_native &&
                  exited && code == 200 && ReadFileAll(child_cancel.result_file) == "CANCEL",
              "RunModeBProxy restores native dialog and cancels cleanly when user cancels");
    }

    // 4. Mode A CompanionBar non-activating window semantics (WS_EX_NOACTIVATE + MA_NOACTIVATE).
    {
        overlay::CompanionBar bar;
        check(bar.Create(GetModuleHandleW(nullptr)) && bar.Hwnd() != nullptr,
              "CompanionBar creates non-activating popup window");
        const LONG_PTR ex = GetWindowLongPtrW(bar.Hwnd(), GWL_EXSTYLE);
        const LRESULT ma = SendMessageW(bar.Hwnd(), WM_MOUSEACTIVATE, 0, 0);
        check((ex & WS_EX_NOACTIVATE) != 0 && (ex & WS_EX_TOOLWINDOW) != 0 && ma == MA_NOACTIVATE,
              "CompanionBar enforces WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW + MA_NOACTIVATE (never steals focus)");
        bar.Destroy();
    }

    return failures ? 1 : 0;
}
