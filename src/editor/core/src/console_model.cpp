#include "mce/editor/console_model.h"

#include <algorithm>
#include <cstring>

#include "mce/assert.h"
#include "mce/convar.h"

namespace mce::editor {

ConsoleModel::ConsoleModel(usize scrollback) {
    MCE_ASSERT_MSG(scrollback > 0, "ConsoleModel needs a scrollback");
    lines_.resize(scrollback);
}

usize ConsoleModel::poll() {
    log::ConsoleLine batch[64];
    usize total = 0;
    for (;;) {
        const usize n = log::read_console(last_seq_, Span<log::ConsoleLine>(batch));
        if (n == 0) break;
        // The log ring keeps the newest kConsoleRingSize lines: a gap in seq means lines were lost.
        if (batch[0].seq > last_seq_ + 1) dropped_ += batch[0].seq - last_seq_ - 1;
        for (usize i = 0; i < n; ++i) {
            const usize cap = lines_.size();
            if (count_ < cap) {
                lines_[(head_ + count_) % cap] = batch[i];
                ++count_;
            } else {
                lines_[head_] = batch[i];
                head_ = (head_ + 1) % cap;
            }
        }
        last_seq_ = batch[n - 1].seq;
        total += n;
        if (n < std::size(batch)) break;
    }
    if (total) ++revision_;
    return total;
}

const log::ConsoleLine& ConsoleModel::line(usize i) const {
    MCE_ASSERT_MSG(i < count_, "console line out of range");
    return lines_[(head_ + i) % lines_.size()];
}

void ConsoleModel::clear() {
    head_ = 0;
    count_ = 0;
    ++revision_;
}

bool ConsoleModel::exec(StringView line) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
    history_cursor_ = -1;
    if (line.empty()) return false;

    if (history_.empty() || history_.back().view() != line) {
        if (history_.size() == kHistorySize) history_.erase(0);
        FixedString<256> entry;
        entry.append(line);
        history_.push_back(entry);
    }
    MCE_LOG_INFO("> {}", line);
    return console::execute(line);
}

StringView ConsoleModel::browse_history(i32 direction) {
    if (history_.empty()) return {};
    const i32 n = static_cast<i32>(history_.size());
    if (history_cursor_ < 0) {
        if (direction > 0) return {};
        history_cursor_ = n - 1;
    } else {
        history_cursor_ += direction;
        if (history_cursor_ < 0) history_cursor_ = 0;
        if (history_cursor_ >= n) {
            history_cursor_ = -1;
            return {};
        }
    }
    return history_[static_cast<usize>(history_cursor_)].view();
}

usize ConsoleModel::complete(StringView prefix, Array<const char*>& out) const {
    out.clear();
    Array<ConVar*> vars;
    ConVar::collect(vars);
    for (ConVar* v : vars)
        if (StringView(v->name()).starts_with(prefix)) out.push_back(v->name());
    Array<ConFunc*> funcs;
    ConFunc::collect(funcs);
    for (ConFunc* f : funcs)
        if (StringView(f->name()).starts_with(prefix)) out.push_back(f->name());
    std::sort(out.begin(), out.end(), [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    return out.size();
}

FixedString<128> ConsoleModel::complete_common(StringView prefix) const {
    FixedString<128> result;
    Array<const char*> names;
    if (complete(prefix, names) == 0) {
        result.append(prefix);
        return result;
    }
    StringView common = names[0];
    for (const char* n : names) {
        const StringView s = n;
        usize k = 0;
        while (k < common.size() && k < s.size() && common[k] == s[k]) ++k;
        common = common.substr(0, k);
    }
    result.append(common);
    return result;
}

}  // namespace mce::editor
