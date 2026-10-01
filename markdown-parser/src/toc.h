#ifndef MD_TOC_H
#define MD_TOC_H

/*
 * Table of contents (checkpoint C6).
 *
 * Collects the headings of a parsed document in document order and renders
 * them as a compact JSON array. Everything the module returns is allocated in
 * the caller's arena, so it is released with md_arena_destroy() and there is
 * no per-entry free.
 *
 * Output shape (one entry per heading, keys always in this order). The
 * top level is the array itself: the entries are the document, so there is
 * no wrapper object naming them.
 *
 *   [{"level":1,"text":"Title","anchor":"title"}]
 *
 * The document is the one --ast emits, so a consumer can rely on the key set
 * and the key order the same way it can on the AST itself. A document with no
 * headings is [], never an empty string and never a wrapper holding null.
 */

#include "arena.h"
#include "ast.h"
#include "status.h"

#include <stddef.h>

/* One heading. The three fields are arena-owned and never NULL. */
typedef struct md_toc_entry {
    int level;           /* heading level, 1..6 */
    const char *text;    /* the heading's plain text */
    const char *anchor;  /* the anchor id, unique within the document */
} md_toc_entry;

typedef struct md_toc {
    const md_toc_entry *entries; /* arena-owned; NULL when count == 0 */
    size_t count;
} md_toc;

/*
 * Walk `root` in document order and collect every heading. Returns MD_OK,
 * MD_ERR_INVAL for a NULL arena or root, or MD_ERR_NOMEM.
 *
 * Headings are found wherever they sit in the block tree, so a heading inside
 * a block quote or a list item is listed. Footnote definitions are held
 * outside the tree (see md_node.footnotes) and so are not visited: a table of
 * contents describes the document, and a definition is a citation rather than
 * a part of it.
 */
md_status md_toc_build(md_arena *arena, const md_node *root, md_toc *out);

/*
 * Serialize a table of contents as compact JSON, allocated in `arena` and
 * NUL-terminated. The same table always renders to the same bytes. Returns
 * NULL on failure with a diagnostic recorded.
 */
char *md_toc_to_json(md_arena *arena, const md_toc *toc);

#endif
