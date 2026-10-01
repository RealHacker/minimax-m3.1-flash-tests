#ifndef MD_H
#define MD_H

/*
 * Public API of the Markdown parser.
 *
 *   md_document *doc = md_parse_blocks(source);
 *   if (doc == NULL) { fprintf(stderr, "%s\n", md_error_message()); }
 *   char *json = md_ast_to_json(md_document_arena(doc), md_document_root(doc));
 *   md_document_destroy(doc);   // one destroy releases the whole AST
 *
 * The returned document owns an arena: every node and string it exposes lives
 * in that arena and stays valid until md_document_destroy() is called. No
 * function ever hands back a pointer to temporary stack data.
 */

#include "arena.h"
#include "ast.h"
#include "blocks.h"
#include "entities.h"
#include "extensions.h"
#include "inlines.h"
#include "lexer.h"
#include "refmap.h"
#include "render_html.h"
#include "render_text.h"
#include "slug.h"
#include "status.h"
#include "toc.h"

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Full parse (checkpoint C2): block structure plus inline content. Paragraph
 * and heading children are inline nodes (text, em, strong, code, link, image,
 * softbreak, hardbreak) and link reference definitions are resolved.
 * Returns NULL on failure and records a diagnostic retrievable through
 * md_error_message(). The caller owns the result and must release it with
 * md_document_destroy().
 */
md_document *md_parse(const char *src);

/* As md_parse(), but for a possibly non-terminated buffer of len bytes. */
md_document *md_parse_n(const char *src, size_t len);

/* As md_parse_n(), but the AST is allocated in a caller-owned arena;
 * md_document_destroy() then leaves that arena alone. */
md_document *md_parse_in(md_arena *arena, const char *src, size_t len);

/*
 * Parse a NUL-terminated Markdown document into an arena-owned AST of blocks
 * only (checkpoint C1): heading and paragraph content stays a single trimmed
 * text child. Returns NULL on failure and records a diagnostic retrievable
 * through md_error_message().
 */
md_document *md_parse_blocks(const char *src);

/* As md_parse_blocks(), but for a possibly non-terminated buffer of len bytes. */
md_document *md_parse_blocks_n(const char *src, size_t len);

/* As md_parse_blocks_n(), but the AST is allocated in a caller-owned arena;
 * md_document_destroy() then leaves that arena alone. */
md_document *md_parse_blocks_in(md_arena *arena, const char *src, size_t len);

/*
 * Parse inline content only and append the resulting inline nodes to
 * `parent`. The input is normalized like a document (tabs expanded, line
 * endings unified) and surrounding whitespace is trimmed. `parent` may be any
 * node, for example a document root. Returns MD_OK on success.
 */
md_status md_parse_inlines(md_arena *arena, const char *src, size_t len,
                           md_node *parent);

/* ------------------------------------------------------------------ */
/* Extensions (checkpoint C4)                                         */
/* ------------------------------------------------------------------ */

/*
 * The public name of the parse options. MDParseOptions and md_options are the
 * same type, so a value built for either is usable with every function that
 * takes the other; md_options is the internal spelling and MDParseOptions is
 * the one a caller of the library writes.
 *
 * A default-constructed set enables nothing:
 *
 *     MDParseOptions opts;
 *     md_options_init(&opts);
 *     opts.extensions = MD_EXT_TABLES | MD_EXT_FOOTNOTES;
 *
 * With an empty mask a parse reproduces the C1-C3 output byte for byte.
 */
typedef md_options MDParseOptions;

/* Zero the options: no extension enabled, no custom registry. */
void md_options_init(MDParseOptions *options);

/*
 * Register a block extension on a registry. Handlers are offered, in
 * registration order, at every block position the core constructs and the
 * built-in extensions have already declined. A handler consumes input by
 * appending nodes to `parent` and setting *lines_used and *consumed; a
 * handler that declines sets *consumed to 0 and changes nothing, and the
 * next handler is offered the same position. Returns MD_OK, or MD_ERR_INVAL
 * for a NULL registry, name or function, MD_ERR_LIMIT past the registration
 * bound, or MD_ERR_NOMEM.
 */
md_status md_ext_register_block(md_ext_registry *registry, const char *name,
                                md_ext_block_fn fn, void *user);

/*
 * Register an inline extension on a registry. Handlers are offered, in
 * registration order, at every character of an inline run that no built-in
 * construct claimed. A handler consumes input by appending nodes to
 * `parent` and setting *consumed; declining sets *consumed to 0 and changes
 * nothing. Returns MD_OK, or the same errors as md_ext_register_block().
 */
md_status md_ext_register_inline(md_ext_registry *registry, const char *name,
                                 md_ext_inline_fn fn, void *user);

/* How many handlers of each kind are registered; 0 for a NULL registry. */
size_t md_ext_registry_block_count(const md_ext_registry *registry);
size_t md_ext_registry_inline_count(const md_ext_registry *registry);

