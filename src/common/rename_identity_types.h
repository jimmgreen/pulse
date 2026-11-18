#pragma once
#include <windows.h>
namespace pulse::ops {
struct RenameFileIdentity {
    DWORD volume = 0, high = 0, low = 0;
    bool operator==(const RenameFileIdentity&) const = default;
};
struct RenameIdentity { RenameFileIdentity source, parent; };
}
