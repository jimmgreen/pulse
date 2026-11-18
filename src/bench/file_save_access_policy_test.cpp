#include "../ui/file_save_access_policy.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace pulse::ui;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
void Evidence(const SaveAccessResult& result) {
    std::cout << "[FACT] state=" << static_cast<int>(result.state) << " stage=" << static_cast<int>(result.stage)
        << " error=" << result.error << " primary_stage=" << static_cast<int>(result.primary_stage) << " primary=" << result.primary_error << " cleanup=" << static_cast<int>(result.cleanup)
        << " cleanup_error=" << result.cleanup_error << " observed_identity=" << result.probe_identity.has_value() << '\n';
    for (const auto* identity : {&result.parent_identity, &result.probe_identity}) {
        if (*identity) {
            std::cout << "[IDENTITY] volume=" << (*identity)->volume << " created=" << (*identity)->creation_time << " file=";
            for (const auto byte : (*identity)->file) std::cout << std::hex << static_cast<unsigned int>(byte) << ':';
            std::cout << std::dec << '\n';
        }
    }
}
bool CreateOwned(const std::filesystem::path& path, const std::string& contents = "owned sentinel", DWORD* error = nullptr, bool* opened = nullptr) {
    if (error) *error = ERROR_SUCCESS;
    if (opened) *opened = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (error) *error = GetLastError(); return false; }
    if (opened) *opened = true;
    DWORD written = 0;
    const bool ok = WriteFile(h, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) && written == contents.size();
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(h) != FALSE;
    const DWORD close_error = closed ? ERROR_SUCCESS : GetLastError();
    if (error) *error = write_error ? write_error : close_error;
    return ok && closed;
}
std::string Contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool Absent(const std::filesystem::path& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
std::optional<SaveConfirmationSnapshot> Capture(const std::filesystem::path& path) {
    SaveTargetRequest request; request.directory = path.parent_path().wstring(); request.filename = path.filename().wstring();
    auto capture = CaptureSaveConfirmation(request);
    Check(capture.error == ERROR_SUCCESS && capture.snapshot.has_value(), "capture isolated Save snapshot");
    return std::move(capture.snapshot);
}
}
int main() {
    Check(ValidateSaveAccessAttributes(FILE_ATTRIBUTE_ENCRYPTED, true)==ERROR_NOT_SUPPORTED,"readonly schema rejects encrypted target metadata");
    Check(ValidateSaveAccessAttributes(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_ENCRYPTED, true)==ERROR_NOT_SUPPORTED,"readonly schema rejects encrypted ancestor metadata");
    Check(ValidateSaveAccessAttributes(FILE_ATTRIBUTE_ENCRYPTED, false)==ERROR_SUCCESS,"ordinary Save attribute contract remains unchanged");
    Check(ValidateSaveAccessAttributes(FILE_ATTRIBUTE_DIRECTORY, true)==ERROR_SUCCESS,"ordinary ancestor permits restricted schema");
    Check(ValidateSaveAccessAttributes(FILE_ATTRIBUTE_REPARSE_POINT, true)==ERROR_NOT_SUPPORTED,"restricted schema retains reparse rejection");
    std::cout << std::unitbuf;
    const auto base = std::filesystem::absolute(L"bench_data") /
        (L"save-access-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(base.c_str(), nullptr)) { std::cout << "fixture create error=" << GetLastError() << '\n'; return 2; }
    std::wcerr << L"[FIXTURE] " << base << L'\n';
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) { RemoveDirectoryW(base.c_str()); return 2; }
    std::thread watchdog([&] {
        if (WaitForSingleObject(stop, 30000) == WAIT_TIMEOUT) {
            std::wcerr << L"[WATCHDOG] unfinished owned fixture: " << base << L"; never delete unknown replacements\n";
            TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    const auto existing = base / L"existing.txt";
    Check(CreateOwned(existing), "CREATE_NEW existing-file fixture");
    auto snapshot = Capture(existing);
    if (snapshot) {
        auto result = ValidateLocalSaveAccessV1(*snapshot); Evidence(result);
        Check(result.state == SaveAccessState::Ready && !result.error && result.cleanup == SaveAccessCleanup::NotRequired &&
            result.probe_identity == snapshot->TargetIdentity(), "existing access probe returns same confirmed identity");
        Check(Contents(existing) == "owned sentinel", "existing bytes preserved without truncation");
        Check(SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set owned readonly attribute");
        result = ValidateLocalSaveAccessV1(*snapshot); Evidence(result);
        Check(result.state == SaveAccessState::Failed && result.error == ERROR_ACCESS_DENIED, "readonly failure exact");
        Check(SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore owned attributes");
        HANDLE locked = CreateFileW(existing.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "existing file holder permits metadata but excludes write");
        result = ValidateLocalSaveAccessV1(*snapshot); Evidence(result);
        Check(result.state == SaveAccessState::Failed && result.error == ERROR_SHARING_VIOLATION, "existing sharing failure exact");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    }
    const auto fresh = base / L"fresh.txt";
    snapshot = Capture(fresh);
    if (snapshot) {
        auto result = ValidateLocalSaveAccessV1(*snapshot); Evidence(result);
        Check(result.state == SaveAccessState::Ready && !result.error && result.cleanup == SaveAccessCleanup::Confirmed &&
            result.probe_identity.has_value() && Absent(fresh), "new target probe closes and confirms absence");
        result = ValidateLocalSaveAccessV1(*snapshot, [] { return true; }); Evidence(result);
        Check(result.state == SaveAccessState::Failed && result.error == ERROR_CANCELLED &&
            result.cleanup == SaveAccessCleanup::NotRequired && Absent(fresh), "pre-cancel creates nothing");
        bool cancel = false;
        result = ValidateLocalSaveAccessV1(*snapshot, [&] { return cancel; }, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::Created) cancel = true;
        }); Evidence(result);
        Check(result.state == SaveAccessState::Failed && result.error == ERROR_CANCELLED &&
            result.cleanup == SaveAccessCleanup::Confirmed && Absent(fresh), "post-create cancel settles own cleanup");
        result = ValidateLocalSaveAccessV1(*snapshot, {}, [](SaveAccessStage stage) {
            if (stage == SaveAccessStage::Created) throw std::runtime_error("controlled observer failure");
        }); Evidence(result);
        Check(result.state == SaveAccessState::Failed && result.error == ERROR_UNHANDLED_EXCEPTION &&
            result.cleanup == SaveAccessCleanup::Confirmed && Absent(fresh), "post-create exception settles own cleanup");
        HANDLE metadata = INVALID_HANDLE_VALUE;
        result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::Created) {
                metadata = CreateFileW(fresh.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                std::cout << "[FACT] metadata holder error=" << (metadata == INVALID_HANDLE_VALUE ? GetLastError() : 0) << '\n';
            }
        }); Evidence(result);
        Check(metadata != INVALID_HANDLE_VALUE, "real metadata-only holder can coexist with new delete-on-close probe");
        Check(result.state == SaveAccessState::Failed && result.cleanup == SaveAccessCleanup::Unconfirmed && result.error,
            "delayed deletion is not falsely accepted");
        if (metadata != INVALID_HANDLE_VALUE) CloseHandle(metadata);
        Check(Absent(fresh), "last metadata holder close completes deletion without path delete");
    }
    const auto race = base / L"race.txt";
    snapshot = Capture(race);
    if (snapshot) {
        bool created = false;
        auto result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::BeforeOpen) created = CreateOwned(race, "competitor");
        }); Evidence(result);
        Check(created && result.state == SaveAccessState::NeedsConfirmation && result.confirmation &&
            result.cleanup == SaveAccessCleanup::NotRequired && Contents(race) == "competitor",
            "real CREATE_NEW race preserves competitor and requires confirmation");
    }
    const auto late = base / L"late.txt";
    snapshot = Capture(late);
    if (snapshot) {
        bool created = false;
        auto result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::AfterClose) created = CreateOwned(late, "late competitor");
        }); Evidence(result);
        Check(created && result.state == SaveAccessState::NeedsConfirmation && result.confirmation &&
            result.cleanup == SaveAccessCleanup::Confirmed && Contents(late) == "late competitor",
            "same name new ID after cleanup is preserved and requires confirmation");
    }
    const auto backup = base / L"original-backup.txt";
    snapshot = Capture(existing);
    if (snapshot) {
        bool attempted = false, moved_target = false, created_target = false, opened_target = false;
        DWORD move_error = 0, create_error = 0;
        auto result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::BeforeOpen) {
                attempted = true;
                moved_target = MoveFileW(existing.c_str(), backup.c_str()) != FALSE;
                if (!moved_target) move_error = GetLastError();
                if (moved_target) created_target = CreateOwned(existing, "replacement", &create_error, &opened_target);
            }
        }); Evidence(result);
        std::cout << "[FACT] in-flight replacement attempted=" << attempted << " move=" << moved_target
            << " move_error=" << move_error << " create_open=" << opened_target << " create_complete=" << created_target
            << " create_error=" << create_error << '\n';
        if (!moved_target) {
            auto unchanged = Capture(existing);
            Check(attempted && (move_error == ERROR_SHARING_VIOLATION || move_error == ERROR_ACCESS_DENIED) &&
                result.state == SaveAccessState::Ready && unchanged && snapshot->TargetIdentity() == unchanged->TargetIdentity() &&
                Contents(existing) == "owned sentinel" && Absent(backup),
                "in-flight replacement prevented and original object identity and contents preserved");
            // Perform the actual replacement after guards close. Do not count a
            // prevented mutation as coverage of replacement/reconfirmation.
            moved_target = MoveFileW(existing.c_str(), backup.c_str()) != FALSE;
            move_error = moved_target ? ERROR_SUCCESS : GetLastError();
            if (moved_target) created_target = CreateOwned(existing, "replacement", &create_error, &opened_target);
            std::cout << "[FACT] post-guard replacement move=" << moved_target << " move_error=" << move_error
                << " create_open=" << opened_target << " create_complete=" << created_target << " create_error=" << create_error << '\n';
            result = ValidateLocalSaveAccessV1(*snapshot); Evidence(result);
        }
        Check(moved_target && created_target && result.state == SaveAccessState::NeedsConfirmation && result.confirmation &&
            Contents(existing) == "replacement" && Contents(backup) == "owned sentinel",
            "real target replacement is never granted prior overwrite consent");
    }
    const auto parent = base / L"parent", moved = base / L"parent-moved";
    Check(CreateDirectoryW(parent.c_str(), nullptr) != FALSE, "create owned parent-guard fixture");
    snapshot = Capture(parent / L"new.txt");
    if (snapshot) {
        bool attempted = false, renamed = false, replacement = false, created_probe = false; DWORD rename_error = 0;
        std::optional<SaveConfirmationSnapshot> replacement_identity;
        auto result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::BeforeOpen) {
                attempted = true; renamed = MoveFileW(parent.c_str(), moved.c_str()) != FALSE;
                if (!renamed) rename_error = GetLastError();
                else {
                    replacement = CreateDirectoryW(parent.c_str(), nullptr) != FALSE;
                    if (replacement) replacement_identity = Capture(parent / L"identity.txt");
                }
            } else if (stage == SaveAccessStage::Created) created_probe = true;
        }); Evidence(result);
        std::cout << "[FACT] guarded parent rename error=" << rename_error << '\n';
        Check(attempted && !renamed && (rename_error == ERROR_SHARING_VIOLATION || rename_error == ERROR_ACCESS_DENIED) &&
            result.state == SaveAccessState::Ready, "real parent rename cannot redirect guarded creation");
        if (renamed) {
            Check(replacement && result.state == SaveAccessState::Failed && result.error == ERROR_FILE_INVALID &&
                result.cleanup == SaveAccessCleanup::NotRequired && !created_probe && Absent(parent / L"new.txt") && Absent(moved / L"new.txt"),
                "actual renamed parent plus same-name replacement never receives probe creation");
            if (replacement) {
                const auto cleanup_identity = Capture(parent / L"identity.txt");
                const bool same = replacement_identity && cleanup_identity &&
                    replacement_identity->ParentIdentity() == cleanup_identity->ParentIdentity();
                Check(same && RemoveDirectoryW(parent.c_str()), "remove only identity-matched empty test-created replacement");
            }
            Check(MoveFileW(moved.c_str(), parent.c_str()) != FALSE, "restore unexpectedly renamed owned parent");
        }
    }
    snapshot = Capture(parent / L"replacement-parent.txt");
    if (snapshot) {
        const bool moved_parent = MoveFileW(parent.c_str(), moved.c_str()) != FALSE;
        const bool new_parent = moved_parent && CreateDirectoryW(parent.c_str(), nullptr);
        Check(new_parent, "replace owned parent with a different directory identity");
        bool created_in_replacement = false;
        const auto result = ValidateLocalSaveAccessV1(*snapshot, {}, [&](SaveAccessStage stage) {
            if (stage == SaveAccessStage::Created) created_in_replacement = true;
        }); Evidence(result);
        Check(new_parent && !created_in_replacement && result.state == SaveAccessState::Failed && result.error == ERROR_FILE_INVALID &&
            result.cleanup == SaveAccessCleanup::NotRequired && Absent(parent / L"replacement-parent.txt"),
            "changed parent identity rejects before creation");
        if (moved_parent) Check(RemoveDirectoryW(moved.c_str()) != FALSE, "old owned parent removed nonrecursively");
    }
    Check(RemoveDirectoryW(parent.c_str()) != FALSE, "owned parent fixture empty after probe");
    // Only known test-created sentinel paths are deleted here, never fresh probe
    // leftovers. Any unconfirmed probe is retained for diagnosis by the caller.
    for (const auto& path : {existing, race, late, backup}) {
        Check(DeleteFileW(path.c_str()) != FALSE, "remove known test-created sentinel");
    }
    Check(Absent(fresh), "no fresh probe left to clean by path");
    Check(RemoveDirectoryW(base.c_str()) != FALSE, "owned fixture directory removed nonrecursively");
    SetEvent(stop); watchdog.join(); CloseHandle(stop);
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}
