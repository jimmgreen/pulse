#include "../../app/extensions/command_projection_cache.h"
#include "../../app/extensions/icon_registry.h"
#include "../../app/extensions/native/commands_adapter.h"
#include "../../../extensions/7zip/text.h"
#include "../../../sdk/native/include/pulse/icons.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace pulse::extensions;
using namespace pulse::extensions::native;
namespace fs = std::filesystem;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    if (ok) {
        std::printf("[PASS] %s\n", label);
        ++g_passed;
    } else {
        std::printf("[FAIL] %s\n", label);
        ++g_failed;
    }
}

PulseStringView Sv(const char* s) noexcept {
    return {s, s ? static_cast<uint32_t>(std::strlen(s)) : 0u};
}

class TempDirGuard {
public:
    explicit TempDirGuard(const std::wstring& suffix) {
        wchar_t mod_buf[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, mod_buf, ARRAYSIZE(mod_buf));
        const fs::path base = len > 0 ? fs::path(std::wstring(mod_buf, len)).parent_path() : fs::current_path();
        path_ = base / (L"pulse-cmd-proj-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64()) + L"-" + suffix);
        std::error_code ec;
        fs::remove_all(path_, ec);
        if (!fs::create_directories(path_, ec) && ec) {
            std::printf("[WARN] create_directories failed (%d) for temp root\n", ec.value());
        }
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& Path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::wstring CacheFilePath(const fs::path& root, std::string_view package_id) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring stem = L"pkg-";
    for (unsigned char c : package_id) {
        stem += digits[c >> 4];
        stem += digits[c & 15];
    }
    return (root / L"extensions" / L"command-cache" / (stem + L".json")).wstring();
}

void WriteRawFile(const std::wstring& path, const std::string& bytes) {
    const fs::path p(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void TestDiskPersistenceAndValidation() {
    TempDirGuard temp(L"disk");
    const std::wstring root = temp.Path().wstring();

    CommandProjectionSnapshot snap;
    snap.package_id = "third.party.archiver";
    snap.revision = "rev-abc-123";
    snap.language = "en-US";

    ProjectedCommand cmd1;
    cmd1.id = "extract-here";
    cmd1.title = "Extract {base}";
    cmd1.detail = "Into {stem} folder";
    cmd1.group = "Archiver";
    cmd1.icon = 2;
    cmd1.icon_name = "archiver-extract";
    cmd1.group_icon = 1;
    cmd1.kinds = PULSE_COMMAND_FILES;
    cmd1.min_selection = 1;
    cmd1.max_selection = 64;
    cmd1.extensions = {".zip", ".7z"};
    cmd1.exclude_extensions = false;
    cmd1.virtual_key = 'E';
    cmd1.modifiers = PULSE_COMMAND_CTRL | PULSE_COMMAND_SHIFT;
    cmd1.flags = PULSE_COMMAND_PRIMARY | PULSE_COMMAND_GROUP_SUBMENU;

    ProjectedCommand cmd2;
    cmd2.id = "hidden-op";
    cmd2.title = "Hidden";
    cmd2.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_BACKGROUND;
    cmd2.min_selection = 0;
    cmd2.max_selection = 16;
    cmd2.exclude_extensions = true;
    cmd2.extensions = {".tmp"};
    cmd2.flags = PULSE_COMMAND_HIDDEN;

    snap.commands = {cmd1, cmd2};

    Check(WriteCommandProjectionFile(root, snap), "WriteCommandProjectionFile succeeds");
    const auto loaded = ReadCommandProjectionFile(root, snap.package_id);
    Check(loaded.has_value() && *loaded == snap,
          "ReadCommandProjectionFile round-trips commands, flags, extensions, shortcuts, and icon_name");

    CommandProjectionSnapshot empty_snap{"third.party.empty", "rev-empty", "zh-CN", {}};
    Check(WriteCommandProjectionFile(root, empty_snap), "WriteCommandProjectionFile accepts explicit empty command list");
    const auto loaded_empty = ReadCommandProjectionFile(root, empty_snap.package_id);
    Check(loaded_empty.has_value() && *loaded_empty == empty_snap && loaded_empty->commands.empty(),
          "ReadCommandProjectionFile preserves empty commands: [] array");

    const auto cache_file = CacheFilePath(temp.Path(), snap.package_id);
    WriteRawFile(cache_file, "{broken json");
    Check(!ReadCommandProjectionFile(root, snap.package_id).has_value(), "corrupt JSON cache file is rejected");

    WriteRawFile(cache_file,
                 R"({"version":2,"packageId":"third.party.archiver","revision":"r","language":"en-US","commands":[]})");
    Check(!ReadCommandProjectionFile(root, snap.package_id).has_value(), "wrong schema version is rejected");

    std::string too_many = R"({"version":1,"packageId":"third.party.archiver","revision":"r","language":"en-US","commands":[)";
    for (int i = 0; i < 33; ++i) {
        if (i > 0) too_many += ',';
        too_many += R"({"id":"c)" + std::to_string(i) + R"(","title":"T","kinds":1,"maxSelection":1})";
    }
    too_many += "]}";
    WriteRawFile(cache_file, too_many);
    Check(!ReadCommandProjectionFile(root, snap.package_id).has_value(), ">32 commands cache file is rejected");

    std::string oversized = R"({"version":1,"packageId":"third.party.archiver","revision":"r","language":"en-US","commands":[{"id":"c0","title":")" +
                            std::string(70000, 'a') + R"(","kinds":1,"maxSelection":1}]})";
    WriteRawFile(cache_file, oversized);
    Check(!ReadCommandProjectionFile(root, snap.package_id).has_value(), ">64 KiB cache file is rejected");

    Check(PurgeCommandProjectionFile(root, snap.package_id) &&
              !ReadCommandProjectionFile(root, snap.package_id).has_value(),
          "PurgeCommandProjectionFile removes persisted file cleanly");
}

