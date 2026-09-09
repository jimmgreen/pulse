#include "../index/content_search.h"
#include "../index/content_search_client.h"
#include "../common/text_decode.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cstdio>
#include <thread>
#include <vector>

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

bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring command = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created))
        return false;
    WaitForSingleObject(created.hProcess, 8000);
    DWORD code = 1;
    GetExitCodeProcess(created.hProcess, &code);
    CloseHandle(created.hThread);
    CloseHandle(created.hProcess);
    return code == 0;
}

void PumpClient(index::ContentSearchClient& client, int polls,
                uint64_t& listed, bool& done, DWORD& error,
                std::vector<index::ContentHit>* hits) {
    for (int poll = 0; poll < polls && !done; ++poll) {
        index::ContentSearchUpdate update;
        while (client.TakeUpdate(update)) {
            if (update.progress.phase == index::ContentSearchPhase::Enumerating)
                listed = (std::max)(listed, update.progress.scanned_files);
            error = update.progress.error;
            if (hits) {
                hits->insert(hits->end(),
                    std::make_move_iterator(update.hits.begin()),
                    std::make_move_iterator(update.hits.end()));
            }
            done = update.progress.done;
        }
        if (!done) Sleep(25);
    }
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

    {
        const std::vector<uint8_t> invoice{
            0xE5, 0x8F, 0x91, 0xE7, 0xA5, 0xA8, 0x20, 0xE9, 0x87, 0x91, 0xE9, 0xA2, 0x9D};
        Check(WriteBytes(root + L"\\invoice.txt", invoice), L"write invoice fixture");
        index::ContentSearchRequest zh = request;
        zh.needle = L"\u53d1\u7968";
        zh.needles = {L"\u53d1\u7968"};
        zh.match_mode = index::ContentMatchMode::AnyWord;
        hits = Search(zh, progress);
        Check(std::any_of(hits.begin(), hits.end(), [](const auto& hit) {
            return hit.name == L"invoice.txt";
        }), L"folder walk finds Chinese content");
    }

    {
        index::ContentSearchRequest words = request;
        words.needle.clear();
        words.needles = {L"Needle", L"value"};
        words.match_mode = index::ContentMatchMode::AllWords;
        hits = Search(words, progress);
        Check(!hits.empty(), L"all-words match requires both tokens");
        words.needles = {L"Needle", L"missing"};
        hits = Search(words, progress);
        Check(hits.empty(), L"all-words rejects a missing token");
        words.needles = {L"missing", L"Needle"};
        words.match_mode = index::ContentMatchMode::AnyWord;
        hits = Search(words, progress);
        Check(!hits.empty(), L"any-word match accepts one token");
        words.match_mode = index::ContentMatchMode::Phrase;
        words.needle = L"Needle value";
        words.needles.clear();
        hits = Search(words, progress);
        Check(!hits.empty(), L"phrase match finds the full line");
        words.match_mode = index::ContentMatchMode::AllWords;
        words.needle.clear();
        words.needles = {L"Needle"};
        words.excluded_needles = {L"value"};
        words.candidate_paths = {root + L"\\utf8.txt"};
        words.root.clear();
        words.roots.clear();
        hits = Search(words, progress);
        Check(hits.empty(), L"excluded needle filters the hit");
        words.excluded_needles.clear();
        hits = Search(words, progress);
        Check(hits.size() == 1 && hits[0].name == L"utf8.txt",
              L"candidate path mode skips directory enumeration");
        words.whole_word = true;
        words.needles = {L"Need"};
        hits = Search(words, progress);
        Check(hits.empty(), L"whole-word rejects a prefix");
    }

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

    const std::vector<uint8_t> tiny{'x'};
    const std::vector<uint8_t> large(2048, 'y');
    Check(WriteBytes(root + L"\\tiny-a.bin", tiny), L"write tiny duplicate a");
    Check(WriteBytes(root + L"\\tiny-b.bin", tiny), L"write tiny duplicate b");
    Check(WriteBytes(root + L"\\large-a.bin", large), L"write large duplicate a");
    Check(WriteBytes(root + L"\\large-b.bin", large), L"write large duplicate b");
    request.minimum_file_bytes = 1024;
    hits = Search(request, progress);
    bool saw_tiny = false;
    bool saw_large = false;
    for (const auto& hit : hits) {
        if (hit.name == L"tiny-a.bin" || hit.name == L"tiny-b.bin") saw_tiny = true;
        if (hit.name == L"large-a.bin" || hit.name == L"large-b.bin") saw_large = true;
    }
    Check(!saw_tiny && saw_large, L"minimum_file_bytes excludes small duplicates");
    {
        uint64_t listed = 0;
        std::atomic<bool> listing_cancelled{false};
        index::RunContentSearch(request, listing_cancelled,
            [&](const index::ContentSearchProgress& value, std::vector<index::ContentHit>) {
                if (value.phase == index::ContentSearchPhase::Enumerating)
                    listed = (std::max)(listed, value.scanned_files);
                return true;
            });
        Check(listed >= 4, L"listing progress counts files below the minimum size");
    }
    request.minimum_file_bytes = 0;

    const fs::path skip_root = fs::path(root) / L"skip-root";
    fs::create_directories(skip_root / L"Windows", ignored);
    const std::vector<uint8_t> hidden{'h', 'i', 'd'};
    Check(WriteBytes((skip_root / L"Windows" / L"hidden.bin").wstring(), hidden),
          L"write skipped Windows child");
    Check(WriteBytes((skip_root / L"keep-a.bin").wstring(), large), L"write skip-root keep a");
    Check(WriteBytes((skip_root / L"keep-b.bin").wstring(), large), L"write skip-root keep b");
    request.root = skip_root.wstring();
    request.roots.clear();
    request.skip_system_locations = true;
    hits = Search(request, progress);
    bool saw_hidden = false;
    size_t keep_hits = 0;
    for (const auto& hit : hits) {
        if (hit.name == L"hidden.bin") saw_hidden = true;
        if (hit.name == L"keep-a.bin" || hit.name == L"keep-b.bin") ++keep_hits;
    }
    Check(!saw_hidden && keep_hits == 2, L"volume-style skip omits depth-0 Windows tree");

    request.skip_system_locations = false;
    request.root = (skip_root / L"Windows").wstring();
    request.mode = index::ContentSearchMode::Content;
    request.needle = L"hid";
    hits = Search(request, progress);
    Check(hits.size() == 1 && hits[0].name == L"hidden.bin",
          L"folder root inside Windows is not skipped");

    const fs::path left = fs::path(root) / L"left";
    const fs::path right = fs::path(root) / L"right";
    fs::create_directories(left, ignored);
    fs::create_directories(right, ignored);
    const std::vector<uint8_t> shared(4096, 'z');
    Check(WriteBytes((left / L"a.bin").wstring(), shared), L"write left duplicate");
    Check(WriteBytes((right / L"b.bin").wstring(), shared), L"write right duplicate");
    request.mode = index::ContentSearchMode::Duplicates;
    request.needle.clear();
    request.root = left.wstring();
    request.roots = {left.wstring(), right.wstring()};
    hits = Search(request, progress);
    uint32_t shared_group = 0;
    size_t shared_hits = 0;
    for (const auto& hit : hits) {
        if (hit.name == L"a.bin" || hit.name == L"b.bin") {
            if (!shared_group) shared_group = hit.group;
            if (hit.group == shared_group) ++shared_hits;
        }
    }
    Check(shared_hits == 2 && shared_group != 0, L"two roots with the same content share a group");

    Check(text::IsOfflinePlaceholder(0x00400000), L"unpinned recall file is offline");
    Check(!text::IsOfflinePlaceholder(0x00400000 | 0x00080000),
          L"pinned recall file is searchable");

    const fs::path cycle = fs::path(root) / L"cycle";
    fs::create_directories(cycle / L"inner", ignored);
    Check(WriteBytes((cycle / L"keep-a.bin").wstring(), large), L"write cycle keep a");
    Check(WriteBytes((cycle / L"keep-b.bin").wstring(), large), L"write cycle keep b");
    Check(MakeJunction((cycle / L"inner" / L"loop").wstring(), cycle.wstring()),
          L"create directory junction loop");
    {
        request.mode = index::ContentSearchMode::Duplicates;
        request.needle.clear();
        request.root = cycle.wstring();
        request.roots.clear();
        request.skip_system_locations = false;
        request.minimum_file_bytes = 0;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            for (int i = 0; i < 80 && !finished.load(); ++i) Sleep(50);
            if (!finished.load()) cancel = true;
        });
        index::ContentSearchProgress cycle_progress;
        std::vector<index::ContentHit> cycle_hits;
        index::RunContentSearch(request, cancel,
            [&](const index::ContentSearchProgress& value,
                std::vector<index::ContentHit> batch) {
                cycle_progress = value;
                cycle_hits.insert(cycle_hits.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
                return true;
            });
        finished = true;
        watchdog.join();
        Check(!cancel.load() && cycle_progress.done &&
              cycle_progress.error == ERROR_SUCCESS,
              L"reparse directories are not followed");
        size_t cycle_keep = 0;
        for (const auto& hit : cycle_hits) {
            if (hit.name == L"keep-a.bin" || hit.name == L"keep-b.bin") ++cycle_keep;
        }
        Check(cycle_keep == 2, L"junction tree still finds local duplicates");
    }

    const fs::path many = fs::path(root) / L"many";
    fs::create_directories(many, ignored);
    bool wrote_many = true;
    for (int i = 0; i < 40; ++i) {
        const std::wstring name = L"n" + std::to_wstring(i) + L".bin";
        wrote_many = WriteBytes((many / name).wstring(), std::vector<uint8_t>{'x'}) &&
                     wrote_many;
    }
    Check(wrote_many, L"write many-file listing fixture");

    {
        index::ContentSearchRequest drive;
        drive.mode = index::ContentSearchMode::Duplicates;
        drive.generation = 8;
        drive.root = L"C:\\";
        drive.skip_system_locations = true;
        drive.minimum_file_bytes = 1024ull * 1024ull;
        drive.recursive = true;
        drive.maximum_hits = 10000;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        uint64_t listed = 0;
        std::thread watchdog([&] {
            for (int i = 0; i < 50 && !finished.load(); ++i) Sleep(50);
            if (!finished.load()) cancel = true;
        });
        index::RunContentSearch(drive, cancel,
            [&](const index::ContentSearchProgress& value, std::vector<index::ContentHit>) {
                if (value.phase == index::ContentSearchPhase::Enumerating)
                    listed = (std::max)(listed, value.scanned_files);
                if (listed > 0) {
                    cancel = true;
                    return false;
                }
                return true;
            });
        finished = true;
        watchdog.join();
        wprintf(L"    in-process C:\\ listed %llu files\n",
                static_cast<unsigned long long>(listed));
        Check(listed > 0, L"C:\\ listing reports files within 2.5s");
    }

    index::ContentSearchClient client;
    client.Start(nullptr, 0);
    request.mode = index::ContentSearchMode::Content;
    request.generation = 99;
    request.root = root;
    request.roots.clear();
    request.needle = L"needle";
    request.skip_system_locations = false;
    request.minimum_file_bytes = 0;
    request.recursive = true;
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
    Check(agent_done && agent_progress.error == ERROR_SUCCESS,
          L"content agent IPC completes");
    Check(agent_progress.generation == 99 && agent_hits.size() == 4,
          L"content agent streams generation-scoped hits");

    request.mode = index::ContentSearchMode::Duplicates;
    request.generation = 101;
    request.root = many.wstring();
    request.roots.clear();
    request.needle.clear();
    request.skip_system_locations = false;
    request.minimum_file_bytes = 0;
    client.SearchAsync(request);
    uint64_t many_listed = 0;
    bool many_done = false;
    DWORD many_error = ERROR_SUCCESS;
    PumpClient(client, 200, many_listed, many_done, many_error, nullptr);
    Check(many_done && many_error == ERROR_SUCCESS,
          L"duplicate agent IPC completes");
    Check(many_listed >= 40, L"duplicate agent listing counts files before hashing");

    {
        index::ContentSearchRequest drive;
        drive.mode = index::ContentSearchMode::Duplicates;
        drive.generation = 102;
        drive.root = L"C:\\";
        drive.skip_system_locations = true;
        drive.minimum_file_bytes = 1024ull * 1024ull;
        drive.recursive = true;
        drive.maximum_hits = 10000;
        client.SearchAsync(drive);
        uint64_t listed = 0;
        bool drive_done = false;
        DWORD drive_error = ERROR_SUCCESS;
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 5000 && listed == 0 && !drive_done) {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) {
                if (update.progress.phase == index::ContentSearchPhase::Enumerating)
                    listed = (std::max)(listed, update.progress.scanned_files);
                drive_done = update.progress.done;
                drive_error = update.progress.error;
            }
            if (listed == 0 && !drive_done) Sleep(25);
        }
        client.Cancel();
        wprintf(L"    agent C:\\ listed %llu files error=%lu\n",
                static_cast<unsigned long long>(listed), drive_error);
        Check(listed > 0, L"content agent duplicate listing on C:\\ reports files");
    }
    client.Stop();

    fs::remove_all(root, ignored);
    wprintf(L"%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
