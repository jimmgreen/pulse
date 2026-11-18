#include "../ops/transfer_enumeration_cursor.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <cstdint>
#include <utility>
namespace fs = std::filesystem;
using pulse::ops::TransferEnumerationCursor;
static void Check(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
    printf("[PASS] %s\n", text);
}
int wmain() {
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"enumeration_cursor_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    try {
        Check(fs::create_directories(root), "unique fixture");
        const auto source = root / L"source", target = root / L"target", remapped = root / L"remapped";
        fs::create_directory(source);
        fs::create_directory(source / L"empty");
        fs::create_directory(source / L"nested");
        for (int i = 0; i < 1200; ++i) {
            std::ofstream file(source / L"nested" / (std::to_wstring(i) + L".bin"), std::ios::binary);
            file << "abc"; file.close(); if (!file) throw std::runtime_error("write fixture");
        }
        TransferEnumerationCursor cursor;
        Check(cursor.Start(source.wstring(), target.wstring()), "start");
        TransferEnumerationCursor::Item item;
        size_t seen = 0, files = 0, empty = 0;
        bool saw_nested = false;
        while (seen < TransferEnumerationCursor::InitialBatchLimit && cursor.Next(item)) {
            ++seen;
            if (item.source == (source / L"nested").wstring()) saw_nested = true;
            if (item.source == (source / L"empty").wstring()) ++empty;
            if (!(item.metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!saw_nested) throw std::runtime_error("parent precedes file"); ++files;
            }
        }
        Check(seen == TransferEnumerationCursor::InitialBatchLimit && !cursor.Complete(), "64-entry initial discovery window precedes EOF");
        cursor.RemapDirectory((source / L"nested").wstring(), remapped.wstring());
        size_t refill_seen = 0;
        while (cursor.Next(item)) {
            if (++refill_seen == TransferEnumerationCursor::RefillBatchLimit)
                Check(!cursor.Complete(), "1024-entry refill boundary remains resumable");
            ++seen;
            if (item.source == (source / L"empty").wstring()) ++empty;
            if (!(item.metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                ++files;
                if (fs::path(item.destination).parent_path() != remapped) throw std::runtime_error("remapped destination");
                if (item.metadata.nFileSizeLow != 3) throw std::runtime_error("enumerated size");
            }
        }
        Check(cursor.Complete() && !cursor.Error() && seen == 1202 && files == 1200 && empty == 1, "complete including empty folder");
        TransferEnumerationCursor interrupted;
        Check(interrupted.Start(source.wstring(), target.wstring()) && interrupted.Next(item), "cancel fixture active");
        interrupted.Close();
        Check(interrupted.Complete() && !interrupted.Next(item), "cancel closes resumable state");
        TransferEnumerationCursor missing;
        Check(!missing.Start((source / L"absent" / L"child").wstring(), target.wstring()) && missing.Error() != 0, "source disappearance yields error");
        Check(cursor.PendingDirectories() == 0 && interrupted.PendingDirectories() == 0 && missing.PendingDirectories() == 0,
            "EOF cancellation and error release all directory mappings");
        cursor.RemapDirectory(source.wstring(), remapped.wstring());
        Check(cursor.PendingDirectories() == 0, "remapping completed directory does not resurrect mapping");
        TransferEnumerationCursor moved_items;
        const auto moved_root = root / L"moved-destinations";
        Check(moved_items.Start(source.wstring(), target.wstring()), "frame-cache fixture starts");
        moved_items.RemapDirectory(source.wstring(), moved_root.wstring());
        size_t moved_seen = 0, moved_files = 0;
        uint64_t moved_bytes = 0;
        bool checked_metadata = false;
        while (moved_items.Next(item)) {
            // The coordinator now transfers path allocations into its journal.
            // The cursor must retain independent pending/open directory paths.
            const auto item_source = std::move(item.source);
            const auto item_destination = std::move(item.destination);
            ++moved_seen;
            if (item.metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                const auto redirected = moved_root / (fs::path(item_source).filename().wstring() + L"-redirected");
                moved_items.RemapDirectory(item_source, redirected.wstring());
            } else {
                ++moved_files;
                moved_bytes += (static_cast<uint64_t>(item.metadata.nFileSizeHigh) << 32) | item.metadata.nFileSizeLow;
                if (fs::path(item_destination).parent_path() != moved_root / L"nested-redirected")
                    throw std::runtime_error("pending directory remap lost after moving item paths");
                if (!checked_metadata) {
                    WIN32_FILE_ATTRIBUTE_DATA actual{};
                    if (!GetFileAttributesExW(item_source.c_str(), GetFileExInfoStandard, &actual) ||
                        actual.dwFileAttributes != item.metadata.dwFileAttributes ||
                        CompareFileTime(&actual.ftCreationTime, &item.metadata.ftCreationTime) ||
                        CompareFileTime(&actual.ftLastAccessTime, &item.metadata.ftLastAccessTime) ||
                        CompareFileTime(&actual.ftLastWriteTime, &item.metadata.ftLastWriteTime))
                        throw std::runtime_error("streamed metadata differs from source");
                    checked_metadata = true;
                }
            }
        }
        Check(checked_metadata && moved_seen == 1202 && moved_files == 1200 && moved_bytes == 3600 &&
              moved_items.Complete() && !moved_items.Error() && moved_items.PendingDirectories() == 0,
              "moved item paths preserve pending remaps metadata and final counts");
        const auto broad = root / L"wide";
        fs::create_directory(broad);
        for (int branch = 0; branch < 32; ++branch) {
            auto path = broad / std::to_wstring(branch);
            fs::create_directory(path);
            for (int depth = 0; depth < 8; ++depth) { path /= L"d"; fs::create_directory(path); }
        }
        TransferEnumerationCursor bounded;
        Check(bounded.Start(broad.wstring(), target.wstring()), "wide/deep fixture starts");
        size_t enumerated_directories = 0, peak_mappings = 0;
        while (bounded.Next(item)) {
            ++enumerated_directories;
            if (bounded.PendingDirectories() > peak_mappings) peak_mappings = bounded.PendingDirectories();
            if (bounded.PendingDirectories() > 10) throw std::runtime_error("mapping retained completed branch");
            // A new direct child means the preceding branch has fully drained.
            if (fs::path(item.source).parent_path() == broad && bounded.PendingDirectories() != 2)
                throw std::runtime_error("wide branch mapping did not converge");
        }
        Check(enumerated_directories == 32 * 9 && peak_mappings == 10 && bounded.PendingDirectories() == 0,
            "mapping count follows active depth across wide batches");
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "[FAIL] %s fixture=%ls\n", error.what(), root.c_str());
        return 1;
    }
}
