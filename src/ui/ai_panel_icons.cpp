// ai_panel_icons.cpp — SVG path data -> D2D geometry for the assistant panel.
#include "ai_panel_icons.h"
#include "svg_path_icon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <unordered_map>

namespace pulse::ui {
namespace {

struct IconPart {
    const char* d;
    uint8_t mode;      // 0 stroke, 1 fill with the icon color, 2 fill with `rgb`
    uint32_t rgb;
    float stroke;      // view-box units
    float opacity;
};

struct IconDef {
    const IconPart* parts;
    size_t count;
};

constexpr IconPart k_spark[] = {
    {"M11 3.2c.7 4.9 3.1 7.3 8 8-4.9.7-7.3 3.1-8 8-.7-4.9-3.1-7.3-8-8 4.9-.7 7.3-3.1 8-8z", 1, 0x000000u, 1.6f, 1.0f},
    {"M19 2.6c.25 1.7 1.05 2.5 2.75 2.75-1.7.25-2.5 1.05-2.75 2.75-.25-1.7-1.05-2.5-2.75-2.75 1.7-.25 2.5-1.05 2.75-2.75z", 1, 0x000000u, 1.6f, 0.75f},
};
constexpr IconPart k_folder[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_folderf[] = {
    {"M2.5 6.5A1.5 1.5 0 0 1 4 5h4.6l2 2H20a1.5 1.5 0 0 1 1.5 1.5V18a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 18z", 2, 0xE9B949u, 1.6f, 1.0f},
    {"M2.5 9.5h19V18a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 18z", 2, 0xF4CD63u, 1.6f, 1.0f},
};
constexpr IconPart k_file[] = {
    {"M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14 3v5h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_x[] = {
    {"M6 6l12 12M18 6L6 18", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_plus[] = {
    {"M12 5v14M5 12h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_dock[] = {
    {"M5 4H19A2 2 0 0 1 21 6V18A2 2 0 0 1 19 20H5A2 2 0 0 1 3 18V6A2 2 0 0 1 5 4Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 4v16", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_float[] = {
    {"M5 4H19A2 2 0 0 1 21 6V18A2 2 0 0 1 19 20H5A2 2 0 0 1 3 18V6A2 2 0 0 1 5 4Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7 10H12A1 1 0 0 1 13 11V16A1 1 0 0 1 12 17H7A1 1 0 0 1 6 16V11A1 1 0 0 1 7 10Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_chevd[] = {
    {"M6 9l6 6 6-6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_chevr[] = {
    {"M9 6l6 6-6 6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_send[] = {
    {"M12 19V5M6 11l6-6 6 6", 0, 0x000000u, 2.2f, 1.0f},
};
constexpr IconPart k_stop[] = {
    {"M9 7H15A2 2 0 0 1 17 9V15A2 2 0 0 1 15 17H9A2 2 0 0 1 7 15V9A2 2 0 0 1 9 7Z", 1, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_retry[] = {
    {"M20 11a8 8 0 1 0-2.3 5.7", 0, 0x000000u, 1.6f, 1.0f},
    {"M20 4v7h-7", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_check[] = {
    {"M5 12.5l4.2 4L19 7", 0, 0x000000u, 2.6f, 1.0f},
};
constexpr IconPart k_undo[] = {
    {"M9 14L4 9l5-5", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 9h10.5a5.5 5.5 0 0 1 0 11H11", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_open[] = {
    {"M14 4h6v6M20 4l-9 9", 0, 0x000000u, 1.6f, 1.0f},
    {"M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_clock[] = {
    {"M3.5 12A8.5 8.5 0 1 0 20.5 12A8.5 8.5 0 1 0 3.5 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 7.5V12l3 2", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_at[] = {
    {"M8.4 12A3.6 3.6 0 1 0 15.6 12A3.6 3.6 0 1 0 8.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M15.6 12v1.4a2.6 2.6 0 0 0 5.2 0V12a8.8 8.8 0 1 0-3.4 6.9", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_shield[] = {
    {"M12 3l7 3v5.2c0 4.4-3 8-7 9.8-4-1.8-7-5.4-7-9.8V6z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 12l2.2 2.2L15.5 10", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_thumb[] = {
    {"M7 10v10H4V10zM7 10l4-7a2 2 0 0 1 2 2.2V9h5.5a2 2 0 0 1 2 2.3l-1.2 7A2 2 0 0 1 17.3 20H7", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_copy[] = {
    {"M10 8H18A2 2 0 0 1 20 10V18A2 2 0 0 1 18 20H10A2 2 0 0 1 8 18V10A2 2 0 0 1 10 8Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_eye[] = {
    {"M2.5 12s3.5-6.5 9.5-6.5S21.5 12 21.5 12s-3.5 6.5-9.5 6.5S2.5 12 2.5 12z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.4 12A2.6 2.6 0 1 0 14.6 12A2.6 2.6 0 1 0 9.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_win[] = {
    {"M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M3 9h18M7 7h.01M9.5 7h.01", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_rows[] = {
    {"M8 6h12M8 12h12M8 18h12", 0, 0x000000u, 1.6f, 1.0f},
    {"M3.5 6h.01M3.5 12h.01M3.5 18h.01", 0, 0x000000u, 2.6f, 1.0f},
};
constexpr IconPart k_move[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 13.5h6M13 11l2.5 2.5L13 16", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_image[] = {
    {"M5.5 4.5H18.5A2 2 0 0 1 20.5 6.5V17.5A2 2 0 0 1 18.5 19.5H5.5A2 2 0 0 1 3.5 17.5V6.5A2 2 0 0 1 5.5 4.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7.2 10A1.8 1.8 0 1 0 10.8 10A1.8 1.8 0 1 0 7.2 10Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 18l5.5-5.5 4 4 2.5-2.5 4 4", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_video[] = {
    {"M5 6H14A2 2 0 0 1 16 8V16A2 2 0 0 1 14 18H5A2 2 0 0 1 3 16V8A2 2 0 0 1 5 6Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 10l5-3v10l-5-3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_doc[] = {
    {"M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14 3v5h5M9 13h6M9 17h6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_down[] = {
    {"M12 4v11M7 10.5l5 5 5-5M5 19.5h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_size[] = {
    {"M4 20V10M10 20V4M16 20v-7M22 20H2", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_dup[] = {
    {"M10 8H18A2 2 0 0 1 20 10V18A2 2 0 0 1 18 20H10A2 2 0 0 1 8 18V10A2 2 0 0 1 10 8Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_cam[] = {
    {"M4 8h3l1.5-2h7L17 8h3a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1z", 0, 0x000000u, 1.6f, 1.0f},
    {"M8.5 13A3.5 3.5 0 1 0 15.5 13A3.5 3.5 0 1 0 8.5 13Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_arr[] = {
    {"M5 12h12M13 7l5 5-5 5", 0, 0x000000u, 2.0f, 1.0f},
};
constexpr IconPart k_tag[] = {
    {"M3.5 12.5V4.5a1 1 0 0 1 1-1h8l8 8-9 9z", 0, 0x000000u, 1.6f, 1.0f},
    {"M6.5 8A1.5 1.5 0 1 0 9.5 8A1.5 1.5 0 1 0 6.5 8Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_folderplus[] = {
    {"M3 7.5A1.5 1.5 0 0 1 4.5 6h4l2 2h9A1.5 1.5 0 0 1 21 9.5v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 11v5M9.5 13.5h5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_search[] = {
    {"M4.5 11A6.5 6.5 0 1 0 17.5 11A6.5 6.5 0 1 0 4.5 11Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M16 16l4.5 4.5", 0, 0x000000u, 1.6f, 1.0f},
};
// 管理连接 (its 20 px boxes scaled to 24).
constexpr IconPart k_chevl[] = {
    {"M15 5.4L8.4 12l6.6 6.6", 0, 0x000000u, 1.9f, 1.0f},
};
constexpr IconPart k_okc[] = {
    {"M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", 0, 0x000000u, 1.8f, 1.0f},
    {"M8.16 12.24l2.64 2.64 5.16-5.52", 0, 0x000000u, 2.16f, 1.0f},
};
constexpr IconPart k_alert[] = {
    {"M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", 0, 0x000000u, 1.8f, 1.0f},
    {"M12 7.44v5.52M12 16.32v.12", 0, 0x000000u, 1.92f, 1.0f},
};
constexpr IconPart k_eyeoff[] = {
    {"M2.5 12s3.5-6.5 9.5-6.5S21.5 12 21.5 12s-3.5 6.5-9.5 6.5S2.5 12 2.5 12z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.4 12A2.6 2.6 0 1 0 14.6 12A2.6 2.6 0 1 0 9.4 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4.5 4.5l15 15", 0, 0x000000u, 1.6f, 1.0f},
};
// Error cards (24 px boxes).
constexpr IconPart k_key[] = {
    {"M4 15A4 4 0 1 0 12 15A4 4 0 1 0 4 15Z", 0, 0x000000u, 1.7f, 1.0f},
    {"M11 12l9-9M17 6l3 3M15 8l2 2", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_wallet[] = {
    {"M4 7.5A2.5 2.5 0 0 1 6.5 5H18v3", 0, 0x000000u, 1.7f, 1.0f},
    {"M6.5 8H17.5A2.5 2.5 0 0 1 20 10.5V16.5A2.5 2.5 0 0 1 17.5 19H6.5A2.5 2.5 0 0 1 4 16.5V10.5A2.5 2.5 0 0 1 6.5 8Z",
     0, 0x000000u, 1.7f, 1.0f},
    {"M14.8 13.5A1.2 1.2 0 1 0 17.2 13.5A1.2 1.2 0 1 0 14.8 13.5Z", 1, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_cloud[] = {
    {"M7 18.5h10a4 4 0 0 0 .6-7.95A6 6 0 0 0 6.2 9.3A4.6 4.6 0 0 0 7 18.5z", 0, 0x000000u, 1.7f, 1.0f},
    {"M12 11v3.2M12 16.6v.1", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_ban[] = {
    {"M3.5 12A8.5 8.5 0 1 0 20.5 12A8.5 8.5 0 1 0 3.5 12Z", 0, 0x000000u, 1.7f, 1.0f},
    {"M6 6l12 12", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_wifioff[] = {
    {"M3.5 9a12.5 12.5 0 0 1 17 0M6.5 12.3a8 8 0 0 1 11 0M9.5 15.5a3.6 3.6 0 0 1 5 0", 0, 0x000000u, 1.7f, 1.0f},
    {"M11.1 18.6A0.9 0.9 0 1 0 12.9 18.6A0.9 0.9 0 1 0 11.1 18.6Z", 1, 0x000000u, 1.7f, 1.0f},
    {"M4 4l16 16", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_plug[] = {
    {"M9 3v4M15 3v4M7 7h10v4a5 5 0 0 1-10 0zM12 16v5", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_gear[] = {
    {"M9 12A3 3 0 1 0 15 12A3 3 0 1 0 9 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M19.4 15a1.6 1.6 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-1.8-.3 1.6 1.6 0 0 0-1 1.5V21"
     "a2 2 0 1 1-4 0v-.1a1.6 1.6 0 0 0-1-1.5 1.6 1.6 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0 .3-1.8"
     " 1.6 1.6 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.6 1.6 0 0 0 1.5-1 1.6 1.6 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8"
     "l.1.1a1.6 1.6 0 0 0 1.8.3H9a1.6 1.6 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 1 1.5 1.6 1.6 0 0 0 1.8-.3"
     "l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.8V9a1.6 1.6 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1z",
     0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_swap[] = {
    {"M7 7h12M15 3l4 4-4 4M17 17H5M9 13l-4 4 4 4", 0, 0x000000u, 1.7f, 1.0f},
};
constexpr IconPart k_warn[] = {
    {"M12 4l9 16H3z", 0, 0x000000u, 1.7f, 1.0f},
    {"M12 10v4M12 17v.1", 0, 0x000000u, 1.8f, 1.0f},
};

// First batch of extensions (24 px boxes).
constexpr IconPart k_bolt[] = {
    {"M13 3L5 13.5h6L10 21l8-10.5h-6z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_pkg[] = {
    {"M4 7.8L12 4l8 3.8v8.4L12 20l-8-3.8z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 7.8l8 3.9 8-3.9M12 11.7V20", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_rename[] = {
    {"M4 20h4L19 9l-4-4L4 16z", 0, 0x000000u, 1.6f, 1.0f},
    {"M13 7l4 4", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_tasks[] = {
    {"M10 6h10M10 12h10M10 18h10", 0, 0x000000u, 1.6f, 1.0f},
    {"M3.5 6l1.4 1.4L7.5 4.8M3.5 12l1.4 1.4 2.6-2.6M3.5 18l1.4 1.4 2.6-2.6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_lock[] = {
    {"M7 10.5H17A2 2 0 0 1 19 12.5V18.5A2 2 0 0 1 17 20.5H7A2 2 0 0 1 5 18.5V12.5A2 2 0 0 1 7 10.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M8 10.5V8a4 4 0 0 1 8 0v2.5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_skip[] = {
    {"M5.5 6.5l8 5.5-8 5.5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M17.5 6.5v11", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_sliders[] = {
    {"M4 7h9M17 7h3M4 17h3M11 17h9", 0, 0x000000u, 1.6f, 1.0f},
    {"M13 7A2 2 0 1 0 17 7A2 2 0 1 0 13 7Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7 17A2 2 0 1 0 11 17A2 2 0 1 0 7 17Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_hdd[] = {
    {"M5.5 5h13A2.5 2.5 0 0 1 21 7.5v9a2.5 2.5 0 0 1-2.5 2.5h-13A2.5 2.5 0 0 1 3 16.5v-9A2.5 2.5 0 0 1 5.5 5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M3 14h18", 0, 0x000000u, 1.6f, 1.0f},
    {"M16.2 16.5a.8 .8 0 1 0 1.6 0a.8 .8 0 1 0-1.6 0z", 1, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_install[] = {
    {"M12 3v11M7.5 9.5L12 14l4.5-4.5", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_temp[] = {
    {"M4 7h16M6 7l1 13h10l1-13", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.5 11.5l5 5M14.5 11.5l-5 5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_trash[] = {
    {"M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3", 0, 0x000000u, 1.6f, 1.0f},
};

constexpr IconPart k_split[] = {
    {"M5.5 5h13a2 2 0 0 1 2 2v10a2 2 0 0 1 -2 2h-13a2 2 0 0 1 -2 -2v-10a2 2 0 0 1 2 -2Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 5v14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_newwin[] = {
    {"M5.5 8h8.5a2 2 0 0 1 2 2v7a2 2 0 0 1 -2 2h-8.5a2 2 0 0 1 -2 -2v-7a2 2 0 0 1 2 -2Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M8 8V6a1.5 1.5 0 0 1 1.5-1.5H19A1.5 1.5 0 0 1 20.5 6v8a1.5 1.5 0 0 1-1.5 1.5h-3", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_desktop[] = {
    {"M4.5 4.5h15a1.5 1.5 0 0 1 1.5 1.5v9a1.5 1.5 0 0 1 -1.5 1.5h-15a1.5 1.5 0 0 1 -1.5 -1.5v-9a1.5 1.5 0 0 1 1.5 -1.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 20h6M12 16.5V20", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_tray[] = {
    {"M4 13.5l2.2-7.2A1.8 1.8 0 0 1 7.9 5h8.2a1.8 1.8 0 0 1 1.7 1.3l2.2 7.2V18a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 18z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4 13.5h4.5l1 2h5l1-2H20", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_star[] = {
    {"M12 3.5l2.6 5.3 5.8.8-4.2 4.1 1 5.8L12 16.8l-5.2 2.7 1-5.8L3.6 9.6l5.8-.8z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_kbd[] = {
    {"M5 6.5h14a2 2 0 0 1 2 2v7a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-7a2 2 0 0 1 2 -2Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7 10h.01M10.3 10h.01M13.7 10h.01M17 10h.01M8 14h8", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_tiles[] = {
    {"M4.5 5h5a1.5 1.5 0 0 1 1.5 1.5v3a1.5 1.5 0 0 1 -1.5 1.5h-5a1.5 1.5 0 0 1 -1.5 -1.5v-3a1.5 1.5 0 0 1 1.5 -1.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14.5 5h5a1.5 1.5 0 0 1 1.5 1.5v3a1.5 1.5 0 0 1 -1.5 1.5h-5a1.5 1.5 0 0 1 -1.5 -1.5v-3a1.5 1.5 0 0 1 1.5 -1.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4.5 13h5a1.5 1.5 0 0 1 1.5 1.5v3a1.5 1.5 0 0 1 -1.5 1.5h-5a1.5 1.5 0 0 1 -1.5 -1.5v-3a1.5 1.5 0 0 1 1.5 -1.5Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M14.5 13h5a1.5 1.5 0 0 1 1.5 1.5v3a1.5 1.5 0 0 1 -1.5 1.5h-5a1.5 1.5 0 0 1 -1.5 -1.5v-3a1.5 1.5 0 0 1 1.5 -1.5Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_mic[] = {
    {"M12 3.5h0a3 3 0 0 1 3 3v5a3 3 0 0 1 -3 3h-0a3 3 0 0 1 -3 -3v-5a3 3 0 0 1 3 -3Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v2.5", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_monitor[] = {
    {"M5.5 4.5h13a2 2 0 0 1 2 2v7.5a2 2 0 0 1 -2 2h-13a2 2 0 0 1 -2 -2v-7.5a2 2 0 0 1 2 -2Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 20h6M12 16v4", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_winset[] = {
    {"M5.5 4h13a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-13a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M3.5 8.5h17", 0, 0x000000u, 1.6f, 1.0f},
    {"M9.7 14A2.3 2.3 0 1 0 14.3 14A2.3 2.3 0 1 0 9.7 14Z", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_rocket[] = {
    {"M12 3.5c3.4 2 5 5.4 5 9.4l-2.2 3.1H9.2L7 12.9c0-4 1.6-7.4 5-9.4z", 0, 0x000000u, 1.6f, 1.0f},
    {"M10.3 10A1.7 1.7 0 1 0 13.7 10A1.7 1.7 0 1 0 10.3 10Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M10 19.5h4", 0, 0x000000u, 1.6f, 1.0f},
};

// P4.
constexpr IconPart k_pin[] = {
    {"M9 4h6l-1 5 3 3v2H7v-2l3-3z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 14v6", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_palette[] = {
    {"M3.5 12A8.5 8.5 0 1 0 20.5 12A8.5 8.5 0 1 0 3.5 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M7.3 10A1.2 1.2 0 1 0 9.7 10A1.2 1.2 0 1 0 7.3 10Z", 1, 0x000000u, 1.6f, 1.0f},
    {"M10.8 7.8A1.2 1.2 0 1 0 13.2 7.8A1.2 1.2 0 1 0 10.8 7.8Z", 1, 0x000000u, 1.6f, 1.0f},
    {"M14.3 10A1.2 1.2 0 1 0 16.7 10A1.2 1.2 0 1 0 14.3 10Z", 1, 0x000000u, 1.6f, 1.0f},
    {"M12 20.5c-1.2 0-1.8-.9-1.4-1.9.5-1.2-.3-2.1-1.5-2.1", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_git[] = {
    {"M4.5 6A2 2 0 1 0 8.5 6A2 2 0 1 0 4.5 6Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M4.5 18A2 2 0 1 0 8.5 18A2 2 0 1 0 4.5 18Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M15.5 9A2 2 0 1 0 19.5 9A2 2 0 1 0 15.5 9Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M6.5 8v8M17.5 11c0 3-3 4-9 5", 0, 0x000000u, 1.6f, 1.0f},
};
// P4b. Push: the HTML's arrow up onto a line; pull mirrors it.
constexpr IconPart k_push[] = {
    {"M12 19V6M6.5 11.5L12 6l5.5 5.5", 0, 0x000000u, 1.6f, 1.0f},
    {"M5 20h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_pull[] = {
    {"M12 4v13M6.5 11.5L12 17l5.5-5.5", 0, 0x000000u, 1.6f, 1.0f},
    {"M5 20h14", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_eject[] = {
    {"M12 5l7 8.5H5z", 0, 0x000000u, 1.6f, 1.0f},
    {"M5 18.5h14", 0, 0x000000u, 1.6f, 1.0f},
};

constexpr IconPart k_quote[] = {
    {"M5 9.5h4.5v4.5c0 2.5-1.5 4-3.5 4.5M14.5 9.5H19v4.5c0 2.5-1.5 4-3.5 4.5", 0, 0x000000u, 1.6f, 1.0f},
    {"M5 9.5V6h4.5v3.5M14.5 9.5V6H19v3.5", 0, 0x000000u, 1.6f, 1.0f},
};

// 55 P2.
constexpr IconPart k_thumbd[] = {
    {"M8 13.5v-9H5.5a1 1 0 0 0-1 1v7a1 1 0 0 0 1 1zM8 13.5l3.5 6.5a2 2 0 0 0 2.3-2.4L13 14h5a2 2 0 0 0 2-2.3l-1-6a2 2 0 0 0-2-1.7H8", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_edit[] = {
    {"M14.5 5.5l4 4L9 19H5v-4z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12.5 7.5l4 4", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_info[] = {
    {"M4 12A8 8 0 1 0 20 12A8 8 0 1 0 4 12Z", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 11v5M12 8h.01", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_detail[] = {
    {"M5 7h14M5 12h14M5 17h9", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_short[] = {
    {"M5 9h14M5 15h8", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_model[] = {
    {"M8 5h8a3 3 0 0 1 3 3v8a3 3 0 0 1-3 3H8a3 3 0 0 1-3-3V8a3 3 0 0 1 3-3z", 0, 0x000000u, 1.6f, 1.0f},
    {"M9 9h6v6H9z", 0, 0x000000u, 1.6f, 1.0f},
};
// 55 P3: the header's 历史对话 (a clock running back) and a row's chat bubble.
constexpr IconPart k_history[] = {
    {"M4.5 12a7.5 7.5 0 1 0 2.2-5.3", 0, 0x000000u, 1.6f, 1.0f},
    {"M4.5 4.5v3.5H8", 0, 0x000000u, 1.6f, 1.0f},
    {"M12 8v4.2l2.8 1.8", 0, 0x000000u, 1.6f, 1.0f},
};
constexpr IconPart k_chat[] = {
    {"M5 6.5A2 2 0 0 1 7 4.5h10a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-6l-4 3.5v-3.5H7a2 2 0 0 1-2-2z", 0, 0x000000u, 1.6f, 1.0f},
};

constexpr IconDef kIcons[] = {
    {k_spark, std::size(k_spark)},
    {k_folder, std::size(k_folder)},
    {k_folderf, std::size(k_folderf)},
    {k_file, std::size(k_file)},
    {k_x, std::size(k_x)},
    {k_plus, std::size(k_plus)},
    {k_dock, std::size(k_dock)},
    {k_float, std::size(k_float)},
    {k_chevd, std::size(k_chevd)},
    {k_chevr, std::size(k_chevr)},
    {k_send, std::size(k_send)},
    {k_stop, std::size(k_stop)},
    {k_retry, std::size(k_retry)},
    {k_check, std::size(k_check)},
    {k_undo, std::size(k_undo)},
    {k_open, std::size(k_open)},
    {k_clock, std::size(k_clock)},
    {k_at, std::size(k_at)},
    {k_shield, std::size(k_shield)},
    {k_thumb, std::size(k_thumb)},
    {k_copy, std::size(k_copy)},
    {k_eye, std::size(k_eye)},
    {k_win, std::size(k_win)},
    {k_rows, std::size(k_rows)},
    {k_move, std::size(k_move)},
    {k_image, std::size(k_image)},
    {k_video, std::size(k_video)},
    {k_doc, std::size(k_doc)},
    {k_down, std::size(k_down)},
    {k_size, std::size(k_size)},
    {k_dup, std::size(k_dup)},
    {k_cam, std::size(k_cam)},
    {k_arr, std::size(k_arr)},
    {k_tag, std::size(k_tag)},
    {k_folderplus, std::size(k_folderplus)},
    {k_search, std::size(k_search)},
    {k_chevl, std::size(k_chevl)},
    {k_okc, std::size(k_okc)},
    {k_alert, std::size(k_alert)},
    {k_eyeoff, std::size(k_eyeoff)},
    {k_key, std::size(k_key)},
    {k_wallet, std::size(k_wallet)},
    {k_cloud, std::size(k_cloud)},
    {k_ban, std::size(k_ban)},
    {k_wifioff, std::size(k_wifioff)},
    {k_plug, std::size(k_plug)},
    {k_gear, std::size(k_gear)},
    {k_swap, std::size(k_swap)},
    {k_warn, std::size(k_warn)},
    {k_bolt, std::size(k_bolt)},
    {k_pkg, std::size(k_pkg)},
    {k_rename, std::size(k_rename)},
    {k_tasks, std::size(k_tasks)},
    {k_lock, std::size(k_lock)},
    {k_skip, std::size(k_skip)},
    {k_sliders, std::size(k_sliders)},
    {k_hdd, std::size(k_hdd)},
    {k_install, std::size(k_install)},
    {k_temp, std::size(k_temp)},
    {k_trash, std::size(k_trash)},
    {k_split, std::size(k_split)},
    {k_newwin, std::size(k_newwin)},
    {k_desktop, std::size(k_desktop)},
    {k_tray, std::size(k_tray)},
    {k_star, std::size(k_star)},
    {k_kbd, std::size(k_kbd)},
    {k_tiles, std::size(k_tiles)},
    {k_mic, std::size(k_mic)},
    {k_monitor, std::size(k_monitor)},
    {k_rocket, std::size(k_rocket)},
    {k_winset, std::size(k_winset)},
    {k_pin, std::size(k_pin)},
    {k_palette, std::size(k_palette)},
    {k_git, std::size(k_git)},
    {k_push, std::size(k_push)},
    {k_pull, std::size(k_pull)},
    {k_eject, std::size(k_eject)},
    {k_quote, std::size(k_quote)},
    {k_thumbd, std::size(k_thumbd)},
    {k_edit, std::size(k_edit)},
    {k_info, std::size(k_info)},
    {k_detail, std::size(k_detail)},
    {k_short, std::size(k_short)},
    {k_model, std::size(k_model)},
    {k_history, std::size(k_history)},
    {k_chat, std::size(k_chat)},
};

static_assert(std::size(kIcons) == static_cast<size_t>(AiIcon::Count), "one table entry per AiIcon");

// Minimal SVG path parser: M L H V C S Q Z and A, absolute and relative,
// implicit command repetition and packed numbers ("-4.9.7").
class PathParser {
public:
    PathParser(const char* d, ID2D1GeometrySink* sink) : s_(d), sink_(sink) {}

    void Run() {
        char cmd = 0;
        for (;;) {
            SkipSeparators();
            if (!*s_) break;
            if (IsCommand(*s_)) cmd = *s_++;
            else if (cmd == 'M') cmd = 'L';   // implicit lineto after moveto
            else if (cmd == 'm') cmd = 'l';
            if (!cmd) break;
            const bool rel = cmd >= 'a' && cmd <= 'z';
            switch (cmd) {
            case 'M': case 'm': {
                float x, y;
                if (!Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                Begin(x, y);
                break;
            }
            case 'L': case 'l': {
                float x, y;
                if (!Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                Line(x, y);
                break;
            }
            case 'H': case 'h': {
                float x;
                if (!Num(x)) return;
                Line(rel ? cx_ + x : x, cy_);
                break;
            }
            case 'V': case 'v': {
                float y;
                if (!Num(y)) return;
                Line(cx_, rel ? cy_ + y : y);
                break;
            }
            case 'C': case 'c': {
                float x1, y1, x2, y2, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return;
                if (rel) { x1 += cx_; y1 += cy_; x2 += cx_; y2 += cy_; x += cx_; y += cy_; }
                Cubic(x1, y1, x2, y2, x, y);
                break;
            }
            case 'S': case 's': {
                float x2, y2, x, y;
                if (!Num(x2) || !Num(y2) || !Num(x) || !Num(y)) return;
                if (rel) { x2 += cx_; y2 += cy_; x += cx_; y += cy_; }
                const bool smooth = last_ == 'C' || last_ == 'S';
                const float x1 = smooth ? 2.0f * cx_ - c2x_ : cx_;
                const float y1 = smooth ? 2.0f * cy_ - c2y_ : cy_;
                Cubic(x1, y1, x2, y2, x, y);
                break;
            }
            case 'Q': case 'q': {
                float x1, y1, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x) || !Num(y)) return;
                if (rel) { x1 += cx_; y1 += cy_; x += cx_; y += cy_; }
                EnsureFigure();
                sink_->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x, y)));
                cx_ = x; cy_ = y;
                break;
            }
            case 'A': case 'a': {
                float rx, ry, rot, large, sweep, x, y;
                if (!Num(rx) || !Num(ry) || !Num(rot) || !Flag(large) || !Flag(sweep) || !Num(x) || !Num(y)) return;
                if (rel) { x += cx_; y += cy_; }
                EnsureFigure();
                D2D1_ARC_SEGMENT arc{};
                arc.point = D2D1::Point2F(x, y);
                arc.size = D2D1::SizeF(std::fabs(rx), std::fabs(ry));
                arc.rotationAngle = rot;
                arc.sweepDirection = sweep != 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
                arc.arcSize = large != 0.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                sink_->AddArc(arc);
                cx_ = x; cy_ = y;
                break;
            }
            case 'Z': case 'z':
                if (open_) {
                    sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open_ = false;
                }
                cx_ = sx_; cy_ = sy_;
                break;
            default:
                return;
            }
            last_ = static_cast<char>(cmd & ~0x20);
        }
        if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
    }

private:
    static bool IsCommand(char c) {
        switch (c) {
        case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v':
        case 'C': case 'c': case 'S': case 's': case 'Q': case 'q': case 'A': case 'a':
        case 'Z': case 'z':
            return true;
        default:
            return false;
        }
    }
    void SkipSeparators() {
        while (*s_ == ' ' || *s_ == ',' || *s_ == '\t' || *s_ == '\n' || *s_ == '\r') ++s_;
    }
    bool Num(float& out) {
        SkipSeparators();
        const char* start = s_;
        if (*s_ == '-' || *s_ == '+') ++s_;
        bool dot = false, digits = false;
        while ((*s_ >= '0' && *s_ <= '9') || (*s_ == '.' && !dot)) {
            if (*s_ == '.') dot = true; else digits = true;
            ++s_;
        }
        if (*s_ == 'e' || *s_ == 'E') {
            ++s_;
            if (*s_ == '-' || *s_ == '+') ++s_;
            while (*s_ >= '0' && *s_ <= '9') ++s_;
        }
        if (!digits) { s_ = start; return false; }
        out = std::strtof(start, nullptr);
        return true;
    }
    bool Flag(float& out) {   // arc flags may be packed without separators: "0 01"
        SkipSeparators();
        if (*s_ != '0' && *s_ != '1') return false;
        out = *s_ == '1' ? 1.0f : 0.0f;
        ++s_;
        return true;
    }
    void Begin(float x, float y) {
        if (open_) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
        sink_->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        open_ = true;
        cx_ = sx_ = x; cy_ = sy_ = y;
    }
    void EnsureFigure() { if (!open_) Begin(cx_, cy_); }
    void Line(float x, float y) {
        EnsureFigure();
        sink_->AddLine(D2D1::Point2F(x, y));
        cx_ = x; cy_ = y;
    }
    void Cubic(float x1, float y1, float x2, float y2, float x, float y) {
        EnsureFigure();
        sink_->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y)));
        c2x_ = x2; c2y_ = y2;
        cx_ = x; cy_ = y;
    }

    const char* s_;
    ID2D1GeometrySink* sink_;
    bool open_ = false;
    char last_ = 0;
    float cx_ = 0, cy_ = 0, sx_ = 0, sy_ = 0, c2x_ = 0, c2y_ = 0;
};

constexpr size_t kMaxParts = 8;

struct Cache {
    ID2D1Factory* factory = nullptr;
    std::array<std::array<ID2D1PathGeometry*, kMaxParts>, static_cast<size_t>(AiIcon::Count)> geometry{};
    ID2D1StrokeStyle* stroke = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1DeviceContext* brush_dc = nullptr;
    std::unordered_map<const char*, ID2D1PathGeometry*> svg;

    void Release() {
        for (auto& icon : geometry)
            for (auto*& g : icon) if (g) { g->Release(); g = nullptr; }
        for (auto& kv : svg) if (kv.second) kv.second->Release();
        svg.clear();
        if (stroke) { stroke->Release(); stroke = nullptr; }
        if (brush) { brush->Release(); brush = nullptr; }
        if (brush_dc) { brush_dc->Release(); brush_dc = nullptr; }
        if (factory) { factory->Release(); factory = nullptr; }
    }
    ~Cache() { Release(); }
    uint64_t used = 0;
};

// One cache per D2D factory, so windows with their own factory (the AI panel
// host, the batch rename dialog) don't evict each other every frame.
constexpr size_t kCacheSlots = 3;
std::array<Cache, kCacheSlots>& Caches() {
    static std::array<Cache, kCacheSlots> caches;
    return caches;
}

ID2D1PathGeometry* Geometry(Cache& c, AiIcon icon, size_t part) {
    ID2D1PathGeometry*& slot = c.geometry[static_cast<size_t>(icon)][part];
    if (slot) return slot;
    ID2D1PathGeometry* path = nullptr;
    if (FAILED(c.factory->CreatePathGeometry(&path))) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) { path->Release(); return nullptr; }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    PathParser(kIcons[static_cast<size_t>(icon)].parts[part].d, sink).Run();
    const HRESULT hr = sink->Close();
    sink->Release();
    if (FAILED(hr)) { path->Release(); return nullptr; }
    slot = path;
    return slot;
}

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

} // namespace

void ReleaseAiIcons() { for (auto& c : Caches()) c.Release(); }

namespace {

// Picks the cache bound to dc's factory (claiming the least recently used
// slot for a new factory) and makes sure stroke + brush exist.
Cache* Prepare(ID2D1DeviceContext* dc, D2D1_COLOR_F color) {
    static uint64_t tick = 0;
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return nullptr;
    auto& caches = Caches();
    Cache* hit = nullptr;
    for (auto& slot : caches) if (slot.factory == factory) hit = &slot;
    if (hit) {
        factory->Release();
    } else {
        hit = &caches[0];
        for (auto& slot : caches) if (slot.used < hit->used) hit = &slot;
        hit->Release();
        hit->factory = factory;   // keeps the reference GetFactory added
    }
    Cache& c = *hit;
    c.used = ++tick;
    if (!c.stroke) {
        const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        c.factory->CreateStrokeStyle(props, nullptr, 0, &c.stroke);
    }
    if (c.brush_dc != dc) {
        if (c.brush) { c.brush->Release(); c.brush = nullptr; }
        if (c.brush_dc) { c.brush_dc->Release(); c.brush_dc = nullptr; }
        if (FAILED(dc->CreateSolidColorBrush(color, &c.brush))) return nullptr;
        c.brush_dc = dc;   // held so a recycled address can't match a dead context
        dc->AddRef();
    }
    return c.stroke ? &c : nullptr;
}

ID2D1PathGeometry* SvgGeometry(Cache& c, const char* d) {
    auto it = c.svg.find(d);
    if (it != c.svg.end()) return it->second;
    ID2D1PathGeometry* path = nullptr;
    if (FAILED(c.factory->CreatePathGeometry(&path))) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) { path->Release(); return nullptr; }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    PathParser(d, sink).Run();
    const HRESULT hr = sink->Close();
    sink->Release();
    if (FAILED(hr)) { path->Release(); return nullptr; }
    c.svg.emplace(d, path);
    return path;
}

} // namespace

void DrawSvgParts(ID2D1DeviceContext* dc, const SvgPart* parts, size_t count, float view,
                  const D2D1_RECT_F& box, D2D1_COLOR_F color) {
    if (!dc || !parts || view <= 0.0f || color.a <= 0.0f) return;
    Cache* cache = Prepare(dc, color);
    if (!cache) return;
    Cache& c = *cache;
    const float size = (std::min)(box.right - box.left, box.bottom - box.top);
    if (size <= 0.0f) return;
    const float k = size / view;
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(box.left, box.top) * saved);
    for (size_t i = 0; i < count; ++i) {
        ID2D1PathGeometry* g = SvgGeometry(c, parts[i].d);
        if (!g) continue;
        c.brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a * parts[i].opacity));
        if (parts[i].fill) dc->FillGeometry(g, c.brush);
        else dc->DrawGeometry(g, c.brush, parts[i].stroke, c.stroke);
    }
    dc->SetTransform(saved);
}

void DrawAiIcon(ID2D1DeviceContext* dc, AiIcon icon, const D2D1_RECT_F& box, D2D1_COLOR_F color,
                const AiIconPose& pose) {
    if (!dc || icon >= AiIcon::Count || pose.opacity <= 0.0f) return;
    Cache* cache = Prepare(dc, color);
    if (!cache) return;
    Cache& c = *cache;
    const float size = (std::min)(box.right - box.left, box.bottom - box.top);
    if (size <= 0.0f) return;
    const float k = size / 24.0f;
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const D2D1_POINT_2F centre = D2D1::Point2F(box.left + size * 0.5f, box.top + size * 0.5f);
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(box.left, box.top);
    if (pose.scale != 1.0f) m = m * D2D1::Matrix3x2F::Scale(pose.scale, pose.scale, centre);
    if (pose.rotate_deg != 0.0f) m = m * D2D1::Matrix3x2F::Rotation(pose.rotate_deg, centre);
    dc->SetTransform(m * saved);
    const IconDef& def = kIcons[static_cast<size_t>(icon)];
    for (size_t i = 0; i < def.count && i < kMaxParts; ++i) {
        const IconPart& part = def.parts[i];
        ID2D1PathGeometry* g = Geometry(c, icon, i);
        if (!g) continue;
        const float alpha = color.a * pose.opacity * part.opacity;
        c.brush->SetColor(part.mode == 2 ? Rgb(part.rgb, pose.opacity * part.opacity)
                                         : D2D1::ColorF(color.r, color.g, color.b, alpha));
        if (part.mode == 0) dc->DrawGeometry(g, c.brush, part.stroke, c.stroke);
        else dc->FillGeometry(g, c.brush);
    }
    dc->SetTransform(saved);
}

} // namespace pulse::ui
