#pragma once
#include "protocol.h"
#include "../common/menu_icon.h"

namespace pulse::ipc {

inline void WriteMenuIcon(PayloadWriter& writer, const MenuIcon& icon) {
    const bool valid = icon.Valid();
    writer.PutU32(valid ? icon.width : 0);
    writer.PutU32(valid ? icon.height : 0);
    writer.PutU32(valid ? static_cast<uint32_t>(icon.bgra.size()) : 0);
    if (valid) writer.PutBytes(icon.bgra);
}

inline bool ReadMenuIcon(PayloadReader& reader, MenuIcon& icon) {
    uint32_t width = 0, height = 0, bytes = 0;
    icon = {};
    if (!reader.GetU32(width) || !reader.GetU32(height) || !reader.GetU32(bytes)) return false;
    if (!width && !height && !bytes) return true;
    if (!width || !height || width > 64 || height > 64 || bytes != width * height * 4 ||
        bytes > reader.remaining()) return false;
    icon.width = width;
    icon.height = height;
    return reader.GetBytes(bytes, icon.bgra);
}

} // namespace pulse::ipc
