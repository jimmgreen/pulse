#include "../ui/file_save_revalidation.h"

#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
int checks = 0, failures = 0;
void Check(bool condition, const char* label, DWORD error = 0) {
    ++checks;
    if (!condition) ++failures;
    std::printf("[%s] %s error=%lu\n", condition ? "PASS" : "FAIL", label, error);
}
struct Fixtures {
    std::filesystem::path root;
    std::vector<std::filesystem::path> files, directories;
    bool Directory(const std::filesystem::path& path) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
        directories.push_back(path);
        return true;
    }
    bool File(const std::filesystem::path& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        files.push_back(path);
        DWORD written = 0;
        const bool ok = WriteFile(file, "confirmation-fixture", 20, &written, nullptr) && written == 20;
        CloseHandle(file);
        return ok;
    }
    bool RenameFile(const std::filesystem::path& source, const std::filesystem::path& target) {
        if (!MoveFileW(source.c_str(), target.c_str())) return false;
        files.push_back(target);
        return true;
    }
    void Cleanup() {
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            const DWORD attributes = GetFileAttributesW(it->c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(it->c_str(), FILE_ATTRIBUTE_NORMAL);
            Check(DeleteFileW(it->c_str()) != FALSE, "owned file cleanup", GetLastError());
        }
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            if (GetFileAttributesW(it->c_str()) != INVALID_FILE_ATTRIBUTES)
                Check(RemoveDirectoryW(it->c_str()) != FALSE, "owned directory cleanup", GetLastError());
        }
        files.clear(); directories.clear();
    }
    ~Fixtures() { Cleanup(); }
};
}

