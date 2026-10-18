// Feeding Minimap widgets in the stock player (M-MISSION step 3b; minimap_feed.h).

#include "minimap_feed.h"

namespace mce::player {

void build_minimap_feed(World& world, const ui::Minimap& minimap, Span<const kit::MapMarkerView> shown, ui::MinimapView& view,
                        Array<ui::MinimapMarker>& markers) {
    view = ui::MinimapView{};
    math::vec2 pos;
    f32 heading = 0.0f;
    if (kit::map_tracked_view(world, minimap.track.view(), pos, heading)) {
        view.tracked = pos;
        view.heading = heading;
    }
    markers.clear();
    for (const kit::MapMarkerView& m : shown) {
        ui::MinimapMarker out;
        out.pos = m.pos;
        out.color = m.color;
        out.icon = m.icon;
        out.layer = m.layer;
        out.always_visible = m.always_visible;
        markers.push_back(out);
    }
}

void MinimapFeed::bind(const ui::Document& doc) {
    widgets_.clear();
    Array<ui::WidgetId> stack;
    stack.push_back(doc.root());
    while (!stack.empty()) {
        const ui::WidgetId w = stack.back();
        stack.pop_back();
        if (doc.kind(w) == ui::WidgetKind::Minimap) widgets_.push_back(w);
        for (ui::WidgetId c = doc.first_child(w); doc.valid(c); c = doc.next_sibling(c)) stack.push_back(c);
    }
}

void MinimapFeed::feed(World& world, ui::Document& doc) {
    if (widgets_.empty()) return;
    kit::collect_map_markers(world, shown_);
    for (const ui::WidgetId w : widgets_) {
        if (!doc.valid(w)) continue;
        ui::MinimapView view;
        build_minimap_feed(world, doc.minimap(w), shown_.span(), view, markers_);
        doc.set_minimap_view(w, view, markers_.span());
    }
}

}  // namespace mce::player
