#include "../ops/transfer_engine.h"
#include "../ops/recycle_empty.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    for (int fail : {0, 1, 3}) {
        std::atomic<bool> cancel{false}, pause{false};
        std::mutex mutex;
        std::condition_variable cv;
        pulse::worker_start_failure = fail;
        bool caught = false;
        try { pulse::ops::SmallCopyPool pool(4, cancel, pause, mutex, cv); }
        catch (const std::system_error&) { caught = true; }
        check(caught, "partial copy pool creation retires all workers and propagates failure");
    }
    pulse::worker_start_failure = -1;
    const auto root = fs::current_path() / "bench_data" /
        ("resource-threads-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directories(root);
    for (int fail : {0, 1, 3, -1}) {
        const auto bin = root / std::to_string(fail);
        fs::create_directories(bin / "$Rfixture");
        std::ofstream(bin / "$Ifixture") << "isolated index";
        for (int i = 0; i < 64; ++i) std::ofstream(bin / "$Rfixture" / std::to_string(i)) << "payload";
        pulse::worker_start_failure = fail;
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        check(result.deleted_files == 64 && !result.failed_entries && !result.cancelled && fs::is_empty(bin),
              "recycle worker creation failure completes each payload exactly once");
    }
    pulse::worker_start_failure = -1;
    {
        const auto bin = root / "fallback-cancel";
        fs::create_directories(bin / "$Rcancel");
        std::ofstream(bin / "$Icancel") << "isolated index";
        std::ofstream(bin / "$Rcancel" / "file.txt") << "keep";
        pulse::worker_start_failure = 0;
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()},
            [] { return pulse::worker_start_failure.load() == -1; }, {}, 4);
        check(result.cancelled && !result.deleted_files && fs::exists(bin / "$Icancel") &&
              fs::exists(bin / "$Rcancel" / "file.txt"), "inline fallback still polls cancellation before deleting");
        pulse::worker_start_failure = -1;
    }
    {
        const auto bin = root / "bounded";
        fs::create_directories(bin / "$Rlarge");
        std::ofstream(bin / "$Ilarge") << "isolated index";
        for (int i = 0; i < 300; ++i) std::ofstream(bin / "$Rlarge" / std::to_string(i)) << "payload";
        pulse::recycle_empty::detail::metadata_entry_limit = 100;
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        check(result.first_error == ERROR_NOT_ENOUGH_MEMORY && result.failed_entries == 1 &&
              result.deleted_files == 0 && fs::exists(bin / "$Ilarge") &&
              std::distance(fs::directory_iterator(bin / "$Rlarge"), fs::directory_iterator{}) == 300,
              "metadata budget stops inside a payload and preserves every file and index");
        pulse::recycle_empty::detail::metadata_entry_limit = 262144;
        const auto completed = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        check(completed.deleted_files == 300 && !completed.failed_entries && fs::is_empty(bin),
              "an intact refused payload can be retried with normal resources");
    }
    {
        const auto bin = root / "large";
        fs::create_directories(bin / "$Rlarge");
        std::ofstream(bin / "$Ilarge") << "isolated index";
        for (int i = 0; i < 20001; ++i) std::ofstream(bin / "$Rlarge" / std::to_string(i)) << "x";
        size_t polls = 0;
        const auto cancelled = pulse::recycle_empty::EmptyFolders({bin.wstring()}, [&] { return ++polls > 10; }, {}, 4);
        check(cancelled.cancelled && cancelled.deleted_files == 0 && fs::exists(bin / "$Ilarge"),
              "cancellation during enumeration leaves the incomplete batch untouched");
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        check(result.deleted_files == 20001 && !result.failed_entries && fs::is_empty(bin),
              "one payload above the 20000-file batch target retains normal empty-bin behavior");
    }
    {
        const auto bin = root / "locked";
        fs::create_directories(bin / "$Rlocked");
        const auto held = bin / "$Rlocked" / "locked.txt";
        std::ofstream(held) << "keep";
        std::ofstream(bin / "$Rlocked" / "free.txt") << "remove";
        std::ofstream(bin / "$Ilocked") << "isolated index";
        HANDLE lock = CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto result = pulse::recycle_empty::EmptyFolders({bin.wstring()}, {}, {}, 4);
        check(lock != INVALID_HANDLE_VALUE && result.failed_entries == 1 && result.deleted_files == 1 &&
              fs::exists(held) && fs::exists(bin / "$Ilocked"), "partial delete failure retains payload and recovery metadata");
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }
    fs::remove_all(root);
    return failures ? 1 : 0;
}
