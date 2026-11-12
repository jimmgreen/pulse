#pragma once
#include <cstdint>
#include <string>
namespace pulse::preview {
bool MakePresentationDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                              bool& truncated);
}
