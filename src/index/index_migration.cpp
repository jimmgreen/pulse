#include "index_migration.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <stdexcept>

namespace pulse::index {
namespace {
namespace fs = std::filesystem;
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
bool SafePath(const fs::path& path) {
    for (auto part = path; !part.empty(); part = part.parent_path()) {
        const DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (part == part.parent_path()) break;
    }
    return true;
}
bool HashFile(const fs::path& path, uint64_t& size, uint64_t& hash) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    hash = 14695981039346656037ull;
    size = 0;
    char buffer[65536];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        const auto count = stream.gcount();
        size += static_cast<uint64_t>(count);
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return stream.eof() && !stream.bad();
}
bool HexDirectory(const std::wstring& name) {
    return name.size() == 16 && name.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
}
bool ArtifactDirectory(const fs::path& relative) {
    std::vector<std::wstring> parts;
    for (const auto& part : relative) parts.push_back(Lower(part.wstring()));
    return (parts.size() == 1 && (parts[0] == L"v9" || parts[0] == L"volumes")) ||
        (parts.size() == 2 && ((parts[0] == L"v9" && parts[1] == L"volumes") ||
            ((parts[0] == L"v9" || parts[0] == L"volumes") && HexDirectory(parts[1])))) ||
        (parts.size() == 3 && parts[0] == L"v9" && parts[1] == L"volumes" && HexDirectory(parts[2]));
}
bool Artifact(const fs::path& relative) {
    std::vector<std::wstring> parts;
    for (const auto& part : relative) parts.push_back(Lower(part.wstring()));
    if (parts.size() == 1) {
        const auto& n = parts[0];
        return n == L"pulse-index.bin" || n == L"pulse-index.bin.tmp" || n == L"pulse-index.dlt" ||
            (n.size() == 17 && n.starts_with(L"pulse-index-") && n.ends_with(L".dlt") &&
             n[12] >= L'a' && n[12] <= L'z');
    }
    const bool shard = (parts.size() == 3 && (parts[0] == L"v9" || parts[0] == L"volumes") && HexDirectory(parts[1])) ||
        (parts.size() == 4 && parts[0] == L"v9" && parts[1] == L"volumes" && HexDirectory(parts[2]));
    if (!shard) return false;
    auto name = parts.back();
    if (name.ends_with(L".tmp")) name.resize(name.size() - 4);
    return name == L"base-a.bin" || name == L"base-b.bin" || name == L"manifest.json" ||
        name == L"wal-a.log" || name == L"wal-b.log";
}
std::vector<MigratedIndexFile> Inventory(const fs::path& root) {
    std::vector<MigratedIndexFile> result;
    if (!fs::exists(root)) return result;
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        const auto relative = it->path().lexically_relative(root);
        if (!SafePath(it->path())) throw std::runtime_error("reparse point");
        if (it->is_directory()) {
            const auto first = Lower(relative.begin()->wstring());
            if ((first != L"v9" && first != L"volumes") || it.depth() >= 3) it.disable_recursion_pending();
            continue;
        }
        if (!Artifact(relative)) continue;
        MigratedIndexFile file{relative.wstring()};
        if (!HashFile(it->path(), file.size, file.hash)) throw std::runtime_error("read index");
        result.push_back(std::move(file));
    }
    return result;
}
bool Matches(const fs::path& path, const MigratedIndexFile& file) {
    uint64_t size = 0, hash = 0;
    return SafePath(path) && HashFile(path, size, hash) && size == file.size && hash == file.hash;
}

bool ValidPlan(const IndexMigration& migration) {
    if (migration.files.empty()) return true;
    const fs::path source(migration.source), target(migration.target);
    if (!source.is_absolute() || !target.is_absolute() || source == source.root_path() ||
        target == target.root_path() || !SafePath(source) || !SafePath(target)) return false;
    for (const auto& file : migration.files) {
        const fs::path relative(file.relative);
        if (relative.is_absolute() || relative.lexically_normal() != relative || !Artifact(relative)) return false;
    }
    return true;
}

DWORD CopyVerified(const fs::path& source, const fs::path& target, const MigratedIndexFile& expected) {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) return GetLastError();
    const fs::path temporary = target.wstring() + L".pulse-copy-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        CloseHandle(input);
        return error;
    }
    DWORD error = 0;
    char buffer[65536];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr)) { error = GetLastError(); break; }
        if (!read) break;
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(output, buffer + offset, read - offset, &written, nullptr) || !written) {
                error = GetLastError();
                if (!error) error = ERROR_WRITE_FAULT;
                break;
            }
            offset += written;
        }
        if (error) break;
    }
    if (!error && !FlushFileBuffers(output)) error = GetLastError();
    CloseHandle(output);
    if (!error && !Matches(temporary, expected)) error = ERROR_CRC;
    if (!error && !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    CloseHandle(input);
    if (error && SafePath(temporary)) DeleteFileW(temporary.c_str());
    return error;
}
}

std::wstring ResolveIndexMigrationTarget(const std::wstring& target) {
    auto path = fs::path(target).lexically_normal().make_preferred();
    if (path.is_absolute() && path == path.root_path()) path /= L"Index";
    return path.wstring();
}

bool SameIndexLocation(const std::wstring& source, const std::wstring& target) {
    if (source.empty() || target.empty()) return false;
    try {
        auto normalize = [](const std::wstring& value) {
            auto path = fs::path(ResolveIndexMigrationTarget(value));
            if (!path.is_absolute()) return std::wstring{};
            while (path != path.root_path() && path.filename().empty()) path = path.parent_path();
            return Lower(path.wstring());
        };
        const auto from = normalize(source), to = normalize(target);
        return !from.empty() && from == to;
    } catch (...) { return false; }
}

