#include "../common/dialog_auto_transaction.h"
#include "../common/dialog_auto_ownership.h"
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <filesystem>
#include <fstream>

#ifdef PULSE_DIALOG_TRANSACTION_STANDALONE
#include "../common/runtime_log.h"
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
#endif

namespace {
using namespace pulse::dialog_auto;
constexpr auto x64 = Architecture::X64, x86 = Architecture::X86;
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* name) {
    ++checks; if (!okay) ++failures;
    std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", name); std::fflush(stdout);
}
size_t Index(Architecture architecture) { return architecture == x64 ? 0 : 1; }
std::wstring Path(Architecture architecture) { return std::wstring(L"C:\\PulseFixture\\") + DllNameFor(architecture); }
std::wstring OtherPath(Architecture architecture) { return std::wstring(L"C:\\OtherFixture\\") + DllNameFor(architecture); }
std::vector<RegistrationTarget> Targets() { return {{x64, Path(x64)}, {x86, Path(x86)}}; }
enum class Operation { Enable, EnableIfUnclaimed, EnableIfDisabledFor, DisableFor };
struct Call { Operation operation; Architecture architecture; std::wstring path; std::wstring old_path = {}; };
struct Fake {
    std::array<Status, 2> states;
    std::array<unsigned, 2> reads{};
    std::vector<Call> calls;
    std::function<std::optional<DWORD>(Operation, Architecture, const std::wstring&)> hook;
    std::function<void(Architecture, unsigned)> on_inspect;
    DWORD Change(Operation operation, Architecture architecture, const std::wstring& path, const std::wstring& old_path = {}) {
        calls.push_back({operation, architecture, path, old_path});
        if (hook) if (const auto result = hook(operation, architecture, path)) return *result;
        auto& state = states[Index(architecture)];
        if (operation == Operation::EnableIfDisabledFor) {
            if (state.enabled || state.conflict || state.dll_path != old_path) return ERROR_ALREADY_EXISTS;
            state = {true, false, ERROR_SUCCESS, path};
            return ERROR_SUCCESS;
        }
        if (!state.dll_path.empty() && state.dll_path != path) return ERROR_ALREADY_EXISTS;
        if (operation == Operation::DisableFor) { state.enabled = false; state.conflict = false; state.error = 0; }
        else state = {true, false, ERROR_SUCCESS, path};
        return ERROR_SUCCESS;
    }
    RegistrationApi Api() {
        return {
            [this](Architecture architecture) {
                const auto n = ++reads[Index(architecture)];
                if (on_inspect) on_inspect(architecture, n);
                return states[Index(architecture)];
            },
            [this](Architecture architecture, const std::wstring& path) { return Change(Operation::Enable, architecture, path); },
            [this](Architecture architecture, const std::wstring& path) { return Change(Operation::EnableIfUnclaimed, architecture, path); },
            [this](Architecture architecture, const std::wstring& path) { return Change(Operation::DisableFor, architecture, path); },
        };
    }
    RegistrationApi ApiWithRebind() {
        auto api = Api();
        api.enable_if_disabled_for = [this](Architecture architecture, const std::wstring& old_path, const std::wstring& new_path) {
            return Change(Operation::EnableIfDisabledFor, architecture, new_path, old_path);
        };
        return api;
    }
    void Seed(bool enabled) {
        states[0] = {enabled, false, 0, Path(x64)};
        states[1] = {enabled, false, 0, Path(x86)};
    }
};
bool LockBlocksAnotherThread() {
    bool blocked = false;
    std::thread contender([&] {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Pulse.DialogAuto.Registration.v1");
        if (!mutex) return;
        const auto result = WaitForSingleObject(mutex, 0);
        blocked = result == WAIT_TIMEOUT;
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(mutex);
        CloseHandle(mutex);
    });
    contender.join(); return blocked;
}
void Cases() {
    {
        Fake fake;
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(!result.error && !result.rollback_error && result.attempted_count == 2 && fake.states[0].enabled && fake.states[1].enabled,
            "dual enable succeeds through two unclaimed operations");
        Check(fake.calls.size() == 2 && fake.calls[0].operation == Operation::EnableIfUnclaimed && fake.calls[1].operation == Operation::EnableIfUnclaimed,
            "forward enable never uses overwrite-capable restoration API");
        const auto disabled = ExecuteRegistrationChange(Targets(), false, fake.Api());
        Check(!disabled.error && disabled.attempted_count == 2 && !fake.states[0].enabled && !fake.states[1].enabled,
            "dual disable succeeds while retaining bound journals");
    }
    for (const bool partial : {false, true}) {
        Fake fake;
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring& path) -> std::optional<DWORD> {
            if (operation != Operation::EnableIfUnclaimed || architecture != x86) return {};
            if (partial) fake.states[1] = {false, true, 0, path};
            return ERROR_ACCESS_DENIED;
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && !result.rollback_error && result.rollback_complete && result.rollback_attempted_count == 2 &&
            result.targets[1].rollback_attempted && !fake.states[0].enabled && !fake.states[1].enabled && !fake.states[1].conflict,
            partial ? "failed second architecture partial write is included in reverse rollback" : "failed second architecture restores first and also verifies failed architecture");
        Check(fake.calls.size() == 4 && fake.calls[2].architecture == x86 && fake.calls[3].architecture == x64,
            "rollback runs in reverse attempted order including failure");
        Check(fake.states[0].dll_path == Path(x64) && result.targets[0].before.dll_path.empty() && result.targets[0].behavior_restored,
            "recovery verifies disabled behavior without claiming byte-identical empty journal");
    }
    {
        Fake fake; fake.Seed(true);
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            if (operation == Operation::DisableFor && architecture == x86) { fake.states[1].enabled = false; return ERROR_ACCESS_DENIED; }
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), false, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && result.rollback_complete && fake.states[0].enabled && fake.states[1].enabled,
            "failed disable restores both previously enabled architectures");
        Check(fake.calls[2].operation == Operation::Enable && fake.calls[3].operation == Operation::Enable,
            "enabled snapshots restore with native Enable and original paths");
    }
    {
        Fake fake; fake.Seed(false);
        const auto enabled = ExecuteRegistrationChange(Targets(), true, fake.Api());
        const auto noop = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(!enabled.error && !noop.error && noop.attempted_count == 0 && fake.calls.size() == 2,
            "already-enabled matching targets are nonmutating no-ops");
        fake.Seed(false); fake.calls.clear();
        const auto disabled = ExecuteRegistrationChange(Targets(), false, fake.Api());
        Check(!disabled.error && !fake.states[0].enabled && !fake.states[1].enabled && fake.states[0].dll_path == Path(x64),
            "already-disabled matching target remains disabled with original path constraint");
    }
    {
        Fake fake; fake.states[0] = {true, false, 0, Path(x64)};
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring& path) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) { fake.states[1] = {false, true, 0, path}; return ERROR_ACCESS_DENIED; }
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && result.attempted_count == 1 && result.rollback_attempted_count == 1 &&
            result.rollback_complete && fake.states[0].enabled && !fake.states[1].enabled && !result.targets[0].attempted,
            "failed new architecture never disables pre-existing matching enabled architecture");
    }
    {
        Fake fake; const std::wstring original = L"c:\\PulseFixture\\pulsedialogauto64.DLL";
        fake.states[0] = {false, false, 0, original};
        const auto result = ExecuteRegistrationChange({{x64, Path(x64)}}, true, fake.Api());
        Check(!result.error && fake.states[0].enabled && fake.calls[0].path == original && result.targets[0].after.dll_path == original,
            "same-path case aliases retain original journal spelling and path constraint");
    }
    {
        Fake fake; fake.states[1].dll_path = OtherPath(x86);
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_ALREADY_EXISTS && fake.calls.empty(), "disabled foreign bundle blocks entire enable before any write");
    }
    {
        Fake fake; fake.Seed(true);
        auto targets = Targets(); targets[1].dll_path = OtherPath(x86);
        const auto result = ExecuteRegistrationChange(targets, false, fake.Api());
        Check(result.error == ERROR_ALREADY_EXISTS && fake.calls.empty() && fake.states[0].enabled,
            "mismatched disable returns error rather than deleting or pretending success");
        Fake absent;
        Check(ExecuteRegistrationChange(Targets(), false, absent.Api()).error == ERROR_NOT_FOUND && absent.calls.empty(),
            "absent disable target is reported as missing, not silently successful");
    }
    for (const bool conflict : {false, true}) {
        Fake fake; fake.states[1].conflict = conflict; fake.states[1].error = conflict ? 0 : ERROR_ACCESS_DENIED;
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == static_cast<DWORD>(conflict ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED) && fake.calls.empty(),
            conflict ? "snapshot conflict blocks all writes" : "snapshot inspection error blocks all writes");
    }
    {
        Fake fake;
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) return ERROR_ACCESS_DENIED;
            if (operation == Operation::DisableFor && architecture == x64) return ERROR_WRITE_FAULT;
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error == ERROR_WRITE_FAULT && !result.rollback_complete && fake.states[0].enabled,
            "rollback failure is explicit and never hidden behind primary error");
    }
    {
        Fake fake;
        fake.on_inspect = [&](Architecture architecture, unsigned n) {
            if (architecture == x86 && n == 2) fake.states[1] = {true, false, 0, OtherPath(x86)};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_REVISION_MISMATCH && result.rollback_complete && !fake.states[0].enabled && fake.states[1].dll_path == OtherPath(x86),
            "per-step reinspection preserves foreign concurrent state and restores only our attempts");
        Check(!result.targets[1].attempted && result.targets[1].after.dll_path == OtherPath(x86),
            "unattempted foreign architecture is not included in rollback");
    }
    {
        Fake fake;
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) {
                fake.states[0] = {true, false, 0, OtherPath(x64)}; return ERROR_ACCESS_DENIED;
            }
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && result.rollback_error == ERROR_REVISION_MISMATCH && !result.rollback_complete && fake.states[0].dll_path == OtherPath(x64),
            "rollback reinspection never overwrites a foreign concurrent owner");
        Check(fake.calls.size() == 3 && !result.targets[0].rollback_attempted,
            "foreign rollback target receives no mutation API call");
    }
    {
        Fake fake;
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring& path) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) {
                fake.states[1] = {false, true, 0, path}; throw std::runtime_error("injected");
            }
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_UNHANDLED_EXCEPTION && result.rollback_complete && !fake.states[0].enabled && !fake.states[1].enabled,
            "throw after partial mutation still rolls back failed architecture");
    }
    {
        Fake fake;
        fake.hook = [](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) return ERROR_SUCCESS;
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.error == ERROR_INVALID_DATA && result.rollback_complete && !fake.states[0].enabled,
            "API success without requested post-state is a failed transaction");
    }
    {
        Fake fake;
        fake.on_inspect = [&](Architecture architecture, unsigned n) {
            if (architecture == x64) fake.states[0].error = n == 3 ? ERROR_ACCESS_DENIED : 0;
        };
        const auto result = ExecuteRegistrationChange({{x64, Path(x64)}}, true, fake.Api());
        Check(result.error == ERROR_ACCESS_DENIED && result.rollback_complete && !fake.states[0].enabled,
            "post-change inspection failure triggers verified recovery");
    }
    {
        Fake fake; unsigned lock_checks = 0; bool held = true;
        fake.on_inspect = [&](Architecture, unsigned) { ++lock_checks; held = held && LockBlocksAnotherThread(); };
        fake.hook = [&](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            ++lock_checks; held = held && LockBlocksAnotherThread();
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) return ERROR_ACCESS_DENIED;
            return {};
        };
        const auto result = ExecuteRegistrationChange(Targets(), true, fake.Api());
        Check(result.rollback_complete && held && lock_checks >= 10,
            "one global mutex remains held during snapshots, changes and reverse rollback");
    }
    {
        Fake fake;
        Check(ExecuteRegistrationChange({{x64, Path(x64)}, {x64, Path(x64)}}, true, fake.Api()).error == ERROR_INVALID_PARAMETER && fake.calls.empty() && fake.reads[0] == 0,
            "duplicate architecture is rejected before any state inspection or mutation");
        Check(ExecuteRegistrationChange({}, true, fake.Api()).error == ERROR_INVALID_PARAMETER && fake.calls.empty(), "empty target set is rejected");
        Check(ExecuteRegistrationChange({{static_cast<Architecture>(9), Path(x64)}}, true, fake.Api()).error == ERROR_INVALID_PARAMETER && fake.calls.empty(),
            "unknown architecture is rejected");
        RegistrationApi partial; partial.inspect = fake.Api().inspect;
        Check(ExecuteRegistrationChange(Targets(), true, partial).error == ERROR_INVALID_PARAMETER && fake.reads[0] == 0,
            "partial injection cannot fall through to native registry operations");
    }
    for (const auto path : {
        L"PulseDialogAuto32.dll", L"C:PulseDialogAuto32.dll", L"C:/PulseFixture/PulseDialogAuto32.dll",
        L"C:\\PulseFixture\\..\\PulseDialogAuto32.dll", L"C:\\PulseFixture\\.\\PulseDialogAuto32.dll",
        L"C:\\PulseFixture\\\\PulseDialogAuto32.dll", L"C:\\PulseFixture \\PulseDialogAuto32.dll",
        L"C:\\PulseFixture\\PulseDialogAuto64.dll", L"C:\\PulseFixture\\PulseDialogAuto32.dll:stream",
        L"\\\\server\\share\\PulseDialogAuto32.dll", L"C:\\NUL\\PulseDialogAuto32.dll"}) {
        Fake fake;
        const auto result = ExecuteRegistrationChange({{x64, Path(x64)}, {x86, path}}, true, fake.Api());
        Check(result.error == ERROR_INVALID_NAME && fake.calls.empty() && fake.reads[0] == 0,
            "all target paths are validated before the first snapshot or write");
    }
    {
        // Mixed x64 disabled-rebind + x86 unclaimed in a single transaction.
        Fake fake;
        const std::wstring old_x64 = L"C:\\OldPulse\\DialogBridge\\aaaa\\PulseDialogAuto64.dll";
        fake.states[0] = {false, false, 0, old_x64};
        fake.states[1] = {};
        const std::vector<RegistrationTarget> mixed{
            {x64, Path(x64), old_x64},
            {x86, Path(x86)}
        };
        const auto ok = ExecuteRegistrationChange(mixed, true, fake.ApiWithRebind());
        Check(!ok.error && ok.attempted_count == 2 && fake.calls.size() == 2 &&
            fake.calls[0].operation == Operation::EnableIfDisabledFor && fake.calls[0].old_path == old_x64 &&
            fake.calls[0].path == Path(x64) && fake.calls[1].operation == Operation::EnableIfUnclaimed &&
            fake.states[0].enabled && fake.states[0].dll_path == Path(x64) &&
            fake.states[1].enabled && fake.states[1].dll_path == Path(x86),
            "mixed x64 disabled-rebind and x86 unclaimed succeed in one transaction");

        // Rollback after x64 disabled-rebind succeeds and x86 fails: must disable new x64 target path.
        Fake rb;
        rb.states[0] = {false, false, 0, old_x64};
        rb.states[1] = {};
        rb.hook = [&](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
            if (operation == Operation::EnableIfUnclaimed && architecture == x86) return ERROR_ACCESS_DENIED;
            return {};
        };
        const auto rolled = ExecuteRegistrationChange(mixed, true, rb.ApiWithRebind());
        Check(rolled.error == ERROR_ACCESS_DENIED && rolled.rollback_complete && !rb.states[0].enabled &&
            rb.calls.size() == 4 &&
            rb.calls[2].operation == Operation::DisableFor && rb.calls[2].architecture == x86 &&
            rb.calls[3].operation == Operation::DisableFor && rb.calls[3].architecture == x64 &&
            rb.calls[3].path == Path(x64),
            "rollback of completed x64 disabled-rebind disables newly bound x64 target path");

        // Foreign disabled dll_path (!= expected_old_dll_path) is still refused before any writes.
        Fake foreign;
        foreign.states[0] = {false, false, 0, OtherPath(x64)};
        const auto refused = ExecuteRegistrationChange(mixed, true, foreign.ApiWithRebind());
        Check(refused.error == ERROR_ALREADY_EXISTS && foreign.calls.empty(),
            "foreign disabled dll_path still fails preflight with ERROR_ALREADY_EXISTS");

        // Disable mode must reject targets carrying expected_old_dll_path.
        Fake invalid_disable;
        Check(ExecuteRegistrationChange(mixed, false, invalid_disable.ApiWithRebind()).error == ERROR_INVALID_PARAMETER &&
            invalid_disable.calls.empty(),
            "disable transaction rejects expected_old_dll_path");
    }
}
int Baseline() {
    Fake fake;
    fake.hook = [](Operation operation, Architecture architecture, const std::wstring&) -> std::optional<DWORD> {
        if (operation == Operation::EnableIfUnclaimed && architecture == x86) return ERROR_ACCESS_DENIED;
        return {};
    };
    const DWORD first = fake.Change(Operation::EnableIfUnclaimed, x64, Path(x64));
    const DWORD second = fake.Change(Operation::EnableIfUnclaimed, x86, Path(x86));
    std::printf("BASELINE_SINGLE_ARCH_ADAPTER first_error=%lu second_error=%lu first_enabled=%u rollback_attempts=0\n",
        first, second, fake.states[0].enabled ? 1u : 0u);
    std::printf("BASELINE_SCOPE=injected independent per-architecture calls, not native registry or a rebuilt historical binary\n");
    return fake.states[0].enabled && second == ERROR_ACCESS_DENIED ? 1 : 2;
}
}
namespace {
void RetiredCases() {
    namespace fs = std::filesystem;
    const auto root = fs::absolute(L"bench_data") /
        (L"retired-transaction-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto old_directory = root / L"Old Install" / L"DialogBridge" / std::wstring(64, L'a');
    const auto old64 = (old_directory / DllNameFor(x64)).wstring();
    const auto old32 = (old_directory / DllNameFor(x86)).wstring();
    const std::vector<RegistrationTarget> targets{{x64, Path(x64), old64, true}, {x86, Path(x86), old32, true}};
    const auto seed = [&](Fake& fake) {
        fake.states[0] = {false, false, 0, old64}; fake.states[1] = {false, false, 0, old32};
    };
    const auto api_for = [&](Fake& fake) {
        auto api = fake.ApiWithRebind();
        api.enable_if_retired_disabled_for = [&fake](Architecture arch, const std::wstring& old_path,
            const std::wstring& new_path, bool& attempted) {
            attempted = false;
            if (!IsRetiredDisabledPulseDialogDll(arch, fake.states[Index(arch)])) return DWORD{ERROR_ALREADY_EXISTS};
            attempted = true;
            return fake.Change(Operation::EnableIfDisabledFor, arch, new_path, old_path);
        };
        return api;
    };
    {
        Fake fake; seed(fake); fake.states[1] = {};
        const auto result = ExecuteRegistrationChange({targets[0], {x86, Path(x86)}}, true, api_for(fake));
        Check(!result.error && result.attempted_count == 2 && fake.states[0].enabled && fake.states[1].enabled,
            "retired x64 and unclaimed x86 use their separate endpoints in one transaction");
    }
    {
        Fake fake; seed(fake); auto api = api_for(fake); const auto change = api.enable_if_retired_disabled_for;
        api.enable_if_retired_disabled_for = [&](Architecture arch, const std::wstring& old_path,
            const std::wstring& new_path, bool& attempted) {
            if (arch == x86) { attempted = false; return DWORD{ERROR_INVALID_DATA}; }
            return change(arch, old_path, new_path, attempted);
        };
        const auto result = ExecuteRegistrationChange(targets, true, api);
        Check(result.error == ERROR_INVALID_DATA && !result.rollback_error && result.attempted_count == 1 &&
            result.rollback_attempted_count == 1 && !result.targets[1].attempted && !result.targets[1].rollback_attempted &&
            !fake.states[0].enabled && !fake.states[1].enabled && fake.states[1].dll_path == old32 &&
            fake.calls.size() == 2 && fake.calls.back().architecture == x64,
            "x86 precondition refusal leaves x86 untouched and rolls back only modified x64");
    }
    {
        Fake fake; seed(fake); auto api = api_for(fake);
        api.enable_if_retired_disabled_for = [](Architecture, const std::wstring&, const std::wstring&, bool& attempted) {
            attempted = false; return DWORD{ERROR_FILE_NOT_FOUND};
        };
        const auto result = ExecuteRegistrationChange(targets, true, api);
        Check(result.error == ERROR_FILE_NOT_FOUND && !result.attempted_count && !result.rollback_attempted_count &&
            fake.calls.empty() && fake.states[0].dll_path == old64,
            "retired precondition refusal does not manufacture zero flags through rollback");
    }
    {
        Fake fake; seed(fake); auto api = api_for(fake); const auto change = api.enable_if_retired_disabled_for;
        api.enable_if_retired_disabled_for = [&](Architecture arch, const std::wstring& old_path,
            const std::wstring& new_path, bool& attempted) -> DWORD {
            change(arch, old_path, new_path, attempted);
            throw std::runtime_error("after retired mutation");
        };
        const auto result = ExecuteRegistrationChange(targets, true, api);
        Check(result.error == ERROR_UNHANDLED_EXCEPTION && !result.rollback_error && result.attempted_count == 1 &&
            result.rollback_attempted_count == 1 && !fake.states[0].enabled && fake.states[1].dll_path == old32,
            "retired callback exception preserves mutation marker and triggers recovery");
    }
    {
        Fake fake; seed(fake); auto api = api_for(fake);
        api.enable_if_retired_disabled_for = {};
        const auto result = ExecuteRegistrationChange(targets, true, api);
        Check(result.error == ERROR_INVALID_PARAMETER && fake.calls.empty(), "retired target cannot downgrade to ordinary rebind callback");
        for (const auto& target : std::vector<RegistrationTarget>{{x64, Path(x64), {}, true}, {x64, Path(x64), Path(x64), true}}) {
            const auto invalid = ExecuteRegistrationChange({target}, true, api_for(fake));
            Check(invalid.error == ERROR_INVALID_PARAMETER && fake.calls.empty(), "retired target requires a distinct expected old identity");
        }
        const auto invalid = ExecuteRegistrationChange(targets, false, api_for(fake));
        Check(invalid.error == ERROR_INVALID_PARAMETER && fake.calls.empty(), "retired constraint is enable-only");
    }
    {
        Fake fake; seed(fake); fake.states[1].conflict = true;
        const auto result = ExecuteRegistrationChange(targets, true, api_for(fake));
        Check(result.error == ERROR_ALREADY_EXISTS && fake.calls.empty(), "other architecture conflict blocks retired batch before all mutation");
    }
    {
        Check(fs::create_directories(old_directory), "create exclusively owned retired transaction fixture");
        Fake fake; seed(fake);
        fake.on_inspect = [&](Architecture arch, unsigned reads) {
            if (arch == x64 && reads == 2) { std::ofstream file(old64, std::ios::binary); file << "restored"; }
        };
        const auto result = ExecuteRegistrationChange(targets, true, api_for(fake));
        Check(result.error == ERROR_ALREADY_EXISTS && !result.attempted_count && !result.rollback_attempted_count && fake.calls.empty(),
            "old file reappearing after transaction preflight is refused without rollback writes");
        const auto base = fs::weakly_canonical(fs::absolute(L"bench_data"));
        Check(fs::weakly_canonical(root).parent_path() == base && fs::is_regular_file(old64), "verify owned transaction cleanup boundary");
        if (fs::weakly_canonical(root).parent_path() == base) fs::remove_all(root);
    }
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--single-arch-baseline") return Baseline();
    Cases();
    RetiredCases();
    std::printf("TRANSACTION_TEST checks=%u failures=%u api=injected_state native_registry_mutations=0\n", checks, failures);
    return failures ? 1 : 0;
}
