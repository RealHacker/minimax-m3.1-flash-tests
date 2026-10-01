/*
 * HTML rendering module (checkpoint C3).
 *
 * Deterministic, allocation-checked, and safe by construction: the only way
 * bytes reach the output is through one of the two escape helpers, so source
 * text can never introduce a tag, an attribute or an entity.
 */
#include "render_html.h"
#include "slug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Escaping                                                            */
/* ------------------------------------------------------------------ */

/*
 * Append `s` to `buf`, escaping everything that would otherwise be markup.
 * `attr` additionally escapes the two quote characters, so an escaped value
 * can never leave its attribute. Every other byte, including UTF-8
 * continuation bytes, is copied through unchanged, so the result stays
 * UTF-8 preserving.
 */
static int escape_copy(md_buffer *buf, const char *s, size_t len, int attr)
{
    size_t i;
    size_t start = 0;

    for (i = 0; i < len; i++) {
        const char *rep = NULL;

        switch (s[i]) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            if (attr) {
                rep = "&quot;";
            }
            break;
        case '\'':
            if (attr) {
                rep = "&#39;";
            }
            break;
        default:
            break;
        }
        if (rep == NULL) {
            continue;
        }
        if (i > start && md_buffer_append(buf, s + start, i - start) != 0) {
            return -1;
        }
        if (md_buffer_append_cstr(buf, rep) != 0) {
            return -1;
        }
        start = i + 1;
    }
    if (len > start && md_buffer_append(buf, s + start, len - start) != 0) {
        return -1;
    }
    return 0;
}

/* Escape `&`, `<` and `>`: everything that could open markup. */
static int escape_text(md_buffer *buf, const char *s, size_t len)
{
    return escape_copy(buf, s, len, 0);
}

/* As escape_text(), plus `'` and `"`, for attribute values. */
static int escape_attr(md_buffer *buf, const char *s, size_t len)
{
    return escape_copy(buf, s, len, 1);
}

static int put(md_buffer *buf, const char *s)
{
    return md_buffer_append_cstr(buf, s);
}

/* Append an attribute only when it carries a value. */
static int put_attr(md_buffer *buf, const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    if (put(buf, " ") != 0) {
        return -1;
    }
    if (put(buf, name) != 0 || put(buf, "=\"") != 0) {
        return -1;
    }
    if (escape_attr(buf, value, strlen(value)) != 0) {
        return -1;
    }
    return put(buf, "\"");
}

