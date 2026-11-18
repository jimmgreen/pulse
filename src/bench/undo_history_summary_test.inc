// No worker or filesystem operations: exercise large persisted batches in memory.
static int RunUndoHistorySummaryTests() {
    ops::OpsManager manager;
    const auto fixture = [](size_t count) {
        std::wstring json = L"[{\"type\":0,\"sup\":true,\"dest\":\"\",\"name\":\"\",\"src\":[";
        for (size_t i = 0; i < count; ++i) {
            if (i) json += L",";
            json += L"\"C:/source/file" + std::to_wstring(i) + L"\"";
        }
        json += L"],\"dst\":[";
        for (size_t i = 0; i < count; ++i) {
            if (i) json += L",";
            json += L"\"D:/destination/file" + std::to_wstring(i) + L"\"";
        }
        return json + L"]}]";
    };
    Check(manager.UndoFromJson(fixture(1)), L"summary single-path fixture loads");
    const auto single_history = manager.UndoHistorySummary();
    Check(manager.UndoFromJson(fixture(50000)), L"summary 50000-path fixture loads");
    const auto large = manager.UndoHistorySummary();
    if (single_history.size() != 1 || large.size() != 1) {
        Check(false, L"summary retains one row per operation");
        return 1;
    }
    const auto copied_characters = [](const auto& item) {
        return item.label.size() + item.first_source.size() + item.first_destination.size();
    };
    Check(copied_characters(single_history.front()) == copied_characters(large.front()) &&
          large.front().source_count == 50000 && large.front().destination_count == 50000,
          L"summary copies fixed string payload for 1 versus 50000 paths and preserves counts");
    const auto full = manager.UndoHistory();
    const auto& entry = full.entries.front().entry;
    const auto& item = large.front();
    Check(item.task_id == entry.task_id && item.type == entry.type && item.supported == entry.supported &&
          item.first_source == entry.sources.front() && item.first_destination == entry.destinations.front(),
          L"summary identity and first paths match full history API");
    Check(item.PreviewCount() == 50000 && item.PreviewPath() == entry.destinations.front(),
          L"completed batch uses destination folder for elsewhere and retains batch detail count");
    auto undone = item;
    undone.undone = true;
    Check(undone.PreviewCount() == 50000 && undone.PreviewPath() == entry.sources.front(),
          L"undone summary uses original source for elsewhere and preview");
    auto no_destination = item;
    no_destination.destination_count = 0;
    Check(no_destination.PreviewPath() == entry.sources.front() && no_destination.PreviewCount() == 50000,
          L"destination-free operations preserve source fallback");
    Check(manager.UndoHistoryPreviewPaths(item.task_id) == entry.destinations,
          L"hover preview still includes every path of the requested operation");
    Check(manager.UndoHistoryPreviewPaths(0).empty(), L"missing hover identity yields no paths");
    wprintf(L"history-summary: %d passed, %d failed; copied wchar count 1=%zu 50000=%zu\n",
            g_pass, g_fail, copied_characters(single_history.front()), copied_characters(item));
    const auto timed = [&](bool summary) {
        const auto start=std::chrono::steady_clock::now();size_t count=0;
        for(int i=0;i<40;++i)count+=summary?manager.UndoHistorySummary().size():manager.UndoHistory().entries.size();
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/40;
        std::printf("HISTORY_%s mean_ms=%.4f rows=%zu\n",summary?"SUMMARY":"FULL",ms,count);
    };
    timed(false);timed(true);
    return g_fail ? 1 : 0;
}
