#pragma once
#include "ops_manager.h"
#include "elevated_transfer.h"
#include "../common/virtual_disk_eject.h"

namespace pulse::ops {
struct ElevatedConflictAnswer {
    ConflictChoice choice = ConflictChoice::Cancel;
    bool apply_to_all = false;
};
struct ElevatedTransferCallbacks {
    bool verify_copies = false;
    std::function<void(bool awaiting)> authorization;
    std::function<void(const OpStatus&)> progress;
    std::function<ElevatedConflictAnswer(const ConflictItemInfo&)> conflict;
    std::function<bool()> paused;
};
// Serialized per process, authorized helper retained until shutdown/parent exit.
// UAC cancellation returns immediately; the same request is never relaunched.
ShellTransferResult TransferWithElevatedHelper(const std::vector<std::wstring>& sources,
    const std::wstring& destination, bool move, HWND owner, std::atomic<bool>& cancel,
    ShellCollisionPolicy policy = ShellCollisionPolicy::System,
    ElevatedTransferCallbacks callbacks = {}, const std::wstring& target_name = {}, bool rename_only = false);
ShellTransferResult DeleteWithElevatedHelper(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel,
    ElevatedTransferCallbacks callbacks = {});
ShellTransferResult EjectWithElevatedHelper(const storage::VirtualDiskIdentity& identity, HWND owner,
    std::atomic<bool>& cancel);
void ShutdownElevatedTransferHelper();
ShellTransferResult CreateFolderWithElevatedHelper(const std::wstring& path, HWND owner,
    std::atomic<bool>& cancel, ElevatedTransferCallbacks callbacks = {});
#ifdef PULSE_ELEVATED_TEST_CLIENT
DWORD ElevatedHelperProcessIdForTesting();
#endif
}
