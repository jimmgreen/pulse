#include "picker_places_test_api.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/picker_places_snapshot.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace pulse::ui;
namespace places = pulse::picker_places;
namespace fs = std::filesystem;

bool Send(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0, DWORD_PTR* reply = nullptr) {
    DWORD_PTR result = 0;
    const bool okay = SendMessageTimeoutW(window, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0;
    if (reply) *reply = result;
    return okay;
}
template<class Predicate> bool Until(Predicate predicate, const std::atomic<bool>& done) {
    const auto deadline = GetTickCount64() + 8000;
    while (!done && GetTickCount64() < deadline) {
        if (predicate()) return true;
        Sleep(20);
    }
    return false;
}
bool PointFor(HWND window, PickerPlacesTestPoint::Kind kind, const std::wstring& path, POINT& point) {
    PickerPlacesTestPoint query{kind, path.c_str(), {}};
    DWORD_PTR result = 0;
    if (!Send(window, kPickerPlacesTestPointMessage, 0, reinterpret_cast<LPARAM>(&query), &result) || !result) return false;
    point = query.point;
    return true;
}
LPARAM Position(POINT point) { return MAKELPARAM(point.x, point.y); }
bool Click(HWND window, POINT point, DWORD modifiers = 0) {
    return Send(window, WM_LBUTTONDOWN, MK_LBUTTON | modifiers, Position(point)) &&
        Send(window, WM_LBUTTONUP, modifiers, Position(point));
}
bool Drag(HWND window, POINT source, POINT target, DWORD modifiers = 0, bool cancel = false) {
    if (!Send(window, WM_LBUTTONDOWN, MK_LBUTTON | modifiers, Position(source)) ||
        !Send(window, WM_MOUSEMOVE, MK_LBUTTON | modifiers, Position({source.x + 16, source.y + 16})) ||
        !Send(window, WM_MOUSEMOVE, MK_LBUTTON | modifiers, Position(target))) return false;
    if (cancel && !Send(window, WM_KEYDOWN, VK_ESCAPE)) return false;
    return Send(window, WM_LBUTTONUP, modifiers, Position(target));
}
struct Keyboard {
    DWORD target = 0;
    bool attached = false;
    BYTE saved[256]{};
    explicit Keyboard(HWND window) {
        target = GetWindowThreadProcessId(window, nullptr);
        attached = AttachThreadInput(GetCurrentThreadId(), target, TRUE) != FALSE;
        GetKeyboardState(saved);
    }
    bool Control(bool pressed) {
        BYTE state[256]{};
        std::copy(std::begin(saved), std::end(saved), std::begin(state));
        state[VK_CONTROL] = state[VK_LCONTROL] = pressed ? 0x80 : 0;
        return attached && SetKeyboardState(state);
    }
    ~Keyboard() { SetKeyboardState(saved); if (attached) AttachThreadInput(GetCurrentThreadId(), target, FALSE); }
};
struct State {
    std::mutex mutex;
    places::Snapshot shared;
    PickerDialogSnapshot dialog;
    unsigned calls = 0;
};
struct Fixture {
    fs::path parent = fs::absolute(L"bench_data").lexically_normal();
    fs::path root = parent / (L"picker-shared-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Fixture() {
        fs::create_directories(root / L"source");
        fs::create_directories(root / L"destination");
        fs::create_directories(root / L"elsewhere");
        for (const auto* relative : {L"source\\alpha.txt", L"source\\beta.txt", L"elsewhere\\favorite.txt"}) {
            std::ofstream file(root / relative, std::ios::binary);
            file << "picker shared places regression\n";
            if (!file) throw std::runtime_error("cannot create isolated picker fixture");
        }
    }
    ~Fixture() {
        if (root.parent_path() == parent && root.filename().wstring().starts_with(L"picker-shared-")) {
            std::error_code error; fs::remove_all(root, error);
        }
    }
};

bool Run(FolderPickerSpec spec, State& state, const std::function<bool(HWND, const std::atomic<bool>&)>& drive) {
    std::atomic<HWND> ready{nullptr};
    std::atomic<bool> done{false};
    std::atomic<int> last_event{-1};
    bool driven = false;
    wchar_t capture_base[32768]{};
    static unsigned capture_number = 0;
    std::wstring capture;
    const DWORD capture_length = GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", capture_base, 32768);
    if (capture_length && capture_length < 32768)
        capture = std::wstring(capture_base) + L"-shared-" + std::to_wstring(++capture_number) + L".png";
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel) return false;
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse picker shared places test", WS_OVERLAPPEDWINDOW,
        160, 160, 900, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) { CloseHandle(cancel); return false; }
    // Consume any launcher SW_HIDE once, then present a real host like a takeover.
    ShowWindow(owner, SW_HIDE);
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    UpdateWindow(owner);
    spec.cancel_event = cancel;
    spec.on_ready = [&](HWND window) { ready = window; return S_OK; };
    spec.read_shared_places = [&] { std::lock_guard lock(state.mutex); return state.shared; };
    spec.add_shared_favorites = [&](const std::vector<places::StarredItem>& items, const std::wstring& group) {
        std::lock_guard lock(state.mutex);
        ++state.calls;
        for (auto item : items) {
            if (std::none_of(state.shared.starred_items.begin(), state.shared.starred_items.end(),
                [&](const auto& old) { return _wcsicmp(old.path.c_str(), item.path.c_str()) == 0; })) {
                item.group = group;
                state.shared.starred_items.push_back(std::move(item));
            }
        }
        return true;
    };
    spec.on_dialog_event = [&](HWND, PickerDialogEvent event, const PickerDialogSnapshot& snapshot, const std::wstring&) {
        last_event = static_cast<int>(event);
        std::lock_guard lock(state.mutex); state.dialog = snapshot; return S_OK;
    };
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    std::thread driver([&] {
        if (SetThreadDesktop(desktop) && Until([&] { return ready.load() != nullptr; }, done)) {
            HWND window = ready;
            try { driven = drive(window, done); } catch (...) { driven = false; }
            if (!capture.empty() && IsWindow(window)) {
                SetPropW(window, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(capture.c_str()));
                Send(window, WM_PAINT);
                RemovePropW(window, L"Pulse.PickerTestSnapshot");
            }
            if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        }
        SetEvent(cancel);
    });
    const auto outcome = ShowFilePickerDetailed(owner, spec, false, {0.0f, 0.47f, 0.83f, 1.0f});
    done = true;
    driver.join();
    DestroyWindow(owner);
    CloseHandle(cancel);
    if (!driven || outcome.status != FilePickerStatus::Cancelled)
        std::printf("[DETAIL] driven=%d ready=%p status=%d error=%lu last_event=%d\n", driven, ready.load(), static_cast<int>(outcome.status), outcome.error, last_event.load());
    return driven && outcome.status == FilePickerStatus::Cancelled;
}
}

