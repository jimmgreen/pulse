// ai_archive_compare_test.cpp - Tests for Batch 6: archive inspect, Zip-Slip
// guarded non-overwriting extract (System32 archiveint.dll), and folder compare.
#include "../app/ai_archive_tools.h"
#include "../app/zip_compress.h"
#include "../common/utf8_file.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* name) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::fprintf(stderr, "[FAIL] %s\n", name);
    }
}

void SetFileTimeOffsetSeconds(const std::wstring& path, int64_t delta_sec) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    u.QuadPart = static_cast<ULONGLONG>(static_cast<int64_t>(u.QuadPart) + delta_sec * 10000000ll);
    FILETIME out{u.LowPart, u.HighPart};
    SetFileTime(h, nullptr, nullptr, &out);
    CloseHandle(h);
}

} // namespace

int main() {
    using namespace pulse::app::ai;
    namespace fs = std::filesystem;

    // 1. Zip-Slip & unsafe filename sanitization
    std::wstring rel;
    Check(SanitizeArchiveRelativePath(L"docs/report.txt", rel) && rel == L"docs\\report.txt",
          "Sanitize normal nested path");
    Check(SanitizeArchiveRelativePath(L"./docs/2026/\xE6\x8A\xA5\xE4\xBB\xB7.pdf", rel) &&
              rel.rfind(L"docs\\2026\\", 0) == 0,
          "Sanitize dot-prefix relative path");
    Check(!SanitizeArchiveRelativePath(L"../evil.txt", rel), "Reject parent traversal ../");
    Check(!SanitizeArchiveRelativePath(L"docs/../../evil.txt", rel), "Reject nested traversal");
    Check(!SanitizeArchiveRelativePath(L"/etc/passwd", rel), "Reject leading /");
    Check(!SanitizeArchiveRelativePath(L"\\Windows\\System32\\x.dll", rel), "Reject leading backslash");
    Check(!SanitizeArchiveRelativePath(L"C:\\Windows\\evil.txt", rel), "Reject drive letter");
    Check(!SanitizeArchiveRelativePath(L"\\\\server\\share\\x.txt", rel), "Reject UNC");
    Check(!SanitizeArchiveRelativePath(L"docs/CON.txt", rel), "Reject Windows reserved device CON");
    Check(!SanitizeArchiveRelativePath(L"docs/bad:name.txt", rel), "Reject colon in filename");
    Check(!SanitizeArchiveRelativePath(L"", rel), "Reject empty path");

    // Create isolated workspace in TEMP
    wchar_t tmp_buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp_buf);
    const fs::path root = fs::path(tmp_buf) / (L"pulse_archive_compare_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"src" / L"sub", ec);

    // 2. Real ZIP creation -> InspectArchive -> ExtractArchive (all, collision non-overwrite, selective)
    const fs::path f1 = root / L"src" / L"readme.txt";
    const fs::path f2 = root / L"src" / L"sub" / L"\x62A5\x4EF7.md";
    Check(pulse::WriteUtf8FileAtomic(f1.wstring(), L"Hello Pulse Archive!"), "Write readme.txt");
    Check(pulse::WriteUtf8FileAtomic(f2.wstring(), L"# \x62A5\x4EF7\x5355\n128500"), "Write Chinese markdown");

    const fs::path zip_path = root / L"sample.zip";
    Check(pulse::app::ZipCompressAvailable(), "System32 archiveint.dll available");
    const auto zr = pulse::app::CompressToZip({f1.wstring(), (root / L"src" / L"sub").wstring()}, zip_path.wstring(),
                                              nullptr, nullptr);
    Check(zr.ok && zr.files >= 2, "CompressToZip creates real sample.zip");

    std::atomic<bool> cancelled{false};
    {
        const ArchiveInspectResult ins = InspectArchive(zip_path.wstring(), 100, cancelled);
        Check(ins.ok, "InspectArchive succeeds");
        Check(ins.file_count == 2, "InspectArchive sees 2 files");
        Check(ins.total_uncompressed > 20, "InspectArchive reports uncompressed size");
        bool saw_readme = false, saw_cn = false;
        for (const auto& e : ins.entries) {
            if (e.path == L"readme.txt") saw_readme = true;
            if (e.path.find(L"\x62A5\x4EF7.md") != std::wstring::npos) saw_cn = true;
        }
        Check(saw_readme && saw_cn, "InspectArchive lists ASCII & Chinese paths accurately");
    }

    const fs::path ext_dir = root / L"out_all";
    {
        const ArchiveExtractResult er = ExtractArchive(zip_path.wstring(), ext_dir.wstring(), {}, cancelled);
        Check(er.ok && er.extracted_files == 2 && er.skipped_unsafe == 0, "ExtractArchive full succeeds");
        std::wstring read_back;
        Check(pulse::ReadUtf8File((ext_dir / L"readme.txt").wstring(), read_back) &&
                  read_back == L"Hello Pulse Archive!",
              "Extracted readme.txt contents match");
        Check(pulse::ReadUtf8File((ext_dir / L"sub" / L"\x62A5\x4EF7.md").wstring(), read_back) &&
                  read_back.find(L"128500") != std::wstring::npos,
              "Extracted Chinese file contents match");
    }

    // Re-extract into the same folder -> must NOT overwrite readme.txt, must create readme (2).txt!
    Check(pulse::WriteUtf8FileAtomic((ext_dir / L"readme.txt").wstring(), L"KEEP EXISTING"), "Modify existing file");
    {
        const ArchiveExtractResult er2 = ExtractArchive(zip_path.wstring(), ext_dir.wstring(), {}, cancelled);
        Check(er2.ok && er2.extracted_files == 2, "Second extract into non-empty folder succeeds");
        std::wstring orig, copy2;
        Check(pulse::ReadUtf8File((ext_dir / L"readme.txt").wstring(), orig) && orig == L"KEEP EXISTING",
              "Existing file NOT overwritten");
        Check(pulse::ReadUtf8File((ext_dir / L"readme (2).txt").wstring(), copy2) && copy2 == L"Hello Pulse Archive!",
              "Collision auto-renamed to readme (2).txt");
    }

    // Selective extract of 1 file
    const fs::path sel_dir = root / L"out_sel";
    {
        const ArchiveExtractResult er3 =
            ExtractArchive(zip_path.wstring(), sel_dir.wstring(), {L"readme.txt"}, cancelled);
        Check(er3.ok && er3.extracted_files == 1, "Selective extract writes only requested entry");
        Check(fs::exists(sel_dir / L"readme.txt") && !fs::exists(sel_dir / L"sub"),
              "Selective extract skips unselected entries");
    }

    // 3. Folder compare covering all 6 categories
    const fs::path left = root / L"left";
    const fs::path right = root / L"right";
    fs::create_directories(left, ec);
    fs::create_directories(right, ec);

    pulse::WriteUtf8FileAtomic((left / L"same.txt").wstring(), L"identical");
    pulse::WriteUtf8FileAtomic((right / L"same.txt").wstring(), L"identical");
    SetFileTimeOffsetSeconds((left / L"same.txt").wstring(), 0);
    SetFileTimeOffsetSeconds((right / L"same.txt").wstring(), 0);

    pulse::WriteUtf8FileAtomic((left / L"only_l.txt").wstring(), L"L");
    pulse::WriteUtf8FileAtomic((right / L"only_r.txt").wstring(), L"R");

    pulse::WriteUtf8FileAtomic((left / L"newer_l.txt").wstring(), L"new");
    pulse::WriteUtf8FileAtomic((right / L"newer_l.txt").wstring(), L"old");
    SetFileTimeOffsetSeconds((left / L"newer_l.txt").wstring(), 0);
    SetFileTimeOffsetSeconds((right / L"newer_l.txt").wstring(), -60);

    pulse::WriteUtf8FileAtomic((left / L"newer_r.txt").wstring(), L"old");
    pulse::WriteUtf8FileAtomic((right / L"newer_r.txt").wstring(), L"new");
    SetFileTimeOffsetSeconds((left / L"newer_r.txt").wstring(), -60);
    SetFileTimeOffsetSeconds((right / L"newer_r.txt").wstring(), 0);

    pulse::WriteUtf8FileAtomic((left / L"diff_sz.txt").wstring(), L"short");
    pulse::WriteUtf8FileAtomic((right / L"diff_sz.txt").wstring(), L"much longer content");
    SetFileTimeOffsetSeconds((left / L"diff_sz.txt").wstring(), -10);
    SetFileTimeOffsetSeconds((right / L"diff_sz.txt").wstring(), -10);

    {
        const FolderCompareSummary cmp = CompareFolderPaths(left.wstring(), right.wstring(), cancelled);
        Check(cmp.ok, "CompareFolderPaths succeeds");
        Check(cmp.same_count == 1, "CompareFolderPaths same_count == 1");
        Check(cmp.only_left.size() == 1 && cmp.only_left[0].name == L"only_l.txt", "CompareFolderPaths only_left");
        Check(cmp.only_right.size() == 1 && cmp.only_right[0].name == L"only_r.txt", "CompareFolderPaths only_right");
        Check(cmp.newer_left.size() == 1 && cmp.newer_left[0].name == L"newer_l.txt", "CompareFolderPaths newer_left");
        Check(cmp.newer_right.size() == 1 && cmp.newer_right[0].name == L"newer_r.txt",
              "CompareFolderPaths newer_right");
        Check(cmp.different_size.size() == 1 && cmp.different_size[0].name == L"diff_sz.txt",
              "CompareFolderPaths different_size");
    }

    fs::remove_all(root, ec);
    std::printf("[SUMMARY] ai_archive_compare_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
