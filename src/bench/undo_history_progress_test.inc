// In-memory long-session fixture: no filesystem or operation worker is needed.
static bool HistoryProgressMatches(const ops::OpsManager& manager) {
    const auto full = manager.UndoHistory();
    const auto light = manager.HistoryProgress();
    return light.has_entries == !full.entries.empty() && light.busy == full.busy &&
        light.completed_sequence == full.completed_sequence && light.completed_steps == full.completed_steps &&
        light.completed_redo == full.completed_redo && light.completed_success == full.completed_success;
}

static int RunUndoHistoryProgressTests(bool baseline) {
    static_assert(std::is_trivially_copyable_v<ops::UndoHistoryProgress>);
    ops::OpsManager manager;
    Check(HistoryProgressMatches(manager) && !manager.HistoryProgress().has_entries,
        L"empty history progress matches the full snapshot");
    std::wstring json = L"[";
    const std::wstring directory = L"C:/" + std::wstring(320, L'x') + L"/";
    for (size_t batch = 0; batch < 12; ++batch) {
        if (batch) json += L",";
        json += L"{\"type\":0,\"sup\":true,\"dest\":\"\",\"name\":\"\",\"src\":[";
        for (size_t item = 0; item < 1000; ++item) {
            if (item) json += L",";
            json += L"\"" + directory + std::to_wstring(batch) + L"-" + std::to_wstring(item) + L"\"";
        }
        json += L"]}";
    }
    json += L"]";
    Check(manager.UndoFromJson(json), L"twelve large long-path history batches load");
    Check(HistoryProgressMatches(manager) && manager.HistoryProgress().has_entries,
        L"populated history progress preserves notification fields");
    size_t copied_characters = 0;
    constexpr size_t reads = 40;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < reads; ++i) {
        if (baseline) {
            const auto snapshot = manager.UndoHistory();
            for (const auto& row : snapshot.entries)
                for (const auto& path : row.entry.sources) copied_characters += path.size();
        } else {
            const auto progress = manager.HistoryProgress();
            if (!progress.has_entries || progress.busy) ++g_fail;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    wprintf(L"HISTORY_PROGRESS mode=%ls reads=%zu copied_path_characters=%zu elapsed_ms=%.3f\n",
        baseline ? L"BASELINE_FULL" : L"MODIFIED_PROGRESS", reads, copied_characters, elapsed);
    Check(baseline ? copied_characters > 100000000 : copied_characters == 0,
        L"frequent reader path payload is measured independently of timing");
    ops::OpRequest request;
    request.type = ops::OpType::Rename;
    request.sources = {L"C:/not-executed"}; request.new_name = L"renamed";
    manager.Submit(std::move(request));
    Check(HistoryProgressMatches(manager) && manager.HistoryProgress().busy,
        L"queued task keeps history controls busy without copying paths");
    return g_fail ? 1 : 0;
}
