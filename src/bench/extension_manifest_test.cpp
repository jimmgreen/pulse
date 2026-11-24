#include "../app/extensions/manifest.h"
#include "../common/json_value.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::string Wrap(std::string extra = {}) {
    return R"({"apiVersion":1,"id":"sample.native","name":"Sample")" + extra + "}";
}
void Reject(const std::string& input, const char* label) {
    const auto result = pulse::extensions::ParseManifest(input);
    Check(!result && !result.manifest && !result.error.empty(), label);
}
}
int main() {
    using pulse::extensions::ParseManifest;
    const auto metadata = ParseManifest(Wrap());
    Check(metadata && metadata.manifest->id == "sample.native" && metadata.manifest->resources.empty(),
        "native template identity and empty capability declarations preserved");
    const std::string native = R"(,"native":{"version":1,"architecture":"windows-x64","library":"sample.dll","sha256":")" + std::string(64, 'a') + R"("})";
    const auto package = ParseManifest(Wrap(native));
    Check(package && package.manifest->native && package.manifest->native->library == "sample.dll",
        "native DLL package accepted without commands");
    for (const auto* field : {"commands", "toolbar", "palette", "workflows", "panel", "runtime"}) {
        for (const auto* value : {"null", "[]", "{}"}) {
            const auto result = ParseManifest(Wrap(native + ",\"" + field + "\":" + value));
            Check(!result && result.error == std::string(field) + " is no longer supported; use a native C++ DLL package",
                "removed declarations explicitly rejected regardless of value");
        }
    }
    Reject(Wrap(R"(,"commands":[{"id":"run","title":"Run","capability":"runtime.invoke"}])"), "old executable command rejected");
    for (const auto* input : {"[]", "null", "{}", R"({"apiVersion":2,"id":"sample","name":"Sample"})",
        R"({"apiVersion":1,"id":"bad id","name":"Sample"})", R"({"apiVersion":1,"id":"sample","name":" "})",
        R"({"apiVersion":1,"id":"sample","id":"duplicate","name":"Sample"})"}) Reject(input, "invalid metadata rejected");
    Reject(Wrap(R"(,"unknown":true)"), "unknown root field rejected");
    Reject(std::string(pulse::extensions::kMaxManifestBytes + 1, ' '), "manifest byte budget enforced");
    for (const auto* value : {R"(["context.read","context.read"])", R"(["unknown"])", R"([1])", "null"})
        Reject(Wrap(std::string(R"(,"resources":)") + value), "invalid resource declarations rejected");
    const auto resources = ParseManifest(Wrap(R"(,"resources":["context.read","files.transferDirectoryTree","operations.own"])"));
    Check(resources && resources.manifest->resources.size() == 3, "native capability declarations preserved");
    pulse::json::Value base;
    Check(pulse::json::Parse(Wrap(native), base), "native mutation fixture parses");
    for (const auto* library : {"../sample.dll", "C:\\sample.dll", "CON.dll", "LPT1.dll", "sample.exe"}) {
        auto value = base;
        auto entry = *value.find("native"); entry.set("library", library); value.set("native", std::move(entry));
        Reject(pulse::json::Serialize(value), "unsafe native library filename rejected");
    }
    for (const auto* field : {"version", "architecture", "sha256"}) {
        auto value = base;
        auto entry = *value.find("native"); entry.set(field, "invalid"); value.set("native", std::move(entry));
        Reject(pulse::json::Serialize(value), "native ABI and digest constraints preserved");
    }
    const auto examples = std::filesystem::path(PULSE_EXTENSION_EXAMPLES_PATH).parent_path() / "native";
    size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(examples)) {
        const auto path = entry.path() / "manifest.json";
        if (!std::filesystem::is_regular_file(path)) continue;
        std::ifstream input(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        Check(static_cast<bool>(ParseManifest(text)), "native template parses without legacy fields");
        ++checked;
    }
    Check(checked >= 11, "every native example manifest checked (11 templates or more)");

    const std::string showcase_manifest = Wrap(
        R"(,"contributes":{"icons":[{"id":"brand","viewBox":24,"layers":[{"path":"M4 4h16v16H4z","fill":true,"color":"accent"}]}]})"
        R"(,"showcase":{"version":"1.2.3","summary":{"zh":"中文摘要","en":"English summary"},"description":{"zh":"第一行\n第二行","en":"Line 1\r\nLine 2"},"category":"utility","publisher":"Sample Lab","engine":{"zh":"自研核心","en":"Custom Core"},"license":"MIT","homepage":"https://example.com","source":"https://github.com/example/sample","icon":"brand","iconColor":"#3A7E9B","tags":["demo","fast"],"highlights":[{"zh":"特性一","en":"Highlight 1"}]})");
    const auto sc = ParseManifest(showcase_manifest);
    Check(sc && sc.manifest->showcase &&
              sc.manifest->showcase->version == "1.2.3" &&
              sc.manifest->showcase->summary.en == "English summary" &&
              sc.manifest->showcase->description.en == "Line 1\nLine 2" &&
              sc.manifest->showcase->category == "utility" &&
              sc.manifest->showcase->publisher == "Sample Lab" &&
              sc.manifest->showcase->engine.zh == "自研核心" &&
              sc.manifest->showcase->license == "MIT" &&
              sc.manifest->showcase->homepage == "https://example.com" &&
              sc.manifest->showcase->source == "https://github.com/example/sample" &&
              sc.manifest->showcase->icon_name == "brand" &&
              sc.manifest->showcase->has_icon_color &&
              sc.manifest->showcase->icon_color == 0x3A7E9Bu &&
              sc.manifest->showcase->tags.size() == 2 &&
              sc.manifest->showcase->highlights.size() == 1,
          "showcase metadata with custom icon, localized multiline description and tags accepted");
    Reject(Wrap(R"(,"showcase":{"unknown":1})"), "showcase unexpected field rejected");
    Reject(Wrap(R"json(,"showcase":{"homepage":"javascript:alert(1)"})json"), "showcase non-http homepage rejected");
    Reject(Wrap(R"(,"showcase":{"source":"file:///C:/foo"})"), "showcase non-http source rejected");
    Reject(Wrap(R"(,"showcase":{"iconColor":"red"})"), "showcase invalid iconColor rejected");
    Reject(Wrap(R"(,"showcase":{"icon":"missing-icon"})"), "showcase undeclared custom icon rejected");
    Reject(Wrap(R"(,"showcase":{"tags":["dup","dup"]})"), "showcase duplicate tags rejected");
    Reject(Wrap(std::string(R"(,"showcase":{"description":")") + std::string(4097, 'a') + R"("})"),
           "showcase oversized description rejected");

    const std::string fh_manifest = Wrap(native +
        R"(,"contributes":{"commands":[)"
        R"({"id":"extract","title":"Extract","kinds":["files"],"minSelection":1,"maxSelection":1,"extensions":[".7z",".tar.zst"]},)"
        R"({"id":"inspect","title":"Inspect","kinds":["files"],"minSelection":1,"maxSelection":1,"extensions":[".cbz"]}],)"
        R"("fileHandlers":[)"
        R"({"id":"archive-open","displayName":{"zh":"压缩包","en":"Archive File"},"verbLabel":{"zh":"解压","en":"Extract"},"extensions":[".7z",".tar.zst"],"command":"extract","action":"command","iconResource":"archive_icons.dll,-103","fallbackOnUnassociated":true},)"
        R"({"id":"comic-preview","displayName":"Comic Book","extensions":[".cbz"],"command":"inspect","action":"preview"}]})");
    const auto fh = ParseManifest(fh_manifest);
    Check(fh && fh.manifest->file_handlers.size() == 2 &&
              fh.manifest->file_handlers[0].id == "archive-open" &&
              fh.manifest->file_handlers[0].display_name.en == "Archive File" &&
              fh.manifest->file_handlers[0].verb_label.zh == "解压" &&
              fh.manifest->file_handlers[0].extensions == std::vector<std::string>{".7z", ".tar.zst"} &&
              fh.manifest->file_handlers[0].command == "extract" &&
              fh.manifest->file_handlers[0].action == pulse::extensions::FileHandlerAction::Command &&
              fh.manifest->file_handlers[0].icon_resource == "archive_icons.dll,-103" &&
              fh.manifest->file_handlers[0].fallback_on_unassociated &&
              fh.manifest->file_handlers[1].id == "comic-preview" &&
              fh.manifest->file_handlers[1].action == pulse::extensions::FileHandlerAction::Preview &&
              !fh.manifest->file_handlers[1].fallback_on_unassociated,
          "contributes.fileHandlers valid command and preview declarations accepted");

    for (const auto* forbidden : {".exe", ".bat", ".cmd", ".ps1", ".lnk", ".dll", ".msi", ".vbs"}) {
        Reject(Wrap(std::string(R"(,"contributes":{"fileHandlers":[{"id":"bad","displayName":"Bad","extensions":[")") +
                    forbidden + R"("],"command":"open"}]})"),
               "contributes.fileHandlers rejects forbidden executable/script extension");
    }
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"a","displayName":"A","extensions":[".zip"],"command":"open"},{"id":"b","displayName":"B","extensions":[".zip"],"command":"open2"}]})"),
           "contributes.fileHandlers rejects duplicate extension across handlers");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"dup","displayName":"A","extensions":[".zip"],"command":"open"},{"id":"dup","displayName":"B","extensions":[".rar"],"command":"open2"}]})"),
           "contributes.fileHandlers rejects duplicate handler id");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"bad-icon","displayName":"A","extensions":[".zip"],"command":"open","iconResource":"../evil.dll,0"}]})"),
           "contributes.fileHandlers rejects path-traversal iconResource");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"bad-icon2","displayName":"A","extensions":[".zip"],"command":"open","iconResource":"sub\\a.ico"}]})"),
           "contributes.fileHandlers rejects backslash iconResource");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"bad-act","displayName":"A","extensions":[".zip"],"command":"open","action":"launch"}]})"),
           "contributes.fileHandlers rejects unknown action");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"empty-ext","displayName":"A","extensions":[],"command":"open"}]})"),
           "contributes.fileHandlers rejects empty extensions array");
    Reject(Wrap(R"(,"contributes":{"fileHandlers":[{"id":"bad-fb","displayName":"A","extensions":[".zip"],"command":"open","fallbackOnUnassociated":"yes"}]})"),
           "contributes.fileHandlers rejects non-boolean fallbackOnUnassociated");

    const std::string qs_manifest = Wrap(native +
        R"(,"showcase":{"version":"1.0.0","quickSettings":{"title":{"zh":"快捷设置","en":"Quick Settings"},"readyState":"ready","statusState":"status","rows":[)"
        R"({"title":{"zh":"自动处理","en":"Auto Process"},"description":{"zh":"自动整理选中项","en":"Organize selection"},"controls":[{"kind":"switch","state":"auto_run"}]},)"
        R"({"title":{"zh":"编码预设","en":"Preset"},"controls":[{"kind":"select","state":"preset","width":120,"options":[{"value":"fast","label":"Fast"},{"value":"best","label":{"zh":"极限","en":"Best"}}],"disabledWhen":{"state":"preset_lock","equals":"1"},"disabledLabel":"Locked"},{"kind":"select","state":"mode","width":140,"options":[{"value":"0","label":"Std"}]}]},)"
        R"({"title":{"zh":"导出日志","en":"Export Log"},"controls":[{"kind":"button","command":"export_log","label":{"zh":"打开目录","en":"Open Folder"},"width":112}]})"
        R"(],"menuPreview":{"caption":"Menu Preview","submenuState":"sub","sampleEntries":[{"name":"sample.arc"},{"name":"folder","directory":true}]}}})");
    const auto qs = ParseManifest(qs_manifest);
    Check(qs && qs.manifest->showcase && qs.manifest->showcase->quick_settings &&
              qs.manifest->showcase->quick_settings->title.en == "Quick Settings" &&
              qs.manifest->showcase->quick_settings->ready_state == "ready" &&
              qs.manifest->showcase->quick_settings->status_state == "status" &&
              qs.manifest->showcase->quick_settings->rows.size() == 3 &&
              qs.manifest->showcase->quick_settings->rows[1].controls.size() == 2 &&
              qs.manifest->showcase->quick_settings->rows[1].controls[0].disabled_when.present &&
              qs.manifest->showcase->quick_settings->rows[1].controls[0].disabled_label.en == "Locked" &&
              qs.manifest->showcase->quick_settings->rows[2].controls[0].kind == pulse::extensions::QuickSettingControlKind::Button &&
              qs.manifest->showcase->quick_settings->menu_preview &&
              qs.manifest->showcase->quick_settings->menu_preview->sample_entries.size() == 2 &&
              qs.manifest->showcase->quick_settings->menu_preview->sample_entries[1].directory,
          "showcase.quickSettings valid switches, selects, buttons, disabledWhen, and menuPreview accepted");

    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[]}}})"), "quickSettings rejects empty rows");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"1","controls":[{"kind":"switch","state":"a"}]},{"title":"2","controls":[{"kind":"switch","state":"b"}]},{"title":"3","controls":[{"kind":"switch","state":"c"}]},{"title":"4","controls":[{"kind":"switch","state":"d"}]},{"title":"5","controls":[{"kind":"switch","state":"e"}]}]}}})"),
           "quickSettings rejects more than 4 rows");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"select","state":"s","options":[]}]}]}}})"),
           "quickSettings select rejects empty options");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"select","state":"s","options":[{"value":"v","label":"A"},{"value":"v","label":"B"}]}]}]}}})"),
           "quickSettings select rejects duplicate option values");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"switch","state":"s","command":"c"}]}]}}})"),
           "quickSettings switch rejects command field");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"button","command":"c","label":"Go","state":"s"}]}]}}})"),
           "quickSettings button rejects state field");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"slider","state":"s"}]}]}}})"),
           "quickSettings rejects unknown control kind");
    Reject(Wrap(R"(,"showcase":{"quickSettings":{"rows":[{"title":"r","controls":[{"kind":"switch","state":"s"}]}],"menuPreview":{"sampleEntries":[{"name":"../bad.arc"}]}}})"),
           "quickSettings menuPreview rejects path-traversal sample entry");

    // activation: commandless packages start with the window unless they opt out;
    // packages with commands start on demand unless they opt in.
    using pulse::extensions::ManifestActivation;
    const std::string command = R"(,"contributes":{"commands":[{"id":"go","title":"Go","kinds":["files"],"minSelection":1,"maxSelection":1}]})";
    const auto plain = ParseManifest(Wrap());
    Check(plain && plain.manifest->activation == ManifestActivation::Default && plain.manifest->StartsWithWindow(),
          "activation: commandless package starts with the window by default");
    const auto empty_commands = ParseManifest(Wrap(R"(,"contributes":{"commands":[]})"));
    Check(empty_commands && empty_commands.manifest->StartsWithWindow(), "activation: empty command list counts as commandless");
    const auto with_command = ParseManifest(Wrap(command));
    Check(with_command && !with_command.manifest->StartsWithWindow(), "activation: package with commands starts on demand by default");
    const auto opt_in = ParseManifest(Wrap(R"(,"activation":"startup")" + command));
    Check(opt_in && opt_in.manifest->activation == ManifestActivation::Startup && opt_in.manifest->StartsWithWindow(),
          "activation: startup opts a command package in");
    const auto opt_out = ParseManifest(Wrap(R"(,"activation":"onDemand")"));
    Check(opt_out && opt_out.manifest->activation == ManifestActivation::OnDemand && !opt_out.manifest->StartsWithWindow(),
          "activation: onDemand opts a commandless package out");
    const auto events = ParseManifest(Wrap(R"(,"resources":["context.events"])" + command));
    Check(events && events.manifest->StartsWithWindow(), "activation: context.events makes a command package start with the window");
    const auto events_lazy = ParseManifest(Wrap(R"(,"resources":["context.events"],"activation":"onDemand")" + command));
    Check(events_lazy && !events_lazy.manifest->StartsWithWindow(), "activation: onDemand overrides the context.events default");
    const auto read_only = ParseManifest(Wrap(R"(,"resources":["context.read"])" + command));
    Check(read_only && !read_only.manifest->StartsWithWindow(), "activation: context.read alone keeps a command package on demand");
    Reject(Wrap(R"(,"activation":"eager")"), "activation rejects unknown values");
    Reject(Wrap(R"(,"activation":true)"), "activation rejects non-strings");
    return failures ? 1 : 0;
}