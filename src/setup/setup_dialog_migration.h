#pragma once
#include "../common/dialog_auto_registration.h"
#include <functional>
#include <string>
#include <vector>
namespace pulse::setup {
struct DialogMigrationApi {
    std::function<dialog_auto::Status()> inspect;
    std::function<DWORD(const std::wstring&)> enable;
};
DialogMigrationApi SystemDialogMigrationApi();
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path);
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable);
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
    bool Apply(std::wstring& error);
    bool Recover(std::wstring& error);
private:
    bool Lock(std::wstring& error);
    std::wstring app_, old_, next_, marker_;
    bool versioned_ = false;
    bool real_ = false, prepared_ = false;
    HANDLE mutex_ = nullptr;
    DialogMigrationApi api_;
};
}
