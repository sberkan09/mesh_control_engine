#include "mce/editor/batch_rename.h"

namespace mce::editor {

FixedString<64> batch_rename_expand(StringView pattern, StringView old_name, u32 index, u32 start) {
    FixedString<64> out;
    usize i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c == '*') {
            out.append(old_name);
            ++i;
        } else if (c == '#') {
            usize width = 0;
            while (i < pattern.size() && pattern[i] == '#') {
                ++width;
                ++i;
            }
            char digits[16];
            usize n = 0;
            for (u32 v = start + index; n < sizeof digits - 1; v /= 10) {  // reversed
                digits[n++] = static_cast<char>('0' + v % 10);
                if (v < 10) break;
            }
            for (usize pad = n; pad < width && pad < 16; ++pad) out.append(StringView("0", 1));
            while (n > 0) out.append(StringView(&digits[--n], 1));
        } else {
            out.append(StringView(&pattern[i], 1));
            ++i;
        }
    }
    return out;
}

}  // namespace mce::editor
