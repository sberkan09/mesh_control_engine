#include "dynamic_tree.h"

#include "mce/assert.h"

namespace mce::physics::detail {

namespace {
f32 perimeter(const math::aabb2& b) { return 2.0f * ((b.max.x - b.min.x) + (b.max.y - b.min.y)); }
bool contains(const math::aabb2& outer, const math::aabb2& inner) {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && inner.max.x <= outer.max.x &&
           inner.max.y <= outer.max.y;
}
i32 imax(i32 a, i32 b) { return a > b ? a : b; }
}  // namespace

i32 DynamicTree::allocate_node() {
    if (free_list_ == kNull) {
        nodes_.push_back(Node{});
        const i32 id = static_cast<i32>(nodes_.size() - 1);
        nodes_[static_cast<usize>(id)].height = 0;
        return id;
    }
    const i32 id = free_list_;
    Node& n = nodes_[static_cast<usize>(id)];
    free_list_ = n.parent;
    n = Node{};
    n.height = 0;
    return id;
}

void DynamicTree::free_node(i32 id) {
    Node& n = nodes_[static_cast<usize>(id)];
    n = Node{};
    n.parent = free_list_;
    n.height = -1;
    free_list_ = id;
}

i32 DynamicTree::create_proxy(const math::aabb2& fat, u32 user) {
    const i32 id = allocate_node();
    Node& n = nodes_[static_cast<usize>(id)];
    n.box = fat;
    n.user = user;
    n.height = 0;
    insert_leaf(id);
    ++proxies_;
    return id;
}

void DynamicTree::destroy_proxy(i32 id) {
    MCE_ASSERT(id >= 0 && static_cast<usize>(id) < nodes_.size() && nodes_[static_cast<usize>(id)].is_leaf());
    remove_leaf(id);
    free_node(id);
    --proxies_;
}

void DynamicTree::move_proxy(i32 id, const math::aabb2& fat) {
    if (contains(nodes_[static_cast<usize>(id)].box, fat) && contains(fat, nodes_[static_cast<usize>(id)].box)) return;
    remove_leaf(id);
    nodes_[static_cast<usize>(id)].box = fat;
    insert_leaf(id);
}

void DynamicTree::insert_leaf(i32 leaf) {
    if (root_ == kNull) {
        root_ = leaf;
        nodes_[static_cast<usize>(leaf)].parent = kNull;
        return;
    }
    // Find the best sibling (branch-and-descend by surface-area cost, as in Box2D 2.x).
    const math::aabb2 leaf_box = nodes_[static_cast<usize>(leaf)].box;
    i32 index = root_;
    while (!nodes_[static_cast<usize>(index)].is_leaf()) {
        const Node& n = nodes_[static_cast<usize>(index)];
        const i32 c1 = n.child1, c2 = n.child2;
        const f32 area = perimeter(n.box);
        const f32 combined = perimeter(math::merge(n.box, leaf_box));
        const f32 cost = 2.0f * combined;
        const f32 inheritance = 2.0f * (combined - area);
        auto child_cost = [&](i32 c) {
            const Node& cn = nodes_[static_cast<usize>(c)];
            const math::aabb2 m = math::merge(leaf_box, cn.box);
            return cn.is_leaf() ? perimeter(m) + inheritance : (perimeter(m) - perimeter(cn.box)) + inheritance;
        };
        const f32 cost1 = child_cost(c1);
        const f32 cost2 = child_cost(c2);
        if (cost < cost1 && cost < cost2) break;
        index = cost1 < cost2 ? c1 : c2;
    }
    const i32 sibling = index;

    const i32 old_parent = nodes_[static_cast<usize>(sibling)].parent;
    const i32 new_parent = allocate_node();
    {
        Node& np = nodes_[static_cast<usize>(new_parent)];
        np.parent = old_parent;
        np.box = math::merge(leaf_box, nodes_[static_cast<usize>(sibling)].box);
        np.height = nodes_[static_cast<usize>(sibling)].height + 1;
        np.child1 = sibling;
        np.child2 = leaf;
    }
    if (old_parent != kNull) {
        Node& op = nodes_[static_cast<usize>(old_parent)];
        if (op.child1 == sibling) op.child1 = new_parent;
        else op.child2 = new_parent;
    } else {
        root_ = new_parent;
    }
    nodes_[static_cast<usize>(sibling)].parent = new_parent;
    nodes_[static_cast<usize>(leaf)].parent = new_parent;

    // Walk back up fixing heights and boxes.
    index = nodes_[static_cast<usize>(leaf)].parent;
    while (index != kNull) {
        index = balance(index);
        Node& n = nodes_[static_cast<usize>(index)];
        const Node& c1 = nodes_[static_cast<usize>(n.child1)];
        const Node& c2 = nodes_[static_cast<usize>(n.child2)];
        n.height = 1 + imax(c1.height, c2.height);
        n.box = math::merge(c1.box, c2.box);
        index = n.parent;
    }
}

void DynamicTree::remove_leaf(i32 leaf) {
    if (leaf == root_) {
        root_ = kNull;
        return;
    }
    const i32 parent = nodes_[static_cast<usize>(leaf)].parent;
    const i32 grand = nodes_[static_cast<usize>(parent)].parent;
    const i32 sibling = nodes_[static_cast<usize>(parent)].child1 == leaf ? nodes_[static_cast<usize>(parent)].child2
                                                                          : nodes_[static_cast<usize>(parent)].child1;
    if (grand != kNull) {
        Node& g = nodes_[static_cast<usize>(grand)];
        if (g.child1 == parent) g.child1 = sibling;
        else g.child2 = sibling;
        nodes_[static_cast<usize>(sibling)].parent = grand;
        free_node(parent);
        i32 index = grand;
        while (index != kNull) {
            index = balance(index);
            Node& n = nodes_[static_cast<usize>(index)];
            const Node& c1 = nodes_[static_cast<usize>(n.child1)];
            const Node& c2 = nodes_[static_cast<usize>(n.child2)];
            n.box = math::merge(c1.box, c2.box);
            n.height = 1 + imax(c1.height, c2.height);
            index = n.parent;
        }
    } else {
        root_ = sibling;
        nodes_[static_cast<usize>(sibling)].parent = kNull;
        free_node(parent);
    }
    nodes_[static_cast<usize>(leaf)].parent = kNull;
}

// Left/right rotation if A is imbalanced. Returns the new subtree root.
i32 DynamicTree::balance(i32 ia) {
    Node* A = &nodes_[static_cast<usize>(ia)];
    if (A->is_leaf() || A->height < 2) return ia;
    const i32 ib = A->child1;
    const i32 ic = A->child2;
    Node* B = &nodes_[static_cast<usize>(ib)];
    Node* C = &nodes_[static_cast<usize>(ic)];
    const i32 bal = C->height - B->height;

    auto rotate = [&](i32 i_up, Node* up, i32 i_other, Node* other, bool up_is_child2) {
        // `up` (child of A with the larger height) becomes the subtree root.
        const i32 iF = up->child1;
        const i32 iG = up->child2;
        Node* F = &nodes_[static_cast<usize>(iF)];
        Node* G = &nodes_[static_cast<usize>(iG)];
        up->child1 = ia;
        up->parent = A->parent;
        A->parent = i_up;
        if (up->parent != kNull) {
            Node& p = nodes_[static_cast<usize>(up->parent)];
            if (p.child1 == ia) p.child1 = i_up;
            else p.child2 = i_up;
        } else {
            root_ = i_up;
        }
        const bool f_taller = F->height > G->height;
        const i32 i_keep = f_taller ? iF : iG;
        const i32 i_move = f_taller ? iG : iF;
        Node* keep = f_taller ? F : G;
        Node* move = f_taller ? G : F;
        up->child2 = i_keep;
        if (up_is_child2) A->child2 = i_move;
        else A->child1 = i_move;
        move->parent = ia;
        A->box = math::merge(other->box, move->box);
        up->box = math::merge(A->box, keep->box);
        A->height = 1 + imax(other->height, move->height);
        up->height = 1 + imax(A->height, keep->height);
        (void)i_other;
    };

    if (bal > 1) {
        rotate(ic, C, ib, B, true);
        return ic;
    }
    if (bal < -1) {
        rotate(ib, B, ic, C, false);
        return ib;
    }
    return ia;
}

bool DynamicTree::validate_node(i32 id, i32 parent) const {
    const Node& n = nodes_[static_cast<usize>(id)];
    if (n.parent != parent) return false;
    if (n.is_leaf()) return n.height == 0;
    const Node& c1 = nodes_[static_cast<usize>(n.child1)];
    const Node& c2 = nodes_[static_cast<usize>(n.child2)];
    if (n.height != 1 + imax(c1.height, c2.height)) return false;
    if (!contains(n.box, c1.box) || !contains(n.box, c2.box)) return false;
    return validate_node(n.child1, id) && validate_node(n.child2, id);
}

bool DynamicTree::validate() const { return root_ == kNull || validate_node(root_, kNull); }

}  // namespace mce::physics::detail
