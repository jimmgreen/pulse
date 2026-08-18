// measure_enum.cpp — Stage 0 directory enumeration benchmark
// pulse_bench_enum [--gen <dir> <count>] [path...]

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstdint>

#pragma comment(lib, "shlwapi.lib")

// ---- NT API dynamic definitions ----
using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NO_MORE_FILES = 0x80000006L;

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtPioApcRoutine = VOID (NTAPI*)(PVOID ApcContext, NtIoStatusBlock* IoStatusBlock, ULONG Reserved);

struct NtFileFullDirInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

enum NtFileInformationClass : int {
    NtFileFullDirectoryInformation = 2,
};

constexpr ULONG NT_FILE_LIST_DIRECTORY = 0x0001;
constexpr ULONG NT_SYNCHRONIZE = 0x00100000;
constexpr ULONG NT_FILE_OPEN = 1;
constexpr ULONG NT_FILE_DIRECTORY_FILE = 0x00000001;
constexpr ULONG NT_FILE_SYNCHRONIZE_IO_NONALERT = 0x00000004;
constexpr ULONG NT_OBJ_CASE_INSENSITIVE = 0x00000040;

#define NtInitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(NtObjectAttributes); \
    (p)->RootDirectory = r; \
    (p)->Attributes = a; \
    (p)->ObjectName = n; \
    (p)->SecurityDescriptor = s; \
    (p)->SecurityQualityOfService = NULL; \
}

using NtCreateFile_t = NTSTATUS (NTAPI*)(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    NtObjectAttributes* ObjectAttributes,
    NtIoStatusBlock* IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength
);

using NtQueryDirectoryFile_t = NTSTATUS (NTAPI*)(
    HANDLE FileHandle,
    HANDLE Event,
    NtPioApcRoutine ApcRoutine,
    PVOID ApcContext,
    NtIoStatusBlock* IoStatusBlock,
    PVOID FileInformationBuffer,
    ULONG Length,
    NtFileInformationClass FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    NtUnicodeString* FileName,
    BOOLEAN RestartScan
);

// ---- utilities ----
static std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws;
    ws.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), n);
    return ws;
}

