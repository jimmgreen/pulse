#pragma once
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }
// Title / artist / album for the visible rows, read through the preview host
// so the Shell property handler stays off the window thread (#92).
void FillAudioMeta(AppState& state, ui::WindowViewModel& vm);
}
