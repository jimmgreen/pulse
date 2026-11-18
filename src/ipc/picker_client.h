#pragma once

#include "picker_transport.h"

namespace pulse::ipc::picker {
// Synchronous caller-thread bridge. Pumps messages, owns only its child host,
// and never opens a native fallback. Same-thread owners use owned modality.
// Cross-thread/process owners must already be disabled by their caller, be
// accessible and have the same user/session/integrity. They are never enabled
// or synchronously messaged here. An enabled external owner is NOT_SUPPORTED.
// Owner HWND/process creation identity and a window property guard cancellation;
// externally re-enabling/destroying it cancels the exchange.
// The optional cancellation event is borrowed and must remain valid until return.
Message RunPickerClient(std::wstring executable, Message request,
                        HWND owner = nullptr, HANDLE cancel = nullptr);
} // namespace pulse::ipc::picker
