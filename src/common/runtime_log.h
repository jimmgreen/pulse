#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace pulse::diagnostics::runtime {
// Event/field names must be fixed identifiers, never user data.
struct Field { const char* name; uint64_t value; };
struct TextField { const char* name; std::wstring_view value; };
inline constexpr size_t kMaxTextFieldLength = 2048;
enum class Level { Info, Warning, Error };
struct Health {
    bool enabled = false;
    uint32_t initialization_error = 0, last_write_error = 0;
    uint64_t write_errors = 0, dropped_events = 0, pending_events = 0;
    uint64_t log_opens = 0, log_writes = 0, critical_writes = 0;
    const char* last_write_operation = "";
};
struct Options {
    uint64_t rotate_bytes = 2 * 1024 * 1024;
    uint32_t heartbeat_ms = 60000;
};
bool Initialize(const std::wstring& data_root, const char* component, Options options = {}) noexcept;
bool Enabled() noexcept;
void Event(const char* name, std::initializer_list<Field> fields = {}, Level level = Level::Info) noexcept;
// Explicit diagnostic paths and fixed reason strings only; no search terms or document content.
// Truncation emits top-level text_truncated:true (outside caller data fields).
// Up to four text fields; values are copied synchronously and truncated to 2048 UTF-16 units.
void TextEvent(const char* name, std::initializer_list<TextField> text_fields,
    std::initializer_list<Field> fields = {}, Level level = Level::Info) noexcept;
Health GetHealth() noexcept;
// Waits for events accepted before the call. Use only on background threads.
// Returns false on timeout or any write failure in the current session.
bool Flush(uint32_t timeout_ms = 2000) noexcept;
uint64_t NextId() noexcept;
void Shutdown() noexcept;
}
