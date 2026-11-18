int RunOperationSkipTests() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::SetLanguage(L"zh-CN");
    const auto root = std::filesystem::path(SandboxRoot()).parent_path() /
        (L"skip_item_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    diagnostics::runtime::Initialize(root.wstring(), "ops-skip-test");
    {
        ops::OperationItemControl control;
        std::atomic<bool> cancel{false};
        auto first = control.Begin(1, cancel);
        Check(!control.Skip(2, first->token), L"skip rejects another task");
        Check(control.Skip(1, first->token) && first->cancel.load(), L"skip cancels exactly the bound item");
        Check(!control.Skip(1, first->token), L"duplicate skip is rejected");
        Check(control.Finish(first), L"item reports explicit skip");
        auto next = control.Begin(1, cancel);
        Check(!control.Skip(1, first->token) && !next->cancel.load(), L"stale click never skips the next item");
        control.Cancel();
        Check(next->cancel.load() && !control.Finish(next), L"whole-task cancellation is distinct from skip");
    }
    std::atomic<bool> requested{false}, accepted{false}, pause_first{false};
    std::atomic<uint64_t> last_token{0}, last_task{0};
    g_ops.Start([&] {
        const auto status = g_ops.Status();
        if (status.current_item_token && !requested.exchange(true)) {
            last_token = status.current_item_token;
            last_task = status.task_id;
            if (pause_first.load()) g_ops.PauseCurrent();
            else accepted = g_ops.SkipCurrentItem(status.task_id, status.current_item_token);
        }
    });
    for (auto type : {ops::OpType::Copy, ops::OpType::Move, ops::OpType::RealDelete, ops::OpType::RecycleDelete}) {
        const auto folder = root / std::to_wstring(static_cast<int>(type));
        const auto source = folder / L"src", destination = folder / L"dst";
        std::filesystem::create_directories(source); std::filesystem::create_directories(destination);
        const auto skipped = source / L"skip.bin", follow = source / L"follow.bin";
        Check(MakePatternFile(skipped.wstring(), 8ull << 20) && MakeFile(follow.wstring(), "follow", 6),
            L"create isolated skip and following-item fixtures");
        const uint64_t original = FileHash(skipped.wstring());
        if (type == ops::OpType::Copy || type == ops::OpType::Move)
            Check(MakeFile((destination / L"skip.bin").wstring(), "original", 8), L"create replacement target");
        const auto old_target = (type == ops::OpType::Copy || type == ops::OpType::Move)
            ? FileHash((destination / L"skip.bin").wstring()) : 0;
        requested = false; accepted = false;
        ops::OpRequest request;
        request.type = type; request.sources = {skipped.wstring(), follow.wstring()};
        request.dest_dir = destination.wstring(); request.collision_policy = ops::CollisionPolicy::Replace;
        const auto previous = g_ops.Status().completed_ops;
        const auto task = g_ops.Submit(std::move(request));
        Check(WaitOpDone(previous, 30000), L"skip permits task to finish without cancellation");
        const auto status = g_ops.Status();
        Check(requested && accepted && status.phase == ops::OpPhase::Completed && status.last_error.empty(),
            L"copy move permanent-delete and recycle-delete accept item skip");
        Check(std::filesystem::exists(skipped) && FileHash(skipped.wstring()) == original,
            L"skipped source retains original content");
        if (type == ops::OpType::Copy || type == ops::OpType::Move) {
            Check(FileHash((destination / L"skip.bin").wstring()) == old_target,
                L"skip preserves the original replacement target");
            Check(std::filesystem::exists(destination / L"follow.bin"), L"next file transfers normally");
            bool temporary = false;
            for (const auto& item : std::filesystem::directory_iterator(destination))
                temporary |= item.path().filename().wstring().find(L".pulse-") != std::wstring::npos;
            Check(!temporary, L"skip leaves no unfinished copy or replacement backup");
        } else {
            Check(!std::filesystem::exists(follow), L"next file deletes normally");
        }
        Check(!g_ops.SkipCurrentItem(task, last_token.load()), L"finished item rejects delayed skip");
    }
    {
        const auto source = root / L"paused-src", destination = root / L"paused-dst";
        std::filesystem::create_directories(source); std::filesystem::create_directories(destination);
        const auto skipped = source / L"skip.bin", follow = source / L"follow.bin";
        Check(MakePatternFile(skipped.wstring(), 32ull << 20) && MakeFile(follow.wstring(), "next", 4),
            L"create paused-copy fixtures");
        Check(MakeFile((destination / L"skip.bin").wstring(), "original", 8), L"create paused-copy original target");
        const auto original = FileHash((destination / L"skip.bin").wstring());
        requested = false; accepted = false; pause_first = true;
        ops::OpRequest request;
        request.type = ops::OpType::Copy; request.sources = {skipped.wstring(), follow.wstring()};
        request.dest_dir = destination.wstring(); request.collision_policy = ops::CollisionPolicy::Replace;
        const auto previous = g_ops.Status().completed_ops;
        const auto task = g_ops.Submit(std::move(request));
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline && g_ops.Status().phase != ops::OpPhase::Paused) Sleep(1);
        Check(g_ops.Status().phase == ops::OpPhase::Paused, L"copy pauses inside CopyFile2 with an owned output");
        pause_first = false;
        Check(g_ops.SkipCurrentItem(task, last_token.load()), L"skip accepts the paused item's identity");
        Check(WaitOpDone(previous, 30000) && g_ops.Status().phase == ops::OpPhase::Completed,
            L"skip resumes the remainder instead of leaving the task paused");
        Check(FileHash((destination / L"skip.bin").wstring()) == original && std::filesystem::exists(skipped),
            L"paused skip preserves original destination and source");
        Check(std::filesystem::exists(destination / L"follow.bin"), L"file after paused skip completes");
        bool temporary = false;
        for (const auto& item : std::filesystem::directory_iterator(destination))
            temporary |= item.path().filename().wstring().find(L".pulse-") != std::wstring::npos;
        Check(!temporary, L"paused skip cleans its unfinished temporary copy");
    }
    g_ops.Stop();
    diagnostics::runtime::Flush(); diagnostics::runtime::Shutdown();
    CoUninitialize();
    return g_fail ? 1 : 0;
}
