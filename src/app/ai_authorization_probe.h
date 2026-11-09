#pragma once
#include <string>
namespace pulse {
struct AppState;
}
namespace pulse::app::ai {
bool RunAiAuthorizationProbe(AppState &state, const std::wstring &directory);
}
