#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *md_node_type_name(md_node_type type)
{
    switch (type) {
    case MD_NODE_DOCUMENT:       return "document";
    case MD_NODE_PARAGRAPH:      return "paragraph";
    case MD_NODE_HEADING:        return "heading";
    case MD_NODE_LIST:           return "list";
    case MD_NODE_ITEM:           return "item";
    case MD_NODE_BLOCK_QUOTE:    return "blockquote";
    case MD_NODE_CODE_BLOCK:     return "code_block";
    case MD_NODE_THEMATIC_BREAK: return "thematic_break";
    case MD_NODE_TEXT:           return "text";
    case MD_NODE_EM:             return "em";
    case MD_NODE_STRONG:         return "strong";
    case MD_NODE_CODE:           return "code";
    case MD_NODE_LINK:           return "link";
    case MD_NODE_IMAGE:          return "image";
    case MD_NODE_SOFTBREAK:      return "softbreak";
    case MD_NODE_HARDBREAK:      return "hardbreak";
    case MD_NODE_TABLE:          return "table";
    case MD_NODE_TABLE_HEADER:   return "table_header";
    case MD_NODE_TABLE_ROW:      return "table_row";
    case MD_NODE_TABLE_CELL:     return "table_cell";
    case MD_NODE_STRIKETHROUGH:  return "strikethrough";
    case MD_NODE_FOOTNOTE_REF:   return "footnote_ref";
    case MD_NODE_FOOTNOTE_DEF:   return "footnote_def";
    }
    return "unknown";
}

const char *md_table_align_name(md_table_align align)
{
    switch (align) {
    case MD_TABLE_ALIGN_LEFT:
        return "left";
    case MD_TABLE_ALIGN_CENTER:
        return "center";
    case MD_TABLE_ALIGN_RIGHT:
        return "right";
    case MD_TABLE_ALIGN_NONE:
        break;
    }
    return "none";
}

md_node *md_node_new(md_arena *arena, md_node_type type)
{
    md_node *node;

    if (arena == NULL) {
        md_set_error("invalid argument: null arena");
        return NULL;
    }
    node = (md_node *)md_arena_calloc(arena, 1u, sizeof *node);
    if (node == NULL) {
        return NULL;
    }
    node->type = type;
    node->level = 0;
    node->ordered = 0;
    node->tight = 1;
    node->start = 1;
    node->info = NULL;
    node->literal = NULL;
    node->value = NULL;
    node->destination = NULL;
    node->title = NULL;
    /* -1 is "not a task item", which is what keeps an ordinary item out of
     * the "checked" key and out of the rendered checkbox. */
    node->checked = -1;
    node->id = NULL;
    node->label = NULL;
    node->align = NULL;
    node->align_count = 0;
    node->extensions = MD_EXT_NONE;
    node->footnotes = NULL;
    node->footnote_count = 0;
    node->footnote_capacity = 0;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    return node;
}

static md_status ensure_child_room(md_node *parent, md_arena *arena)
{
    size_t cap;
    md_node **grown;

    if (parent->child_count < parent->child_capacity) {
        return MD_OK;
    }
    cap = parent->child_capacity != 0 ? parent->child_capacity * 2u : 4u;
    if (cap < parent->child_capacity ||
        cap > ((size_t)-1) / sizeof(md_node *)) {
        md_set_error("too many children");
        return MD_ERR_LIMIT;
    }
    grown = (md_node **)md_arena_alloc(arena, cap * sizeof(md_node *));
    if (grown == NULL) {
        return MD_ERR_NOMEM;
    }
    if (parent->child_count > 0) {
        memcpy(grown, parent->children, parent->child_count * sizeof(md_node *));
    }
    parent->children = grown;
    parent->child_capacity = cap;
    return MD_OK;
}

md_status md_node_append(md_arena *arena, md_node *parent, md_node *child)
{
    md_status status;

    if (arena == NULL || parent == NULL || child == NULL) {
        md_set_error("invalid argument: null node");
        return MD_ERR_INVAL;
    }
    status = ensure_child_room(parent, arena);
    if (status != MD_OK) {
        return status;
    }
    parent->children[parent->child_count++] = child;
    return MD_OK;
}

