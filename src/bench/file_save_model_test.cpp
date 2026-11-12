#include "../ui/file_save_model.h"
#ifdef PULSE_SAVE_PATH_PIPELINE_TEST
#include "../ui/file_save_access_policy.h"
#endif

#include <cstdio>
#include <filesystem>
#include <shobjidl.h>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

int CreateDirectoriesTest() {
    using namespace pulse::ui;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"save-create-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!CreateDirectoryW(root.c_str(), nullptr)) { Check(false, "isolated creation fixture"); return 1; }
    SaveTargetRequest request;
    request.directory = root.wstring(); request.filename = L"one\\two\\report"; request.default_extension = L"txt";
    auto plan = PlanSaveDirectories(request);
    Check(!plan.error && plan.missing.size() == 2 && !std::filesystem::exists(root / L"one"), "plan lists missing ancestors without creating anything");
    Check(plan.target == (root / L"one" / L"two" / L"report.txt").wstring(), "plan preserves resolved extension");
    Check(PlanSaveDirectories(request, FOS_PATHMUSTEXIST).error != 0, "native PATHMUSTEXIST never creates directories");
    Check(PlanSaveDirectories(request, FOS_NOTESTFILECREATE).error != 0, "native NOTESTFILECREATE never creates directories");
    request.require_existing_save_parent = true;
    Check(PlanSaveDirectories(request).error != 0, "require-existing parent remains rejection");
    request.require_existing_save_parent = false; request.no_test_file_create = true;
    Check(PlanSaveDirectories(request).error != 0, "no-test request remains nonmutating");
    request.no_test_file_create = false;
    const auto cancelled = CreateSaveDirectories(request, plan, 0, [] { return true; });
    Check(cancelled.error == ERROR_CANCELLED && cancelled.created.empty() && !std::filesystem::exists(root / L"one"), "cancel before consent work leaves no directory");
    const auto created = CreateSaveDirectories(request, plan);
    Check(!created.error && created.created.size() == 2 && std::filesystem::is_directory(root / L"one" / L"two"), "approved directories created in ancestor order");
    Check(!std::filesystem::exists(plan.target) && ValidateSaveTarget(request).state == SaveTargetState::Ready, "creation never creates file and original save validation resumes");
    Check(CreateSaveDirectories(request, plan).error == ERROR_RETRY, "stale creation consent is not silently reused");
    request.filename = L"..\\outside\\report.txt";
    Check(PlanSaveDirectories(request).error == ERROR_INVALID_NAME, "parent traversal never auto-creates outside base");
    request.filename = (root / L"absolute" / L"report.txt").wstring();
    Check(PlanSaveDirectories(request).error == ERROR_NOT_SUPPORTED, "absolute missing targets are not automatic creation candidates");
    const auto occupied = root / L"occupied";
    { std::ofstream f(occupied); f << "keep"; }
    request.filename = L"occupied\\report.txt";
    Check(PlanSaveDirectories(request).error == ERROR_DIRECTORY, "existing file is never replaced by directory");
    request.filename = L"race\\report.txt";
    plan = PlanSaveDirectories(request);
    const auto race = root / L"race";
    CreateDirectoryW(race.c_str(), nullptr);
    Check(CreateSaveDirectories(request, plan).error == ERROR_RETRY, "changed directory plan requires new consent");
    request.filename = L"partial\\next\\report.txt";
    plan = PlanSaveDirectories(request);
    const auto partial = root / L"partial";
    const auto interrupted = CreateSaveDirectories(request, plan, 0, [&] { return std::filesystem::exists(partial); });
    Check(interrupted.error == ERROR_CANCELLED && interrupted.created.size() == 1 &&
        std::filesystem::is_directory(partial) && !std::filesystem::exists(partial / L"next"),
        "cancellation stops next create and reports retained approved directories");
    Check(RemoveDirectoryW(partial.c_str()) != FALSE, "partial creation evidence cleaned explicitly");
    const bool cleaned = RemoveDirectoryW(race.c_str()) && DeleteFileW(occupied.c_str()) &&
        RemoveDirectoryW((root / L"one" / L"two").c_str()) && RemoveDirectoryW((root / L"one").c_str()) && RemoveDirectoryW(root.c_str());
    Check(cleaned, "creation fixtures cleaned without recursive deletion");
    return failures ? 1 : 0;
}