void TestMemoryCacheLifecycle() {
    command_projection::ResetForTest();

    CommandProjectionSnapshot disk_snap;
    disk_snap.package_id = "sample.plugin";
    disk_snap.revision = "sha-v1";
    disk_snap.language = "zh-CN";
    disk_snap.commands.push_back({"open", "Disk Title", "", "", 0, "", 0, "", PULSE_COMMAND_FILES, 1, 10, {}, false, 0, 0, 0});

    uint64_t warm_gen = 0;
    Check(command_projection::BeginWarmRead("sample.plugin", warm_gen), "BeginWarmRead starts cold read");
    uint64_t dup_gen = 999;
    Check(!command_projection::BeginWarmRead("sample.plugin", dup_gen) && dup_gen == warm_gen,
          "duplicate BeginWarmRead while reading returns false");

    // Live publish occurs while warm disk read is in flight.
    CommandProjectionSnapshot live_snap = disk_snap;
    live_snap.commands[0].title = "Live Title";
    command_projection::PublishLive(live_snap);

    // Late warm read with old generation must NOT clobber fresher live snapshot.
    command_projection::PublishWarmRead("sample.plugin", warm_gen, disk_snap);
    const auto hit = command_projection::Lookup("sample.plugin", "sha-v1", "zh-CN");
    Check(hit.has_value() && hit->commands.size() == 1 && hit->commands[0].title == "Live Title",
          "stale PublishWarmRead does not overwrite fresher PublishLive");

    Check(!command_projection::Lookup("sample.plugin", "sha-v2", "zh-CN").has_value(),
          "revision mismatch returns nullopt");
    Check(!command_projection::Lookup("sample.plugin", "sha-v1", "en-US").has_value(),
          "language mismatch returns nullopt");

    command_projection::Invalidate("sample.plugin");
    Check(!command_projection::Lookup("sample.plugin", "sha-v1", "zh-CN").has_value(),
          "Invalidate clears cached entry");

    uint64_t next_gen = 0;
    Check(command_projection::BeginWarmRead("sample.plugin", next_gen) && next_gen > warm_gen,
          "Invalidate allows re-warming at a higher generation");
    command_projection::PublishWarmRead("sample.plugin", next_gen, disk_snap);
    const auto rewarmed = command_projection::Lookup("sample.plugin", "sha-v1", "zh-CN");
    Check(rewarmed.has_value() && rewarmed->commands[0].title == "Disk Title",
          "PublishWarmRead succeeds when generation matches");

    command_projection::ResetForTest();
}

