#include "../index/content_search.h"
#include "../index/content_search_client.h"
#include "../common/text_decode.h"

#include <windows.h>
#include <filesystem>
#include <cstdio>

using namespace pulse;

namespace {

int passed = 0;
int failed = 0;

void Check(bool condition, const wchar_t* name) {
    ++(condition ? passed : failed);
    wprintf(L"[%s] %s\n", condition ? L"PASS" : L"FAIL", name);
}

std::wstring FixtureRoot() {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto build = std::filesystem::path(std::wstring(module, length)).parent_path();
    return (build.parent_path() / L"bench_data" / L"contenttest").wstring();
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() || (WriteFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
    CloseHandle(file);
    return ok;
}

std::vector<index::ContentHit> Search(index::ContentSearchRequest request,
                                      index::ContentSearchProgress& final) {
    std::atomic<bool> cancelled{false};
    std::vector<index::ContentHit> hits;
    index::RunContentSearch(request, cancelled,
        [&](const index::ContentSearchProgress& progress,
            std::vector<index::ContentHit> batch) {
            final = progress;
            hits.insert(hits.end(), std::make_move_iterator(batch.begin()),
                        std::make_move_iterator(batch.end()));
            return true;
        });
    return hits;
}

} // namespace

int wmain() {
    namespace fs = std::filesystem;
    const std::wstring root = FixtureRoot();
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(fs::path(root) / L"nested", ignored);

    const std::vector<uint8_t> utf8{
        0xEF, 0xBB, 0xBF, 'f', 'i', 'r', 's', 't', '\n',
        'N', 'e', 'e', 'd', 'l', 'e', ' ', 'v', 'a', 'l', 'u', 'e', '\n'};
    const std::vector<uint8_t> utf16le{
        0xFF, 0xFE, 'a', 0, '\n', 0, 'n', 0, 'e', 0, 'e', 0, 'd', 0, 'l', 0, 'e', 0};
    const std::vector<uint8_t> utf16be{
        0xFE, 0xFF, 0, 'a', 0, '\n', 0, 'N', 0, 'E', 0, 'E', 0, 'D', 0, 'L', 0, 'E'};
    const std::vector<uint8_t> binary{0x4D, 0x5A, 0, 1, 2, 3, 'n', 'e', 'e', 'd', 'l', 'e'};
    Check(WriteBytes(root + L"\\utf8.txt", utf8), L"write UTF-8 fixture");
    Check(WriteBytes(root + L"\\utf16le.txt", utf16le), L"write UTF-16 LE fixture");
    Check(WriteBytes(root + L"\\utf16be.txt", utf16be), L"write UTF-16 BE fixture");
    Check(WriteBytes(root + L"\\binary.bin", binary), L"write binary fixture");
    Check(WriteBytes(root + L"\\nested\\nested.txt", utf8), L"write recursive fixture");

    index::ContentSearchRequest request;
    request.generation = 42;
    request.root = root;
    request.needle = L"needle";
    index::ContentSearchProgress progress;
    auto hits = Search(request, progress);
    Check(progress.done && progress.error == ERROR_SUCCESS, L"content search completes");
    Check(progress.generation == 42, L"content search preserves generation");
    Check(hits.size() == 4, L"UTF-8 and UTF-16 matches exclude binary");
    const auto utf8_hit = std::find_if(hits.begin(), hits.end(), [](const auto& hit) {
        return hit.name == L"utf8.txt";
    });
    Check(utf8_hit != hits.end() && utf8_hit->line == 2 &&
          utf8_hit->snippet == L"Needle value", L"line number and snippet");

    request.recursive = false;
    hits = Search(request, progress);
    Check(hits.size() == 3, L"non-recursive search excludes descendants");

    request.recursive = true;
    request.maximum_hits = 1;
    hits = Search(request, progress);
    Check(hits.size() == 1 && progress.truncated, L"content hit limit truncates");

    std::atomic<bool> cancelled{true};
    index::ContentSearchProgress cancelled_progress;
    request.maximum_hits = 10000;
    const bool completed = index::RunContentSearch(request, cancelled,
        [&](const auto& value, std::vector<index::ContentHit>) {
            cancelled_progress = value;
            return true;
        });
    Check(!completed && cancelled_progress.done &&
          cancelled_progress.error == ERROR_CANCELLED, L"cancelled generation completes safely");

    const std::vector<uint8_t> duplicate{'s', 'a', 'm', 'e'};
    Check(WriteBytes(root + L"\\dup-a.txt", duplicate), L"write first duplicate");
    Check(WriteBytes(root + L"\\dup-b.txt", duplicate), L"write second duplicate");
    DeleteFileW((root + L"\\dup-link.txt").c_str());
    Check(CreateHardLinkW((root + L"\\dup-link.txt").c_str(),
                          (root + L"\\dup-a.txt").c_str(), nullptr) != FALSE,
          L"create duplicate hard link");
    Check(WriteBytes(root + L"\\empty-a.txt", {}), L"write first empty duplicate");
    Check(WriteBytes(root + L"\\empty-b.txt", {}), L"write second empty duplicate");
    request.mode = index::ContentSearchMode::Duplicates;
    request.needle.clear();
    hits = Search(request, progress);
    size_t same_group = 0;
    size_t empty_group = 0;
    for (const auto& hit : hits) {
        if (hit.size == duplicate.size()) ++same_group;
        if (hit.size == 0) ++empty_group;
    }
    Check(same_group == 2, L"hard links count as one duplicate identity");
    Check(empty_group == 2, L"empty duplicate files are reported");

    Check(text::IsOfflinePlaceholder(0x00400000), L"unpinned recall file is offline");
    Check(!text::IsOfflinePlaceholder(0x00400000 | 0x00080000),
          L"pinned recall file is searchable");

    index::ContentSearchClient client;
    client.Start(nullptr, 0);
    request.mode = index::ContentSearchMode::Content;
    request.generation = 99;
    request.needle = L"needle";
    request.maximum_hits = 10000;
    client.SearchAsync(request);
    std::vector<index::ContentHit> agent_hits;
    index::ContentSearchProgress agent_progress;
    bool agent_done = false;
    for (int poll = 0; poll < 200 && !agent_done; ++poll) {
        index::ContentSearchUpdate update;
        while (client.TakeUpdate(update)) {
            agent_progress = update.progress;
            agent_hits.insert(agent_hits.end(),
                std::make_move_iterator(update.hits.begin()),
                std::make_move_iterator(update.hits.end()));
            agent_done = update.progress.done;
        }
        if (!agent_done) Sleep(25);
    }
    client.Stop();
    Check(agent_done && agent_progress.error == ERROR_SUCCESS,
          L"content agent IPC completes");
    Check(agent_progress.generation == 99 && agent_hits.size() == 4,
          L"content agent streams generation-scoped hits");

    fs::remove_all(root, ignored);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
