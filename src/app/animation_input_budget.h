#pragma once
namespace pulse::app {
// Let queued input run first, but a stream of mouse moves must not indefinitely
// defer progress/hover frames to low-priority WM_PAINT. At most 8 ms of deferral
// also leaves room for 120 Hz displays; normal compositor pacing stays in charge.
inline bool YieldAnimationToInput(bool input_waiting, double since_frame_ms) noexcept {
    return input_waiting && since_frame_ms < 8.0;
}
}
