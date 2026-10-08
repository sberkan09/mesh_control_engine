#pragma once
// Structured cook errors (assets.md §7.1 CookReport): {file, location, stage, message}. The same
// rows mce_cook prints as "file:line:col: error: message [stage]" and the editor's error list
// renders with jump-to-asset (FR-EDT-070).

#include "mce/cook/string.h"
#include "mce/result.h"

namespace mce::cook {

enum class Severity : u8 { Info, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Error;
    Errc code = Errc::Unknown;
    String file;        // project-relative ("assets/scenes/level.mcscene") or a tool path
    u32 line = 0;       // 1-based; 0 = whole file
    u32 col = 0;
    const char* stage = "";  // static: "cook.scan", "cook.scene", ...
    String message;
};

class DiagList {
public:
    void add(Severity sev, Errc code, StringView file, u32 line, u32 col, const char* stage, StringView message);
    void error(Errc code, StringView file, u32 line, u32 col, const char* stage, StringView message) {
        add(Severity::Error, code, file, line, col, stage, message);
    }
    void warning(StringView file, u32 line, u32 col, const char* stage, StringView message) {
        add(Severity::Warning, Errc::Ok, file, line, col, stage, message);
    }
    void append(const DiagList& other);

    Span<const Diagnostic> all() const { return items_.span(); }
    u32 error_count() const { return errors_; }
    u32 warning_count() const { return warnings_; }
    usize size() const { return items_.size(); }

private:
    Array<Diagnostic> items_;
    u32 errors_ = 0;
    u32 warnings_ = 0;
};

// "file:line:col: error: message [stage]"
void format_diagnostic(const Diagnostic& d, String& out);

}  // namespace mce::cook