static std::wstring add_long_path_prefix(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\?\\UNC\\")) return path;
    if (path.starts_with(L"\\\\")) {
        // UNC -> \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

static double millis(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct EntryInfo {
    std::wstring name;
    LONGLONG size;
    FILETIME mtime;
    DWORD attrs;
    bool is_dir;
};

static void collect_findfirstfile(const std::wstring& path, std::vector<EntryInfo>& out) {
    std::wstring pattern = add_long_path_prefix(path);
    if (!pattern.ends_with(L"\\")) pattern += L"\\";
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return;
        throw std::runtime_error("FindFirstFileExW failed");
    }
    do {
        EntryInfo e;
        e.name = fd.cFileName;
        e.size = static_cast<LONGLONG>(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        e.attrs = fd.dwFileAttributes;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void collect_findfirstfileex_largefetch(const std::wstring& path, std::vector<EntryInfo>& out) {
    std::wstring pattern = add_long_path_prefix(path);
    if (!pattern.ends_with(L"\\")) pattern += L"\\";
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return;
        throw std::runtime_error("FindFirstFileExW LARGE_FETCH failed");
    }
    do {
        EntryInfo e;
        e.name = fd.cFileName;
        e.size = static_cast<LONGLONG>(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        e.attrs = fd.dwFileAttributes;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static NtCreateFile_t g_NtCreateFile = nullptr;
static NtQueryDirectoryFile_t g_NtQueryDirectoryFile = nullptr;

static void init_ntapi() {
    if (g_NtCreateFile) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) throw std::runtime_error("ntdll.dll not loaded");
    g_NtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(ntdll, "NtCreateFile"));
    g_NtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
    if (!g_NtCreateFile || !g_NtQueryDirectoryFile)
        throw std::runtime_error("NtCreateFile / NtQueryDirectoryFile not found");
}

static void collect_ntquerydirectoryfile(const std::wstring& path, std::vector<EntryInfo>& out) {
    init_ntapi();

    std::wstring target = add_long_path_prefix(path);
    // NtCreateFile expects an NT object path: \\?\C:\... is a Win32 long-path prefix;
    // the native equivalent is \??\C:\... .
    if (target.starts_with(L"\\\\?\\")) {
        target = L"\\??\\" + target.substr(4);
    }
    // NtCreateFile expects a path to the directory itself, no trailing wildcard.
    if (target.ends_with(L"\\")) target.pop_back();

    NtUnicodeString us;
    us.Buffer = const_cast<PWSTR>(target.c_str());
    us.Length = static_cast<USHORT>(target.size() * sizeof(WCHAR));
    us.MaximumLength = static_cast<USHORT>((target.size() + 1) * sizeof(WCHAR));

    NtObjectAttributes oa;
    NtInitializeObjectAttributes(&oa, &us, NT_OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    NtIoStatusBlock iosb{};
    HANDLE h;
    NTSTATUS status = g_NtCreateFile(
        &h,
        NT_FILE_LIST_DIRECTORY | NT_SYNCHRONIZE,
        &oa,
        &iosb,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NT_FILE_OPEN,
        NT_FILE_DIRECTORY_FILE,
        nullptr,
        0);

    if (status != STATUS_SUCCESS) {
        std::ostringstream oss;
        oss << "NtCreateFile failed, status=0x" << std::hex << status;
        throw std::runtime_error(oss.str());
    }

    constexpr SIZE_T buf_size = 64 * 1024;
    std::vector<BYTE> buffer(buf_size);
    bool restart = true;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        CloseHandle(h);
        throw std::runtime_error("CreateEvent failed");
    }

    for (;;) {
        iosb = {};
        status = g_NtQueryDirectoryFile(
            h,
            hEvent,
            nullptr,
            nullptr,
            &iosb,
            buffer.data(),
            static_cast<ULONG>(buf_size),
            NtFileFullDirectoryInformation,
            FALSE,
            nullptr,
            restart ? TRUE : FALSE);

        restart = false;

        if (status == STATUS_PENDING) {
            WaitForSingleObject(hEvent, INFINITE);
            status = iosb.Status;
        }

        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) {
            CloseHandle(hEvent);
            CloseHandle(h);
            std::ostringstream oss;
            oss << "NtQueryDirectoryFile failed, status=0x" << std::hex << status;
            throw std::runtime_error(oss.str());
        }

        auto* info = reinterpret_cast<NtFileFullDirInformation*>(buffer.data());
        for (;;) {
            EntryInfo e;
            e.name.assign(info->FileName, info->FileNameLength / sizeof(WCHAR));
            e.size = info->EndOfFile.QuadPart;
            e.mtime.dwLowDateTime = info->LastWriteTime.LowPart;
            e.mtime.dwHighDateTime = info->LastWriteTime.HighPart;
            e.attrs = info->FileAttributes;
            e.is_dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            out.push_back(e);

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<NtFileFullDirInformation*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }

    CloseHandle(hEvent);
    CloseHandle(h);
}

// ---- generation ----
static bool create_directory_recursive(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    if (err == ERROR_PATH_NOT_FOUND) {
        std::wstring parent = path;
        auto pos = parent.find_last_of(L"\\/");
        if (pos != std::wstring::npos && pos > 0) {
            parent = parent.substr(0, pos);
            if (create_directory_recursive(parent)) {
                if (CreateDirectoryW(path.c_str(), nullptr)) return true;
                err = GetLastError();
                return err == ERROR_ALREADY_EXISTS;
            }
        }
    }
    return false;
}

static void gen_files_thread(int thread_id, int total_threads, const std::wstring& dir, int64_t count,
                             std::atomic<int64_t>& done, std::mutex& out_mtx, bool progress) {
    wchar_t name[32];
    for (int64_t i = thread_id; i < count; i += total_threads) {
        swprintf_s(name, L"bench_%06lld.dat", i + 1);
        std::wstring path = dir;
        if (!path.ends_with(L"\\")) path += L"\\";
        path += name;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        int64_t d = ++done;
        if (progress && (d % 10000 == 0 || d == count)) {
            std::lock_guard<std::mutex> lk(out_mtx);
            std::wcout << L"  generated " << d << L" / " << count << L"\n";
        }
    }
}

static void generate_directory(const std::wstring& dir, int64_t count) {
    if (!create_directory_recursive(dir)) {
        std::wcerr << L"failed to create directory: " << dir << L"\n";
        return;
    }
    std::wcout << L"Generating " << count << L" empty files in " << dir << L"\n";
    std::atomic<int64_t> done{0};
    std::mutex out_mtx;
    bool progress = count >= 100000;

    unsigned nthreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
    if (count < 1000) nthreads = 1;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < nthreads; ++t) {
        threads.emplace_back(gen_files_thread, t, nthreads, dir, count, std::ref(done), std::ref(out_mtx), progress);
    }
    for (auto& th : threads) th.join();
    std::wcout << L"done: " << done.load() << L" files\n";
}

// ---- benchmark harness ----
struct BenchResult {
    std::string method;
    std::vector<double> times_ms;
    int64_t count = 0;
    bool ok = false;
    std::string error;
};

using Collector = void(*)(const std::wstring&, std::vector<EntryInfo>&);

static BenchResult run_bench(const std::string& method_name, Collector fn, const std::wstring& path, int iterations) {
    BenchResult r;
    r.method = method_name;
    for (int i = 0; i < iterations; ++i) {
        try {
            std::vector<EntryInfo> entries;
            auto t0 = std::chrono::steady_clock::now();
            fn(path, entries);
            auto t1 = std::chrono::steady_clock::now();
            r.times_ms.push_back(millis(t1 - t0));
            if (i == 0) r.count = static_cast<int64_t>(entries.size());
            else if (r.count != static_cast<int64_t>(entries.size())) {
                r.ok = false;
                r.error = "inconsistent entry count across iterations";
                return r;
            }
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = e.what();
            r.ok = false;
            return r;
        }
    }
    return r;
}

static bool compare_counts(const BenchResult& a, const BenchResult& b) {
    return a.ok && b.ok && a.count == b.count;
}

static void print_results(const std::wstring& path, const std::vector<BenchResult>& results) {
    std::wcout << L"\n=== " << path << L" ===\n";
    std::wcout << std::fixed << std::setprecision(2);
    std::wcout << L"  Method                 |  1st (ms) | best/5 (ms) | count\n";
    std::wcout << L"  -----------------------|-----------|-------------|--------\n";
    for (const auto& r : results) {
        if (!r.ok) {
            std::wcout << L"  " << std::left << std::setw(23) << to_wide(r.method).c_str()
                       << L" | error: " << to_wide(r.error).c_str() << L"\n";
            continue;
        }
        double first = r.times_ms.front();
        double best = *std::min_element(r.times_ms.begin(), r.times_ms.end());
        std::wcout << L"  " << std::left << std::setw(23) << to_wide(r.method).c_str()
                   << L" | " << std::right << std::setw(9) << first
                   << L" | " << std::setw(11) << best
                   << L" | " << r.count << L"\n";
    }
    if (results.size() >= 3 && compare_counts(results[0], results[1]) && compare_counts(results[1], results[2])) {
        std::wcout << L"  counts match across methods.\n";
    } else {
        std::wcout << L"  WARNING: counts differ across methods.\n";
    }
    std::wcout << L"  Note: FindFirstFileW/ExW return . and ..; NtQueryDirectoryFile(FileFullDirectoryInformation) does not.\n";
    std::wcout << L"  When comparing, only non-. .. entries are considered via the stored count.\n";
}

static int usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--gen <dir> <count>] [path...]\n";
    return 1;
}

int wmain(int argc, wchar_t* argv[]);

int main() {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int r = wmain(argc, argv);
    LocalFree(argv);
    return r;
}

int wmain(int argc, wchar_t* argv[]) {
    std::ios::sync_with_stdio(false);

    if (argc < 2) {
        return usage("pulse_bench_enum");
    }

    int idx = 1;
    std::vector<std::wstring> measure_paths;

    while (idx < argc) {
        if (std::wstring_view(argv[idx]) == L"--gen") {
            if (idx + 3 > argc) return usage("pulse_bench_enum");
            std::wstring dir = argv[idx + 1];
            int64_t count = _wtoi64(argv[idx + 2]);
            generate_directory(dir, count);
            idx += 3;
        } else {
            measure_paths.push_back(argv[idx]);
            ++idx;
        }
    }

    if (measure_paths.empty()) {
        std::wcout << L"No paths to measure.\n";
        return 0;
    }

    for (const auto& p : measure_paths) {
        std::vector<BenchResult> results;
        results.push_back(run_bench("FindFirstFileW", collect_findfirstfile, p, 5));
        results.push_back(run_bench("FindFirstFileExW+LARGE", collect_findfirstfileex_largefetch, p, 5));
        results.push_back(run_bench("NtQueryDirectoryFile", collect_ntquerydirectoryfile, p, 5));
        print_results(p, results);
    }

    return 0;
}
