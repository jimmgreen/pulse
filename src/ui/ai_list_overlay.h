// ai_list_overlay.h — Pulse 助手 in the file list (scenes 3–5): what the assistant is doing to the folder a pane shows.
//
//   reading   a spark walks the rows the assistant read; each row lights up
//             for a moment and keeps a dot; a thin progress line runs under
//             the column header and the title row says "正在读取 k/n".
//   planned   each item to be moved carries a tag naming its folder.
//   sorting   new folders open with a 新建 pill, the items slide out and a
//             card of each flies into its folder, which bumps.
//
// The agent (src/app/ai_assistant_agent.cpp) owns the overlay and changes it
// on the UI thread; the list renderer reads it while drawing. Names are kept
// lower-case (the model may not match the case on disk).
#pragma once
#include <d2d1_1.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::ui {

class Compositor;
struct Theme;

struct AiListOverlay {
    std::wstring folder;                 // full path of the folder it applies to

    // Reading. Purely time based: row j of the m read rows on screen lights
    // up at read_start + j * read_ms / m.
    uint64_t read_start = 0;             // 0: no walk
    uint64_t read_ms = 0;                // the walk's length
    size_t read_total = 0;               // items read (the n of k/n)
    std::unordered_set<std::wstring> read_names;
    std::wstring watch_text;             // "Pulse 助手正在读取 {k}/{n}"

    // Destination tags before the confirmation.
    struct Dest {
        std::wstring label;              // the folder's name
        uint8_t tint = 5;                // AiTint (Image .. Neutral); kDestAccent: a new name
    };
    std::unordered_map<std::wstring, Dest> dests;
    uint64_t dest_start = 0;             // tags fade in (70 ms apart in list order)
    uint64_t dest_end = 0;               // tags fade out (declined)

    // Sorting.
    std::wstring new_badge;              // 新建
    std::unordered_map<std::wstring, uint64_t> new_folders;   // folder name -> created
    std::unordered_map<std::wstring, uint64_t> leaving;       // item -> its move started
    std::unordered_map<std::wstring, std::wstring> target;    // item -> folder name
    std::unordered_set<std::wstring> targets;                 // every destination folder name
    // folder name -> times an item lands in it (its row bumps at each)
    std::unordered_map<std::wstring, std::vector<uint64_t>> bumps;

    uint64_t end_ms = 0;                 // nothing left to draw after this (0: open ended)
    // Rows a batch rename just named (批量重命名 scene 5): name -> flash start.
    std::unordered_map<std::wstring, uint64_t> flash;
    // AI batch rename (flow B): rows by the name they have now -> the name
    // they show until `at`, when the new one slides in (.45 s) and the row's
    // accent ring flashes (1.3 s).
    struct Morph {
        std::wstring was;
        uint64_t at = 0;
    };
    std::unordered_map<std::wstring, Morph> morph;
    static constexpr uint8_t kDestAccent = 255;
};

// The walk's k of n at `now` (1..n), the same number the chip and the step show.
size_t AiListReadK(const AiListOverlay& o, uint64_t now);
// Reading is on screen (spark, progress line, title pill).
bool AiListReading(const AiListOverlay& o, uint64_t now);
std::wstring AiListLower(const std::wstring& s);

// Drawn by the list renderer (ui_pane_list.cpp) for one pane per frame.
class AiListPainter {
public:
    AiListPainter(const AiListOverlay* overlay, Compositor* compositor, const Theme& theme, float scale,
                  uint64_t now, int pane_key);
    ~AiListPainter();
    AiListPainter(const AiListPainter&) = delete;
    AiListPainter& operator=(const AiListPainter&) = delete;

    bool On() const noexcept { return o_ != nullptr; }
    // Something moves: the caller asks for another frame.
    bool Animating() const noexcept { return animating_; }

    struct RowMotion {
        float dx = 0.0f;                 // px, sliding out to the right
        float opacity = 1.0f;
        bool gone = false;               // fully out: skip the row
        bool fly = false;                // a card of it starts flying this frame (keep its look)
    };
    // Call for every drawn row in display order before Background().
    RowMotion Row(const std::wstring& name, const D2D1_RECT_F& cell);
    // Behind the row's content: the read flash, a folder's bump tint.
    void Background(const D2D1_RECT_F& bg, float inset, float radius);
    // The icon's scale (bump 1 → 1.28 → 1, the read pop 1 → 1.3 → 1).
    float IconScale() const;
    // Over the icon: the seen dot.
    void IconDecor(const D2D1_RECT_F& icon);
    // After the name: the destination tag or the 新建 pill, inside
    // [left, right] and centred on the text line.
    void AfterName(float left, float right, float text_top, float text_h);
    // Width the row's new-name pill (batch rename) wants after the name,
    // gap included; the list takes it from the name so the pill shows.
    float AfterNameReserve() const;
    // The row's name while a batch rename lands: `was` (non-null) until its
    // turn, then the real name fading in from `dy` px below.
    struct NameMorph {
        const std::wstring* was = nullptr;
        float alpha = 1.0f;
        float dy = 0.0f;
    };
    NameMorph Morph() const noexcept { return row_morph_; }

    // After the rows, inside the list's clip: the flying cards. `content`
    // draws a row's icon and name into a rect (the card's).
    void Flights(const D2D1_RECT_F& list, const std::function<void(const std::wstring&, const D2D1_RECT_F&)>& content);
    // After the list's clip is popped: the reader spark (it rides in the
    // pane's left margin) and the progress line under the column header.
    void Margin(const D2D1_RECT_F& list);
    // The title row's pill, right-aligned at `right`, centred in [top, top+h].
    void Watch(float right, float top, float h, float min_left);

    struct State;   // per pane, kept between frames (ai_list_overlay.cpp)

private:
    const AiListOverlay* o_;
    Compositor* compositor_;
    const Theme& theme_;
    float s_;
    uint64_t now_;
    State* st_ = nullptr;
    bool animating_ = false;
    // The row being drawn.
    std::wstring row_name_;
    int row_read_ = -1;                  // its index among the read rows on screen
    uint64_t row_read_at_ = 0;           // when it lights up
    bool row_seen_ = false;
    float row_flash_ = 0.0f;             // 0..1 read flash strength
    float row_bump_ = 0.0f;              // 0..1 bump progress (0 none)
    float row_done_ = 0.0f;              // 0..1 renamed-row flash strength
    float row_ring_ = 0.0f;              // 0..1 renamed-row accent ring (flow B)
    NameMorph row_morph_;
    float row_icon_scale_ = 1.0f;
    int row_ordinal_ = 0;                // display order (for the tag stagger)
};

void ReleaseAiListOverlayResources();

} // namespace pulse::ui

namespace pulse {
// The overlay for `folder` (case-insensitive), or null. ai_assistant_agent.cpp.
std::shared_ptr<const ui::AiListOverlay> AiListOverlayFor(const std::wstring& folder);
// Flashes the rows `names` (file names in `folder`) once, 30 ms apart: the
// batch rename's "these changed". Leaves the assistant's own work alone.
void AiListFlashRows(const std::wstring& folder, const std::vector<std::wstring>& names);
} // namespace pulse
