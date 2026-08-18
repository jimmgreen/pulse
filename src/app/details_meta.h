// details_meta.h — Security/volume facts for the details panel's 安全/其他
// sections. FetchDetailsMeta blocks (security + volume APIs); callers must
// stay off the UI thread (see PrefetchDetailsMeta in app_main.cpp).
#pragma once
#include <string>

namespace pulse::app {

struct DetailsMeta {
    std::wstring owner;        // 所有者 ("DOMAIN\\user"); empty = failed
    std::wstring permissions;  // effective-rights summary for the current user
    std::wstring drive;        // 驱动器 "Data (D:)"
    std::wstring file_system;  // 文件系统 "NTFS"
    std::wstring free_space;   // 可用空间 "256 GB / 500 GB (51%)"
};

DetailsMeta FetchDetailsMeta(const std::wstring& path);

// Pure: access mask -> Chinese summary ("完全控制", "读取和执行、读取", ...).
std::wstring FormatAccessMask(unsigned int mask, bool is_dir);

} // namespace pulse::app
