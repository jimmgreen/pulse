#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

LONG CALLBACK TraceEmbeddedPreviewException(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    void* frames[24]{};
    const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
    std::cerr << "[EXCEPTION] " << std::hex << exception->ExceptionRecord->ExceptionCode << std::dec << std::endl;
    MEMORY_BASIC_INFORMATION memory{};
    VirtualQuery(exception->ExceptionRecord->ExceptionAddress, &memory, sizeof(memory));
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), module, MAX_PATH);
    std::wcerr << L"module=" << module << L" address=" << exception->ExceptionRecord->ExceptionAddress
        << L" base=" << memory.AllocationBase << std::endl;
    for (DWORD i = 0; i < exception->ExceptionRecord->NumberParameters; ++i)
        std::cerr << "parameter=" << std::hex << exception->ExceptionRecord->ExceptionInformation[i] << std::dec << std::endl;
    for (USHORT i = 0; i < count; ++i) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[i]), &offset, symbol))
            std::cerr << symbol->Name << "+" << offset << std::endl;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace pulse::ui {
struct DetailsPreviewProbe {
    static size_t Matches(const QuickPreviewWindow& preview) { return preview.find_matches_.size(); }
    static uint64_t Generation(const QuickPreviewWindow& preview) { return preview.generation_; }
    static bool BitmapReady(const QuickPreviewWindow& preview) {
        return preview.native_kind_ == QuickPreviewWindow::NativeKind::Bitmap && preview.decoded_w_ && preview.decoded_h_;
    }
    static const TableView& Table(const QuickPreviewWindow& preview) { return preview.table_; }
    static const std::wstring& Text(const QuickPreviewWindow& preview) { return preview.preview_text_; }
    static const std::wstring& Query(const QuickPreviewWindow& preview) { return preview.find_query_; }
    static void Find(QuickPreviewWindow& preview, const wchar_t* query) {
        preview.OpenFind();
        SetWindowTextW(preview.find_edit_, query);
        preview.SyncFindFromEdit();
    }
    static void CloseFind(QuickPreviewWindow& preview) { preview.CloseFind(); }
    static Compositor& Surface(QuickPreviewWindow& preview) { return preview.compositor_; }
    static void Render(QuickPreviewWindow& preview) { preview.Render(); }
};
}

int RunImagePreviewProbe(const std::filesystem::path& source, const std::filesystem::path& output) {
    using Probe = pulse::ui::DetailsPreviewProbe;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", L"off");
    pulse::diagnostics::runtime::Initialize(output.wstring(), "image-probe");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse image preview test", WS_OVERLAPPEDWINDOW,
        60, 60, 850, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    int failures = 0;
    for (bool embedded : {false, true}) {
        const auto cold = std::filesystem::absolute(source);
        for (int attempt = 0; attempt < 2; ++attempt) {
            pulse::ui::QuickPreviewWindow preview;
            const bool initialized = embedded ? preview.InitializeEmbedded(owner, WM_APP + 189, WM_APP + 190, 0)
                : preview.Initialize(owner, WM_APP + 189, WM_APP + 190, 0);
            if (!initialized) { ++failures; continue; }
            pulse::ui::QuickPreviewItem item;
            item.path = cold.wstring(); item.name = cold.filename().wstring(); item.size = std::filesystem::file_size(cold);
            const auto start = GetTickCount64();
            if (embedded) preview.ShowEmbedded(item, RECT{10, 10, 790, 530}, false, false);
            else preview.Show(item, false, pulse::ui::WindowEffect::None, false, nullptr, false);
            while (!Probe::BitmapReady(preview) && GetTickCount64() - start < 15000) Pump(10);
            const bool ready = Probe::BitmapReady(preview);
            std::cout << (ready ? "[PASS] " : "[FAIL] ") << "image mode=" << (embedded ? "embedded" : "full")
                << " attempt=" << attempt << " elapsed_ms=" << GetTickCount64() - start << std::endl;
            failures += !ready;
            if (ready && attempt == 0) {
                auto& surface = Probe::Surface(preview);
                surface.CaptureNextFrame(); Probe::Render(preview);
                if (!CapturePresentedFrame(surface, (output / (embedded ? L"image-embedded.png" : L"image-full.png")).wstring())) ++failures;
                surface.ReleaseHeldFrame();
            }
            preview.Close(); Pump(30);
        }
    }
    DestroyWindow(owner);
    pulse::diagnostics::runtime::Shutdown();
    CoUninitialize();
    return failures ? 1 : 0;
}

