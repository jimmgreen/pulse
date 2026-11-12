#pragma once
#include "../../../../sdk/native/include/pulse/diagnostics.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pulse::extensions::native {

class DiagnosticsSink {
public:
    struct Snapshot {
        PulseStatus status = PULSE_OK;
        uint64_t sequence = 0;
        std::string interface_name;
        std::string operation;
        std::string subject;
        std::string field;
        std::string message;
    };

    void Record(PulseStatus status, std::string_view interface_name, std::string_view operation,
                std::string_view subject, std::string_view field, std::string_view message) {
        if (status == PULSE_OK) return;
        Snapshot next;
        next.status = status;
        next.interface_name = BoundedCopy(interface_name, 64);
        next.operation = BoundedCopy(operation, 64);
        next.subject = BoundedCopy(subject, 256);
        next.field = BoundedCopy(field, 64);
        next.message = BoundedCopy(message, 1024);
        std::lock_guard lock(mutex_);
        next.sequence = ++next_sequence_;
        current_ = std::move(next);
    }

    // Parses formatted "<subject>[.<field>]: <reason>" strings emitted by ConvertUiModel / ParseUiTree.
    void RecordParsed(PulseStatus status, std::string_view interface_name, std::string_view operation,
                      std::string_view formatted_error, std::string_view default_subject = {}) {
        if (status == PULSE_OK) return;
        std::string_view subject = default_subject;
        std::string_view field;
        std::string_view message = formatted_error;
        const size_t colon = formatted_error.find(": ");
        if (colon != std::string_view::npos && colon > 0 && colon <= 256) {
            const std::string_view prefix = formatted_error.substr(0, colon);
            message = formatted_error.substr(colon + 2);
            const size_t dot = prefix.rfind('.');
            if (dot != std::string_view::npos && dot + 1 < prefix.size()) {
                const std::string_view tail = prefix.substr(dot + 1);
                if (KnownFieldSuffix(tail)) {
                    subject = prefix.substr(0, dot);
                    field = tail;
                } else {
                    subject = prefix;
                }
            } else if (KnownFieldSuffix(prefix)) {
                field = prefix;
            } else {
                subject = prefix;
            }
        }
        Record(status, interface_name, operation, subject, field, message);
    }

    void Clear() noexcept {
        std::lock_guard lock(mutex_);
        current_ = Snapshot{};
    }

    Snapshot Read() const {
        std::lock_guard lock(mutex_);
        return current_;
    }

private:
    static std::string BoundedCopy(std::string_view value, size_t max_bytes) {
        if (value.empty()) return {};
        const size_t n = (std::min)(value.size(), max_bytes);
        std::string out(value.data(), n);
        std::replace(out.begin(), out.end(), '\0', '?');
        return out;
    }

    static bool KnownFieldSuffix(std::string_view token) noexcept {
        static constexpr std::string_view kKnown[] = {
            "id", "type", "text", "command", "binding", "state", "orientation", "placeholder",
            "glyph", "keycap", "detail", "message", "color", "closeCommand", "enabledBinding",
            "visibleBinding", "role", "alignment", "badgeText", "status", "trailingGlyph",
            "summary", "filterText", "filterCommand", "sortCommand", "modified", "fileType",
            "fileSize", "helper", "countLabel", "actionText", "centerText", "rightText",
            "kind", "checked", "enabled", "visible", "iconOnly", "bordered", "selected",
            "current", "separator", "dot", "moveIntent", "expanded", "cut", "missing",
            "ready", "value", "columns", "gap", "minHeight", "minWidth", "maxWidth",
            "maxHeight", "flex", "stops", "itemCount", "capacity", "freeBytes", "totalBytes",
            "options", "tagColors", "menu", "actions", "rows", "data_columns", "validation",
            "data_source", "children", "commands", "states", "updates", "validate", "key",
            "size", "shortcut", "cells", "width", "height", "required", "minLength", "maxLength",
            "data_operation", "title", "heading", "subject", "subject_detail", "footnote",
            "icon", "footnote_icon", "subject_kind", "buttons", "default_button", "cancel_button",
            "ok_label", "initial_path", "default_name", "default_extension", "filters",
            "label", "pattern", "initial_query", "items", "flags", "duration_ms",
            "path", "left_path", "right_path", "mode", "sources"
        };
        for (const auto known : kKnown) {
            if (token == known) return true;
        }
        return false;
    }

    mutable std::mutex mutex_;
    uint64_t next_sequence_ = 0;
    Snapshot current_{};
};

class DiagnosticsAdapter {
public:
    DiagnosticsAdapter(std::shared_ptr<DiagnosticsSink> sink, std::function<bool()> alive);
    ~DiagnosticsAdapter();
    DiagnosticsAdapter(const DiagnosticsAdapter&) = delete;
    DiagnosticsAdapter& operator=(const DiagnosticsAdapter&) = delete;

    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
    const std::shared_ptr<DiagnosticsSink>& Sink() const noexcept { return sink_; }

private:
    struct State;
    std::shared_ptr<DiagnosticsSink> sink_;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
