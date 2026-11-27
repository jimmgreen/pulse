#pragma once
#include "../common/browse_request.h"
#include "../common/direct_reveal_client.h"
#include <cstdint>

namespace pulse::explorer_bridge::cold_start {
enum class EndpointState { Absent, Starting, Ready };
struct Outcome {
    DirectRevealResult result = DirectRevealResult::Rejected;
    DWORD error = ERROR_NOT_READY;
};
class Hooks {
public:
    virtual ~Hooks() = default;
    virtual uint64_t Now() noexcept = 0;
    virtual void Pause(DWORD milliseconds) noexcept = 0;
    virtual EndpointState Find() noexcept = 0;
    virtual DWORD Acquire(DWORD timeout_ms) noexcept = 0;
    virtual void Release() noexcept = 0;
    virtual DWORD Start() noexcept = 0;
    virtual Outcome Submit(const browse::Request& request, bool wait_for_ready, uint64_t deadline_tick) noexcept = 0;
};
// The mutex coordinates startup only. Submission has its own request ID and is
// invoked exactly once, including rejection and uncertain acknowledgement.
// Cold submission may retry explicit non-admission with that same ID until the
// original startup deadline; a verified warm endpoint has no readiness grace.
Outcome Deliver(const browse::Request& request, Hooks& hooks, DWORD ready_timeout_ms = 8000) noexcept;
Outcome Deliver(const browse::Request& request, DWORD ready_timeout_ms = 8000) noexcept;
}
