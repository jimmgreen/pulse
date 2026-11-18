#include "../ui/file_save_model.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
} // namespace

int main(int argc, char** argv) {
    using namespace pulse::ui;
    using S = SaveTargetState;
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
    for (const auto* invalid : {L"", L"..", L"NUL.txt", L"COM1", L"LPT\u00b9.log", L"a:b", L"sub\\file", L"a?b"}) {
        request.filename = invalid;
        Check(ResolveSaveTarget(request).state != S::Resolved, "invalid leaf name is not accepted");
    }
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
