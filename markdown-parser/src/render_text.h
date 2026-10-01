#ifndef MD_RENDER_TEXT_H
#define MD_RENDER_TEXT_H

/*
 * Plaintext rendering (checkpoint C3).
 *
 * The renderer walks a parsed AST and produces a readable plain text version
 * of the document in arena-owned memory. All markup is removed: emphasis,
 * strong emphasis, link and image syntax collapse to their text, and code
 * spans keep their literal content. The block structure survives as line
 * structure - a blank line between top level blocks, one line per list item
 * in a tight list, a blank line between items in a loose list, and verbatim
 * lines for code blocks.
 *
 * Node mapping:
 *   document / blockquote   children, blocks separated by a blank line
 *   paragraph / heading     children, followed by a line break
 *   list                    items, one per line (tight) or blank line (loose)
 *   item                    children
 *   code_block              verbatim content
 *   thematic_break          nothing (it carries no text)
 *   text / code             value
 *   em / strong             children
 *   link                    children
 *   image                   the alt text of its children
 *   softbreak / hardbreak   a line break
 *
 * The result never ends with a blank line: trailing newlines are trimmed and
 * exactly one final newline is appended when the document has content. An
 * empty document renders to the empty string.
 */

#include "arena.h"
#include "ast.h"
#include "status.h"

#include <stddef.h>

/*
 * Render `node` and its subtree as plain text allocated in `arena`. The
 * returned string is NUL-terminated and owned by the arena; it is never a
 * pointer to temporary stack data. Returns NULL on failure and records a
 * diagnostic retrievable through md_error_message().
 */
char *md_render_text(md_arena *arena, const md_node *node);

#endif