int RunPickerSharedPlacesUiTest() {
    // Like picker_layout_ui_test, use the displayed desktop: readiness requires
    // an actual composed frame, which an inactive CreateDesktop cannot provide.
    // Only process-owned windows and the isolated fixture are driven below.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](const char* label, bool okay) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label); std::fflush(stdout);
        if (!okay) ++failures;
    };
    try {
        Fixture fixture;
        const auto source = (fixture.root / L"source").wstring();
        const auto alpha = (fixture.root / L"source" / L"alpha.txt").wstring();
        const auto beta = (fixture.root / L"source" / L"beta.txt").wstring();
        const auto destination = (fixture.root / L"destination").wstring();
        const auto favorite = (fixture.root / L"elsewhere" / L"favorite.txt").wstring();
        State state;
        state.shared.shared_available = true;
        state.shared.workspaces.push_back({destination, L"Test workspace", 0x4CC2FF});
        state.shared.starred_items.push_back({favorite, 1, {}});
        FolderPickerSpec spec;
        spec.pulse_painted_plain_child = true;
        spec.mode = PickerMode::File; spec.initial_path = source; spec.allow_multiselect = true;
        check("multiselect favorite drop adds references, repeated drop stays idempotent", Run(spec, state, [&](HWND window, const auto& done) {
            POINT a{}, b{}, favorite_header{};
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Row, alpha, a) &&
                PointFor(window, PickerPlacesTestPoint::Kind::Row, beta, b) &&
                PointFor(window, PickerPlacesTestPoint::Kind::FavoriteHeader, L"", favorite_header); }, done)) return false;
            Keyboard keyboard(window);
            if (!Click(window, a) || !keyboard.Control(true) || !Click(window, b, MK_CONTROL) || !keyboard.Control(false)) return false;
            for (unsigned attempt = 1; attempt <= 2; ++attempt) {
                if (!Drag(window, a, favorite_header) || !Until([&] {
                    std::lock_guard lock(state.mutex); return state.calls >= attempt;
                }, done)) return false;
            }
            std::lock_guard lock(state.mutex);
            return state.shared.starred_items.size() == 3 && fs::exists(alpha) && fs::exists(beta);
        }));
        check("Ctrl workspace drop copies file and preserves source", Run(spec, state, [&](HWND window, const auto& done) {
            POINT a{}, target{};
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Row, alpha, a) &&
                PointFor(window, PickerPlacesTestPoint::Kind::Sidebar, destination, target); }, done)) return false;
            Keyboard keyboard(window);
            if (!keyboard.Control(true) || !Drag(window, a, target, MK_CONTROL) || !keyboard.Control(false)) return false;
            return Until([&] { return fs::exists(fs::path(destination) / L"alpha.txt"); }, done) && fs::exists(alpha);
        }));
        check("Escape cancels workspace drag without filesystem operation", Run(spec, state, [&](HWND window, const auto& done) {
            POINT b{}, target{};
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Row, beta, b) &&
                PointFor(window, PickerPlacesTestPoint::Kind::Sidebar, destination, target); }, done)) return false;
            if (!Drag(window, b, target, 0, true) || !Send(window, WM_NULL)) return false;
            return IsWindow(window) && fs::exists(beta) && !fs::exists(fs::path(destination) / L"beta.txt");
        }));
        check("favorite file click locates and selects without accepting dialog", Run(spec, state, [&](HWND window, const auto& done) {
            POINT target{};
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Sidebar, favorite, target); }, done) || !Click(window, target)) return false;
            return Until([&] {
                std::lock_guard lock(state.mutex);
                return IsWindow(window) && SamePickerPath(state.dialog.folder, (fixture.root / L"elsewhere").wstring()) &&
                    (state.dialog.selection == favorite || std::find(state.dialog.selections.begin(), state.dialog.selections.end(), favorite) != state.dialog.selections.end());
            }, done);
        }));
        spec.mode = PickerMode::Save; spec.filename = L"kept-output.txt"; spec.allow_multiselect = false;
        check("save dialog favorite drag preserves typed filename", Run(spec, state, [&](HWND window, const auto& done) {
            POINT b{}, target{};
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Row, beta, b) &&
                PointFor(window, PickerPlacesTestPoint::Kind::FavoriteHeader, L"", target); }, done)) return false;
            HWND edit = GetDlgItem(window, 102);
            if (!edit || !Send(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"kept-output.txt"))) return false;
            unsigned before = 0; { std::lock_guard lock(state.mutex); before = state.calls; }
            if (!Drag(window, b, target) || !Until([&] { std::lock_guard lock(state.mutex); return state.calls > before; }, done)) return false;
            wchar_t name[256]{};
            if (!Send(edit, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(name))) return false;
            return std::wstring(name) == L"kept-output.txt" && fs::exists(beta);
        }));
    } catch (const std::exception& error) { std::printf("[FAIL] fixture or picker exception: %s\n", error.what()); ++failures; }
    CoUninitialize();
    return failures ? 1 : 0;
}

