#ifndef MD_EXTENSIONS_H
#define MD_EXTENSIONS_H

/*
 * Optional syntax extensions (checkpoint C4).
 *
 * Extensions are selected by a bitmask carried in md_options. The mask is
 * empty by default, so every entry point that predates C4 keeps exactly the
 * C1-C3 behavior it had: a disabled extension leaves its syntax as ordinary
 * text, and a parse with MD_EXT_NONE produces byte-identical output to C3.
 *
 * Built-in extensions:
 *
 *   MD_EXT_TABLES          pipe tables, "type":"table" nodes
 *   MD_EXT_STRIKETHROUGH   ~~strikethrough~~
 *   MD_EXT_TASK_LIST       "- [ ]" / "- [x]" task items
 *   MD_EXT_FOOTNOTES       [^1] references and [^1]: definitions
 *
 * On top of the built-ins a caller can register its own block and inline
 * extensions at runtime; see md_ext_register_block() and
 * md_ext_register_inline() below and the worked example in the README.
 *
 * Ordering inside one block position is fixed, so a registered extension
 * can never change how core or built-in syntax is parsed: core blocks
 * (thematic break, heading, fence, block quote, list) first, then the
 * built-in extension blocks, then registered block extensions in
 * registration order, then a paragraph. The same order applies inline, where
 * a registered inline extension is offered every character that is not one
 * of the built-in markers.
 */

#include "arena.h"
#include "ast.h"
#include "status.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Extension flags                                                     */
/* ------------------------------------------------------------------ */
/* md_ext_flags, md_table_align, the MD_EXT_* bits and
 * md_table_align_name() are declared in ast.h, which this header includes:
 * the AST records a parse's extension mask, and the JSON serializer needs
 * the footnotes bit to decide whether "footnotes" is part of a document.
 *
 * The mask is empty by default, and a disabled extension must leave its
 * syntax as ordinary text, so an empty md_options reproduces C1-C3 exactly.
 */

/*
 * Upper bound on the columns of one table. A row with more separators than
 * this keeps the extra cells as text, so a pathological row cannot make the
 * parser allocate without bound.
 */
#define MD_EXT_MAX_TABLE_COLUMNS 256u

/* "tables", "strikethrough", "tasklist", "footnotes", "all", "none".
 * Returns 0 and sets *flags on success, -1 when the name is unknown. */
int md_ext_flag_by_name(const char *name, size_t len, md_ext_flags *flags);

/* Inverse of md_ext_flag_by_name(): the canonical name of a single bit. */
const char *md_ext_flag_name(md_ext_flags bit);

/* ------------------------------------------------------------------ */
/* Parse options                                                       */
/* ------------------------------------------------------------------ */

typedef struct md_ext_registry md_ext_registry;

/* ------------------------------------------------------------------ */
/* Parse limits (checkpoint C5)                                       */
/* ------------------------------------------------------------------ */

/*
 * Two ceilings on what one parse may cost. Both are zero in a default
 * md_options, which means "use the library default", so an md_options built
 * the C1-C4 way parses exactly as it always did.
 *
 *   max_nesting   the deepest block nesting a document may reach. 0 selects
 *                 MD_MAX_NESTING (64), the C1-C4 limit. A smaller value makes
 *                 the parser give up earlier; a larger one is clamped to
 *                 MD_MAX_NESTING_HARD, which no configuration can raise.
 *   max_bytes     the most memory one parse may take from its arena. 0 means
 *                 no cap, and the arena is created with md_arena_create().
 *
 * Exceeding either is not a crash and not a bare failure: the parse returns
 * NULL, md_last_error() reports MD_ERROR_NESTING or MD_ERROR_MEMORY with the
 * 1-based line the parser was working on, and md_error_to_json() renders
 * exactly that as {"error":{"line":N,"col":M,"message":"..."}}.
 */
typedef struct md_limits {
    size_t max_nesting;
    size_t max_bytes;
} md_limits;

typedef struct md_options {
    md_ext_flags extensions;      /* bitmask of enabled built-ins */
    md_ext_registry *custom;      /* runtime registrations, NULL when none */
    md_limits limits;             /* nesting and memory ceilings (C5) */
} md_options;

