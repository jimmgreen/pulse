// measure_mft.cpp — Stage 0 MFT / USN journal benchmark
// Usage: pulse_bench_mft [drive_letter]
//   drive_letter: e.g. C (default C)

#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <sddl.h>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cctype>

#pragma comment(lib, "advapi32.lib")

static double millis(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static bool is_admin() {
    BOOL admin = FALSE;
    PSID administrators_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &administrators_group)) {
        CheckTokenMembership(nullptr, administrators_group, &admin);
        FreeSid(administrators_group);
    }
    return admin == TRUE;
}

static HANDLE open_volume(wchar_t letter) {
    wchar_t path[16];
    swprintf_s(path, L"\\\\.\\%c:", letter);
    // FSCTL_ENUM_USN_DATA / FSCTL_QUERY_USN_JOURNAL typically require FILE_READ_DATA on the volume.
    // Try it first; fall back to FILE_READ_ATTRIBUTES so non-admin can at least own a handle and
    // report the degraded state cleanly.
    HANDLE h = CreateFileW(path, FILE_READ_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::wcout << L"CreateFileW(FILE_READ_DATA) failed, error=" << err
                   << L"; falling back to FILE_READ_ATTRIBUTES.\n";
        h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            std::wcout << L"CreateFileW(FILE_READ_ATTRIBUTES) also failed, error=" << err << L"\n";
        }
    }
    return h;
}

static void query_usn_journal(HANDLE hvol) {
    USN_JOURNAL_DATA_V0 jd{};
    DWORD br = 0;
    BOOL ok = DeviceIoControl(hvol, FSCTL_QUERY_USN_JOURNAL,
                              nullptr, 0,
                              &jd, sizeof(jd),
                              &br, nullptr);
    std::wcout << L"\n=== USN Journal ===\n";
    if (!ok) {
        DWORD err = GetLastError();
        std::wcout << L"FSCTL_QUERY_USN_JOURNAL failed, error=" << err << L"\n";
        if (err == ERROR_INVALID_FUNCTION) {
            std::wcout << L"  (USN journal may not be active on this volume.)\n";
        }
        return;
    }
    std::wcout << L"UsnJournalID    = " << jd.UsnJournalID << L"\n";
    std::wcout << L"FirstUsn        = " << jd.FirstUsn << L"\n";
    std::wcout << L"NextUsn         = " << jd.NextUsn << L"\n";
    std::wcout << L"LowestValidUsn  = " << jd.LowestValidUsn << L"\n";
    std::wcout << L"MaxUsn          = " << jd.MaxUsn << L"\n";
    std::wcout << L"MaximumSize     = " << jd.MaximumSize << L" bytes\n";
    std::wcout << L"AllocationDelta = " << jd.AllocationDelta << L" bytes\n";
}

static void read_usn_journal(HANDLE hvol, int64_t limit_records) {
    USN_JOURNAL_DATA_V0 jd{};
    DWORD br = 0;
    BOOL ok = DeviceIoControl(hvol, FSCTL_QUERY_USN_JOURNAL,
                              nullptr, 0,
                              &jd, sizeof(jd),
                              &br, nullptr);
    if (!ok) {
        std::wcout << L"\n=== Read USN Journal ===\n";
        std::wcout << L"skip: cannot query journal\n";
        return;
    }

    READ_USN_JOURNAL_DATA_V0 rud{};
    rud.UsnJournalID = jd.UsnJournalID;
    rud.ReasonMask = 0xFFFFFFFF;
    rud.ReturnOnlyOnClose = 0;
    rud.Timeout = 0;
    rud.BytesToWaitFor = 0;
    rud.StartUsn = jd.NextUsn - 1;
    if (rud.StartUsn < jd.LowestValidUsn) rud.StartUsn = jd.LowestValidUsn;
    (void)jd.FirstUsn; // unused field referenced only for completeness

    constexpr DWORD buf_size = 1 * 1024 * 1024;
    std::vector<BYTE> buffer(buf_size);
    int64_t records = 0;
    bool truncated = false;

    auto t0 = std::chrono::steady_clock::now();
    while (records < limit_records) {
        DWORD bytes_read = 0;
        ok = DeviceIoControl(hvol, FSCTL_READ_USN_JOURNAL,
                             &rud, sizeof(rud),
                             buffer.data(), buf_size,
                             &bytes_read, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF || err == ERROR_NO_MORE_ITEMS) break;
            std::wcout << L"FSCTL_READ_USN_JOURNAL error=" << err << L"\n";
            break;
        }
        if (bytes_read < sizeof(USN)) break;
        USN* usn = reinterpret_cast<USN*>(buffer.data());
        PUSN_RECORD_V2 rec = reinterpret_cast<PUSN_RECORD_V2>(buffer.data() + sizeof(USN));
        DWORD remaining = bytes_read - static_cast<DWORD>(sizeof(USN));
        if (remaining == 0) break;

        while (remaining > 0) {
            if (rec->RecordLength == 0 || rec->RecordLength > remaining) break;
            ++records;
            if (records >= limit_records) {
                truncated = true;
                break;
            }
            remaining -= rec->RecordLength;
            rec = reinterpret_cast<PUSN_RECORD_V2>(reinterpret_cast<BYTE*>(rec) + rec->RecordLength);
        }
        rud.StartUsn = *usn;
        if (rud.StartUsn <= jd.LowestValidUsn || rud.StartUsn >= jd.NextUsn) break;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = millis(t1 - t0);

    std::wcout << L"\n=== Read Recent USN Journal ===\n";
    std::wcout << L"records read = " << records << (truncated ? L" (truncated to limit)" : L"") << L"\n";
    std::wcout << std::fixed << std::setprecision(2);
    std::wcout << L"time         = " << ms << L" ms\n";
    if (ms > 0) std::wcout << L"rate         = " << (records * 1000.0 / ms) << L" rec/s\n";
}

