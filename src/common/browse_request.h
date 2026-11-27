#pragma once
#include <string>

namespace pulse::browse {
enum class Intent { OpenFolder, Reveal };
struct Request {
    Intent intent = Intent::OpenFolder;
    std::wstring path;
};
enum class Match { Unrelated, Supported, Unsupported };
}
