#pragma once
#include "transfer_plan.h"
#include <windows.h>
#include <atomic>
namespace pulse::extensions::transfer_snapshot {
// Worker-only read leases. No mutation permission, and no path-based execution
// guarantee after release. Size/mtime is a version hint, not immutable content.
class Handles {
public:
    Handles() = default;
    ~Handles();
    Handles(Handles&& other) noexcept;
    Handles& operator=(Handles&& other) noexcept;
    Handles(const Handles&) = delete;
    Handles& operator=(const Handles&) = delete;
    HANDLE Leaf() const;
    HANDLE Parent() const;
    void Reset();
    void Add(HANDLE handle);
private:
    std::vector<HANDLE> chain_;
};
struct Source { transfer_plan::SourceFile snapshot; Handles handles; };
struct Target { transfer_plan::TargetDirectory snapshot; Handles handles; };
template<class T> struct Result {
    DWORD error = ERROR_SUCCESS;
    std::optional<T> value;
    explicit operator bool() const { return error == ERROR_SUCCESS && value.has_value(); }
};
// Cancellation is observed between opens/queries; it cannot preempt a synchronous
// filesystem driver call. Snapshot leases exclude new write/delete opens.
// The executor target lease permits directory writes for native relative rename,
// but excludes delete sharing; it does not freeze directory metadata.
Result<Source> CaptureSource(const std::wstring& path, const std::atomic<bool>& cancelled);
Result<Target> CaptureTarget(const std::wstring& path, const std::atomic<bool>& cancelled);
Result<Source> ReopenSource(const transfer_plan::SourceFile& expected, const std::atomic<bool>& cancelled);
Result<Source> AcquireTransferSource(const transfer_plan::SourceFile& expected, bool move, const std::atomic<bool>& cancelled);
DWORD VerifyTransferTarget(HANDLE handle, const transfer_plan::TargetDirectory& expected);
Result<Target> AcquireTransferTarget(const transfer_plan::TargetDirectory& expected, const std::atomic<bool>& cancelled);
Result<Target> ReopenTarget(const transfer_plan::TargetDirectory& expected, const std::atomic<bool>& cancelled);
}
