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

namespace pulse::app::hang {

void Start(HWND hwnd, const std::wstring& log_path);
void Stop();

// Call lifecycle and frame boundaries on the UI thread. Nested frames return 0.
// IDs can be joined to frame timing records. The monitor never uses DbgHelp.
bool StartFrameWatch(const std::wstring& log_path);
void StopFrameWatch();
uint64_t BeginFrame();
void EndFrame(uint64_t id);

} // namespace pulse::app::hang
