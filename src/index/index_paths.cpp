#include "index_paths.h"
#include <shlobj.h>
#include <windows.h>

namespace pulse::index {

std::wstring DataDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        std::wstring dir = std::wstring(path) + L"\\Pulse";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L"";
}

std::wstring CacheFilePath() {
    std::wstring dir = DataDir();
    if (dir.empty()) return {};
    return dir + L"\\pulse-index.bin";
}

} // namespace pulse::index
