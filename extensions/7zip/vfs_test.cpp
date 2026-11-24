#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "engine.h"
#include "vfs_mount.h"

namespace fs = std::filesystem;

namespace {
int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void WriteFileBytes(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

struct MockMountRecord {
    std::string mount_id;
    std::string display_name;
    PulseVfsMountDesc desc{};
};

struct MockVfsHost {
    PulseVfsMountHandle next_handle = 1;
    std::map<PulseVfsMountHandle, MockMountRecord> mounts;
    int register_calls = 0;
    int unregister_calls = 0;
    int invalidate_calls = 0;
    int open_calls = 0;
    uint32_t last_open_mode = 0;
    PulseVfsMountHandle last_open_handle = 0;
    std::string last_open_rel_path;

    static PulseStatus PULSE_CALL RegisterMount(void* context, const PulseVfsMountDesc* desc,
                                                PulseVfsMountHandle* out_handle) noexcept {
        if (!context || !desc || !out_handle) return PULSE_ERROR_ARGUMENT;
        auto& self = *static_cast<MockVfsHost*>(context);
        ++self.register_calls;
        const PulseVfsMountHandle h = self.next_handle++;
        MockMountRecord rec;
        rec.mount_id.assign(desc->mount_id.data ? desc->mount_id.data : "", desc->mount_id.length);
        rec.display_name.assign(desc->display_name.data ? desc->display_name.data : "", desc->display_name.length);
        rec.desc = *desc;
        self.mounts.emplace(h, std::move(rec));
        *out_handle = h;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UnregisterMount(void* context, PulseVfsMountHandle handle) noexcept {
        if (!context || handle == 0) return PULSE_ERROR_ARGUMENT;
        auto& self = *static_cast<MockVfsHost*>(context);
        const auto it = self.mounts.find(handle);
        if (it == self.mounts.end()) return PULSE_ERROR_ARGUMENT;
        self.mounts.erase(it);
        ++self.unregister_calls;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL InvalidatePath(void* context, PulseVfsMountHandle handle,
                                                 PulseStringView) noexcept {
        if (!context || handle == 0) return PULSE_ERROR_ARGUMENT;
        auto& self = *static_cast<MockVfsHost*>(context);
        if (self.mounts.find(handle) == self.mounts.end()) return PULSE_ERROR_ARGUMENT;
        ++self.invalidate_calls;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL OpenPath(void* context, PulseVfsMountHandle handle,
                                           PulseStringView rel_path, uint32_t mode) noexcept {
        if (!context || handle == 0) return PULSE_ERROR_ARGUMENT;
        auto& self = *static_cast<MockVfsHost*>(context);
        if (self.mounts.find(handle) == self.mounts.end()) return PULSE_ERROR_ARGUMENT;
        ++self.open_calls;
        self.last_open_mode = mode;
        self.last_open_handle = handle;
        self.last_open_rel_path.assign(rel_path.data ? rel_path.data : "", rel_path.length);
        return PULSE_OK;
    }

    PulseVfsV1 Table() noexcept {
        PulseVfsV1 t{};
        t.size = sizeof(t);
        t.version = PULSE_VFS_VERSION_1;
        t.context = this;
        t.register_mount = RegisterMount;
        t.unregister_mount = UnregisterMount;
        t.invalidate_path = InvalidatePath;
        t.open_path = OpenPath;
        return t;
    }
};

struct CollectedEntry {
    std::string name;
    uint32_t flags = 0;
    uint64_t size_bytes = 0;
};

struct CollectCtx {
    std::vector<CollectedEntry> items;
    static uint32_t PULSE_CALL Emit(void* ctx, const PulseVfsEntry* entry) noexcept {
        if (!ctx || !entry) return 0u;
        auto& self = *static_cast<CollectCtx*>(ctx);
        CollectedEntry item;
        item.name.assign(entry->name.data ? entry->name.data : "", entry->name.length);
        item.flags = entry->flags;
        item.size_bytes = entry->size_bytes;
        self.items.push_back(std::move(item));
        return 1u;
    }
};

struct CancelFlag {
    std::atomic<bool> flag{false};
    PulseVfsCancelToken Token() noexcept {
        return {this, [](void* ctx) noexcept -> uint32_t {
            return static_cast<CancelFlag*>(ctx)->flag.load(std::memory_order_acquire) ? 1u : 0u;
        }};
    }
};
} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    const fs::path root = fs::path(module_path).parent_path() / L"vfs-test-data";
    fs::remove_all(root);
    fs::create_directories(root / L"tmp");
    SetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", (root / L"tmp").c_str());

    const engine::Cancelled never = [] { return false; };
    const engine::Report quiet;

    // 1. Build test zip archive with nested folders, Unicode name, and empty file.
    const fs::path src = root / L"src";
    const std::string big_text = std::string(120000, 'z') + "-tail-marker";
    const std::string bin_data = std::string("\x00\x01\x02\x7f\xfe\xff", 6);
    WriteFileBytes(src / L"文档" / L"说明.txt", big_text);
    WriteFileBytes(src / L"sub" / L"deep" / L"data.bin", bin_data);
    WriteFileBytes(root / L"empty.bin", "");

    const fs::path out_dir = root / L"archives";
    fs::create_directories(out_dir);
    engine::CreateOptions zip_opts;
    zip_opts.format = engine::Format::Zip;
    const auto created = engine::Create(
        {(src / L"文档").wstring(), (src / L"sub").wstring(), (root / L"empty.bin").wstring()},
        out_dir.wstring(), L"sample.zip", zip_opts, never, quiet);
    Check(created.status == engine::Status::Ok && fs::exists(out_dir / L"sample.zip"),
          "sample.zip created through out-of-process worker");

    const fs::path archive = out_dir / L"sample.zip";

    // 2. Worker IPC ExtractEntry checks.
    const fs::path ext_dir = root / L"extracted";
    fs::create_directories(ext_dir);

    const auto r1 = engine::ExtractEntry(
        archive.wstring(), L"文档/说明.txt", (ext_dir / L"readme.txt").wstring(),
        never, quiet, {});
    Check(r1.status == engine::Status::Ok && r1.files == 1 && r1.bytes == big_text.size() &&
          ReadFileBytes(ext_dir / L"readme.txt") == big_text,
          "worker ExtractEntry decodes nested Unicode file with forward slashes");

    const auto r2 = engine::ExtractEntry(
        archive.wstring(), L"SUB\\DEEP\\DATA.BIN", (ext_dir / L"data.bin").wstring(),
        never, quiet, {});
    Check(r2.status == engine::Status::Ok && r2.files == 1 && r2.bytes == bin_data.size() &&
          ReadFileBytes(ext_dir / L"data.bin") == bin_data,
          "worker ExtractEntry matches case-insensitively across nested paths");

    const auto r3 = engine::ExtractEntry(
        archive.wstring(), L"empty.bin", (ext_dir / L"empty.bin").wstring(),
        never, quiet, {});
    Check(r3.status == engine::Status::Ok && fs::exists(ext_dir / L"empty.bin") &&
          fs::file_size(ext_dir / L"empty.bin") == 0,
          "worker ExtractEntry extracts 0-byte file");

    WriteFileBytes(ext_dir / L"existing.txt", "keep-me");
    const auto r_exists = engine::ExtractEntry(
        archive.wstring(), L"empty.bin", (ext_dir / L"existing.txt").wstring(),
        never, quiet, {});
    Check(r_exists.status == engine::Status::InvalidInput &&
          ReadFileBytes(ext_dir / L"existing.txt") == "keep-me",
          "worker ExtractEntry refuses to overwrite existing destination file");

    const auto r_missing = engine::ExtractEntry(
        archive.wstring(), L"missing/file.txt", (ext_dir / L"missing.txt").wstring(),
        never, quiet, {});
    Check(r_missing.status == engine::Status::InvalidInput && !fs::exists(ext_dir / L"missing.txt"),
          "worker ExtractEntry rejects missing archive entry");

    const auto r_dir = engine::ExtractEntry(
        archive.wstring(), L"sub", (ext_dir / L"dir.bin").wstring(),
        never, quiet, {});
    Check(r_dir.status == engine::Status::InvalidInput && !fs::exists(ext_dir / L"dir.bin"),
          "worker ExtractEntry rejects directory entry");

    const auto r_unsafe = engine::ExtractEntry(
        archive.wstring(), L"../evil.txt", (ext_dir / L"evil.txt").wstring(),
        never, quiet, {});
    Check(r_unsafe.status == engine::Status::InvalidInput && !fs::exists(ext_dir / L"evil.txt"),
          "worker ExtractEntry rejects path traversal");

    const auto r_cancel = engine::ExtractEntry(
        archive.wstring(), L"empty.bin", (ext_dir / L"cancelled.bin").wstring(),
        [] { return true; }, quiet, {});
    Check(r_cancel.status == engine::Status::Cancelled && !fs::exists(ext_dir / L"cancelled.bin"),
          "worker ExtractEntry honors cancellation");

    // 3. Encrypted 7z single-entry extraction check.
    engine::CreateOptions enc_opts;
    enc_opts.format = engine::Format::SevenZip;
    enc_opts.password = L"s3cret";
    enc_opts.encrypt_names = true;
    const auto enc_created = engine::Create(
        {(src / L"sub" / L"deep" / L"data.bin").wstring()},
        out_dir.wstring(), L"secret.7z", enc_opts, never, quiet);
    Check(enc_created.status == engine::Status::Ok, "encrypted 7z created");
    const auto r_bad_pw = engine::ExtractEntry(
        (out_dir / L"secret.7z").wstring(), L"data.bin", (ext_dir / L"secret_bad.bin").wstring(),
        never, quiet, [](bool retry, std::wstring& pw) {
            if (retry) return false;
            pw = L"wrong";
            return true;
        });
    Check((r_bad_pw.status == engine::Status::WrongPassword || r_bad_pw.status == engine::Status::NeedPassword) &&
          !fs::exists(ext_dir / L"secret_bad.bin"),
          "worker ExtractEntry rejects wrong password without leaving destination");
    const auto r_ok_pw = engine::ExtractEntry(
        (out_dir / L"secret.7z").wstring(), L"data.bin", (ext_dir / L"secret_ok.bin").wstring(),
        never, quiet, [](bool, std::wstring& pw) {
            pw = L"s3cret";
            return true;
        });
    Check(r_ok_pw.status == engine::Status::Ok && ReadFileBytes(ext_dir / L"secret_ok.bin") == bin_data,
          "worker ExtractEntry extracts encrypted 7z item with correct password");

    // 4. ArchiveVfsManager round-trip against mock PulseVfsV1.
    MockVfsHost host;
    ArchiveVfsManager manager;
    Check(manager.Initialize(host.Table()) && manager.Available(),
          "ArchiveVfsManager initializes with PulseVfsV1");

    Check(manager.OpenArchive(archive.wstring(), false) == PULSE_OK &&
          manager.MountCount() == 1 && host.register_calls == 1 &&
          host.open_calls == 1 && host.last_open_mode == PULSE_VFS_OPEN_CURRENT_TAB,
          "OpenArchive mounts and opens archive in current tab");

    std::wstring upper_archive = archive.wstring();
    std::transform(upper_archive.begin(), upper_archive.end(), upper_archive.begin(), towupper);
    Check(manager.OpenArchive(upper_archive, true) == PULSE_OK &&
          manager.MountCount() == 1 && host.register_calls == 1 &&
          host.open_calls == 2 && host.last_open_mode == PULSE_VFS_OPEN_NEW_TAB,
          "OpenArchive deduplicates case-variant path and focuses in new tab");

    const auto& mount = host.mounts.begin()->second.desc;
    CancelFlag cancel_flag;
    const PulseVfsCancelToken token = cancel_flag.Token();

    CollectCtx root_list;
    const PulseStatus list_root_st = mount.list_dir(
        mount.user, mount.mount_id, {"", 0}, CollectCtx::Emit, &root_list, &token);
    Check(list_root_st == PULSE_OK && root_list.items.size() == 3,
          "VFS list_dir enumerates archive root entries");

    CollectCtx deep_list;
    const PulseStatus list_deep_st = mount.list_dir(
        mount.user, mount.mount_id, {"sub/deep", 8}, CollectCtx::Emit, &deep_list, &token);
    Check(list_deep_st == PULSE_OK && deep_list.items.size() == 1 &&
          deep_list.items[0].name == "data.bin" &&
          deep_list.items[0].size_bytes == bin_data.size() &&
          (deep_list.items[0].flags & PULSE_VFS_ENTRY_DIRECTORY) == 0u,
          "VFS list_dir resolves nested folder case-insensitively");

    CollectCtx stat_root;
    Check(mount.stat(mount.user, mount.mount_id, {"", 0}, CollectCtx::Emit, &stat_root, &token) == PULSE_OK &&
          stat_root.items.size() == 1 &&
          (stat_root.items[0].flags & PULSE_VFS_ENTRY_DIRECTORY) != 0u,
          "VFS stat returns root directory entry");

    CollectCtx stat_file;
    Check(mount.stat(mount.user, mount.mount_id, {"sub/deep/data.bin", 17}, CollectCtx::Emit, &stat_file, &token) == PULSE_OK &&
          stat_file.items.size() == 1 && stat_file.items[0].size_bytes == bin_data.size(),
          "VFS stat returns nested file size");

    CollectCtx stat_missing;
    Check(mount.stat(mount.user, mount.mount_id, {"nope.txt", 8}, CollectCtx::Emit, &stat_missing, &token) == PULSE_ERROR_ARGUMENT,
          "VFS stat returns error on missing path");

    // extract_file: fresh path AND pre-existing 0-byte placeholder.
    const fs::path vfs_out1 = root / L"vfs_out" / L"fresh.bin";
    const std::string vfs_out1_utf8 = vfs_out1.u8string().empty()
        ? std::string{} : std::string(reinterpret_cast<const char*>(vfs_out1.u8string().data()), vfs_out1.u8string().size());
    Check(mount.extract_file(mount.user, mount.mount_id, {"sub/deep/data.bin", 17},
                             {vfs_out1_utf8.data(), static_cast<uint32_t>(vfs_out1_utf8.size())},
                             &token) == PULSE_OK &&
          ReadFileBytes(vfs_out1) == bin_data,
          "VFS extract_file materializes nested file to new path");

    const fs::path vfs_out2 = root / L"vfs_out" / L"placeholder.bin";
    WriteFileBytes(vfs_out2, "");
    const std::string vfs_out2_utf8(reinterpret_cast<const char*>(vfs_out2.u8string().data()), vfs_out2.u8string().size());
    Check(mount.extract_file(mount.user, mount.mount_id, {"sub/deep/data.bin", 17},
                             {vfs_out2_utf8.data(), static_cast<uint32_t>(vfs_out2_utf8.size())},
                             &token) == PULSE_OK &&
          ReadFileBytes(vfs_out2) == bin_data,
          "VFS extract_file atomically replaces pre-created placeholder temp file");

    // LRU cap test (9 additional archives => total 10 opened => max 8 active mounts).
    for (int i = 0; i < 9; ++i) {
        const fs::path arc_i = out_dir / (L"extra_" + std::to_wstring(i) + L".zip");
        std::error_code ec;
        fs::copy_file(archive, arc_i, fs::copy_options::overwrite_existing, ec);
        Check(manager.OpenArchive(arc_i.wstring(), false) == PULSE_OK, "LRU extra archive opens");
    }
    Check(manager.MountCount() == 8 && host.register_calls == 10 && host.unregister_calls == 2,
          "ArchiveVfsManager enforces 8-mount LRU cap cleanly");

    manager.Clear();
    Check(manager.MountCount() == 0 && host.unregister_calls == 10 && host.mounts.empty(),
          "ArchiveVfsManager::Clear unregisters all mounts cleanly");

    // 5. Write support: extract_items, modify, one-step undo; read-only formats stay read-only.
    {
        MockVfsHost host2;
        ArchiveVfsManager writer;
        Check(writer.Initialize(host2.Table()), "second manager initializes");
        const fs::path editable = out_dir / L"editable.zip";
        std::error_code copy_ec;
        fs::copy_file(archive, editable, fs::copy_options::overwrite_existing, copy_ec);
        engine::CreateOptions tgz_opts;
        tgz_opts.format = engine::Format::TarGz;
        const auto tgz_created = engine::Create({(src / L"sub").wstring()}, out_dir.wstring(), L"ro.tar.gz", tgz_opts, never, quiet);
        Check(writer.OpenArchive(editable.wstring(), false) == PULSE_OK && tgz_created.status == engine::Status::Ok &&
              writer.OpenArchive((out_dir / L"ro.tar.gz").wstring(), false) == PULSE_OK && host2.mounts.size() == 2,
              "writable and read-only archives mount");
        const PulseVfsMountDesc* zip_desc = nullptr;
        const PulseVfsMountDesc* tgz_desc = nullptr;
        for (const auto& [handle, rec] : host2.mounts) {
            (void)handle;
            if (rec.display_name.find("editable") != std::string::npos) zip_desc = &rec.desc;
            else tgz_desc = &rec.desc;
        }
        Check(zip_desc && tgz_desc && zip_desc->size >= PULSE_VFS_MOUNT_DESC_V2_SIZE &&
              (zip_desc->capabilities & PULSE_VFS_CAP_MODIFY) && !(zip_desc->capabilities & PULSE_VFS_CAP_READONLY) &&
              zip_desc->modify && zip_desc->extract_items &&
              (tgz_desc->capabilities & PULSE_VFS_CAP_READONLY) && !(tgz_desc->capabilities & PULSE_VFS_CAP_MODIFY) &&
              !tgz_desc->modify && (tgz_desc->capabilities & PULSE_VFS_CAP_EXTRACT_ITEMS),
              "capabilities: zip is modifiable, tar.gz is read-only, both extract items");
        if (zip_desc && tgz_desc) {
            const auto& m = *zip_desc;
            struct OpState {
                uint64_t progress_calls = 0;
                std::string message;
                static void PULSE_CALL Progress(void* c, const PulseVfsProgress*) { ++static_cast<OpState*>(c)->progress_calls; }
                static void PULSE_CALL Message(void* c, PulseStringView t) {
                    static_cast<OpState*>(c)->message.assign(t.data ? t.data : "", t.length);
                }
            } state;
            PulseVfsOperation op{sizeof(PulseVfsOperation), &state, &token, &OpState::Progress, &OpState::Message};
            const auto u8 = [](const fs::path& p) {
                const auto s8 = p.u8string();
                return std::string(reinterpret_cast<const char*>(s8.data()), s8.size());
            };
            const auto view = [](const std::string& t) { return PulseStringView{t.data(), static_cast<uint32_t>(t.size())}; };

            const fs::path pick_dir = root / L"picked";
            fs::create_directories(pick_dir);
            const std::string pick_utf8 = u8(pick_dir);
            const std::string doc = "文档", deep = "sub/deep";
            const PulseStringView picks[] = {view(doc), {"empty.bin", 9}};
            Check(m.extract_items(m.user, m.mount_id, picks, 2, view(pick_utf8), &op) == PULSE_OK &&
                  ReadFileBytes(pick_dir / L"文档" / L"说明.txt") == big_text && fs::exists(pick_dir / L"empty.bin") &&
                  !fs::exists(pick_dir / L"sub") && state.progress_calls > 0,
                  "extract_items writes a folder and a file picked at the root, with progress");
            const fs::path pick2 = root / L"picked2";
            fs::create_directories(pick2);
            const std::string pick2_utf8 = u8(pick2);
            const PulseStringView deep_pick[] = {{"sub/deep/data.bin", 17}};
            Check(m.extract_items(m.user, m.mount_id, deep_pick, 1, view(pick2_utf8), &op) == PULSE_OK &&
                  ReadFileBytes(pick2 / L"data.bin") == bin_data && !fs::exists(pick2 / L"sub"),
                  "extract_items rebases a nested pick onto its folder");
            const PulseStringView mixed[] = {{"empty.bin", 9}, {"sub/deep/data.bin", 17}};
            Check(m.extract_items(m.user, m.mount_id, mixed, 2, view(pick2_utf8), &op) == PULSE_ERROR_ARGUMENT,
                  "extract_items refuses picks from different folders");

            WriteFileBytes(root / L"new.txt", "fresh text");
            const std::string new_src = u8(root / L"new.txt");
            PulseVfsChange put{sizeof(PulseVfsChange), PULSE_VFS_CHANGE_PUT, view(deep), view(new_src), {}};
            const std::string target = "sub/deep/new.txt";
            put.rel_path = view(target);
            Check(m.modify(m.user, m.mount_id, &put, 1, &op) == PULSE_OK, "modify PUT adds a file");
            CollectCtx after_put;
            m.list_dir(m.user, m.mount_id, view(deep), CollectCtx::Emit, &after_put, &token);
            Check(after_put.items.size() == 2 && (after_put.items[0].flags & PULSE_VFS_ENTRY_READONLY) == 0u,
                  "the listing shows the added file (writable entries)");
            const std::string renamed = "sub/deep/renamed.bin", data_rel = "sub/deep/data.bin";
            PulseVfsChange batch[2] = {
                {sizeof(PulseVfsChange), PULSE_VFS_CHANGE_RENAME, view(data_rel), {}, view(renamed)},
                {sizeof(PulseVfsChange), PULSE_VFS_CHANGE_DELETE, view(doc), {}, {}}};
            Check(m.modify(m.user, m.mount_id, batch, 2, &op) == PULSE_OK, "modify applies rename + delete together");
            CollectCtx root_after;
            m.list_dir(m.user, m.mount_id, {"", 0}, CollectCtx::Emit, &root_after, &token);
            CollectCtx deep_after;
            m.list_dir(m.user, m.mount_id, view(deep), CollectCtx::Emit, &deep_after, &token);
            bool has_renamed = false;
            for (const auto& e : deep_after.items) has_renamed = has_renamed || e.name == "renamed.bin";
            Check(root_after.items.size() == 2 && has_renamed && deep_after.items.size() == 2,
                  "listing reflects the rename and the deleted folder");
            PulseVfsChange restore{sizeof(PulseVfsChange), PULSE_VFS_CHANGE_RESTORE, {}, {}, {}};
            CollectCtx root_undo;
            Check(m.modify(m.user, m.mount_id, &restore, 1, &op) == PULSE_OK &&
                  m.list_dir(m.user, m.mount_id, {"", 0}, CollectCtx::Emit, &root_undo, &token) == PULSE_OK &&
                  root_undo.items.size() == 3, "RESTORE undoes the last change");
            state.message.clear();
            Check(m.modify(m.user, m.mount_id, &restore, 1, &op) == PULSE_ERROR_STATE && !state.message.empty(),
                  "only one step of undo is kept");
            const std::string missing = "nope.txt", missing_to = "x.txt";
            PulseVfsChange bad{sizeof(PulseVfsChange), PULSE_VFS_CHANGE_RENAME, view(missing), {}, view(missing_to)};
            state.message.clear();
            Check(m.modify(m.user, m.mount_id, &bad, 1, &op) == PULSE_ERROR_ARGUMENT && !state.message.empty(),
                  "modify reports a reason when it refuses");
            bool stray = false;
            for (const auto& item : fs::directory_iterator(out_dir)) {
                const auto name = item.path().filename().wstring();
                stray = stray || name.rfind(L".editable", 0) == 0;
            }
            Check(!stray, "no temporary or backup files are left beside the archive");
        }
        writer.Clear();
    }

    engine::ShutdownWorkers();
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}