/*
 * Plaintext rendering module (checkpoint C3).
 *
 * Markup is dropped and line structure is kept. Content bytes are copied
 * through unchanged, so UTF-8 stays valid and the output is byte-for-byte
 * deterministic.
 */
#include "render_text.h"

#include <stdlib.h>
#include <string.h>

#define MD_TEXT_INDENT "  "

static int put(md_buffer *buf, const char *s)
{
    return md_buffer_append_cstr(buf, s);
}

static int render(md_buffer *buf, const md_node *node, unsigned depth, int tight);

/* Render an inline subtree: no block structure, no line ending. */
static int render_footnotes(md_buffer *buf, const md_node *node, unsigned depth);
static int render_footnote_def(md_buffer *buf, const md_node *node,
                               unsigned depth);
static int render_table_row(md_buffer *buf, const md_node *node, unsigned depth);

static int render_inlines(md_buffer *buf, const md_node *node, unsigned depth)
{
    size_t i;

    for (i = 0; i < node->child_count; i++) {
        if (render(buf, node->children[i], depth, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Make sure the buffer ends with exactly one newline. */
static int end_line(md_buffer *buf)
{
    if (buf->len == 0u) {
        return 0;
    }
    if (buf->data[buf->len - 1u] != '\n' && md_buffer_append_char(buf, '\n') != 0) {
        return -1;
    }
    return 0;
}

/* Make sure the buffer ends with exactly one blank line, collapsing any
 * already present; a rule or an empty block must not stack blank lines. */
static int end_block(md_buffer *buf)
{
    if (buf->len == 0u) {
        return 0;
    }
    if (buf->len >= 2u && buf->data[buf->len - 1u] == '\n' &&
        buf->data[buf->len - 2u] == '\n') {
        return 0;
    }
    if (end_line(buf) != 0) {
        return -1;
    }
    return md_buffer_append_char(buf, '\n');
}

/*
 * Indent the start of a block nested inside a list item. Indentation is only
 * written at the start of a line: a container that already positioned the
 * line (a task item writing its checkbox, say) keeps that position, which is
 * what keeps a marker's column from drifting.
 */
static int put_indent(md_buffer *buf, unsigned depth)
{
    unsigned i;

    if (buf->len != 0u && buf->data[buf->len - 1u] != '\n') {
        return 0;
    }
    for (i = 0; i < depth; i++) {
        if (put(buf, MD_TEXT_INDENT) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Containers whose children are blocks: document, blockquote, list, item. */
static int render_container(md_buffer *buf, const md_node *node,
                            int separate_with_blank, unsigned child_depth, int tight)
{
    size_t i;

    for (i = 0; i < node->child_count; i++) {
        if (i > 0u && separate_with_blank && end_block(buf) != 0) {
            return -1;
        }
        if (render(buf, node->children[i], child_depth, tight) != 0) {
            return -1;
        }
    }
    return 0;
}

static int render_table_row(md_buffer *buf, const md_node *node, unsigned depth)
{
    size_t i;

    if (put_indent(buf, depth) != 0) {
        return -1;
    }
    for (i = 0; i < node->child_count; i++) {
        const md_node *cell = node->children[i];

        if (cell->type != MD_NODE_TABLE_CELL) {
            continue;
        }
        if (i > 0u && md_buffer_append_char(buf, ' ') != 0) {
            return -1;
        }
        if (render_inlines(buf, cell, depth) != 0) {
            return -1;
        }
    }
    return end_line(buf);
}

static int render_footnotes(md_buffer *buf, const md_node *node, unsigned depth)
{
    size_t i;

    for (i = 0; i < node->footnote_count; i++) {
        if (end_line(buf) != 0) {
            return -1;
        }
        if (render_footnote_def(buf, node->footnotes[i], depth) != 0) {
            return -1;
        }
    }
    return end_line(buf);
}

static int render_footnote_def(md_buffer *buf, const md_node *node,
                               unsigned depth)
{
    const char *label = node->label != NULL ? node->label : "";

    if (put_indent(buf, depth) != 0 ||
        md_buffer_append(buf, "[^", 2u) != 0 ||
        md_buffer_append(buf, label, strlen(label)) != 0 ||
        md_buffer_append_char(buf, ']') != 0) {
        return -1;
    }
    /* A single-block note stays on one line; anything longer keeps the block
     * structure of its own. */
    if (node->child_count == 1u) {
        const md_node *body = node->children[0];

        if (md_buffer_append_char(buf, ' ') != 0) {
            return -1;
        }
        if (body->type == MD_NODE_PARAGRAPH) {
            if (render_inlines(buf, body, depth) != 0) {
                return -1;
            }
        } else if (render(buf, body, depth, 0) != 0) {
            return -1;
        }
        return end_line(buf);
    }
    if (node->child_count > 1u && md_buffer_append_char(buf, '\n') != 0) {
        return -1;
    }
    return render_container(buf, node, 1, depth + 1u, 0);
}

static int render(md_buffer *buf, const md_node *node, unsigned depth, int tight)
{
    if (buf == NULL || node == NULL) {
        md_set_error("invalid argument: null node");
        return -1;
    }
    switch (node->type) {
    case MD_NODE_DOCUMENT:
        if (render_container(buf, node, 1, depth, 0) != 0) {
            return -1;
        }
        /* Footnote definitions follow the block flow, so the notes read in
         * the same order a reader meets the references. */
        if (node->footnote_count == 0u) {
            return 0;
        }
        return render_footnotes(buf, node, depth);

    case MD_NODE_BLOCK_QUOTE:
        /* A quote has no marker of its own, only the blank-line spacing. */
        return render_container(buf, node, 1, depth, 0);

    case MD_NODE_TABLE:
        /* The table itself has no line: each row is one line. */
        return render_container(buf, node, 0, depth, 0);

    case MD_NODE_TABLE_ROW:
    case MD_NODE_TABLE_HEADER:
        /* One line per row, cells separated by a single space. */
        return render_table_row(buf, node, depth);

    case MD_NODE_TABLE_CELL:
        return render_inlines(buf, node, depth);

    case MD_NODE_FOOTNOTE_REF: {
        const char *label = node->label != NULL ? node->label : "";

        if (md_buffer_append(buf, "[^", 2u) != 0 ||
            md_buffer_append(buf, label, strlen(label)) != 0 ||
            md_buffer_append_char(buf, ']') != 0) {
            return -1;
        }
        return 0;
    }

    case MD_NODE_FOOTNOTE_DEF:
        return render_footnote_def(buf, node, depth);

    case MD_NODE_PARAGRAPH:
    case MD_NODE_HEADING:
        if (put_indent(buf, depth) != 0 || render_inlines(buf, node, depth) != 0) {
            return -1;
        }
        return end_line(buf);

    case MD_NODE_LIST:
        /* A tight list keeps one line per item; a loose list separates them
         * with a blank line. Items start one level deeper than the list. */
        if (render_container(buf, node, node->tight ? 0 : 1, depth + 1u,
                             node->tight) != 0) {
            return -1;
        }
        return end_line(buf);

    case MD_NODE_ITEM:
        /* Inside a tight item the blocks follow each other directly. A task
         * item is prefixed with the marker its source carried. */
        if (put_indent(buf, depth) != 0) {
            return -1;
        }
        if (node->checked == 0 || node->checked == 1) {
            const char *mark = node->checked == 1 ? "[x] " : "[ ] ";

            if (md_buffer_append(buf, mark, 4u) != 0) {
                return -1;
            }
        }
        return render_container(buf, node, tight ? 0 : 1, depth, tight);

    case MD_NODE_CODE_BLOCK: {
        const char *literal = node->literal != NULL ? node->literal : "";
        size_t len = strlen(literal);

        /* Trailing blank lines of a code block are not part of its text. */
        while (len > 0u && literal[len - 1u] == '\n') {
            len--;
        }
        if (put_indent(buf, depth) != 0) {
            return -1;
        }
        if (md_buffer_append(buf, literal, len) != 0) {
            return -1;
        }
        return end_line(buf);
    }

    case MD_NODE_THEMATIC_BREAK:
        /* A rule carries no text of its own. */
        return 0;

    case MD_NODE_TEXT:
    case MD_NODE_CODE:
        /* Code spans keep their literal content. */
        return md_buffer_append(buf, node->value != NULL ? node->value : "",
                                node->value != NULL ? strlen(node->value) : 0u);

    case MD_NODE_STRIKETHROUGH:
    case MD_NODE_EM:
    case MD_NODE_STRONG:
    case MD_NODE_LINK:
    case MD_NODE_IMAGE: /* an image renders as its alt text */
        return render_inlines(buf, node, depth);

    case MD_NODE_SOFTBREAK:
    case MD_NODE_HARDBREAK:
        return md_buffer_append_char(buf, '\n');
    }
    md_set_error("unknown node type");
    return -1;
}

char *md_render_text(md_arena *arena, const md_node *node)
{
    md_buffer buf;
    char *text;
    char *copy;

    if (arena == NULL || node == NULL) {
        md_set_error("invalid argument: null node");
        return NULL;
    }
    md_buffer_init(&buf);
    if (render(&buf, node, 0u, 0) != 0) {
        md_buffer_free(&buf);
        if (md_error_message()[0] == '\0') {
            md_set_error("failed to render text");
        }
        return NULL;
    }
    /* Exactly one final newline, and never a trailing blank line. */
    while (buf.len > 0u && buf.data[buf.len - 1u] == '\n') {
        buf.len--;
    }
    if (buf.len > 0u && md_buffer_append_char(&buf, '\n') != 0) {
        md_buffer_free(&buf);
        return NULL;
    }
    text = md_buffer_release(&buf);
    if (text == NULL) {
        return NULL;
    }
    copy = md_arena_strndup(arena, text, strlen(text));
    free(text);
    return copy;
}