/*
 * The C4 entry points. Each takes the extensions to apply; the entry points
 * above are exactly these with an empty MDParseOptions, so C1-C3 behavior and
 * output are unchanged when no extension is enabled. With an empty mask a
 * disabled construct stays ordinary text, which is the contract an option
 * set has to keep.
 *
 * The options and any registry they point at are borrowed for the duration of
 * the call: the document keeps the mask it was parsed with (see
 * md_node.extensions) so that md_ast_to_json() can tell a task item and a
 * footnotes array from an ordinary document, but it copies no function
 * pointer and owns neither the options nor the registry. Release those with
 * md_options_init() and md_ext_registry_destroy() when you are done.
 */
md_document *md_parse_opts(const MDParseOptions *options, const char *src);
md_document *md_parse_opts_n(const MDParseOptions *options, const char *src,
                             size_t len);
md_document *md_parse_opts_in(md_arena *arena, const MDParseOptions *options,
                              const char *src, size_t len);

/* As md_parse_inlines(), with the extensions applied. */
md_status md_parse_inlines_opts(md_arena *arena, const char *src, size_t len,
                                md_node *parent,
                                const MDParseOptions *options);

/* ------------------------------------------------------------------ */
/* Streaming and limits (checkpoint C5)                               */
/* ------------------------------------------------------------------ */

/*
 * Parse the rest of `in` as a Markdown document (checkpoint C5).
 *
 *     FILE *f = fopen("doc.md", "rb");
 *     md_document *doc = md_parse_stream(f, NULL);
 *     fclose(f);
 *
 * The stream is consumed forward in fixed-size chunks and is never held
 * whole, so a large document costs one copy of itself rather than a copy
 * plus the raw bytes. `in` is not rewound and not closed. A stream already
 * positioned part-way through parses exactly its remainder.
 *
 * A NULL `options` is an empty option set -- no extensions, library default
 * limits -- which makes this the streaming form of md_parse(). The same
 * normalizer, line splitter, block parser and options serve both paths, so
 * the same bytes through either produce byte-identical AST JSON.
 */
md_document *md_parse_stream(FILE *in, const MDParseOptions *options);

/*
 * As md_parse_stream(), with the AST in a caller-owned arena, so
 * md_document_destroy() leaves that arena alone. `max_bytes` bounds the
 * input that will be read; 0 selects MD_MAX_INPUT_BYTES. Note that the
 * caller-created arena is not created with a cap here, so a memory ceiling
 * belongs in the arena (md_arena_create_limit()) or in the options.
 */
md_document *md_parse_stream_in(md_arena *arena, FILE *in, size_t max_bytes,
                                const MDParseOptions *options);

/*
 * The parser's limits. Both ceilings default to the library values, so an
 * md_options from md_options_init() parses exactly as C1-C4 did:
 *
 *     MDParseOptions opts;
 *     md_options_init(&opts);
 *     opts.limits.max_nesting = 8;      // give up at 8 levels
 *     opts.limits.max_bytes   = 1u << 20;  // one MiB of AST
 *
 * max_nesting is clamped to [1, MD_MAX_NESTING_HARD] and can never be raised
 * past that, which is what bounds the parser's use of the C stack: the
 * block parser descends one frame per nesting level, so a hostile document
 * can make the parse fail but cannot make it overflow the stack. max_bytes
 * caps what the parse may take from its arena; the document text is charged
 * against it too, so the cap bounds a large document as well.
 *
 * Where each limit applies:
 *
 *   - max_nesting is enforced by every entry point, because refusing to
 *     descend costs nothing and the stack has to be bounded whatever the
 *     caller does with the arena.
 *
 *   - max_bytes is a property of the arena, so it takes effect on the entry
 *     points that create one -- md_parse(), md_parse_opts(), md_parse_stream()
 *     and the rest -- which build the arena with md_arena_create_limit().
 *     On the _in forms the arena is the caller's, and what bounds memory is
 *     the cap that arena was created with:
 *
 *         md_arena *a = md_arena_create_limit(1u << 20);
 *         opts.limits.max_bytes = 0;   // irrelevant here: `a` is the cap
 *         doc = md_parse_opts_in(a, &opts, src, len);
 *
 *     That is deliberate rather than a gap. The alternative -- checking
 *     options->limits.max_bytes after the fact -- could only report an
 *     overrun once the memory had already been taken, so it would bound
 *     nothing while appearing to. A caller that supplies its own arena
 *     supplies its own memory policy; md_limits_bytes() still reports what
 *     the option set asked for, and that value is what the library's own
 *     entry points will use.
 *
 * Exceeding a limit is reported, not crashed: the parse returns NULL,
 * md_last_error() reports MD_ERROR_NESTING or MD_ERROR_MEMORY with the line
 * it was working on, and md_error_to_json() renders
 * {"error":{"line":N,"col":M,"message":"nesting too deep"}}.
 */

/*
 * The structured form of the last failure. md_error_message() still returns
 * the same text it always did; this is the same record with its kind and
 * position attached. Never NULL; reset with md_error_reset().
 */

