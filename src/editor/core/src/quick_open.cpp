#include "mce/editor/quick_open.h"

namespace mce::editor {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_separator(char c) { return c == '/' || c == '_' || c == '-' || c == ' ' || c == '.'; }

constexpr i32 kBase = 100;  // keeps every match above the -1 of "no match"
constexpr i32 kLengthCap = 64;

}  // namespace

const char* quick_open_kind_name(QuickOpenKind kind) {
    switch (kind) {
        case QuickOpenKind::Asset:
            return "Asset";
        case QuickOpenKind::Entity:
            return "Entity";
        case QuickOpenKind::Panel:
            return "Panel";
        case QuickOpenKind::Command:
            return "Command";
    }
    return "";
}

void QuickOpen::add(QuickOpenKind kind, StringView label, u64 id) {
    QuickOpenItem item;
    item.kind = kind;
    item.label = label;
    item.id = id;
    items_.push_back(std::move(item));
}

i32 QuickOpen::score(StringView query, StringView label) {
    if (query.empty()) return kBase;
    i32 s = kBase;
    isize last = -2;
    usize at = 0;
    for (const char qc : query) {
        const char q = lower(qc);
        while (at < label.size() && lower(label[at]) != q) ++at;
        if (at == label.size()) return -1;
        s += 1;
        if (at == 0 || is_separator(label[at - 1]) || (is_upper(label[at]) && is_lower(label[at - 1]))) s += 16;
        if (static_cast<isize>(at) == last + 1) s += 8;
        last = static_cast<isize>(at);
        ++at;
    }
    const i32 len = label.size() > static_cast<usize>(kLengthCap) ? kLengthCap : static_cast<i32>(label.size());
    return s - len / 4;
}

void QuickOpen::search(StringView query, u32 limit, Array<u32>& out) const {
    out.clear();
    if (limit == 0) return;
    Array<i32> scores;  // scores[k] belongs to out[k]; both stay sorted best first
    for (u32 i = 0; i < items_.size(); ++i) {
        const i32 s = score(query, items_[i].label.view());
        if (s < 0) continue;
        if (out.size() == limit && s <= scores[out.size() - 1]) continue;  // not better than the worst kept
        out.push_back(i);
        scores.push_back(s);
        // An earlier item wins a tie, so the new one only moves past strictly lower scores.
        for (usize k = out.size() - 1; k > 0 && scores[k - 1] < scores[k]; --k) {
            const u32 oi = out[k - 1];
            out[k - 1] = out[k];
            out[k] = oi;
            const i32 os = scores[k - 1];
            scores[k - 1] = scores[k];
            scores[k] = os;
        }
        if (out.size() > limit) {
            out.pop_back();
            scores.pop_back();
        }
    }
}

}  // namespace mce::editor