int PathTargetsTest() {
    using namespace pulse::ui;
    using S = SaveTargetState;
    for (bool no_test : {false, true}) {
        SaveTargetRequest request;
        request.directory = L"C:\\fixture";
        request.default_extension = L"txt";
        request.no_test_file_create = no_test;
        const auto resolves = [&](const wchar_t* input, const wchar_t* expected, const char* label) {
            request.filename = input;
            const auto result = ResolveSaveTarget(request);
            Check(result.state == S::Resolved && result.path == expected, label);
        };
        resolves(L"D:\\other\\report", L"D:\\other\\report.txt", "absolute Save target does not depend on no-test flag");
        resolves(L"sub\\report", L"C:\\fixture\\sub\\report.txt", "relative Save target uses explicit picker folder");
        resolves(L"sub/report", L"C:\\fixture\\sub\\report.txt", "relative path separators normalize before strict parsing");
        resolves(L"D:/other/report", L"D:\\other\\report.txt", "absolute path separators normalize before strict parsing");
        resolves(L"C:\\folder.v1\\report", L"C:\\folder.v1\\report.txt", "extension detection applies to leaf, not parent");
        resolves(L"C:\\folder with spaces\\ new file.custom", L"C:\\folder with spaces\\ new file.custom", "path spaces and explicit extension remain literal");
        resolves(L"\\\\?\\C:\\other\\report", L"C:\\other\\report.txt", "extended absolute target is parsed before validation");
        resolves(L"\\\\server\\share\\folder\\report", L"\\\\server\\share\\folder\\report.txt", "UNC resolution remains metadata-free");
        resolves(L"\\\\?\\UNC\\server\\share\\folder\\report", L"\\\\server\\share\\folder\\report.txt", "extended UNC target retains share boundary");
        request.filters = {{L"Text", L"*.txt"}, {L"Image", L"*.png"}};
        request.filter_index = 1; request.native_save_defaults = true;
        request.filename = L"sub\\report";
        auto selected = ResolveSaveTarget(request);
        Check(selected.state == S::Resolved && selected.path == L"C:\\fixture\\sub\\report.png" && selected.filter_index == 1,
            "path target keeps negotiated current filter and filter index");
        request.filters.clear(); request.filter_index = 0; request.native_save_defaults = false;
        for (const auto* invalid : {L"sub\\..\\report", L"sub\\.\\report", L"sub\\\\report",
                 L"C:\\other\\NUL.txt", L"sub\\COM1", L"sub\\file:stream",
                 L"\\\\.\\pipe\\report", L"C:sub\\report", L"\\sub\\report"}) {
            request.filename = invalid;
            Check(ResolveSaveTarget(request).state != S::Resolved, "path target retains strict syntax and device-name boundaries");
        }
        for (const auto* unsupported : {L"sub\\trailing.", L"sub\\trailing "}) {
            request.filename = unsupported;
            Check(ResolveSaveTarget(request).state == S::Unsupported, "path leaf trailing dot or space is not silently trimmed");
        }
        request.directory = L"relative"; request.filename = L"sub\\report";
        Check(ResolveSaveTarget(request).state != S::Resolved, "relative target never uses process CWD implicitly");
        Check(request.no_test_file_create == no_test && !request.caller_validates_access && !request.require_existing_save_parent,
            "path parsing never grants no-test or caller-access flags");
    }
#ifdef PULSE_SAVE_PATH_PIPELINE_TEST
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"save-path-target-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto child = root / L"child", guarded = root / L"guarded", moved = root / L"old-guarded";
    const auto existing = root / L"existing.txt", previous = root / L"old-existing.txt";
    if (!CreateDirectoryW(root.c_str(), nullptr)) { Check(false, "exclusive path-model fixture created"); return 1; }
    const bool child_created = CreateDirectoryW(child.c_str(), nullptr) != FALSE;
    const bool guarded_created = CreateDirectoryW(guarded.c_str(), nullptr) != FALSE;
    Check(child_created && guarded_created, "owned path parents created");
    SaveTargetRequest request;
    request.directory = root.wstring(); request.filename = (child / L"new.txt").wstring();
    const auto resolved = ValidateSaveTarget(request);
    Check(resolved.state == S::Ready && !std::filesystem::exists(child / L"new.txt"),
        "ordinary absolute target receives metadata validation without creating it");
    const auto captured = CaptureSaveConfirmation(request);
    Check(captured.state == SaveRevalidationState::Ready && captured.snapshot && !captured.snapshot->NoTestFileCreate(),
        "absolute target captures normal parent identity, without granting no-test");
    if (captured.snapshot) {
        const auto access = ValidateLocalSaveAccessV1(*captured.snapshot);
        Check(access.state == SaveAccessState::Ready && access.cleanup == SaveAccessCleanup::Confirmed &&
              !std::filesystem::exists(child / L"new.txt"),
            "ordinary absolute new target still performs access probe and confirmed cleanup");
        const auto denied = ValidateLocalSaveAccessV1(*captured.snapshot, [] { return true; });
        Check(denied.state == SaveAccessState::Failed && denied.error == ERROR_CANCELLED,
            "path target preserves cancellation before access");
    }
    { std::ofstream output(existing, std::ios::binary); output << "unchanged"; }
    request.filename = existing.wstring();
    auto metadata = ValidateSaveTarget(request);
    Check(metadata.state == S::ConfirmOverwrite && metadata.target_exists, "absolute existing target still requires overwrite confirmation");
    const auto overwrite = CaptureSaveConfirmation(request);
    Check(overwrite.state == SaveRevalidationState::ConfirmOverwrite && overwrite.snapshot,
        "absolute target retains overwrite identity snapshot");
    HANDLE lock = CreateFileW(existing.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "owned target holder excludes write access");
    if (overwrite.snapshot && lock != INVALID_HANDLE_VALUE) {
        const auto access = ValidateLocalSaveAccessV1(*overwrite.snapshot);
        Check(access.state == SaveAccessState::Failed && access.error == ERROR_SHARING_VIOLATION,
            "full path cannot bypass caller write-sharing boundary");
    }
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    const bool marked = SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    metadata = ValidateSaveTarget(request);
    Check(marked && metadata.read_only && metadata.state == S::ConfirmOverwrite &&
          CaptureSaveConfirmation(request).error == ERROR_ACCESS_DENIED, "absolute path retains read-only metadata and access rejection");
    SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (overwrite.snapshot) {
        const bool renamed = MoveFileW(existing.c_str(), previous.c_str()) != FALSE;
        if (renamed) { std::ofstream output(existing, std::ios::binary); output << "replacement"; }
        const auto changed = RevalidateSaveConfirmation(*overwrite.snapshot);
        Check(renamed && changed.state == SaveRevalidationState::ConfirmOverwrite && changed.snapshot,
            "full-path target replacement still invalidates old overwrite consent");
        const auto access = ValidateLocalSaveAccessV1(*overwrite.snapshot);
        Check(access.state == SaveAccessState::NeedsConfirmation, "full path cannot skip fresh consent after replacement");
    }
    request.filename = (root / L"missing" / L"new.txt").wstring();
    Check(ValidateSaveTarget(request).state == S::ParentMissing && CaptureSaveConfirmation(request).state == SaveRevalidationState::Failed,
        "ordinary full path does not gain missing-parent permission");
    request.filename = child.wstring();
    Check(ValidateSaveTarget(request).state == S::TargetDirectory && CaptureSaveConfirmation(request).error == ERROR_DIRECTORY,
        "absolute directory target remains rejected");
    request.filename = (guarded / L"new.txt").wstring();
    const auto parent = CaptureSaveConfirmation(request);
    const bool renamed_parent = MoveFileW(guarded.c_str(), moved.c_str()) != FALSE;
    const bool recreated_parent = renamed_parent && CreateDirectoryW(guarded.c_str(), nullptr);
    Check(parent.snapshot && renamed_parent && recreated_parent &&
          RevalidateSaveConfirmation(*parent.snapshot).error == ERROR_FILE_INVALID,
        "full path retains parent replacement revalidation");
    { std::ifstream input(std::filesystem::exists(previous) ? previous : existing, std::ios::binary); std::string content; input >> content;
      Check(content == "unchanged", "path validation and access checks preserve original bytes"); }
    const bool files_cleaned = DeleteFileW(existing.c_str()) && (!std::filesystem::exists(previous) || DeleteFileW(previous.c_str()));
    const bool dirs_cleaned = RemoveDirectoryW(child.c_str()) && RemoveDirectoryW(guarded.c_str()) &&
        (!std::filesystem::exists(moved) || RemoveDirectoryW(moved.c_str())) && RemoveDirectoryW(root.c_str());
    Check(files_cleaned && dirs_cleaned, "owned path fixtures cleaned without deleting unknown new targets");
#endif
    std::printf("Save path target failures=%d\n", failures);
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--create-directories") return CreateDirectoriesTest();
    using namespace pulse::ui;
    using S = SaveTargetState;
    if (argc == 2 && std::string_view(argv[1]) == "--infer-filter") {
        const std::vector<PickerFilter> filters = {{L"Text", L"*.txt"}, {L"Images", L"*.png;*.jpg"}, {L"All", L"*.*"}};
        const auto infer = [&](std::wstring_view name, size_t current = 0) {
            return InferSaveFilterFromFilename(name, filters, current);
        };
        Check(infer(L"report.PNG") == 1, "typed extension selects unique concrete type case insensitively");
        Check(infer(L"child\\report.JpG") == 1, "relative filename uses leaf extension");
        Check(infer(L"child.v1/report.png") == 1, "relative slash path ignores parent dots");
        Check(infer(L"C:\\folder.v1\\report.png") == 1, "absolute path uses leaf without filesystem access");
        Check(infer(L"report.png", 2) == 1, "all-files does not mask unique concrete type");
        Check(!infer(L"report.jpg", 1), "current matching multi-extension type remains selected");
        for (const auto* name : {L"report", L"report.", L".png", L".hidden.png", L"report.png ", L"child.png\\report", L"child\\", L"report.unknown", L"report?.png"})
            Check(!infer(name), "ambiguous incomplete or unknown filename does not infer type");
        Check(!infer(L"report.png", 9), "invalid current index cannot infer");
        const std::vector<PickerFilter> duplicate = {{L"All", L"*.*"}, {L"PNG one", L"*.png"}, {L"PNG two", L"*.PNG;*.jpg"}};
        Check(!InferSaveFilterFromFilename(L"report.png", duplicate, 0), "two caller formats sharing extension are not guessed");
        Check(!InferSaveFilterFromFilename(L"report.png", duplicate, 2), "already matching caller format stays unchanged despite ambiguity");
        const std::vector<PickerFilter> wildcard = {{L"Text", L"*.txt"}, {L"Mixed", L"*.png;*.*"}, {L"Excel", L"*.xl*"}, {L"Loose", L"report*.png"}};
        Check(!InferSaveFilterFromFilename(L"report.png", wildcard, 0), "mixed and arbitrary wildcard masks cannot infer a format");
        Check(!InferSaveFilterFromFilename(L"report.xlsx", wildcard, 0), "partial extension wildcard cannot infer a format");
        const std::vector<PickerFilter> compound = {{L"Text", L"*.txt"}, {L"Tar", L"*.tar.gz"}};
        Check(InferSaveFilterFromFilename(L"archive.TAR.GZ", compound, 0) == 1, "concrete compound extension matches entire suffix");
        auto overlap = compound; overlap.push_back({L"Gzip", L"*.gz"});
        Check(!InferSaveFilterFromFilename(L"archive.tar.gz", overlap, 0), "overlapping compound extension formats are ambiguous");
        const std::vector<PickerFilter> repeated = {{L"Text", L"*.txt"}, {L"PNG", L"*.png;*.PNG"}};
        Check(InferSaveFilterFromFilename(L"report.png", repeated, 0) == 1, "duplicate masks inside one caller type remain unambiguous");
        std::printf("FAILURES=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--path-targets") return PathTargetsTest();
    if (argc == 2 && std::string_view(argv[1]) == "--native-defaults") {
        SaveTargetRequest native;
        native.directory = L"C:\\fixture"; native.native_save_defaults = true;
        native.filters = {{L"HTML", L"*.html"}, {L"All", L"*.*"}};
        for (const auto* extension : {L"", L"txt", L"*.txt", L"*.html"}) {
            native.default_extension = extension;
            for (bool changed : {false, true}) {
                native.filter_changed = changed; native.filename = L"export";
                Check(ResolveSaveTarget(native).path == L"C:\\fixture\\export.html", "current literal filter wins with native defaults");
                native.filename = L"export.custom";
                Check(ResolveSaveTarget(native).path == L"C:\\fixture\\export.custom", "explicit filename extension retained");
            }
        }
        native.filter_index = 1; native.filename = L"export"; native.default_extension = L"txt";
        Check(ResolveSaveTarget(native).path == L"C:\\fixture\\export.txt", "All filter uses literal default");
        native.default_extension = L"*.html";
        Check(ResolveSaveTarget(native).state == S::Unsupported, "All filter does not strip wildcard default");
        native.filename = L"export.custom";
        Check(ResolveSaveTarget(native).path.ends_with(L"export.custom"), "explicit extension ignores simple wildcard default");
        for (const auto* invalid : {L"../html", L"C:\\html", L"*.html;*.txt", L"*.h*", L".html"}) {
            native.default_extension = invalid;
            Check(ResolveSaveTarget(native).state == S::Unsupported, "unverified default syntax stays rejected");
        }
        native.native_save_defaults = false; native.filter_index = 0; native.filename = L"export"; native.default_extension = L"txt"; native.filter_changed = false;
        Check(ResolveSaveTarget(native).path.ends_with(L"export.txt"), "unnegotiated initial precedence unchanged");
        native.default_extension = L"*.html";
        Check(ResolveSaveTarget(native).state == S::Unsupported, "unnegotiated wildcard default rejected");
        return failures ? 1 : 0;
    }
    SaveTargetRequest request;
    request.directory = L"C:\\fixture";
    request.filename = L"report";
    request.default_extension = L"txt";
    request.filters = {{L"Text", L"*.txt"}, {L"Images", L"*.png;*.jpg"}, {L"All", L"*.*"}};
    auto resolved = ResolveSaveTarget(request);
    Check(resolved.state == S::Resolved && resolved.path == L"C:\\fixture\\report.txt", "initial explicit default extension");
    request.filter_index = 1;
    Check(ResolveSaveTarget(request).path.ends_with(L"report.txt"), "initial filter does not silently override explicit default");
    request.filter_changed = true;
    Check(ResolveSaveTarget(request).path.ends_with(L"report.png"), "changed filter uses its first concrete extension");
    request.filter_index = 2;
    Check(ResolveSaveTarget(request).path.ends_with(L"report.txt"), "all-files selection restores original default extension");
    request.filename = L"report.custom";
    Check(ResolveSaveTarget(request).path.ends_with(L"report.custom"), "typed extension is preserved");
    request.filter_index = 3;
    Check(ResolveSaveTarget(request).state == S::InvalidFilter, "out-of-range zero-based filter rejected");
    request.filter_index = 0;
    request.filters[0].pattern = L"report*.txt";
    Check(ResolveSaveTarget(request).state == S::Unsupported, "complex wildcard requires fallback");
    request.filters.clear();
    request.default_extension = L".txt";
    Check(ResolveSaveTarget(request).state == S::Unsupported, "default extension with leading dot requires fallback");
    request.default_extension.clear();
    request.filename = L"new-file";
    Check(ResolveSaveTarget(request).path.ends_with(L"new-file"), "no default does not invent an extension");
    for (const auto* invalid : {L"", L"..", L"NUL.txt", L"COM1", L"LPT\u00b9.log", L"a:b", L"a?b"}) {
        request.filename = invalid;
        Check(ResolveSaveTarget(request).state != S::Resolved, "invalid leaf name is not accepted");
    }
    request.filename = L"sub\\file";
    Check(ResolveSaveTarget(request).path == L"C:\\fixture\\sub\\file", "relative path is validated as a target, not an invalid leaf");
    request.filename = L"trailing.";
    Check(ResolveSaveTarget(request).state == S::Unsupported, "trailing dot is not silently renamed");
    request.filename = std::wstring(L"a\0b", 3);
    Check(ResolveSaveTarget(request).state == S::InvalidName, "embedded NUL rejected");
    request.filename = std::wstring(1, static_cast<wchar_t>(0xd800));
    Check(ResolveSaveTarget(request).state == S::InvalidName, "invalid UTF-16 rejected");
    request.filename = L"report";
    request.directory = L"relative";
    Check(ResolveSaveTarget(request).state == S::InvalidDirectory, "process CWD is never implicit");
    request.directory = L"\\\\server\\share\\folder";
    Check(ResolveSaveTarget(request).state == S::Resolved, "UNC resolution requires no network access");
    request.directory = L"\\\\?\\C:\\folder";
    Check(ResolveSaveTarget(request).path == L"C:\\folder\\report", "extended drive prefix accepted");
    request.directory = L"\\\\.\\pipe\\name";
    Check(ResolveSaveTarget(request).state == S::InvalidDirectory, "device namespace rejected");

    // All filesystem mutations are fixtures below the repository's bench_data.
    const auto bench = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / L"bench_data";
    const auto root = std::filesystem::absolute(bench / (L"save-model-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64())));
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        Check(false, "isolated fixture directory created");
        return 1;
    }
    request.directory = root.wstring();
    request.filename = L"new.txt";
    const auto new_path = root / request.filename;
    Check(ValidateSaveTarget(request).state == S::Ready && !std::filesystem::exists(new_path),
        "new target accepted without creating it");
    const auto existing = root / L"existing.txt";
    { std::ofstream output(existing, std::ios::binary); output << "unchanged"; }
    request.filename = L"existing.txt";
    auto result = ValidateSaveTarget(request);
    Check(result.state == S::ConfirmOverwrite && result.target_exists && !result.read_only,
        "existing file requests explicit overwrite confirmation");
    const bool marked_read_only = SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    result = ValidateSaveTarget(request);
    Check(marked_read_only && result.state == S::ConfirmOverwrite && result.read_only,
        "read-only is separate from overwrite confirmation");
    SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL);
    { std::ifstream input(existing, std::ios::binary); std::string content; input >> content;
      Check(content == "unchanged", "validation never truncates or writes existing contents"); }
    request.directory = existing.wstring();
    Check(ValidateSaveTarget(request).state == S::ParentNotDirectory, "file cannot serve as parent directory");
    request.directory = (root / L"absent").wstring();
    Check(ValidateSaveTarget(request).state == S::ParentMissing, "missing parent is reported");
    const auto child = root / L"child";
    const bool child_created = CreateDirectoryW(child.c_str(), nullptr) != FALSE;
    request.directory = root.wstring();
    request.filename = L"child";
    Check(child_created && ValidateSaveTarget(request).state == S::TargetDirectory, "directory cannot be overwritten as file");
    const bool cleaned = RemoveDirectoryW(child.c_str()) && DeleteFileW(existing.c_str()) && RemoveDirectoryW(root.c_str());
    Check(cleaned, "isolated fixtures removed");
    return failures ? 1 : 0;
}
