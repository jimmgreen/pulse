#include "../ipc/win_e_broker_protocol.h"
#include "../app/win_e_chord.h"
#include <cstdio>
#include <thread>
#include <atomic>
using namespace pulse::win_e_broker;
int main() {
    unsigned failures = 0;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); if (!value) ++failures;
    };
    Shared shared;
    Initialize(shared, 42, 3, 123456);
    check(ValidHeader(shared, 42, 3, 123456), "valid parent identity and fixed-size header");
    check(!ValidHeader(shared, 43, 3, 123456) && !ValidHeader(shared, 42, 4, 123456) &&
        !ValidHeader(shared, 42, 3, 123457), "PID, session and creation identity mismatches rejected");
    shared.magic ^= 1;
    check(!ValidHeader(shared, 42, 3, 123456), "foreign protocol magic rejected");
    shared.magic = kMagic; shared.version++;
    check(!ValidHeader(shared, 42, 3, 123456), "unknown protocol version rejected");
    shared.version = kVersion; shared.bytes++;
    check(!ValidHeader(shared, 42, 3, 123456), "wrong mapping layout rejected");
    Initialize(shared, 42, 3, 123456);
    Nonce nonce{}, other{};
    check(ParseNonce(L"0123456789abcdef0123456789ABCDEF", nonce) &&
        NonceText(nonce) == L"0123456789abcdef0123456789abcdef", "strict 128-bit nonce roundtrip");
    check(!ParseNonce(L"0123", other) && !ParseNonce(L"g123456789abcdef0123456789abcdef", other),
        "short and nonhex nonce rejected without partial output");
    other = nonce; other[0] ^= 1;
    check(MappingName(42, nonce) != MappingName(42, other) && MappingName(42, nonce) != MappingName(43, nonce) &&
        StopEventName(42, nonce) == MappingName(42, nonce) + L".Stop", "nonce and parent isolate all session object names");
    check(LoadOwner(shared) == Owner::Medium && LoadState(shared) == SlotState::Empty && shared.chord_flags == 0,
        "new session is unarmed with no inherited chord");
    check(Transition(shared, SlotState::Empty, SlotState::Writing) &&
        !Transition(shared, SlotState::Empty, SlotState::Writing), "only one offer writer owns the slot");
    InterlockedIncrement64(&shared.sequence); InterlockedExchange(&shared.chord_flags, 3);
    check(Transition(shared, SlotState::Writing, SlotState::Offered) && LoadSequence(shared) == 1 && shared.chord_flags == 3,
        "published offer exposes stable sequence and accepted chord ownership");
    check(Transition(shared, SlotState::Offered, SlotState::Claimed) &&
        !Transition(shared, SlotState::Offered, SlotState::Rejected) &&
        Transition(shared, SlotState::Claimed, SlotState::Admitted), "claimed request cannot be cancelled through stale offered state");
    check(!Transition(shared, SlotState::Admitted, SlotState::Empty), "admitted request cannot be silently discarded");
    bool races = true;
    for (unsigned i = 0; i < 100; ++i) {
        InterlockedExchange(&shared.state, static_cast<LONG>(SlotState::Admitted));
        std::atomic<unsigned> winners{0};
        std::thread dispatched([&] { if (Transition(shared, SlotState::Admitted, SlotState::Dispatched)) ++winners; });
        std::thread cancelled([&] { if (Transition(shared, SlotState::Admitted, SlotState::Rejected)) ++winners; });
        dispatched.join(); cancelled.join();
        const auto terminal = LoadState(shared);
        races &= winners == 1 && (terminal == SlotState::Dispatched || terminal == SlotState::Rejected) &&
            !Transition(shared, terminal, terminal == SlotState::Dispatched ? SlotState::Rejected : SlotState::Dispatched);
        races &= Transition(shared, terminal, SlotState::Empty);
    }
    check(races, "dispatch versus cancellation has exactly one terminal winner across 100 races");
    pulse::app::WinEChord inherited;
    inherited.down = (shared.chord_flags & 1) != 0;
    inherited.accepted = (shared.chord_flags & 2) != 0;
    check(inherited.Press(true) == pulse::app::WinEChord::Action::Swallow && inherited.Release() &&
        inherited.Press(true) == pulse::app::WinEChord::Action::Submit,
        "inherited held accepted chord consumes repeat/up before admitting a new cycle");
    InterlockedExchange(&shared.state, 99);
    check(!Transition(shared, static_cast<SlotState>(99), SlotState::Empty), "unknown slot states fail closed");
    check(CreationTime(GetCurrentProcess()) != 0, "real process creation identity is available");
    return failures ? 1 : 0;
}