/* Render a recorded failure as the structured error document described above.
 * Arena-owned and NUL-terminated, like every other string this library
 * returns. A NULL `error` means md_last_error(). */
char *md_error_to_json(md_arena *arena, const md_error *error);

/* The resolved nesting limit of `options`, and its memory cap in bytes. */
unsigned md_limits_nesting(const MDParseOptions *options);
size_t md_limits_bytes(const MDParseOptions *options);


/*
 * Read an extension configuration and apply it to `options`. The document is
 * a JSON object whose keys are extension names mapped to booleans:
 *
 *     { "tables": true, "strikethrough": true,
 *       "task_lists": true, "footnotes": true }
 *
 * The enabled set is the union of the keys that are true, so it does not
 * depend on the order they are written in; "all" and "none" are accepted as
 * names, the array form { "extensions": ["tables"] } is still accepted, and
 * {} enables nothing. An unknown name or a value that is neither a boolean
 * nor an array is rejected, so a configuration can never enable an
 * extension by accident. "-" reads the configuration from stdin. Returns 0
 * on success and -1 on failure, with a diagnostic retrievable through
 * md_error_message(). This is what `md --ext CONFIG.json` calls.
 */
int md_options_from_json(md_arena *arena, const char *json, size_t len,
                         MDParseOptions *options);
int md_options_from_file(md_arena *arena, const char *path,
                         MDParseOptions *options);

/*
 * Create an empty extension registry, and release it. Names are copied into
 * the arena. The registry is released by md_ext_registry_destroy(); the arena
 * it was created in is the caller's to release.
 */
md_ext_registry *md_ext_registry_create(md_arena *arena);
void md_ext_registry_destroy(md_ext_registry *registry);

/* The canonical name of a single extension bit, or NULL when there is none. */
const char *md_ext_flag_name(md_ext_flags bit);

/*
 * Look up an extension by name: "tables", "strikethrough", "task_lists" and
 * "footnotes", plus the aliases md_ext_flag_name's table does not show
 * ("all", "none", "tasklist", "strike", ...). Returns 0 and sets *flags on
 * success, -1 when the name is not an extension. A NULL name or out pointer
 * is MD_ERR-style invalid input and also returns -1.
 */
int md_ext_flag_by_name(const char *name, size_t len, md_ext_flags *flags);

/* ------------------------------------------------------------------ */
/* Renderers (checkpoint C3)                                          */
/* ------------------------------------------------------------------ */

/*
 * Render a parsed AST as an HTML fragment (md --html) or as plain text
 * (md --text). Both return arena-owned, NUL-terminated memory that lives as
 * long as the arena, and both return NULL on failure with a diagnostic
 * retrievable through md_error_message(). Neither ever returns a pointer to
 * temporary stack data, and both are deterministic: the same AST always
 * renders to the same bytes.
 *
 * The HTML output is escaped: text for `&`, `<` and `>`, attribute values
 * additionally for `"` and `'`, so no source text can become live markup.
 * Each block ends with a newline, so a non-empty document ends with one.
 * The text output drops markup and keeps the block line structure, with
 * exactly one final newline.
 */
char *md_render_html(md_arena *arena, const md_node *node);
char *md_render_text(md_arena *arena, const md_node *node);

/* ------------------------------------------------------------------ */
/* Table of contents (checkpoint C6)                                  */
/* ------------------------------------------------------------------ */

/*
 * Collect the headings of a parsed document in document order, and render
 * them as JSON. This is what `md --toc FILE` prints.
 *
 *     md_document *doc = md_parse(source);
 *     md_toc toc;
 *     if (md_toc_build(md_document_arena(doc), md_document_root(doc), &toc)
 *         != MD_OK) { ... }
 *     char *json = md_toc_to_json(md_document_arena(doc), &toc);
 *
 * The document is a JSON array of the entries themselves, with no wrapper
 * object around them:
 *
 *   [{"level":1,"text":"Title","anchor":"title"}]
 *
 * one entry per heading and the three keys always in that order, so a
 * consumer can rely on the shape the way it does on the AST. A document with
 * no headings is [].
 *
 * "text" is the heading's plain text with the inline markup removed. "anchor"
 * is its anchor: lowercased, hyphen-separated, trimmed, and unique within the
 * document. Two headings that map to the same string get "-1", "-2" and so
 * on appended, so every entry in a document has a distinct anchor and no
 * generated link can point at two headings at once. A heading that maps to
 * nothing at all -- "###" -- is called "section".
 *
 * The anchor rule is byte-wise and ASCII-only, so the same document produces
 * the same anchors on every machine regardless of locale; it is the rule the
 * HTML renderer already used for footnote ids (see md_slug_write()). The
 * entries and both strings are allocated in the caller's arena and released
 * with it; there is nothing to free individually.
 */
md_status md_toc_build(md_arena *arena, const md_node *root, md_toc *out);
char *md_toc_to_json(md_arena *arena, const md_toc *toc);

#ifdef __cplusplus
}
#endif

#endif
