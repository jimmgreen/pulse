#include "index_paths.h"
#include "index_config.h"
#include <atomic>
#include <mutex>
#include <shlobj.h>
#include <windows.h>

namespace pulse::index {

namespace {
std::atomic<bool> g_machine_scope{false};
std::mutex g_directory_mutex;
std::wstring g_active_directory;
}

void SetMachineIndexScope(bool machine_scope) {
    g_machine_scope.store(machine_scope);
}

bool MachineIndexScope() {
    return g_machine_scope.load();
}

void SetActiveIndexDirectory(const std::wstring& directory) {
    std::lock_guard<std::mutex> lock(g_directory_mutex);
    g_active_directory = directory;
}

std::wstring DataDir() {
    {
        std::lock_guard<std::mutex> lock(g_directory_mutex);
        if (!g_active_directory.empty()) return g_active_directory;
    }
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