int RunPickerPlacesActionsUiTest() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    bool okay = false;
    try {
        Fixture fixture;
        const auto first = (fixture.root / L"source").wstring();
        const auto second = (fixture.root / L"destination").wstring();
        const auto member = (fixture.root / L"elsewhere").wstring();
        const std::wstring group = L"pulse:star-group:actions-test";
        State state;
        state.shared.shared_available = true;
        state.shared.starred_groups = {{L"actions-test", L"Actions test group"}};
        state.shared.starred_items = {{first, 2, {}}, {second, 2, {}}, {member, 2, L"actions-test"}};
        FolderPickerSpec spec;
        spec.pulse_painted_plain_child = true;
        spec.mode = PickerMode::File;
        spec.initial_path = first;
        okay = Run(spec, state, [&](HWND window, const auto& done) {
            struct Menus { HWND first = nullptr; unsigned count = 0; };
            const auto menus = [&] {
                Menus result;
                EnumThreadWindows(GetWindowThreadProcessId(window, nullptr), [](HWND candidate, LPARAM value) -> BOOL {
                    wchar_t name[64]{};
                    GetClassNameW(candidate, name, 64);
                    if (IsWindowVisible(candidate) && std::wstring(name) == L"PulseFluentMenu") {
                        auto& result = *reinterpret_cast<Menus*>(value);
                        if (!result.first) result.first = candidate;
                        ++result.count;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&result));
                return result;
            };
            const auto fail = [&](const char* phase) {
                const auto open = menus();
                std::printf("[DETAIL] places actions phase=%s window=%p visible_menus=%u\n", phase, window, open.count);
                std::fflush(stdout);
                if (open.first) {
                    PostMessageW(open.first, WM_KEYDOWN, VK_ESCAPE, 0);
                    PostMessageW(open.first, WM_KEYDOWN, VK_ESCAPE, 0);
                }
                return false;
            };
            const auto visible = [&](const std::wstring& path) {
                POINT point{};
                return PointFor(window, PickerPlacesTestPoint::Kind::Sidebar, path, point);
            };
            const auto menu_action = [&](const std::wstring& path, WPARAM key, bool restore) {
                POINT point{};
                const auto kind = path.empty() ? PickerPlacesTestPoint::Kind::FavoriteHeader : PickerPlacesTestPoint::Kind::Sidebar;
                if (!PointFor(window, kind, path, point) ||
                    !PostMessageW(window, WM_RBUTTONUP, 0, Position(point))) return false;
                if (!Until([&] { return menus().count == 1; }, done)) return false;
                HWND menu = menus().first;
                if (restore) {
                    if (!PostMessageW(menu, WM_KEYDOWN, 'R', 0) ||
                        !Until([&] { return menus().count == 2; }, done)) return false;
                }
                if (!PostMessageW(menu, WM_KEYDOWN, key, 0)) return false;
                return Until([&] { return menus().count == 0; }, done);
            };
            if (!Until([&] { return visible(first) && visible(second) && visible(group) && visible(member); }, done))
                return fail("initial sidebar rows");
            if (!menu_action(first, 'H', false) || !Until([&] { return !visible(first) && visible(second); }, done))
                return fail("hide first favorite");
            if (!menu_action(second, 'H', false) || !Until([&] { return !visible(first) && !visible(second); }, done))
                return fail("hide second favorite");
            if (!menu_action(L"", VK_RETURN, true) || !Until([&] { return visible(first) && !visible(second); }, done))
                return fail("restore first only via R Enter");
            if (!menu_action(L"", 'A', true) || !Until([&] { return visible(first) && visible(second); }, done))
                return fail("restore all via R A");
            if (!menu_action(group, 'H', false) || !Until([&] {
                return !visible(group) && !visible(member) && visible(first) && visible(second);
            }, done)) return fail("hide group and members");
            if (!menu_action(L"", VK_RETURN, true) || !Until([&] { return visible(group) && visible(member); }, done))
                return fail("restore group and members");
            return true;
        });
    } catch (const std::exception& error) {
        std::printf("[DETAIL] places actions fixture or picker exception: %s\n", error.what());
    }
    std::printf("[%s] picker places menus hide, restore one/all, and restore groups without orphan members\n", okay ? "PASS" : "FAIL");
    std::fflush(stdout);
    CoUninitialize();
    return okay ? 0 : 1;
}

