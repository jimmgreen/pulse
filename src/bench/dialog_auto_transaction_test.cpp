#include "../common/dialog_auto_transaction.h"
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

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
enum class Operation { Enable, EnableIfUnclaimed, DisableFor };
struct Call { Operation operation; Architecture architecture; std::wstring path; };
struct Fake {
    std::array<Status, 2> states;
    std::array<unsigned, 2> reads{};
    std::vector<Call> calls;
    std::function<std::optional<DWORD>(Operation, Architecture, const std::wstring&)> hook;
    std::function<void(Architecture, unsigned)> on_inspect;
    DWORD Change(Operation operation, Architecture architecture, const std::wstring& path) {
        calls.push_back({operation, architecture, path});
        if (hook) if (const auto result = hook(operation, architecture, path)) return *result;
        auto& state = states[Index(architecture)];
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
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--single-arch-baseline") return Baseline();
    Cases();
    std::printf("TRANSACTION_TEST checks=%u failures=%u api=injected_state native_registry_mutations=0\n", checks, failures);
    return failures ? 1 : 0;
}
