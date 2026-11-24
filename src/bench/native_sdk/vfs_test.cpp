#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/native/diagnostics_adapter.h"
#include "../../app/extensions/native/vfs_adapter.h"
#include "../../app/extensions/vfs_path.h"
#include "../../app/extensions/vfs_registry.h"

#include <pulse/client.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace pulse::extensions::native;
using namespace pulse::extensions::vfs;
using pulse::sdk::DiagnosticsClient;
using pulse::sdk::VfsClient;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

std::wstring ExeSiblingPath(std::wstring_view filename) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring(filename);
    buf.resize(slash + 1);
    buf.append(filename);
    return buf;
}

struct MockHostEnv {
    PulseVfsV1 table{};
    bool valid = true;

    static PulseStatus PULSE_CALL QueryInterface(void* context,
                                                 PulseStringView name,
                                                 uint32_t version,
                                                 void* out_table,
                                                 uint32_t out_size) noexcept {
        auto* self = static_cast<MockHostEnv*>(context);
        if (!self || !self->valid || !out_table) return PULSE_ERROR_ARGUMENT;
        if (std::string_view(name.data ? name.data : "", name.length) == PULSE_VFS_INTERFACE_NAME) {
            if (version != PULSE_VFS_VERSION_1) return PULSE_ERROR_ABI;
            if (out_size < sizeof(PulseVfsV1)) return PULSE_ERROR_ARGUMENT;
            std::memcpy(out_table, &self->table, sizeof(PulseVfsV1));
            return PULSE_OK;
        }
        return PULSE_ERROR_UNAVAILABLE;
    }

    PulseHostV1 Host() noexcept {
        return PulseHostV1{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, this, &QueryInterface};
    }
};

} // namespace

