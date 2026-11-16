#include "mce/editor/thumb_cache.h"

#include <algorithm>

#include "mce/cook/fs.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/project.h"
#include "mce/hash.h"

namespace mce::editor {

u64 thumb_stamp(EditorProject& project, const ProjectAsset& asset) {
    const cook::String path = project.asset_path(asset.path.view());
    u64 size = 0;
    i64 mtime = 0;
    if (!cook::fs::stat(path.view(), size, mtime)) return 0;
    u64 h = fnv1a64(&size, sizeof(size));
    h = fnv1a64(&mtime, sizeof(mtime), h);
    if (asset.type == asset::ChunkType::Mesh) {
        const u8 mounted = project.mounted() != nullptr ? 1 : 0;
        h = fnv1a64(&mounted, sizeof(mounted), h);
    }
    return h == 0 ? 1 : h;
}

void ThumbCache::begin_frame(u32 budget) {
    ++frame_;
    budget_ = budget;
}

ThumbCache::Entry* ThumbCache::find(asset::Guid g) {
    for (Entry& e : entries_)
        if (e.guid == g) return &e;
    return nullptr;
}

const ThumbCache::Entry* ThumbCache::get(EditorProject& project, const ProjectAsset& asset, u32 size) {
    size = std::clamp(size, kThumbMin, kThumbMax);
    if (size != size_px_) {
        entries_.clear();
        size_px_ = size;
    }
    Entry* e = find(asset.guid);
    if (!e) {
        Entry fresh;
        fresh.guid = asset.guid;
        entries_.push_back(static_cast<Entry&&>(fresh));
        e = &entries_[entries_.size() - 1];
    }
    e->used = frame_;
    bool want = e->revision == 0;
    if (!want && frame_ >= e->checked + kRecheckFrames) {
        e->checked = frame_;
        want = thumb_stamp(project, asset) != e->stamp;
    }
    if (want && budget_ > 0) {
        --budget_;
        ++renders_;
        e->stamp = thumb_stamp(project, asset);
        e->checked = frame_;
        e->real = render_thumbnail(project, asset, size, e->image);
        ++e->revision;
    }
    return e->revision == 0 ? nullptr : e;
}

void ThumbCache::trim() {
    while (entries_.size() > kMaxEntries) {
        usize oldest = 0;
        for (usize i = 1; i < entries_.size(); ++i)
            if (entries_[i].used < entries_[oldest].used) oldest = i;
        entries_.erase(oldest);
    }
}

void ThumbCache::drop_missing(const EditorProject& project, Array<asset::Guid>& gone) {
    for (usize i = entries_.size(); i-- > 0;) {
        if (project.find(entries_[i].guid)) continue;
        gone.push_back(entries_[i].guid);
        entries_.erase(i);
    }
}

}  // namespace mce::editor