static int put_number(md_buffer *buf, long value)
{
    char num[32];

    if (snprintf(num, sizeof num, "%ld", value) < 0) {
        md_set_error("failed to format number");
        return -1;
    }
    return put(buf, num);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static int render_node(md_buffer *buf, const md_node *node, int tight);

/* Render children with the block context inherited from the parent. */
static int render_children(md_buffer *buf, const md_node *node, int tight)
{
    size_t i;

    for (i = 0; i < node->child_count; i++) {
        if (render_node(buf, node->children[i], tight) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Append the first word of an info string, as CommonMark uses for classes. */
static int put_code_class(md_buffer *buf, const char *info)
{
    size_t len = 0;

    if (info == NULL) {
        return 0;
    }
    while (info[len] != '\0' && info[len] != ' ' && info[len] != '\t' &&
           info[len] != '\n' && info[len] != '\r') {
        len++;
    }
    if (len == 0u) {
        return 0;
    }
    if (put(buf, " class=\"language-") != 0) {
        return -1;
    }
    if (escape_attr(buf, info, len) != 0) {
        return -1;
    }
    return put(buf, "\"");
}

/* Raw, unescaped text of a subtree: used for image alt attributes. */
static int collect_text(md_buffer *buf, const md_node *node)
{
    size_t i;

    if (node == NULL) {
        return 0;
    }
    switch (node->type) {
    case MD_NODE_TEXT:
    case MD_NODE_CODE:
        return md_buffer_append(buf, node->value != NULL ? node->value : "",
                                node->value != NULL ? strlen(node->value) : 0u);
    case MD_NODE_SOFTBREAK:
    case MD_NODE_HARDBREAK:
        return md_buffer_append_char(buf, ' ');
    case MD_NODE_CODE_BLOCK:
        return md_buffer_append(buf, node->literal != NULL ? node->literal : "",
                                node->literal != NULL ? strlen(node->literal) : 0u);
    default:
        break;
    }
    for (i = 0; i < node->child_count; i++) {
        if (collect_text(buf, node->children[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int render_heading(md_buffer *buf, const md_node *node)
{
    int level = (node->level >= 1 && node->level <= 6) ? node->level : 1;

    if (put(buf, "<h") != 0 || put_number(buf, (long)level) != 0) {
        return -1;
    }
    if (put(buf, ">") != 0 || render_children(buf, node, 0) != 0) {
        return -1;
    }
    if (put(buf, "</h") != 0 || put_number(buf, (long)level) != 0) {
        return -1;
    }
    return put(buf, ">\n");
}

static int render_list(md_buffer *buf, const md_node *node)
{
    size_t i;

    if (node->ordered) {
        if (put(buf, "<ol") != 0) {
            return -1;
        }
        /* The attribute is emitted only when it differs from the default. */
        if (node->start != 1) {
            if (put(buf, " start=\"") != 0 ||
                put_number(buf, node->start) != 0 || put(buf, "\"") != 0) {
                return -1;
            }
        }
        if (put(buf, ">\n") != 0) {
            return -1;
        }
    } else if (put(buf, "<ul>\n") != 0) {
        return -1;
    }
    for (i = 0; i < node->child_count; i++) {
        /* The item node emits its own <li>...</li> wrapper. */
        if (render_node(buf, node->children[i], node->tight) != 0) {
            return -1;
        }
    }
    return put(buf, node->ordered ? "</ol>\n" : "</ul>\n");
}

static int render_code_block(md_buffer *buf, const md_node *node)
{
    const char *literal = node->literal != NULL ? node->literal : "";
    size_t len = strlen(literal);

    if (put(buf, "<pre><code") != 0) {
        return -1;
    }
    if (put_code_class(buf, node->info) != 0) {
        return -1;
    }
    if (put(buf, ">") != 0) {
        return -1;
    }
    if (escape_text(buf, literal, len) != 0) {
        return -1;
    }
    if (len > 0u && literal[len - 1u] != '\n' && put(buf, "\n") != 0) {
        return -1;
    }
    return put(buf, "</code></pre>\n");
}

static int render_image(md_buffer *buf, const md_node *node)
{
    md_buffer alt;
    int rc = -1;

    md_buffer_init(&alt);
    if (collect_text(&alt, node) != 0) {
        goto done;
    }
    if (put(buf, "<img src=\"") != 0) {
        goto done;
    }
    if (escape_attr(buf, node->destination != NULL ? node->destination : "",
                    node->destination != NULL ? strlen(node->destination) : 0u) != 0) {
        goto done;
    }
    if (put(buf, "\" alt=\"") != 0) {
        goto done;
    }
    if (escape_attr(buf, alt.data != NULL ? alt.data : "",
                    alt.data != NULL ? alt.len : 0u) != 0) {
        goto done;
    }
    if (put(buf, "\"") != 0) {
        goto done;
    }
    if (put_attr(buf, "title", node->title) != 0) {
        goto done;
    }
    /* An image is phrasing content: it never carries a trailing newline. */
    rc = put(buf, " />");
done:
    md_buffer_free(&alt);
    return rc;
}

/*
 * A document is its block flow followed by the footnote definitions it
 * collected, in document order, so a reader sees every note that was
 * referenced and no note that was not.
 */
static int render_document(md_buffer *buf, const md_node *node)
{
    size_t i;

    if (render_children(buf, node, 0) != 0) {
        return -1;
    }
    if (node->footnote_count == 0u) {
        return 0;
    }
    if (put(buf, "<section class=\"footnotes\">\n") != 0) {
        return -1;
    }
    for (i = 0; i < node->footnote_count; i++) {
        if (render_node(buf, node->footnotes[i], 0) != 0) {
            return -1;
        }
    }
    return put(buf, "</section>\n");
}

/*
 * A table. Alignment is expressed with an `align` attribute on each cell, so
 * a consumer that does not know the attribute still sees a rectangular
 * table, and one that does can lay the columns out.
 */
static int render_table(md_buffer *buf, const md_node *node)
{
    size_t i;
    size_t j;
    int in_body = 0; /* the single <tbody> is opened once, at the first row */

    if (put(buf, "<table>\n") != 0) {
        return -1;
    }
    for (i = 0; i < node->child_count; i++) {
        const md_node *row = node->children[i];
        int is_header = (row->type == MD_NODE_TABLE_HEADER);
        const char *tag = is_header ? "th" : "td";

        if (row->type != MD_NODE_TABLE_HEADER &&
            row->type != MD_NODE_TABLE_ROW) {
            continue;
        }
        /* All body rows share one <tbody>, so the table stays rectangular
         * for a consumer that counts sections. */
        if (!is_header && !in_body) {
            if (put(buf, "<tbody>\n") != 0) {
                return -1;
            }
            in_body = 1;
        }
        if (is_header && put(buf, "<thead>\n") != 0) {
            return -1;
        }
        for (j = 0; j < row->child_count; j++) {
            md_table_align align = MD_TABLE_ALIGN_NONE;

            if (row->children[j]->type != MD_NODE_TABLE_CELL) {
                continue;
            }
            /* Alignment is per column, so it is indexed by the cell's
             * position in the row, not by the row's position in the table. */
            if (j < node->align_count) {
                align = node->align[j];
            }
            if (put(buf, "<") != 0 || put(buf, tag) != 0) {
                return -1;
            }
            if (align != MD_TABLE_ALIGN_NONE) {
                const char *name = md_table_align_name(align);

                if (put(buf, " align=\"") != 0 || put(buf, name) != 0 ||
                    put(buf, "\"") != 0) {
                    return -1;
                }
            }
            if (put(buf, ">") != 0 ||
                render_node(buf, row->children[j], 0) != 0) {
                return -1;
            }
            if (put(buf, "</") != 0 || put(buf, tag) != 0 ||
                put(buf, ">\n") != 0) {
                return -1;
            }
        }
        if (is_header && put(buf, "</thead>\n") != 0) {
            return -1;
        }
    }
    if (in_body && put(buf, "</tbody>\n") != 0) {
        return -1;
    }
    return put(buf, "</table>\n");
}

/*
 * A footnote reference links to its definition, and a definition ends with a
 * back link to the same id. The id goes through the attribute escaper, so no
 * label in the source can inject markup.
 */
/*
 * The HTML id and href for a footnote. An id may hold a space or any other
 * character that is legal in an attribute but not in a fragment identifier,
 * so the normalized label is turned into a slug. The mapping is byte-wise and
 * fixed, so the reference and the definition, which share an id, always
 * agree on the slug, and neither can break out of the attribute.
 *
 * The rule itself is md_slug_write(), shared with the C6 table of contents.
 * It is asked not to lowercase, which is the one place the two differ: a
 * footnote id keeps the case of its label. The case was a matter of taste
 * when this was the only caller, and it is kept rather than changed, because
 * the HTML output is a contract the earlier checkpoints already shipped.
 *
 * A run of unkept bytes collapses to one '-', so two labels that differ only
 * in punctuation can share a slug. That is harmless: the slug is derived, not
 * an identifier, and the reference and the definition are always rendered
 * from the same id.
 */
#define put_footnote_slug(buf, id, len) md_slug_write((buf), (id), (len), 0)

static int render_footnote_ref(md_buffer *buf, const md_node *node)
{
    const char *id = node->id != NULL ? node->id : "";
    const char *label = node->label != NULL ? node->label : "";

    if (put(buf, "<sup class=\"footnote-ref\"><a href=\"#fn-") != 0 ||
        put_footnote_slug(buf, id, strlen(id)) != 0 ||
        put(buf, "\" id=\"fnref-") != 0 ||
        put_footnote_slug(buf, id, strlen(id)) != 0 || put(buf, "\">") != 0 ||
        escape_attr(buf, label, strlen(label)) != 0) {
        return -1;
    }
    return put(buf, "</a></sup>");
}

static int render_footnote_def(md_buffer *buf, const md_node *node)
{
    const char *id = node->id != NULL ? node->id : "";

    if (put(buf, "<div class=\"footnote\" id=\"fn-") != 0 ||
        put_footnote_slug(buf, id, strlen(id)) != 0 ||
        put(buf, "\">") != 0 || render_children(buf, node, 0) != 0 ||
        put(buf, "<a href=\"#fnref-") != 0 ||
        put_footnote_slug(buf, id, strlen(id)) != 0) {
        return -1;
    }
    return put(buf, "\" class=\"footnote-backref\">&#8617;</a></div>\n");
}

static int render_node(md_buffer *buf, const md_node *node, int tight)
{
    if (node == NULL) {
        md_set_error("invalid argument: null node");
        return -1;
    }
    switch (node->type) {
    case MD_NODE_DOCUMENT:
        return render_document(buf, node);
    case MD_NODE_PARAGRAPH:
        if (tight) {
            /* Tight list items hold bare text, with no paragraph wrapper. */
            return render_children(buf, node, 0);
        }
        if (put(buf, "<p>") != 0 || render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</p>\n");
    case MD_NODE_HEADING:
        return render_heading(buf, node);
    case MD_NODE_LIST:
        return render_list(buf, node);
    case MD_NODE_ITEM:
        /* A task item carries a checkbox before its content. An item that is
         * not a task item emits no checkbox, so a plain list stays exactly
         * the C3 output. */
        if (put(buf, "<li>") != 0) {
            return -1;
        }
        if (node->checked == 0 || node->checked == 1) {
            if (put(buf, "<input type=\"checkbox\" disabled=\"\"") != 0) {
                return -1;
            }
            if (node->checked == 1 && put(buf, " checked=\"\"") != 0) {
                return -1;
            }
            if (put(buf, " /> ") != 0) {
                return -1;
            }
        }
        if (render_children(buf, node, tight) != 0) {
            return -1;
        }
        return put(buf, "</li>\n");
    case MD_NODE_TABLE:
        return render_table(buf, node);
    case MD_NODE_TABLE_HEADER:
    case MD_NODE_TABLE_ROW:
    case MD_NODE_TABLE_CELL:
        /* Rows and cells are emitted by the table renderer, which is the
         * only place that knows the tag and the alignment. */
        return render_children(buf, node, 0);
    case MD_NODE_STRIKETHROUGH:
        if (put(buf, "<del>") != 0 || render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</del>");
    case MD_NODE_FOOTNOTE_REF:
        return render_footnote_ref(buf, node);
    case MD_NODE_FOOTNOTE_DEF:
        return render_footnote_def(buf, node);
    case MD_NODE_BLOCK_QUOTE:
        if (put(buf, "<blockquote>\n") != 0 ||
            render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</blockquote>\n");
    case MD_NODE_CODE_BLOCK:
        return render_code_block(buf, node);
    case MD_NODE_THEMATIC_BREAK:
        return put(buf, "<hr />\n");
    case MD_NODE_TEXT:
        return escape_text(buf, node->value != NULL ? node->value : "",
                           node->value != NULL ? strlen(node->value) : 0u);
    case MD_NODE_EM:
        if (put(buf, "<em>") != 0 || render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</em>");
    case MD_NODE_STRONG:
        if (put(buf, "<strong>") != 0 || render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</strong>");
    case MD_NODE_CODE:
        if (put(buf, "<code>") != 0) {
            return -1;
        }
        if (escape_text(buf, node->value != NULL ? node->value : "",
                        node->value != NULL ? strlen(node->value) : 0u) != 0) {
            return -1;
        }
        return put(buf, "</code>");
    case MD_NODE_LINK:
        if (put(buf, "<a href=\"") != 0) {
            return -1;
        }
        if (escape_attr(buf, node->destination != NULL ? node->destination : "",
                        node->destination != NULL ? strlen(node->destination) : 0u) != 0) {
            return -1;
        }
        if (put(buf, "\"") != 0 || put_attr(buf, "title", node->title) != 0 ||
            put(buf, ">") != 0 || render_children(buf, node, 0) != 0) {
            return -1;
        }
        return put(buf, "</a>");
    case MD_NODE_IMAGE:
        return render_image(buf, node);
    case MD_NODE_SOFTBREAK:
        return put(buf, "\n");
    case MD_NODE_HARDBREAK:
        return put(buf, "<br />\n");
    }
    md_set_error("unknown node type");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

char *md_render_html(md_arena *arena, const md_node *node)
{
    md_buffer buf;
    char *text;
    char *copy;

    if (arena == NULL || node == NULL) {
        md_set_error("invalid argument: null node");
        return NULL;
    }
    md_buffer_init(&buf);
    if (render_node(&buf, node, 0) != 0) {
        md_buffer_free(&buf);
        if (md_error_message()[0] == '\0') {
            md_set_error("failed to render HTML");
        }
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