static void enum_mft(HANDLE hvol, wchar_t /*letter*/) {
    std::wcout << L"\n=== Full MFT Enumeration (FSCTL_ENUM_USN_DATA) ===\n";

    MFT_ENUM_DATA_V0 med{};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = _I64_MAX;

    constexpr DWORD buf_size = 4 * 1024 * 1024;
    std::vector<BYTE> buffer(buf_size);
    int64_t records = 0;
    bool truncated = false;

    const auto max_duration = std::chrono::minutes(5);
    auto t0 = std::chrono::steady_clock::now();

    for (;;) {
        DWORD bytes_read = 0;
        BOOL ok = DeviceIoControl(hvol, FSCTL_ENUM_USN_DATA,
                                  &med, sizeof(med),
                                  buffer.data(), buf_size,
                                  &bytes_read, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF || err == ERROR_NO_MORE_ITEMS) break;
            std::wcout << L"FSCTL_ENUM_USN_DATA error=" << err << L"\n";
            break;
        }
        if (bytes_read == 0) break;

        PUSN_RECORD_V2 rec = reinterpret_cast<PUSN_RECORD_V2>(buffer.data());
        DWORD remaining = bytes_read;
        while (remaining > 0) {
            if (rec->RecordLength == 0 || rec->RecordLength > remaining) break;
            ++records;
            remaining -= rec->RecordLength;
            med.StartFileReferenceNumber = rec->FileReferenceNumber;
            rec = reinterpret_cast<PUSN_RECORD_V2>(reinterpret_cast<BYTE*>(rec) + rec->RecordLength);
        }
        if (truncated) break;

        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed > max_duration) {
            truncated = true;
            std::wcout << L"  stopped at 5-minute budget.\n";
            break;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = millis(t1 - t0);

    std::wcout << std::fixed << std::setprecision(2);
    std::wcout << L"records enum = " << records << (truncated ? L" (truncated)" : L"") << L"\n";
    std::wcout << L"time         = " << ms << L" ms\n";
    if (ms > 0) std::wcout << L"rate         = " << (records * 1000.0 / ms) << L" rec/s\n";

    double rec_per_s = (ms > 0) ? (records * 1000.0 / ms) : 0;
    // Rough guess: a consumer C: drive commonly holds ~1.0M files+dirs.
    const int64_t estimated_total = 1'000'000;
    if (records > 0 && rec_per_s > 0) {
        if (truncated) {
            double build_s = estimated_total / rec_per_s;
            std::wcout << L"  estimated full build time for " << estimated_total
                       << L" records = " << std::fixed << std::setprecision(1) << build_s << L" s\n";
            std::wcout << L"  memory estimate @ 80 bytes/record = "
                       << (estimated_total * 80 / (1024.0 * 1024.0)) << L" MB\n";
        } else {
            double build_s = records / rec_per_s;
            std::wcout << L"  full build time at measured rate = " << build_s << L" s\n";
            std::wcout << L"  memory estimate @ 80 bytes/record = "
                       << (records * 80 / (1024.0 * 1024.0)) << L" MB\n";
        }
    } else {
        std::wcout << L"  (no records enumerated; cannot estimate rate/memory)\n";
    }
}

static int usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [drive_letter]\n";
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

    if (argc > 2) return usage("pulse_bench_mft");

    wchar_t drive = L'C';
    if (argc == 2 && argv[1][0] != 0) {
        drive = static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(argv[1][0])));
    }

    std::wcout << L"=== MFT / USN benchmark on " << drive << L": ===\n";
    bool admin = is_admin();
    std::wcout << L"admin        = " << (admin ? L"yes" : L"no") << L"\n";
    if (!admin) {
        std::wcout << L"  (non-admin: FSCTL_ENUM_USN_DATA usually fails or returns truncated data; running in degraded mode.)\n";
    }

    HANDLE hvol = open_volume(drive);
    if (hvol == INVALID_HANDLE_VALUE) {
        std::wcerr << L"failed to open volume " << drive << L":\n";
        return 1;
    }

    query_usn_journal(hvol);
    read_usn_journal(hvol, 10000);

    enum_mft(hvol, drive);

    CloseHandle(hvol);
    return 0;
}
