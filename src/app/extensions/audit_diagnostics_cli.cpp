#include "audit_diagnostics_cli.h"
#include "audit_persistence.h"
#include <windows.h>
#include <cwchar>
namespace pulse::extensions {
std::optional<int> HandleAuditDiagnosticsCli(int argc, wchar_t** argv) {
    bool requested = false;
    for (int i = 1; i < argc; ++i)
        requested |= std::wcscmp(argv[i], L"--extension-audit-read") == 0 ||
                     std::wcscmp(argv[i], L"--extension-audit-export") == 0;
    if (!requested) return std::nullopt;
    auto result = json::Value::Object();
    if (argc == 3 && std::wcscmp(argv[1], L"--extension-audit-read") == 0)
        result = ReadPersistentAudit(argv[2]);
    else if (argc == 4 && std::wcscmp(argv[1], L"--extension-audit-export") == 0)
        result = ExportPersistentAudit(argv[2], argv[3]);
    else {
        result.set("ok", false); result.set("partial", false); result.set("error", "invalid_args");
        result.set("grantsAuthority", false);
    }
    const auto bytes = json::Serialize(result) + "\n";
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) return 3;
    DWORD written = 0;
    if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) return 3;
    return result.get_bool("ok") ? 0 : 2;
}
}