int main() {
    // 1. VFS path parsing, normalization, traversal rejection, parent/breadcrumb helpers.
    {
        Check(IsVfsPath(L"pulse:vfs:pulse.7zip/archive-1"), "IsVfsPath recognizes pulse:vfs: scheme");
        Check(!IsVfsPath(L"pulse:device:dev/store"), "IsVfsPath rejects pulse:device:");

        const auto parsed = ParseVfsPath(L"pulse:vfs:pulse.7zip/archive-1/文档\\子目录/测试.txt");
        Check(parsed.has_value() &&
              parsed->package_id == "pulse.7zip" &&
              parsed->mount_id == "archive-1" &&
              parsed->rel_path_utf8 == "文档/子目录/测试.txt" &&
              parsed->segments.size() == 3 &&
              parsed->segments[0] == L"文档" &&
              parsed->segments[2] == L"测试.txt",
              "ParseVfsPath normalizes backslashes and decodes UTF-8 segments");

        const auto child = JoinVfsChild(*parsed, L"next.bin");
        Check(child == L"pulse:vfs:pulse.7zip/archive-1/文档/子目录/测试.txt/next.bin",
              "JoinVfsChild appends child segment");

        const auto p1 = ParentVfsPath(child);
        const auto p_root = ParentVfsPath(L"pulse:vfs:pulse.7zip/archive-1/doc");
        const auto p_top = ParentVfsPath(L"pulse:vfs:pulse.7zip/archive-1");
        Check(p1.has_value() && *p1 == L"pulse:vfs:pulse.7zip/archive-1/文档/子目录/测试.txt" &&
              p_root.has_value() && *p_root == L"pulse:vfs:pulse.7zip/archive-1" &&
              !p_top.has_value(),
              "ParentVfsPath walks up and stops cleanly at mount root");

        Check(IsSameOrDescendantVfsPath(child, "pulse.7zip", "archive-1", "文档/子目录") &&
              IsSameOrDescendantVfsPath(child, "pulse.7zip", "archive-1", "") &&
              !IsSameOrDescendantVfsPath(child, "pulse.7zip", "archive-1", "文档/子"),
              "IsSameOrDescendantVfsPath checks slash boundary accurately");

        Check(!ParseVfsPath(L"pulse:vfs:pulse.7zip/archive-1/../secret").has_value(),
              "ParseVfsPath rejects .. traversal");
        Check(!ParseVfsPath(L"pulse:vfs:pulse.7zip/archive-1/a//b").has_value(),
              "ParseVfsPath rejects empty // segment");
        Check(!ParseVfsPath(L"pulse:vfs:../mount/file").has_value(),
              "ParseVfsPath rejects traversal in package id");
        Check(!ParseVfsPath(L"pulse:vfs:pulse.7zip/bad*mount").has_value(),
              "ParseVfsPath rejects invalid characters in mount id");
        Check(!IsValidVfsEntryName("bad:name") && !IsValidVfsEntryName("..") && IsValidVfsEntryName("readme.md"),
              "IsValidVfsEntryName rejects invalid characters and dot segments");

        std::string deep(4097, 'a');
        std::string norm;
        Check(!NormalizeRelPathUtf8(deep, norm), "NormalizeRelPathUtf8 enforces 4096-byte cap");
    }

    // 2. VfsAdapter validation, DiagnosticsSink, VfsClient provider round-trip, and safe async drain.
    {
        VfsRegistry registry;
        auto sink = std::make_shared<DiagnosticsSink>();
        DiagnosticsAdapter diag(sink, [] { return true; });
        std::atomic<int> wake_count{0};
        std::vector<std::string> invalidations;
        std::vector<std::pair<std::wstring, uint32_t>> opened;

        VfsBackend backend;
        backend.valid_owner = [] { return true; };
        backend.invalidate_path = [&](std::string_view pkg, std::string_view mount, std::string_view rel) {
            invalidations.push_back(std::string(pkg) + "/" + std::string(mount) + "/" + std::string(rel));
        };
        backend.open_path = [&](const std::wstring& uri, uint32_t mode) {
            opened.emplace_back(uri, mode);
            return PULSE_OK;
        };

        VfsAdapter adapter("pulse.7zip", backend, [] { return true; }, sink,
                           [&] { wake_count.fetch_add(1, std::memory_order_relaxed); },
                           &registry);

        MockHostEnv env;
        Check(adapter.Query(PULSE_VFS_VERSION_1, &env.table, sizeof(env.table)) == PULSE_OK,
              "VfsAdapter::Query returns PulseVfsV1");
        Check(adapter.Query(99u, &env.table, sizeof(env.table)) == PULSE_ERROR_ABI,
              "VfsAdapter::Query rejects unsupported version");

        const auto client_opt = VfsClient::TryQuery(pulse::sdk::Host{env.Host()});
        Check(client_opt.has_value() && client_opt->Available(),
              "VfsClient::TryQuery attaches to PulseVfsV1");
        const VfsClient& client = *client_opt;
        Check(VfsClient::FormatUri("pulse.7zip", "arc0", "\\docs\\a.txt") ==
              "pulse:vfs:pulse.7zip/arc0/docs/a.txt",
              "VfsClient::FormatUri formats canonical virtual URI");

        // DiagnosticsSink records invalid mount descriptor.
        {
            pulse::sdk::VfsProviderCallbacks bad_cb;
            bad_cb.capabilities = 0u; // missing PULSE_VFS_CAP_LIST_DIR
            PulseStatus reg_st = PULSE_OK;
            auto bad_handle = client.RegisterProvider("arc0", "Broken", std::move(bad_cb), &reg_st);
            Check(!bad_handle.has_value() && reg_st == PULSE_ERROR_ARGUMENT &&
                  sink->Read().interface_name == PULSE_VFS_INTERFACE_NAME &&
                  sink->Read().field == "capabilities",
                  "Invalid VFS capabilities fails closed and records structured Diagnostic");
        }

        // Cross-thread call from worker thread is rejected safely.
        {
            PulseStatus cross_st = PULSE_OK;
            std::thread worker([&] {
                pulse::sdk::VfsProviderCallbacks cb;
                cb.list_dir = [](std::string_view, const auto&, const auto&) { return PULSE_OK; };
                auto h = client.RegisterProvider("cross", "Cross", std::move(cb), &cross_st);
                (void)h;
            });
            worker.join();
            Check(cross_st == PULSE_ERROR_STATE && sink->Read().field == "thread",
                  "VfsAdapter rejects cross-thread UI calls");
        }

        // Working mount with ListDirectory, Stat, ExtractFile, InvalidatePath, OpenPath.
        pulse::sdk::VfsProviderCallbacks cb;
        cb.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_EXTRACT_FILE |
                          PULSE_VFS_CAP_STAT | PULSE_VFS_CAP_READONLY;
        cb.list_dir = [](std::string_view rel,
                         const std::function<bool(const pulse::sdk::VfsEntryData&)>& emit,
                         const std::function<bool()>&) -> PulseStatus {
            if (rel.empty()) {
                emit({"docs", 0, 1700000000000LL, PULSE_VFS_ENTRY_DIRECTORY | PULSE_VFS_ENTRY_READONLY, "Folder"});
                emit({"readme.txt", 11, 1700000001000LL, PULSE_VFS_ENTRY_READONLY, "Text"});
                emit({"../evil", 10, 0, 0, ""}); // registry must reject invalid entry name!
                return PULSE_OK;
            }
            if (rel == "docs") {
                emit({"notes.txt", 5, 1700000002000LL, 0, "Text"});
                return PULSE_OK;
            }
            return PULSE_ERROR_UNAVAILABLE;
        };
        cb.stat = [](std::string_view rel, pulse::sdk::VfsEntryData& out, const std::function<bool()>&) -> PulseStatus {
            if (rel == "docs/notes.txt") {
                out = {"notes.txt", 5, 1700000002000LL, PULSE_VFS_ENTRY_READONLY, "Text"};
                return PULSE_OK;
            }
            return PULSE_ERROR_UNAVAILABLE;
        };
        cb.extract_file = [](std::string_view rel,
                             std::string_view dest_utf8,
                             const std::function<bool()>&) -> PulseStatus {
            if (rel != "docs/notes.txt") return PULSE_ERROR_UNAVAILABLE;
            std::wstring wdest;
            if (!Utf8ToWideStrict(dest_utf8, wdest)) return PULSE_ERROR_ARGUMENT;
            HANDLE h = CreateFileW(wdest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return PULSE_ERROR_UNAVAILABLE;
            static constexpr char kPayload[] = "hello";
            DWORD written = 0;
            const BOOL ok = WriteFile(h, kPayload, 5, &written, nullptr);
            CloseHandle(h);
            return (ok && written == 5) ? PULSE_OK : PULSE_ERROR_UNAVAILABLE;
        };

        auto mount = client.RegisterProvider("arc0", "Sample Archive", std::move(cb));
        Check(mount.has_value() && mount->Active(), "RegisterProvider mounts arc0");

        // Duplicate mount_id for same package fails with PULSE_ERROR_STATE.
        {
            pulse::sdk::VfsProviderCallbacks dup_cb;
            dup_cb.list_dir = [](std::string_view, const auto&, const auto&) { return PULSE_OK; };
            PulseStatus dup_st = PULSE_OK;
            auto dup = client.RegisterProvider("arc0", "Duplicate", std::move(dup_cb), &dup_st);
            Check(!dup.has_value() && dup_st == PULSE_ERROR_STATE,
                  "Duplicate active mount_id for same package is rejected");
        }

        // v2 descriptor: size versioning, write capabilities, extract_items/modify forwarding.
        {
            const auto noop_list = [](void*, PulseStringView, PulseStringView, PulseVfsEmitEntryFn, void*,
                                      const PulseVfsCancelToken*) noexcept -> PulseStatus { return PULSE_OK; };
            PulseVfsMountDesc old_desc{};
            old_desc.size = PULSE_VFS_MOUNT_DESC_V1_SIZE;  // a plugin built against the v1 header
            old_desc.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_READONLY;
            old_desc.mount_id = {"v1mount", 7};
            old_desc.display_name = {"Old", 3};
            old_desc.list_dir = noop_list;
            pulse::sdk::VfsClient::MountHandle old_handle;
            Check(client.RegisterMount(old_desc, old_handle) == PULSE_OK && old_handle.Active(),
                  "v1-sized PulseVfsMountDesc from older plugins is still accepted");
            old_handle.Reset();
            PulseVfsMountDesc short_desc = old_desc;
            short_desc.size = PULSE_VFS_MOUNT_DESC_V1_SIZE - 8u;
            pulse::sdk::VfsClient::MountHandle short_handle;
            Check(client.RegisterMount(short_desc, short_handle) == PULSE_ERROR_ARGUMENT && sink->Read().field == "size",
                  "descriptor smaller than v1 is rejected");
            PulseVfsMountDesc v1_modify = old_desc;
            v1_modify.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_MODIFY;
            pulse::sdk::VfsClient::MountHandle v1_modify_handle;
            Check(client.RegisterMount(v1_modify, v1_modify_handle) == PULSE_ERROR_ARGUMENT && sink->Read().field == "modify",
                  "PULSE_VFS_CAP_MODIFY on a v1-sized descriptor is rejected (no callback field)");

            pulse::sdk::VfsProviderCallbacks both;
            both.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_MODIFY | PULSE_VFS_CAP_READONLY;
            both.list_dir = [](std::string_view, const auto&, const auto&) { return PULSE_OK; };
            both.modify = [](const std::vector<pulse::sdk::VfsChange>&, const pulse::sdk::VfsOperation&) { return PULSE_OK; };
            PulseStatus both_st = PULSE_OK;
            auto both_handle = client.RegisterProvider("rwboth", "Both", std::move(both), &both_st);
            Check(!both_handle.has_value() && both_st == PULSE_ERROR_ARGUMENT,
                  "MODIFY together with READONLY is rejected");

            struct Seen {
                std::vector<std::string> picks;
                std::string dest;
                std::vector<std::string> changes;
            };
            auto seen = std::make_shared<Seen>();
            pulse::sdk::VfsProviderCallbacks rw;
            rw.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_EXTRACT_ITEMS | PULSE_VFS_CAP_MODIFY;
            rw.list_dir = [](std::string_view, const auto&, const auto&) { return PULSE_OK; };
            rw.extract_items = [seen](const std::vector<std::string_view>& rels, std::string_view dest,
                                      const pulse::sdk::VfsOperation& op) -> PulseStatus {
                for (auto r : rels) seen->picks.emplace_back(r);
                seen->dest = std::string(dest);
                op.Progress(5, 10, 1, 2, "a.txt");
                return op.Cancelled() ? PULSE_ERROR_STATE : PULSE_OK;
            };
            rw.modify = [seen](const std::vector<pulse::sdk::VfsChange>& changes, const pulse::sdk::VfsOperation& op) -> PulseStatus {
                for (const auto& c : changes)
                    seen->changes.push_back(std::to_string(c.kind) + ":" + std::string(c.rel_path) + ">" +
                                            std::string(c.new_rel_path) + "|" + std::string(c.source_path_utf8));
                if (!changes.empty() && changes[0].rel_path == "locked.txt") {
                    op.Message("The archive is in use");
                    return PULSE_ERROR_STATE;
                }
                return PULSE_OK;
            };
            auto rw_mount = client.RegisterProvider("rw0", "Writable", std::move(rw));
            Check(rw_mount.has_value() &&
                  (registry.CapabilitiesOf(L"pulse:vfs:pulse.7zip/rw0/x") & PULSE_VFS_CAP_MODIFY) != 0u &&
                  (registry.CapabilitiesOf(L"pulse:vfs:pulse.7zip/arc0") & PULSE_VFS_CAP_MODIFY) == 0u &&
                  registry.CapabilitiesOf(L"pulse:vfs:pulse.7zip/none") == 0u,
                  "v2 provider mounts and CapabilitiesOf reports per-mount write support");

            pulse::extensions::vfs::VfsOperationHooks hooks;
            uint64_t progress_bytes = 0;
            std::wstring progress_item;
            hooks.progress = [&](const pulse::extensions::vfs::VfsProgressInfo& p) {
                progress_bytes = p.completed_bytes;
                progress_item = p.current;
            };
            const PulseStatus ex_st = registry.ExtractItems(
                {L"pulse:vfs:pulse.7zip/rw0/文档/a.txt", L"pulse:vfs:pulse.7zip/rw0/文档/子"}, L"C:\\temp\\目标", hooks);
            Check(ex_st == PULSE_OK && seen->picks.size() == 2 && seen->picks[0] == "文档/a.txt" &&
                  seen->dest == "C:\\temp\\目标" && progress_bytes == 5 && progress_item == L"a.txt",
                  "ExtractItems forwards relative picks, UTF-8 destination and progress");
            Check(registry.ExtractItems({L"pulse:vfs:pulse.7zip/rw0/a", L"pulse:vfs:pulse.7zip/arc0/b"}, L"C:\\t", hooks) ==
                      PULSE_ERROR_ARGUMENT &&
                  registry.ExtractItems({L"pulse:vfs:pulse.7zip/arc0/readme.txt"}, L"C:\\t", hooks) == PULSE_ERROR_UNAVAILABLE,
                  "ExtractItems rejects mixed mounts and mounts without the capability");

            using pulse::extensions::vfs::VfsChangeRequest;
            const std::vector<VfsChangeRequest> batch = {
                {PULSE_VFS_CHANGE_PUT, L"pulse:vfs:pulse.7zip/rw0/new.txt", L"C:\\src\\新.txt", {}},
                {PULSE_VFS_CHANGE_RENAME, L"pulse:vfs:pulse.7zip/rw0/old", {}, L"pulse:vfs:pulse.7zip/rw0/renamed"},
                {PULSE_VFS_CHANGE_DELETE, L"pulse:vfs:pulse.7zip/rw0/gone", {}, {}}};
            Check(registry.Modify(batch, hooks) == PULSE_OK && seen->changes.size() == 3 &&
                  seen->changes[0] == "0:new.txt>|C:\\src\\新.txt" && seen->changes[1] == "2:old>renamed|" &&
                  seen->changes[2] == "1:gone>|",
                  "Modify forwards PUT/RENAME/DELETE as one call");
            Check(registry.Modify({{PULSE_VFS_CHANGE_RENAME, L"pulse:vfs:pulse.7zip/rw0/a", {}, L"pulse:vfs:pulse.7zip/arc0/a"}}, hooks) ==
                      PULSE_ERROR_ARGUMENT &&
                  registry.Modify({{PULSE_VFS_CHANGE_RESTORE, L"pulse:vfs:pulse.7zip/rw0", {}, {}},
                                   {PULSE_VFS_CHANGE_DELETE, L"pulse:vfs:pulse.7zip/rw0/x", {}, {}}}, hooks) == PULSE_ERROR_ARGUMENT &&
                  registry.Modify({{PULSE_VFS_CHANGE_DELETE, L"pulse:vfs:pulse.7zip/arc0/readme.txt", {}, {}}}, hooks) ==
                      PULSE_ERROR_UNAVAILABLE &&
                  seen->changes.size() == 3,
                  "Modify rejects cross-mount renames, RESTORE mixed with changes, and read-only mounts");
            seen->changes.clear();
            Check(registry.Modify({{PULSE_VFS_CHANGE_RESTORE, L"pulse:vfs:pulse.7zip/rw0", {}, {}}}, hooks) == PULSE_OK &&
                      seen->changes.size() == 1 && seen->changes[0] == "3:>|" &&
                  registry.Modify({{PULSE_VFS_CHANGE_DELETE, L"pulse:vfs:pulse.7zip/rw0/locked.txt", {}, {}}}, hooks) ==
                      PULSE_ERROR_STATE &&
                  hooks.message == L"The archive is in use",
                  "RESTORE reaches the provider at the mount root; failure reasons come back to the host");
            rw_mount->Reset();
        }

        // List root + verify evil entry filtered cleanly + metadata preserved.
        {
            std::vector<pulse::extensions::vfs::VfsEntryData> rows;
            const PulseStatus st = registry.ListDirectory(L"pulse:vfs:pulse.7zip/arc0", rows);
            Check(st == PULSE_OK && rows.size() == 2 &&
                  rows[0].name == L"docs" && (rows[0].flags & PULSE_VFS_ENTRY_DIRECTORY) != 0 &&
                  rows[1].name == L"readme.txt" && rows[1].size_bytes == 11u,
                  "ListDirectory returns valid entries and ignores invalid emitted entry names");
        }

        // Stat subentry.
        {
            pulse::extensions::vfs::VfsEntryData entry;
            const PulseStatus st = registry.Stat(L"pulse:vfs:pulse.7zip/arc0/docs/notes.txt", entry);
            Check(st == PULSE_OK && entry.name == L"notes.txt" && entry.size_bytes == 5u,
                  "Stat inspects single VFS item");
        }

        // ExtractFile writes real temp file beside EXE, then deletes it cleanly.
        {
            const std::wstring dest = ExeSiblingPath(L"vfs_test_extracted_notes.txt");
            DeleteFileW(dest.c_str());
            const PulseStatus st = registry.ExtractFile(L"pulse:vfs:pulse.7zip/arc0/docs/notes.txt", dest);
            std::string actual;
            HANDLE h = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char buf[32]{};
                DWORD read_bytes = 0;
                if (ReadFile(h, buf, sizeof(buf), &read_bytes, nullptr)) {
                    actual.assign(buf, read_bytes);
                }
                CloseHandle(h);
            }
            DeleteFileW(dest.c_str());
            Check(st == PULSE_OK && actual == "hello",
                  "ExtractFile materializes virtual file cleanly without %TEMP% dependency");
        }

        // InvalidatePath & OpenPath forwarding + bad rel_path / mode rejection.
        Check(mount->Invalidate("\\docs\\") == PULSE_OK &&
              invalidations.size() == 1 && invalidations[0] == "pulse.7zip/arc0/docs",
              "Invalidate forwards normalized relative path to host backend");
        Check(mount->Open("docs", PULSE_VFS_OPEN_NEW_TAB) == PULSE_OK &&
              opened.size() == 1 &&
              opened[0].first == L"pulse:vfs:pulse.7zip/arc0/docs" &&
              opened[0].second == PULSE_VFS_OPEN_NEW_TAB,
              "Open forwards canonical pulse:vfs: URI and tab mode to host backend");
        Check(mount->Invalidate("../bad") == PULSE_ERROR_ARGUMENT &&
              mount->Open("", 99u) == PULSE_ERROR_ARGUMENT,
              "Invalidate and Open reject traversal rel_path and unknown mode");

        // Concurrent worker drain safety: Close() must NOT block UI thread while a worker is
        // inside list_dir, and must signal cancel + keep CallbackActive() true until worker exits!
        std::mutex sync_mu;
        std::condition_variable sync_cv;
        bool inside_callback = false;
        pulse::sdk::VfsProviderCallbacks slow_cb;
        slow_cb.list_dir = [&](std::string_view,
                               const std::function<bool(const pulse::sdk::VfsEntryData&)>&,
                               const std::function<bool()>& cancelled) -> PulseStatus {
            {
                std::lock_guard lock(sync_mu);
                inside_callback = true;
            }
            sync_cv.notify_all();
            for (int i = 0; i < 400; ++i) {
                if (cancelled && cancelled()) return PULSE_ERROR_STATE;
                Sleep(5);
            }
            return PULSE_OK;
        };
        auto slow_mount = client.RegisterProvider("slow", "Slow Mount", std::move(slow_cb));
        Check(slow_mount.has_value(), "Register slow mount for drain safety test");

        std::atomic<PulseStatus> worker_st{PULSE_OK};
        std::thread slow_worker([&] {
            std::vector<pulse::extensions::vfs::VfsEntryData> rows;
            worker_st.store(registry.ListDirectory(L"pulse:vfs:pulse.7zip/slow", rows),
                            std::memory_order_release);
        });
        {
            std::unique_lock lock(sync_mu);
            sync_cv.wait_for(lock, std::chrono::seconds(2), [&] { return inside_callback; });
        }
        Check(inside_callback && adapter.CallbackActive(),
              "CallbackActive is true while worker thread is inside VFS provider callback");

        const auto close_t0 = std::chrono::steady_clock::now();
        adapter.Close();
        const auto close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - close_t0).count();
        Check(close_ms < 200, "VfsAdapter::Close unregisters mounts without blocking UI thread");

        slow_worker.join();
        Check(worker_st.load(std::memory_order_acquire) == PULSE_ERROR_STATE &&
              !adapter.CallbackActive() &&
              wake_count.load(std::memory_order_relaxed) >= 1,
              "Unregistering mount signals cooperative cancel, drains worker cleanly, and wakes host");

        // After Close(), arc0 and slow must no longer resolve in registry.
        VfsMountInfo dummy;
        Check(!registry.DescribeMount("pulse.7zip", "arc0", dummy) &&
              !registry.DescribeMount("pulse.7zip", "slow", dummy),
              "Adapter Close removes all session mounts from registry");
        mount->Reset();
        slow_mount->Reset();
    }

    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}