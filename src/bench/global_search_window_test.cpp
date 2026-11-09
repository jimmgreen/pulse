#include <windows.h>
#include <memory>
#include <string>
#include "../app/global_search_window.h"
namespace {
bool reject_search_activation = false;
HWND search_activation_other = nullptr;
BOOL WINAPI SearchTestSetForegroundWindow(HWND window) {
    return reject_search_activation ? FALSE : ::SetForegroundWindow(window);
}
HWND WINAPI SearchTestGetForegroundWindow() {
    return reject_search_activation ? search_activation_other : ::GetForegroundWindow();
}
}
#define SetForegroundWindow SearchTestSetForegroundWindow
#define GetForegroundWindow SearchTestGetForegroundWindow
#include "../app/global_search_window.cpp"
#undef SetForegroundWindow
#undef GetForegroundWindow
#include <wincodec.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include "../common/current_user_security.h"

namespace pulse {
struct GlobalSearchWindowTestPeer {
    using State = GlobalSearchWindow::Impl;
    static State& Get(GlobalSearchWindow& window) { return *window.impl_; }
};
}

namespace pulse::index {
struct ContentRefreshTestPeer {
    static void Queue(ContentSearchClient& client, ContentSearchUpdate update) {
        std::lock_guard lock(client.updates_mu_);
        client.updates_.push_back(std::move(update));
    }
};
}

namespace {
std::vector<std::pair<std::wstring, bool>> recorded_opens;
void RecordOpen(std::wstring path, bool location) { recorded_opens.emplace_back(std::move(path), location); }

int RunActivationDeniedTests() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(L"bench_data/search-activation-denied");
    std::filesystem::create_directories(root / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"profile").c_str());
    pulse::diagnostics::runtime::Initialize((root / L"profile").wstring(), "search-activation-test");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    search_activation_other = CreateWindowExW(0, L"STATIC", L"Activation fixture", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(search_activation_other != nullptr, "create independent foreground fixture");
    {
        pulse::GlobalSearchWindow window;
        reject_search_activation = true;
        const bool shown = window.Show(nullptr, true, 1, root.wstring());
        check(shown, "activation refusal is not a search-window open failure");
        check(window.Visible(), "activation refusal keeps the search window available to click");
        reject_search_activation = false;
        auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
        check(window.Show(nullptr, true, 1, root.wstring()) && window.Visible() &&
            GetForegroundWindow() == state.hwnd && GetFocus() == state.edit,
            "subsequent permitted activation focuses the existing search edit");
        window.Hide();
        check(!window.Visible(), "explicit dismissal still hides search");
    }
    DestroyWindow(search_activation_other); search_activation_other = nullptr;
    pulse::diagnostics::runtime::Shutdown();
    CoUninitialize();
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS") << " activation-denied\n";
    return failures ? 1 : 0;
}

int RunOpenRoutingTests() {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    State state;
    state.rows.push_back({});
    state.rows.front().path = L"\\\\server\\share\\folder\\file.txt";
    state.selected = 0;
    int routed = 0;
    bool route = true, saw_directory = false, saw_location = false;
    state.events.open_path = [&](const std::wstring& path, bool directory, bool location) {
        ++routed;
        saw_directory = directory;
        saw_location = location;
        check(path == state.rows.front().path, "routing preserves the full UNC result path");
        return route;
    };
    recorded_opens.clear();
    state.Open(true, RecordOpen);
    check(routed == 1 && saw_location && !saw_directory && recorded_opens.empty(),
        "host-handled file reveal never launches the Shell fallback");
    state.rows.front().directory = true;
    state.Open(false, RecordOpen);
    check(routed == 2 && saw_directory && !saw_location && recorded_opens.empty(),
        "folder opens can be handled by Pulse directly");
    route = false;
    state.Open(true, RecordOpen);
    check(recorded_opens.size() == 1 && recorded_opens.back().second,
        "unhandled requests retain the Shell location action");
    state.rows.front().directory = false;
    state.Open(false, RecordOpen);
    check(recorded_opens.size() == 2 && !recorded_opens.back().second,
        "ordinary file opening retains its associated application action");
    state.rows.front().app = true;
    const int before_app = routed;
    state.Open(true, RecordOpen);
    check(routed == before_app && recorded_opens.size() == 3 && !recorded_opens.back().second,
        "application results launch without a folder routing callback");
    state.rows.front().app = false;
    state.stale = true;
    state.Open(false, RecordOpen);
    check(routed == before_app && recorded_opens.size() == 3,
        "stale results cannot route or launch");
    return failures ? 1 : 0;
}

