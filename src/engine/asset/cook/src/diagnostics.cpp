#include "mce/cook/diagnostics.h"

namespace mce::cook {

void DiagList::add(Severity sev, Errc code, StringView file, u32 line, u32 col, const char* stage, StringView message) {
    Diagnostic d;
    d.severity = sev;
    d.code = code;
    d.file = String(file);
    d.line = line;
    d.col = col;
    d.stage = stage;
    d.message = String(message);
    items_.push_back(std::move(d));
    if (sev == Severity::Error) ++errors_;
    if (sev == Severity::Warning) ++warnings_;
}

void DiagList::append(const DiagList& other) {
    for (const Diagnostic& d : other.all()) add(d.severity, d.code, d.file, d.line, d.col, d.stage, d.message);
}

void format_diagnostic(const Diagnostic& d, String& out) {
    out += d.file;
    if (d.line) appendf(out, ":%u", d.line);
    if (d.line && d.col) appendf(out, ":%u", d.col);
    out += ": ";
    out += d.severity == Severity::Error ? "error: " : (d.severity == Severity::Warning ? "warning: " : "info: ");
    out += d.message;
    if (d.stage && *d.stage) {
        out += " [";
        out += d.stage;
        out += ']';
    }
}

}  // namespace mce::cook
