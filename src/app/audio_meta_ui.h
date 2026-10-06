#pragma once
#include "entry_sort.h"
#include <memory>
#include <string>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
namespace ui { struct WindowViewModel; }

// Title / artist / album for the details rows, read through the preview host
// so the Shell property handler stays off the window thread (#92).
void FillAudioMeta(AppState& state, ui::WindowViewModel& vm);

// The tags known so far for a Title / Artist / Album sort, in the shape the
// worker sorts with; also records what the rows were sorted with so the frame
// timer knows when more tags have landed. Null when the tab sorts by something
// else.
std::shared_ptr<const app::AudioMetaLookup> SortAudioMeta(AppState& state, app::Tab& tab,
                                                        const std::wstring& path);

// Moves the rows once more tags have come back, at most every so often and
// never while a press, drag or rename is in flight. True means something is
// still waiting to move, so the frame timer keeps asking.
bool ResortForAudioMeta(AppState& state);
}
