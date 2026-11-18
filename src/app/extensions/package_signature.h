#pragma once
#include <string>
#include <string_view>
namespace pulse::extensions {
// Empty policy allows unsigned development packages. A supplied signature always
// requires an explicitly pinned publisher; signature identity grants no capability.
bool VerifyPackageSignature(std::string_view manifest, std::string_view signature,
    std::string_view policy, std::string& error);
}
