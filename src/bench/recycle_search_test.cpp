#include "../app/recycle_search.h"
#include <cstdio>

namespace pulse::fs {
int recycle_enumerations = 0;
bool recycle_cancel_forwarded = false;
void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info, const std::function<bool()>& cancelled) {
    ++recycle_enumerations;
    recycle_cancel_forwarded = static_cast<bool>(cancelled);
    out.clear();
    if (info) info->valid = true;
}
}

int main() {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
        if (!value) ++failures;
    };
    fs::DirEntry entry;
    entry.name = L"Quarterly Report.txt";
    entry.full_path = L"C:\\Documents\\Quarterly Report.txt";
    entry.recycle_path = L"C:\\$Recycle.Bin\\test-sid\\$R123.txt";
    entry.size = 1234;
    auto second = entry;
    second.recycle_path = L"C:\\$Recycle.Bin\\test-sid\\$R456.txt";
    std::vector<fs::DirEntry> entries{entry, second};
    index::Query query;
    query.needle = L"report ext:txt";
    query.limit = 1;
    auto result = app::SearchRecycleEntries(query, entries);
    check(result.total == 2 && result.hits.size() == 2, "original names match; provider leaves pagination to merge");
    check(result.hits.size() == 2 && result.hits[0].recycle_path != result.hits[1].recycle_path &&
        result.hits[0].path == entry.full_path, "duplicate original paths retain separate payload identities");
    query.needle = L"$R123";
    check(app::SearchRecycleEntries(query, entries).total == 0, "physical payload name is not the display name");
    query.needle = L"report ext:pdf";
    check(app::SearchRecycleEntries(query, entries).total == 0, "extension predicate applies");
    query.needle = L"report";
    query.path_prefix = L"C:\\Documents";
    check(app::SearchRecycleEntries(query, entries).total == 0, "ordinary folder scope does not include deleted items");
    query.path_prefix = L"pulse:recycle";
    check(app::SearchRecycleEntries(query, entries).total == 2, "explicit recycle scope matches");
    result = app::SearchRecycleEntries(query, entries, [] { return true; });
    check(result.error == ERROR_CANCELLED && result.hits.empty(), "cancellation publishes no partial candidates");
    int polls = 0;
    result = app::SearchRecycleEntries(query, entries, [&] { return ++polls == 3; });
    check(result.error == ERROR_CANCELLED && result.hits.empty() && result.total == 0,
        "cancellation after a match discards earlier candidates");
    query.needle = L"report content:confidential";
    check(app::SearchRecycleEntries(query, entries).total == 0,
        "content query cannot claim an unread payload matches");
    query.needle = L"report";
    query.folders_only = true;
    check(app::SearchRecycleEntries(query, entries).total == 0, "folders-only excludes files");
    query.folders_only = false;
    entries[0].recycle_path = std::wstring{};
    check(app::SearchRecycleEntries(query, entries).total == 1, "ordinary entry cannot impersonate recycled item");
    result = app::SearchRecycleBin(query);
    check(result.error == 0 && result.total == 0, "empty recycle source is successful");
    const int scans = fs::recycle_enumerations;
    query.path_prefix = L"C:\\Documents";
    app::SearchRecycleBin(query);
    check(fs::recycle_enumerations == scans, "ordinary folder scope returns before enumerating the recycle bin");
    query.path_prefix.clear();
    app::SearchRecycleBin(query, [] { return false; });
    check(fs::recycle_cancel_forwarded, "search cancellation callback reaches filesystem enumeration");
    return failures ? 1 : 0;
}
