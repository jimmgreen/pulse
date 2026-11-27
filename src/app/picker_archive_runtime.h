#pragma once
#include "extensions/runtime.h"
#include "../common/picker_places_snapshot.h"

namespace pulse::app {
// UI-thread only: the startup catalog is immutable; this does no filesystem or plugin work.
inline std::wstring PickerArchiveWorker() {
    for (size_t i = 0; i < extensions::PackageCount(); ++i) {
        const auto* manifest = extensions::PackageManifest(i);
        if (!manifest || manifest->id != "pulse.7zip" || !extensions::PackageEnabled(i)) continue;
        const auto native = extensions::PackageNativePath(i);
        const auto slash = native.find_last_of(L"\\/");
        if (slash == std::wstring::npos) continue;
        auto worker = native.substr(0, slash + 1) + L"pulse_7zip_worker.exe";
        if (picker_places::detail::DiskPath(worker)) return worker;
    }
    return {};
}
}
