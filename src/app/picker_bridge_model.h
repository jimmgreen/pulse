#pragma once

#include "../ipc/picker_protocol.h"
#include "../ui/folder_picker_dialog.h"

#include <variant>

namespace pulse::app {

// A spec is ready for the host UI. A message is an immediate terminal response.
// No windows, IPC or filesystem operations occur in either function.
using PickerBridgePreparation = std::variant<ui::FolderPickerSpec, ipc::picker::Message>;
PickerBridgePreparation PreparePickerBridge(const ipc::picker::Message& request);
ipc::picker::Message CompletePickerBridge(const ipc::picker::Message& request,
                                         const ui::FilePickerOutcome& outcome,
                                         ipc::picker::ErrorSource failure_source = ipc::picker::ErrorSource::Win32);

} // namespace pulse::app