int RunReopenStateTests() {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    State state;
    state.hwnd = state.edit = CreateWindowExW(0, L"EDIT", L"", WS_POPUP,
        0, 0, 100, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(state.edit != nullptr, "hidden query edit created");
    if (!state.edit) return 1;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const std::wstring folder = std::filesystem::path(executable).parent_path().wstring();
    state.context.recent_files = {{executable, false}};
    state.Changed();
    check(state.rows.size() == 1 && state.rows.front().path == executable,
          "prewarm populates its original recent context");
    state.context.recent_files = {{folder, true}};
    state.Changed(true);
    check(state.showing_recent && state.rows.size() == 1 && state.rows.front().path == folder &&
          state.rows.front().directory && state.total == 1 && state.selected == 0 && !state.busy,
          "reopen replaces prewarm rows with current recent context before presentation");
    check(state.appear_start == 0, "empty-query reopen does not replay list entrance");
    state.appear_start = pulse::ui::motion::NowMs();
    state.Changed(true);
    check(state.appear_start == 0, "rapid empty-query reopen also completes prior entrance");
    state.context.recent_files.clear();
    state.Changed(true);
    check(state.rows.empty() && state.total == 0 && state.selected == -1,
          "empty current context cannot expose cached recent rows");

    SetWindowTextW(state.edit, L"needle");
    state.showing_recent = false;
    state.rows.push_back({});
    state.rows.back().path = L"old-result";
    state.Changed();
    check(state.stale && state.rows.size() == 1 && state.rows.front().path == L"old-result" && state.busy,
          "ordinary query edits retain dimmed results while refreshing");
    const auto old_generation = state.generation;
    state.Changed(true);
    check(state.query == L"needle" && !state.showing_recent && !state.stale && state.rows.empty() &&
          state.busy && state.generation > old_generation,
          "nonempty reopen refreshes the query without exposing old results");
    check(state.selected == 0 && state.scroll == 0 && state.scroll_target == 0,
          "nonempty reopen resets stale selection and scroll");
    return failures ? 1 : 0;
}

int RunSelectionIdentityTests() {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    using pulse::index::SearchResult;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    auto results = [](std::initializer_list<const wchar_t*> names, DWORD error = 0) {
        SearchResult result;
        result.error = error;
        for (const auto* name : names) {
            pulse::index::Hit hit;
            hit.name = name;
            hit.path = L"C:\\synthetic-global-search\\" + hit.name;
            result.hits.push_back(std::move(hit));
        }
        result.total = result.hits.size();
        return result;
    };
    auto selected_path = [](const State& state) {
        return state.selected >= 0 && static_cast<size_t>(state.selected) < state.rows.size()
            ? state.rows[static_cast<size_t>(state.selected)].path : std::wstring{};
    };
    for (bool network_first : {false, true}) {
        State state;
        state.AcceptFilenames(network_first, state.generation, results({L"b.txt", L"c.txt"}));
        state.Key(VK_DOWN);
        const auto selected = selected_path(state);
        state.AcceptFilenames(!network_first, state.generation, results({L"a.txt"}));
        check(selected.ends_with(L"c.txt") && selected_path(state) == selected && state.rows.size() == 3,
              "both provider arrival orders preserve keyboard-selected path");
        recorded_opens.clear();
        state.Open(false, RecordOpen);
        state.Open(true, RecordOpen);
        check(recorded_opens == std::vector<std::pair<std::wstring, bool>>{{selected, false}, {selected, true}},
              "open and open-location dispatch the retained identity only");
    }
    {
        State state;
        auto first = results({L"same.txt", L"same.txt"});
        first.hits[1].path = L"D:\\synthetic-global-search\\same.txt";
        state.AcceptFilenames(true, state.generation, first);
        // The production mouse handler computes this displayed row from y.
        state.Message(WM_LBUTTONDOWN, 0, MAKELPARAM(30, 136 + static_cast<int>(pulse::kRow) + 10));
        const auto selected = selected_path(state);
        state.AcceptFilenames(false, state.generation, results({L"inserted.txt", L"same.txt"}));
        check(selected.starts_with(L"D:") && selected_path(state) == selected && state.rows.size() == 3,
              "same names in different directories retain mouse-selected full path");
    }
    {
        State state;
        auto original = results({L"UPPER.TXT"});
        state.AcceptFilenames(true, state.generation, original);
        state.AcceptFilenames(false, state.generation, results({L"upper.txt"}));
        check(state.rows.size() == 1 && state.selected == 0 && selected_path(state).ends_with(L"upper.txt"),
              "case-insensitive dedup retains selected Windows path identity");
    }
    {
        State state;
        SearchResult many;
        for (int i = 0; i < 20; ++i) {
            auto row = results({L"row.txt"}).hits.front();
            row.path += std::to_wstring(i);
            many.hits.push_back(std::move(row));
        }
        many.total = many.hits.size();
        state.AcceptFilenames(true, state.generation, many);
        state.first = 5; state.selected = 7;
        const auto anchor = state.rows[5].path;
        const auto selection = selected_path(state);
        state.AcceptFilenames(false, state.generation, results({L"before.txt"}));
        check(state.rows[static_cast<size_t>(state.first)].path == anchor && selected_path(state) == selection,
              "provider insertion preserves scroll anchor without forcing selection into view");
    }
    {
        State state;
        state.AcceptFilenames(true, state.generation, results({L"evicted.txt"}));
        SearchResult full;
        for (size_t i = 0; i < pulse::kMaximumResults; ++i) {
            auto row = results({L"local.txt"}).hits.front();
            row.path += std::to_wstring(i);
            full.hits.push_back(std::move(row));
        }
        full.total = full.hits.size();
        state.AcceptFilenames(false, state.generation, std::move(full));
        recorded_opens.clear();
        state.Open(false, RecordOpen); state.Open(true, RecordOpen);
        check(state.selected == -1 && recorded_opens.empty() && state.truncated,
              "cap-evicted selection cannot silently open a replacement result");
        state.Key(VK_RETURN);
        check(state.selected == -1, "Enter remains inert until user reselects after eviction");
        state.Key(VK_DOWN);
        state.Open(false, RecordOpen);
        check(recorded_opens.size() == 1 && recorded_opens.front().first == state.rows.front().path,
              "explicit keyboard movement establishes a new actionable selection");
    }
    {
        State state;
        state.AcceptFilenames(false, state.generation, results({L"first.txt"}));
        const auto old_generation = state.generation;
        state.Cancel();
        state.AcceptFilenames(true, old_generation, results({L"stale.txt"}));
        check(state.rows.size() == 1 && state.rows.front().name == L"first.txt",
              "cancelled generation cannot replace filename selection");
        state.Changed();
        state.AcceptFilenames(false, state.generation, results({L"fresh.txt"}));
        check(state.selected == 0 && selected_path(state).ends_with(L"fresh.txt"),
              "new query resets selection to first new result");
    }
    for (bool network_first : {false, true}) {
        State state;
        state.providers.ExpectNetwork(false);
        const auto local = results({L"usable.txt"});
        const auto failure = results({}, ERROR_CONNECTION_ABORTED);
        if (network_first) {
            state.AcceptFilenames(true, state.generation, failure);
            state.AcceptFilenames(false, state.generation, local);
        } else {
            state.AcceptFilenames(false, state.generation, local);
            state.AcceptFilenames(true, state.generation, failure);
        }
        check(!state.busy && state.error.empty() && state.rows.size() == 1u && !state.network_result.error,
              "no network roots: unreachable agent (1236) leaves a complete local result");
    }
    for (bool local_has_hit : {false, true}) for (DWORD error : {DWORD{ERROR_CONNECTION_ABORTED}, DWORD{ERROR_INVALID_DATA}})
        for (bool network_first : {false, true}) {
            State state;
            const auto local = local_has_hit ? results({L"usable.txt"}) : results({});
            const auto failure = results({}, error);
            if (network_first) {
                state.AcceptFilenames(true, state.generation, failure);
                state.AcceptFilenames(false, state.generation, local);
            } else {
                state.AcceptFilenames(false, state.generation, local);
                state.AcceptFilenames(true, state.generation, failure);
            }
            const auto expected = pulse::Text(local_has_hit ? pulse::l10n::StringId::SearchIncomplete
                                                          : pulse::l10n::StringId::GlobalSearchFailed);
            check(!state.busy && !state.error.empty() && state.error == expected &&
                  state.network_result.error == error && state.rows.size() == (local_has_hit ? 1u : 0u),
                  "provider failure preserves usable rows and distinguishes partial from empty error");
            state.AcceptFilenames(false, state.generation, local);
            check(state.error == expected, "unrelated provider success cannot erase network failure");
            state.AcceptFilenames(true, state.generation, results({}));
            check(state.error.empty() && !state.busy, "successful retry clears only its provider error");
        }
    {
        State state;
        state.busy = true;
        state.Message(WM_TIMER, pulse::kConnectTimeout, 0);
        check(!state.busy && state.local_result.error == ERROR_TIMEOUT && state.network_result.error == ERROR_TIMEOUT &&
              !state.error.empty(), "timeout marks every pending provider failed");
        state.AcceptFilenames(false, state.generation, results({L"late.txt"}));
        check(state.rows.size() == 1 && state.network_result.error == ERROR_TIMEOUT &&
              state.error == pulse::Text(pulse::l10n::StringId::SearchIncomplete),
              "one late success retains the other provider timeout and partial status");
        state.AcceptFilenames(true, state.generation, results({}));
        check(!state.busy && state.error.empty(), "both late successes recover timeout state");
    }
    {
        State state;
        state.AcceptFilenames(false, state.generation, results({}, ERROR_INVALID_DATA));
        state.AcceptFilenames(true, state.generation, results({L"network.txt"}));
        check(state.local_result.error == ERROR_INVALID_DATA && state.rows.size() == 1 &&
              state.error == pulse::Text(pulse::l10n::StringId::SearchIncomplete),
              "network success preserves independent local provider failure");
    }
    {
        State state;
        state.AcceptFilenames(false, state.generation, results({}));
        state.AcceptFilenames(true, state.generation, results({}));
        check(state.rows.empty() && state.error.empty() && !state.busy,
              "two successful empty providers remain genuine no-results state");
    }
    return failures ? 1 : 0;
}

void Pump(DWORD milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(10);
    } while (GetTickCount64() < end);
}
bool CapturePresentedFrame(pulse::ui::Compositor& compositor, const std::wstring& path) {
    auto* source = compositor.HeldFrame();
    if (!source) return false;
    const auto size = source->GetPixelSize();
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> readback;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        source->GetPixelFormat());
    HRESULT result = compositor.Dc()->CreateBitmap(size, nullptr, 0, &properties, &readback);
    if (SUCCEEDED(result)) result = readback->CopyFromBitmap(nullptr, source, nullptr);
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(result) || FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
        mapped.pitch, mapped.pitch * size.height, mapped.bits, &bitmap);
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    readback->Unmap();
    return SUCCEEDED(result);
}
bool CapturePresentedFrame(pulse::GlobalSearchWindowTestPeer::State& state, const std::wstring& path) {
    return CapturePresentedFrame(state.compositor, path);
}
bool Capture(pulse::GlobalSearchWindowTestPeer::State& state, const std::wstring& path) {
    Pump(30);
    const UINT width = static_cast<UINT>(state.Px(state.width)), height = static_cast<UINT>(state.Px(state.height));
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> image;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &image);
    state.EnsureTarget();
    const auto live_target = state.target;
    const auto live_brush = state.brush;
    state.target.Reset(); state.brush.Reset();
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    properties.dpiX = properties.dpiY = 96 * state.scale;
    if (SUCCEEDED(result)) result = state.factory->CreateWicBitmapRenderTarget(image.Get(), properties, &state.target);
    if (SUCCEEDED(result)) result = state.target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &state.brush);
    if (SUCCEEDED(result)) {
        state.Paint();
        RECT edit_rect{}; GetClientRect(state.edit, &edit_rect);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = edit_rect.right; info.bmiHeader.biHeight = -edit_rect.bottom;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HDC memory = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const auto old = SelectObject(memory, bitmap);
        FillRect(memory, &edit_rect, state.background);
        if (!state.compositor.PaintLumaEdit(state.edit, memory, state.edit_format.Get(), state.Theme().text, state.EditBackground()))
            SendMessageW(state.edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        if (state.EditBackground().a == 0 && static_cast<const BYTE*>(pixels)[
            (static_cast<size_t>(edit_rect.bottom / 2) * edit_rect.right + edit_rect.right - 2) * 4 + 3] != 0) {
            std::cout << "[FAIL] Search edit background is not transparent\n";
            result = E_FAIL;
        }
        SelectObject(memory, old);
        Microsoft::WRL::ComPtr<IWICBitmap> edit_image;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> edit_bitmap;
        if (SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &edit_image)) &&
            SUCCEEDED(state.target->CreateBitmapFromWicBitmap(edit_image.Get(), nullptr, &edit_bitmap))) {
            state.target->BeginDraw();
            state.target->DrawBitmap(edit_bitmap.Get(), {116, 23, state.width - 72, 58});
            const HRESULT painted_edit = state.target->EndDraw();
            if (SUCCEEDED(result)) result = painted_edit;
        }
        DeleteObject(bitmap); DeleteDC(memory);
    }
    state.target = live_target; state.brush = live_brush;
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result = frame->WriteSource(image.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    if (FAILED(result)) std::cout << "Capture failure HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
    return SUCCEEDED(result);
}

