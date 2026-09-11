#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::index {
struct MigratedIndexFile {
    std::wstring relative;
    uint64_t size = 0;
    uint64_t hash = 0;
    bool copied = false;
};
struct IndexMigration {
    std::wstring source;
    std::wstring target;
    std::vector<MigratedIndexFile> files;
    uint32_t failure = 0;
};
// Only recognized index artifacts are copied/deleted; config and user files stay put.
std::wstring ResolveIndexMigrationTarget(const std::wstring& target);
bool SameIndexLocation(const std::wstring& source, const std::wstring& target);
bool CopyIndexForMigration(const std::wstring& source, const std::wstring& target,
                           IndexMigration& migration, std::wstring& error);
bool RemoveMigratedIndexSource(const IndexMigration& migration, std::wstring& error);
void DiscardIndexMigrationCopies(const IndexMigration& migration);
}
