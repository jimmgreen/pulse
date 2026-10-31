#pragma once

#include "file_dialog_wrapper.h"

namespace pulse::shell_bridge {
using CoCreateInstanceExFunction = HRESULT (WINAPI*)(REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD, MULTI_QI*);
struct FileDialogActivationEx {
    bool enabled = false;
    CoCreateInstanceExFunction original = &::CoCreateInstanceEx;
    FileDialogBridgeServices services;
};

// Explicit local adapter, not an installed hook. original receives every input
// unchanged exactly once. Only all-S_OK, same-object Open/Save combinations of
// 1..64 known IIDs on an STA with exact INPROC and no server/outer are wrapped.
// Remote/security settings, partial success, unknown interfaces and failures
// retain the original aggregate HRESULT and every MULTI_QI field/interface.
// A nonzero incoming result HRESULT is passed through and does not veto an
// otherwise valid native S_OK output; non-null incoming interface slots bypass.
// Wrapping is staged for the entire array; failure never partially commits.
// Caller supplies the normal COM-valid array/IID pointers and owns configuration
// for the duration of the call. Missing original returns E_INVALIDARG untouched.
HRESULT ActivateFileDialogEx(const FileDialogActivationEx& activation, REFCLSID clsid,
    IUnknown* outer, DWORD context, COSERVERINFO* server, DWORD count, MULTI_QI* results) noexcept;
}