int RunPickerFilesystemDropUiTest() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    bool okay = false;
    try {
        Fixture fixture;
        const auto source = fixture.root / L"source";
        const auto child = source / L"child";
        fs::create_directory(child);
        State state;
        FolderPickerSpec spec;
        spec.pulse_painted_plain_child = true;
        spec.mode = PickerMode::File;
        spec.initial_path = source.wstring();
        okay = Run(spec, state, [&](HWND window, const auto& done) {
            POINT alpha{}, beta{}, target{};
            if (!Until([&] {
                return PointFor(window, PickerPlacesTestPoint::Kind::Row, (source / L"alpha.txt").wstring(), alpha) &&
                    PointFor(window, PickerPlacesTestPoint::Kind::Row, child.wstring(), target);
            }, done)) {
                std::printf("[DETAIL] filesystem drop: source or child row unavailable\n"); return false;
            }
            Keyboard keyboard(window);
            if (!keyboard.Control(true) || !Drag(window, alpha, target, MK_CONTROL) || !keyboard.Control(false) ||
                !Until([&] { return fs::exists(child / L"alpha.txt"); }, done) || !fs::exists(source / L"alpha.txt")) {
                std::printf("[DETAIL] filesystem drop: Ctrl copy into child failed or removed source\n"); return false;
            }
            if (!Until([&] {
                return PointFor(window, PickerPlacesTestPoint::Kind::Row, (source / L"beta.txt").wstring(), beta) &&
                    PointFor(window, PickerPlacesTestPoint::Kind::Row, child.wstring(), target);
            }, done) || !Drag(window, beta, target, 0, true) || !Send(window, WM_NULL) ||
                fs::exists(child / L"beta.txt") || !fs::exists(source / L"beta.txt")) {
                std::printf("[DETAIL] filesystem drop: Escape did not preserve source and destination\n"); return false;
            }
            if (!keyboard.Control(true) || !Send(window, WM_KEYDOWN, 'Z') || !keyboard.Control(false) ||
                !Until([&] { return !fs::exists(child / L"alpha.txt"); }, done) ||
                !fs::exists(source / L"alpha.txt")) {
                std::printf("[DETAIL] filesystem drop: Ctrl+Z did not undo copy after cancelled drag\n"); return false;
            }
            if (!Until([&] { return PointFor(window, PickerPlacesTestPoint::Kind::Row, child.wstring(), target); }, done)) return false;
            const auto external = (fixture.root / L"elsewhere" / L"favorite.txt").wstring();
            PickerOleTestDrop incoming{external.c_str(), target};
            DWORD_PTR accepted = 0;
            if (!Send(window, kPickerOleTestMessage, 0, reinterpret_cast<LPARAM>(&incoming), &accepted) ||
                !accepted || incoming.effect != DROPEFFECT_COPY ||
                !Until([&] { return fs::exists(child / L"favorite.txt"); }, done) || !fs::exists(external)) {
                std::printf("[DETAIL] filesystem drop: registered OLE target did not accept external CF_HDROP copy\n"); return false;
            }
            return true;
        });
    } catch (const std::exception& error) {
        std::printf("[DETAIL] filesystem drop fixture or picker exception: %s\n", error.what());
    }
    std::printf("[%s] picker internal copy, cancelled drop, Ctrl+Z and external CF_HDROP copy\n", okay ? "PASS" : "FAIL");
    std::fflush(stdout);
    CoUninitialize();
    return okay ? 0 : 1;
}
