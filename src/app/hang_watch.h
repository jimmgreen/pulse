// hang_watch.h — Diagnostic UI-stall sampler, enabled by --hang-watch.
// A background thread probes the main window with WM_NULL; when the UI thread
// stops answering for kStallMs it samples every thread's stack (suspend +
// RtlVirtualUnwind, no allocation while suspended) and appends the frames to
// a text log. pulse.exe frames are written as RVAs (resolve with pulse.map);
// system modules are resolved to their nearest export via dbghelp.
#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

#define PULSE_UI_PROBE_API 1
namespace pulse::app::hang {

// Posted nonce probe targets the process host; acknowledge directly on its UI
// thread. Context is the UI's last observation, not live worker progress.
UINT UiProbeMessage();
void SetProbeWindow(HWND window);
void AckUiProbe(uint64_t token);
void SetOperationContext(uint64_t task, uint32_t phase, uint64_t completed, uint64_t total);


void Start(HWND hwnd, const std::wstring& log_path);
void Stop();

// Call lifecycle and frame boundaries on the UI thread. Nested frames return 0.
// IDs can be joined to frame timing records. The monitor never uses DbgHelp.
bool StartFrameWatch(const std::wstring& log_path);
void StopFrameWatch();
uint64_t BeginFrame();
void EndFrame(uint64_t id);

// Non-render tracking reuses the frame monitor. Handler stacks start at 400ms;
// dispatches >=100ms enqueue bounded records for background runtime logging.
// A 500ms posted probe and 5s health summaries remain active after stack budgets.
uint64_t BeginDispatch(UINT message);
void EndDispatch(uint64_t id);
void NoteDispatchHeartbeat();
void PushModalPause();
void PopModalPause();

} // namespace pulse::app::hang
