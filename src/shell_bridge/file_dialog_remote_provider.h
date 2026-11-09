#pragma once
#include "file_dialog_inprocess_session.h"
namespace pulse::shell_bridge {
HRESULT LoadFileDialogRemoteProvider(const std::wstring&, std::shared_ptr<const FileDialogInprocessProvider>&, bool allow_unverified) noexcept;
}
