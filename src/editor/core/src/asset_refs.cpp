#include "mce/editor/asset_refs.h"

#include "mce/cook/fs.h"

namespace mce::editor {
namespace {

constexpr u64 kMaxScanBytes = 8u << 20;  // as references_to: a larger file is skipped

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// A hyphenated uuid starts at text[at].
bool uuid_at(StringView text, usize at) {
    if (at + 36 > text.size()) return false;
    for (usize i = 0; i < 36; ++i) {
        const char c = text[at + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!is_hex(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

void AssetRefs::build(const EditorProject& project) {
    nodes_.clear();
    index_.clear();
    for (const ProjectAsset& a : project.assets()) {
        Node n;
        n.path = a.path;
        n.type = a.type;
        index_.insert_or_assign(a.guid, static_cast<u32>(nodes_.size()));
        nodes_.push_back(std::move(n));
    }
    u32 i = 0;
    for (const ProjectAsset& a : project.assets()) {
        const u32 self = i++;
        const cook::String abs = project.asset_path(a.path.view());
        u64 size = 0;
        i64 mtime = 0;
        if (!cook::fs::stat(abs.view(), size, mtime) || size > kMaxScanBytes) continue;
        cook::String text;
        if (!cook::fs::read_text(abs.view(), text)) continue;
        const StringView t = text.view();
        for (usize at = 0; at + 36 <= t.size(); ++at) {
            if (!uuid_at(t, at)) continue;
            Result<asset::Guid> g = asset::parse_guid(t.substr(at, 36));
            at += 35;
            if (!g) continue;
            const u32* target = index_.find(g.value());
            if (!target || *target == self) continue;
            Array<u32>& users = nodes_[*target].users;
            if (users.empty() || users[users.size() - 1] != self) users.push_back(self);
        }
    }
}

void AssetRefs::used_by(asset::Guid guid, Array<cook::String>& out) const {
    const u32* at = index_.find(guid);
    if (!at) return;
    for (const u32 u : nodes_[*at].users) out.push_back(nodes_[u].path);
}

u32 AssetRefs::use_count(asset::Guid guid) const {
    const u32* at = index_.find(guid);
    return at ? static_cast<u32>(nodes_[*at].users.size()) : 0;
}

void AssetRefs::unused(Array<asset::Guid>& out) const {
    for (const FlatMap<asset::Guid, u32>::Entry& e : index_) {
        const Node& n = nodes_[e.value];
        if (n.type == asset::ChunkType::Scene || !n.users.empty()) continue;
        out.push_back(e.key);
    }
}

}  // namespace mce::editor
