#include "inlines.h"

#include "entities.h"
#include "md.h"
#include "status.h"

#include <stdlib.h>
#include <string.h>

#define MD_NPOS ((size_t)-1)

/* ------------------------------------------------------------------ */
/* Character classes                                                   */
/* ------------------------------------------------------------------ */

static int is_ascii_punct(char c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

/* Decode the code point at pos; invalid bytes decode to themselves so that
 * every input byte is preserved. *width receives the byte length. */
static unsigned long decode_at(const char *s, size_t len, size_t pos,
                               size_t *width)
{
    unsigned char c;

    if (pos >= len) {
        *width = 0;
        return 0;
    }
    c = (unsigned char)s[pos];
    if (c < 0x80u) {
        *width = 1;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u && pos + 1u < len &&
        ((unsigned char)s[pos + 1u] & 0xC0u) == 0x80u) {
        *width = 2;
        return ((unsigned long)(c & 0x1Fu) << 6) |
               (unsigned long)((unsigned char)s[pos + 1u] & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && pos + 2u < len &&
        ((unsigned char)s[pos + 1u] & 0xC0u) == 0x80u &&
        ((unsigned char)s[pos + 2u] & 0xC0u) == 0x80u) {
        *width = 3;
        return ((unsigned long)(c & 0x0Fu) << 12) |
               ((unsigned long)((unsigned char)s[pos + 1u] & 0x3Fu) << 6) |
               (unsigned long)((unsigned char)s[pos + 2u] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && pos + 3u < len &&
        ((unsigned char)s[pos + 1u] & 0xC0u) == 0x80u &&
        ((unsigned char)s[pos + 2u] & 0xC0u) == 0x80u &&
        ((unsigned char)s[pos + 3u] & 0xC0u) == 0x80u) {
        *width = 4;
        return ((unsigned long)(c & 0x07u) << 18) |
               ((unsigned long)((unsigned char)s[pos + 1u] & 0x3Fu) << 12) |
               ((unsigned long)((unsigned char)s[pos + 2u] & 0x3Fu) << 6) |
               (unsigned long)((unsigned char)s[pos + 3u] & 0x3Fu);
    }
    *width = 1;
    return c;
}

/* Start offset of the code point that ends at pos (pos > 0). */
static size_t prev_char_start(const char *s, size_t pos)
{
    size_t i = pos - 1u;
    size_t back = 0;

    while (i > 0 && back < 3u && ((unsigned char)s[i] & 0xC0u) == 0x80u) {
        i--;
        back++;
    }
    return i;
}

static int is_unicode_space(unsigned long cp)
{
    if (cp == 0x20ul || (cp >= 0x09ul && cp <= 0x0Dul)) {
        return 1;
    }
    return cp == 0x85ul || cp == 0xA0ul || cp == 0x1680ul ||
           (cp >= 0x2000ul && cp <= 0x200Aul) || cp == 0x2028ul ||
           cp == 0x2029ul || cp == 0x202Ful || cp == 0x205Ful || cp == 0x3000ul;
}

/* Approximation of CommonMark's "Unicode punctuation" class. */
static int is_unicode_punct(unsigned long cp)
{
    if (cp < 0x80ul) {
        return is_ascii_punct((char)cp);
    }
    if ((cp >= 0xA1ul && cp <= 0xBFul && cp != 0xA7ul) ||
        (cp >= 0x2010ul && cp <= 0x2027ul) ||
        (cp >= 0x2030ul && cp <= 0x205Eul) ||
        (cp >= 0x3001ul && cp <= 0x303Ful) ||
        (cp >= 0xFF01ul && cp <= 0xFF65ul)) {
        return 1;
    }
    return 0;
}

static int is_space_at(const char *s, size_t len, size_t pos)
{
    size_t width = 0;

    if (pos >= len) {
        return 0;
    }
    return is_unicode_space(decode_at(s, len, pos, &width));
}

static int is_space_before(const char *s, size_t len, size_t pos)
{
    size_t width = 0;

    if (pos == 0) {
        return 1; /* the start of input counts as whitespace */
    }
    return is_unicode_space(decode_at(s, len, prev_char_start(s, pos), &width));
}

static int is_punct_at(const char *s, size_t len, size_t pos)
{
    size_t width = 0;

    if (pos >= len) {
        return 0;
    }
    return is_unicode_punct(decode_at(s, len, pos, &width));
}

static int is_punct_before(const char *s, size_t len, size_t pos)
{
    size_t width = 0;

    if (pos == 0) {
        return 0;
    }
    return is_unicode_punct(decode_at(s, len, prev_char_start(s, pos), &width));
}

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

/* Upper bound on remembered delimiter runs per inline run. */
#define MD_MAX_DELIMS 64u

/* An unmatched delimiter run waiting for a closer. */
typedef struct {
    char c;
    unsigned count;      /* delimiters of the run still available */
    size_t child_index;  /* where the run's content starts in `parent` */
    int can_open;
    int can_close;
} md_delim;

typedef struct {
    md_arena *arena;
    const char *text;
    size_t len;   /* buffer length */
    size_t limit; /* the parser never reads at or beyond this offset */
    size_t pos;
    md_refmap *refs;
    md_buffer pending; /* literal text seen since the last emitted node */
    unsigned depth;
    unsigned max_nesting; /* resolved ceiling for bracket nesting */
    size_t err_off;       /* offset of a refused construct, in `text` */
    int err_set;
    size_t last_close;    /* last ']' that can close a label, or MD_NPOS */
    int last_close_known;
    md_delim delims[MD_MAX_DELIMS];
    size_t ndelims;
    const md_options *opts; /* C4 extensions and custom registry */
} md_inline_ctx;

/*
 * Record the first refused construct and the reason.
 *
 * The kind and the wording are set here, where the decision is made, so the
 * caller only has to carry the position. MD_ERR_LIMIT is the status that
 * goes with MD_ERROR_NESTING, as it is for the block parser.
 */
static void inl_note_error(md_inline_ctx *ctx, size_t off)
{
    if (ctx->err_set) {
        return;
    }
    if (off > ctx->len) {
        off = ctx->len;
    }
    ctx->err_off = off;
    ctx->err_set = 1;
    md_set_error_kind(MD_ERROR_NESTING, "nesting too deep");
}

/* The ceiling for bracket nesting: the caller's configured limit, already
 * clamped to MD_MAX_NESTING_HARD, defaulting to MD_MAX_NESTING. */
static unsigned inl_max_nesting(const md_inline_ctx *ctx)
{
    return md_limits_nesting(ctx->opts);
}

/* Extension mask of a context; NULL options mean no extensions at all. */
static md_ext_flags inl_ext(const md_inline_ctx *ctx)
{
    return ctx->opts != NULL ? ctx->opts->extensions : MD_EXT_NONE;
}

/* Merge neighbouring text children so literals stay a single node. */
static md_status coalesce_text(md_arena *arena, md_node *parent)
{
    size_t i = 0;
    md_status status;

    while (i < parent->child_count) {
        md_node *child = parent->children[i];

        if (child->type != MD_NODE_TEXT) {
            status = coalesce_text(arena, child);
            if (status != MD_OK) {
                return status;
            }
            i++;
            continue;
        }
        {
            size_t j = i + 1u;
            size_t total = 0;
            size_t k;
            char *merged;
            md_node *node;

            while (j < parent->child_count &&
                   parent->children[j]->type == MD_NODE_TEXT) {
                total += strlen(parent->children[j]->value);
                j++;
            }
            if (j - i == 1u) {
                i = j;
                continue;
            }
            total += strlen(child->value);
            merged = (char *)md_arena_alloc(arena, total + 1u);
            if (merged == NULL) {
                return MD_ERR_NOMEM;
            }
            for (k = i; k < j; k++) {
                size_t n = strlen(parent->children[k]->value);

                memcpy(merged, parent->children[k]->value, n);
                merged += n;
            }
            merged -= total;
            merged[total] = '\0';
            node = md_make_text(arena, merged);
            if (node == NULL) {
                return MD_ERR_NOMEM;
            }
            parent->children[i] = node;
            memmove(&parent->children[i + 1u], &parent->children[j],
                    (parent->child_count - j) * sizeof(md_node *));
            parent->child_count -= (j - i - 1u);
        }
    }
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Extension inline constructs (checkpoint C4)                         */
/* ------------------------------------------------------------------ */

static md_status parse_nested(md_inline_ctx *ctx, md_node *node, size_t start,
                              size_t stop);
static md_status emit_literal(md_inline_ctx *ctx, size_t from, size_t len);
static md_status flush_pending(md_inline_ctx *ctx, md_node *parent);

/* Find the next unescaped occurrence of `needle` in text[start..limit). */
static size_t find_closer(const md_inline_ctx *ctx, size_t start,
                          const char *needle, size_t nlen)
{
    size_t i = start;

    while (i + nlen <= ctx->limit) {
        if (ctx->text[i] == '\\') {
            i += 2u;
            continue;
        }
        if (memcmp(ctx->text + i, needle, nlen) == 0) {
            return i;
        }
        i++;
    }
    return ctx->limit;
}

/*
 * "~~struck~~". A closer that is missing, escaped, or empty leaves the
 * opening marker as literal text, so no text is ever lost.
 */
static md_status parse_strikethrough(md_inline_ctx *ctx, md_node *parent)
{
    size_t start = ctx->pos;
    size_t closer;
    md_node *node;
    md_status status;

    if (start + 4u > ctx->limit || ctx->text[start + 1u] != '~') {
        return emit_literal(ctx, start, 1u);
    }
    closer = find_closer(ctx, start + 2u, "~~", 2u);
    if (closer >= ctx->limit || closer == start + 2u) {
        return emit_literal(ctx, start, 2u);
    }
    node = md_make_strikethrough(ctx->arena);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = flush_pending(ctx, parent);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(ctx->arena, parent, node);
    if (status != MD_OK) {
        return status;
    }
    status = parse_nested(ctx, node, start + 2u, closer);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos = closer + 2u;
    return MD_OK;
}

/*
 * "[^label]". A reference to a label that no definition provides stays
 * literal text, which is the rule reference links follow. The id is the
 * normalized label, so a back link and its reference agree. When the
 * position is not a footnote reference the function declines without
 * touching the cursor or the output, leaving the '[' to the link parser.
 */
static md_status parse_footnote_ref(md_inline_ctx *ctx, md_node *parent,
                                    int *consumed)
{
    size_t start = ctx->pos;
    size_t label_off = 0;
    size_t label_len = 0;
    size_t end = 0;
    char *id;
    char *label;
    md_node *node;
    md_status status;

    *consumed = 0;
    if (!md_ext_footnote_label(ctx->text, ctx->limit, start, &label_off,
                               &label_len, &end)) {
        return MD_OK;
    }
    if (ctx->refs == NULL ||
        md_refmap_lookup(ctx->refs, ctx->text + label_off, label_len) == NULL) {
        return MD_OK;
    }
    id = md_ext_footnote_id(ctx->arena, ctx->text + label_off, label_len);
    if (id == NULL) {
        return MD_ERR_NOMEM;
    }
    label = md_arena_strndup(ctx->arena, ctx->text + label_off, label_len);
    if (label == NULL) {
        return MD_ERR_NOMEM;
    }
    node = md_make_footnote_ref(ctx->arena, id, label);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = flush_pending(ctx, parent);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(ctx->arena, parent, node);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos = end;
    *consumed = 1;
    return MD_OK;
}

/*
 * Offer the position to every registered inline extension, in registration
 * order, stopping at the first one that consumes input. The bytes it
 * consumed are never rescanned, which bounds the work per position.
 */
static md_status run_registered_inlines(md_inline_ctx *ctx, md_node *parent)
{
    md_ext_registry *registry;
    size_t start = ctx->pos;
    size_t i;
    md_status status;

    if (ctx->opts == NULL || ctx->opts->custom == NULL) {
        return emit_literal(ctx, start, 1u);
    }
    /* A handler appends its own nodes, so the text queued so far has to be
     * written to the tree first or the handler's node would come out before
     * the text that precedes it. */
    status = flush_pending(ctx, parent);
    if (status != MD_OK) {
        return status;
    }
    registry = ctx->opts->custom;
    for (i = 0; i < md_ext_registry_inline_count(registry); i++) {
        md_ext_env env;
        size_t consumed = 0;

        env.arena = ctx->arena;
        env.flags = inl_ext(ctx);
        env.options = ctx->opts;
        status = md_ext_invoke_inline(registry, i, &env, parent, ctx->text,
                                      ctx->limit, start, &consumed);
        if (status != MD_OK) {
            return status;
        }
        if (consumed > 0u) {
            if (start + consumed > ctx->limit) {
                md_set_error("extension consumed past the end of the run");
                return MD_ERR_PARSE;
            }
            ctx->pos = start + consumed;
            return MD_OK;
        }
    }
    return emit_literal(ctx, start, 1u);
}

static md_status inline_run(md_inline_ctx *ctx, md_node *parent);

/* Queue len bytes of literal text starting at `from`, then skip them. */
static md_status emit_literal(md_inline_ctx *ctx,
                              size_t from, size_t len)
{
    if (len == 0u) {
        return MD_OK;
    }
    if (md_buffer_append(&ctx->pending, ctx->text + from, len) != 0) {
        return MD_ERR_NOMEM;
    }
    ctx->pos = from + len;
    return MD_OK;
}

/* Queue already-decoded bytes without touching the cursor. */
static md_status emit_bytes(md_inline_ctx *ctx, const char *s, size_t len)
{
    if (len == 0u) {
        return MD_OK;
    }
    return (md_buffer_append(&ctx->pending, s, len) == 0) ? MD_OK : MD_ERR_NOMEM;
}

/* Merge `s` into the text node at `index`, inserting a new one if needed. */
static md_status merge_text_at(md_arena *arena, md_node *parent, size_t index,
                               const char *s, size_t len, int at_front)
{
    md_node *node;
    md_node *target;
    size_t have;
    size_t total;
    char *merged;

    if (index < parent->child_count) {
        target = parent->children[index];
    } else {
        node = md_make_text_n(arena, s, len);
        if (node == NULL) {
            return MD_ERR_NOMEM;
        }
        return md_node_append(arena, parent, node);
    }
    if (target->type != MD_NODE_TEXT) {
        node = md_make_text_n(arena, s, len);
        if (node == NULL) {
            return MD_ERR_NOMEM;
        }
        return md_node_insert(arena, parent, node, index);
    }
    /* Arena text nodes are immutable: allocate the combined value once. */
    have = target->value != NULL ? strlen(target->value) : 0u;
    total = have + len;
    merged = (char *)md_arena_alloc(arena, total + 1u);
    if (merged == NULL) {
        return MD_ERR_NOMEM;
    }
    if (have > 0u) {
        memcpy(merged + (at_front ? len : 0u), target->value, have);
    }
    if (len > 0u) {
        memcpy(merged + (at_front ? 0u : have), s, len);
    }
    merged[total] = '\0';
    node = md_make_text(arena, merged);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    parent->children[index] = node;
    return MD_OK;
}

/* Queue a text node, merging into the last child when it is already text. */
static md_status append_text_node(md_arena *arena, md_node *parent,
                                  const char *s, size_t len)
{
    if (parent->child_count > 0u &&
        parent->children[parent->child_count - 1u]->type == MD_NODE_TEXT) {
        return merge_text_at(arena, parent, parent->child_count - 1u, s, len, 0);
    }
    return merge_text_at(arena, parent, parent->child_count, s, len, 0);
}

/* Move the queued literal run into parent as a text node. */
static md_status flush_pending(md_inline_ctx *ctx, md_node *parent)
{
    md_status status;

    if (ctx->pending.len == 0u) {
        return MD_OK;
    }
    status = append_text_node(ctx->arena, parent, ctx->pending.data,
                              ctx->pending.len);
    ctx->pending.len = 0u;
    return status;
}

/*
 * Move the queued literal run into parent without merging it into the
 * previous child. Delimiter handling uses this so that a remembered run
 * keeps pointing at its own content.
 */
static md_status flush_pending_split(md_inline_ctx *ctx, md_node *parent)
{
    md_node *node;
    md_status status;

    if (ctx->pending.len == 0u) {
        return MD_OK;
    }
    node = md_make_text_n(ctx->arena, ctx->pending.data, ctx->pending.len);
    ctx->pending.len = 0u;
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, node);
    return status;
}


/* Run a nested inline parse over [start, stop) into `node`. */
/*
 * Parse text[start..stop) into `node`. The cursor, limit, depth, pending text
 * buffer and the delimiter stack are saved and restored, so an outer run keeps
 * its queued literal text and its openers, and delimiters never pair across a
 * link label boundary.
 */
static md_status parse_nested(md_inline_ctx *ctx, md_node *node, size_t start,
                              size_t stop)
{
    size_t saved_pos = ctx->pos;
    size_t saved_limit = ctx->limit;
    size_t saved_ndelims = ctx->ndelims;
    unsigned saved_depth = ctx->depth;
    md_buffer saved_pending = ctx->pending;
    md_status status;

    if (ctx->depth >= MD_INLINE_MAX_NESTING || start >= stop) {
        return MD_OK;
    }
    ctx->pos = start;
    ctx->limit = stop;
    /* The cached "last closer" is an answer about [0, limit), so narrowing
     * the scope invalidates it. */
    ctx->last_close_known = 0;
    ctx->depth = saved_depth + 1u;
    ctx->ndelims = 0;
    /* inline_run() owns the pending buffer it is given and frees it before
     * returning, so the nested run gets one of its own. */
    ctx->pending.data = NULL;
    ctx->pending.len = 0;
    ctx->pending.cap = 0;
    ctx->pending.failed = 0;
    status = inline_run(ctx, node);
    ctx->pending = saved_pending;
    ctx->pos = saved_pos;
    ctx->limit = saved_limit;
    ctx->depth = saved_depth;
    ctx->ndelims = saved_ndelims;
    return status;
}

/* ------------------------------------------------------------------ */
/* C1 text handling, kept for md_parse_blocks                          */
/* ------------------------------------------------------------------ */

md_node *md_inline_text(md_arena *arena, const char *src, size_t len)
{
    if (arena == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_inline_text");
        return NULL;
    }
    if (src == NULL) {
        src = "";
        len = 0;
    }
    while (len > 0 && src[0] == ' ') {
        src++;
        len--;
    }
    while (len > 0 && src[len - 1u] == ' ') {
        len--;
    }
    return md_make_text_n(arena, src, len);
}

md_status md_inline_lines_raw(md_arena *arena, const char *text,
                              const md_span *lines, size_t count,
                              md_node *parent)
{
    md_buffer buf;
    char *raw;
    md_node *node;
    size_t i;

    if (arena == NULL || text == NULL || parent == NULL ||
        (lines == NULL && count > 0)) {
        md_set_error("invalid argument: md_inline_lines_raw");
        return MD_ERR_INVAL;
    }
    md_buffer_init(&buf);
    for (i = 0; i < count; i++) {
        const char *p = text + lines[i].off;
        size_t len = md_line_rtrim_len(p, lines[i].len);

        if (i == 0) {
            size_t skip = 0;

            while (skip < len && p[skip] == ' ') {
                skip++;
            }
            p += skip;
            len -= skip;
        }
        if (md_buffer_append(&buf, p, len) != 0) {
            md_buffer_free(&buf);
            return MD_ERR_NOMEM;
        }
        if (i + 1u < count && md_buffer_append_char(&buf, '\n') != 0) {
            md_buffer_free(&buf);
            return MD_ERR_NOMEM;
        }
    }
    raw = md_buffer_release(&buf);
    if (raw == NULL) {
        return MD_ERR_NOMEM;
    }
    node = md_make_text_n(arena, raw, strlen(raw));
    free(raw);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    if (node->value[0] == '\0') {
        /* Empty content (e.g. "#") produces no child at all. */
        return MD_OK;
    }
    return md_node_append(arena, parent, node);
}

/* Join content lines. Leading spaces go away; trailing spaces stay because
 * they decide between a softbreak and a hardbreak. */
static char *join_lines(const char *text, const md_span *lines, size_t count,
                        size_t *out_len)
{
    md_buffer buf;
    char *raw;
    size_t i;

    md_buffer_init(&buf);
    for (i = 0; i < count; i++) {
        const char *p = text + lines[i].off;
        size_t len = lines[i].len;

        while (len > 0 && p[0] == ' ') {
            p++;
            len--;
        }
        if (md_buffer_append(&buf, p, len) != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
        if (i + 1u < count && md_buffer_append_char(&buf, '\n') != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
    }
    *out_len = buf.len; /* release() resets the buffer */
    raw = md_buffer_release(&buf);
    if (raw == NULL) {
        *out_len = 0;
    }
    return raw;
}

/* ------------------------------------------------------------------ */
/* Escapes                                                             */
/* ------------------------------------------------------------------ */

char *md_inline_unescape(md_arena *arena, const char *src, size_t off,
                         size_t len)
{
    md_buffer buf;
    char *raw;
    char *out;
    size_t i = 0;

    if (arena == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_inline_unescape");
        return NULL;
    }
    if (src == NULL) {
        len = 0;
    }
    md_buffer_init(&buf);
    while (i < len) {
        char c = src[off + i];

        if (c == '\\' && i + 1u < len && is_ascii_punct(src[off + i + 1u])) {
            c = src[off + i + 1u];
            i += 2u;
        } else {
            i++;
        }
        if (md_buffer_append_char(&buf, c) != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
    }
    raw = md_buffer_release(&buf);
    if (raw == NULL) {
        return NULL;
    }
    out = md_arena_strndup(arena, raw, strlen(raw));
    free(raw);
    return out;
}

/* ------------------------------------------------------------------ */
/* Code spans                                                          */
/* ------------------------------------------------------------------ */

static size_t backtick_run(const md_inline_ctx *ctx, size_t pos)
{
    size_t n = 0;

    while (pos + n < ctx->limit && ctx->text[pos + n] == '`') {
        n++;
    }
    return n;
}

/*
 * When a code span starts at pos, store its content span and return the
 * offset just past the closing run; otherwise return MD_NPOS and the
 * backticks are literal text.
 */
static size_t scan_code_span(const md_inline_ctx *ctx, size_t pos,
                             size_t *content_off, size_t *content_len)
{
    size_t open = backtick_run(ctx, pos);
    size_t q;

    if (open == 0u) {
        return MD_NPOS;
    }
    q = pos + open;
    while (q < ctx->limit) {
        size_t run;

        if (ctx->text[q] != '`') {
            q++;
            continue;
        }
        run = backtick_run(ctx, q);
        if (run == open) {
            *content_off = pos + open;
            *content_len = q - (pos + open);
            return q + run;
        }
        q += run;
    }
    return MD_NPOS;
}

static md_status parse_code_span(md_inline_ctx *ctx, md_node *parent)
{
    size_t content_off = 0;
    size_t content_len = 0;
    size_t end = scan_code_span(ctx, ctx->pos, &content_off, &content_len);
    md_buffer buf;
    char *raw;
    md_node *node;
    size_t i;

    if (end == MD_NPOS) {
        return emit_literal(ctx, ctx->pos, backtick_run(ctx, ctx->pos));
    }
    md_buffer_init(&buf);
    for (i = 0; i < content_len; i++) {
        char c = ctx->text[content_off + i];

        if (c == '\n') {
            c = ' '; /* a line ending inside a code span becomes a space */
        }
        if (md_buffer_append_char(&buf, c) != 0) {
            md_buffer_free(&buf);
            return MD_ERR_NOMEM;
        }
    }
    /* A single space is stripped from each end when both are present. */
    if (buf.len >= 2u && buf.data[0] == ' ' && buf.data[buf.len - 1u] == ' ') {
        int only_spaces = 1;
        size_t k;

        for (k = 1u; k + 1u < buf.len; k++) {
            if (buf.data[k] != ' ') {
                only_spaces = 0;
                break;
            }
        }
        if (!only_spaces) {
            memmove(buf.data, buf.data + 1, buf.len - 2u);
            buf.len -= 2u;
        }
    }
    raw = md_buffer_release(&buf);
    if (raw == NULL) {
        return MD_ERR_NOMEM;
    }
    node = md_make_code(ctx->arena, raw, strlen(raw));
    free(raw);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    /* The queued literal text belongs before the code node. */
    {
        md_status status = flush_pending(ctx, parent);

        if (status != MD_OK) {
            return status;
        }
    }
    ctx->pos = end;
    return md_node_append(ctx->arena, parent, node);
}

/* ------------------------------------------------------------------ */
/* Line breaks                                                         */
/* ------------------------------------------------------------------ */

static md_status emit_break(md_inline_ctx *ctx, md_node *parent, int hard)
{
    size_t spaces = 0;
    md_node *node;
    md_status status;

    while (spaces < ctx->pending.len &&
           ctx->pending.data[ctx->pending.len - 1u - spaces] == ' ') {
        spaces++;
    }
    ctx->pending.len -= spaces;
    node = hard ? md_make_hardbreak(ctx->arena) : md_make_softbreak(ctx->arena);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = flush_pending(ctx, parent);
    if (status != MD_OK) {
        return status;
    }
    return md_node_append(ctx->arena, parent, node);
}

static md_status parse_break(md_inline_ctx *ctx, md_node *parent)
{
    size_t spaces = 0;

    while (spaces < ctx->pending.len &&
           ctx->pending.data[ctx->pending.len - 1u - spaces] == ' ') {
        spaces++;
    }
    ctx->pos++;
    return emit_break(ctx, parent, spaces >= 2u);
}

/* ------------------------------------------------------------------ */
/* Emphasis                                                            */
/* ------------------------------------------------------------------ */

static void delim_flanking(const md_inline_ctx *ctx, size_t pos, size_t n,
                           char c, int *can_open, int *can_close)
{
    int before_space = is_space_before(ctx->text, ctx->len, pos);
    int after_space = (pos + n >= ctx->len) ||
                      is_space_at(ctx->text, ctx->len, pos + n);
    int before_punct = !before_space && is_punct_before(ctx->text, ctx->len, pos);
    int after_punct = (pos + n < ctx->len) && !after_space &&
                      is_punct_at(ctx->text, ctx->len, pos + n);
    /* Left-flanking: not followed by space, and not preceded by punctuation
     * unless the run is surrounded by space or punctuation. Right-flanking
     * is the mirror image. */
    int left = !after_space && (!before_punct || before_space || before_punct);
    int right = !before_space && (!after_punct || after_space || after_punct);

    if (c == '_') {
        /* Underscores never open or close inside a word. */
        *can_open = left && (!right || before_punct);
        *can_close = right && (!left || after_punct);
    } else {
        *can_open = left;
        *can_close = right;
    }
}

static size_t run_length(const md_inline_ctx *ctx, size_t pos, char c)
{
    size_t n = 0;

    while (pos + n < ctx->limit && ctx->text[pos + n] == c) {
        n++;
    }
    return n;
}

/* An unmatched delimiter run waiting for a closer. */

/*
 * Emphasis follows the CommonMark delimiter-stack algorithm: every run that
 * can open is remembered, and a run that can close pairs with the nearest
 * still-open run of the same kind. Because the nearest opener wins, runs
 * nested inside the pair (as in "*a **b** c*") match each other first, which
 * is what produces em(strong(...)) rather than a flat sequence.
 */
static md_status wrap_openers(md_inline_ctx *ctx, md_node *parent,
                              size_t stack_index, size_t run)
{
    md_delim *d = &ctx->delims[stack_index];
    size_t pos = ctx->pos;
    md_node *node;
    md_status status;

    for (;;) {
        unsigned usable = (d->count >= 2u && run >= 2u) ? 2u : 1u;
        size_t k;

        status = flush_pending_split(ctx, parent);
        if (status != MD_OK) {
            return status;
        }
        node = (usable == 2u) ? md_make_strong(ctx->arena) : md_make_em(ctx->arena);
        if (node == NULL) {
            return MD_ERR_NOMEM;
        }
        /* Move the nodes produced since the opener into the new wrapper. */
        for (k = d->child_index; k < parent->child_count; k++) {
            status = md_node_append(ctx->arena, node, parent->children[k]);
            if (status != MD_OK) {
                return status;
            }
        }
        parent->child_count = d->child_index;
        status = md_node_append(ctx->arena, parent, node);
        if (status != MD_OK) {
            return status;
        }
        d->count -= usable;
        run -= usable;
        pos += usable;
        if (d->count == 0u || run == 0u) {
            break;
        }
        /* Both runs have delimiters left: the wrapper nests once more. */
    }
    ctx->pos = pos;
    d->count = 0u;
    if (run > 0u) {
        /* Unused closers of the run stay literal text. */
        status = emit_literal(ctx, pos, run);
        if (status != MD_OK) {
            return status;
        }
    }
    return MD_OK;
}

/*
 * Insert the `count` delimiters of an unmatched run at `index`.
 *
 * The run goes into the arena rather than a fixed buffer, because a run is
 * bounded only by the length of the inline content: "*********`" is a valid
 * paragraph whose nine asterisks simply never find a closer, and a caller
 * must not be able to turn a long run into a failed parse. The arena's own
 * memory cap is the only limit that applies here, which is the point of
 * having one.
 */
static md_status insert_delims(md_inline_ctx *ctx, md_node *parent,
                                size_t index, char c, unsigned count)
{
    char *buf;
    unsigned i;

    if (count == 0u) {
        return MD_OK;
    }
    buf = (char *)md_arena_alloc(ctx->arena, (size_t)count);
    if (buf == NULL) {
        return MD_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        buf[i] = c;
    }
    /* The delimiters precede the content that was produced after them. */
    return merge_text_at(ctx->arena, parent, index, buf, (size_t)count, 1);
}

/*
 * Consume the delimiter run at ctx->pos: pair it with an opener, remember it
 * for a later closer, or emit it literally.
 */
static md_status parse_emphasis(md_inline_ctx *ctx, md_node *parent)
{
    char c = ctx->text[ctx->pos];
    size_t run = run_length(ctx, ctx->pos, c);
    int can_open = 0;
    int can_close = 0;
    size_t i;
    md_status status;

    delim_flanking(ctx, ctx->pos, run, c, &can_open, &can_close);

    if (can_close) {
        int closer_both = can_open && can_close;

        for (i = ctx->ndelims; i > 0; i--) {
            md_delim *d = &ctx->delims[i - 1u];

            if (d->c != c || d->count == 0u) {
                continue;
            }
            /* Rule of three: a run that can both open and close only pairs
             * with another when the combined length is not a multiple of 3. */
            if (((d->can_open && d->can_close) || closer_both) &&
                ((d->count + run) % 3u == 0u) &&
                !(d->count % 3u == 0u && run % 3u == 0u)) {
                /* This opener is off limits; its delimiters are literal. */
                status = insert_delims(ctx, parent, d->child_index, c, d->count);
                if (status != MD_OK) {
                    return status;
                }
                d->count = 0u;
                continue;
            }
            if (ctx->depth >= MD_INLINE_MAX_NESTING) {
                break;
            }
            return wrap_openers(ctx, parent, i - 1u, run);
        }
    }
    if (can_open && run > 0u && ctx->ndelims < MD_MAX_DELIMS &&
        ctx->depth < MD_INLINE_MAX_NESTING) {
        md_delim *d;
        md_status flushed = flush_pending_split(ctx, parent);

        if (flushed != MD_OK) {
            return flushed;
        }
        d = &ctx->delims[ctx->ndelims++];
        d->c = c;
        d->count = (unsigned)run;
        d->child_index = parent->child_count;
        d->can_open = can_open;
        d->can_close = can_close;
        ctx->pos += run;
        return MD_OK;
    }
    return emit_literal(ctx, ctx->pos, run);
}

/* Emit the delimiters of every opener that never found a closer. */
static md_status flush_delims(md_inline_ctx *ctx, md_node *parent)
{
    md_status status;

    while (ctx->ndelims > 0) {
        md_delim *d = &ctx->delims[--ctx->ndelims];

        if (d->count == 0u) {
            continue;
        }
        status = flush_pending_split(ctx, parent);
        if (status != MD_OK) {
            return status;
        }
        status = insert_delims(ctx, parent, d->child_index, d->c, d->count);
        if (status != MD_OK) {
            return status;
        }
        d->count = 0u;
    }
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Link destinations, titles and labels                                */
/* ------------------------------------------------------------------ */

static void skip_inline_ws(const md_inline_ctx *ctx, size_t *p)
{
    while (*p < ctx->limit && (ctx->text[*p] == ' ' || ctx->text[*p] == '\t' ||
                               ctx->text[*p] == '\n')) {
        (*p)++;
    }
}

/* Read a destination at *p; on success stores its span. */
static int read_destination(const md_inline_ctx *ctx, size_t *p, size_t *off,
                            size_t *len)
{
    size_t start;

    if (*p < ctx->limit && ctx->text[*p] == '<') {
        size_t q = *p + 1u;

        while (q < ctx->limit) {
            char c = ctx->text[q];

            if (c == '\\' && q + 1u < ctx->limit &&
                is_ascii_punct(ctx->text[q + 1u])) {
                q += 2u;
                continue;
            }
            if (c == '\n' || c == '<') {
                return 0;
            }
            if (c == '>') {
                *off = *p + 1u;
                *len = q - (*p + 1u);
                *p = q + 1u;
                return 1;
            }
            q++;
        }
        return 0;
    }
    start = *p;
    while (*p < ctx->limit) {
        unsigned char c = (unsigned char)ctx->text[*p];

        if (c == '\\' && *p + 1u < ctx->limit &&
            is_ascii_punct(ctx->text[*p + 1u])) {
            *p += 2u;
            continue;
        }
        if (c <= 0x20u || c == 0x7Fu) {
            break;
        }
        if (c == ')') {
            /* The closing parenthesis of the link ends the destination. */
            break;
        }
        if (c == '(') {
            /* Balanced parentheses are allowed in a bare destination. */
            size_t q = *p + 1u;
            int depth = 1;

            while (q < ctx->limit && depth > 0) {
                char d = ctx->text[q];

                if (d == '\\' && q + 1u < ctx->limit &&
                    is_ascii_punct(ctx->text[q + 1u])) {
                    q += 2u;
                    continue;
                }
                if (d == '(') {
                    depth++;
                } else if (d == ')') {
                    depth--;
                } else if (d == '\n') {
                    return 0;
                }
                q++;
            }
            if (depth != 0) {
                return 0;
            }
            *p = q;
            continue;
        }
        (*p)++;
    }
    *off = start;
    *len = *p - start;
    return 1;
}

/* Read a title in quotes or parentheses at *p; on success stores its span. */
static int read_title(const md_inline_ctx *ctx, size_t *p, size_t *off,
                      size_t *len)
{
    char open;
    char close;
    size_t q;
    size_t start;

    if (*p >= ctx->limit) {
        return 0;
    }
    open = ctx->text[*p];
    if (open == '"' || open == '\'') {
        close = open;
    } else if (open == '(') {
        close = ')';
    } else {
        return 0;
    }
    q = *p + 1u;
    start = q;
    while (q < ctx->limit) {
        char c = ctx->text[q];

        if (c == '\\' && q + 1u < ctx->limit && is_ascii_punct(ctx->text[q + 1u])) {
            q += 2u;
            continue;
        }
        if (c == close) {
            *off = start;
            *len = q - start;
            *p = q + 1u;
            return 1;
        }
        if (open == '(' && c == '(') {
            return 0; /* an unescaped nested parenthesis */
        }
        q++;
    }
    return 0;
}

/*
 * The last ']' in the run that could close a link label, or MD_NPOS.
 *
 * A ']' inside a code span or behind a backslash is not a label closer, so
 * the scan skips them the same way find_label_end() does. The answer is
 * cached in the context: it is only ever needed once a label has run past
 * the nesting limit, and a document of nothing but unmatched '[' must not
 * pay for a whole-run scan at every one of them.
 */
static size_t find_last_close(md_inline_ctx *ctx)
{
    size_t i = 0;
    size_t last = MD_NPOS;

    if (ctx->last_close_known) {
        return ctx->last_close;
    }
    while (i < ctx->limit) {
        char c = ctx->text[i];

        if (c == '\\' && i + 1u < ctx->limit) {
            i += 2u;
            continue;
        }
        if (c == '`') {
            size_t off = 0;
            size_t len = 0;
            size_t end = scan_code_span(ctx, i, &off, &len);

            i = (end != MD_NPOS) ? end : i + backtick_run(ctx, i);
            continue;
        }
        if (c == ']') {
            last = i;
        }
        i++;
    }
    ctx->last_close = last;
    ctx->last_close_known = 1;
    return last;
}

/*
 * Find the ']' that closes the '[' at start, or MD_NPOS.
 *
 * `nesting` counts the open brackets inside the label and is exact. An
 * earlier version stopped counting at a fixed ceiling and then treated the
 * next ']' as the closer, so a label nested deeper than that ceiling was
 * closed at the wrong bracket: 70 '[', 'a', 64 ']', "(/u)", 6 ']' was read
 * as a link whose label was 70 brackets deep, where the same document built
 * with 30 brackets is correctly ordinary text. Guessing is not an option
 * once the count passes `limit`, because the closer may be anywhere.
 *
 * So the scan stops there and asks find_last_close() the one question that
 * decides the outcome:
 *
 *   - A ']' is still ahead: the brackets do close, only deeper than the
 *     document is allowed to nest, so *too_deep is set and the caller
 *     refuses the document instead of inventing a label boundary.
 *   - None is: the brackets never formed a link. They are ordinary text,
 *     MD_NPOS is the usual answer, and a run of a hundred unmatched '['
 *     stays the literal text CommonMark says it is.
 */
static size_t find_label_end(md_inline_ctx *ctx, size_t start,
                             unsigned limit, int *too_deep)
{
    size_t i = start + 1u;
    unsigned nesting = 1;

    *too_deep = 0;
    while (i < ctx->limit) {
        char c = ctx->text[i];

        if (c == '\\' && i + 1u < ctx->limit) {
            i += 2u;
            continue;
        }
        if (c == '`') {
            size_t off = 0;
            size_t len = 0;
            size_t end = scan_code_span(ctx, i, &off, &len);

            i = (end != MD_NPOS) ? end : i + backtick_run(ctx, i);
            continue;
        }
        if (c == '[') {
            if (nesting >= limit) {
                size_t last = find_last_close(ctx);

                /* MD_NPOS is the largest offset there is, so it has to be
                 * tested before the comparison rather than after it. */
                *too_deep = (last != MD_NPOS) && (i < last);
                return MD_NPOS;
            }
            nesting++;
            i++;
            continue;
        }
        if (c == ']') {
            nesting--;
            if (nesting == 0u) {
                return i;
            }
            i++;
            continue;
        }
        i++;
    }
    return MD_NPOS;
}

/* Resolve the tail of a link: "(dest 'title')", "[ref]", "[]" or a shortcut. */
static md_status parse_link_tail(md_inline_ctx *ctx, size_t label_end,
                                 size_t open, const char **dest,
                                 const char **title, size_t *after)
{
    size_t p = label_end + 1u;

    *title = NULL;
    if (p < ctx->limit && ctx->text[p] == '(') {
        size_t dest_off = 0;
        size_t dest_len = 0;
        size_t title_off = 0;
        size_t title_len = 0;

        p++;
        skip_inline_ws(ctx, &p);
        if (!read_destination(ctx, &p, &dest_off, &dest_len)) {
            return MD_ERR_PARSE;
        }
        skip_inline_ws(ctx, &p);
        if (p < ctx->limit && ctx->text[p] != ')') {
            if (!read_title(ctx, &p, &title_off, &title_len)) {
                return MD_ERR_PARSE;
            }
            *title = md_inline_unescape(ctx->arena, ctx->text, title_off,
                                        title_len);
            if (*title == NULL) {
                return MD_ERR_NOMEM;
            }
            skip_inline_ws(ctx, &p);
        }
        if (p >= ctx->limit || ctx->text[p] != ')') {
            return MD_ERR_PARSE;
        }
        p++;
        *dest = md_inline_unescape(ctx->arena, ctx->text, dest_off, dest_len);
        if (*dest == NULL) {
            return MD_ERR_NOMEM;
        }
        *after = p;
        return MD_OK;
    }
    {
        const char *label = NULL;
        size_t label_len = 0;
        const md_refdef *def;

        if (p < ctx->limit && ctx->text[p] == '[') {
            size_t q = p + 1u;

            while (q < ctx->limit && ctx->text[q] != ']' && ctx->text[q] != '[') {
                q++;
            }
            if (q >= ctx->limit || ctx->text[q] != ']') {
                return MD_ERR_PARSE;
            }
            if (q > p + 1u) {
                label = ctx->text + p + 1u;
                label_len = q - (p + 1u);
            }
            p = q + 1u;
        }
        if (label == NULL) {
            /* Shortcut reference: the link text doubles as the label. */
            label = ctx->text + open + 1u;
            label_len = label_end - (open + 1u);
        }
        def = (ctx->refs != NULL) ? md_refmap_lookup(ctx->refs, label, label_len)
                                  : NULL;
        if (def == NULL) {
            return MD_ERR_PARSE; /* an undefined reference stays literal */
        }
        *dest = def->destination;
        *title = def->title;
        *after = p;
        return MD_OK;
    }
}

/*
 * Parse the link or image that starts at the '[' in ctx->pos. On success the
 * node is appended to `parent` and the cursor moves past the construct; on
 * MD_ERR_PARSE nothing is emitted and the caller keeps the text literal.
 */
static md_status parse_bracket(md_inline_ctx *ctx, md_node *parent, int image)
{
    size_t label_end;
    const char *dest = NULL;
    const char *title = NULL;
    size_t after = 0;
    md_node *node;
    md_status status;
    int too_deep = 0;

    /* A bracket deeper than the document's nesting limit is refused, not
     * downgraded to literal text. Reporting it as ordinary text is how a
     * 10000-deep label used to parse cleanly: the refusal is the only thing
     * that distinguishes it from a bracket that was never a link. */
    if (ctx->depth >= MD_INLINE_MAX_NESTING) {
        inl_note_error(ctx, ctx->pos);
        return MD_ERR_LIMIT;
    }
    label_end = find_label_end(ctx, ctx->pos, inl_max_nesting(ctx), &too_deep);
    if (too_deep) {
        inl_note_error(ctx, ctx->pos);
        return MD_ERR_LIMIT;
    }
    if (label_end == MD_NPOS) {
        return MD_ERR_PARSE;
    }
    status = parse_link_tail(ctx, label_end, ctx->pos, &dest, &title, &after);
    if (status != MD_OK) {
        return status;
    }
    node = image ? md_make_image(ctx->arena, dest, title)
                 : md_make_link(ctx->arena, dest, title);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = flush_pending(ctx, parent);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(ctx->arena, parent, node);
    if (status != MD_OK) {
        return status;
    }
    status = parse_nested(ctx, node, ctx->pos + 1u, label_end);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos = after;
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Escapes and entities                                                */
/* ------------------------------------------------------------------ */

static md_status parse_backslash(md_inline_ctx *ctx, md_node *parent)
{
    size_t next = ctx->pos + 1u;
    md_status status;

    if (next >= ctx->limit) {
        return emit_literal(ctx, ctx->pos, 1u);
    }
    if (ctx->text[next] == '\n') {
        ctx->pos = next + 1u;
        return emit_break(ctx, parent, 1);
    }
    if (is_ascii_punct(ctx->text[next])) {
        status = emit_literal(ctx, next, 1u);
        if (status != MD_OK) {
            return status;
        }
        return MD_OK;
    }
    return emit_literal(ctx, ctx->pos, 1u);
}

static md_status parse_entity(md_inline_ctx *ctx)
{
    char decoded[8];
    size_t decoded_len = 0;
    size_t used = md_entity_decode(ctx->text, ctx->limit, ctx->pos, decoded,
                                   sizeof decoded, &decoded_len);
    md_status status;

    if (used == 0u) {
        return emit_literal(ctx, ctx->pos, 1u);
    }
    /* The reference is replaced by its decoded text. */
    status = emit_bytes(ctx, decoded, decoded_len);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos += used;
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static md_status inline_run(md_inline_ctx *ctx, md_node *parent)
{
    md_status status;

    md_buffer_init(&ctx->pending);
    while (ctx->pos < ctx->limit) {
        char c = ctx->text[ctx->pos];

        switch (c) {
        case '\n':
            status = parse_break(ctx, parent);
            break;
        case '\\':
            status = parse_backslash(ctx, parent);
            break;
        case '`':
            status = parse_code_span(ctx, parent);
            break;
        case '&':
            status = parse_entity(ctx);
            break;
        case '[':
            if ((inl_ext(ctx) & MD_EXT_FOOTNOTES) != 0u) {
                int consumed = 0;

                status = parse_footnote_ref(ctx, parent, &consumed);
                if (consumed) {
                    break; /* the reference is the node at this position */
                }
            } else {
                status = MD_OK;
            }
            if (status != MD_OK) {
                break;
            }
            status = parse_bracket(ctx, parent, 0);
            if (status == MD_ERR_PARSE) {
                /* Not a link: the bracket stays literal text. */
                status = emit_literal(ctx, ctx->pos, 1u);
            }
            break;
        case '!':
            if (ctx->pos + 1u < ctx->limit && ctx->text[ctx->pos + 1u] == '[') {
                size_t bang = ctx->pos;

                /* The '!' is part of the image syntax, not text. */
                ctx->pos++;
                status = parse_bracket(ctx, parent, 1);
                if (status == MD_ERR_PARSE) {
                    /* Not an image: "![" stays literal text. */
                    ctx->pos = bang;
                    status = emit_literal(ctx, bang, 2u);
                }
            } else {
                status = emit_literal(ctx, ctx->pos, 1u);
            }
            break;
        case '*':
        case '_':
            status = parse_emphasis(ctx, parent);
            break;
        case '~':
            if ((inl_ext(ctx) & MD_EXT_STRIKETHROUGH) != 0u) {
                status = parse_strikethrough(ctx, parent);
            } else {
                status = emit_literal(ctx, ctx->pos, 1u);
            }
            break;
        default:
            status = run_registered_inlines(ctx, parent);
            break;
        }
        if (status != MD_OK) {
            md_buffer_free(&ctx->pending);
            return status;
        }
    }
    /* Trailing whitespace of the run is not significant. */
    while (ctx->pending.len > 0u &&
           ctx->pending.data[ctx->pending.len - 1u] == ' ') {
        ctx->pending.len--;
    }
    status = flush_delims(ctx, parent);
    if (status != MD_OK) {
        md_buffer_free(&ctx->pending);
        return status;
    }
    status = flush_pending(ctx, parent);
    md_buffer_free(&ctx->pending);
    if (status != MD_OK) {
        return status;
    }
    return coalesce_text(ctx->arena, parent);
}

md_status md_inline_lines(md_arena *arena, const char *text,
                          const md_span *lines, size_t count,
                          md_node *parent, md_refmap *refs)
{
    return md_inline_lines_opts(arena, text, lines, count, parent, refs, NULL);
}

/*
 * Map an offset in the joined inline buffer back to a document offset.
 *
 * join_lines() drops the leading spaces of every line and joins what is left
 * with '\n', so the mapping walks the same way. Only the line the parser was
 * on is reported, and an offset that names a joining newline is reported as
 * that newline, so the caller never sees a position from a different line.
 */
static size_t joined_to_doc_offset(const char *text, const md_span *lines,
                                   size_t count, size_t joined_off)
{
    size_t i;
    size_t at = 0;

    if (count == 0u) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        size_t start = lines[i].off;
        size_t len = lines[i].len;

        while (len > 0u && text[start] == ' ') {
            start++;
            len--;
        }
        if (joined_off < at + len) {
            return start + (joined_off - at);
        }
        at += len;
        if (i + 1u < count) {
            if (joined_off == at) {
                return start + len; /* the newline joining to the next line */
            }
            at += 1u;
        }
    }
    /* Past the end: the end of the last line. */
    return lines[count - 1u].off + lines[count - 1u].len;
}

md_status md_inline_lines_opts_err(md_arena *arena, const char *text,
                                   const md_span *lines, size_t count,
                                   md_node *parent, md_refmap *refs,
                                   const md_options *options,
                                   size_t *err_off);

/* As md_inline_lines_opts(), and report where a nesting failure happened. */
md_status md_inline_lines_opts_err(md_arena *arena, const char *text,
                                   const md_span *lines, size_t count,
                                   md_node *parent, md_refmap *refs,
                                   const md_options *options,
                                   size_t *err_off);

md_status md_inline_lines_opts(md_arena *arena, const char *text,
                               const md_span *lines, size_t count,
                               md_node *parent, md_refmap *refs,
                               const md_options *options)
{
    return md_inline_lines_opts_err(arena, text, lines, count, parent, refs,
                                    options, NULL);
}

/*
 * As md_inline_lines_opts(), and on a nesting failure report where in the
 * document it happened.
 *
 * `err_off` receives the offset of the first refused construct, so a caller
 * that owns the document text can turn it into a line and a column. It is
 * only written for MD_ERR_NESTING: a bracket label too deep to close, which
 * is a limit the document broke rather than a construct that merely failed
 * to parse. A NULL `err_off` is accepted and discards the position.
 */
md_status md_inline_lines_opts_err(md_arena *arena, const char *text,
                                   const md_span *lines, size_t count,
                                   md_node *parent, md_refmap *refs,
                                   const md_options *options,
                                   size_t *err_off)
{
    char *joined;
    size_t len = 0;
    md_inline_ctx ctx;
    md_status status;

    if (arena == NULL || text == NULL || parent == NULL ||
        (lines == NULL && count > 0)) {
        md_set_error("invalid argument: md_inline_lines");
        return MD_ERR_INVAL;
    }
    joined = join_lines(text, lines, count, &len);
    if (joined == NULL) {
        return MD_ERR_NOMEM;
    }
    ctx.arena = arena;
    ctx.text = joined;
    ctx.len = len;
    ctx.limit = len;
    ctx.pos = 0;
    ctx.refs = refs;
    ctx.depth = 0;
    ctx.err_off = 0;
    ctx.err_set = 0;
    ctx.last_close = MD_NPOS;
    ctx.last_close_known = 0;
    ctx.opts = options;
    ctx.max_nesting = inl_max_nesting(&ctx);
    ctx.ndelims = 0;
    ctx.pending.data = NULL;
    ctx.pending.len = 0;
    ctx.pending.cap = 0;
    ctx.pending.failed = 0;
    status = inline_run(&ctx, parent);
    if (status == MD_ERR_LIMIT && ctx.err_set && err_off != NULL) {
        *err_off = joined_to_doc_offset(text, lines, count, ctx.err_off);
    }
    free(joined);
    return status;
}

md_status md_inline_text_run(md_arena *arena, const char *text, size_t len,
                             md_node *parent, md_refmap *refs)
{
    return md_inline_text_run_opts(arena, text, len, parent, refs, NULL);
}

md_status md_inline_text_run_opts(md_arena *arena, const char *text, size_t len,
                                  md_node *parent, md_refmap *refs,
                                  const md_options *options)
{
    md_inline_ctx ctx;
    md_status status;

    if (arena == NULL || (text == NULL && len > 0) || parent == NULL) {
        md_set_error("invalid argument: md_inline_text_run");
        return MD_ERR_INVAL;
    }
    if (text == NULL) {
        text = "";
        len = 0;
    }
    ctx.arena = arena;
    ctx.text = text;
    ctx.len = len;
    ctx.limit = len;
    ctx.pos = 0;
    ctx.refs = refs;
    ctx.depth = 0;
    ctx.err_off = 0;
    ctx.err_set = 0;
    ctx.last_close = MD_NPOS;
    ctx.last_close_known = 0;
    ctx.opts = options;
    ctx.max_nesting = inl_max_nesting(&ctx);
    ctx.ndelims = 0;
    ctx.pending.data = NULL;
    ctx.pending.len = 0;
    ctx.pending.cap = 0;
    ctx.pending.failed = 0;
    status = inline_run(&ctx, parent);
    return status;
}

md_status md_inline_text_run_raw(md_arena *arena, const char *text, size_t len,
                                 md_node *parent)
{
    md_node *node;

    if (arena == NULL || (text == NULL && len > 0) || parent == NULL) {
        md_set_error("invalid argument: md_inline_text_run_raw");
        return MD_ERR_INVAL;
    }
    node = md_inline_text(arena, text, len);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    return md_node_append(arena, parent, node);
}

md_status md_parse_inlines(md_arena *arena, const char *src, size_t len,
                           md_node *parent)
{
    return md_parse_inlines_opts(arena, src, len, parent, NULL);
}

md_status md_parse_inlines_opts(md_arena *arena, const char *src, size_t len,
                                md_node *parent, const md_options *options)
{
    char *norm = NULL;
    size_t norm_len = 0;
    size_t start = 0;
    md_status status;

    if (arena == NULL || (src == NULL && len > 0) || parent == NULL) {
        md_set_error("invalid argument: md_parse_inlines");
        return MD_ERR_INVAL;
    }
    if (src == NULL) {
        src = "";
        len = 0;
    }
    if (md_normalize(arena, src, len, &norm, &norm_len) != 0) {
        return MD_ERR_NOMEM;
    }
    while (start < norm_len && norm[start] == ' ') {
        start++;
    }
    while (norm_len > start && norm[norm_len - 1u] == ' ') {
        norm_len--;
    }
    status = md_inline_text_run_opts(arena, norm + start, norm_len - start,
                                     parent, NULL, options);
    return status;
}

/* ------------------------------------------------------------------ */
/* Link reference definitions                                          */
/* ------------------------------------------------------------------ */

int md_inline_scan_refdef(const char *text, size_t off, size_t len,
                          size_t *label_off, size_t *label_len,
                          size_t *dest_off, size_t *dest_len,
                          size_t *title_off, size_t *title_len,
                          int *has_title)
{
    md_inline_ctx sub;
    size_t p = off;
    size_t end = off + len;
    size_t label_start;
    size_t q;
    size_t d_off = 0;
    size_t d_len = 0;
    size_t t_off = 0;
    size_t t_len = 0;
    int title_present = 0;

    if (text == NULL || label_off == NULL || label_len == NULL ||
        dest_off == NULL || dest_len == NULL || title_off == NULL ||
        title_len == NULL || has_title == NULL) {
        return 0;
    }
    while (p < end && p - off < 4u && text[p] == ' ') {
        p++;
    }
    if (p >= end || text[p] != '[') {
        return 0;
    }
    p++;
    label_start = p;
    q = p;
    while (q < end) {
        if (text[q] == '\\' && q + 1u < end) {
            q += 2u;
            continue;
        }
        if (text[q] == '[' || text[q] == '\n') {
            return 0;
        }
        if (text[q] == ']') {
            break;
        }
        q++;
    }
    if (q >= end || q == label_start ||
        q - label_start > (size_t)MD_REF_LABEL_MAX) {
        return 0;
    }
    *label_off = label_start;
    *label_len = q - label_start;
    p = q + 1u;
    if (p >= end || text[p] != ':') {
        return 0;
    }
    p++;
    while (p < end && (text[p] == ' ' || text[p] == '\t')) {
        p++;
    }
    sub.arena = NULL;
    sub.text = text;
    sub.len = end;
    sub.limit = end;
    sub.pos = p;
    sub.refs = NULL;
    sub.depth = 0;
    sub.max_nesting = 0; /* a definition is a single line, never nested */
    sub.err_off = 0;
    sub.err_set = 0;
    sub.last_close = MD_NPOS;
    sub.last_close_known = 0;
    sub.opts = NULL;
    sub.pending.data = NULL;
    sub.pending.len = 0;
    sub.pending.cap = 0;
    sub.pending.failed = 0;
    if (!read_destination(&sub, &p, &d_off, &d_len) || d_len == 0u) {
        return 0; /* an absent or empty destination is not a definition */
    }
    *dest_off = d_off;
    *dest_len = d_len;
    while (p < end && (text[p] == ' ' || text[p] == '\t')) {
        p++;
    }
    if (p < end) {
        if (!read_title(&sub, &p, &t_off, &t_len)) {
            return 0;
        }
        title_present = 1;
        *title_off = t_off;
        *title_len = t_len;
        while (p < end && (text[p] == ' ' || text[p] == '\t')) {
            p++;
        }
    }
    if (p != end) {
        return 0; /* trailing junk means this is not a definition */
    }
    *has_title = title_present;
    return 1;
}
