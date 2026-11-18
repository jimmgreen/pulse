// toolbar_pin_drag.h — the toolbar while the 「自定义工具栏」 popover is open
// (toolbar picker T3c). Set by the app, read by the toolbar drawing, per
// renderer.
#pragma once

namespace pulse::ui {

struct ToolbarPinDragState {
    int view = -1;             // sort/filter/group drag, independent of command pins
    int index = -1;            // pin being dragged (the toolbar's pins: tools, then apps); -1 none
    float left = 0.0f;         // its left edge, px
    float dy = 0.0f;           // px below its row
    bool out = false;          // let go here, it leaves the toolbar
    int preview_command = 0;   // a tool hovered in the picker: drawn as a dashed preview pin
    int linked = -1;           // pin of the picker's hovered (or dragged) pinned tile: a pulsing ring
};

void SetToolbarPinDrag(const void* renderer, const ToolbarPinDragState& state);
// Files were dropped on pinned app `app` (index into the apps): its icon
// gulps (shrinks to 0.82 and springs back, 340 ms).
void GulpToolbarApp(const void* renderer, int app);

} // namespace pulse::ui
