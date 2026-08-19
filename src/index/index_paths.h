// index_paths.h — Shared data directory for the index file (UI + Pulse.Index).
#pragma once
#include <string>

namespace pulse::index {

void SetMachineIndexScope(bool machine_scope);
bool MachineIndexScope();
std::wstring DataDir();
std::wstring CacheFilePath();

} // namespace pulse::index
