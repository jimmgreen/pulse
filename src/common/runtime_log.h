#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

namespace pulse::diagnostics::runtime {
// Only fixed event/field identifiers and numeric values are accepted. Callers
// must never encode paths, search terms, URLs or document content as identifiers.
struct Field { const char* name; uint64_t value; };
enum class Level { Info, Warning, Error };
struct Health {
    bool enabled = false;
    uint32_t initialization_error = 0, last_write_error = 0;
    uint64_t write_errors = 0, dropped_events = 0, pending_events = 0;
};
struct Options {
    uint64_t rotate_bytes = 2 * 1024 * 1024;
    uint32_t heartbeat_ms = 60000;
};
bool Initialize(const std::wstring& data_root, const char* component, Options options = {}) noexcept;
bool Enabled() noexcept;
void Event(const char* name, std::initializer_list<Field> fields = {}, Level level = Level::Info) noexcept;
Health GetHealth() noexcept;
// Waits for events accepted before the call. Use only on background threads.
// Returns false on timeout or any write failure in the current session.
bool Flush(uint32_t timeout_ms = 2000) noexcept;
uint64_t NextId() noexcept;
void Shutdown() noexcept;
}
