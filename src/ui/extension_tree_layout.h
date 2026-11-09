#pragma once
#include "../app/extensions/ui_tree.h"
#include "fluent_components.h"
#include <functional>
#include <map>
#include <optional>
#include <vector>
namespace pulse::ui {
struct ExtensionLayoutCallbacks {
    std::function<bool(const extensions::UiNode &)> visible, enabled;
    std::function<int(const extensions::UiNode &)> selected;
    std::function<float(const extensions::UiNode &, float)> measure;
    std::function<float(const extensions::UiNode &, float)> feedback_height;
    std::function<void(const extensions::UiNode &, D2D1_RECT_F, D2D1_RECT_F, bool, bool)> place;
};
class ExtensionTreeLayout {
  public:
    struct Viewport {
        const extensions::UiNode *node{};
        D2D1_RECT_F bounds{}, clip{};
        float offset = 0, content = 0;
        bool visible = true, enabled = true;
        size_t depth = 0;
    };
    void Reset();
    float Arrange(const extensions::UiNode &root, D2D1_RECT_F viewport, float outer_offset, float scale,
                  const ExtensionLayoutCallbacks &callbacks);
    const std::map<const extensions::UiNode *, Viewport> &Viewports() const { return viewports_; }
    const extensions::UiNode *HitScrollbar(float x, float y) const;
    fluent::ScrollbarSpec Scrollbar(const extensions::UiNode *node, bool expanded = false) const;
    bool ScrollAt(float x, float y, float delta);
    bool Drag(const extensions::UiNode *node, float y, float grab);
    std::optional<D2D1_RECT_F> Bounds(const extensions::UiNode *node) const;
    bool Reveal(const extensions::UiNode *node);

  private:
    struct Placement {
        D2D1_RECT_F bounds{};
        std::vector<const extensions::UiNode *> ancestors;
    };
    std::map<const extensions::UiNode *, Viewport> viewports_;
    std::map<const extensions::UiNode *, Placement> placements_;
    float scale_ = 1;
    struct Flow {
        int columns = 1;
        std::vector<float> widths;
    };
    float ConstrainWidth(const extensions::UiNode &node, float width) const;
    float ConstrainHeight(const extensions::UiNode &node, float height) const;
    bool ChildVisible(const extensions::UiNode &node, size_t index, const ExtensionLayoutCallbacks &callbacks) const;
    Flow ChildFlow(const extensions::UiNode &node, float width, const ExtensionLayoutCallbacks &callbacks) const;
    float Measure(const extensions::UiNode &node, float width, const ExtensionLayoutCallbacks &callbacks) const;
    float Place(const extensions::UiNode &node, float x, float y, float width, float root_height, D2D1_RECT_F clip,
                bool visible, bool enabled, std::vector<const extensions::UiNode *> &ancestors,
                const ExtensionLayoutCallbacks &callbacks, bool root = false, float allocated_height = 0);
};
} // namespace pulse::ui