int main(int argc, char** argv) {
    using namespace pulse::ui;
    using State = SaveRevalidationState;
    Fixtures fixtures;
    fixtures.root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"save_revalidation_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    if (!fixtures.Directory(fixtures.root)) { Check(false, "exclusive fixture directory", GetLastError()); return 1; }
    if (argc == 2 && std::string(argv[1]) == "--no-test-create") {
        SaveTargetRequest r; r.directory = fixtures.root.wstring(); r.filename = L"absent\\leaf.txt"; r.no_test_file_create = true;
        auto missing = CaptureSaveConfirmation(r);
        Check(missing.state == State::Ready && missing.snapshot && missing.snapshot->NoTestFileCreate(), "missing parent accepted without probe", missing.error);
        Check(!std::filesystem::exists(fixtures.root / L"absent"), "no parent or target created");
        if (missing.snapshot) {
            Check(RevalidateSaveConfirmation(*missing.snapshot).state == State::Ready, "missing path revalidated without creating");
            Check(RevalidateSaveConfirmation(*missing.snapshot, [] { return true; }).error == ERROR_CANCELLED, "cancellation retained");
        }
        r.require_existing_save_parent = true;
        Check(CaptureSaveConfirmation(r).state == State::Failed, "PATHMUSTEXIST rejects missing parent");
        r.require_existing_save_parent = false; r.no_test_file_create = false;
        Check(CaptureSaveConfirmation(r).state == State::Failed, "ordinary save does not accept missing parent");
        r.no_test_file_create = true;
        Check(fixtures.Directory(fixtures.root / L"absent") && fixtures.File(fixtures.root / L"absent" / L"leaf.txt"), "create owned replacement after first candidate");
        if (missing.snapshot) Check(RevalidateSaveConfirmation(*missing.snapshot).state == State::ConfirmOverwrite, "newly appeared target requires overwrite consent");
        HANDLE locked = CreateFileW((fixtures.root / L"absent" / L"leaf.txt").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(locked != INVALID_HANDLE_VALUE && CaptureSaveConfirmation(r).state == State::ConfirmOverwrite, "no-test existing exclusively locked target requires consent without write probe");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        auto existing = CaptureSaveConfirmation(r);
        Check(existing.state == State::ConfirmOverwrite, "existing target still requires consent");
        if (existing.snapshot) Check(RevalidateSaveConfirmation(*existing.snapshot).state == State::Ready, "confirmed existing target revalidated");
        SetFileAttributesW((fixtures.root / L"absent" / L"leaf.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        Check(CaptureSaveConfirmation(r).error == ERROR_ACCESS_DENIED, "read-only target remains rejected");
        r.filename = L"absent";
        Check(CaptureSaveConfirmation(r).error == ERROR_DIRECTORY, "directory target remains rejected");
        r.filename = L"absent\\..\\invalid.txt";
        Check(CaptureSaveConfirmation(r).state == State::Failed, "invalid path segments rejected");
        const auto guarded = fixtures.root / L"guarded", moved = fixtures.root / L"old-guarded";
        Check(fixtures.Directory(guarded), "create parent identity fixture");
        r.directory = guarded.wstring(); r.filename = L"new.txt";
        const auto parent_snapshot = CaptureSaveConfirmation(r);
        const bool renamed = MoveFileW(guarded.c_str(), moved.c_str()) != FALSE;
        if (renamed) fixtures.directories.push_back(moved);
        Check(renamed && fixtures.Directory(guarded), "replace owned parent while retaining original identity");
        Check(parent_snapshot.snapshot && RevalidateSaveConfirmation(*parent_snapshot.snapshot).error == ERROR_FILE_INVALID, "existing parent replacement remains rejected");
        fixtures.Cleanup(); std::printf("checks=%d failures=%d\n", checks, failures); return failures ? 1 : 0;
    }
    const auto parent = fixtures.root / L"parent";
    Check(fixtures.Directory(parent), "exclusive target directory");
    SaveTargetRequest request;
    request.directory = parent.wstring();
    request.filename = L"target.txt";
    const auto target = parent / request.filename;
    Check(fixtures.File(target), "exclusive target created");
    auto first = CaptureSaveConfirmation(request);
    Check(first.state == State::ConfirmOverwrite && first.error == 0 && first.snapshot &&
        first.snapshot->Path() == target.wstring() && first.snapshot->Parent() == parent.wstring() &&
        first.snapshot->TargetExists(), "capture normalized path and existing identity", first.error);
    if (!first.snapshot) { fixtures.Cleanup(); return 1; }
    auto result = RevalidateSaveConfirmation(*first.snapshot);
    Check(result.state == State::Ready && result.snapshot && result.error == 0, "same target and parent accepted", result.error);
    unsigned cancel_checks = 0;
    result = RevalidateSaveConfirmation(*first.snapshot, [&] { return ++cancel_checks == 2; });
    Check(result.state == State::Failed && result.error == ERROR_CANCELLED && !result.snapshot && cancel_checks == 2,
        "cancellation after parent query drops all partial metadata", result.error);
    Check(fixtures.RenameFile(target, parent / L"original.txt") && fixtures.File(target), "replacement keeps old object alive under different name");
    result = RevalidateSaveConfirmation(*first.snapshot);
    Check(result.state == State::ConfirmOverwrite && result.snapshot && result.snapshot->Path() == first.snapshot->Path(),
        "replacement requires fresh confirmation at same fixed path", result.error);
    if (result.snapshot) {
        auto confirmed = RevalidateSaveConfirmation(*result.snapshot);
        Check(confirmed.state == State::Ready && confirmed.snapshot, "new identity can be explicitly confirmed", confirmed.error);
    }
    Check(DeleteFileW(target.c_str()) != FALSE, "target removed after confirmation snapshot");
    result = RevalidateSaveConfirmation(*first.snapshot);
    Check(result.state == State::Ready && result.snapshot && !result.snapshot->TargetExists(), "target disappearance with stable parent becomes new target", result.error);
    const auto absent = CaptureSaveConfirmation(request);
    Check(absent.state == State::Ready && absent.snapshot && !absent.snapshot->TargetExists(), "capture absent target without creating it", absent.error);
    Check(GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "metadata capture did not create target");
    Check(fixtures.File(target), "target appeared after absent snapshot");
    if (absent.snapshot) {
        result = RevalidateSaveConfirmation(*absent.snapshot);
        Check(result.state == State::ConfirmOverwrite && result.snapshot, "newly appeared target requires confirmation", result.error);
    }
    const auto current = CaptureSaveConfirmation(request);
    Check(current.snapshot.has_value(), "fresh target snapshot captured", current.error);
    Check(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "readonly transition installed");
    if (current.snapshot) {
        result = RevalidateSaveConfirmation(*current.snapshot);
        Check(result.state == State::Failed && result.error == ERROR_ACCESS_DENIED && !result.snapshot,
            "readonly transition fails without accepted snapshot", result.error);
    }
    auto caller_request = request;
    caller_request.caller_validates_access = true;
    const auto caller_readonly = CaptureSaveConfirmation(caller_request);
    Check(caller_readonly.state == State::ConfirmOverwrite && caller_readonly.snapshot,
        "caller validation preserves overwrite confirmation for readonly target", caller_readonly.error);
    if (caller_readonly.snapshot) {
        const auto caller_confirmed = RevalidateSaveConfirmation(*caller_readonly.snapshot);
        Check(caller_confirmed.state == State::Ready && caller_confirmed.snapshot,
            "caller validation readonly snapshot survives read-only revalidation", caller_confirmed.error);
    }
    Check(SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "readonly transition restored");
    Check(DeleteFileW(target.c_str()) != FALSE && fixtures.Directory(target), "target replaced by directory");
    if (current.snapshot) {
        result = RevalidateSaveConfirmation(*current.snapshot);
        Check(result.state == State::Failed && result.error == ERROR_DIRECTORY && !result.snapshot,
            "directory target fails without partial result", result.error);
    }
    Check(RemoveDirectoryW(target.c_str()) != FALSE, "directory target removed");
    // Move rather than delete the old parent so its identity cannot be reused.
    const auto moved = fixtures.root / L"old-parent";
    const bool moved_ok = MoveFileW(parent.c_str(), moved.c_str()) != FALSE;
    Check(moved_ok, "old parent retained at another path");
    if (moved_ok) {
        fixtures.directories.push_back(moved);
        fixtures.files.push_back(moved / L"original.txt");
        result = RevalidateSaveConfirmation(*first.snapshot);
        Check(result.state == State::Failed && result.error != ERROR_SUCCESS && !result.snapshot,
            "missing parent is a real error, never absent target", result.error);
        Check(fixtures.Directory(parent), "replacement parent created at original path");
        result = RevalidateSaveConfirmation(*first.snapshot);
        Check(result.state == State::Failed && result.error == ERROR_FILE_INVALID && !result.snapshot,
            "replacement parent invalidates confirmation", result.error);
    }
    result = RevalidateSaveConfirmation(*first.snapshot, [] { return true; });
    Check(result.state == State::Failed && result.error == ERROR_CANCELLED && !result.snapshot,
        "cancelled work returns no snapshot", result.error);
    // ResolveSaveTarget classifies trailing-dot names as Unsupported rather
    // than silently normalizing them, including the parent-directory token.
    request.filename = L"..";
    const auto unsupported = CaptureSaveConfirmation(request);
    Check(ResolveSaveTarget(request).state == SaveTargetState::Unsupported &&
        unsupported.state == State::Failed && unsupported.error == ERROR_NOT_SUPPORTED && !unsupported.snapshot,
        "unsupported trailing-dot path token preserves exact model classification", unsupported.error);
    request.filename = L"invalid:name.txt";
    const auto invalid = CaptureSaveConfirmation(request);
    Check(ResolveSaveTarget(request).state == SaveTargetState::InvalidName &&
        invalid.state == State::Failed && invalid.error == ERROR_INVALID_NAME && !invalid.snapshot,
        "invalid leaf preserves exact invalid-name classification without acceptance", invalid.error);
    fixtures.Cleanup();
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}

