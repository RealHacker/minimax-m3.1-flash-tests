#ifndef MD_RENDER_HTML_H
#define MD_RENDER_HTML_H

/*
 * HTML rendering (checkpoint C3).
 *
 * The renderer walks a parsed AST and produces a complete HTML fragment in
 * arena-owned memory. Output is deterministic: block order follows document
 * order, tags are fixed per node type, and every block is followed by exactly
 * one newline. No part of the source document can become live markup: text is
 * escaped for `&`, `<` and `>`, and attribute values additionally escape
 * `"` and `'`, so a literal `<script>` in the input always reaches the output
 * as `&lt;script&gt;`.
 *
 * Node mapping:
 *   document      children, one block per line
 *   paragraph     <p>...</p>
 *   heading       <h1>..<h6>...</hN>
 *   list          <ul>...</ul> or <ol start="N">...</ol>
 *   item          <li>...</li>
 *   blockquote    <blockquote>...</blockquote>
 *   code_block    <pre><code class="language-INFO">verbatim</code></pre>
 *   thematic_break <hr />
 *   text          escaped text
 *   em            <em>...</em>
 *   strong        <strong>...</strong>
 *   code          <code>escaped</code>
 *   link          <a href="..." title="...">...</a>
 *   image         <img src="..." alt="..." title="..." />
 *   softbreak     newline
 *   hardbreak     <br />
 *
 * `title` attributes are emitted only when the source carried a title, and
 * `class` only when the code block has an info string. The class name is the
 * first whitespace-delimited word of the info string, as in CommonMark.
 */

#include "arena.h"
#include "ast.h"
#include "status.h"

/*
 * Render `node` and its subtree as an HTML fragment allocated in `arena`.
 * The returned string is NUL-terminated and owned by the arena; it is never
 * a pointer to temporary stack data. Returns NULL on failure and records a
 * diagnostic retrievable through md_error_message().
 */
char *md_render_html(md_arena *arena, const md_node *node);

#endif
