#pragma once

#include "file_dialog_wrapper.h"

namespace pulse::shell_bridge {
using CoCreateInstanceFunction = HRESULT (WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);

struct FileDialogActivation {
    bool enabled = false;
    CoCreateInstanceFunction original = &::CoCreateInstance;
    FileDialogBridgeServices services;
};

// Explicit adapter only: no hooks, registration or interception installation.
// original must be an unhooked callable (not this adapter). The caller owns an
// immutable configuration for the duration of each call. Only exact in-process
// STA Open/Save activations with known interfaces are wrapped. All other calls
// retain their original parameters, HRESULT and native object identity.
HRESULT ActivateFileDialog(const FileDialogActivation& activation, REFCLSID clsid,
                           LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) noexcept;
}
