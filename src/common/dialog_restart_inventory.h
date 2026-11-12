#pragma once
#include "dialog_bundle.h"
#include "dialog_process_diagnostics.h"
#include <functional>
#include <string>
#include <vector>
namespace pulse::dialog_restart {
struct CurrentComponents {
    std::wstring install_root, x64_directory, x86_directory;
    DWORD x64_error = ERROR_NOT_FOUND, x86_error = ERROR_NOT_FOUND;
};
struct ProcessIdentity { DWORD pid = 0; uint64_t creation_time = 0; };
struct Record {
    std::wstring app_basename;
    uint64_t app_image_id = 0; // Opaque full image-path fingerprint, not binary attestation.
    std::vector<ProcessIdentity> processes;
    uint32_t old_component_count = 0;
};
struct Inventory {
    std::vector<Record> records;
    bool scan_complete = true, cancelled = false;
    uint32_t query_errors = 0, scanned_processes = 0;
    DWORD next_pid = 0; // Last sampled PID on a budget-limited pass; continue after it.
};
// Caller retains/revalidates leases when installed pointers change, not per process.
CurrentComponents CaptureCurrent(const std::wstring& install_root,
    const dialog_bundle::BundleLease* x64, const dialog_bundle::BundleLease* x86);
// Pure metadata reducer: model tests can supply owned synthetic snapshots.
Inventory Classify(const CurrentComponents& current,
    const std::vector<dialog_diagnostics::RestartProcessSnapshot>& processes);
// Pure scheduler; rotates sorted unique IDs so bounded scans do not starve later PIDs.
std::vector<DWORD> ProcessOrder(std::vector<DWORD> pids, DWORD start_after_pid);
// Worker only: <=65536 lightweight IDs; <=512 module queries, <=1500 ms between queries.
// No absence/failure is interpreted as a restart requirement. Cancel returns no records.
Inventory Scan(const CurrentComponents& current, const std::function<bool()>& cancelled = {},
    DWORD start_after_pid = 0);
}
