#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace pulse::dialog_history {
enum class State : uint32_t { Unknown=0, Fallback=1, Failure=2, Success=3 };
enum class Reason : uint32_t { Unknown=0, UnsupportedRequest=1, ProviderUnavailable=2, Timeout=3,
    PermissionDenied=4, PermissionUndiagnosable=5, ComponentMismatch=6, AutomaticDisabled=7,
    RegistrationConflict=8, InitialFolderUnsupported=9, ProviderFailure=10, NativeFallback=11, Succeeded=12, ShownClosed=13, AdminAutoEntryFailed=14 };
enum class Action : uint32_t { None=0, RestartApplication=1, CheckIntegrationSettings=2,
    RetryDialog=3, UseNativeDialog=4, ExportDiagnostics=5 };
enum class Certainty : uint32_t { Incomplete=0, Observed=1 };
enum class Operation : uint32_t { Unknown=0, Open=1, Save=2 };
struct Record {
    std::wstring app_basename;
    uint64_t app_image_id=0; // Revision-2 opaque application-path fingerprint, not binary attestation.
    State state=State::Unknown;
    Reason reason=Reason::Unknown;
    Action action=Action::ExportDiagnostics;
    Certainty certainty=Certainty::Incomplete;
    Operation operation=Operation::Unknown;
    std::wstring first_utc,last_utc; // ISO UTC, never collection time.
    uint64_t count=0;
    bool resolved_by_success=false,later_success=false;
    std::string last_producer;
    uint64_t last_sequence=0;
};
struct Cursor { std::string producer; uint64_t sequence=0; std::wstring last_utc; };
struct Identity {
    std::string producer,source,session;
    uint32_t pid=0;
    uint64_t creation_time=0,image_id=0;
    std::wstring basename;
    bool ambiguous=false;
};
struct History {
    std::vector<Record> records;
    std::vector<Cursor> cursors;
    std::vector<Identity> identities;
    uint64_t unattributed_events=0;
    std::wstring ignore_before_utc;
    bool partial=false,retention_limited=false,recovered_backup=false;
};
struct CollectionResult { DWORD error=0; uint32_t added=0,invalid=0; bool cancelled=false,partial=false; };
// Pure, bounded reducer for local-only collector output; no process snapshot joins.
CollectionResult ApplyLocalExcerpt(History&,std::string_view jsonl);
// Only the fixed-name bounded collector is used. At most four already-verified
// managed session directories; no recursion and no filename supplied by log data.
CollectionResult CollectTakeoverHistory(const std::wstring& runtime_root,
    const std::vector<std::wstring>& verified_managed_session_dirs,History&,
    const std::function<bool()>& cancelled={});
// Fixed takeover-history.json plus one backup, <=512KiB, <=128 records.
// No implicit directory creation. The controller supplies its own state directory.
DWORD LoadHistory(const std::wstring& state_directory,History&) noexcept;
DWORD SaveHistory(const std::wstring& state_directory,const History&) noexcept;
}
