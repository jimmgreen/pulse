#pragma once

#include "file_dialog_wrapper.h"

namespace pulse::shell_bridge {
using CoGetClassObjectFunction = HRESULT (WINAPI*)(REFCLSID, DWORD, LPVOID, REFIID, LPVOID*);
struct FileDialogClassActivation {
    bool enabled = false;
    CoGetClassObjectFunction original = &::CoGetClassObject;
    FileDialogBridgeServices services;
};

// Explicit local STA/exact-INPROC Open/Save adapter, not an installed hook.
// Initial unknown factory IIDs bypass unchanged; IClassFactory2-capable native
// factories also bypass entirely. A returned proxy supports ONLY IUnknown and
// IClassFactory: later unknown factory QI returns E_NOINTERFACE, never an inner
// identity. This is an explicit interface-set limitation, not arbitrary native
// factory compatibility. CreateInstance unknown *object* IIDs instead forward
// unchanged to the native factory. LockServer calls must be balanced by callers.
HRESULT ActivateFileDialogClass(const FileDialogClassActivation& activation, REFCLSID clsid,
    DWORD context, LPVOID server, REFIID iid, LPVOID* output) noexcept;
}
