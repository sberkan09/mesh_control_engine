#pragma once
// Feeds a UI document's Minimap widgets from the world (M-MISSION step 3b). The widget reads no world: each
// frame the host gives it the tracked entity's place (kit::map_tracked_view, by the widget's `track` name) and
// the shown map markers (kit::collect_map_markers). A document without Minimap widgets costs nothing: bind()
// finds them once when the document is built, and feed() returns at once when there are none.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/map_markers.h"
#include "mce/ui/ui.h"

namespace mce::player {

// The widget input for one Minimap: its tracked view (kept at the origin when the tracked entity is missing)
// and the markers, copied in the world's marker order.
void build_minimap_feed(World& world, const ui::Minimap& minimap, Span<const kit::MapMarkerView> shown, ui::MinimapView& view,
                        Array<ui::MinimapMarker>& markers);

class MinimapFeed {
public:
    // Finds the document's Minimap widgets (call once after it is built).
    void bind(const ui::Document& doc);
    bool empty() const { return widgets_.empty(); }
    // Every Minimap of `doc`: set_minimap_view with the world's markers and the tracked entity's view.
    void feed(World& world, ui::Document& doc);

private:
    Array<ui::WidgetId> widgets_;
    Array<kit::MapMarkerView> shown_;     // scratch, reused every frame
    Array<ui::MinimapMarker> markers_;
};

}  // namespace mce::player
