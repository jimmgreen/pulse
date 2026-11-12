#pragma once
#include "../../../../sdk/native/include/pulse/commands.h"
#include "../manifest.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace pulse::extensions::native {
inline constexpr int kFirstStaticCommandToken = 0x28000000;
inline constexpr int kLastStaticCommandToken = 0x2fffffff;

enum class StaticCommandLanguage : uint8_t {
    ZhCN,
    EnUS,
    ZhTW,
};

struct CommandSelectionEntry {
    std::string path;
    std::string name;
    bool directory = false;
};

struct CommandSelection {
    std::string directory;
    std::vector<CommandSelectionEntry> entries;
    bool valid = false;
};

struct StaticCommandTarget {
    size_t package = 0;
    std::string package_id;
    std::string command_id;
    std::string identity;
};

struct CommandInfo {
    int token = 0;
    std::string identity, title;
    std::string package, group; // group: the descriptor's caption; empty = ungrouped
    std::string detail;         // resolved for the current selection; empty = none
    uint32_t icon = 0, group_icon = 0, key = 0, modifiers = 0, flags = 0;
    bool enabled = false;
};
struct PackageCommandSummary {
    std::string identity;
    std::string title;
    uint32_t icon = 0;
    bool hidden_by_plugin = false;
};
class CommandsAdapter {
public:
    CommandsAdapter(HWND owner, std::string package, PulseContextV1 context, std::function<bool()> live);
    ~CommandsAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
    static std::vector<CommandInfo> Entries(HWND owner);
    static std::vector<PackageCommandSummary> LivePackageCommands(HWND owner, const std::string& package);
    // A package's commands as the context menu shows them for a sample selection
    // (matching and detail lines alike), without calling the plugin.
    struct PreviewEntry { std::string name; bool directory = false; };
    static std::vector<CommandInfo> Preview(HWND owner, const std::string& package,
                                            const std::vector<PreviewEntry>& entries, const std::string& directory);
    static std::string ResolveLocalizedText(const ManifestLocalizedText& text,
                                            StaticCommandLanguage language);
    static CommandInfo EvaluateStaticContribution(HWND owner, size_t package,
                                                  const std::string& package_id,
                                                  const ManifestCommandContribution& contribution,
                                                  StaticCommandLanguage language,
                                                  const CommandSelection& selection);
    static std::optional<StaticCommandTarget> LookupStaticToken(HWND owner, int token);
    static void ClearStaticOwner(HWND owner);
    static std::string Identity(HWND owner, int token);
    static bool Invoke(HWND owner, int token);
    static bool InvokeIdentity(HWND owner, const std::string& identity);
    static bool Shortcut(HWND owner, uint32_t key, uint32_t modifiers);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
