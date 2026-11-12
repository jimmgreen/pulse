#pragma once
#include "../common/dialog_auto_registration.h"
#include <functional>
#include <string>
#include <vector>
#include <array>
namespace pulse::setup {
struct DialogMigrationApi {
    std::function<dialog_auto::Status()> inspect;
    std::function<DWORD(const std::wstring&)> enable;
    std::function<dialog_auto::Status(dialog_auto::Architecture)> inspect_arch;
    std::function<DWORD(dialog_auto::Architecture, const std::wstring&)> enable_arch;
};
DialogMigrationApi SystemDialogMigrationApi();
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path);
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path, dialog_auto::Architecture architecture);
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable);
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable, dialog_auto::Architecture architecture);
std::vector<std::wstring> RetiredRuntimeFiles(const std::vector<std::wstring>& shipped);
std::vector<std::wstring> LegacyDialogFiles(const std::wstring& app,
    const std::vector<std::wstring>& shipped = {});
// The marker lives beside the file transaction journal; recovery runs after files restore.
class DialogLayoutMigration {
public:
    DialogLayoutMigration(std::wstring app, bool real, DialogMigrationApi api = SystemDialogMigrationApi());
    ~DialogLayoutMigration();
    bool Prepare(std::wstring& error);
    // Relative immutable version directory, validated by setup before mutation.
    void SetVersionDirectory(const std::wstring& relative);
    void SetCompanionVersionDirectory(dialog_auto::Architecture architecture, const std::wstring& relative);
    bool Apply(std::wstring& error);
    bool Recover(std::wstring& error);
    // After the transaction finished: lets other processes read the registration
    // (dialog_auto::Inspect takes the same mutex), e.g. the admin helper setup starts.
    void ReleaseLock();
private:
    bool Lock(std::wstring& error);
    struct Registration {
        dialog_auto::Architecture architecture = dialog_auto::Architecture::X64;
        std::wstring old_path, next_path;
        bool configured = false, versioned = false;
        unsigned stage = 0;
        LONGLONG phase_offset = 0;
    };
    dialog_auto::Status Inspect(dialog_auto::Architecture architecture) const;
    DWORD Enable(dialog_auto::Architecture architecture, const std::wstring& path) const;
    bool SetPhase(const Registration& registration, unsigned phase, std::wstring& error) const;
    bool ReadJournal(std::vector<Registration>& registrations, bool& legacy, std::wstring& error) const;
    std::wstring app_, marker_;
    std::array<Registration, 2> registrations_;
    bool valid_plan_ = true;
    bool real_ = false, prepared_ = false;
    HANDLE mutex_ = nullptr;
    DialogMigrationApi api_;
};
}
