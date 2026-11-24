// Generated from app_strings.tsv by pulse_ext strings. Do not edit; edit app_strings.tsv instead.
// source sha256: 137f9dd30691a5d83866c71e0bfa2b08a73cdb9d5cc91a55a2b7b9d75f5f1b26
#pragma once
#include <pulse/app.hpp>
#include <string_view>

namespace app_strings {
// 你好
inline ::pulse::sdk::LocalText Hello() { return ::pulse::sdk::Tr("\344\275\240\345\245\275", "\345\246\263\345\245\275", "Hello"); }
// 已移动 {0} 个文件到 {1}
inline ::pulse::sdk::LocalText Moved(std::string_view a0, std::string_view a1) {
    return ::pulse::sdk::Tr(::pulse::sdk::Format("\345\267\262\347\247\273\345\212\250 {0} \344\270\252\346\226\207\344\273\266\345\210\260 {1}", {a0, a1}),
                            ::pulse::sdk::Format("\345\267\262\347\247\273\345\213\225 {0} \345\200\213\346\252\224\346\241\210\345\210\260 {1}", {a0, a1}),
                            ::pulse::sdk::Format("Moved {0} files to {1}", {a0, a1}));
}
// 仅中文
inline ::pulse::sdk::LocalText OnlyChinese() { return ::pulse::sdk::Tr("\344\273\205\344\270\255\346\226\207", "", ""); }
// 引号"、反斜杠＼、换行 、问号??=
inline ::pulse::sdk::LocalText Tricky() { return ::pulse::sdk::Tr("\345\274\225\345\217\267\"\343\200\201\345\217\215\346\226\234\346\235\240\\\343\200\201\346\215\242\350\241\214\n\343\200\201\351\227\256\345\217\267\?\?=", "", "quote\" back\\slash\tTab\?\?="); }
}  // namespace app_strings