md_status md_node_insert(md_arena *arena, md_node *parent, md_node *child,
                         size_t index)
{
    md_status status;

    if (arena == NULL || parent == NULL || child == NULL) {
        md_set_error("invalid argument: null node");
        return MD_ERR_INVAL;
    }
    if (index > parent->child_count) {
        md_set_error("invalid argument: child index");
        return MD_ERR_INVAL;
    }
    status = ensure_child_room(parent, arena);
    if (status != MD_OK) {
        return status;
    }
    if (index < parent->child_count) {
        memmove(&parent->children[index + 1u], &parent->children[index],
                (parent->child_count - index) * sizeof(md_node *));
    }
    parent->children[index] = child;
    parent->child_count++;
    return MD_OK;
}

md_node *md_make_text(md_arena *arena, const char *value)
{
    return md_make_text_n(arena, value, value != NULL ? strlen(value) : 0u);
}

md_node *md_make_text_n(md_arena *arena, const char *value, size_t len)
{
    md_node *node = md_node_new(arena, MD_NODE_TEXT);

    if (node == NULL) {
        return NULL;
    }
    if (value == NULL) {
        node->value = "";
        return node;
    }
    node->value = md_arena_strndup(arena, value, len);
    if (node->value == NULL) {
        return NULL;
    }
    return node;
}

md_node *md_make_code_block(md_arena *arena, const char *info, size_t info_len,
                            const char *literal, size_t literal_len)
{
    md_node *node = md_node_new(arena, MD_NODE_CODE_BLOCK);

    if (node == NULL) {
        return NULL;
    }
    node->info = md_arena_strndup(arena, info != NULL ? info : "", info_len);
    if (node->info == NULL) {
        return NULL;
    }
    node->literal = md_arena_strndup(arena, literal != NULL ? literal : "", literal_len);
    if (node->literal == NULL) {
        return NULL;
    }
    return node;
}

md_node *md_make_em(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_EM);
}

md_node *md_make_strong(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_STRONG);
}

md_node *md_make_code(md_arena *arena, const char *value, size_t len)
{
    md_node *node = md_node_new(arena, MD_NODE_CODE);

    if (node == NULL) {
        return NULL;
    }
    node->value = md_arena_strndup(arena, value != NULL ? value : "", len);
    if (node->value == NULL) {
        return NULL;
    }
    return node;
}

static md_node *make_linklike(md_arena *arena, md_node_type type,
                              const char *destination, const char *title)
{
    md_node *node = md_node_new(arena, type);

    if (node == NULL) {
        return NULL;
    }
    node->destination = md_arena_strdup(arena,
                                        destination != NULL ? destination : "");
    if (node->destination == NULL) {
        return NULL;
    }
    if (title != NULL) {
        node->title = md_arena_strdup(arena, title);
        if (node->title == NULL) {
            return NULL;
        }
    }
    return node;
}

md_node *md_make_link(md_arena *arena, const char *destination,
                      const char *title)
{
    return make_linklike(arena, MD_NODE_LINK, destination, title);
}

md_node *md_make_image(md_arena *arena, const char *destination,
                       const char *title)
{
    return make_linklike(arena, MD_NODE_IMAGE, destination, title);
}

md_node *md_make_softbreak(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_SOFTBREAK);
}

md_node *md_make_hardbreak(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_HARDBREAK);
}

/* ------------------------------------------------------------------ */
/* Extension nodes (checkpoint C4)                                    */
/* ------------------------------------------------------------------ */

md_node *md_make_table(md_arena *arena, const md_table_align *align,
                       size_t align_count)
{
    md_node *node = md_node_new(arena, MD_NODE_TABLE);

    if (node == NULL) {
        return NULL;
    }
    if (align != NULL && align_count > 0u) {
        node->align = (md_table_align *)md_arena_alloc(arena,
                                                       align_count * sizeof *align);
        if (node->align == NULL) {
            return NULL;
        }
        memcpy(node->align, align, align_count * sizeof *align);
        node->align_count = align_count;
    }
    return node;
}

md_node *md_make_table_header(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_TABLE_HEADER);
}

md_node *md_make_table_row(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_TABLE_ROW);
}

md_node *md_make_table_cell(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_TABLE_CELL);
}

md_node *md_make_strikethrough(md_arena *arena)
{
    return md_node_new(arena, MD_NODE_STRIKETHROUGH);
}

static md_node *make_footnote(md_arena *arena, md_node_type type,
                              const char *id, const char *label)
{
    md_node *node = md_node_new(arena, type);

    if (node == NULL) {
        return NULL;
    }
    node->id = md_arena_strdup(arena, id != NULL ? id : "");
    node->label = md_arena_strdup(arena, label != NULL ? label : "");
    if (node->id == NULL || node->label == NULL) {
        return NULL;
    }
    return node;
}

