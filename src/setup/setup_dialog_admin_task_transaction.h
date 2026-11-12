#pragma once
#include "../common/dialog_admin_auto_task.h"
#include <functional>
#include <string>
namespace pulse::setup {
struct DialogAdminTaskApi {
    std::function<HRESULT(dialog_admin_auto::Snapshot&)> capture;
    std::function<HRESULT(const dialog_admin_auto::Snapshot&)> restore;
};
class DialogAdminTaskTransaction {
public:
    DialogAdminTaskTransaction(std::wstring root,bool real,DialogAdminTaskApi api={})
        :root_(std::move(root)),real_(real),api_(std::move(api)){}
    bool Prepare(std::wstring& error);
    bool Recover(std::wstring& error);
private:
    std::wstring root_;
    bool real_;
    DialogAdminTaskApi api_;
};
}
