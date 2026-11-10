#pragma once
#include "media_props.h"

namespace pulse::app::media {
// Worker-only fallback; reads at most 4 MiB of container headers, never decodes.
void ReadMatroskaProps(const std::wstring& path, Props& props);
}