md_node *md_make_footnote_ref(md_arena *arena, const char *id,
                              const char *label)
{
    return make_footnote(arena, MD_NODE_FOOTNOTE_REF, id, label);
}

md_node *md_make_footnote_def(md_arena *arena, const char *id,
                              const char *label)
{
    return make_footnote(arena, MD_NODE_FOOTNOTE_DEF, id, label);
}

void md_node_set_task(md_node *item, int checked)
{
    if (item == NULL) {
        return;
    }
    /* Any value other than 0 or 1 means "not a task item", which is what
     * keeps an ordinary list item identical to its C1-C3 output. */
    item->checked = (checked == 0 || checked == 1) ? checked : -1;
}

md_status md_node_append_footnote(md_arena *arena, md_node *document,
                                  md_node *definition)
{
    if (arena == NULL || document == NULL || definition == NULL) {
        md_set_error("invalid argument: md_node_append_footnote");
        return MD_ERR_INVAL;
    }
    if (document->footnote_count == document->footnote_capacity) {
        size_t want = document->footnote_capacity
                          ? document->footnote_capacity * 2u
                          : 4u;
        md_node **grown = (md_node **)md_arena_alloc(arena,
                                                     want * sizeof *grown);

        if (grown == NULL) {
            return MD_ERR_NOMEM;
        }
        if (document->footnote_count > 0u) {
            memcpy(grown, document->footnotes,
                   document->footnote_count * sizeof *grown);
        }
        document->footnotes = grown;
        document->footnote_capacity = want;
    }
    document->footnotes[document->footnote_count++] = definition;
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Document                                                            */
/* ------------------------------------------------------------------ */

md_document *md_document_create(md_arena *arena, int owns_arena)
{
    md_document *doc;

    if (arena == NULL) {
        md_set_error("invalid argument: null arena");
        return NULL;
    }
    doc = (md_document *)md_arena_calloc(arena, 1u, sizeof *doc);
    if (doc == NULL) {
        if (owns_arena) {
            md_arena_destroy(arena);
        }
        return NULL;
    }
    doc->arena = arena;
    doc->owns_arena = owns_arena;
    doc->root = md_node_new(arena, MD_NODE_DOCUMENT);
    if (doc->root == NULL) {
        if (owns_arena) {
            md_arena_destroy(arena);
        }
        return NULL;
    }
    return doc;
}

void md_document_destroy(md_document *doc)
{
    md_arena *arena;

    if (doc == NULL) {
        return;
    }
    arena = doc->arena;
    if (doc->owns_arena) {
        md_arena_destroy(arena);
    }
    /* When the arena is caller-owned the md_document header itself dies with
     * the arena, so there is nothing further to release here. */
}

md_node *md_document_root(md_document *doc)
{
    return (doc != NULL) ? doc->root : NULL;
}

md_arena *md_document_arena(md_document *doc)
{
    return (doc != NULL) ? doc->arena : NULL;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

#define MD_CHILDREN_OPEN ",\"children\":["
#define MD_CHILDREN_EMPTY ",\"children\":[]"

/* Decimal of a size_t, into a caller-supplied buffer, NUL-terminated. */
static const char *md_utoa(char *out, size_t out_size, size_t value)
{
    char digits[3 * sizeof(size_t) + 1u];
    size_t n = 0;
    size_t i;

    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && n < sizeof digits);
    if (n >= out_size) {
        n = out_size - 1u;
    }
    for (i = 0; i < n; i++) {
        out[i] = digits[n - 1u - i];
    }
    out[n] = '\0';
    return out;
}

int md_json_write_string(md_buffer *buf, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (buf == NULL || (s == NULL && len > 0)) {
        return -1;
    }
    if (md_buffer_append_char(buf, '"') != 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':  if (md_buffer_append(buf, "\\\"", 2u) != 0) return -1; break;
        case '\\': if (md_buffer_append(buf, "\\\\", 2u) != 0) return -1; break;
        case '\b': if (md_buffer_append(buf, "\\b", 2u) != 0) return -1; break;
        case '\f': if (md_buffer_append(buf, "\\f", 2u) != 0) return -1; break;
        case '\n': if (md_buffer_append(buf, "\\n", 2u) != 0) return -1; break;
        case '\r': if (md_buffer_append(buf, "\\r", 2u) != 0) return -1; break;
        case '\t': if (md_buffer_append(buf, "\\t", 2u) != 0) return -1; break;
        default:
            if (c < 0x20u) {
                char esc[6];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(c >> 4) & 0x0Fu];
                esc[5] = hex[c & 0x0Fu];
                if (md_buffer_append(buf, esc, sizeof esc) != 0) return -1;
            } else {
                /* Bytes >= 0x20 (including UTF-8 continuations) pass through. */
                if (md_buffer_append_char(buf, (char)c) != 0) return -1;
            }
            break;
        }
    }
    return md_buffer_append_char(buf, '"');
}