/*
 * The parse options under their public name. md.h re-exports this type, which
 * is the name a caller of the library writes; the two names are the same
 * type, so a value built for one works with the other.
 */
typedef struct md_options MDParseOptions;

/* Options with no extension enabled and no custom registry. */
void md_options_init(md_options *options);

/*
 * The hard ceiling on block nesting. The parser descends the C stack once per
 * nesting level, so this bound is what makes "never overflow the C stack" a
 * property of the library rather than of the caller's configuration: no
 * md_limits value can push the recursion past it.
 */
#define MD_MAX_NESTING_HARD 256u

/* The resolved block nesting limit of `options`: the configured value clamped
 * to [1, MD_MAX_NESTING_HARD], or MD_MAX_NESTING when none is configured. */
unsigned md_limits_nesting(const md_options *options);

/* The resolved arena memory cap of `options`, in bytes; 0 when uncapped. */
size_t md_limits_bytes(const md_options *options);

/*
 * Render a recorded failure as the structured error document:
 *
 *   {"error":{"line":12,"col":1,"message":"nesting too deep"}}
 *
 * Keys are always present and always in that order, and line and col are 0
 * when the failure is not tied to a place in the input. The result is
 * arena-owned and NUL-terminated, like every other string this library
 * returns, and the message is escaped like any other JSON string. Returns
 * NULL on failure. A NULL `error` argument is the same as NULL
 * md_last_error().
 */
char *md_error_to_json(md_arena *arena, const md_error *error);


/* ------------------------------------------------------------------ */
/* Runtime registration                                                */
/* ------------------------------------------------------------------ */

/*
 * What a handler is given: the arena to allocate nodes and strings in, the
 * enabled extension mask, and the options the parse was started with. The
 * environment is valid only for the duration of the handler call.
 */
typedef struct md_ext_env {
    md_arena *arena;
    md_ext_flags flags;
    const md_options *options;
} md_ext_env;

/*
 * Block extension. Called with the first line of a block position that no
 * core or built-in construct claimed, plus the lines that follow inside the
 * same container. On success the handler appends its node to `parent` and
 * sets *lines_used to the number of consumed lines; when it declines it sets
 * *consumed to 0 and changes nothing.
 */
typedef md_status (*md_ext_block_fn)(void *user, const md_ext_env *env,
                                     md_node *parent, const char *text,
                                     size_t len, const char *const *lines,
                                     const size_t *line_lens,
                                     size_t line_count, size_t *lines_used,
                                     int *consumed);

/*
 * Inline extension. Called with the inline run and the offset it is at; the
 * handler inspects the bytes from there. To consume input it appends node(s)
 * to `parent`, sets *consumed to the number of bytes taken, and returns
 * MD_OK. To decline it sets *consumed to 0 and changes nothing.
 */
typedef md_status (*md_ext_inline_fn)(void *user, const md_ext_env *env,
                                      md_node *parent, const char *text,
                                      size_t len, size_t pos,
                                      size_t *consumed);

/* Create an empty registry. Names are copied into it. */
md_ext_registry *md_ext_registry_create(md_arena *arena);

/* Release the registry (not the arena it was created in). */
void md_ext_registry_destroy(md_ext_registry *registry);

/*
 * Register a block extension. Handlers are offered in registration order
 * after the core and built-in block constructs. Re-registering the same name
 * replaces the previous handler, which keeps a configuration file and a
 * caller-supplied default from colliding silently. Returns MD_OK, or
 * MD_ERR_INVAL / MD_ERR_NOMEM.
 */
md_status md_ext_register_block(md_ext_registry *registry, const char *name,
                                md_ext_block_fn fn, void *user);

/* As md_ext_register_block(), for inline extensions. */
md_status md_ext_register_inline(md_ext_registry *registry, const char *name,
                                 md_ext_inline_fn fn, void *user);

size_t md_ext_registry_block_count(const md_ext_registry *registry);
size_t md_ext_registry_inline_count(const md_ext_registry *registry);

/*
 * Invoke the nth registered block or inline handler. The parsers own the
 * dispatch loop and enumerate by ordinal, so the registry's layout stays
 * private to extensions.c. A handler that is not there consumed nothing.
 */
