#ifndef MD_AST_H
#define MD_AST_H

/*
 * Arena-owned Markdown AST plus the stable JSON serialization used by
 * `md --ast`. Every node lives in the arena of its document; nodes are
 * created through md_node_new() and never freed individually.
 *
 * JSON shape (keys always emitted, always in this order):
 *   document      {"type":"document","children":[...]}
 *   paragraph     {"type":"paragraph","children":[...]}
 *   heading       {"type":"heading","level":N,"children":[...]}
 *   list          {"type":"list","ordered":B,"start":N,"tight":B,"children":[...]}
 *   item          {"type":"item","children":[...]}
 *   blockquote    {"type":"blockquote","children":[...]}
 *   code_block    {"type":"code_block","info":"...","literal":"...","children":[]}
 *   thematic_break {"type":"thematic_break"}
 *   text          {"type":"text","value":"..."}
 *   em            {"type":"em","children":[...]}
 *   strong        {"type":"strong","children":[...]}
 *   code          {"type":"code","value":"..."}
 *   link          {"type":"link","destination":"...","title":"...","children":[...]}
 *   image         {"type":"image","destination":"...","title":"...","children":[...]}
 *   softbreak     {"type":"softbreak"}
 *   hardbreak     {"type":"hardbreak"}
 *
 * Checkpoint C4 adds node types that only exist when the matching extension
 * is enabled, and each one is emitted with a fixed key set as well:
 *   table         {"type":"table","align":["none",...],"children":[...]}
 *   table_header  {"type":"table_header","children":[...]}
 *   table_row     {"type":"table_row","children":[...]}
 *   table_cell    {"type":"table_cell","children":[...]}
 *   strikethrough {"type":"strikethrough","children":[...]}
 *   footnote_ref  {"type":"footnote_ref","id":"...","label":"..."}
 *   footnote_def  {"type":"footnote_def","id":"...","label":"...","children":[...]}
 *
 * Two keys are conditional, which is what keeps C1-C3 output byte-identical
 * when no extension is enabled: an item carries "checked":B only when it is
 * a task-list item, and a document carries "footnotes":[...] only when the
 * parse ran with MD_EXT_FOOTNOTES. Every other key is unconditional.
 *
 * Key order and the key set are fixed per node type. `type` is always first
 * and `children` always last, so a consumer can split a node on those two.
 * `title` is always emitted for link and image nodes: it is "" when the
 * source carried no title. A node never omits a required key, so consumers
 * can rely on the shape.
 */

#include "arena.h"
#include "status.h"

#include <stddef.h>

typedef enum md_node_type {
    MD_NODE_DOCUMENT = 0,
    MD_NODE_PARAGRAPH,
    MD_NODE_HEADING,
    MD_NODE_LIST,
    MD_NODE_ITEM,
    MD_NODE_BLOCK_QUOTE,
    MD_NODE_CODE_BLOCK,
    MD_NODE_THEMATIC_BREAK,
    MD_NODE_TEXT,
    /* Inline nodes (checkpoint C2). */
    MD_NODE_EM,
    MD_NODE_STRONG,
    MD_NODE_CODE,
    MD_NODE_LINK,
    MD_NODE_IMAGE,
    MD_NODE_SOFTBREAK,
    MD_NODE_HARDBREAK,
    /* Extension nodes (checkpoint C4); only produced when enabled. */
    MD_NODE_TABLE,
    MD_NODE_TABLE_HEADER,
    MD_NODE_TABLE_ROW,
    MD_NODE_TABLE_CELL,
    MD_NODE_STRIKETHROUGH,
    MD_NODE_FOOTNOTE_REF,
    MD_NODE_FOOTNOTE_DEF
} md_node_type;

/* Bitmask of enabled extensions, shared with md_options. */
typedef unsigned md_ext_flags;

/*
 * The extension bits live here rather than in extensions.h because the JSON
 * serializer has to know which of them make a key appear, and ast.c must not
 * depend on the extension registry. extensions.h re-exports them unchanged.
 */
#define MD_EXT_NONE          0u
#define MD_EXT_TABLES        (1u << 0)
#define MD_EXT_STRIKETHROUGH (1u << 1)
#define MD_EXT_TASK_LIST     (1u << 2)
#define MD_EXT_FOOTNOTES     (1u << 3)
#define MD_EXT_BUILTIN_ALL   (MD_EXT_TABLES | MD_EXT_STRIKETHROUGH | \
                              MD_EXT_TASK_LIST | MD_EXT_FOOTNOTES)

/* Column alignment of a table, in the AST and in JSON. */
typedef enum md_table_align {
    MD_TABLE_ALIGN_NONE = 0,
    MD_TABLE_ALIGN_LEFT,
    MD_TABLE_ALIGN_CENTER,
    MD_TABLE_ALIGN_RIGHT
} md_table_align;

