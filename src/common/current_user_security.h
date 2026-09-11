#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace pulse {

inline std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> token_user(bytes);
    const bool have_user = bytes != 0 &&
        GetTokenInformation(token, TokenUser, token_user.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!have_user) return {};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_user.data());
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return {};
    const std::wstring result(sid);
    LocalFree(sid);
    return result;
}

class CurrentUserSecurityAttributes {
public:
    CurrentUserSecurityAttributes() {
        const std::wstring sid = CurrentUserSidString();
        if (sid.empty()) return;
        const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            return;
        attributes_.lpSecurityDescriptor = descriptor_;
    }

    ~CurrentUserSecurityAttributes() {
        if (descriptor_) LocalFree(descriptor_);
    }

    CurrentUserSecurityAttributes(const CurrentUserSecurityAttributes&) = delete;
    CurrentUserSecurityAttributes& operator=(const CurrentUserSecurityAttributes&) = delete;

    explicit operator bool() const { return descriptor_ != nullptr; }
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }

private:
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};

} // namespace pulse
