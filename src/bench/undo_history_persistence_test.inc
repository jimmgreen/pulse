#include "../common/json_utils.h"

static std::wstring BaselineUndoToJson(const std::vector<ops::UndoEntry>& entries) {
    std::wstring out = L"[\n";
    bool first = true;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.generated_output) continue;
        if (!first) out += L",\n";
        first = false;
        wchar_t head[64];
        swprintf_s(head, L"  {\"type\":%d,\"sup\":%s,", (int)e.type, e.supported ? L"true" : L"false");
        out += head;
        out += L"\"dest\":\"";
        pulse::json::Escape(e.dest_dir, out);
        out += L"\",\"name\":\"";
        pulse::json::Escape(e.new_name, out);
        out += L"\",\"src\":[";
        for (size_t j = 0; j < e.sources.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.sources[j], out);
            out += L"\"";
            if (j + 1 < e.sources.size()) out += L",";
        }
        out += L"],\"dst\":[";
        for (size_t j = 0; j < e.destinations.size(); ++j) {
            out += L"\"";
            pulse::json::Escape(e.destinations[j], out);
            out += L"\"";
            if (j + 1 < e.destinations.size()) out += L",";
        }
        out += L"]}";
        out += L"\n";
    }
    out += L"]";
    return out;
}

// Isolated in-memory history persistence; no user files or preferences.
static int RunUndoHistoryPersistenceTests() {
    ops::OpsManager manager;
    std::wstring json = L"[{\"type\":0,\"sup\":true,\"dest\":\"D:\\\\target\",\"name\":\"\",\"src\":[";
    constexpr size_t count = 40000;
    for (size_t i = 0; i < count; ++i) {
        if (i) json += L",";
        json += L"\"C:\\\\source\\\\file-" + std::to_wstring(i) + L".txt\"";
    }
    json += L"],\"dst\":[]}]";
    Check(manager.UndoFromJson(json), L"large merged operation restores all input paths");
    const auto cold_start = std::chrono::steady_clock::now();
    const auto first = manager.UndoJsonSnapshot();
    const auto cold_end = std::chrono::steady_clock::now();
    ops::OpsManager restored;
    Check(restored.UndoFromJson(*first), L"immutable persistence payload parses");
    const auto paths = restored.UndoHistoryPreviewPaths(restored.UndoHistorySummary().front().task_id);
    Check(paths.size() == count && paths.front() == L"C:\\source\\file-0.txt" &&
        paths.back() == L"C:\\source\\file-39999.txt", L"persistence retains every path and boundary values");
    std::vector<ops::UndoEntry> baseline_entries;
    for (const auto& item : manager.UndoHistory().entries) baseline_entries.push_back(item.entry);
    Check(BaselineUndoToJson(baseline_entries) == *first, L"cached payload exactly matches original serializer");
    size_t baseline_characters = 0;
    const auto baseline_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) baseline_characters += BaselineUndoToJson(baseline_entries).size();
    const auto baseline_end = std::chrono::steady_clock::now();
    Check(baseline_characters == first->size() * 20, L"baseline serializes the complete payload every pass");
    bool stable = true;
    const auto warm_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) stable = stable && manager.UndoJsonSnapshot() == first;
    const auto warm_end = std::chrono::steady_clock::now();
    Check(stable, L"unchanged history shares one immutable payload");
    manager.RecordExternalCreate(L"C:\\source\\new.txt", 0, L"new");
    const auto changed = manager.UndoJsonSnapshot();
    Check(changed != first && changed->find(L"new.txt") != std::wstring::npos &&
        first->find(L"new.txt") == std::wstring::npos, L"mutation publishes new content and preserves held snapshot");
    Check(manager.UndoHistoryCount() == 2 && manager.UndoHistorySummary(false).size() == 1,
        L"collapsed history returns one newest supported row without losing count");
    const auto newest = manager.UndoHistorySummary(false);
    const auto full = manager.UndoHistorySummary();
    Check(newest.size() == 1 && newest.front().task_id == full.front().task_id &&
        newest.front().type == ops::OpType::CreateTextFile && newest.front().source_count == 1 &&
        manager.UndoHistoryCount() == full.size(), L"collapsed summary preserves newest identity type and counts");
    Check(manager.UndoFromJson(L"[]") && *manager.UndoJsonSnapshot() == L"[\n]",
        L"restoring empty history invalidates previous persisted content");
    Check(manager.UndoHistorySummary(false).empty() && manager.UndoHistoryCount() == 0,
        L"empty restored history has no collapsed row or count");
    std::wstring unsupported = json;
    unsupported.replace(unsupported.find(L"true"), 4, L"false");
    Check(manager.UndoFromJson(unsupported) && manager.UndoHistorySummary(false).empty() &&
        manager.UndoHistoryCount() == 1, L"unsupported-only history keeps its count without an undo row");
    wprintf(L"history-persistence: baseline_20_ms=%.3f\n",
        std::chrono::duration<double, std::milli>(baseline_end - baseline_start).count());
    wprintf(L"history-persistence: paths=%zu bytes=%zu cold_ms=%.3f warm_1000_ms=%.3f\n", count,
        first->size() * sizeof(wchar_t),
        std::chrono::duration<double, std::milli>(cold_end - cold_start).count(),
        std::chrono::duration<double, std::milli>(warm_end - warm_start).count());
    wprintf(L"history-persistence: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
