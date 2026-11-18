#include "../ui/file_operation_dialog.h"

namespace pulse::ui {
struct OperationFlightTestAccess {
    static void Animations(FileOperationWindow& w, bool enabled) { w.flight_animations_override_ = enabled; }
    static void LocateAfter(FileOperationWindow& w) { w.locate_after_ = true; }
    static void Dismiss(FileOperationWindow& w) { w.RequestDismiss(); }
    static void Cancel(FileOperationWindow& w) { w.RequestCancel(); }
    static void Sample(FileOperationWindow& w, float t) { w.flight_.Sample(t); }
    static bool Shot(FileOperationWindow& w, const wchar_t* path) { return w.flight_.SaveSnapshot(path); }
    static RECT Bounds(const FileOperationWindow& w) { return w.flight_.Bounds(); }
    static float Opacity(const FileOperationWindow& w) { return w.flight_.Opacity(); }
    static bool Active(const FileOperationWindow& w) { return w.flight_.Active(); }
    static HWND Layer(const FileOperationWindow& w) { return w.flight_.Window(); }
    static HWND Window(const FileOperationWindow& w) { return w.hwnd_; }
    static bool Closing(const FileOperationWindow& w) { return w.closing_; }
    static bool Opening(const FileOperationWindow& w) { return w.opening_; }
    static ULONGLONG Deadline(const FileOperationWindow& w) { return w.success_dismiss_at_; }
    static void FinishHide(FileOperationWindow& w) { w.FinishHideToStatus(); }
    static void FinishShow(FileOperationWindow& w) { w.FinishShowFromStatus(); }
    static constexpr UINT HideMs() { return FileOperationWindow::kHideZoomMs; }
    static constexpr UINT ShowMs() { return FileOperationWindow::kShowZoomMs; }
    static constexpr ULONGLONG CloseDelay() { return FileOperationWindow::kSuccessDismissMs; }
};
}