static int json_key(md_buffer *buf, const char *key, int first)
{
    if (!first && md_buffer_append_char(buf, ',') != 0) {
        return -1;
    }
    if (md_json_write_string(buf, key, strlen(key)) != 0) {
        return -1;
    }
    return md_buffer_append_char(buf, ':');
}

static int json_string_field(md_buffer *buf, const char *key, const char *value,
                             int first)
{
    if (json_key(buf, key, first) != 0) {
        return -1;
    }
    return md_json_write_string(buf, value != NULL ? value : "",
                                value != NULL ? strlen(value) : 0u);
}

static int json_number_field(md_buffer *buf, const char *key, long value, int first)
{
    char num[32];

    if (json_key(buf, key, first) != 0) {
        return -1;
    }
    if (snprintf(num, sizeof num, "%ld", value) < 0) {
        md_set_error("failed to format JSON number");
        return -1;
    }
    return md_buffer_append_cstr(buf, num);
}

static int json_bool_field(md_buffer *buf, const char *key, int value, int first)
{
    if (json_key(buf, key, first) != 0) {
        return -1;
    }
    return md_buffer_append_cstr(buf, value ? "true" : "false");
}

static int json_write_node(md_buffer *buf, const md_node *node){
    size_t i;

    if (buf == NULL || node == NULL) {
        md_set_error("invalid argument: null node");
        return -1;
    }
    if (md_buffer_append_char(buf, '{') != 0) {
        return -1;
    }
    {
        const char *name = md_node_type_name(node->type);

        if (json_key(buf, "type", 1) != 0 ||
            md_json_write_string(buf, name, strlen(name)) != 0) {
            return -1;
        }
    }

    switch (node->type) {
    case MD_NODE_HEADING:
        if (json_number_field(buf, "level", (long)node->level, 0) != 0) return -1;
        break;
    case MD_NODE_LIST:
        if (json_bool_field(buf, "ordered", node->ordered, 0) != 0) return -1;
        if (json_number_field(buf, "start", node->start, 0) != 0) return -1;
        if (json_bool_field(buf, "tight", node->tight, 0) != 0) return -1;
        break;
    case MD_NODE_TABLE: {
        /* "align" lists one entry per column, "none" when unspecified. */
        if (json_key(buf, "align", 0) != 0) return -1;
        if (md_buffer_append_char(buf, '[') != 0) return -1;
        for (i = 0; i < node->align_count; i++) {
            const char *name = md_table_align_name(node->align[i]);

            if (i > 0 && md_buffer_append_char(buf, ',') != 0) return -1;
            if (md_json_write_string(buf, name, strlen(name)) != 0) return -1;
        }
        if (md_buffer_append_char(buf, ']') != 0) return -1;
        break;
    }
    case MD_NODE_ITEM:
        /* "checked" appears only on a task-list item, so a plain item keeps
         * the exact key set it had in C1-C3. */
        if (node->checked == 0 || node->checked == 1) {
            if (json_bool_field(buf, "checked", node->checked, 0) != 0) return -1;
        }
        break;
    case MD_NODE_FOOTNOTE_REF:
        if (json_string_field(buf, "id", node->id, 0) != 0) return -1;
        if (json_string_field(buf, "label", node->label, 0) != 0) return -1;
        return md_buffer_append_char(buf, '}');
    case MD_NODE_FOOTNOTE_DEF:
        if (json_string_field(buf, "id", node->id, 0) != 0) return -1;
        if (json_string_field(buf, "label", node->label, 0) != 0) return -1;
        break;
    case MD_NODE_CODE_BLOCK:
        if (json_string_field(buf, "info", node->info, 0) != 0) return -1;
        if (json_string_field(buf, "literal", node->literal, 0) != 0) return -1;
        if (md_buffer_append(buf, MD_CHILDREN_EMPTY,
                             sizeof MD_CHILDREN_EMPTY - 1u) != 0) return -1;
        return md_buffer_append_char(buf, '}');
    case MD_NODE_THEMATIC_BREAK:
        return md_buffer_append_char(buf, '}');
    case MD_NODE_SOFTBREAK:
    case MD_NODE_HARDBREAK:
        return md_buffer_append_char(buf, '}');
    case MD_NODE_TEXT:
    case MD_NODE_CODE:
        if (json_string_field(buf, "value", node->value, 0) != 0) return -1;
        return md_buffer_append_char(buf, '}');
    case MD_NODE_LINK:
    case MD_NODE_IMAGE:
        /* `title` is always emitted; it is "" when the source had no title. */
        if (json_string_field(buf, "destination", node->destination, 0) != 0) return -1;
        if (json_string_field(buf, "title", node->title, 0) != 0) return -1;
        break;
    case MD_NODE_DOCUMENT:
        /*
         * "footnotes" is emitted before "children" and only when the parse
         * enabled the extension, so a document that enabled no extension
         * still produces the exact JSON it produced before C4. The
         * definitions are a sibling key, not a replacement for the block
         * flow: the ordinary "children" tail below always runs, so both
         * keys are present and the object is well formed.
         */
        if ((node->extensions & MD_EXT_FOOTNOTES) != 0u) {
            if (json_key(buf, "footnotes", 0) != 0) return -1;
            if (md_buffer_append_char(buf, '[') != 0) return -1;
            for (i = 0; i < node->footnote_count; i++) {
                if (i > 0 && md_buffer_append_char(buf, ',') != 0) return -1;
                if (json_write_node(buf, node->footnotes[i]) != 0) return -1;
            }
            if (md_buffer_append_char(buf, ']') != 0) return -1;
        }
        break;
    case MD_NODE_PARAGRAPH:
    case MD_NODE_BLOCK_QUOTE:
    case MD_NODE_TABLE_HEADER:
    case MD_NODE_TABLE_ROW:
    case MD_NODE_TABLE_CELL:
    case MD_NODE_EM:
    case MD_NODE_STRONG:
    case MD_NODE_STRIKETHROUGH:
        break;
    }

    if (md_buffer_append(buf, MD_CHILDREN_OPEN, sizeof MD_CHILDREN_OPEN - 1u) != 0) {
        return -1;
    }
    for (i = 0; i < node->child_count; i++) {
        if (i > 0 && md_buffer_append_char(buf, ',') != 0) {
            return -1;
        }
        if (json_write_node(buf, node->children[i]) != 0) {
            return -1;
        }
    }
    if (md_buffer_append(buf, "]}", 2u) != 0) {
        return -1;
    }
    return 0;
}

