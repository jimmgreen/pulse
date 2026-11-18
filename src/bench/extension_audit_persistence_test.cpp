#include "../app/extensions/audit_persistence.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace pulse::extensions;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
bool Wait(AuditPersistence& audit, uint64_t count) {
    const auto end = GetTickCount64() + 15000;
    while (GetTickCount64() < end) {
        const auto state = audit.Status();
        if (state.persisted >= count) return true;
        if (state.failures) return false;
        Sleep(5);
    }
    return false;
}
void Put(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes;
}
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool Junction(const std::filesystem::path& link, const std::filesystem::path& target, DWORD& error) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) { error = GetLastError(); return false; }
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        error = GetLastError(); RemoveDirectoryW(link.c_str()); return false;
    }
    const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    const size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    struct MountPoint {
        DWORD tag;
        WORD size, reserved;
        WORD substitute_offset, substitute_size, print_offset, print_size;
    };
    if (sizeof(MountPoint) + path_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        error = ERROR_BUFFER_OVERFLOW; CloseHandle(directory); RemoveDirectoryW(link.c_str()); return false;
    }
    std::vector<unsigned char> bytes(sizeof(MountPoint) + path_bytes, 0);
    auto* point = reinterpret_cast<MountPoint*>(bytes.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->size = static_cast<WORD>(8 + path_bytes);
    point->substitute_size = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    point->print_size = static_cast<WORD>(print.size() * sizeof(wchar_t));
    auto* names = reinterpret_cast<wchar_t*>(bytes.data() + sizeof(MountPoint));
    std::copy(substitute.begin(), substitute.end(), names);
    std::copy(print.begin(), print.end(), names + substitute.size() + 1);
    DWORD returned = 0;
    const bool made = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, bytes.data(),
                                     static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE;
    error = made ? ERROR_SUCCESS : GetLastError();
    CloseHandle(directory);
    if (!made) RemoveDirectoryW(link.c_str());
    return made;
}
}
int wmain() {
    SetPersistentExtensionAuditStartupFailure(true);
    Check(PersistentExtensionAuditStatus().failures == 1 &&
          PersistentExtensionAuditStatus().last_error == "data_root_unavailable", "missing startup root is host-observable");
    SetPersistentExtensionAuditStartupFailure(false);
    Check(PersistentExtensionAuditStatus().last_error == "worker_start_failed", "worker startup failure is host-observable");
    StopPersistentExtensionAudit();
    wchar_t temporary[32768]{};
    if (!GetTempPathW(32768, temporary)) return 2;
    const auto root = std::filesystem::path(temporary) /
        (L"PulseAuditTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    const auto log = root / L"history";
    {
        AuditPersistence audit((root / L"native-fileops").wstring());
        Check(audit.Enqueue("native.plugin", "files.deleteCurrentSelection", "accepted") &&
              audit.Enqueue("native.plugin", "files.transfer", "submitted") &&
              audit.Enqueue("native.plugin", "files.rename.commit", "submitted"),
              "native destructive requests retain plugin identity in audit");
        Check(!audit.Enqueue("native.plugin", "context.get", "ok") &&
              !audit.Enqueue("native.plugin", "settings.get", "ok") &&
              !audit.Enqueue("native.plugin", "package.trust", "revoked"),
              "non-file-operation calls are excluded from audit");
        audit.Stop();
        const auto records = ReadPersistentAudit((root / L"native-fileops").wstring());
        Check(records.get_bool("ok") && records.find("records")->size() == 3 &&
              pulse::json::Serialize(records).find("native.plugin") != std::string::npos,
              "native destructive audit events persist with plugin id");
    }
    {
        AuditPersistence audit((root / L"denials").wstring());
        for (const auto* outcome : {"permission_denied", "request_budget", "limit_exceeded", "version_exhausted"})
            Check(audit.Enqueue("sample.audit", "files.transfer", outcome), "file operation rejection outcome remains auditable");
        audit.Stop();
        Check(audit.Status().persisted == 4 && audit.Status().dropped == 0,
              "resource denial and budget errors persist without arbitrary payload allowance");
    }
    {
        AuditPersistence audit(log.wstring());
        Check(audit.Enqueue("sample.audit", "files.transfer", "ok"), "admission accepts only minimal known event");
        Check(Wait(audit, 1), "worker persists without caller file I/O");
        Check(!audit.Enqueue("sample.audit", "C:/secret/password", "ok"), "path rejected as operation");
        Check(!audit.Enqueue("sample.audit", "files.transfer", "secret-token"), "payload rejected as outcome");
        Check(!audit.Enqueue("C:/private", "files.transfer", "ok"), "private path rejected as package id");
        audit.Stop();
        const auto status = audit.Status();
        Check(status.persisted == 1 && status.dropped == 3 && status.queued == 0, "stop drains and accounts rejected events");
        Check(!audit.Enqueue("sample.audit", "files.transfer", "ok"), "closed queue rejects new events");
    }
    const auto first = ReadPersistentAudit(log.wstring());
    Check(first.get_bool("ok") && !first.get_bool("grantsAuthority") && first.find("records")->size() == 1,
          "host local read returns bounded records without grants");
    const auto serialized = pulse::json::Serialize(first);
    Check(serialized.find("C:/") == std::string::npos && serialized.find("secret-token") == std::string::npos &&
          first.find("records")->at(0).size() == 5, "persistent records contain only five approved fields");
    Check(first.find("lastWriterStatus")->get_string("dropped") == "3", "final writer summary preserves admission loss counts");
    const auto session = first.find("records")->at(0).get_string("sessionId");
    auto legacy = Bytes(log / L"audit-0.jsonl");
    const auto operation_at = legacy.find("files.transfer");
    Check(operation_at != std::string::npos, "locate saved file operation in isolated audit fixture");
    if (operation_at != std::string::npos) {
        legacy.replace(operation_at, std::string("files.transfer").size(), "settings.set");
        Put(log / L"audit-0.jsonl", legacy);
    }
    Check(ReadPersistentAudit(log.wstring()).get_bool("ok"), "pre-upgrade non-file audit records remain readable");
    {
        AuditPersistence audit(log.wstring());
        Check(audit.Enqueue("sample.audit", "files.transfer", "dispatched") && Wait(audit, 1), "later host session appends");
        audit.Stop();
    }
    const auto second = ReadPersistentAudit(log.wstring());
    Check(second.get_bool("ok") && second.find("records")->size() == 2 &&
          second.find("records")->at(1).get_string("sessionId") != session, "new session receives independent random id");
    const auto exported = root / L"export.json";
    Check(ExportPersistentAudit(log.wstring(), exported.wstring()).get_bool("ok"), "explicit export writes new external file");
    const auto before = Bytes(exported);
    Check(!ExportPersistentAudit(log.wstring(), exported.wstring()).get_bool("ok") && Bytes(exported) == before,
          "export cannot overwrite existing destination");
    Check(!ExportPersistentAudit(log.wstring(), (log / L"export.json").wstring()).get_bool("ok"),
          "export cannot contaminate source retention directory");
    wchar_t short_path[32768]{};
    const DWORD short_length = GetShortPathNameW(log.c_str(), short_path, 32768);
    Check(short_length && short_length < 32768 &&
          ExportPersistentAudit(log.wstring(), (std::filesystem::path(short_path) / L"alias-export.json").wstring()).get_string("error") ==
              "destination_inside_audit_root", "canonical directory identity rejects available short-path alias export");
    if (short_length && std::wstring(short_path) == log.wstring())
        std::printf("[INFO] filesystem supplied no distinct 8.3 alias; canonical path boundary exercised\n");
    {
        HANDLE lock = CreateFileW((log / L"audit.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED position{};
        const bool held = lock != INVALID_HANDLE_VALUE && LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position);
        Check(held, "establish competing file lock");
        if (held) {
            AuditPersistence audit(log.wstring());
            audit.Enqueue("sample.audit", "files.transfer", "dispatched");
            audit.Stop();
            Check(audit.Status().last_error == "store_busy" && audit.Status().persisted == 0 && audit.Status().dropped == 1,
                  "lock conflict is nonblocking and observable on shutdown");
            Check(ReadPersistentAudit(log.wstring()).get_string("error") == "store_busy", "local reader reports busy rather than empty history");
            UnlockFileEx(lock, 0, 1, 0, &position);
        }
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    }
    {
        AuditPersistence audit(log.wstring());
        uint64_t accepted = 0;
        for (size_t batch = 0; batch < 24; ++batch) {
            for (size_t i = 0; i < 128; ++i)
                if (audit.Enqueue("sample.audit", "files.transfer", "pendingConfirmation")) ++accepted;
            if (!Wait(audit, accepted)) {
                const auto waiting = audit.Status();
                std::printf("[DIAG] rotation_wait batch=%zu target=%llu persisted=%llu failures=%llu queued=%zu last_error=%s\n",
                            batch, static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(waiting.persisted),
                            static_cast<unsigned long long>(waiting.failures), waiting.queued, waiting.last_error.c_str());
                break;
            }
        }
        audit.Stop();
        const auto state = audit.Status();
        std::printf("[DIAG] rotation admitted=%llu accepted=%llu persisted=%llu dropped=%llu failures=%llu queued=%zu last_error=%s\n",
                    static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(state.accepted),
                    static_cast<unsigned long long>(state.persisted), static_cast<unsigned long long>(state.dropped),
                    static_cast<unsigned long long>(state.failures), state.queued, state.last_error.c_str());
        Check(state.persisted == accepted && state.failures == 0 && accepted == 24 * 128, "rotation preserves admitted batches");
        uintmax_t total = 0;
        bool bounded = true;
        for (const auto& file : std::filesystem::directory_iterator(log)) {
            total += file.file_size();
            if (file.path().extension() == L".jsonl") bounded &= file.file_size() <= kAuditSegmentBytes;
        }
        const auto history = ReadPersistentAudit(log.wstring());
        Check(bounded && total <= kAuditMaximumDiskBytes && history.get_bool("ok") &&
              history.find("records")->size() < accepted, "four segment rotation enforces total retention budget");
    }
    const auto corrupt = root / L"corrupt";
    std::filesystem::create_directory(corrupt);
    Put(corrupt / L"audit-0.jsonl", "{\"payload\":\"private-path\"}\n");
    {
        AuditPersistence audit(corrupt.wstring());
        audit.Enqueue("sample.audit", "files.transfer", "ok"); audit.Stop();
        Check(audit.Status().last_error == "corrupt_data" && audit.Status().persisted == 0, "corrupt segment fails closed without overwrite");
    }
    Check(!ReadPersistentAudit(corrupt.wstring()).get_bool("ok") &&
          pulse::json::Serialize(ReadPersistentAudit(corrupt.wstring())).find("private-path") == std::string::npos,
          "corrupt payload is not echoed into export");
    Put(corrupt / L"audit-0.jsonl", std::string(kAuditSegmentBytes + 1, 'x'));
    Check(ReadPersistentAudit(corrupt.wstring()).get_string("error") == "disk_budget_exceeded", "oversized segment rejected before parse");
    std::filesystem::remove(corrupt / L"audit-0.jsonl");
    Put(corrupt / L"audit.pending", "interrupted");
    Check(ReadPersistentAudit(corrupt.wstring()).get_string("error") == "interrupted_commit", "crash leftover is explicit diagnostic not replayed");
    std::filesystem::remove(corrupt / L"audit.pending");
    Put(corrupt / L"audit-status.json", "{\"accepted\":\"0\",\"persisted\":\"0\",\"dropped\":\"0\",\"failures\":\"0\",\"lastError\":\"secret-token\"}");
    const auto bad_status = ReadPersistentAudit(corrupt.wstring());
    Check(bad_status.get_string("error") == "corrupt_status" &&
          pulse::json::Serialize(bad_status).find("secret-token") == std::string::npos,
          "corrupt status cannot export arbitrary secret fields");
    std::filesystem::remove(corrupt / L"audit-status.json");
    Put(corrupt / L"unrecognized.txt", "preserve");
    Check(ReadPersistentAudit(corrupt.wstring()).get_string("error") == "unexpected_or_unsafe_entry" &&
          Bytes(corrupt / L"unrecognized.txt") == "preserve", "unknown directory content fails closed without deletion");
    std::filesystem::remove(corrupt / L"unrecognized.txt");
    const auto linked = corrupt / L"audit-0.jsonl";
    const auto sentinel = root / L"sentinel.txt";
    Put(sentinel, "unchanged");
    const bool hardlink = CreateHardLinkW(linked.c_str(), sentinel.c_str(), nullptr) != FALSE;
    Check(hardlink, "create real hardlink boundary fixture");
    if (hardlink) {
        Check(ReadPersistentAudit(corrupt.wstring()).get_string("error") == "unsafe_path" && Bytes(sentinel) == "unchanged",
              "hardlinked segment rejected without touching external object");
        std::filesystem::remove(linked);
    }
    const auto junction = root / L"linked-directory";
    const bool symlink = CreateSymbolicLinkW(junction.c_str(), log.c_str(),
                                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    const DWORD symlink_error = symlink ? ERROR_SUCCESS : GetLastError();
    DWORD junction_error = ERROR_SUCCESS;
    const bool mounted = !symlink && Junction(junction, log, junction_error);
    if (symlink || mounted) {
        std::printf("[INFO] directory reparse fixture=%s; symlink_error=%lu\n",
                    symlink ? "symbolic-link" : "junction", symlink_error);
        Check(ReadPersistentAudit(junction.wstring()).get_string("error") == "unsafe_path", "real directory reparse rejected");
        Check(RemoveDirectoryW(junction.c_str()) != FALSE, "remove reparse fixture without traversing target");
    } else {
        std::printf("[UNVERIFIED] directory reparse fixture unavailable symlink_error=%lu junction_error=%lu; not counted as passed\n",
                    symlink_error, junction_error);
        ++failures;
    }
    // A freshly-created exact temporary root, no user data; never traverse a remaining reparse point.
    if (!std::filesystem::exists(junction)) std::filesystem::remove_all(root);
    std::printf("Audit persistence: failures=%d\n", failures);
    return failures ? 1 : 0;
}
