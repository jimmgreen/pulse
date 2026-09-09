// index_paths.h — Shared data directory for the index file (UI + Pulse.Index).
#pragma once
#include <string>

namespace pulse::index {

void SetMachineIndexScope(bool machine_scope);
bool MachineIndexScope();
// A running host keeps one directory until all of its index handles are closed.
void SetActiveIndexDirectory(const std::wstring& directory);
std::wstring DataDir();
std::wstring CacheFilePath();

} // namespace pulse::index
