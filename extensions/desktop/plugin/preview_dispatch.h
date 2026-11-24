#pragma once
#include <pulse/workbench.h>
#include "ipc/desktop_ipc.h"
#include <array>
#include <charconv>
#include <limits>

namespace pulse::desktop::plugin {
class PreviewDispatch {
public:
    bool Dispatch(std::string_view json, const PulseWorkbenchV2* api, PulseStatus& status) {
        std::string mode, path;
        ipc::JsonFindString(json, "mode", mode);
        uint32_t navigation = 0;
        if (mode == "preview") navigation = PULSE_NAVIGATE_PREVIEW;
        else if (mode == "hover_preview") navigation = PULSE_NAVIGATE_HOVER_PREVIEW;
        else if (mode == "dismiss_hover_preview") navigation = PULSE_NAVIGATE_DISMISS_HOVER_PREVIEW;
        else return false;
        status = PULSE_ERROR_ARGUMENT;
        if (!ipc::JsonFindString(json, "path", path) || !AbsolutePath(path)) return true;
        PulseScreenRect rect{};
        const std::array<const char*, 4> keys{"rect_x", "rect_y", "rect_w", "rect_h"};
        std::array<int32_t, 4> values{};
        int fields = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            const std::string key = "\"" + std::string(keys[i]) + "\"";
            auto pos = json.find(key);
            if (pos == std::string_view::npos) continue;
            ++fields;
            pos += key.size();
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
            if (pos == json.size() || json[pos++] != ':') return true;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
            auto result = std::from_chars(json.data() + pos, json.data() + json.size(), values[i]);
            if (result.ec != std::errc{}) return true;
            auto end = result.ptr;
            while (end < json.data() + json.size() && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
            if (end == json.data() + json.size() || (*end != ',' && *end != '}')) return true;
        }
        if (fields && (fields != 4 || values[2] <= 0 || values[3] <= 0)) return true;
        rect = {values[0], values[1], values[2], values[3]};
        status = PULSE_ERROR_UNAVAILABLE;
        if (!api || !api->reveal_path_from_rect) return true;
        status = api->reveal_path_from_rect(api->context, {path.data(), static_cast<uint32_t>(path.size())},
                                             navigation, fields ? &rect : nullptr);
        if (status == PULSE_OK) {
            if (navigation == PULSE_NAVIGATE_HOVER_PREVIEW) hover_path_ = path;
            else if (navigation == PULSE_NAVIGATE_PREVIEW || hover_path_ == path) hover_path_.clear();
        }
        return true;
    }
    PulseStatus Deactivate(const PulseWorkbenchV2* api) {
        if (hover_path_.empty()) return PULSE_OK;
        PulseStatus status = PULSE_ERROR_UNAVAILABLE;
        if (api && api->reveal_path_from_rect)
            status = api->reveal_path_from_rect(api->context, {hover_path_.data(), static_cast<uint32_t>(hover_path_.size())},
                                              PULSE_NAVIGATE_DISMISS_HOVER_PREVIEW, nullptr);
        hover_path_.clear();
        return status;
    }
    const std::string& HoverPath() const { return hover_path_; }
private:
    static bool AbsolutePath(std::string_view path) {
        const bool drive = path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && (path[2] == '\\' || path[2] == '/');
        const bool unc = path.size() >= 5 && path[0] == '\\' && path[1] == '\\';
        if (!drive && !unc) return false;
        for (unsigned char ch : path) if (ch < 32 || ch == '"') return false;
        return true;
    }
    std::string hover_path_;
};
}
