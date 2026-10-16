#pragma once
// The .mcbt source format (M-AI 1, ADR-0045): read by the `bt` importer, written by tools that edit
// a tree (the editor's Behavior Tree panel, M-AI 5; MCP's bt_* tools, M-AI 6). The format is
// documented in bt_importer.cpp and user guide 13.8.
//
// BtDocument is the source as data, kept close to the text: each node is its kind, the kind's own
// value (a leaf's task name, key, seconds or signal) and its other members as JSON text, in file
// order. Reading checks the shape only (one kind per node, children where the kind takes them);
// build_behavior_tree is the cook itself, so a tool asks it whether a document is valid and gets
// the cook's file:line:col errors. Writing gives the same text for a document read from what was
// written.
//
// Node order: nodes[0] is the root; children are indices. A tree cooks to one node per document
// node, in pre-order (BtDocument::preorder), which is how a debugger maps AiBrain::nodes back.

#include "mce/asset/guid.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"

namespace mce::cook {

inline constexpr u32 kBtFormatVersion = 1;  // the "version" a .mcbt file carries
inline constexpr u32 kBtNoNode = ~0u;

// How many children a kind takes: none (a leaf), one (a decorator) or an array (a composite).
enum class BtArity : u8 { Leaf, One, Many };

// The kinds a .mcbt names, in asset::BtNodeKind order ("sequence" ... "token").
Span<const char* const> bt_kind_names();
// Leaf for a name that is not a kind.
BtArity bt_kind_arity(StringView kind);
bool bt_is_kind(StringView kind);
// The key types ("bool", "int", "float", "vec3", "entity").
Span<const char* const> bt_key_type_names();

// A member of a node or a key's default as JSON text, written inline: `{"a": 1, "b": [1, 2]}`.
void write_inline_json(JsonValue v, String& out);

struct BtDocField {
    String name;
    String json;  // inline JSON text
};

struct BtDocNode {
    String kind;               // "sequence", "task", ... (bt_kind_names)
    String value;              // a leaf's kind value as JSON text ("\"chase\"", "2"); empty otherwise
    Array<BtDocField> fields;  // the rest, in file order ("name", "key", "seconds", ...)
    Array<u32> children;       // indices into BtDocument::nodes

    // The field's JSON text, or empty.
    StringView field(StringView name) const;
    // Sets (or, with empty json, removes) a field; a new one goes last.
    void set_field(StringView name, StringView json);
};

struct BtDocKey {
    String name;
    String type;  // bt_key_type_names
    String def;   // the default as JSON text; empty: none
};

struct BtDocument {
    String name;
    Array<BtDocKey> keys;
    Array<BtDocNode> nodes;  // nodes[0] is the root (empty: no root yet)

    // The document nodes in the order the cook writes them: out[i] is cooked node i. Nodes no path
    // from the root reaches are left out.
    void preorder(Array<u32>& out) const;
    // The parent of `node` (kBtNoNode for the root or an unreached node).
    u32 parent(u32 node) const;
    u32 find_key(StringView name) const;  // kBtNoNode when absent
};

// Parses a .mcbt root into `out` (the shape only; see above). False when any error was added.
bool read_behavior_tree_document(JsonValue doc, StringView file, BtDocument& out, DiagList& diags);

// Appends `d` as .mcbt source text (two-space indented, a leaf on one line, a trailing newline).
void write_behavior_tree_document(const BtDocument& d, asset::Guid guid, String& out);

// The cook: the CHNK_BT payload of a parsed .mcbt root. False when any error diagnostic was added.
bool build_behavior_tree(JsonValue doc, StringView file, Array<byte>& payload, DiagList& diags);

}  // namespace mce::cook
