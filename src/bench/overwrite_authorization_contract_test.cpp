// Documents the Win32 contract the coordinator must not mistake for an atomic
// expected-identity replacement. Only newly created GUID fixtures are mutated.
#include "../common/rename_identity.h"
#include "../ops/undo_file_version.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
namespace {
int failed = 0;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failed;
}
bool Write(const fs::path& path, const char* content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    file.close();
    return static_cast<bool>(file);
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
int wmain() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return 2;
    wchar_t guid[40]{};
    StringFromGUID2(id, guid, 40);
    const auto root = fs::absolute(fs::path(L"bench_data") / (std::wstring(L"overwrite_contract_") + guid));
    if (!fs::create_directories(root)) return 2;
    // Observational matrix: metadata-only opens may not participate in the
    // same sharing checks as data/delete opens. Never infer a strong lease
    // from the requested share mask alone.
    struct Access { const char* name; DWORD rights; };
    const Access access[] = {{"attributes",FILE_READ_ATTRIBUTES},{"generic_read",GENERIC_READ},
        {"delete_attributes",DELETE|FILE_READ_ATTRIBUTES},{"read_data_delete",FILE_READ_DATA|DELETE}};
    unsigned candidates = 0;
    for (size_t i=0;i<4;++i) {
        for (size_t j=0;j<2;++j) {
            const DWORD sharing = FILE_SHARE_READ | (j ? FILE_SHARE_WRITE : 0);
            bool moved = false, replaced = false, write_opened = false, opened_all = true;
            DWORD move_error = ERROR_SUCCESS, replace_error = ERROR_SUCCESS, write_error = ERROR_SUCCESS;
            for (int operation=0;operation<3;++operation) {
                const auto prefix = std::to_wstring(i)+L"-"+std::to_wstring(j)+L"-"+std::to_wstring(operation);
                const auto original = root/(prefix+L"-target.txt");
                const auto staging = root/(prefix+L"-stage.txt");
                const auto renamed = root/(prefix+L"-renamed.txt");
                if (!Write(original,"approved") || !Write(staging,"replacement")) return 2;
                HANDLE pinned = CreateFileW(original.c_str(),access[i].rights,sharing,nullptr,OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if (pinned==INVALID_HANDLE_VALUE) {
                    printf("[INFO] lease_open access=%s share=0x%lx error=%lu\n",access[i].name,sharing,GetLastError());
                    Check(false,"matrix fixture lease opens"); opened_all=false; continue;
                }
                if (operation==0) {
                    moved=MoveFileW(original.c_str(),renamed.c_str())!=FALSE;
                    move_error=moved?ERROR_SUCCESS:GetLastError();
                } else if (operation==1) {
                    replaced=ReplaceFileW(original.c_str(),staging.c_str(),nullptr,0,nullptr,nullptr)!=FALSE;
                    replace_error=replaced?ERROR_SUCCESS:GetLastError();
                } else {
                    HANDLE writer=CreateFileW(original.c_str(),GENERIC_WRITE,
                        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
                    write_opened=writer!=INVALID_HANDLE_VALUE;
                    write_error=write_opened?ERROR_SUCCESS:GetLastError();
                    if (write_opened) CloseHandle(writer);
                }
                CloseHandle(pinned);
                if (operation==0)
                    Check(Read(moved?renamed:original)=="approved","observed MoveFile result preserves fixture contents");
                else if (operation==1)
                    Check(Read(original)==(replaced?"replacement":"approved"),"observed ReplaceFile result matches final fixture contents");
                else Check(Read(original)=="approved","writer-open probe does not modify fixture content");
            }
            const bool candidate=opened_all && !moved && !write_opened && replaced;
            if (candidate) ++candidates;
            printf("[INFO] lease_matrix access=%s rights=0x%lx share=0x%lx move=%d move_error=%lu replace=%d replace_error=%lu write_open=%d write_error=%lu candidate=%d\n",
                access[i].name,access[i].rights,sharing,moved?1:0,move_error,replaced?1:0,replace_error,
                write_opened?1:0,write_error,candidate?1:0);
        }
    }
    printf("[INFO] observed_replace_lease_candidates=%u; observation alone does not authorize path-based mutation\n",candidates);
    // Independent fixtures ensure no matrix outcome suppresses the version tests.
    const auto target = root / L"target.txt", stage = root / L"stage.txt", saved = root / L"approved.txt";
    if (!Write(target, "approved") || !Write(stage, "replacement")) return 2;

    pulse::ops::RenameIdentity authorized{};
    if (pulse::ops::CaptureRenameIdentity(target.wstring(), authorized) != ERROR_SUCCESS ||
        !MoveFileW(target.c_str(), saved.c_str()) || !Write(target, "unrelated")) return 2;
    const DWORD stale = pulse::ops::RenameIdentified(target.wstring(), L"should-not-move.txt", authorized);
    Check(stale == ERROR_FILE_INVALID && Read(target) == "unrelated" && !fs::exists(root / L"should-not-move.txt"),
          "existing handle-bound rename rejects a substituted file identity");
    Check(ReplaceFileW(target.c_str(), stage.c_str(), nullptr, 0, nullptr, nullptr) &&
          Read(target) == "replacement" && Read(saved) == "approved",
          "path-based ReplaceFileW replaces the current occupant, not the earlier approved object");

    pulse::ops::RenameIdentity before_edit{};
    if (pulse::ops::CaptureRenameIdentity(target.wstring(), before_edit) != ERROR_SUCCESS ||
        !Write(target, "user edited")) return 2;
    const DWORD renamed_edit = pulse::ops::RenameIdentified(target.wstring(), L"edited.txt", before_edit);
    Check(renamed_edit == ERROR_SUCCESS && Read(root / L"edited.txt") == "user edited",
          "identity-only authorization does not detect in-place content modification");
    const auto versioned = root / L"versioned.txt";
    if (!Write(versioned, "12345678")) return 2;
    pulse::ops::UndoFileVersion metadata_only, too_large, hashed;
    DWORD error = ERROR_SUCCESS;
    Check(pulse::ops::CaptureUndoFileVersion(versioned.wstring(), metadata_only, error) && !metadata_only.has_sha256,
          "default receipt avoids full-file hashing");
    Check(pulse::ops::CaptureUndoFileVersion(versioned.wstring(), too_large, error, 4) && !too_large.has_sha256,
          "hash budget skips a larger file without reading its content");
    Check(pulse::ops::CaptureUndoFileVersion(versioned.wstring(), hashed, error, 8) && hashed.has_sha256,
          "explicit budget captures a SHA receipt for a bounded file");
    pulse::ops::UndoFileLease lease;
    const bool acquired = lease.Acquire(versioned.wstring(), hashed, error);
    Check(acquired, "unchanged file acquires the identity and version lease");
    const auto moved_during_lease = root / L"moved-while-leased.txt";
    const bool moved_during = acquired && MoveFileW(versioned.c_str(), moved_during_lease.c_str());
    Check(acquired && !moved_during, "held lease prevents another path rename");
    lease.Reset();
    if (moved_during && !MoveFileW(moved_during_lease.c_str(), versioned.c_str())) return 2;
    HANDLE writer = CreateFileW(versioned.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) return 2;
    Check(!lease.Acquire(versioned.wstring(), hashed, error) && error == ERROR_SHARING_VIOLATION,
          "an existing writer prevents acquiring a mutation lease");
    CloseHandle(writer);
    if (!Write(versioned, "abcdefgh")) return 2;
    HANDLE restore_time = CreateFileW(versioned.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (restore_time == INVALID_HANDLE_VALUE) return 2;
    const FILETIME original_time{hashed.modified.LowPart, static_cast<DWORD>(hashed.modified.HighPart)};
    const BOOL restored = SetFileTime(restore_time, nullptr, nullptr, &original_time);
    CloseHandle(restore_time);
    if (!restored) return 2;
    Check(!lease.Acquire(versioned.wstring(), hashed, error),
          "same-size in-place edit with restored write time invalidates the old receipt");
    pulse::ops::UndoFileVersion hash_only_check;
    if (!pulse::ops::CaptureUndoFileVersion(versioned.wstring(), hash_only_check, error)) return 2;
    hash_only_check.sha256 = hashed.sha256;
    hash_only_check.has_sha256 = true;
    Check(!lease.Acquire(versioned.wstring(), hash_only_check, error) && error == ERROR_FILE_INVALID,
          "SHA independently rejects changed bytes even when supplied metadata matches");
    pulse::ops::UndoFileVersion directory_receipt;
    Check(!pulse::ops::CaptureUndoFileVersion(root.wstring(), directory_receipt, error) && error == ERROR_NOT_SUPPORTED,
          "directory identity cannot authorize recursive deletion of new children");
    wprintf(L"[INFO] retained fixture: %ls\n", root.c_str());
    return failed ? 1 : 0;
}
