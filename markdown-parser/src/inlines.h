#ifndef MD_INLINES_H
#define MD_INLINES_H

/*
 * Inline content parsing (checkpoint C2).
 *
 * The block parser hands the content lines of a paragraph or heading to
 * md_inline_lines(), which joins them with '\n' and runs the inline parser
 * over the result. Supported inline constructs:
 *
 *   emphasis / strong   *a*, _a_, **a**, __a__, nested runs
 *   code spans          `a`, ``a `b` c``
 *   inline links        [a](dest "title")
 *   reference links     [a][ref], [ref], [a][]  (definitions: [ref]: url "t")
 *   images              ![a](dest "title"), ![a][ref]
 *   escapes             \* \& \\ and a backslash at end of line (hard break)
 *   entities            &amp; &#35; &#x23;
 *   breaks              softbreak (one newline) and hardbreak (2+ spaces or
 *                       a trailing backslash before the newline)
 *
 * A delimiter that cannot be matched stays literal text, as does an
 * undefined reference label, an unknown entity, and a link whose tail does
 * not parse. Nothing is ever dropped.
 *
 * Documented simplifications relative to CommonMark: emphasis uses a
 * simplified delimiter search (a `*` inside a code span or link label may be
 * treated as a delimiter), autolinks are not recognized, and named entities
 * are limited to the table in entities.h.
 *
 * Two nesting limits apply, and they are different in kind. Recursion into
 * nested constructs is capped at compile time by MD_INLINE_MAX_NESTING, which
 * bounds the C stack; past it a construct stays literal text, as CommonMark
 * expects of a run of delimiters. Bracket nesting inside a link label is
 * instead bounded by the caller's configured limits.max_nesting (default
 * MD_MAX_NESTING, never above MD_MAX_NESTING_HARD), because a label nested
 * deeper than that cannot be located correctly at all: past the limit the
 * closing ']' is undecidable, so the document is refused with MD_ERR_NESTING
 * rather than being given a label boundary that was guessed.
 */

#include "arena.h"
#include "ast.h"
#include "extensions.h"
#include "lexer.h"
#include "refmap.h"
#include "status.h"

#include <stddef.h>

/* Recursion cap for nested links, images and emphasis. */
#define MD_INLINE_MAX_NESTING 64u

/* C1 behavior: one trimmed text node holding the joined lines. */
md_status md_inline_lines_raw(md_arena *arena, const char *text,
                              const md_span *lines, size_t count,
                              md_node *parent);

/*
 * C1 behavior for a single already-trimmed slice, used for a table cell when
 * a parse runs without inline parsing. Never parses markup.
 */
md_status md_inline_text_run_raw(md_arena *arena, const char *text, size_t len,
                                 md_node *parent);

/* C2: parse the joined lines into inline child nodes of `parent`. */
md_status md_inline_lines(md_arena *arena, const char *text,
                          const md_span *lines, size_t count,
                          md_node *parent, md_refmap *refs);

/*
 * C2 plus the C4 extensions selected by `options`: the same parse as
 * md_inline_lines() when options is NULL or its mask is MD_EXT_NONE.
 */
md_status md_inline_lines_opts(md_arena *arena, const char *text,
                               const md_span *lines, size_t count,
                               md_node *parent, md_refmap *refs,
                               const md_options *options);

/*
 * As md_inline_lines_opts(), and on MD_ERR_NESTING report the document offset
 * of the construct that was refused, so a caller holding the document text
 * can name the line and column. `err_off` is written only for that status;
 * a NULL `err_off` is accepted.
 */
md_status md_inline_lines_opts_err(md_arena *arena, const char *text,
                                   const md_span *lines, size_t count,
                                   md_node *parent, md_refmap *refs,
                                   const md_options *options,
                                   size_t *err_off);

/* Parse text[0..len) as inline content into `parent`, without normalization. */
md_status md_inline_text_run(md_arena *arena, const char *text, size_t len,
                             md_node *parent, md_refmap *refs);

/* As md_inline_text_run(), with the C4 extensions. */
md_status md_inline_text_run_opts(md_arena *arena, const char *text,
                                  size_t len, md_node *parent,
                                  md_refmap *refs, const md_options *options);

/* Parse a single inline string with the C4 extensions and no reference map. */
md_status md_parse_inlines_opts(md_arena *arena, const char *src, size_t len,
                                md_node *parent, const md_options *options);

/* Copy src[off..off+len) into the arena, resolving backslash escapes. */
char *md_inline_unescape(md_arena *arena, const char *src, size_t off,
                         size_t len);

/*
 * Read a link reference definition from a single source line. On success the
 * label, destination and title spans are returned as offsets into text and
 * returns 1; returns 0 when the line is not a definition. A definition that
 * carries no title sets *has_title to 0 and the title span to 0.
 */
int md_inline_scan_refdef(const char *text, size_t off, size_t len,
                          size_t *label_off, size_t *label_len,
                          size_t *dest_off, size_t *dest_len,
                          size_t *title_off, size_t *title_len,
                          int *has_title);

#endif