int RunProviderStatusRender(const std::filesystem::path& parent) {
    using State = pulse::GlobalSearchWindowTestPeer::State;
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto output = parent / (L"provider-status-" + std::to_wstring(GetCurrentProcessId()) +
                                  L"-" + std::to_wstring(GetTickCount64()));
    std::error_code directory_error;
    std::filesystem::create_directories(parent, directory_error);
    if (directory_error || !std::filesystem::create_directory(output, directory_error)) {
        CoUninitialize();
        return 2;
    }
    std::wcout << L"[ARTIFACT] " << output.wstring() << L'\n';
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    for (bool dark : {false, true}) for (float scale : {1.0f, 1.5f}) {
        State state;
        state.dark = dark;
        state.scale = scale;
        // A private hidden HWND supplies the real compositor target. Never call
        // GlobalSearchWindow::Show, which would start the provider clients.
        state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"STATIC",
            L"Pulse provider-status render fixture", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
            state.Px(780), state.Px(488), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (state.hwnd) state.edit = pulse::ui::CreateChildEdit(state.hwnd);
        const bool ready = state.hwnd && state.edit && state.EnsureTarget();
        check(ready, "private hidden window initializes real compositor without providers");
        if (!ready) continue;
        SetWindowTheme(state.edit, L"", L"");
        SetLayeredWindowAttributes(state.edit, 0, 255, LWA_ALPHA);
        ShowWindow(state.edit, SW_SHOW);
        SetWindowTextW(state.edit, L"report");
        state.query = L"report";
        state.Layout();
        state.ApplyAppearance();
        auto text_fits = [&](float width, float height, float size) {
            Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            HRESULT result = state.write_factory->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size * pulse::ui::typography::UiFontScale(), pulse::l10n::LocaleName(), &format);
            if (SUCCEEDED(result)) {
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                result = state.write_factory->CreateTextLayout(state.error.c_str(),
                    static_cast<UINT32>(state.error.size()), format.Get(), width, height, &layout);
            }
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(result)) result = layout->GetMetrics(&metrics);
            return SUCCEEDED(result) && metrics.widthIncludingTrailingWhitespace <= width && metrics.height <= height;
        };
        for (bool partial : {true, false}) {
            pulse::index::SearchResult local, network;
            if (partial) {
                pulse::index::Hit hit;
                hit.name = L"report.txt";
                hit.path = (output / L"synthetic-report.txt").wstring();
                local.hits.push_back(std::move(hit));
                local.total = 1;
            }
            network.error = ERROR_CONNECTION_ABORTED;
            state.AcceptFilenames(false, state.generation, std::move(local));
            state.AcceptFilenames(true, state.generation, std::move(network));
            const std::wstring name = std::wstring(partial ? L"partial-" : L"empty-error-") +
                (dark ? L"dark-" : L"light-") + (scale == 1.0f ? L"100.png" : L"150.png");
            check(!state.busy && !state.error.empty() && state.rows.size() == (partial ? 1u : 0u),
                  "real merge supplies requested provider error rendering state");
            check(partial ? state.FooterStatus() == pulse::Text(pulse::l10n::StringId::SearchIncomplete)
                          : state.FooterStatus().empty(),
                  "production footer shows partial status and leaves empty error to the center");
            check(partial ? text_fits(130, 32, 12) : text_fits(state.width - 32, state.height - pulse::kFooter - 144, 16),
                  "partial footer or empty-state error text fits its production bounds");
            check(Capture(state, (output / name).wstring()), "capture provider error with production Paint");
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
}
int RunLiveRecycleSearch(const std::wstring& name) {
    const auto utf8 = [](const std::wstring& value) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), bytes, nullptr, nullptr);
        return text;
    };
    wchar_t original[32768]{}, report[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_PATH", original, 32768);
    GetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_REPORT", report, 32768);
    if (!*original || !*report) return 2;
    std::ofstream log{std::filesystem::path(report)};
    if (!log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    {
        pulse::GlobalSearchWindow window;
        pulse::GlobalSearchHandoff handed;
        bool handoff = false;
        window.SetHandoffHandler([&](const auto& request) { handed = request; handoff = true; });
        check(window.Show(nullptr, true, 1, L""), "show real global search popup");
        auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
        // The parent intentionally starts helpers hidden. A second explicit
        // show overrides STARTUPINFO's first-ShowWindow visibility setting.
        ShowWindow(state.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(state.hwnd);
        SetFocus(state.edit);
        check(IsWindowVisible(state.hwnd) != FALSE, "live search fixture is visible despite hidden process startup");
        SetWindowTextW(state.edit, name.c_str());
        state.Changed();
        int matched = -1;
        unsigned recycle_messages = 0, debounce_messages = 0, deactivated_messages = 0;
        const auto deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < deadline && matched < 0) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.hwnd == state.hwnd && message.message == pulse::kRecycleResult) ++recycle_messages;
                if (message.hwnd == state.hwnd && message.message == WM_TIMER && message.wParam == pulse::kDebounce) ++debounce_messages;
                if (message.hwnd == state.hwnd && message.message == WM_ACTIVATEAPP && !message.wParam) ++deactivated_messages;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            for (size_t i = 0; i < state.rows.size(); ++i) {
                const auto& row = state.rows[i];
                if (row.name == name && !row.recycle_path.empty() &&
                    pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(row.path),
                        pulse::path::StripExtendedPathPrefix(original))) matched = static_cast<int>(i);
            }
            if (matched < 0) Sleep(20);
        }
        check(matched >= 0, "real current-user recycled filename appears in global results");
        wchar_t edit_text[32768]{};
        GetWindowTextW(state.edit, edit_text, 32768);
        log << "[DIAG] sid=" << utf8(pulse::CurrentUserSidString()) << " query=" << utf8(state.query)
            << " edit=" << utf8(edit_text) << " scope=" << utf8(state.ScopePath())
            << " generation=" << state.generation << " visible=" << window.Visible()
            << " composing=" << state.composing << " content=" << state.content_mode
            << " recycle_ready=" << state.recycle_ready << " recycle_error=" << state.recycle_result.error
            << " recycle_hits=" << state.recycle_result.hits.size() << " rows=" << state.rows.size()
            << " local_ready=" << state.providers.local_ready << " network_ready=" << state.providers.network_ready
            << " provider_error=" << state.providers.Error() << " recycle_messages=" << recycle_messages
            << " debounce_messages=" << debounce_messages << " deactivate_messages=" << deactivated_messages << std::endl;
        if (matched < 0) {
            pulse::index::Query direct_query;
            direct_query.needle = name;
            const auto direct = pulse::app::SearchRecycleBin(direct_query);
            size_t named = 0;
            for (const auto& hit : direct.hits) if (hit.name == name) ++named;
            log << "[DIAG] direct_error=" << direct.error << " direct_hits=" << direct.hits.size()
                << " direct_named=" << named << std::endl;
        }
        if (matched >= 0) {
            state.selected = matched;
            state.ClampSelection(); state.Paint();
            check(state.rows[matched].name == name && !state.rows[matched].recycle_path.empty(),
                "display row retains original label and recycle identity");
            auto screenshot = std::filesystem::path(report); screenshot.replace_extension(L".png");
            check(Capture(state, screenshot.wstring()), "capture visible recycle search result");
            recorded_opens.clear();
            state.Open(false, RecordOpen);
            check(handoff && handed.folder == L"pulse:recycle" && !handed.content &&
                handed.query == L"\"" + name + L"\"", "opening hands off exact filename into recycle scope");
            check(recorded_opens.empty(), "opening never invokes original-path shell action");
        }
        window.Shutdown();
    }
    CoUninitialize();
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

