#define PULSE_WORKER_BATCH_TEST
#include "../app/app_worker.cpp"
#include <cstdio>
static HANDLE blocked, release_io, delivered;
static std::atomic<int> block_count{0}, callbacks{0};
namespace pulse::fs {
// This fixture exercises filesystem scheduling only, without phone/Shell services.
bool IsDevicePath(const std::wstring& path) { return path.starts_with(L"pulse:device:"); }
std::wstring FormatDevicePath(const DevicePath&) { throw std::runtime_error("unexpected device fixture"); }
uint64_t WatchClock() noexcept { return GetTickCount64(); }
void WaitForStartingWatch(const std::wstring&, DWORD, DWORD) {}
bool IsVirtualPath(const std::wstring& p) { return p.starts_with(L"pulse:"); }
bool IsUncPath(const std::wstring& p) { return p.starts_with(L"\\\\"); }
DWORD ReadReparseTag(const std::wstring&) { return 0; }
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>&, const std::atomic_bool*) {
    if (path.find(L"blocked") != std::wstring::npos) {
        ++block_count;
        SetEvent(blocked);
        WaitForSingleObject(release_io, INFINITE);
    }
}
void EnumerateRecycleBin(std::vector<DirEntry>&, RecycleBinInfo*) {}
bool QueryDirectoryIdentity(const std::wstring&, DirectoryIdentity&) { return true; }
NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring&) { return {}; }
bool SaveNetSnapshot(const NetSnapshotWrite&, const SnapshotPtr&) { return true; }
}
namespace pulse::app {
void RefreshDriveEjectable(const std::wstring&, uint8_t) {}
std::vector<ThisPcLauncher> CurrentThisPcLaunchers() { return {}; }
DeviceRegistry& DeviceRegistry::Instance() { throw std::runtime_error("unexpected device fixture"); }
std::vector<fs::DeviceInfo> DeviceRegistry::Devices() const { return {}; }
fs::DeviceError DeviceRegistry::ListFolder(const std::wstring&, std::vector<fs::DirEntry>&,
                                          const std::function<bool()>&) { return fs::DeviceError::Failed; }
FolderSortMode DefaultFolderSortMode() noexcept { return FolderSortMode::FoldersFirst; }
ScopedFolderSortMode::ScopedFolderSortMode(FolderSortMode) noexcept : previous_(0) {}
ScopedFolderSortMode::~ScopedFolderSortMode() = default;
ScopedMediaSortFolder::ScopedMediaSortFolder(const std::wstring&) noexcept : previous_(nullptr) {}
ScopedMediaSortFolder::~ScopedMediaSortFolder() = default;
std::wstring FindGitRoot(const std::wstring&) { return {}; }
void ResolveLinksInPlace(const std::wstring&, std::vector<fs::DirEntry>&, const std::function<bool()>&) {}
ScopedEntryGrouping::ScopedEntryGrouping(int, const std::wstring&) {}
ScopedEntryGrouping::~ScopedEntryGrouping() = default;
bool EntryLess(const fs::DirEntry&, const fs::DirEntry&, ui::SortColumn, ui::SortDirection) { return false; }
void SortEntriesBySize(std::vector<fs::DirEntry>&, ui::SortDirection, const FolderSizeLookup&, const std::function<void()>&) {}
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir, const std::function<void()>& tick) {
    std::sort(entries.begin(), entries.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) { if (tick) tick(); return EntryLess(a, b, col, dir); });
}
}
namespace pulse::app::startup_trace {
void Mark(const char*) noexcept {}
}
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
static int failures = 0;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool Drained() {
    for (int i = 0; i < 1000 && pulse::fs::active_network_enumerations; ++i) Sleep(2);
    return pulse::fs::active_network_enumerations == 0;
}
int main() {
    using namespace pulse;
    blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    delivered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        auto generation = worker.Refresh(L"\\\\server\\blocked", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(blocked, 2000) == WAIT_OBJECT_0, "WorkerPool reaches blocked enumeration");
        worker.CancelGeneration(generation);
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "replacement navigation completes while old provider blocked");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop returns while provider ignores cancellation");
        Check(callbacks == 1, "only replacement result delivered");
    }
    SetEvent(release_io);
    Check(Drained(), "provider retires safely after WorkerPool destruction");
    Check(callbacks == 1, "late completion cannot call destroyed pool");
    ResetEvent(release_io); ResetEvent(blocked); ResetEvent(delivered);
    callbacks = 0; block_count = 0;
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        for (int i = 0; i < 4; ++i)
            worker.Refresh(L"\\\\server\\blocked" + std::to_wstring(i), ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto waiting = GetTickCount64();
        const unsigned expected = std::min(4u, std::max(2u, std::thread::hardware_concurrency())) - 1;
        while (block_count < static_cast<int>(expected) && GetTickCount64() - waiting < 2000) Sleep(2);
        Check(block_count == static_cast<int>(expected), "network concurrency reserves one navigation worker");
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "queued local navigation bypasses blocked UNC workers");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop cancels concurrent and queued generations");
    }
    SetEvent(release_io);
    Check(Drained() && callbacks == 1, "concurrent late results discarded and reclaimed");
    Check(app::raw_listing_batches_created == 0, "single-presentation requests retain direct move without shared batch copies");
    {
        wchar_t current[MAX_PATH]{};
        GetCurrentDirectoryW(ARRAYSIZE(current), current);
        const wchar_t marked[] = {fs::DriveLetterOf(current), 0};
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", L"worker-enumeration-fixture");
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", marked);
        const std::wstring remote_root = std::wstring(marked) + L":\\";
        Check(fs::IsRemoteLikeDrive(remote_root), "fixture marks a drive as remote-like without a live share");
        ResetEvent(release_io); ResetEvent(blocked); ResetEvent(delivered);
        callbacks = 0; block_count = 0;
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        std::vector<uint64_t> generations;
        for (int i = 0; i < 4; ++i)
            generations.push_back(worker.Refresh(remote_root + L"blocked" + std::to_wstring(i),
                ui::SortColumn::Name, ui::SortDirection::Asc));
        const unsigned expected = std::min(4u, std::max(2u, std::thread::hardware_concurrency())) - 1;
        const auto waiting = GetTickCount64();
        while (block_count < static_cast<int>(expected) && GetTickCount64() - waiting < 2000) Sleep(2);
        Check(fs::active_network_enumerations == expected && fs::active_local_enumerations == 0,
              "mapped/cloud enumeration uses network quota and reserves local capacity");
        // The provider is a stub: this unmarked path succeeds without touching a disk.
        worker.Refresh(L"local-healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0,
              "local navigation bypasses blocked mapped/cloud requests");
        for (auto generation : generations) worker.CancelGeneration(generation);
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "mapped/cloud cancellation stops without waiting for provider");
        SetEvent(release_io);
        Check(Drained() && callbacks == 1, "mapped/cloud late results are discarded after cancellation");
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
    }
    for (const bool supersede : {false, true}) {
        ResetEvent(release_io); ResetEvent(blocked); callbacks = 0; block_count = 0;
        HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<unsigned> parked{0};
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; });
        const unsigned count = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
        for (unsigned i = 0; i < count; ++i) worker.EnqueueIo([&] { ++parked; WaitForSingleObject(gate, 5000); });
        const auto deadline = GetTickCount64() + 2000;
        while (parked < count && GetTickCount64() < deadline) Sleep(1);
        Check(parked == count, "all workers parked to form deterministic request batch");
        const auto first = worker.Refresh(L"C:\\blocked", ui::SortColumn::Name, ui::SortDirection::Asc);
        worker.Refresh(L"C:\\blocked", ui::SortColumn::Size, ui::SortDirection::Desc);
        if (supersede) worker.Refresh(L"C:\\blocked", ui::SortColumn::Name, ui::SortDirection::Asc);
        SetEvent(gate);
        Check(WaitForSingleObject(blocked, 2000) == WAIT_OBJECT_0, "shared provider starts");
        if (!supersede) worker.CancelGeneration(first);
        SetEvent(release_io);
        const int expected_callbacks = supersede ? 2 : 1;
        const auto finished = GetTickCount64() + 3000;
        while (callbacks < expected_callbacks && GetTickCount64() < finished) Sleep(1);
        worker.Stop();
        Check(callbacks == expected_callbacks, "cancel one subscriber preserves other presentation");
        Check(block_count == (supersede ? 2 : 1), "same-key refresh starts fresh batch; different presentations share enumeration");
        CloseHandle(gate);
    }
    Check(app::raw_listing_batches_created == 2, "shared batch allocated only when second compatible presentation queues");
    CloseHandle(blocked); CloseHandle(release_io); CloseHandle(delivered);
    return failures ? 1 : 0;
}
