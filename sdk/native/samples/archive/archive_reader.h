#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace archive_reader {
struct Entry {
    std::string path;
    uint64_t size = 0;
    uint64_t packed = 0;
    uint64_t modified = 0; // Windows FILETIME ticks; zero when absent.
    bool dir = false;
    bool encrypted = false;
};
enum class Status { Ok, Cancelled, Unsupported, IoError, InvalidArchive, LimitReached };
struct Result {
    std::vector<Entry> entries;
    std::string format;
    bool incomplete = false;
    Status status = Status::InvalidArchive;
    std::string error;
};
// Blocking: call only on a worker. No extraction or writes. Cancellation may be
// queried from stream reads, handler progress and item enumeration on that worker.
// Listing limits: 4096 entries, 4096 UTF-8 bytes per path, 4 MiB total paths.
Result Read(const std::wstring& path, const std::function<bool()>& cancelled = {});
struct ExtractionProgress {
    std::string path;
    uint64_t completed_bytes = 0, total_bytes = 0, completed_items = 0, total_items = 0;
};
// Existing empty private staging only. Caller removes staging after success/failure.
// All output handles close before return; never extracts links or overwrites files.
Result Extract(const std::wstring& path, const std::wstring& staging,
    const std::function<bool()>& cancelled = {},
    const std::function<void(const ExtractionProgress&)>& progress = {},
    uint64_t max_bytes = uint64_t{4} * 1024 * 1024 * 1024);
// Same lexical validation used before extraction; exposed for hostile-name tests.
bool ValidExtractionPath(const std::string& path);
}
