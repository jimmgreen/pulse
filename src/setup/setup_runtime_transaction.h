#pragma once
#include <functional>
#include <string>
namespace pulse::setup {
struct RuntimeRegistrationApi {
    std::function<bool(std::string &, std::wstring &)> capture;
    std::function<bool(const std::string &, std::wstring &)> restore;
};
class RuntimeRegistrationTransaction {
  public:
    RuntimeRegistrationTransaction(std::wstring app, bool real, RuntimeRegistrationApi api = {})
        : app_(std::move(app)), real_(real), api_(std::move(api)) {
    }
    bool Prepare(std::wstring &error);
    bool Recover(std::wstring &error);

  private:
    std::wstring app_;
    bool real_;
    RuntimeRegistrationApi api_;
};
} // namespace pulse::setup