md_status md_ext_invoke_block(md_ext_registry *registry, size_t ordinal,
                              const md_ext_env *env, md_node *parent,
                              const char *text, size_t len,
                              const char *const *lines, const size_t *line_lens,
                              size_t line_count, size_t *lines_used,
                              int *consumed);
md_status md_ext_invoke_inline(md_ext_registry *registry, size_t ordinal,
                               const md_ext_env *env, md_node *parent,
                               const char *text, size_t len, size_t pos,
                               size_t *consumed);

/* ------------------------------------------------------------------ */
/* Configuration files                                                 */
/* ------------------------------------------------------------------ */

/*
 * Parse a JSON configuration and apply it to `options`.
 *
 * The document is an object whose members are extension names mapped to
 * booleans; a member set to true contributes its bit and a member set to
 * false contributes nothing:
 *
 *     { "tables": true, "strikethrough": true,
 *       "task_lists": true, "footnotes": true }
 *
 * The names are "tables", "strikethrough", "task_lists" and "footnotes", plus
 * the aliases md_ext_flag_by_name() accepts ("all", "none", "tasklist",
 * "strike", ...). The enabled set is the union of the true keys, so it does
 * not depend on the order they are written in.
 *
 * The array form
 *
 *     { "extensions": ["tables", "footnotes"] }
 *
 * is accepted as well and unions the same way.
 *
 * "all" turns on every built-in, "none" on none, and an absent, empty or
 * false member enables nothing, so a configuration can never turn an
 * extension on by accident. A name that is not an extension and a value that
 * is neither a boolean nor an array are both rejected, so a typo is a
 * diagnostic rather than a run that quietly did less than the file asked
 * for. Returns 0 on success, -1 on a malformed or unknown configuration,
 * with a diagnostic retrievable through md_error_message().
 */
int md_options_from_json(md_arena *arena, const char *json, size_t len,
                         md_options *options);

/* Read a configuration file and apply it. "-" reads stdin. */
int md_options_from_file(md_arena *arena, const char *path,
                         md_options *options);

/* ------------------------------------------------------------------ */
/* Syntax primitives shared by the block and inline parsers            */
/* ------------------------------------------------------------------ */

/*
 * Split one table row on unescaped '|' characters. Leading and trailing
 * pipes are optional and are not part of any cell. Returns the number of
 * cells written to `cells` (each an offset/length pair into the row). With
 * cell_off and cell_len both NULL only the number of cells is computed, so
 * the table predicate can test a candidate row without allocating.
 */
size_t md_ext_table_split(const char *row, size_t len, size_t *cell_off,
                          size_t *cell_len, size_t capacity);

/* Cells a table row would yield, or 0 when it holds no separator. */
size_t md_ext_table_count(const char *row, size_t len);

/*
 * Read a table delimiter row such as "| --- | :-: |". Returns the number of
 * columns, or 0 when the line is not a delimiter row. The capacity is the
 * size of the caller's `align` array and is capped at eight, which bounds the
 * stack this runs in; a row with more columns than that is reported as not a
 * delimiter row.
 */
size_t md_ext_table_delims(const char *row, size_t len, md_table_align *align,
                           size_t capacity);

/*
 * Read a task-list marker at the start of list item content. Returns 1 and
 * sets *checked when the content opens with "[ ]", "[x]" or "[X]" followed
 * by a space, a tab or the end of the line; sets *rest to the offset of the
 * content that follows the marker.
 */
int md_ext_task_marker(const char *text, size_t len, size_t indent,
                       int *checked, size_t *rest);

/*
 * Read a footnote label at text[off..off+len). A label is '[' '^' label ']'
 * with a non-empty label and no nested '['. On success returns 1 and sets
 * the label span and the span of the bytes that follow the closing bracket.
 */
int md_ext_footnote_label(const char *text, size_t len, size_t off,
                          size_t *label_off, size_t *label_len,
                          size_t *end);

/* Copy a footnote label into arena memory, normalized as a reference label
 * (case folded, internal whitespace collapsed) so that references and
 * definitions match. Returns NULL on allocation failure. */
char *md_ext_footnote_id(md_arena *arena, const char *label, size_t len);

#endif
