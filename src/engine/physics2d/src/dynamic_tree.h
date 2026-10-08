#pragma once
// Dynamic AABB tree broadphase (physics.md §3.2, D5): the Box2D 2.x dynamic-tree shape — fat leaves,
// surface-area insertion cost, AVL-style rotations on the way up. One implementation for the
// MVP and the 2,000-body v0.5 budget. Structure depends only on the call sequence, so it is
// deterministic; callers still sort query output before acting on it.

#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace mce::physics::detail {

class DynamicTree {
public:
    static constexpr i32 kNull = -1;

    i32 create_proxy(const math::aabb2& fat, u32 user);
    void destroy_proxy(i32 id);
    // Re-inserts the leaf with a new fat AABB.
    void move_proxy(i32 id, const math::aabb2& fat);
    const math::aabb2& fat_aabb(i32 id) const { return nodes_[static_cast<usize>(id)].box; }
    u32 user(i32 id) const { return nodes_[static_cast<usize>(id)].user; }
    i32 height() const { return root_ == kNull ? 0 : nodes_[static_cast<usize>(root_)].height; }
    u32 proxy_count() const { return proxies_; }

    // fn(u32 user) -> bool (false stops the walk).
    template <class Fn>
    void query(const math::aabb2& box, Fn&& fn) const {
        if (root_ == kNull) return;
        SmallVector<i32, 64> stack;
        stack.push_back(root_);
        while (!stack.empty()) {
            const i32 id = stack.back();
            stack.pop_back();
            const Node& n = nodes_[static_cast<usize>(id)];
            if (!n.box.overlaps(box)) continue;
            if (n.is_leaf()) {
                if (!fn(n.user)) return;
            } else {
                stack.push_back(n.child1);
                stack.push_back(n.child2);
            }
        }
    }

    // Segment p1 -> p2 walk. fn(u32 user, f32 max_fraction) -> f32: return the new clip fraction
    // (0 stops, the input value keeps going, a smaller value clips the ray).
    template <class Fn>
    void raycast(math::vec2 p1, math::vec2 p2, f32 max_fraction, Fn&& fn) const {
        if (root_ == kNull) return;
        const math::vec2 d = p2 - p1;
        if (math::length_sq(d) <= 0.0f) return;
        const math::vec2 r = math::normalize(d);
        const math::vec2 v{-r.y, r.x};  // perpendicular
        const math::vec2 abs_v{v.x < 0 ? -v.x : v.x, v.y < 0 ? -v.y : v.y};
        auto segment_box = [&](f32 frac) {
            const math::vec2 t = p1 + frac * d;
            return math::aabb2{math::min(p1, t), math::max(p1, t)};
        };
        math::aabb2 seg = segment_box(max_fraction);
        SmallVector<i32, 64> stack;
        stack.push_back(root_);
        while (!stack.empty()) {
            const i32 id = stack.back();
            stack.pop_back();
            const Node& n = nodes_[static_cast<usize>(id)];
            if (!n.box.overlaps(seg)) continue;
            // Separating axis |dot(v, p1 - c)| > dot(|v|, h)
            const math::vec2 c = n.box.center();
            const math::vec2 h = n.box.extents();
            const f32 sep = math::dot(v, p1 - c);
            if ((sep < 0 ? -sep : sep) - math::dot(abs_v, h) > 0.0f) continue;
            if (n.is_leaf()) {
                const f32 value = fn(n.user, max_fraction);
                if (value == 0.0f) return;
                if (value > 0.0f && value < max_fraction) {
                    max_fraction = value;
                    seg = segment_box(max_fraction);
                }
            } else {
                stack.push_back(n.child1);
                stack.push_back(n.child2);
            }
        }
    }

    // Debug/validation: walks the tree checking parent links, heights and containment.
    bool validate() const;
    template <class Fn>
    void each_node(Fn&& fn) const {  // fn(const aabb2&, bool leaf)
        if (root_ == kNull) return;
        SmallVector<i32, 64> stack;
        stack.push_back(root_);
        while (!stack.empty()) {
            const Node& n = nodes_[static_cast<usize>(stack.back())];
            stack.pop_back();
            fn(n.box, n.is_leaf());
            if (!n.is_leaf()) {
                stack.push_back(n.child1);
                stack.push_back(n.child2);
            }
        }
    }

private:
    struct Node {
        math::aabb2 box;
        u32 user = 0;
        i32 parent = kNull;  // doubles as the free-list link
        i32 child1 = kNull;
        i32 child2 = kNull;
        i32 height = -1;  // -1 = free
        bool is_leaf() const { return child1 == kNull; }
    };

    i32 allocate_node();
    void free_node(i32 id);
    void insert_leaf(i32 leaf);
    void remove_leaf(i32 leaf);
    i32 balance(i32 a);
    bool validate_node(i32 id, i32 parent) const;

    Array<Node> nodes_;
    i32 root_ = kNull;
    i32 free_list_ = kNull;
    u32 proxies_ = 0;
};

}  // namespace mce::physics::detail
