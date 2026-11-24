#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions {

struct ProjectedCommand {
    std::string id;
    std::string title;
    std::string detail;
    std::string group;
    uint32_t icon = 0;
    std::string icon_name;
    uint32_t group_icon = 0;
    std::string group_icon_name;
    uint32_t kinds = 0;
    uint32_t min_selection = 0;
    uint32_t max_selection = 0;
    std::vector<std::string> extensions;
    bool exclude_extensions = false;
    uint32_t virtual_key = 0;
    uint32_t modifiers = 0;
    uint32_t flags = 0;
    bool operator==(const ProjectedCommand&) const = default;
};

struct CommandProjectionSnapshot {
    std::string package_id;
    std::string revision;
    std::string language; // "zh-CN", "zh-TW", "en-US"
    std::vector<ProjectedCommand> commands;
    bool operator==(const CommandProjectionSnapshot&) const = default;
};

std::optional<CommandProjectionSnapshot> ReadCommandProjectionFile(const std::wstring& root_directory,
                                                                   std::string_view package_id);
bool WriteCommandProjectionFile(const std::wstring& root_directory, const CommandProjectionSnapshot& snapshot);
bool PurgeCommandProjectionFile(const std::wstring& root_directory, std::string_view package_id);

namespace command_projection {

std::optional<CommandProjectionSnapshot> Lookup(std::string_view package_id,
                                                std::string_view revision,
                                                std::string_view language);
bool BeginWarmRead(std::string_view package_id, uint64_t& out_generation);
void PublishWarmRead(std::string_view package_id, uint64_t generation,
                     std::optional<CommandProjectionSnapshot> snapshot);
void PublishLive(CommandProjectionSnapshot snapshot);
void Invalidate(std::string_view package_id);
void ResetForTest();

} // namespace command_projection

} // namespace pulse::extensions