#include "global_search_embedded_preview_test.inc"
#include "global_search_mockup_flow_test.inc"
#include "global_search_header_test.inc"

int wmain(int argc, wchar_t** argv) {
    if (argc > 3 && std::wstring_view(argv[1]) == L"--image-preview")
        return RunImagePreviewProbe(argv[2], argv[3]);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--embedded-preview")
        return RunEmbeddedPreviewTests(argc > 2 ? argv[2] : L"bench_data/search-embedded-preview");
    if (argc > 1 && std::wstring_view(argv[1]) == L"--shared-header-offscreen")
        return RunSharedSearchHeaderTests(argc > 2 ? argv[2] : L"bench_data/search-shared-header");
    if (argc > 1 && std::wstring_view(argv[1]) == L"--mockup-flow")
        return RunMockupFlow(argc > 2 ? argv[2] : L"bench_data/search-mockup-flow", argc > 3);
    wchar_t live_name[1024]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_NAME", live_name, 1024))
        return RunLiveRecycleSearch(live_name);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--activation-denied") return RunActivationDeniedTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--open-routing") return RunOpenRoutingTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--selection-identity") return RunSelectionIdentityTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--reopen-state") return RunReopenStateTests();
    if (argc > 1 && std::wstring_view(argv[1]) == L"--provider-status-render")
        return RunProviderStatusRender(argc > 2 ? argv[2] : L"bench_data/global-search-provider-status");
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool cancel_only = argc > 1 && std::wstring_view(argv[1]) == L"--cancel-only";
    std::filesystem::path output = cancel_only ? L"bench_data/global-search-cancel" :
        (argc > 1 ? argv[1] : L"bench_data/global-search-window");
    std::filesystem::create_directories(output);
    const auto profile = output / L"profile";
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    int failed = 0;
    const auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!ok) ++failed; };
    pulse::GlobalSearchWindow window;
    check(window.Show(nullptr, true, 1, output.wstring()), "Show creates independent visible window");
    auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
    if (cancel_only) {
        SetActiveWindow(nullptr);
        std::cout << "[INFO] null foreground fixture=" << (GetForegroundWindow() == nullptr) << '\n';
        window.Hide();
        check(window.Show(nullptr, true, 1, output.wstring()), "Summon after local deactivation");
        check(window.Visible() && GetForegroundWindow() == state.hwnd && GetFocus() == state.edit,
            "Search owns foreground and edit focus before accepting Escape");
        for (int cycle = 0; cycle < 8; ++cycle) {
            SendMessageW(state.edit, WM_KEYDOWN, VK_ESCAPE, 0);
            Pump(20);
            check(!window.Visible(), "Native edit Escape hides search without reopening");
            check(window.Show(nullptr, true, 1, output.wstring()) && GetForegroundWindow() == state.hwnd &&
                GetFocus() == state.edit, "Repeated summon retains keyboard ownership");
        }
        window.Hide();
        pulse::GlobalSearchContext refreshed;
        refreshed.show_apps = false;
        refreshed.recent_files.push_back({std::filesystem::absolute(output).wstring(), true});
        window.SetContext(std::move(refreshed));
        state.compositor.CaptureNextFrame();
        check(window.Show(nullptr, true, 1, output.wstring()), "Show refreshed recent context");
        check(state.rows.size() == 1 && state.rows.front().path == std::filesystem::absolute(output).wstring() &&
            state.appear_start == 0, "Current recent content is ready without entrance animation at Show return");
        check(CapturePresentedFrame(state, (output / L"reopen-first-frame.png").wstring()), "Capture first submitted frame without pumping messages");
        state.compositor.ReleaseHeldFrame();
        state.Cancel();
        const auto files = output / L"files";
        std::filesystem::create_directories(files);
        std::ofstream(files / L"needle.txt") << "pulse-global-search-cancellation-fixture";
        state.content_mode = true;
        state.current_only = true;
        state.current_folder = files.wstring();
        pulse::index::ContentIndexConfig config;
        config.roots = {{files.wstring()}};
        state.contents.Resume();
        state.contents.Configure(config);
        const auto deadline = GetTickCount64() + 5000;
        while (state.contents.GetConfig().roots.empty() && GetTickCount64() < deadline) Pump(30);
        SetWindowTextW(state.edit, L"pulse-global-search-cancellation-fixture");
        state.Search();
        check(state.contents.CurrentGeneration() == state.generation, "Content query dispatched");
        const auto generation = state.generation;
        const auto started = GetTickCount64();
        state.Key(VK_ESCAPE);
        check(!window.Visible() && state.generation != generation && state.contents.CurrentGeneration() == 0,
            "Escape hides and cancels content query");
        check(GetTickCount64() - started < 250, "Escape does not wait for worker");
        Pump(200);
        check(!window.Visible() && !state.busy, "Late completion does not reopen search");
        check(window.Show(nullptr, true, 1, files.wstring()), "Search can reopen after cancellation");
        state.Cancel();
        window.Shutdown();
        CoUninitialize();
        return failed ? 1 : 0;
    }
    check(window.Visible() && GetFocus() == state.edit, "Show focuses native IME edit");
    check(GetWindow(state.hwnd, GW_OWNER) == nullptr, "Hidden main owner cannot hide popup");
    check((GetWindowLongPtrW(state.hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0,
        "Popup uses the main window no-redirection composition style");
    state.Cancel();
    check(Capture(state, (output / L"empty-dark.png").wstring()), "Capture empty dark");
    check(state.empty_art.Get() != nullptr, "Existing empty-state SVG resource renders successfully");

    SetWindowTextW(state.edit, L"季度预算");
    state.Cancel(); state.content_mode = true;
    state.rows = {
        {L"2026 年第三季度预算.xlsx", L"D:\\工作\\财务\\2026 年第三季度预算.xlsx", L"本次季度预算总额为 128 万元，主要用于产品研发…", false},
        {L"项目预算评审.pdf", L"D:\\工作\\项目资料\\项目预算评审.pdf", L"季度预算调整方案已通过评审，将于下周执行。", false},
        {L"预算会议纪要.docx", L"C:\\Users\\Example\\Documents\\预算会议纪要.docx", L"会议确认季度预算与部门年度计划保持一致。", false}
    };
    state.total = state.rows.size();
    check(Capture(state, (output / L"content-dark.png").wstring()), "Capture content dark");
    state.Key(VK_DOWN); check(state.selected == 1, "Arrow down selects next row");
    state.Key(VK_UP); check(state.selected == 0, "Arrow up selects previous row");
    state.composing = true;
    check(!state.Key(VK_RETURN) && window.Visible(), "IME Enter is not intercepted");
    state.composing = false;
    const auto rows = state.rows;
    window.Show(nullptr, false, 1.5f, output.wstring()); state.Cancel();
    state.scale = 1.5f;
    SetWindowPos(state.hwnd, nullptr, 0, 0, state.Px(780), state.Px(488), SWP_NOMOVE | SWP_NOZORDER);
    state.Layout();
    state.rows = rows; state.total = rows.size(); state.content_mode = true;
    check(Capture(state, (output / L"content-light-150.png").wstring()), "Capture content light 150 percent DPI");
    state.content_mode = false;
    for (int i = 0; i < 14; ++i) state.rows.push_back({L"预算文档 " + std::to_wstring(i) + L".txt", L"D:\\工作\\财务\\预算文档.txt", L"", false});
    state.total = state.rows.size(); state.selected = static_cast<int>(state.rows.size()) - 1; state.ClampSelection();
    check(state.first > 0 && state.selected < state.first + state.PageSize(), "Keyboard selection scrolls into viewport");
    check(Capture(state, (output / L"filename-light-150.png").wstring()), "Capture filename scrolling");
    state.content_mode = true; state.selected = state.first = 0;
    SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    const int scrolled_first = state.first;
    check(scrolled_first > 0, "Mouse wheel scrolls away from selected first result");
    for (int phase = 0; phase < 3; ++phase) {
        pulse::index::ContentSearchUpdate update;
        update.progress.generation = state.generation;
        update.progress.done = phase == 2;
        if (phase == 1) {
            pulse::index::ContentHit hit;
            hit.name = L"appended.txt"; hit.path = L"D:\\appended.txt";
            update.hits.push_back(std::move(hit));
        }
        pulse::index::ContentRefreshTestPeer::Queue(state.contents, std::move(update));
        SendMessageW(state.hwnd, pulse::kContentResult, 0, 0);
        check(state.first == scrolled_first && state.selected == 0,
            "Content progress, appended hits and completion preserve mouse scroll");
    }
    state.Key(VK_DOWN);
    check(state.selected >= state.first && state.selected < state.first + state.PageSize(),
        "Keyboard navigation still reveals selection after mouse scrolling");
    state.rows.clear(); state.total = 0; state.busy = true;
    check(Capture(state, (output / L"loading-light-150.png").wstring()), "Capture loading");
    state.busy = false;
    check(Capture(state, (output / L"no-results-light-150.png").wstring()), "Capture no results");
    state.error = pulse::l10n::Get(pulse::l10n::StringId::GlobalSearchFailed);
    check(Capture(state, (output / L"error-light-150.png").wstring()), "Capture error");
    state.error.clear();
    const auto test_accent = D2D1::ColorF(0xc08040);
    for (const auto effect : {pulse::ui::WindowEffect::None, pulse::ui::WindowEffect::Mica,
        pulse::ui::WindowEffect::MicaAlt, pulse::ui::WindowEffect::Acrylic}) {
        window.SetAppearance(true, effect, L"", test_accent);
        for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
        check(state.target.Get() == state.compositor.Dc() && !state.compositor.NeedsRecovery() &&
            state.effect == effect && state.Theme().accent.r == test_accent.r,
            "Shared compositor renders selected material and accent");
        DWORD actual_backdrop = 0;
        const DWORD expected_backdrop = effect == pulse::ui::WindowEffect::None ? 1 :
            effect == pulse::ui::WindowEffect::Mica ? 2 : effect == pulse::ui::WindowEffect::Acrylic ? 3 : 4;
        check(SUCCEEDED(DwmGetWindowAttribute(state.hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
            &actual_backdrop, sizeof(actual_backdrop))) && actual_backdrop == expected_backdrop,
            "DWM window has the requested system backdrop type");
        check(state.compositor.UsesTransparentComposition(), "Popup composition preserves alpha");
        Microsoft::WRL::ComPtr<ID2D1Image> source;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> source_bitmap, sample;
        auto* dc = state.compositor.Dc();
        dc->GetTarget(&source);
        HRESULT sampled = source.As(&source_bitmap);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(sampled)) sampled = dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample);
        const D2D1_RECT_U area{15, 300, 16, 301};
        if (SUCCEEDED(sampled)) sampled = sample->CopyFromBitmap(nullptr, source_bitmap.Get(), &area);
        D2D1_MAPPED_RECT pixel{};
        if (SUCCEEDED(sampled)) sampled = sample->Map(D2D1_MAP_OPTIONS_READ, &pixel);
        int alpha = -1;
        if (SUCCEEDED(sampled)) { alpha = pixel.bits[3]; sample->Unmap(); }
        check(effect == pulse::ui::WindowEffect::None ? alpha == 255 : alpha > 0 && alpha < 255,
            "Body stays translucent for DWM material and opaque for None");
    }
    const auto wallpaper = (output / L"empty-dark.png").wstring();
    window.SetAppearance(false, pulse::ui::WindowEffect::MicaAlt, wallpaper, test_accent);
    const auto image_deadline = GetTickCount64() + 5000;
    while (!state.material.SourceBitmap(wallpaper) && GetTickCount64() < image_deadline) Pump(30);
    for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
    check(state.material.SourceBitmap(wallpaper) != nullptr && !state.backdrop,
        "Custom background uses shared async material instead of DWM");
    check(state.compositor.SaveSnapshot((output / L"material-custom.png").c_str()), "Capture shared custom background material");
    window.SetAppearance(false, pulse::ui::WindowEffect::None, wallpaper, test_accent);
    for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
    check(state.compositor.SaveSnapshot((output / L"material-image.png").c_str()), "Capture image background without effect");
    window.SetAppearance(false, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
    state.local_result.hits = {{L"C:\\fixture\\same.txt", L"same.txt", false}};
    state.local_result.total = 1;
    state.network_result.hits = {{L"c:\\FIXTURE\\SAME.txt", L"SAME.txt", false}, {L"\\\\server\\share\\network.txt", L"network.txt", false}};
    state.network_result.total = 2; state.providers.local_ready = state.providers.network_ready = true;
    state.MergeFilenames();
    check(state.rows.size() == 2 && state.total == 2 && !state.busy, "Local and network merge deduplicates Windows paths");
    state.error.clear(); state.content_mode = true; state.current_only = true;
    const auto files = output / L"files";
    std::filesystem::create_directories(files);
    std::ofstream(files / L"needle.txt") << "A real global-search-fixture-needle text document.";
    state.current_folder = files.wstring();
    pulse::index::ContentIndexConfig config; config.roots = {{files.wstring()}};
    state.contents.Resume(); state.contents.Configure(config);
    const auto config_deadline = GetTickCount64() + 5000;
    while (state.contents.GetConfig().roots.empty() && GetTickCount64() < config_deadline) Pump(30);
    SetWindowTextW(state.edit, L"global-search-fixture-needle");
    state.Search();
    const auto query_deadline = GetTickCount64() + 15000;
    while (state.busy && GetTickCount64() < query_deadline) Pump(30);
    check(!state.busy && state.rows.size() == 1 && state.rows[0].name == L"needle.txt" &&
        state.rows[0].snippet.find(L"global-search-fixture-needle") != std::wstring::npos, "Real Instant content query delivers hit and snippet without result store");
    check(Capture(state, (output / L"real-content.png").wstring()), "Capture real content result");
    SetWindowTextW(state.edit, L"pulse-global-search-cancellation-fixture");
    state.Search();
    check(state.contents.CurrentGeneration() == state.generation, "Content query dispatched to independent async client");
    const auto generation = state.generation;
    const auto started = GetTickCount64();
    state.Key(VK_ESCAPE);
    check(!window.Visible() && state.generation != generation && state.contents.CurrentGeneration() == 0, "Escape hides and invalidates/cancels content generation");
    check(GetTickCount64() - started < 250, "Hide does not join search worker");
    check(window.Show(nullptr, true, 1, output.wstring()), "Show resumes after cancellation");
    state.Cancel(); window.Shutdown();
    check(!window.Visible(), "Shutdown releases window");
    CoUninitialize();
    return failed ? 1 : 0;
}