void TestCommandsAdapterExportAndEvaluation() {
    const std::string pkg_id = "sample.commands.proj";

    PulseIconLayer layer{};
    layer.size = sizeof(layer);
    layer.draw_flags = PULSE_ICON_DRAW_FILL | PULSE_ICON_DRAW_CLOSED;
    layer.path_data = Sv("M 2 2 L 14 2 L 14 14 L 2 14 Z");
    layer.color_role = PULSE_ICON_COLOR_ACCENT;
    PulseIconDescriptor icon_desc{};
    icon_desc.size = sizeof(icon_desc);
    icon_desc.view_box_milli = 16000u;
    icon_desc.name = Sv("brand-archive");
    icon_desc.layers = &layer;
    icon_desc.layer_count = 1;
    PulseIconId registered_icon = 0;
    Check(icons::GlobalRegistry().RegisterDynamic(pkg_id, &icon_desc, &registered_icon) == PULSE_OK &&
              registered_icon >= PULSE_ICON_CUSTOM_MIN,
          "dynamic vector icon registers in GlobalRegistry for cold evaluation");
    const auto resolved_icon = icons::GlobalRegistry().ResolveName(pkg_id, "brand-archive");
    Check(resolved_icon.has_value() && *resolved_icon == registered_icon,
          "registered vector icon resolves by package/name");

    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x4123u));
    bool live = true;
    PulseContextV1 ctx{};
    CommandsAdapter adapter(owner, pkg_id, ctx, [&] { return live; });
    int change_count = 0;
    adapter.SetOnChanged([&] { ++change_count; });

    PulseCommandsV2 table{};
    Check(adapter.Query(PULSE_COMMANDS_VERSION_2, &table, sizeof(table)) == PULSE_OK,
          "CommandsAdapter exposes PulseCommandsV2");

    const PulseStringView ext_views[] = {Sv(".zip")};
    PulseCommandDescriptor desc{};
    desc.size = sizeof(desc);
    desc.id = Sv("extract");
    desc.title = Sv("Extract archive");
    desc.detail = Sv("To {stem}\\ ({base})");
    desc.group = Sv("Sample Group");
    desc.icon = PULSE_COMMAND_ICON_EXTRACT;
    desc.icon_name = Sv("brand-archive");
    desc.kinds = PULSE_COMMAND_FILES;
    desc.min_selection = 1;
    desc.max_selection = 16;
    desc.extensions = ext_views;
    desc.extension_count = 1;
    desc.flags = PULSE_COMMAND_PRIMARY;
    desc.invoke = [](void*, PulseCommandId, const PulseContextSelectionSnapshot*) {};

    PulseCommandId cmd_id = 0;
    Check(table.register_command(table.context, &desc, &cmd_id) == PULSE_OK && cmd_id != 0 && change_count == 1,
          "SetOnChanged fires on register_command");

    desc.detail = Sv("Updated {stem}");
    Check(table.update_command(table.context, cmd_id, &desc) == PULSE_OK && change_count == 2,
          "SetOnChanged fires on update_command");

    Check(table.set_command_flags(table.context, cmd_id, PULSE_COMMAND_PRIMARY | PULSE_COMMAND_CHECKED) == PULSE_OK &&
              change_count == 3,
          "SetOnChanged fires on set_command_flags");

    const auto exported = adapter.ExportProjection();
    Check(exported.size() == 1 &&
              exported[0].id == "extract" &&
              exported[0].icon_name == "brand-archive" &&
              exported[0].detail == "Updated {stem}" &&
              exported[0].flags == (PULSE_COMMAND_PRIMARY | PULSE_COMMAND_CHECKED) &&
              exported[0].extensions == std::vector<std::string>{".zip"},
          "ExportProjection captures live command metadata and icon_name");

    Check(table.unregister_command(table.context, cmd_id) == PULSE_OK && change_count == 4 &&
              adapter.ExportProjection().empty(),
          "SetOnChanged fires on unregister_command");
    adapter.Close();

    // Cold evaluation of exported projection against a single .zip selection vs .txt selection.
    CommandSelection zip_sel;
    zip_sel.directory = "C:\\Work";
    zip_sel.valid = true;
    zip_sel.entries.push_back({"C:\\Work\\demo.zip", "demo.zip", false});

    const auto eval_zip = CommandsAdapter::EvaluateProjectedCommand(owner, 0, pkg_id, exported[0], zip_sel);
    Check(eval_zip.enabled &&
              eval_zip.token >= kFirstStaticCommandToken &&
              eval_zip.identity == "native:" + pkg_id + ":extract" &&
              eval_zip.detail == "Updated demo" &&
              eval_zip.icon == *resolved_icon,
          "EvaluateProjectedCommand resolves {stem}, custom icon_name, and static token cleanly");

    CommandSelection txt_sel = zip_sel;
    txt_sel.entries[0] = {"C:\\Work\\readme.txt", "readme.txt", false};
    const auto eval_txt = CommandsAdapter::EvaluateProjectedCommand(owner, 0, pkg_id, exported[0], txt_sel);
    Check(!eval_txt.enabled, "EvaluateProjectedCommand enforces extension filter");

    // Stale cross-session numeric custom icon ID without matching icon_name must reset to 0 cleanly.
    ProjectedCommand stale_icon_cmd = exported[0];
    stale_icon_cmd.icon_name.clear();
    stale_icon_cmd.icon = PULSE_ICON_CUSTOM_MIN + 777u;
    const auto eval_stale = CommandsAdapter::EvaluateProjectedCommand(owner, 0, pkg_id, stale_icon_cmd, zip_sel);
    Check(eval_stale.icon == 0u,
          "EvaluateProjectedCommand ignores stale numeric custom icon IDs when icon_name is empty");

    CommandsAdapter::ClearStaticOwner(owner);
    if (registered_icon) icons::GlobalRegistry().UnregisterDynamic(pkg_id, registered_icon);
}

