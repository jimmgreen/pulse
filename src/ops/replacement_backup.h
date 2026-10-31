#pragma once
#include "verification_worker.h"
#include <atomic>

namespace pulse::ops {
struct BoundReplacementBackup {
    std::wstring original;
    std::wstring backup;
    VerificationStamp identity{};
};
// Cold conflict/recovery operations stay outside the per-entry copy coordinator.
// Capture and rename use one handle; restore never replaces a new occupant.
bool CreateBoundReplacementBackup(const std::wstring& original, const std::wstring& backup,
                                  BoundReplacementBackup& result, DWORD& error,
                                  const std::atomic<bool>* cancel = nullptr);
bool RestoreBoundReplacementBackup(const BoundReplacementBackup& backup, DWORD& error);
// Files/reparse points and empty directories only; never recursively deletes children.
// Cancellation at entry leaves the object unchanged.
bool CleanupBoundReplacementBackup(const BoundReplacementBackup& backup, DWORD& error,
                                   const std::atomic<bool>* cancel = nullptr);
} // namespace pulse::ops