/* "left", "center", "right" or "none": the fixed spelling used in JSON. */
const char *md_table_align_name(md_table_align align);

typedef struct md_node md_node;

struct md_node {
    md_node_type type;

    int level;          /* heading: 1..6 */
    int ordered;        /* list: 1 for ordered lists */
    int tight;          /* list: 1 when the list is tight */
    long start;         /* list: first number of an ordered list, else 1 */

    const char *info;   /* code_block: info string, "" when absent */
    const char *literal;/* code_block: verbatim content, "" when empty */
    const char *value;  /* text, code: node value */

    const char *destination; /* link, image: target, "" when absent */
    const char *title;       /* link, image: title, NULL when the source had none */

    /*
     * item: 1 for a checked task item, 0 for an unchecked one, and -1 for
     * an item that is not a task item. JSON emits "checked" only when the
     * value is 0 or 1, so ordinary lists keep their C1-C3 shape.
     */
    int checked;

    /* footnote_ref, footnote_def: normalized id and the label as written. */
    const char *id;
    const char *label;

    /* table: per-column alignment, align_count entries. */
    md_table_align *align;
    size_t align_count;

    /* document: the extension mask the parse ran with. */
    md_ext_flags extensions;

    /* document: footnote definitions, in document order. */
    md_node **footnotes;
    size_t footnote_count;
    size_t footnote_capacity;

    md_node **children; /* NULL when child_count == 0 */
    size_t child_count;
    size_t child_capacity;
};

const char *md_node_type_name(md_node_type type);

md_node *md_node_new(md_arena *arena, md_node_type type);
md_status md_node_append(md_arena *arena, md_node *parent, md_node *child);

/*
 * Insert child so that it becomes the child at `index`, shifting later
 * children right. index must be in 0..parent->child_count.
 */
md_status md_node_insert(md_arena *arena, md_node *parent, md_node *child,
                         size_t index);

/* Convenience constructors used by the block and inline parsers. */
md_node *md_make_text(md_arena *arena, const char *value);
md_node *md_make_text_n(md_arena *arena, const char *value, size_t len);
md_node *md_make_code_block(md_arena *arena, const char *info, size_t info_len,
                             const char *literal, size_t literal_len);
md_node *md_make_em(md_arena *arena);
md_node *md_make_strong(md_arena *arena);
md_node *md_make_code(md_arena *arena, const char *value, size_t len);
md_node *md_make_link(md_arena *arena, const char *destination,
                      const char *title);
md_node *md_make_image(md_arena *arena, const char *destination,
                       const char *title);
md_node *md_make_softbreak(md_arena *arena);
md_node *md_make_hardbreak(md_arena *arena);

/* Extension constructors. `label` is the source spelling and `id` the
 * normalized identifier that references and back links agree on; both are
 * arena copies. */
md_node *md_make_table(md_arena *arena, const md_table_align *align,
                       size_t align_count);
md_node *md_make_table_header(md_arena *arena);
md_node *md_make_table_row(md_arena *arena);
md_node *md_make_table_cell(md_arena *arena);
md_node *md_make_strikethrough(md_arena *arena);
md_node *md_make_footnote_ref(md_arena *arena, const char *id,
                              const char *label);
md_node *md_make_footnote_def(md_arena *arena, const char *id,
                              const char *label);

/* Mark an item as a task item: checked is 0 or 1. Any other value clears
 * the marker, so a plain item serializes exactly as it did in C1-C3. */
void md_node_set_task(md_node *item, int checked);

/* Record a footnote definition on a document, in document order. */
md_status md_node_append_footnote(md_arena *arena, md_node *document,
                                  md_node *definition);

/* ------------------------------------------------------------------ */
/* Document                                                            */
/* ------------------------------------------------------------------ */

typedef struct md_document md_document;

struct md_document {
    md_arena *arena;
    int owns_arena; /* 1 unless the caller supplied the arena */
    md_node *root;  /* MD_NODE_DOCUMENT */
};

/* Takes ownership of arena when owns_arena != 0. */
md_document *md_document_create(md_arena *arena, int owns_arena);
void md_document_destroy(md_document *doc);
md_node *md_document_root(md_document *doc);
md_arena *md_document_arena(md_document *doc);

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

/*
 * Serialize a node subtree as compact JSON into arena-owned memory. Never
 * returns a pointer to temporary stack data; returns NULL on failure and
 * records a diagnostic.
 */
char *md_ast_to_json(md_arena *arena, const md_node *node);

/* Append a JSON string literal (quotes included) for arbitrary bytes.
 * Control characters are escaped, UTF-8 sequences pass through unchanged. */
int md_json_write_string(md_buffer *buf, const char *s, size_t len);

#endif
