#include "../picker_inprocess/picker_property_panel.h"
// Explicitly opt in to short-lived, process-owned picker windows with --interaction.
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/folder_picker_dialog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <mutex>

namespace {
using namespace pulse::ui;
constexpr int kPathId = 101;
constexpr int kFilenameId = 102;
constexpr int kSearchId = 103;

HWND OwnPicker() {
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t name[80]{};
        GetClassNameW(hwnd, name, 80);
        if (process == GetCurrentProcessId() &&
            std::wstring_view(name) == L"PulseFolderPickerWindow") {
            *reinterpret_cast<HWND*>(context) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND OwnConfirmation() {
    HWND hwnd = OwnPicker();
    return hwnd && GetPropW(hwnd, L"Pulse.PickerOverwriteGeneration") ? hwnd : nullptr;
}

bool Send(HWND hwnd, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) return false;
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const bool sent = SendMessageTimeoutW(hwnd, message, wparam, lparam,
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
    const DWORD send_error = sent ? ERROR_SUCCESS : GetLastError();
    if (!sent) {
        std::printf("[DIAG] Send failed hwnd=%p message=0x%04X error=%lu visible=%d enabled=%d\n",
            static_cast<void*>(hwnd), message, send_error, IsWindowVisible(hwnd), IsWindowEnabled(hwnd));
        std::fflush(stdout);
    }
    return sent;
}

std::wstring Text(HWND hwnd) {
    wchar_t text[32768]{};
    Send(hwnd, WM_GETTEXT, std::size(text), reinterpret_cast<LPARAM>(text));
    return text;
}

bool SetText(HWND hwnd, const std::wstring& text) {
    return Send(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

bool Until(const std::function<bool()>& ready, const std::atomic<bool>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!done.load() && std::chrono::steady_clock::now() < end) {
        if (ready()) return true;
        // Poll a condition, rather than assuming an asynchronous operation has finished.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Outcome {
    bool picked = false;
    bool driven = false;
    FilePickerResult result;
    FilePickerStatus status = FilePickerStatus::Failed;
    DWORD error = ERROR_SUCCESS;
};
using Driver = std::function<bool(HWND, const std::atomic<bool>&)>;

Outcome Run(const FolderPickerSpec& spec, const Driver& drive, bool legacy = false, HWND owner = nullptr) {
    Outcome outcome;
    const bool dark = GetEnvironmentVariableW(L"PULSE_PICKER_TEST_DARK", nullptr, 0) != 0;
    auto presented_spec = spec;
    if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_PAINTED", nullptr, 0)) presented_spec.pulse_painted_plain_child = true;
    std::atomic<bool> done = false;
    bool ready = false, close_observed = false, cleanup_confirm = false, cleanup_picker = false;
    bool readiness_timeout = false, close_timeout = false;
    DWORD desktop_error = ERROR_SUCCESS;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    std::thread driver([&] {
        if (!SetThreadDesktop(desktop)) { desktop_error = GetLastError(); return; }
        HWND hwnd = nullptr;
        ready = Until([&] {
                hwnd = OwnPicker();
                return hwnd && IsWindowVisible(hwnd) && GetDlgItem(hwnd, kSearchId) &&
                    (spec.mode == PickerMode::Folder ||
                     (GetDlgItem(hwnd, kFilenameId) && IsWindowEnabled(GetDlgItem(hwnd, kFilenameId))));
            }, done);
        readiness_timeout = !ready && !done.load();
        if (ready) {
            outcome.driven = drive(hwnd, done);
            if (outcome.driven) {
                close_observed = Until([&] { return !IsWindow(hwnd); }, done);
                close_timeout = !close_observed && !done.load();
            }
        }
        if (HWND confirm = OwnConfirmation()) { cleanup_confirm = true; PostMessageW(confirm, WM_CLOSE, 0, 0); }
        if (hwnd && IsWindow(hwnd)) { cleanup_picker = true; PostMessageW(hwnd, WM_CLOSE, 0, 0); }
    });
    if (legacy) {
        std::wstring path;
        outcome.picked = ShowFolderPicker(owner, presented_spec, dark, {0.0f, 0.47f, 0.83f, 1.0f}, path);
        if (outcome.picked) outcome.result.paths.push_back(std::move(path));
    } else {
        auto detail = ShowFilePickerDetailed(owner, presented_spec, dark, {0.0f, 0.47f, 0.83f, 1.0f});
        outcome.status = detail.status;
        outcome.error = detail.error;
        outcome.picked = detail.status == FilePickerStatus::Accepted;
        outcome.result = std::move(detail.result);
    }
    done = true;
    driver.join();
    if (spec.mode == PickerMode::Save) {
        std::printf("[DIAG] SaveRun filename=[%ls] ready=%d driven=%d close_observed=%d cleanup_confirm=%d cleanup_picker=%d desktop_error=%lu status=%d error=%lu paths=%zu cancel_wait=%lu\n",
            spec.filename.c_str(), ready, outcome.driven, close_observed, cleanup_confirm, cleanup_picker, desktop_error,
            static_cast<int>(outcome.status), outcome.error, outcome.result.paths.size(),
            spec.cancel_event ? WaitForSingleObject(spec.cancel_event, 0) : WAIT_FAILED);
        std::printf("[DIAG] SaveRun readiness_timeout=%d close_timeout=%d\n", readiness_timeout, close_timeout);
        for (const auto& path : outcome.result.paths) std::printf("[DIAG] SaveRun path=[%ls]\n", path.c_str());
        std::fflush(stdout);
    }
    return outcome;
}

struct Fixture {
    std::filesystem::path root;
    explicit Fixture(bool in_workspace = false) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = (in_workspace ? std::filesystem::absolute(L"bench_data") : std::filesystem::path(temp)) /
            (L"PulsePickerInteraction-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root / L"nested");
        for (const auto* name : {L"alpha.txt", L"two words.txt", L"blocked.bin", L"background.png",
                                 L"nested\\inside.txt"}) {
            const auto path = root / name;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture creation");
            CloseHandle(file);
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

bool HasPaths(const Outcome& outcome, const std::vector<std::wstring>& expected) {
    if (!outcome.driven || !outcome.picked || outcome.result.paths.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (!SamePickerPath(outcome.result.paths[i], expected[i])) return false;
    return true;
}
} // namespace

int RunFilePickerDialogTest(int argc, wchar_t** argv) {
    const bool owner_focus_interactive = argc == 3 && std::wstring_view(argv[1]) == L"--owner-focus" && std::wstring_view(argv[2]) == L"--interactive";
    const bool owner_focus = (argc == 2 && std::wstring_view(argv[1]) == L"--owner-focus") || owner_focus_interactive;
    const bool outcomes = argc == 2 && std::wstring_view(argv[1]) == L"--outcomes";
    const bool save_interactive = argc == 3 && std::wstring_view(argv[1]) == L"--save" && std::wstring_view(argv[2]) == L"--interactive";
    const bool save = (argc == 2 && std::wstring_view(argv[1]) == L"--save") || save_interactive;
    const bool mockup_panels = argc == 2 && std::wstring_view(argv[1]) == L"--mockup-panels";
    const bool input_surfaces = argc == 2 && std::wstring_view(argv[1]) == L"--input-surfaces";
    if (!mockup_panels && !owner_focus && !save && !outcomes && !input_surfaces && (argc != 2 || std::wstring_view(argv[1]) != L"--interaction")) {
        std::puts("Use --interaction, --input-surfaces, --outcomes or --owner-focus for scoped file picker regression cases.");
        return 2;
    }
    // Surface regression runs without activating any window on the user's desktop.
    if ((owner_focus && !owner_focus_interactive) || input_surfaces || outcomes || (save && !save_interactive)) {
        const auto name = L"PulsePickerInputs_" + std::to_wstring(GetCurrentProcessId());
        HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) return 3;
        // Windows releases this process-owned desktop when the test exits.
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](const char* name, bool pass) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!pass) ++failures;
    };
    try {
        Fixture fixture(owner_focus || outcomes || save || mockup_panels);
        if (mockup_panels) {
            wchar_t snapshot_base[32768]{};
            const bool has_base = GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot_base, 32768) != 0;
            check("mockup snapshot destination provided", has_base);
            if (has_base) for (bool properties : {false, true}) for (int width : {640, 1000}) {
                FolderPickerSpec spec;
                spec.mode = PickerMode::Save; spec.initial_path = fixture.root.wstring();
                spec.filename = L"alpha"; spec.default_extension = L"txt";
                spec.filters = {{L"Text documents (*.txt)", L"*.txt"}};
                spec.title = properties ? L"Save document with properties" : L"Save text document";
                spec.explicit_title = true;
                auto add = [&](uint32_t kind, uint32_t layout, const wchar_t* label, const wchar_t* text) {
                    PickerCustomControl c;
                    c.kind = kind; c.property_layout = layout; c.state = 3;
                    c.id = static_cast<uint32_t>(spec.custom_controls.size() + 1);
                    c.label = label; c.text = text;
                    spec.custom_controls.push_back(c);
                };
                if (properties) {
                    pulse::picker_inprocess::PropertyPanel panel;
                    panel.fields = {
                        {0, PULSE_PICKER_UI_V2_PROPERTY_SCALAR, L"Title", {L"Quarterly project report"}},
                        {1, PULSE_PICKER_UI_V2_PROPERTY_SCALAR, L"Subject", {L"Progress and next steps"}},
                        {2, PULSE_PICKER_UI_V2_PROPERTY_LIST, L"Authors", {L"Project team"}},
                        {3, PULSE_PICKER_UI_V2_PROPERTY_LIST, L"Tags", {L"Planning"}}
                    };
                    panel.Build(spec.custom_controls);
                } else {
                    add(3, 0, L"Encoding", L"");
                    auto& combo = spec.custom_controls.back();
                    combo.items = {{1, 3, L"UTF-8"}, {2, 3, L"UTF-16"}};
                    combo.has_selected = true; combo.selected_item_id = 1;
                    add(7, 0, L"Use platform line endings", L"");
                    spec.custom_controls.back().checked = true;
                }
                const auto snapshot = std::wstring(snapshot_base) + (properties ? L"-properties-" : L"-simple-") +
                    std::to_wstring(width) + L".png";
                const auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                    const int dpi = static_cast<int>(pulse::compat::WindowDpi(hwnd));
                    const bool sized = SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(width, dpi, 96),
                        MulDiv(640, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                    const bool prop = SetPropW(hwnd, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(snapshot.c_str())) != FALSE;
                    const bool painted = Send(hwnd, WM_PAINT);
                    RemovePropW(hwnd, L"Pulse.PickerTestSnapshot");
                    const bool captured = std::filesystem::exists(snapshot) && std::filesystem::file_size(snapshot) > 0;
                    const bool closed = Send(hwnd, WM_CLOSE);
                    return sized && prop && painted && captured && closed;
                });
                check("mockup panel snapshot captured and cancelled without result or file writes",
                    outcome.driven && outcome.status == FilePickerStatus::Cancelled && outcome.result.paths.empty() &&
                    std::filesystem::file_size(fixture.root / L"alpha.txt") == 0 &&
                    !std::filesystem::exists(fixture.root / L"alpha"));
            }
            CoUninitialize();
            return failures ? 1 : 0;
        }

        if (owner_focus) {
            std::printf("[OWNER-FOCUS] desktop=%s; foreground assertions remain mandatory\n",
                owner_focus_interactive ? "interactive opt-in (owned windows only)" : "non-active isolated desktop");
            bool foreground_unavailable=false;
            constexpr UINT activate_owned=WM_APP+202;
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"PulsePickerOwnerFocusTest";
            wc.lpfnWndProc=[](HWND hwnd,UINT message,WPARAM wp,LPARAM lp)->LRESULT {
                if(message==WM_APP+202) {
                    SetForegroundWindow(hwnd);SetActiveWindow(hwnd);SetFocus(hwnd);
                    return GetForegroundWindow()==hwnd && GetActiveWindow()==hwnd;
                }
                return DefWindowProcW(hwnd,message,wp,lp);
            };
            RegisterClassW(&wc);
            HWND owner=CreateWindowExW(0,wc.lpszClassName,L"Picker owner fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                40,40,500,400,nullptr,nullptr,wc.hInstance,nullptr);
            HWND other=CreateWindowExW(0,wc.lpszClassName,L"Other owner fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                80,80,500,400,nullptr,nullptr,wc.hInstance,nullptr);
            check("two process-owned owner windows created",owner && other);
            if(owner && other) for(int variant=0;variant<3;++variant) {
                std::printf("[OWNER-FOCUS] case=%d (0 accept, 1 cancel, 2 switched cancellation)\n",variant);
                std::fflush(stdout);
                SetForegroundWindow(owner);SetActiveWindow(owner);
                bool picker_foreground=false,owner_disabled=false;
                FolderPickerSpec spec;spec.mode=PickerMode::File;spec.initial_path=fixture.root.wstring();
                spec.on_ready=[&](HWND hwnd) {
                    picker_foreground=GetForegroundWindow()==hwnd;
                    if(!picker_foreground) {
                        foreground_unavailable=true;
                        std::printf("[ENVIRONMENT-UNVERIFIED] picker cannot establish foreground: picker=%p foreground=%p interactive=%d\n",
                            static_cast<void*>(hwnd),static_cast<void*>(GetForegroundWindow()),owner_focus_interactive);
                        std::fflush(stdout);
                    }
                    owner_disabled=!IsWindowEnabled(owner);
                    return S_OK;
                };
                bool switched=false;
                const auto outcome=Run(spec,[&](HWND hwnd,const std::atomic<bool>&) {
                    if(variant==0) return SetText(GetDlgItem(hwnd,kFilenameId),L"alpha.txt") && Send(GetDlgItem(hwnd,kFilenameId),WM_KEYDOWN,VK_RETURN);
                    if(variant==2) {
                        DWORD_PTR activated=0;
                        switched=SendMessageTimeoutW(other,activate_owned,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&activated) && activated==1;
                        if(!switched) return false;
                    }
                    return PostMessageW(hwnd,WM_CLOSE,0,0)!=FALSE;
                },false,owner);
                check("picker initially owns foreground while original owner is disabled",picker_foreground && owner_disabled);
                if(variant==0) check("double-owner confirmation returns exact selected target",HasPaths(outcome,{(fixture.root/L"alpha.txt").wstring()}));
                else check("double-owner cancellation returns no target",outcome.driven && outcome.status==FilePickerStatus::Cancelled && outcome.result.paths.empty());
                check("only original owner modality is restored",IsWindowEnabled(owner) && IsWindowEnabled(other));
                check(variant==2 ? "explicit switch to another window is not stolen back" : "closing picker restores original owner activation",
                    (variant!=2 || switched) && GetForegroundWindow()==(variant==2 ? other : owner) && GetActiveWindow()==(variant==2 ? other : owner));
            }
            if(other) DestroyWindow(other);
            if(owner) DestroyWindow(owner);
            if(foreground_unavailable) std::puts("[UNVERIFIED] foreground restoration was not verified in this environment; assertion failures are retained.");
            CoUninitialize();return foreground_unavailable ? 4 : failures ? 1 : 0;
        }
        if (save) {
            { std::ofstream content(fixture.root / L"alpha.txt", std::ios::binary); content << "save-fixture"; }
            FolderPickerSpec spec;
            spec.mode = PickerMode::Save;
            spec.initial_path = fixture.root.wstring();
            spec.filename = L"new-file";
            spec.default_extension = L"txt";
            spec.filters = {{L"Text", L"*.txt"}};
            auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                const HWND edit = GetDlgItem(hwnd, kFilenameId);
                const auto observed_name = Text(edit);
                const bool initial_name = observed_name == spec.filename;
                std::printf("[DIAG] initial filename hwnd=%p expected=[%ls] actual=[%ls] visible=%d enabled=%d\n",
                    static_cast<void*>(edit), spec.filename.c_str(), observed_name.c_str(), IsWindowVisible(edit), IsWindowEnabled(edit));
                check("save native filename edit contains the initial text", initial_name);
                wchar_t snapshot[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot, 32768)) {
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", snapshot);
                    Send(hwnd, WM_PAINT);
                    RemovePropW(hwnd, L"Pulse.PickerTestSnapshot");
                }
                return initial_name && Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
            });
            check("save returns new target with default extension without creating it",
                HasPaths(outcome, {(fixture.root / L"new-file.txt").wstring()}) &&
                !std::filesystem::exists(fixture.root / L"new-file.txt"));
            spec.allow_multiselect = true;
            const auto multiple = ShowFilePickerDetailed(nullptr, spec, false, {0, 0.47f, 0.83f, 1});
            check("save rejects multiselect before showing UI", multiple.status == FilePickerStatus::Failed &&
                multiple.error == ERROR_INVALID_PARAMETER && !OwnPicker());
            spec.allow_multiselect = false;
            spec.filename = L"alpha.txt";
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                if (!Send(confirm, WM_KEYDOWN, VK_ESCAPE)) return false;
                if (!Until([&] { return !OwnConfirmation() && IsWindowEnabled(hwnd); }, done)) return false;
                return PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
            });
            check("declining overwrite keeps picker open and submits no path",
                outcome.driven && outcome.status == FilePickerStatus::Cancelled && outcome.result.paths.empty());
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                // Inline confirmation defaults to returning to editing; move explicitly to Replace.
                wchar_t snapshot[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot, 32768)) {
                    const std::wstring confirmation_snapshot = std::wstring(snapshot) + L".overwrite.png";
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(confirmation_snapshot.c_str()));
                    Send(hwnd, WM_PAINT); RemovePropW(hwnd, L"Pulse.PickerTestSnapshot");
                }
                return Send(confirm, WM_KEYDOWN, VK_LEFT) && Send(confirm, WM_KEYDOWN, VK_RETURN);
            });
            check("confirming overwrite returns existing target without writing",
                HasPaths(outcome, {(fixture.root / L"alpha.txt").wstring()}) &&
                std::filesystem::file_size(fixture.root / L"alpha.txt") == 12);
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                return Send(confirm, WM_KEYDOWN, VK_TAB) && Send(confirm, WM_KEYDOWN, VK_RETURN);
            });
            check("rename save returns a different target without writing either file",
                outcome.driven && outcome.status == FilePickerStatus::Accepted && outcome.result.paths.size() == 1 &&
                std::filesystem::path(outcome.result.paths.front()).parent_path() == fixture.root &&
                std::filesystem::path(outcome.result.paths.front()).extension() == L".txt" &&
                outcome.result.paths.front() != (fixture.root / L"alpha.txt").wstring() &&
                !std::filesystem::exists(outcome.result.paths.front()) &&
                std::filesystem::file_size(fixture.root / L"alpha.txt") == 12);
            bool replaced = false, saw_second_confirmation = false;
            const auto alpha = fixture.root / L"alpha.txt";
            const auto retained = fixture.root / L"retained-alpha.txt";
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                const HANDLE old_generation = GetPropW(confirm, L"Pulse.PickerOverwriteGeneration");
                if (!MoveFileW(alpha.c_str(), retained.c_str())) return false;
                { std::ofstream content(alpha, std::ios::binary); content << "save-fixture"; replaced = content.good(); }
                if (!replaced || !Send(confirm, WM_KEYDOWN, VK_LEFT) || !Send(confirm, WM_KEYDOWN, VK_RETURN)) return false;
                HWND second = nullptr;
                saw_second_confirmation = Until([&] {
                    second = OwnConfirmation();
                    return second && GetPropW(second, L"Pulse.PickerOverwriteGeneration") != old_generation;
                }, done);
                return saw_second_confirmation && Send(second, WM_KEYDOWN, VK_LEFT) && Send(second, WM_KEYDOWN, VK_RETURN);
            });
            check("replacing target during overwrite asks again before accepting the replacement",
                replaced && saw_second_confirmation && HasPaths(outcome, {alpha.wstring()}));
            if (std::filesystem::exists(retained)) {
                check("replacement UI fixture restored", DeleteFileW(alpha.c_str()) && MoveFileW(retained.c_str(), alpha.c_str()));
            }
            bool readonly_observed = false;
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                if (!SetFileAttributesW(alpha.c_str(), FILE_ATTRIBUTE_READONLY) ||
                    !Send(confirm, WM_KEYDOWN, VK_LEFT) || !Send(confirm, WM_KEYDOWN, VK_RETURN)) return false;
                // The filename edit is disabled for the pending revalidation;
                // enabled again means its error result was handled by the UI.
                readonly_observed = Until([&] { return !OwnConfirmation() && IsWindowEnabled(hwnd) &&
                    IsWindowEnabled(GetDlgItem(hwnd, kFilenameId)); }, done);
                return readonly_observed && PostMessageW(hwnd, WM_CLOSE, 0, 0);
            });
            check("readonly transition after confirmation remains open and returns no path on cancel",
                readonly_observed && outcome.driven && outcome.status == FilePickerStatus::Cancelled && outcome.result.paths.empty());
            check("readonly UI fixture restored", SetFileAttributesW(alpha.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
            bool filename_invalidated = false;
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN)) return false;
                HWND confirm = nullptr;
                if (!Until([&] { confirm = OwnConfirmation(); return confirm != nullptr; }, done)) return false;
                const auto changed = L"changed-during-confirmation.txt";
                if (!Send(GetDlgItem(hwnd, kFilenameId), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(changed))) return false;
                filename_invalidated = Until([&] { return !OwnConfirmation() && IsWindowEnabled(hwnd) &&
                    IsWindowEnabled(GetDlgItem(hwnd, kFilenameId)) && Text(GetDlgItem(hwnd, kFilenameId)) == changed; }, done);
                return filename_invalidated && PostMessageW(hwnd, WM_CLOSE, 0, 0);
            });
            check("filename edit during nested confirmation invalidates old consent and submits neither path",
                filename_invalidated && outcome.driven && outcome.status == FilePickerStatus::Cancelled && outcome.result.paths.empty() &&
                !std::filesystem::exists(fixture.root / L"changed-during-confirmation.txt"));
            HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            check("save cancellation fixture created", cancel != nullptr);
            if (cancel) {
                spec.cancel_event = cancel;
                outcome = Run(spec, [cancel](HWND hwnd, const std::atomic<bool>& done) {
                    const auto initial = Text(GetDlgItem(hwnd, kFilenameId));
                    const bool submitted = Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
                    const bool confirmation = submitted && Until([] { return OwnConfirmation() != nullptr; }, done);
                    SetLastError(ERROR_SUCCESS);
                    const bool signalled = confirmation && SetEvent(cancel);
                    const DWORD event_error = GetLastError();
                    std::printf("[DIAG] cancel-case filename=[%ls] submit=%d confirm=%d SetEvent=%d error=%lu wait=%lu done=%d\n",
                        initial.c_str(), submitted, confirmation, signalled, event_error, WaitForSingleObject(cancel, 0), done.load());
                    std::fflush(stdout);
                    return submitted && confirmation && signalled;
                });
                check("bridge cancellation closes nested overwrite confirmation and picker",
                    outcome.driven && outcome.status == FilePickerStatus::Cancelled && outcome.result.paths.empty() && !OwnConfirmation());
                CloseHandle(cancel);
                spec.cancel_event = nullptr;
            }
            check("save cases never created a new file", !std::filesystem::exists(fixture.root / L"new-file.txt"));
            CoUninitialize();
            return failures ? 1 : 0;
        }
        if (outcomes) {
            FolderPickerSpec spec;
            spec.mode = PickerMode::File;
            spec.initial_path = fixture.root.wstring();
            const HWND invalid_owner = reinterpret_cast<HWND>(static_cast<INT_PTR>(-1));
            const auto invalid = ShowFilePickerDetailed(invalid_owner, spec, false, {0, 0.47f, 0.83f, 1});
            check("invalid owner is failure, not user cancellation",
                invalid.status == FilePickerStatus::Failed && invalid.error == ERROR_INVALID_WINDOW_HANDLE && invalid.result.paths.empty());
            const auto cancelled = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
                return PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
            });
            check("closing picker reports explicit cancellation without error or paths",
                cancelled.driven && cancelled.status == FilePickerStatus::Cancelled &&
                cancelled.error == ERROR_SUCCESS && cancelled.result.paths.empty());
            HANDLE cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            check("create isolated bridge cancellation event", cancel != nullptr);
            if (cancel) {
                spec.cancel_event = cancel;
                const auto before_show = ShowFilePickerDetailed(nullptr, spec, false, {0, 0.47f, 0.83f, 1});
                check("pre-cancelled bridge does not show a picker", before_show.status == FilePickerStatus::Cancelled && !OwnPicker());
                ResetEvent(cancel);
                const auto disconnected = Run(spec, [cancel](HWND, const std::atomic<bool>&) {
                    return SetEvent(cancel) != FALSE;
                });
                check("bridge cancellation closes modal picker without a result",
                    disconnected.driven && disconnected.status == FilePickerStatus::Cancelled && disconnected.result.paths.empty());
                spec.cancel_event = nullptr;
                CloseHandle(cancel);
            }
            const auto accepted = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return filename && SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("typed file reports acceptance and its actual result",
                accepted.status == FilePickerStatus::Accepted && accepted.error == ERROR_SUCCESS &&
                HasPaths(accepted, {(fixture.root / L"alpha.txt").wstring()}));
            CoUninitialize();
            return failures ? 1 : 0;
        }
        if (input_surfaces) {
            FolderPickerSpec spec;
            spec.mode = PickerMode::File;
            spec.initial_path = fixture.root.wstring();
            const auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                wchar_t snapshot[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot, 32768)) {
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", snapshot);
                    Send(hwnd, WM_PAINT);
                    check("embedded picker snapshot is saved", std::filesystem::exists(snapshot));
                }
                bool passed = true;
                for (const int id : {kPathId, kFilenameId, kSearchId}) {
                    HWND edit = GetDlgItem(hwnd, id);
                    DWORD flags = 0;
                    BYTE alpha = 0;
                    const bool redirected = edit && GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) &&
                        (flags & LWA_ALPHA) && alpha == 255;
                    check("picker input inherits redirected surface from shared control", redirected);
                    const bool typed = edit && SetText(edit, L"Typed 中文") && Text(edit) == L"Typed 中文";
                    check("picker input retains Unicode editing", typed);
                    passed = passed && redirected && typed;
                }
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return passed;
            });
            check("picker input check closes without selecting or changing files", outcome.driven && !outcome.picked);
            const auto picked = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return filename && SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("embedded picker accepts a typed filename", HasPaths(picked, {(fixture.root / L"alpha.txt").wstring()}));
            CoUninitialize();
            return failures ? 1 : 0;
        }
        const auto path = [&](const wchar_t* name) { return (fixture.root / name).wstring(); };
        FolderPickerSpec spec;
        spec.mode = PickerMode::File;
        spec.initial_path = fixture.root.wstring();
        spec.filename = L"alpha.txt";
        spec.filters = {{L"Text files", L"*.txt"}};
        std::mutex folder_mutex;
        std::wstring observed_folder;
        spec.on_dialog_event = [&](HWND, PickerDialogEvent, const PickerDialogSnapshot& snapshot, const std::wstring&) {
            std::lock_guard lock(folder_mutex); observed_folder = snapshot.folder; return S_OK;
        };
        auto folder_is = [&](const std::wstring& expected) {
            std::lock_guard lock(folder_mutex); return SamePickerPath(observed_folder, expected);
        };
        auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            return Until([&] { return folder_is(spec.initial_path); }, done) &&
                Text(GetDlgItem(hwnd, kFilenameId)) == spec.filename &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("initial directory and filename", HasPaths(outcome, {path(L"alpha.txt")}));

        spec.allow_multiselect = true;
        spec.filename = L"\"alpha.txt\" \"two words.txt\"";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("quoted multiple filenames", HasPaths(outcome,
              {path(L"alpha.txt"), path(L"two words.txt")}));

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("filter rejects disallowed file and allows retry",
              HasPaths(outcome, {path(L"alpha.txt")}));

        spec.filename = L"nested";
        outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return folder_is(path(L"nested")) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"inside.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("typed directory navigates before file submission",
              HasPaths(outcome, {path(L"nested\\inside.txt")}));

        spec.filename = L"alpha.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND search = GetDlgItem(hwnd, kSearchId);
            return SetText(search, L"alpha") && Send(search, WM_KEYDOWN, VK_DOWN) &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_ESCAPE);
        });
        check("search and arrow key do not submit; Escape cancels",
              outcome.driven && !outcome.picked && outcome.result.paths.empty());

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) && Send(hwnd, WM_CLOSE);
        });
        check("cancel rejected candidate validation", outcome.driven && !outcome.picked &&
              outcome.result.paths.empty());
        spec.filename = L"two words.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("new request excludes previous validation result",
              HasPaths(outcome, {path(L"two words.txt")}));

        FolderPickerSpec folder;
        folder.mode = PickerMode::Folder;
        folder.initial_path = fixture.root.wstring();
        outcome = Run(folder, [](HWND hwnd, const std::atomic<bool>& done) {
            // The folder mode has no filename control. Enter is harmless until
            // the directory listing has verified that the current folder exists.
            const bool closed = Until([&] {
                if (!IsWindow(hwnd)) return true;
                Send(hwnd, WM_KEYDOWN, VK_RETURN);
                return !IsWindow(hwnd);
            }, done);
            return closed || done.load();
        }, true);
        check("legacy folder entry confirms current directory",
              HasPaths(outcome, {fixture.root.wstring()}));

        FolderPickerSpec image;
        image.mode = PickerMode::Image;
        image.initial_path = fixture.root.wstring();
        image.filename = L"alpha.txt";
        image.filters = {{L"Text and images", L"*.txt;*.png"}};
        outcome = Run(image, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"background.png") && Send(filename, WM_KEYDOWN, VK_RETURN);
        }, true);
        check("legacy image entry rejects text despite filter and accepts image extension",
              HasPaths(outcome, {path(L"background.png")}));
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture: %s\n", error.what());
        ++failures;
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
