// arrival_select.h - the items an action brings into the folder get selected.
//
// A paste, a drop, 压缩 or a shell verb (7-Zip's "extract here", "create
// shortcut") arms the pane's folder. Rows that were not listed when it was
// armed, or that the operation reports as its results, are selected as the
// listing shows them, with the first one centered in the pane. Pulse's own
// operations stay armed until shortly after their task ends; shell verbs for
// a few seconds after the last arrival. Changing the selection by hand, or
// leaving the folder, disarms it.
#pragma once
#include <cstdint>
#include <string>

namespace pulse {
struct AppState;
namespace app { struct Pane; struct Tab; }
namespace ops { struct CompletedOperation; struct FinishedTask; }

// Arms the folder `pane` shows (null: the focused pane). `task_id` 0 means
// no Pulse task: allow at least a minute for an external tool's first result,
// then settle after new arrivals. Known tasks select only reported outputs.
void ArmArrivalSelection(AppState& s, const app::Pane* pane, uint64_t task_id, uint32_t settle_ms);
// `path` will turn up in the armed folder (the archive 压缩 just wrote).
void ExpectArrival(AppState& s, const std::wstring& path);
// Ops results, for every window sharing `s`'s core.
void NoteArrivalCompletion(AppState& s, const ops::CompletedOperation& completed);
void NoteArrivalFinished(AppState& s, const ops::FinishedTask& finished);
// False for shell verbs that only open, show or remove things; true for the
// rest, unnamed handler commands (archivers) included.
bool ShellVerbMayCreate(const std::wstring& verb);
void CenterResultInPane(AppState& s, app::Pane& pane, app::Tab& tab, int index);
// After a listing update of `tab`; true when it changed the selection.
bool ApplyArrivalSelection(AppState& s, app::Pane& pane, app::Tab& tab, bool focused);
}
