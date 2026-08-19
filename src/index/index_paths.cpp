#include "index_paths.h"
#include "index_config.h"
#include <atomic>
#include <shlobj.h>
#include <windows.h>

namespace pulse::index {

namespace {
std::atomic<bool> g_machine_scope{false};
}

void SetMachineIndexScope(bool machine_scope) {
    g_machine_scope.store(machine_scope);
}

bool MachineIndexScope() {
    return g_machine_scope.load();
}

std::wstring DataDir() {
    if (MachineIndexScope()) {
        IndexConfig config;
        if (LoadMachineConfig(config, nullptr) && !config.index_path.empty()) {
            CreateDirectoryW(config.index_path.c_str(), nullptr);
            return config.index_path;
        }
        return MachineIndexRoot();
    }
    return UserIndexRoot();
}

std::wstring CacheFilePath() {
    std::wstring dir = DataDir();
    if (dir.empty()) return {};
    return dir + L"\\pulse-index.bin";
}

} // namespace pulse::index
