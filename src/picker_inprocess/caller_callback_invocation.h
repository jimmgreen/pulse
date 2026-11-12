#pragma once
#include <windows.h>
namespace pulse::picker_inprocess {
enum class CallerInvocationStage { None, Push, EnterDpi, EnterLanguage, Callback, RestoreDpi, RestoreLanguage, Pop };
struct CallerInvocationOutcome {
    HRESULT caller_result = E_PENDING;
    HRESULT infrastructure_result = S_OK;
    CallerInvocationStage failed_stage = CallerInvocationStage::None;
    bool caller_returned = false;
};
// Operations is injectable and performs no implicit IO here. A user's HRESULT
// is independent of infrastructure failure, including failures after it returns.
// Every successful push has one checked pop attempt; cleanup is best effort.
template<class Operations, class Callback>
CallerInvocationOutcome InvokeCallerCallback(Operations& operations, Callback callback) noexcept {
    CallerInvocationOutcome result;
    const auto fail = [&](CallerInvocationStage stage, HRESULT error) {
        if (result.infrastructure_result == S_OK) {
            result.infrastructure_result = FAILED(error) ? error : E_UNEXPECTED;
            result.failed_stage = stage;
        }
    };
    const auto step = [&](CallerInvocationStage stage) {
        HRESULT error = E_FAIL;
        try { error = operations.Step(stage); } catch (...) {}
        if (error != S_OK) { fail(stage, error); return false; }
        return true;
    };
    if (!step(CallerInvocationStage::Push)) return result;
    const bool entered = step(CallerInvocationStage::EnterDpi) && step(CallerInvocationStage::EnterLanguage);
    if (entered) {
        try { result.caller_result = callback(); result.caller_returned = true; }
        catch (...) { fail(CallerInvocationStage::Callback, E_FAIL); }
    }
    step(CallerInvocationStage::RestoreDpi);
    step(CallerInvocationStage::RestoreLanguage);
    step(CallerInvocationStage::Pop);
    return result;
}
}