bool CopyIndexForMigration(const std::wstring& source, const std::wstring& target,
                           IndexMigration& migration, std::wstring& error) {
    migration = {};
    try {
        fs::path from = fs::path(source).lexically_normal().make_preferred();
        fs::path to = ResolveIndexMigrationTarget(target);
        while (from != from.root_path() && from.filename().empty()) from = from.parent_path();
        while (to != to.root_path() && to.filename().empty()) to = to.parent_path();
        migration.failure = ERROR_INVALID_PARAMETER;
        if (!from.is_absolute() || !to.is_absolute() || from == from.root_path() || to == to.root_path() ||
            !SafePath(from) || !SafePath(to)) throw std::runtime_error("unsafe directory");
        auto a = Lower(from.wstring()), b = Lower(to.wstring());
        while (a.size() > 3 && a.back() == L'\\') a.pop_back();
        while (b.size() > 3 && b.back() == L'\\') b.pop_back();
        migration.source = from.wstring();
        migration.target = to.wstring();
        if (a == b) { migration.failure = 0; return true; }
        if ((a + L"\\").starts_with(b + L"\\") || (b + L"\\").starts_with(a + L"\\"))
            throw std::runtime_error("overlapping directories");
        const auto files = Inventory(from);
        for (const auto& existing : Inventory(to)) {
            const auto match = std::find_if(files.begin(), files.end(), [&](const auto& file) {
                return Lower(file.relative) == Lower(existing.relative) && file.size == existing.size && file.hash == existing.hash;
            });
            if (match == files.end()) {
                migration.failure = ERROR_ALREADY_EXISTS;
                throw std::runtime_error("target has a different index");
            }
        }
        fs::create_directories(to);
        uint64_t required = 0;
        for (const auto& file : files)
            if (!fs::exists(to / file.relative)) required += file.size;
        if (fs::space(to).available < required) {
            migration.failure = ERROR_DISK_FULL;
            throw std::runtime_error("insufficient space");
        }
        migration.failure = ERROR_WRITE_FAULT;
        for (auto file : files) {
            const auto old_file = from / file.relative, new_file = to / file.relative;
            fs::create_directories(new_file.parent_path());
            if (!SafePath(new_file)) throw std::runtime_error("unsafe copy path");
            if (fs::exists(new_file) && Matches(new_file, file)) {
                migration.files.push_back(file);
                continue;
            }
            DWORD copy_error = ERROR_WRITE_FAULT;
            for (int attempt = 0; attempt < 3; ++attempt) {
                copy_error = CopyVerified(old_file, new_file, file);
                if (!copy_error) break;
                migration.failure = copy_error;
                if (copy_error != ERROR_SHARING_VIOLATION && copy_error != ERROR_LOCK_VIOLATION) break;
                Sleep(200);
            }
            if (copy_error) throw std::runtime_error("copy failed");
            file.copied = true;
            migration.files.push_back(file);
            if (!Matches(new_file, file) || !Matches(old_file, file)) throw std::runtime_error("verification failed");
        }
        migration.failure = 0;
        return true;
    } catch (...) {
        // Keep originals; only remove copies whose contents still match this attempt.
        DiscardIndexMigrationCopies(migration);
        migration.files.clear();
        error = L"索引迁移失败：请使用不含旧索引的独立目录，检查路径、权限及剩余空间。原索引已保留。";
        return false;
    }
}

void DiscardIndexMigrationCopies(const IndexMigration& migration) {
    if (!ValidPlan(migration)) return;
    for (const auto& file : migration.files) {
        const auto copy = fs::path(migration.target) / file.relative;
        if (file.copied && Matches(copy, file)) DeleteFileW(copy.c_str());
    }
}

bool RemoveMigratedIndexSource(const IndexMigration& migration, std::wstring& error) {
    if (migration.source == migration.target) return true;
    if (!ValidPlan(migration)) {
        error = L"旧索引清理记录无效，文件已保留。";
        return false;
    }
    bool ok = true;
    for (const auto& file : migration.files) {
        const auto old_file = fs::path(migration.source) / file.relative;
        if (GetFileAttributesW(old_file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) continue;
        }
        bool removed = false;
        for (int attempt = 0; attempt < 3 && !removed; ++attempt) {
            if (!Matches(old_file, file)) break;
            removed = DeleteFileW(old_file.c_str()) != FALSE;
            if (!removed) Sleep(200);
        }
        if (!removed) {
            error = L"新索引已迁移，但部分旧索引文件无法清理。";
            ok = false;
        }
    }
    // Include empty shards that had no files in the copy inventory.
    try {
        const fs::path source(migration.source);
        if (!source.is_absolute() || source == source.root_path() || !SafePath(source)) return false;
        std::vector<fs::path> directories;
        if (fs::exists(source)) {
            for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
                if (!SafePath(it->path()) || !ArtifactDirectory(it->path().lexically_relative(source))) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_directory()) directories.push_back(it->path());
            }
        }
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            if (SafePath(*it) && !RemoveDirectoryW(it->c_str())) {
                const DWORD code = GetLastError();
                if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                    ok = false;
            }
        }
        // Non-recursive removal preserves configuration and any unrelated files.
        if (SafePath(source) && !RemoveDirectoryW(source.c_str())) {
            const DWORD code = GetLastError();
            if (code != ERROR_DIR_NOT_EMPTY && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                ok = false;
        }
    } catch (...) {
        ok = false;
    }
    if (!ok) error = L"新索引已迁移，但部分旧索引文件或空目录无法清理。";
    return ok;
}
}
