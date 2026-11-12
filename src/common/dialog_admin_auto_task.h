#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace pulse::dialog_admin_auto {
inline constexpr wchar_t kHelperName[] = L"PulseDialogAdminAuto.exe";
struct Identity { std::wstring sid; DWORD session = 0; };
struct TaskPlan { std::wstring name, root, executable, arguments, sid, xml, sddl; DWORD session = 0; };
struct Snapshot { bool exists = false, running = false; DWORD last_result = 0; std::wstring root, executable, xml; };
struct Result { HRESULT error = S_OK, rollback_error = S_OK; bool registered = false, running = false; };
struct Backend {
    std::function<HRESULT(Snapshot&)> inspect;
    std::function<HRESULT()> stop, remove, start;
    std::function<HRESULT(const std::wstring&)> put;
};
// Pure policy builders used by offline tests; production entry points validate live identity/ACLs first.
HRESULT BuildTaskPlan(const std::wstring& root, const std::wstring& program_files,
    const Identity& identity, const std::wstring& helper, TaskPlan& plan) noexcept;
Result ApplyTaskPlan(const TaskPlan& plan, bool start, const Backend& backend) noexcept;
HRESULT MatchInteractiveIdentity(const Identity& caller, const Identity& interactive) noexcept;
HRESULT InteractiveIdentity(Identity& identity, bool require_elevated) noexcept;
HRESULT ValidateInstalledRoot(const std::wstring& root, bool require_helper = true) noexcept;
// Worker-thread APIs: bundle/ACL checks and bounded Scheduler/helper readiness waits.
Result Configure(const std::wstring& root, bool start) noexcept;
HRESULT Restore(const std::wstring& root, const Snapshot& before) noexcept;
HRESULT Remove(const std::wstring& root) noexcept;
HRESULT StartExisting(const std::wstring& root) noexcept;
HRESULT Inspect(const std::wstring& root, Snapshot& snapshot) noexcept;
}
