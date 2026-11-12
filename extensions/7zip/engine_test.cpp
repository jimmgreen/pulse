// engine_test.cpp - round trips through the real 7z.dll in an isolated temp folder.
#include "engine.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void Write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
fs::path Fresh(const fs::path& root, const wchar_t* name) {
    auto path = root / name;
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}
engine::AskPassword Answers(std::vector<std::wstring> answers, int* asked = nullptr) {
    auto shared = std::make_shared<std::vector<std::wstring>>(std::move(answers));
    return [shared, asked](bool, std::wstring& password) {
        if (asked) ++*asked;
        if (shared->empty()) return false;
        password = shared->front();
        shared->erase(shared->begin());
        return true;
    };
}
}

static int Run();
// --fixture DIR: sample archives for trying the extension in Pulse by hand.
static int Fixture(const fs::path& dir) {
    const engine::Cancelled never = [] { return false; };
    const auto src = dir / L".fixture-source";
    fs::remove_all(src);
    Write(src / L"报告" / L"第一章.txt", "第一章\n");
    Write(src / L"报告" / L"图片" / L"a.bin", std::string(3000, 'x'));
    Write(src / L"说明.txt", "说明\n");
    Write(src / L"清单.csv", "a,b\n");
    engine::CreateOptions encrypted;
    encrypted.password = L"pulse";
    encrypted.encrypt_names = true;
    engine::Create({(src / L"报告").wstring()}, dir.wstring(), L"加密-密码pulse.7z", encrypted, never, {});
    engine::CreateOptions zip;
    zip.format = engine::Format::Zip;
    engine::Create({(src / L"说明.txt").wstring(), (src / L"清单.csv").wstring(), (src / L"报告").wstring()}, dir.wstring(),
                   L"多个顶层项.zip", zip, never, {});
    engine::CreateOptions tgz;
    tgz.format = engine::Format::TarGz;
    engine::Create({(src / L"报告").wstring()}, dir.wstring(), L"单个文件夹.tar.gz", tgz, never, {});
    fs::remove_all(src);
    std::printf("fixture written\n");
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture") return Fixture(argv[2]);
    try { return Run(); } catch (const std::exception& e) { std::printf("exception: %s\n", e.what()); return 2; }
}
static int Run() {
    // Beside the executable: isolated from user files, and writable in sandboxed runs.
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const fs::path root = fs::path(module).parent_path() / L"engine-test-data";
    SetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", (root / L"tmp").c_str());
    fs::create_directories(root / L"tmp");
    fs::remove_all(root);
    fs::create_directories(root / L"tmp");
    const auto source = root / L"源文件";
    Write(source / L"文档.txt", std::string(200000, 'a') + "end");
    Write(source / L"子目录" / L"数据.bin", std::string(5000, '\x7f'));
    Write(root / L"single.txt", "single file\n");
    const engine::Cancelled never = [] { return false; };
    const engine::Report quiet = [](const engine::Progress&) {};

    Check(!engine::SupportedFormats().empty(), "7z.dll lists its formats");
    Check(engine::ArchiveExtensions().size() <= 32, "context menu extension list fits the SDK limit");

    // 7z with encrypted names.
    {
        auto out = Fresh(root, L"out7z");
        engine::CreateOptions options;
        options.password = L"秘密123";
        options.encrypt_names = true;
        const auto created = engine::Create({source.wstring()}, out.wstring(), L"测试.7z", options, never, quiet);
        Check(created.status == engine::Status::Ok && created.outputs.size() == 1 && created.files == 2, "7z with password is created");
        const auto archive = out / L"测试.7z";
        int asked = 0;
        const auto refused = engine::List(archive.wstring(), never, Answers({}, &asked));
        Check(refused.status == engine::Status::NeedPassword && asked == 1, "encrypted names ask for a password to list");
        const auto wrong = engine::List(archive.wstring(), never, Answers({L"nope"}));
        Check(wrong.status == engine::Status::WrongPassword, "a wrong header password is reported as wrong");
        const auto listed = engine::List(archive.wstring(), never, Answers({L"秘密123"}));
        Check(listed.status == engine::Status::Ok && listed.entries.size() == 4 && listed.format == L"7z", "the right password lists 4 entries");
        auto dest = Fresh(root, L"x7z");
        const auto extracted = engine::Extract(archive.wstring(), dest.wstring(), false, never, quiet, Answers({L"秘密123"}));
        Check(extracted.status == engine::Status::Ok && extracted.files == 2, "7z extracts with the password");
        Check(Read(dest / L"源文件" / L"文档.txt").size() == 200003, "extracted content matches");
        const auto tested = engine::Extract(archive.wstring(), {}, true, never, quiet, Answers({L"秘密123"}));
        Check(tested.status == engine::Status::Ok, "test mode passes");
    }
    // zip AES with names visible: wrong password detected during extraction.
    {
        auto out = Fresh(root, L"outzip");
        engine::CreateOptions options;
        options.format = engine::Format::Zip;
        options.password = L"pw";
        const auto created = engine::Create({source.wstring(), (root / L"single.txt").wstring()}, out.wstring(), L"a.zip", options, never, quiet);
        Check(created.status == engine::Status::Ok, "zip AES-256 is created");
        const auto listed = engine::List((out / L"a.zip").wstring(), never, Answers({}));
        Check(listed.status == engine::Status::Ok && listed.entries.size() >= 4, "zip names list without a password");
        auto dest = Fresh(root, L"xzipwrong");
        const auto wrong = engine::Extract((out / L"a.zip").wstring(), dest.wstring(), false, never, quiet, Answers({L"bad"}));
        Check(wrong.status == engine::Status::WrongPassword, "a wrong zip password is reported as wrong");
        dest = Fresh(root, L"xzip");
        const auto right = engine::Extract((out / L"a.zip").wstring(), dest.wstring(), false, never, quiet, Answers({L"pw"}));
        Check(right.status == engine::Status::Ok && Read(dest / L"single.txt") == "single file\n", "zip extracts with the password");
    }
    // tar.gz opens through to the tar.
    {
        auto out = Fresh(root, L"outtgz");
        engine::CreateOptions options;
        options.format = engine::Format::TarGz;
        const auto created = engine::Create({source.wstring()}, out.wstring(), L"src.tar.gz", options, never, quiet);
        std::printf("  tar.gz: status=%d error=%s outputs=%zu\n", static_cast<int>(created.status), created.error.c_str(), created.outputs.size());
        Check(created.status == engine::Status::Ok && fs::exists(out / L"src.tar.gz") && fs::directory_iterator(out) != fs::directory_iterator(),
              "tar.gz is created without leaving the tar behind");
        Check(std::distance(fs::directory_iterator(out), fs::directory_iterator()) == 1, "only the .tar.gz is in the output folder");
        const auto listed = engine::List((out / L"src.tar.gz").wstring(), never, {});
        Check(listed.status == engine::Status::Ok && listed.nested && listed.format == L"tar", "tar.gz lists the inner tar");
        auto dest = Fresh(root, L"xtgz");
        const auto extracted = engine::Extract((out / L"src.tar.gz").wstring(), dest.wstring(), false, never, quiet, {});
        Check(extracted.status == engine::Status::Ok && fs::exists(dest / L"源文件" / L"子目录" / L"数据.bin"), "tar.gz extracts its files directly");
        // Security software can refuse %TEMP%; the inner tar then goes next to the archive.
        SetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", (root / L"missing" / L"tmp").c_str());
        auto fallback = Fresh(root, L"xtgz-fallback");
        const auto refused = engine::Extract((out / L"src.tar.gz").wstring(), fallback.wstring(), false, never, quiet, {});
        SetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", (root / L"tmp").c_str());
        Check(refused.status == engine::Status::Ok && fs::exists(fallback / L"源文件" / L"子目录" / L"数据.bin"),
              "tar.gz extracts when the temporary folder is refused");
        Check(std::distance(fs::directory_iterator(out), fs::directory_iterator()) == 1,
              "the fallback temporary folder is removed");
    }
    // Split volumes.
    {
        auto out = Fresh(root, L"outvol");
        engine::CreateOptions options;
        options.level = 0;
        options.volume_size = 64 * 1024;
        const auto created = engine::Create({source.wstring()}, out.wstring(), L"v.7z", options, never, quiet);
        Check(created.status == engine::Status::Ok && created.outputs.size() >= 3, "store-level 7z splits into volumes");
        auto dest = Fresh(root, L"xvol");
        const auto extracted = engine::Extract((out / L"v.7z.001").wstring(), dest.wstring(), false, never, quiet, {});
        std::printf("  volumes: status=%d error=%s\n", static_cast<int>(extracted.status), extracted.error.c_str());
        Check(extracted.status == engine::Status::Ok && Read(dest / L"源文件" / L"文档.txt").size() == 200003, "volumes extract from .001");
    }
    // Damage, safety and hashes.
    {
        auto out = Fresh(root, L"outbad");
        Write(out / L"bad.zip", "PK\x03\x04 not really a zip");
        auto dest = Fresh(root, L"xbad");
        const auto bad = engine::Extract((out / L"bad.zip").wstring(), dest.wstring(), false, never, quiet, {});
        Check(bad.status != engine::Status::Ok, "a damaged archive fails cleanly");
        Check(!engine::SafeRelativePath(L"..\\evil") && !engine::SafeRelativePath(L"C:\\x") && !engine::SafeRelativePath(L"a\\con.txt") &&
              engine::SafeRelativePath(L"目录\\文件.txt"), "unsafe archive paths are rejected");
        const auto hashes = engine::Hash({(root / L"single.txt").wstring()}, never, quiet);
        Check(hashes.size() == 1 && hashes[0].crc32 == "441c4824" &&
              hashes[0].sha256 == "f23f43097568948e84d241ad1fa1c556ac2df1967a85195df1b2305cbae0c4da", "CRC32 and SHA-256 are computed");
        std::printf("  crc32=%s sha256=%s\n", hashes.empty() ? "" : hashes[0].crc32.c_str(), hashes.empty() ? "" : hashes[0].sha256.c_str());
    }
    // Cancellation.
    {
        auto out = Fresh(root, L"outcancel");
        const auto cancelled = engine::Create({source.wstring()}, out.wstring(), L"c.7z", {}, [] { return true; }, quiet);
        Check(cancelled.status == engine::Status::Cancelled && fs::is_empty(out), "cancelled creation leaves nothing behind");
    }
    // Streaming budgets: high-expansion nested preview, cross-layer totals, entry/path caps, and disk reserve.
    {
        auto out = Fresh(root, L"outbudget");
        const auto bomb_src = Fresh(root, L"bomb-src");
        Write(bomb_src / L"zeros.bin", std::string(34u << 20, '\0'));
        engine::CreateOptions tgz;
        tgz.format = engine::Format::TarGz;
        tgz.level = 1;
        const auto bomb_created = engine::Create({(bomb_src / L"zeros.bin").wstring()}, out.wstring(), L"bomb.tar.gz", tgz, never, quiet);
        fs::remove_all(bomb_src);
        Check(bomb_created.status == engine::Status::Ok && fs::file_size(out / L"bomb.tar.gz") < (1u << 20),
              "high-expansion tar.gz fixture is compact on disk");

        Fresh(root, L"tmp");
        auto preview_budget = engine::PreviewBudget();
        preview_budget.min_free_disk_bytes = 0;
        const auto bomb_list = engine::List((out / L"bomb.tar.gz").wstring(), never, {}, 4096, preview_budget);
        Check(bomb_list.status == engine::Status::LimitReached,
              "preview List stops high-expansion tar.gz at PreviewBudget.max_total_bytes");
        Check(fs::is_empty(root / L"tmp") && std::distance(fs::directory_iterator(out), fs::directory_iterator()) == 1,
              "aborted nested preview leaves no .PulseZip-* or inner tar behind");

        auto no_wrap = preview_budget;
        no_wrap.expand_wrappers = false;
        const auto outer_only = engine::List((out / L"bomb.tar.gz").wstring(), never, {}, 4096, no_wrap);
        Check(outer_only.status == engine::Status::Ok && !outer_only.nested && outer_only.entries.size() == 1,
              "expand_wrappers=false lists outer stream without extracting inner tar");

        engine::CreateOptions plain_zip;
        plain_zip.format = engine::Format::Zip;
        const auto plain_created = engine::Create({source.wstring(), (root / L"single.txt").wstring()}, out.wstring(), L"plain.zip", plain_zip, never, quiet);
        Check(plain_created.status == engine::Status::Ok, "unencrypted zip fixture is created for budget checks");
        const auto zip_path = (out / L"plain.zip").wstring();
        const auto tgz_path = (root / L"outtgz" / L"src.tar.gz").wstring();

        auto dest = Fresh(root, L"xbudget-total");
        auto small_total = engine::DefaultExtractBudget();
        small_total.min_free_disk_bytes = 0;
        small_total.max_total_bytes = 64 * 1024;
        const auto total_hit = engine::Extract(zip_path, dest.wstring(), false, never, quiet, {}, small_total);
        Check(total_hit.status == engine::Status::LimitReached && fs::is_empty(dest),
              "Extract stops at small max_total_bytes and cleans partial output");

        // src.tar.gz unpacks ~210 KiB inner tar + ~205 KiB final files (> 300 KiB across layers, < 300 KiB per layer).
        dest = Fresh(root, L"xbudget-cross");
        Fresh(root, L"tmp");
        auto cross_layer = engine::DefaultExtractBudget();
        cross_layer.min_free_disk_bytes = 0;
        cross_layer.max_total_bytes = 300 * 1024;
        const auto cross_hit = engine::Extract(tgz_path, dest.wstring(), false, never, quiet, {}, cross_layer);
        Check(cross_hit.status == engine::Status::LimitReached && fs::is_empty(dest) && fs::is_empty(root / L"tmp"),
              "Extract shares max_total_bytes across outer wrapper temp and final files");

        dest = Fresh(root, L"xbudget-single");
        auto small_single = engine::DefaultExtractBudget();
        small_single.min_free_disk_bytes = 0;
        small_single.max_single_bytes = 64 * 1024;
        const auto single_hit = engine::Extract(zip_path, dest.wstring(), false, never, quiet, {}, small_single);
        Check(single_hit.status == engine::Status::LimitReached && fs::is_empty(dest),
              "Extract enforces max_single_bytes on individual files");

        dest = Fresh(root, L"xbudget-entries");
        auto small_entries = engine::DefaultExtractBudget();
        small_entries.min_free_disk_bytes = 0;
        small_entries.max_entries = 2;
        const auto entries_hit = engine::Extract(zip_path, dest.wstring(), false, never, quiet, {}, small_entries);
        const auto list_entries_hit = engine::List(zip_path, never, {}, SIZE_MAX, small_entries);
        const auto list_truncated = engine::List(zip_path, never, {}, 2, small_entries);
        Check(entries_hit.status == engine::Status::LimitReached && fs::is_empty(dest) &&
              list_entries_hit.status == engine::Status::LimitReached &&
              list_truncated.status == engine::Status::Ok && list_truncated.truncated && list_truncated.entries.size() == 2,
              "max_entries limits untrusted item counts while preserving explicit List truncation");

        dest = Fresh(root, L"xbudget-paths");
        auto small_paths = engine::DefaultExtractBudget();
        small_paths.min_free_disk_bytes = 0;
        small_paths.max_path_bytes = 8;
        const auto paths_hit = engine::Extract(zip_path, dest.wstring(), false, never, quiet, {}, small_paths);
        Check(paths_hit.status == engine::Status::LimitReached && fs::is_empty(dest),
              "Extract enforces cumulative max_path_bytes budget");

        dest = Fresh(root, L"xbudget-disk");
        Fresh(root, L"tmp");
        auto low_disk = engine::DefaultExtractBudget();
        low_disk.min_free_disk_bytes = UINT64_MAX - 1;
        const auto disk_extract = engine::Extract(zip_path, dest.wstring(), false, never, quiet, {}, low_disk);
        const auto disk_list = engine::List(tgz_path, never, {}, 4096, low_disk);
        Check(disk_extract.status == engine::Status::LimitReached && fs::is_empty(dest) &&
              disk_list.status == engine::Status::LimitReached && fs::is_empty(root / L"tmp"),
              "min_free_disk_bytes aborts Extract and wrapper List cleanly without touching disk reserve");
    }
    fs::remove_all(root);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