int RunOperationFlightProbe(pulse::AppState& s) {
    using namespace pulse;
    using Access = ui::OperationFlightTestAccess;
    std::cout << "[INFO] flight probe entered" << std::endl;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << message << std::endl;
        failures += !ok;
    };
    const auto output = std::filesystem::absolute(L"../bench_data/operation_flight_exact").lexically_normal();
    std::filesystem::create_directories(output);
    int dismisses = 0, arrivals = 0, cancels = 0, located = 0;
    auto* tab = ActiveTab(s);
    check(tab && s.compositor.Dc(), "fixture has active tab and D2D context");
    if (!tab || !s.compositor.Dc()) return 1;
    tab->current_path = output.wstring(); tab->loading = false; tab->view_mode = ui::ViewMode::Details;
    auto listing = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry entry; entry.name = L"flight-example.txt"; entry.full_path = (output/L"flight-example.txt").wstring();
    entry.attrs = FILE_ATTRIBUTE_NORMAL; listing->push_back(std::move(entry)); tab->SetSnapshot(listing);
    s.scale = 1; s.compositor.Resize(1100,720); s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1);
    std::cout << "[INFO] fixture initialized; building application VM" << std::endl;
    auto main_vm = BuildVm(s, false);
    std::cout << "[INFO] application VM ready; drawing main background" << std::endl;
    main_vm.status.task_id = 81001; main_vm.status.task_active = true;
    main_vm.status.task_text = L"正在复制 24 个文件"; main_vm.status.task_progress = 46;
    main_vm.status.hint_text.clear(); main_vm.status.hint_action_text.clear();
    const auto main_bounds = D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    s.compositor.Dc()->BeginDraw(); s.renderer.Render(main_vm, main_bounds, ui::MakeTheme(false,ui::HexColor(0x6474C7)));
    std::cout << "[INFO] main renderer returned" << std::endl;
    check(SUCCEEDED(s.compositor.Dc()->EndDraw()) && s.compositor.SaveSnapshot((output/L"main-background.png").c_str()),
        "main window task-pill background captured by application compositor");
    const auto pill = s.renderer.TaskPillRect();
    check(pill.right>pill.left, "running task pill has actual renderer geometry");
    POINT anchor{static_cast<LONG>(pill.left+60*s.scale),static_cast<LONG>(pill.top+11*s.scale)};
    ClientToScreen(s.hwnd,&anchor);
    POINT owner_origin{}; ClientToScreen(s.hwnd,&owner_origin);
    std::ofstream frames(output/L"frames.tsv");
    frames << "frame\tleft\ttop\tright\tbottom\topacity\towner_left\towner_top\n";
    ui::FileOperationCallbacks callbacks;
    callbacks.status_anchor = [&]() -> std::optional<POINT> { return anchor; };
    callbacks.dismiss = [&] { ++dismisses; };
    callbacks.reached_status = [&] { ++arrivals; };
    callbacks.cancel = [&] { ++cancels; };
    callbacks.locate_results = [&](uint64_t) { ++located; };
    ui::FileOperationWindow card;
    check(card.Create(s.hwnd, std::move(callbacks)), "real operation card created with status-pill screen anchor");
    if (!Access::Window(card)) return 1;
    card.SetTheme(false, ui::HexColor(0x6474C7));
    ops::OpStatus running;
    running.task_id = 81001; running.type = ops::OpType::Copy; running.phase = ops::OpPhase::Running;
    running.active = true; running.percent = 46; running.summary = L"复制 24 个文件";
    running.total_items = 24; running.completed_items = 11;
    running.destination_dir = L"C:\\Fixture\\Target";
    Access::Animations(card, true);
    Access::LocateAfter(card);
    card.Update(running); card.Show(false);
    RECT start{}; GetWindowRect(Access::Window(card), &start);
    check(Access::HideMs() == 340 && Access::ShowMs() == 260 && Access::CloseDelay() == 1200,
        "HTML durations are hide 340ms, show 260ms, completion delay 1200ms");
    const auto sequence = [&](const wchar_t* name) {
        for (int step = 0; step <= 20; ++step) {
            const float t = static_cast<float>(step) / 20.0f;
            Access::Sample(card, t);
            const auto file = output / (std::wstring(name) + L"-" + std::to_wstring(static_cast<int>(t * 100)) + L".png");
            check(Access::Shot(card, file.c_str()), "actual flight layer sequential frame saved");
            const auto bounds = Access::Bounds(card);
            frames << (wcscmp(name, L"hide") == 0 ? "hide" : "show") << "-" << static_cast<int>(t*100) << ".png\t"
                << bounds.left << '\t' << bounds.top << '\t' << bounds.right << '\t' << bounds.bottom << '\t'
                << Access::Opacity(card) << '\t' << owner_origin.x << '\t' << owner_origin.y << '\n';
            frames.flush();
        }
    };
    Access::Dismiss(card);
    check(dismisses == 1 && cancels == 0, "manual hide dismisses card without cancelling task");
    {
        check(Access::Closing(card) && Access::Active(card), "manual hide starts cross-window flight layer");
        sequence(L"hide");
        const auto end = Access::Bounds(card);
        check(std::abs((end.left + end.right) * 0.5f - anchor.x) <= 2 &&
            std::abs((end.top + end.bottom) * 0.5f - anchor.y) <= 2,
            "hide reaches actual status-pill screen point");
        check(std::abs(static_cast<float>(end.right-end.left)/(start.right-start.left)-0.08f) < 0.015f &&
            std::abs(Access::Opacity(card)-0.2f)<0.02f, "hide ends at scale .08 and opacity .2");
        Access::FinishHide(card);
        check(!card.IsVisible() && !Access::Active(card) && arrivals == 1, "hide completion removes overlay and reaches status once");
        card.Show(false, true);
        check(Access::Opening(card) && Access::Active(card), "status reopen starts expansion flight");
        sequence(L"show");
        Access::FinishShow(card);
        check(card.IsVisible() && !Access::Active(card), "reopen restores card and disposes overlay");
        Access::Dismiss(card); Access::Sample(card, 0.5f);
        const auto reverse_start = Access::Bounds(card);
        card.Show(false, true); Access::Sample(card, 0);
        const auto reverse = Access::Bounds(card);
        check(std::abs(reverse.left-reverse_start.left)<=2 && std::abs(reverse.top-reverse_start.top)<=2 &&
            std::abs(reverse.right-reverse_start.right)<=2, "rapid hide/reopen reverses from sampled frame without jumping");
        Access::Sample(card,1); Access::FinishShow(card);
    }
    Access::Animations(card, false);
    Access::Dismiss(card);
    check(!card.IsVisible() && !Access::Active(card), "forced reduced motion hides immediately without overlay");
    card.Show(false, true);
    check(card.IsVisible() && !Access::Active(card), "forced reduced motion reopens immediately without overlay");
    Access::Animations(card, true);
    auto done = running; done.active = false; done.phase = ops::OpPhase::Completed; done.percent = 100;
    Access::LocateAfter(card);
    const auto before = GetTickCount64(); card.Update(done);
    check(located == 0, "completion does not locate results before flight arrives");
    check(Access::Deadline(card) >= before + 1200 && Access::Deadline(card) <= GetTickCount64() + 1200 && card.IsVisible(),
        "completion keeps card visible for full 1200ms delay");
    const auto timer_deadline = GetTickCount64()+2500;
    while ((card.IsVisible() || Access::Active(card)) && GetTickCount64()<timer_deadline) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(5);
    }
    check(!card.IsVisible() && !Access::Active(card), "real timer completes delayed success flight and overlay cleanup");
    check(located == 1, "completion locates results exactly once after flight arrives");
    card.SetTheme(true, ui::HexColor(0x6474C7));
    card.Show(false); Access::Dismiss(card); Access::Sample(card,0.5f);
    check(Access::Active(card) && Access::Shot(card,(output/L"dark-flight-midpoint.png").c_str()),
        "dark-theme flight retains visible card pixels");
    Access::FinishHide(card);
    card.Hide(); running.task_id++; card.Update(running); card.Show(false);
    Access::Cancel(card);
    check(cancels == 1 && !card.IsVisible() && !Access::Active(card), "cancel closes immediately and leaves no status flight");
    card.Show(false); Access::Dismiss(card);
    const HWND layer = Access::Layer(card);
    card.Destroy();
    check(!Access::Window(card) && (!layer || !IsWindow(layer)), "dispose during flight destroys owned overlay");
    return failures ? 1 : 0;
}