char *md_error_to_json(md_arena *arena, const md_error *error)
{
    md_buffer buf;
    char *text;
    char *copy;
    const md_error *err = error != NULL ? error : md_last_error();
    char digits[3 * sizeof(size_t) + 1u];

    if (arena == NULL) {
        md_set_error("invalid argument: null arena");
        return NULL;
    }
    md_buffer_init(&buf);
    if (md_buffer_append_cstr(&buf, "{\"error\":{\"line\":") != 0 ||
        md_buffer_append_cstr(&buf, md_utoa(digits, sizeof digits, err->line)) != 0 ||
        md_buffer_append_cstr(&buf, ",\"col\":") != 0 ||
        md_buffer_append_cstr(&buf, md_utoa(digits, sizeof digits, err->col)) != 0 ||
        md_buffer_append_cstr(&buf, ",\"message\":") != 0 ||
        md_json_write_string(&buf, err->message, strlen(err->message)) != 0 ||
        md_buffer_append_cstr(&buf, "}}") != 0) {
        md_buffer_free(&buf);
        if (md_error_message()[0] == '\0') {
            md_set_error("failed to serialize error to JSON");
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

char *md_ast_to_json(md_arena *arena, const md_node *node)
{
    md_buffer buf;
    char *text;
    char *copy;

    if (arena == NULL || node == NULL) {
        md_set_error("invalid argument: null node");
        return NULL;
    }
    md_buffer_init(&buf);
    if (json_write_node(&buf, node) != 0) {
        md_buffer_free(&buf);
        if (md_error_message()[0] == '\0') {
            md_set_error("failed to serialize AST to JSON");
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