void TestSevenZipTextHostLanguage() {
    text::SetHostLanguage("en-US");
    Check(text::CurrentLanguage() == text::MenuLanguage::EnUS &&
              std::string_view(text::Get(text::Id::CmdSmart)) == "Smart extract",
          "text::SetHostLanguage(\"en-US\") switches 7-Zip UI strings to English");

    text::SetHostLanguage("zh-CN");
    Check(text::CurrentLanguage() == text::MenuLanguage::ZhCN &&
              std::string_view(text::Get(text::Id::CmdSmart)) == "智能解压",
          "text::SetHostLanguage(\"zh-CN\") switches 7-Zip UI strings to Simplified Chinese");

    text::SetHostLanguage("zh-Hant-HK");
    Check(text::CurrentLanguage() == text::MenuLanguage::ZhTW &&
              std::string_view(text::Get(text::Id::CmdSmart)) == "智慧解壓縮",
          "text::SetHostLanguage(\"zh-Hant-HK\") switches 7-Zip UI strings to Traditional Chinese");

    text::SetHostLanguage("fr-FR");
    Check(text::CurrentLanguage() == text::MenuLanguage::EnUS,
          "unrecognized BCP-47 tag falls back cleanly to English");

    text::SetHostLanguage("");
    (void)text::CurrentLanguage();
    Check(true, "empty language tag restores OS display fallback cleanly");
}

} // namespace

int main() {
    TestDiskPersistenceAndValidation();
    TestMemoryCacheLifecycle();
    TestCommandsAdapterExportAndEvaluation();
    TestSevenZipTextHostLanguage();
    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