int RunEmbeddedPreviewTests(const std::filesystem::path& output) {
    using Probe = pulse::ui::DetailsPreviewProbe;
    const auto exception_handler = AddVectoredExceptionHandler(1, TraceEmbeddedPreviewException);
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    std::filesystem::create_directories(output);
    const auto profile = std::filesystem::absolute(output / L"profile");
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    const auto first = std::filesystem::absolute(output / L"first.txt");
    const auto second = std::filesystem::absolute(output / L"second.txt");
    const auto rich = std::filesystem::absolute(output / L"rich.md");
    const auto csv = std::filesystem::absolute(output / L"table.csv");
    std::ofstream(csv) << "Name,Count\nneedle,12\nother,34\n";
    std::ofstream(rich) << "# Preview heading\n\nneedle **bold**\n\nsecond needle\n";
    std::ofstream(first) << "Preview fixture\nneedle one\nother text\nNEEDLE two\n";
    std::ofstream(second) << "Second preview\nreplacement only\n";
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    {
        pulse::GlobalSearchWindow window;
        window.SetContext({});
        auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
        state.dark = false; state.scale = 1; state.context.show_apps = false;
        const bool created = state.Create();
        check(created, "create live embedded-preview owner");
        if (!created) {
            std::cout << "[INFO] window=" << state.hwnd << " edit=" << state.edit
                << " error=" << GetLastError() << std::endl;
            window.Shutdown();
            return 1;
        }
        state.Prepare();
        state.Cancel();
        state.animations = false;
        SetWindowPos(state.hwnd, nullptr, 30, 30, 1100, 760, SWP_NOZORDER | SWP_SHOWWINDOW);
        state.rows.clear();
        for (const auto& file : {first, second, rich, csv}) {
            state.rows.push_back({});
            auto& row = state.rows.back();
            row.path = file.wstring(); row.name = file.filename().wstring();
            row.size = std::filesystem::file_size(file);
        }
        state.selected = 0; state.total = 4; state.stale = false; state.busy = false;
        state.content_mode = true; state.query = L"needle"; state.preview_open = true;
        state.showing_recent = false;
        ++state.layout_epoch; state.BuildLayout();
        state.preview.SetSearchQuery(state.query);
        check(Probe::Query(state.preview) == L"needle" && Probe::Matches(state.preview) == 0,
            "query can precede asynchronous preview content");
        SetFocus(state.edit);
        state.SyncPreview();
        check(GetFocus() == state.edit, "initial preview synchronization preserves search edit focus");
        auto wait_text = [&](const wchar_t* expected) {
            const auto deadline = GetTickCount64() + 10000;
            while (GetTickCount64() < deadline && Probe::Text(state.preview).find(expected) == std::wstring::npos)
                Pump(25);
            return Probe::Text(state.preview).find(expected) != std::wstring::npos;
        };
        check(state.preview.visible() && GetParent(state.preview.hwnd()) == state.hwnd,
            "preview is a visible child of search");
        check(wait_text(L"needle one") && Probe::Matches(state.preview) == 2,
            "asynchronous text payload applies case-insensitive external highlights");
        const auto loaded_generation = Probe::Generation(state.preview);
        for (int frame = 0; frame < 5; ++frame) { state.Invalidate(); Pump(30); }
        check(Probe::Generation(state.preview) == loaded_generation,
            "metadata enrichment does not reload preview on repaint");
        state.events.staged_paths = [] { return std::vector<std::wstring>{}; };
        state.tray_open = true; state.SyncPreview();
        check(!IsWindowVisible(state.preview.hwnd()), "tray overlay hides native preview surface");
        state.tray_open = false; state.SyncPreview();
        check(IsWindowVisible(state.preview.hwnd()) && Probe::Generation(state.preview) == loaded_generation,
            "closing overlay restores preview without reloading");
        state.query = L"other"; state.SyncPreview(); Pump(30);
        check(GetFocus() == state.edit, "query synchronization preserves search edit focus");
        check(Probe::Matches(state.preview) == 1, "query changes recompute current payload matches");
        state.query.clear(); state.SyncPreview();
        check(Probe::Matches(state.preview) == 0, "empty query clears highlights");
        state.query = L"needle"; state.SyncPreview();
        Probe::Find(state.preview, L"other");
        check(Probe::Matches(state.preview) == 1, "interactive find temporarily overrides external query");
        Probe::CloseFind(state.preview);
        check(Probe::Query(state.preview) == L"needle" && Probe::Matches(state.preview) == 2,
            "closing interactive find restores external query");
        for (bool dark : {false, true}) {
            state.dark = dark; state.ApplyAppearance(); state.SyncPreview();
            check(wait_text(L"needle one") && Probe::Matches(state.preview) == 2,
                "theme reset retains external query after asynchronous reload");
            Pump(60);
            state.compositor.CaptureNextFrame(); state.Paint();
            const std::wstring suffix = dark ? L"dark.png" : L"light.png";
            check(CapturePresentedFrame(state, (output / (L"owner-" + suffix)).wstring()),
                "capture presented search owner frame");
            state.compositor.ReleaseHeldFrame();
            auto& surface = Probe::Surface(state.preview);
            surface.CaptureNextFrame(); Probe::Render(state.preview);
            check(CapturePresentedFrame(surface, (output / (L"preview-" + suffix)).wstring()),
                "capture presented child preview frame including highlights");
            surface.ReleaseHeldFrame();
        }
        state.selected = 1; state.query = L"replacement"; state.SyncPreview();
        check(wait_text(L"replacement only") && Probe::Matches(state.preview) == 1 &&
            Probe::Text(state.preview).find(L"needle") == std::wstring::npos,
            "selection switch replaces text and matches without stale results");
        state.selected = 0; state.query = L"needle"; state.SyncPreview();
        state.selected = 1; state.query = L"replacement"; state.SyncPreview();
        check(wait_text(L"replacement only") && Probe::Matches(state.preview) == 1,
            "rapid selection switching retains the latest payload");
        state.selected = 2; state.query = L"needle"; state.SyncPreview();
        check(wait_text(L"Preview heading") && Probe::Matches(state.preview) == 2,
            "Markdown payload reuses rich text search highlights");
        state.selected = 3; state.SyncPreview();
        check(wait_text(L"needle") && Probe::Table(state.preview).HasData() && Probe::Matches(state.preview) == 1,
            "CSV payload uses shared table preview and highlights");
        state.content_mode = false; state.query = L"first ext:txt";
        const auto name_matches = state.FilenameHighlightRanges(L"FIRST.txt");
        check(name_matches.size() == 1 && name_matches[0].start == 0 && name_matches[0].length == 5,
            "filename highlighting reuses shared matching and excludes filter tokens");
        state.query = L"baogao"; state.search_pinyin = true;
        check(!state.FilenameHighlightRanges(L"报告.txt").empty(), "filename highlighting supports pinyin matches");
        state.search_pinyin = false;
        check(state.FilenameHighlightRanges(L"报告.txt").empty(), "filename highlighting respects disabled pinyin");
        state.query = L"first"; state.selected = 0;
        ++state.layout_epoch; state.BuildLayout(); state.SyncPreview();
        check(Probe::Query(state.preview).empty(), "filename query does not highlight unrelated file contents");
        for (bool dark : {false, true}) {
            state.dark = dark; state.ApplyAppearance(); state.SyncPreview(); Pump(60);
            state.compositor.CaptureNextFrame(); state.Paint();
            check(CapturePresentedFrame(state, (output / (dark ? L"filename-dark.png" : L"filename-light.png")).wstring()),
                "capture shared filename highlight in search results and preview title");
            state.compositor.ReleaseHeldFrame();
        }
        SetWindowPos(state.hwnd, nullptr, 0, 0, 700, 760, SWP_NOMOVE | SWP_NOZORDER);
        state.SyncPreview();
        check(!state.preview.visible(), "narrow layout closes embedded preview");
        SetWindowPos(state.hwnd, nullptr, 0, 0, 1100, 760, SWP_NOMOVE | SWP_NOZORDER);
        state.SyncPreview();
        check(state.preview.visible(), "wide layout restores embedded preview");
        state.stale = true; state.SyncPreview();
        check(!state.preview.visible(), "invalidated search results close the old preview");
        state.stale = false; state.rows.clear(); state.selected = -1; state.SyncPreview();
        check(!state.preview.visible(), "empty results retain no previous preview");
        window.Hide();
        check(!state.preview.visible(), "hiding search closes embedded preview");
        window.Shutdown();
    }
    CoUninitialize();
    RemoveVectoredExceptionHandler(exception_handler);
    return failures ? 1 : 0;
}
