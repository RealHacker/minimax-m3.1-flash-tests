#include "blocks.h"

#include "extensions.h"
#include "inlines.h"
#include "md.h"
#include "refmap.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

/*
 * Where a failure happened, shared by every nested context of one parse.
 *
 * A byte offset into the document text is recorded rather than a line number,
 * because each nested context indexes its own line slice and only the
 * document-level text is common to all of them; the entry point turns the
 * offset into a line and column once, at the end.
 */
typedef struct md_block_err {
    size_t off; /* byte offset into the document text */
    int set;
} md_block_err;

typedef struct {
    md_arena *arena;
    const char *text;      /* normalized, tab-free buffer */
    size_t text_len;
    const md_span *lines;  /* current container's line slice */
    size_t line_count;
    size_t pos;            /* cursor within the slice */
    unsigned depth;
    unsigned max_nesting;  /* resolved ceiling, <= MD_MAX_NESTING_HARD */
    md_refmap *refs;       /* link reference definitions, shared document-wide */
    int inlines;           /* 1: parse inline children, 0: raw text (C1 mode) */
    const md_options *opts;/* extension mask and custom registry (C4) */
    md_node *doc_root;     /* document node, for footnote definitions */
    md_block_err *err;     /* where a failure happened, shared document-wide */
} md_block_ctx;

/* Extension mask of a context; NULL options mean no extensions at all. */
static md_ext_flags ctx_ext(const md_block_ctx *ctx)
{
    return ctx->opts != NULL ? ctx->opts->extensions : MD_EXT_NONE;
}

/* A list marker, resolved once per item. */
typedef struct {
    size_t indent;        /* leading spaces before the marker */
    size_t marker_end;    /* offset just past the marker */
    size_t content_start; /* offset of the item content */
    int ordered;
    long number;          /* ordered lists: the item number */
    char delim;           /* ordered lists: '.' or ')' */
    int empty;            /* marker with no content on its line */
} md_list_marker;

/* Both accessors are bounds-safe: an index past the last line yields an
 * empty line, so a cursor overshoot can never read outside the span array. */
static const char *ctx_ptr(const md_block_ctx *ctx, size_t index)
{
    if (index >= ctx->line_count) {
        return ctx->text;
    }
    return ctx->text + ctx->lines[index].off;
}

static size_t ctx_len(const md_block_ctx *ctx, size_t index)
{
    if (index >= ctx->line_count) {
        return 0u;
    }
    return ctx->lines[index].len;
}

/* The offset of the first byte of `index` in the normalized text.
 *
 * Written as a table lookup rather than as ctx_ptr(ctx, index) - ctx->text
 * so that an index past the last line yields a plain zero instead of
 * subtracting two pointers. The two agree today, but only one of them is
 * obviously right at a glance. */
static size_t ctx_offset(const md_block_ctx *ctx, size_t index)
{
    if (index >= ctx->line_count) {
        return 0u;
    }
    return ctx->lines[index].off;
}

static int ctx_done(const md_block_ctx *ctx)
{
    return ctx->pos >= ctx->line_count;
}

/*
 * Record the line the parser is on as the place a failure happened. Only the
 * first note is kept, so the deepest frame that noticed the problem keeps it
 * rather than an outer frame overwriting it with a vaguer position.
 */
/*
 * Record where a failure happened, as a byte offset into the document text.
 *
 * The offset is the start of the line the parser was working on rather than
 * the parent context's cursor, because a container is consumed before it is
 * descended into: by the time a nested context is refused, the parent cursor
 * has already run to the end of its slice and would name the wrong line. Only
 * the first failure is recorded, so the innermost limit reached is the one
 * reported.
 */
static void ctx_note_error_off(md_block_ctx *ctx, size_t off)
{
    if (ctx->err == NULL || ctx->err->set) {
        return;
    }
    if (off > ctx->text_len) {
        off = ctx->text_len;
    }
    ctx->err->off = off;
    ctx->err->set = 1;
}

/* Record the current line as the place a failure happened. */
static void ctx_note_error(md_block_ctx *ctx)
{
    const char *p;

    if (ctx->err == NULL || ctx->err->set) {
        return;
    }
    p = ctx_ptr(ctx, ctx->pos);
    if (p == NULL || p < ctx->text) {
        return;
    }
    ctx->err->off = (size_t)(p - ctx->text);
    ctx->err->set = 1;
}

/* ------------------------------------------------------------------ */
/* Line scanners (tabs are already expanded to spaces)                 */
/* ------------------------------------------------------------------ */

static int scan_thematic_break(const char *p, size_t len, size_t indent)
{
    char ch;
    size_t count = 0;
    size_t i = indent;

    if (i >= len) {
        return 0;
    }
    ch = p[i];
    if (ch != '*' && ch != '-' && ch != '_') {
        return 0;
    }
    for (; i < len; i++) {
        if (p[i] == ch) {
            count++;
        } else if (p[i] != ' ') {
            return 0;
        }
    }
    return count >= 3u;
}

/* Returns 1 for an ATX heading; sets level and the content span. */
static int scan_atx_heading(const char *p, size_t len, size_t indent,
                            int *level, size_t *off, size_t *clen)
{
    size_t hashes = 0;
    size_t end;
    size_t stop;
    size_t i = indent;

    while (i < len && p[i] == '#') {
        hashes++;
        i++;
    }
    if (hashes == 0u || hashes > 6u) {
        return 0;
    }
    if (i < len && p[i] != ' ') {
        return 0; /* "#tag" is not a heading */
    }

    end = md_line_rtrim_len(p, len);
    stop = end;
    while (stop > hashes && p[stop - 1u] == '#') {
        stop--;
    }
    if (stop < end) {
        if (stop == hashes) {
            end = stop; /* "##" / "### ###" */
        } else if (p[stop - 1u] == ' ') {
            end = stop;
            while (end > hashes && p[end - 1u] == ' ') {
                end--;
            }
        }
        /* Otherwise the run is not a closing sequence and stays as content. */
    }

    *level = (int)hashes;
    *off = indent + hashes;
    *clen = (end > *off) ? end - *off : 0u;
    return 1;
}

/* Returns 1 for a fence start; sets fence char/length, info span and indent. */
static int scan_fence(const char *p, size_t len, size_t indent,
                      char *fence, size_t *fence_len,
                      size_t *info_off, size_t *info_len)
{
    size_t i = indent;
    size_t count = 0;
    char ch;

    if (i >= len || (p[i] != '`' && p[i] != '~')) {
        return 0;
    }
    ch = p[i];
    while (i < len && p[i] == ch) {
        count++;
        i++;
    }
    if (count < 3u) {
        return 0;
    }
    if (ch == '`') {
        size_t j;

        for (j = i; j < len; j++) {
            if (p[j] == '`') {
                return 0; /* backtick info strings are not allowed */
            }
        }
    }
    *fence = ch;
    *fence_len = count;
    *info_off = i;
    *info_len = md_line_rtrim_len(p, len) - i;
    return 1;
}

/* Returns 1 for a closing fence line matching `fence` with at least `min` chars. */
static int scan_fence_close(const char *p, size_t len, char fence, size_t min)
{
    size_t i = 0;
    size_t count = 0;

    while (i < len && p[i] == ' ') {
        i++;
    }
    if (i >= 4u) {
        return 0;
    }
    while (i < len && p[i] == fence) {
        count++;
        i++;
    }
    if (count < min) {
        return 0;
    }
    for (; i < len; i++) {
        if (p[i] != ' ') {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 for a setext underline; sets level (1 for '=', 2 for '-'). */
static int scan_setext_underline(const char *p, size_t len, size_t indent, int *level)
{
    char ch;
    size_t i = indent;

    if (i >= len) {
        return 0;
    }
    ch = p[i];
    if (ch != '=' && ch != '-') {
        return 0;
    }
    for (; i < len; i++) {
        if (p[i] != ch) {
            return 0;
        }
    }
    *level = (ch == '=') ? 1 : 2;
    return 1;
}

/*
 * Parse a list marker at `indent`. When `interrupting` is set the CommonMark
 * restriction for markers that may interrupt a paragraph is applied: ordered
 * lists must start at 1 and empty items never interrupt.
 */
static int scan_list_marker(const char *p, size_t len, size_t indent, int interrupting,
                            md_list_marker *out)
{
    size_t i = indent;
    long number = 0;
    int ordered = 0;
    char delim = '\0';
    size_t marker_end;
    size_t spaces;
    size_t rest;

    if (i >= len) {
        return 0;
    }
    if (p[i] == '-' || p[i] == '+' || p[i] == '*') {
        ordered = 0;
        i++;
    } else if (p[i] >= '0' && p[i] <= '9') {
        size_t digits = 0;

        while (i < len && p[i] >= '0' && p[i] <= '9' && digits < 10u) {
            number = number * 10 + (p[i] - '0');
            digits++;
            i++;
        }
        if (digits == 0u || digits > 9u || i >= len) {
            return 0;
        }
        if (p[i] != '.' && p[i] != ')') {
            return 0;
        }
        delim = p[i];
        i++;
        ordered = 1;
        if (interrupting && number != 1) {
            return 0;
        }
    } else {
        return 0;
    }

    marker_end = i;
    spaces = 0;
    while (i < len && p[i] == ' ') {
        spaces++;
        i++;
    }
    rest = md_line_rtrim_len(p, len);
    if (i >= rest) {
        /* Empty item: the content starts one column past the marker. */
        out->content_start = marker_end + 1u;
        out->empty = 1;
    } else if (spaces == 0u) {
        /* "*text*" is emphasis, not a list: a marker needs white space. */
        return 0;
    } else if (spaces > 4u) {
        /* 5+ spaces: one space belongs to the marker, the rest is content. */
        out->content_start = marker_end + 1u;
        out->empty = 0;
    } else {
        out->content_start = marker_end + spaces;
        out->empty = 0;
    }
    if (out->content_start > len) {
        out->content_start = len;
    }

    out->indent = indent;
    out->marker_end = marker_end;
    out->ordered = ordered;
    out->number = number;
    out->delim = delim;
    if (interrupting && out->empty) {
        return 0; /* an empty item cannot interrupt a paragraph */
    }
    return 1;
}

/* Defined with the other extension blocks below. */
static int table_starts_at(const md_block_ctx *ctx, size_t index, size_t indent);

/* Does the line begin a new block, i.e. can it interrupt an open paragraph? */
static int line_starts_block(const md_block_ctx *ctx, const char *p, size_t len,
                             size_t indent, size_t index)
{
    int level;
    size_t off;
    size_t clen;
    char fence;
    size_t fence_len;
    size_t info_off;
    size_t info_len;
    md_list_marker marker;

    if (indent >= 4u) {
        return 0; /* indented code never interrupts a paragraph */
    }
    if (scan_thematic_break(p, len, indent)) {
        return 1;
    }
    if (scan_atx_heading(p, len, indent, &level, &off, &clen)) {
        return 1;
    }
    if (scan_fence(p, len, indent, &fence, &fence_len, &info_off, &info_len)) {
        return 1;
    }
    if (p[indent] == '>') {
        return 1;
    }
    if (scan_list_marker(p, len, indent, 1, &marker)) {
        return 1;
    }
    if (table_starts_at(ctx, index, indent)) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Line slice builder                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    md_arena *arena;
    md_span *tmp;   /* scratch, reallocated */
    size_t tmp_n;
    size_t tmp_cap;
    md_span *out;   /* arena-owned, exactly tmp_n entries */
    size_t n;
} md_span_builder;

static int spans_init(md_span_builder *b, md_arena *arena)
{
    b->arena = arena;
    b->tmp = NULL;
    b->tmp_n = 0;
    b->tmp_cap = 0;
    b->out = NULL;
    b->n = 0;
    return 0;
}

static int spans_push(md_span_builder *b, size_t off, size_t len)
{
    if (b->tmp_n == b->tmp_cap) {
        size_t cap = b->tmp_cap != 0 ? b->tmp_cap * 2u : 16u;
        md_span *grown;

        if (cap < b->tmp_cap || cap > ((size_t)-1) / sizeof(md_span)) {
            md_set_error("too many lines");
            return -1;
        }
        grown = (md_span *)realloc(b->tmp, cap * sizeof *grown);
        if (grown == NULL) {
            md_set_error("out of memory collecting lines");
            return -1;
        }
        b->tmp = grown;
        b->tmp_cap = cap;
    }
    b->tmp[b->tmp_n].off = off;
    b->tmp[b->tmp_n].len = len;
    b->tmp_n++;
    return 0;
}

static int spans_finish(md_span_builder *b)
{
    if (b->tmp_n == 0u) {
        b->out = (md_span *)md_arena_alloc(b->arena, sizeof(md_span));
        if (b->out == NULL) {
            return -1;
        }
    } else {
        b->out = (md_span *)md_arena_memdup(b->arena, b->tmp,
                                            b->tmp_n * sizeof(md_span));
        if (b->out == NULL) {
            return -1;
        }
    }
    b->n = b->tmp_n;
    free(b->tmp);
    b->tmp = NULL;
    b->tmp_n = 0;
    b->tmp_cap = 0;
    return 0;
}

static void spans_abort(md_span_builder *b)
{
    free(b->tmp);
    b->tmp = NULL;
    b->tmp_n = 0;
    b->tmp_cap = 0;
}

static md_status parse_blocks(md_block_ctx *ctx, md_node *parent);

/* Block constructs, defined below this dispatcher. */
static md_status parse_thematic_break(md_block_ctx *ctx, md_node *parent);
static md_status parse_atx_heading(md_block_ctx *ctx, md_node *parent,
                                   const char *p, size_t len, size_t indent);
static md_status parse_fenced_code(md_block_ctx *ctx, md_node *parent,
                                   const char *p, size_t len, size_t indent);
static md_status parse_block_quote(md_block_ctx *ctx, md_node *parent);
static md_status parse_list(md_block_ctx *ctx, md_node *parent);
static md_status parse_paragraph(md_block_ctx *ctx, md_node *parent);

/* Pick the block construct that starts at the current line. */
/* Can the line at `index` head a table? It needs a delimiter row right
 * after it whose column count matches the header's. A paragraph uses this to
 * decide whether a table interrupts it, so it must stay cheap and must not
 * allocate: the delimiter row is validated into a fixed local array. */
static int table_starts_at(const md_block_ctx *ctx, size_t index, size_t indent)
{
    const char *p;
    size_t len;
    size_t columns;
    md_table_align align[8];

    if ((ctx_ext(ctx) & MD_EXT_TABLES) == 0u) {
        return 0;
    }
    if (index >= ctx->line_count) {
        return 0;
    }
    p = ctx_ptr(ctx, index);
    len = ctx_len(ctx, index);
    if (indent >= len) {
        return 0;
    }
    /* The header needs a pipe, so a plain paragraph is rejected at once. */
    if (md_ext_table_count(p + indent, len - indent) < 1u) {
        return 0;
    }
    if (index + 1u >= ctx->line_count) {
        return 0;
    }
    {
        const char *dp = ctx_ptr(ctx, index + 1u);
        size_t dlen = ctx_len(ctx, index + 1u);
        size_t dindent = md_line_indent(dp, dlen, 3u);

        if (dindent >= 3u || md_line_is_blank(dp, dlen)) {
            return 0;
        }
        columns = md_ext_table_delims(dp + dindent, dlen - dindent, align, 8u);
    }
    if (columns == 0u) {
        return 0;
    }
    return md_ext_table_count(p + indent, len - indent) == columns;
}

/* Does the line open a footnote definition, "[^label]:", at `indent`? */
static int scan_footnote_def(const char *p, size_t len, size_t indent,
                             size_t *label_off, size_t *label_len,
                             size_t *content_off)
{
    size_t local_off = 0;
    size_t local_len = 0;
    size_t end = 0;

    if (indent >= len || p[indent] != '[') {
        return 0;
    }
    if (!md_ext_footnote_label(p, len, indent, &local_off, &local_len, &end)) {
        return 0;
    }
    if (end >= len || p[end] != ':') {
        return 0;
    }
    if (label_off != NULL) {
        *label_off = local_off;
    }
    if (label_len != NULL) {
        *label_len = local_len;
    }
    if (content_off != NULL) {
        *content_off = end + 1u;
    }
    return 1;
}

static md_status parse_blocks(md_block_ctx *ctx, md_node *parent);
static md_status parse_container(md_block_ctx *ctx, md_node *parent,
                                 const md_span *spans, size_t count);
static md_status parse_table(md_block_ctx *ctx, md_node *parent,
                             const char *p, size_t len, size_t indent);
static md_status parse_footnote_def(md_block_ctx *ctx, md_node *parent,
                                    const char *p, size_t len, size_t indent);
static md_status run_registered_blocks(md_block_ctx *ctx, md_node *parent,
                                       const char *p, size_t len, size_t indent,
                                       int *consumed);

/* ------------------------------------------------------------------ */
/* Extension blocks (checkpoint C4)                                    */
/* ------------------------------------------------------------------ */

/* Parse one table row's cells as inline content of `row_node`. */
static md_status fill_table_row(md_block_ctx *ctx, md_node *row_node,
                                const char *p, size_t len, size_t indent,
                                size_t columns)
{
    size_t *off = NULL;
    size_t *cell = NULL;
    size_t found;
    size_t i;
    md_status status = MD_OK;

    if (columns > MD_EXT_MAX_TABLE_COLUMNS) {
        columns = MD_EXT_MAX_TABLE_COLUMNS;
    }
    if (columns > 0u) {
        off = (size_t *)md_arena_alloc(ctx->arena, columns * sizeof *off);
        cell = (size_t *)md_arena_alloc(ctx->arena, columns * sizeof *cell);
        if (off == NULL || cell == NULL) {
            return MD_ERR_NOMEM;
        }
    }
    found = md_ext_table_split(p + indent, len - indent, off, cell, columns);
    for (i = 0; i < columns; i++) {
        md_node *cell_node = md_make_table_cell(ctx->arena);

        if (cell_node == NULL) {
            return MD_ERR_NOMEM;
        }
        status = md_node_append(ctx->arena, row_node, cell_node);
        if (status != MD_OK) {
            return status;
        }
        if (i < found) {
            const char *text = p + indent + off[i];

            status = ctx->inlines
                         ? md_inline_text_run_opts(ctx->arena, text, cell[i],
                                                   cell_node, ctx->refs,
                                                   ctx->opts)
                         : md_inline_text_run_raw(ctx->arena, text, cell[i],
                                                  cell_node);
            if (status != MD_OK) {
                return status;
            }
        }
    }
    return MD_OK;
}

/*
 * A pipe table: a header row, a delimiter row that fixes the alignment, and
 * then every following non-blank line that carries a pipe. The delimiter row
 * is consumed and produces no node, and its column count is the column count
 * of the table: a short row is padded with empty cells and a long one has its
 * extra cells dropped, which is what keeps the AST rectangular.
 */
static md_status parse_table(md_block_ctx *ctx, md_node *parent,
                             const char *p, size_t len, size_t indent)
{
    md_table_align align[8];
    md_table_align *columns_align;
    size_t columns;
    size_t delim_indent;
    md_node *table;
    md_node *header;
    md_status status;
    size_t index = ctx->pos;

    {
        const char *dp = ctx_ptr(ctx, index + 1u);
        size_t dlen = ctx_len(ctx, index + 1u);

        delim_indent = md_line_indent(dp, dlen, 3u);
        columns = md_ext_table_delims(dp + delim_indent, dlen - delim_indent,
                                      align, 8u);
    }
    if (columns > MD_EXT_MAX_TABLE_COLUMNS) {
        columns = MD_EXT_MAX_TABLE_COLUMNS;
    }
    columns_align = (md_table_align *)md_arena_alloc(ctx->arena,
                                                     columns * sizeof *columns_align);
    if (columns_align == NULL) {
        return MD_ERR_NOMEM;
    }
    memcpy(columns_align, align,
           (columns < 8u ? columns : 8u) * sizeof *columns_align);

    table = md_make_table(ctx->arena, columns_align, columns);
    if (table == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, table);
    if (status != MD_OK) {
        return status;
    }

    header = md_make_table_header(ctx->arena);
    if (header == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, table, header);
    if (status != MD_OK) {
        return status;
    }
    status = fill_table_row(ctx, header, p, len, indent, columns);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos = index + 2u; /* the delimiter row is not a body row */

    while (!ctx_done(ctx)) {
        const char *rp = ctx_ptr(ctx, ctx->pos);
        size_t rlen = ctx_len(ctx, ctx->pos);
        md_node *row;

        if (md_line_is_blank(rp, rlen) ||
            md_ext_table_count(rp, rlen) == 0u) {
            break; /* a body row carries at least one pipe */
        }
        if (line_starts_block(ctx, rp, rlen, md_line_indent(rp, rlen, 4u),
                              ctx->pos)) {
            break; /* another block wins over a table row */
        }
        row = md_make_table_row(ctx->arena);
        if (row == NULL) {
            return MD_ERR_NOMEM;
        }
        status = md_node_append(ctx->arena, table, row);
        if (status != MD_OK) {
            return status;
        }
        status = fill_table_row(ctx, row, rp, rlen,
                                md_line_indent(rp, rlen, 4u), columns);
        if (status != MD_OK) {
            return status;
        }
        ctx->pos++;
    }
    return MD_OK;
}

/*
 * A footnote definition. The definition node is recorded on the document
 * rather than in the block flow, so the text of a definition never appears
 * where it was written; a reference is what makes it visible. Continuation
 * lines are indented by four spaces or more, and a blank line only continues
 * the definition when an indented line follows it.
 */
static md_status parse_footnote_def(md_block_ctx *ctx, md_node *parent,
                                    const char *p, size_t len, size_t indent)
{
    size_t label_off = 0;
    size_t label_len = 0;
    /* The definition is recorded on the document, so `parent` goes unused on
     * purpose: a definition never appears in the block flow. */
    (void)parent;
    size_t content_off = 0;
    char *id;
    char *label;
    md_node *def;
    md_span_builder builder;
    md_status status;

    if (!scan_footnote_def(p, len, indent, &label_off, &label_len,
                           &content_off)) {
        return MD_OK;
    }
    id = md_ext_footnote_id(ctx->arena, p + label_off, label_len);
    if (id == NULL) {
        return MD_ERR_NOMEM;
    }
    label = md_arena_strndup(ctx->arena, p + label_off, label_len);
    if (label == NULL) {
        return MD_ERR_NOMEM;
    }
    def = md_make_footnote_def(ctx->arena, id, label);
    if (def == NULL) {
        return MD_ERR_NOMEM;
    }
    if (ctx->doc_root != NULL) {
        status = md_node_append_footnote(ctx->arena, ctx->doc_root, def);
        if (status != MD_OK) {
            return status;
        }
    }

    if (spans_init(&builder, ctx->arena) != 0) {
        return MD_ERR_NOMEM;
    }
    {
        size_t rest = md_line_rtrim_len(p, len);
        size_t start = content_off < rest ? content_off : rest;

        /* Up to three spaces after the colon are part of the marker. */
        while (start < rest && start < content_off + 3u && p[start] == ' ') {
            start++;
        }
        if (spans_push(&builder, ctx->lines[ctx->pos].off + start,
                       rest - start) != 0) {
            spans_abort(&builder);
            return MD_ERR_NOMEM;
        }
    }
    ctx->pos++;
    while (!ctx_done(ctx)) {
        const char *cp = ctx_ptr(ctx, ctx->pos);
        size_t clen = ctx_len(ctx, ctx->pos);
        size_t cindent = md_line_indent(cp, clen, 4u);
        size_t rest = md_line_rtrim_len(cp, clen);
        int blank = md_line_is_blank(cp, clen);

        if (!blank && cindent < 4u) {
            break; /* the definition ends at the next top-level line */
        }
        if (blank) {
            /* Keep the blank only when indented content follows it. */
            size_t look = ctx->pos + 1u;

            while (look < ctx->line_count &&
                   md_line_is_blank(ctx_ptr(ctx, look), ctx_len(ctx, look))) {
                look++;
            }
            if (look >= ctx->line_count ||
                md_line_indent(ctx_ptr(ctx, look), ctx_len(ctx, look), 4u) < 4u) {
                break;
            }
            if (spans_push(&builder, ctx->lines[ctx->pos].off, 0u) != 0) {
                spans_abort(&builder);
                return MD_ERR_NOMEM;
            }
            ctx->pos++;
            continue;
        }
        if (spans_push(&builder, ctx->lines[ctx->pos].off + cindent,
                       rest - cindent) != 0) {
            spans_abort(&builder);
            return MD_ERR_NOMEM;
        }
        ctx->pos++;
    }
    if (spans_finish(&builder) != 0) {
        return MD_ERR_NOMEM;
    }
    status = parse_container(ctx, def, builder.out, builder.n);
    if (status != MD_OK) {
        return status;
    }
    return MD_OK;
}

/*
 * Offer the line to every registered block extension, in registration order,
 * and return as soon as one claims it. The lines of the current container are
 * passed along so a handler can consume more than the one it looked at.
 */
static md_status run_registered_blocks(md_block_ctx *ctx, md_node *parent,
                                       const char *p, size_t len, size_t indent,
                                       int *consumed)
{
    md_ext_registry *registry;
    const char **line_ptrs = NULL;
    size_t *line_lens = NULL;
    size_t i;
    md_status status = MD_OK;

    *consumed = 0;
    if (ctx->opts == NULL || ctx->opts->custom == NULL) {
        return MD_OK; /* nothing claimed the line; a paragraph takes it */
    }
    registry = ctx->opts->custom;
    if (md_ext_registry_block_count(registry) == 0u) {
        return MD_OK;
    }
    if (ctx->line_count > 0u) {
        line_ptrs = (const char **)md_arena_alloc(ctx->arena,
                                                  ctx->line_count * sizeof *line_ptrs);
        line_lens = (size_t *)md_arena_alloc(ctx->arena,
                                             ctx->line_count * sizeof *line_lens);
        if (line_ptrs == NULL || line_lens == NULL) {
            return MD_ERR_NOMEM;
        }
        for (i = 0; i < ctx->line_count; i++) {
            line_ptrs[i] = ctx_ptr(ctx, i);
            line_lens[i] = ctx_len(ctx, i);
        }
    }
    for (i = 0; i < md_ext_registry_block_count(registry); i++) {
        md_ext_env env;
        size_t used = 0;
        int consumed_flag = 0;

        env.arena = ctx->arena;
        env.flags = ctx_ext(ctx);
        env.options = ctx->opts;
        status = md_ext_invoke_block(registry, i, &env, parent, p + indent,
                                     len - indent, line_ptrs, line_lens,
                                     ctx->line_count, &used, &consumed_flag);
        if (status != MD_OK) {
            return status;
        }
        if (consumed_flag) {
            if (used == 0u || used > ctx->line_count - ctx->pos) {
                md_set_error("extension consumed an out-of-range line count");
                return MD_ERR_PARSE;
            }
            ctx->pos += used;
            *consumed = 1;
            return MD_OK;
        }
    }
    return MD_OK;
}

/*
 * The built-in extension blocks, then any registered block extension, in
 * registration order. Sets *consumed when a construct claimed the line, in
 * which case the caller must not fall through to a paragraph.
 */
static md_status parse_extension_blocks(md_block_ctx *ctx, md_node *parent,
                                        const char *p, size_t len, size_t indent,
                                        int *consumed)
{
    md_ext_flags ext = ctx_ext(ctx);

    *consumed = 0;
    /* A registered extension is only offered a line that no built-in
     * claimed, so a registration can never shadow core or built-in syntax. */
    if ((ext & MD_EXT_TABLES) != 0u && table_starts_at(ctx, ctx->pos, indent)) {
        *consumed = 1;
        return parse_table(ctx, parent, p, len, indent);
    }
    if ((ext & MD_EXT_FOOTNOTES) != 0u && ctx->doc_root != NULL &&
        scan_footnote_def(p, len, indent, NULL, NULL, NULL)) {
        *consumed = 1;
        return parse_footnote_def(ctx, parent, p, len, indent);
    }
    return run_registered_blocks(ctx, parent, p, len, indent, consumed);
}

static md_status parse_blocks_dispatch(md_block_ctx *ctx, md_node *parent,
                                       const char *p, size_t len, size_t indent,
                                       int *level, size_t *off, size_t *clen,
                                       char *fence, size_t *fence_len,
                                       size_t *info_off, size_t *info_len,
                                       md_list_marker *marker)
{
    md_status status;

    if (scan_thematic_break(p, len, indent)) {
        return parse_thematic_break(ctx, parent);
    }
    if (scan_atx_heading(p, len, indent, level, off, clen)) {
        return parse_atx_heading(ctx, parent, p, len, indent);
    }
    if (scan_fence(p, len, indent, fence, fence_len, info_off, info_len)) {
        return parse_fenced_code(ctx, parent, p, len, indent);
    }
    if (indent < len && p[indent] == '>') {
        return parse_block_quote(ctx, parent);
    }
    if (scan_list_marker(p, len, indent, 0, marker)) {
        return parse_list(ctx, parent);
    }
    {
        int consumed = 0;

        status = parse_extension_blocks(ctx, parent, p, len, indent,
                                        &consumed);
        if (status != MD_OK || consumed) {
            return status;
        }
    }
    return parse_paragraph(ctx, parent);
}

/* Build a child context over an arena-owned span array and parse it. */
static md_status parse_container(md_block_ctx *ctx, md_node *parent,
                                 const md_span *spans, size_t count)
{
    md_block_ctx sub;
    md_status status;

    /*
     * Checked before the frame is built, so the deepest container the parser
     * will ever descend into is the last one it successfully entered. The
     * limit is inherited, already clamped by md_limits_nesting().
     */
    if (ctx->depth + 1u > ctx->max_nesting) {
        /* The first line of the container being refused is the line the
         * error names: that is where the document asked for one level too
         * many, and it is a real line even when the parent cursor has moved. */
        ctx_note_error_off(ctx, count > 0u ? spans[0].off
                                          : ctx_offset(ctx, ctx->pos));
        md_set_error_kind(MD_ERROR_NESTING, "nesting too deep");
        return MD_ERR_LIMIT;
    }
    sub.arena = ctx->arena;
    sub.text = ctx->text;
    sub.text_len = ctx->text_len;
    sub.lines = spans;
    sub.line_count = count;
    sub.pos = 0;
    sub.depth = ctx->depth + 1u;
    sub.max_nesting = ctx->max_nesting;
    sub.refs = ctx->refs;
    sub.inlines = ctx->inlines;
    sub.opts = ctx->opts;
    sub.doc_root = ctx->doc_root;
    sub.err = ctx->err;
    status = parse_blocks(&sub, parent);
    if (status != MD_OK) {
        return status;
    }
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Block parsers                                                       */
/* ------------------------------------------------------------------ */

static md_status finish_text(md_block_ctx *ctx, md_node *parent,
                             const md_span *lines, size_t count)
{
    size_t err_off = 0;
    md_status status;

    if (count == 0u) {
        return MD_OK;
    }
    if (ctx->inlines) {
        /* The options carry the extension mask, so they have to reach the
         * inline parser: without this a document with extensions enabled
         * would parse its blocks as extended and its inline content as if
         * nothing were enabled. */
        status = md_inline_lines_opts_err(ctx->arena, ctx->text, lines, count,
                                          parent, ctx->refs, ctx->opts,
                                          &err_off);
        if (status == MD_ERR_LIMIT) {
            /* An inline bracket label nested past the limit. The offset is
             * absolute, unlike every other position here, which is why it
             * can name the exact line even when the offending text is the
             * second line of a multi-line block. */
            ctx_note_error_off(ctx, err_off);
        }
        return status;
    }
    return md_inline_lines_raw(ctx->arena, ctx->text, lines, count, parent);
}

/*
 * Consume a link reference definition ("[ref]: dest \"title\"") if the
 * current line is one. Definitions create no node: they only register a
 * label for later reference links and images.
 */
static md_status parse_refdef(md_block_ctx *ctx)
{
    size_t len = ctx_len(ctx, ctx->pos);
    size_t span = ctx->lines[ctx->pos].off;
    size_t label_off = 0;
    size_t label_len = 0;
    size_t dest_off = 0;
    size_t dest_len = 0;
    size_t title_off = 0;
    size_t title_len = 0;
    int has_title = 0;
    char *dest;
    char *title;

    if (!md_inline_scan_refdef(ctx->text, span, len, &label_off, &label_len,
                               &dest_off, &dest_len, &title_off, &title_len,
                               &has_title)) {
        return MD_OK; /* not a definition: leave the line alone */
    }
    dest = md_inline_unescape(ctx->arena, ctx->text, dest_off, dest_len);
    if (dest == NULL) {
        return MD_ERR_NOMEM;
    }
    title = has_title
                ? md_inline_unescape(ctx->arena, ctx->text, title_off, title_len)
                : NULL;
    if (has_title && title == NULL) {
        return MD_ERR_NOMEM;
    }
    if (md_refmap_define(ctx->refs, ctx->text + label_off, label_len, dest,
                         strlen(dest), title, title != NULL ? strlen(title) : 0u) != 0) {
        return MD_ERR_NOMEM;
    }
    ctx->pos++;
    return MD_OK;
}

static md_status make_heading(md_block_ctx *ctx, md_node *parent, int level,
                              const md_span *lines, size_t count)
{
    md_node *heading = md_node_new(ctx->arena, MD_NODE_HEADING);
    md_status status;

    if (heading == NULL) {
        return MD_ERR_NOMEM;
    }
    heading->level = level;
    if (md_node_append(ctx->arena, parent, heading) != MD_OK) {
        return MD_ERR_NOMEM;
    }
    status = finish_text(ctx, heading, lines, count);
    return status;
}

static md_status parse_atx_heading(md_block_ctx *ctx, md_node *parent,
                                   const char *p, size_t len, size_t indent)
{
    int level;
    size_t off;
    size_t clen;
    md_span span;
    md_node *heading;
    md_status status;

    if (!scan_atx_heading(p, len, indent, &level, &off, &clen)) {
        md_set_error("internal error: expected an ATX heading");
        return MD_ERR_PARSE;
    }
    heading = md_node_new(ctx->arena, MD_NODE_HEADING);
    if (heading == NULL) {
        return MD_ERR_NOMEM;
    }
    heading->level = level;
    status = md_node_append(ctx->arena, parent, heading);
    if (status != MD_OK) {
        return status;
    }
    span.off = ctx->lines[ctx->pos].off + off;
    span.len = clen;
    ctx->pos++;
    return finish_text(ctx, heading, &span, 1u);
}

static md_status parse_thematic_break(md_block_ctx *ctx, md_node *parent)
{
    md_node *node = md_node_new(ctx->arena, MD_NODE_THEMATIC_BREAK);
    md_status status;

    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, node);
    ctx->pos++;
    return status;
}

static md_status parse_paragraph(md_block_ctx *ctx, md_node *parent)
{
    size_t start = ctx->pos;
    size_t end;
    int level = 0;
    md_node *para;
    md_status status;

    /* A paragraph may open with link reference definitions, which produce
     * no output; they are stripped before the remaining lines are scanned. */
    while (ctx->inlines && ctx->refs != NULL &&
           ctx->pos < ctx->line_count) {
        size_t before = ctx->pos;
        const char *p = ctx_ptr(ctx, ctx->pos);
        size_t len = ctx_len(ctx, ctx->pos);

        if (md_line_is_blank(p, len) || p[0] == '>' || len >= 4u) {
            break;
        }
        /*
         * A footnote definition is also a syntactically valid link
         * reference definition, so without this the refdef scan below would
         * claim it first and the note would be recorded as a link label
         * instead. The extension wins while it is enabled; with it disabled
         * the line stays a reference definition, which is exactly what the
         * C1-C3 output requires.
         */
        if ((ctx_ext(ctx) & MD_EXT_FOOTNOTES) != 0u &&
            ctx->doc_root != NULL && scan_footnote_def(p, len, 0u, NULL, NULL,
                                                      NULL)) {
            break;
        }
        status = parse_refdef(ctx);
        if (status != MD_OK) {
            return status;
        }
        if (ctx->pos == before) {
            break; /* the line is not a definition */
        }
        start = ctx->pos;
    }
    if (start >= ctx->line_count) {
        return MD_OK; /* the paragraph held nothing but definitions */
    }
    end = start + 1u;

    while (end < ctx->line_count) {
        const char *p = ctx_ptr(ctx, end);
        size_t len = ctx_len(ctx, end);
        size_t indent;

        if (md_line_is_blank(p, len)) {
            break;
        }        indent = md_line_indent(p, len, 4u);
        if (indent < 4u && scan_setext_underline(p, len, indent, &level)) {
            md_status hstatus = make_heading(ctx, parent, level,
                                             ctx->lines + start, end - start);
            ctx->pos = end + 1u;
            return hstatus;
        }
        if (indent < 4u && line_starts_block(ctx, p, len, indent, end)) {
            break;
        }
        end++;
    }

    para = md_node_new(ctx->arena, MD_NODE_PARAGRAPH);
    if (para == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, para);
    if (status != MD_OK) {
        return status;
    }
    status = finish_text(ctx, para, ctx->lines + start, end - start);
    if (status != MD_OK) {
        return status;
    }
    ctx->pos = end;
    return MD_OK;
}

static md_status parse_fenced_code(md_block_ctx *ctx, md_node *parent,
                                   const char *p, size_t len, size_t indent)
{
    char fence;
    size_t fence_len;
    size_t info_off;
    size_t info_len;
    md_buffer content;
    char *raw;
    md_node *node;
    md_status status;

    if (!scan_fence(p, len, indent, &fence, &fence_len, &info_off, &info_len)) {
        md_set_error("internal error: expected a code fence");
        return MD_ERR_PARSE;
    }

    md_buffer_init(&content);
    ctx->pos++;
    while (!ctx_done(ctx)) {
        const char *cp = ctx_ptr(ctx, ctx->pos);
        size_t clen = ctx_len(ctx, ctx->pos);
        size_t skip;
        size_t cindent;

        if (scan_fence_close(cp, clen, fence, fence_len)) {
            ctx->pos++;
            break;
        }
        cindent = md_line_indent(cp, clen, indent);
        skip = cindent > indent ? indent : cindent;
        if (md_buffer_append(&content, cp + skip, clen - skip) != 0 ||
            md_buffer_append_char(&content, '\n') != 0) {
            md_buffer_free(&content);
            return MD_ERR_NOMEM;
        }
        ctx->pos++;
    }

    raw = md_buffer_release(&content);
    if (raw == NULL) {
        return MD_ERR_NOMEM;
    }
    node = md_make_code_block(ctx->arena,
                              p + info_off, info_len,
                              raw, strlen(raw));
    free(raw);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, node);
    return status;
}

static md_status parse_indented_code(md_block_ctx *ctx, md_node *parent)
{
    md_buffer content;
    char *raw;
    size_t last_content = 0; /* index in the buffer after the last real line */
    md_node *node;
    md_status status;
    size_t i;

    md_buffer_init(&content);
    while (!ctx_done(ctx)) {
        const char *p = ctx_ptr(ctx, ctx->pos);
        size_t len = ctx_len(ctx, ctx->pos);

        if (md_line_is_blank(p, len)) {
            size_t look = ctx->pos + 1u;
            int more = 0;

            while (look < ctx->line_count) {
                const char *lp = ctx_ptr(ctx, look);
                size_t ll = ctx_len(ctx, look);

                if (md_line_is_blank(lp, ll)) {
                    look++;
                    continue;
                }
                more = md_line_indent(lp, ll, 4u) >= 4u;
                break;
            }
            if (!more) {
                break;
            }
            if (md_buffer_append_char(&content, '\n') != 0) {
                md_buffer_free(&content);
                return MD_ERR_NOMEM;
            }
            ctx->pos++;
            continue;
        }
        if (md_line_indent(p, len, 4u) < 4u) {
            break;
        }
        if (md_buffer_append(&content, p + 4u, len - 4u) != 0 ||
            md_buffer_append_char(&content, '\n') != 0) {
            md_buffer_free(&content);
            return MD_ERR_NOMEM;
        }
        last_content = content.len;
        ctx->pos++;
    }

    raw = md_buffer_release(&content);
    if (raw == NULL) {
        return MD_ERR_NOMEM;
    }
    /* Trailing blank lines are not part of the code block. */
    i = strlen(raw);
    while (i > last_content) {
        i--;
    }
    node = md_make_code_block(ctx->arena, NULL, 0u, raw, i);
    free(raw);
    if (node == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, node);
    return status;
}

static md_status parse_block_quote(md_block_ctx *ctx, md_node *parent)
{
    md_span_builder builder;
    md_node *quote;
    md_status status;
    int last_blank = 0;

    if (spans_init(&builder, ctx->arena) != 0) {
        return MD_ERR_NOMEM;
    }
    quote = md_node_new(ctx->arena, MD_NODE_BLOCK_QUOTE);
    if (quote == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(ctx->arena, parent, quote);
    if (status != MD_OK) {
        return status;
    }

    while (!ctx_done(ctx)) {
        const char *p = ctx_ptr(ctx, ctx->pos);
        size_t len = ctx_len(ctx, ctx->pos);
        size_t indent = md_line_indent(p, len, 3u);
        md_span span;

        if (md_line_is_blank(p, len)) {
            /* Keep a blank line only when the quote continues afterwards. */
            size_t look = ctx->pos + 1u;
            int continues = 0;

            while (look < ctx->line_count) {
                const char *lp = ctx_ptr(ctx, look);
                size_t ll = ctx_len(ctx, look);
                size_t li = md_line_indent(lp, ll, 3u);

                if (md_line_is_blank(lp, ll)) {
                    look++;
                    continue;
                }
                continues = (li < ll && lp[li] == '>');
                break;
            }
            if (!continues) {
                break;
            }
            span.off = ctx->lines[ctx->pos].off;
            span.len = 0u;
            if (spans_push(&builder, span.off, span.len) != 0) {
                spans_abort(&builder);
                return MD_ERR_NOMEM;
            }
            last_blank = 1;
            ctx->pos++;
            continue;
        }

        if (indent < len && p[indent] == '>') {
            size_t start = indent + 1u;
            size_t rest = md_line_rtrim_len(p, len);

            if (start < rest && p[start] == ' ') {
                start++;
            }
            if (start > rest) {
                start = rest;
            }
            span.off = ctx->lines[ctx->pos].off + start;
            span.len = rest - start;
            if (spans_push(&builder, span.off, span.len) != 0) {
                spans_abort(&builder);
                return MD_ERR_NOMEM;
            }
            last_blank = 0;
            ctx->pos++;
            continue;
        }

        /* Lazy continuation of a paragraph inside the quote. */
        if (last_blank ||
            line_starts_block(ctx, p, len, md_line_indent(p, len, 4u), ctx->pos)) {
            break;
        }
        span.off = ctx->lines[ctx->pos].off;
        span.len = md_line_rtrim_len(p, len);
        if (spans_push(&builder, span.off, span.len) != 0) {
            spans_abort(&builder);
            return MD_ERR_NOMEM;
        }
        last_blank = 0;
        ctx->pos++;
    }

    if (spans_finish(&builder) != 0) {
        return MD_ERR_NOMEM;
    }
    return parse_container(ctx, quote, builder.out, builder.n);
}

static int list_marker_at(const md_block_ctx *ctx, size_t index, int interrupting,
                          md_list_marker *out)
{
    const char *p = ctx_ptr(ctx, index);
    size_t len = ctx_len(ctx, index);
    size_t indent = md_line_indent(p, len, 4u);

    if (indent >= 4u) {
        return 0;
    }
    return scan_list_marker(p, len, indent, interrupting, out);
}

static int same_list_kind(const md_list_marker *a, const md_list_marker *b)
{
    if (a->ordered != b->ordered) {
        return 0;
    }
    if (a->ordered) {
        return a->delim == b->delim;
    }
    return 1;
}

static md_status parse_list(md_block_ctx *ctx, md_node *parent)
{
    md_list_marker first;
    md_node *list;
    md_status status;
    char bullet = '\0';
    long start_number = 1;
    int loose = 0;
    int pending_blank = 0;

    if (!list_marker_at(ctx, ctx->pos, 0, &first)) {
        md_set_error("internal error: expected a list marker");
        return MD_ERR_PARSE;
    }
    if (first.ordered) {
        start_number = first.number;
    } else {
        bullet = ctx_ptr(ctx, ctx->pos)[first.indent];
    }

    list = md_node_new(ctx->arena, MD_NODE_LIST);
    if (list == NULL) {
        return MD_ERR_NOMEM;
    }
    list->ordered = first.ordered;
    list->start = start_number;
    list->tight = 1;
    status = md_node_append(ctx->arena, parent, list);
    if (status != MD_OK) {
        return status;
    }

    for (;;) {
        md_list_marker marker;
        md_span_builder builder;
        md_node *item;
        size_t content_start;
        int item_blank = 0;
        int item_has_content_after_blank = 0;
        int task_checked = -1;

        if (ctx_done(ctx)) {
            break;
        }

        /* Skip blank lines between items of the same list. */
        if (pending_blank) {
            size_t save = ctx->pos;

            while (!ctx_done(ctx) && md_line_is_blank(ctx_ptr(ctx, ctx->pos),
                                                     ctx_len(ctx, ctx->pos))) {
                ctx->pos++;
            }
            if (ctx_done(ctx) || !list_marker_at(ctx, ctx->pos, 0, &marker) ||
                !same_list_kind(&first, &marker)) {
                ctx->pos = save;
                break;
            }
            loose = 1;
            pending_blank = 0;
        }

        if (!list_marker_at(ctx, ctx->pos, 0, &marker) ||
            !same_list_kind(&first, &marker)) {
            break;
        }
        if (!first.ordered && ctx_ptr(ctx, ctx->pos)[marker.indent] != bullet) {
            break; /* a different bullet starts a new list */
        }

        content_start = marker.content_start;
        if (spans_init(&builder, ctx->arena) != 0) {
            return MD_ERR_NOMEM;
        }
        {
            const char *p = ctx_ptr(ctx, ctx->pos);
            size_t len = ctx_len(ctx, ctx->pos);
            size_t rest = md_line_rtrim_len(p, len);
            size_t start = content_start < rest ? content_start : rest;

            /* A task marker is taken off the item's content here, so it never
             * reaches the inline parser and never shows up in the text. */
            if ((ctx_ext(ctx) & MD_EXT_TASK_LIST) != 0u) {
                size_t after = 0;
                int checked = 0;

                if (md_ext_task_marker(p, len, start, &checked, &after)) {
                    task_checked = checked;
                    start = after < rest ? after : rest;
                }
            }
            if (spans_push(&builder, ctx->lines[ctx->pos].off + start,
                           rest - start) != 0) {
                spans_abort(&builder);
                return MD_ERR_NOMEM;
            }
            item_has_content_after_blank = 1;
        }
        ctx->pos++;

        while (!ctx_done(ctx)) {
            const char *p = ctx_ptr(ctx, ctx->pos);
            size_t len = ctx_len(ctx, ctx->pos);
            size_t indent = md_line_indent(p, len, content_start + 1u);

            if (md_line_is_blank(p, len)) {
                size_t look = ctx->pos + 1u;
                int more = 0;

                while (look < ctx->line_count) {
                    const char *lp = ctx_ptr(ctx, look);
                    size_t ll = ctx_len(ctx, look);

                    if (md_line_is_blank(lp, ll)) {
                        look++;
                        continue;
                    }
                    more = md_line_indent(lp, ll, content_start + 1u) >= content_start;
                    break;
                }
                if (!more) {
                    pending_blank = 1;
                    break;
                }
                if (spans_push(&builder, ctx->lines[ctx->pos].off, 0u) != 0) {
                    spans_abort(&builder);
                    return MD_ERR_NOMEM;
                }
                item_blank = 1;
                ctx->pos++;
                continue;
            }
            if (indent >= content_start) {
                size_t rest = md_line_rtrim_len(p, len);

                if (spans_push(&builder, ctx->lines[ctx->pos].off + content_start,
                               rest - content_start) != 0) {
                    spans_abort(&builder);
                    return MD_ERR_NOMEM;
                }
                if (item_blank) {
                    item_has_content_after_blank = 1;
                }
                item_blank = 0;
                ctx->pos++;
                continue;
            }
            /* A sibling item ends this one. */
            if (list_marker_at(ctx, ctx->pos, 0, &marker) &&
                same_list_kind(&first, &marker) &&
                (first.ordered || ctx_ptr(ctx, ctx->pos)[marker.indent] == bullet)) {
                break;
            }
            if (list_marker_at(ctx, ctx->pos, 0, &marker) &&
                !same_list_kind(&first, &marker)) {
                break;
            }
            /* Lazy continuation of the item's trailing paragraph. */
            if (item_blank ||
                line_starts_block(ctx, p, len, md_line_indent(p, len, 4u),
                                  ctx->pos)) {
                break;
            }
            if (spans_push(&builder, ctx->lines[ctx->pos].off,
                           md_line_rtrim_len(p, len)) != 0) {
                spans_abort(&builder);
                return MD_ERR_NOMEM;
            }
            item_blank = 0;
            ctx->pos++;
        }

        if (spans_finish(&builder) != 0) {
            return MD_ERR_NOMEM;
        }
        if (item_blank && item_has_content_after_blank) {
            loose = 1;
        }

        item = md_node_new(ctx->arena, MD_NODE_ITEM);
        if (item == NULL) {
            return MD_ERR_NOMEM;
        }
        if (task_checked >= 0) {
            md_node_set_task(item, task_checked);
        }
        status = md_node_append(ctx->arena, list, item);
        if (status != MD_OK) {
            return status;
        }
        status = parse_container(ctx, item, builder.out, builder.n);
        if (status != MD_OK) {
            return status;
        }
    }

    list->tight = loose ? 0 : 1;
    if (list->child_count == 0u) {
        /* No item was produced: drop the empty list. */
        if (parent->child_count > 0u &&
            parent->children[parent->child_count - 1u] == list) {
            parent->child_count--;
        }
    }
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Block dispatch                                                      */
/* ------------------------------------------------------------------ */

static md_status parse_blocks(md_block_ctx *ctx, md_node *parent)
{
    if (ctx->arena == NULL || ctx->lines == NULL || parent == NULL) {
        md_set_error("invalid argument: parse_blocks");
        return MD_ERR_INVAL;
    }
    /*
     * The one place the parser descends the C stack, and therefore the one
     * place that has to be bounded. The limit comes from the options, is
     * clamped to MD_MAX_NESTING_HARD by md_limits_nesting(), and can never
     * be raised past that by any configuration -- which is what makes
     * "never overflow the C stack" a property of the library.
     */
    if (ctx->depth > ctx->max_nesting) {
        ctx_note_error(ctx);
        md_set_error_kind(MD_ERROR_NESTING, "nesting too deep");
        return MD_ERR_LIMIT;
    }

    while (!ctx_done(ctx)) {
        const char *p = ctx_ptr(ctx, ctx->pos);
        size_t len = ctx_len(ctx, ctx->pos);
        size_t indent;
        md_list_marker marker;
        md_status status;
        int level;
        size_t off;
        size_t clen;
        char fence;
        size_t fence_len;
        size_t info_off;
        size_t info_len;

        if (md_line_is_blank(p, len)) {
            ctx->pos++;
            continue;
        }
        indent = md_line_indent(p, len, 4u);
        if (indent >= 4u) {
            status = parse_indented_code(ctx, parent);
        } else if (ctx->inlines && ctx->refs != NULL &&
                   !((ctx_ext(ctx) & MD_EXT_FOOTNOTES) != 0u &&
                     ctx->doc_root != NULL &&
                     scan_footnote_def(p, len, indent, NULL, NULL, NULL))) {
            /* A link reference definition creates no node. A footnote
             * definition also scans as one, so it is excluded here and
             * offered to the extension instead; otherwise the note would
             * silently become a link label and lose its text. */
            size_t before = ctx->pos;

            status = parse_refdef(ctx);
            if (status == MD_OK && ctx->pos == before) {
                status = parse_blocks_dispatch(ctx, parent, p, len, indent,
                                               &level, &off, &clen, &fence,
                                               &fence_len, &info_off,
                                               &info_len, &marker);
            }
        } else {
            status = parse_blocks_dispatch(ctx, parent, p, len, indent, &level,
                                           &off, &clen, &fence, &fence_len,
                                           &info_off, &info_len, &marker);
        }
        if (status != MD_OK) {
            /*
             * A failure raised anywhere below -- an arena that hit its cap,
             * say -- is attributed to the line this container was working
             * on, so the structured error still points into the document.
             * ctx_note_error() keeps the first, most specific, position.
             */
            ctx_note_error(ctx);
            return status;
        }
    }
    return MD_OK;
}

/*
 * Strip container markers so that a nested definition is seen as if it were
 * at the top level: up to three spaces and any number of ">" prefixes.
 */
static void strip_containers(const char *p, size_t *off, size_t *rest)
{
    size_t i = 0;

    for (;;) {
        size_t spaces = 0;

        while (i < *rest && p[i] == ' ' && spaces < 3u) {
            i++;
            spaces++;
        }
        if (i < *rest && p[i] == '>') {
            i++;
            if (i < *rest && p[i] == ' ') {
                i++;
            }
            continue;
        }
        break;
    }
    *off = i;
}

/* Is the line a fence line for `c` with at least `n` markers? */
static int is_fence_line(const char *p, size_t len, char c, size_t n)
{
    size_t i = 0;
    size_t count = 0;

    while (i < len && p[i] == ' ' && i < 3u) {
        i++;
    }
    if (len - i < n) {
        return 0;
    }
    while (i < len && p[i] == c) {
        i++;
        count++;
    }
    if (count < n) {
        return 0;
    }
    while (i < len) {
        if (p[i] != ' ' && p[i] != '\t') {
            return 0;
        }
        i++;
    }
    return 1;
}

/*
 * Register every link reference definition of the document before block
 * parsing starts, so that a definition may be used by a reference that
 * appears earlier in the file. Fenced code and indented code are skipped,
 * and a definition only counts where a block may start.
 */
static md_status collect_refdefs(md_block_ctx *ctx, md_ext_flags ext)
{
    char fence = '\0';
    size_t fence_len = 0;
    int at_block_start = 1;
    size_t i;

    for (i = 0; i < ctx->line_count; i++) {
        const md_span *span = &ctx->lines[i];
        const char *p = ctx->text + span->off;
        size_t len = span->len;
        size_t off = 0;
        size_t label_off = 0;
        size_t label_len = 0;
        size_t dest_off = 0;
        size_t dest_len = 0;
        size_t title_off = 0;
        size_t title_len = 0;
        int has_title = 0;
        char *dest;
        char *title;
        size_t fn_label_off = 0;
        size_t fn_label_len = 0;

        strip_containers(p, &off, &len);
        p += off;
        if (fence != '\0') {
            if (is_fence_line(p, len, fence, fence_len)) {
                fence = '\0';
                fence_len = 0;
            }
            at_block_start = 0;
            continue;
        }
        if (is_fence_line(p, len, '`', 3u) || is_fence_line(p, len, '~', 3u)) {
            fence = p[0];
            fence_len = 0;
            while (fence_len < len && p[fence_len] == fence) {
                fence_len++;
            }
            at_block_start = 0;
            continue;
        }
        if (md_line_is_blank(p, len)) {
            at_block_start = 1;
            continue;
        }
        if (md_line_indent(p, len, 4u) >= 4u) {
            at_block_start = 0; /* indented code */
            continue;
        }
        if (!at_block_start) {
            at_block_start = 0;
            continue; /* a lazy continuation line cannot start a definition */
        }
        /*
         * A footnote definition is also a syntactically valid link
         * reference definition, so it is recognized before the refdef scan
         * below would claim it: the label is registered under the footnote
         * key, which is the label without the '^'. Registering it as a
         * link label instead would both lose the note and turn every
         * matching reference into a link.
         */
        if ((ext & MD_EXT_FOOTNOTES) != 0u &&
            scan_footnote_def(p, len, 0u, &fn_label_off, &fn_label_len,
                              NULL)) {
            if (md_refmap_define(ctx->refs, p + fn_label_off, fn_label_len, "",
                                 0u, NULL, 0u) != 0) {
                return MD_ERR_NOMEM;
            }
            at_block_start = 0;
            continue;
        }
        if (!md_inline_scan_refdef(ctx->text, ctx->lines[i].off + off, len,
                                   &label_off, &label_len, &dest_off,
                                   &dest_len, &title_off, &title_len,
                                   &has_title)) {
            at_block_start = 0;
            continue;
        }
        dest = md_inline_unescape(ctx->arena, ctx->text, dest_off, dest_len);
        if (dest == NULL) {
            return MD_ERR_NOMEM;
        }
        title = has_title
                    ? md_inline_unescape(ctx->arena, ctx->text, title_off, title_len)
                    : NULL;
        if (has_title && title == NULL) {
            return MD_ERR_NOMEM;
        }
        if (md_refmap_define(ctx->refs, ctx->text + label_off, label_len, dest,
                             strlen(dest), title,
                             title != NULL ? strlen(title) : 0u) != 0) {
            return MD_ERR_NOMEM;
        }
    }
    return MD_OK;
}

static md_status parse_blocks_into_ex(md_arena *arena, const md_lines *lines,
                                      md_node *parent, unsigned depth,
                                      md_refmap *refs, int inlines,
                                      const md_options *opts,
                                      md_node *doc_root,
                                      md_block_err *err)
{
    md_block_ctx ctx;

    if (arena == NULL || lines == NULL || parent == NULL) {
        md_set_error("invalid argument: md_parse_blocks_into");
        return MD_ERR_INVAL;
    }
    ctx.arena = arena;
    ctx.text = lines->text;
    ctx.text_len = lines->text_len;
    ctx.lines = lines->spans;
    ctx.line_count = lines->count;
    ctx.pos = 0;
    ctx.depth = depth;
    ctx.max_nesting = md_limits_nesting(opts);
    ctx.refs = refs;
    ctx.inlines = inlines;
    ctx.opts = opts;
    ctx.doc_root = doc_root;
    ctx.err = err;
    if (inlines && refs != NULL) {
        md_status collected = collect_refdefs(&ctx, ctx_ext(&ctx));

        if (collected != MD_OK) {
            ctx_note_error(&ctx);
            return collected;
        }
    }
    return parse_blocks(&ctx, parent);
}

md_status md_parse_blocks_into(md_arena *arena, const md_lines *lines,
                               md_node *parent, unsigned depth)
{
    return parse_blocks_into_ex(arena, lines, parent, depth, NULL, 0, NULL,
                                NULL, NULL);
}

/*
 * Turn a byte offset in the document text into the 1-based line and column
 * the structured error reports. Offsets are produced by ctx_note_error() from
 * the document text itself, so the conversion is exact; an offset that fell
 * outside every line (which cannot happen today) leaves the position at 0
 * rather than reporting a place the failure was not at.
 */
static void err_offset_to_line(const md_lines *lines, size_t off, size_t *line,
                               size_t *col)
{
    size_t i;

    *line = 0u;
    *col = 0u;
    for (i = 0; i < lines->count; i++) {
        size_t start = lines->spans[i].off;
        size_t end = start + lines->spans[i].len;

        if (off >= start && off <= end) {
            *line = i + 1u;
            *col = off - start + 1u;
            return;
        }
    }
}

/*
 * Everything a document parse does after its input has been turned into
 * lines. Buffered and streaming entry points both land here, which is what
 * makes their output identical rather than merely similar: there is one
 * normalizer, one line splitter, and one block parser.
 */
static md_document *parse_lines_in(md_arena *arena, md_lines *lines,
                                   int inlines, const md_options *opts)
{
    md_document *doc;
    md_refmap *refs = NULL;
    md_block_err err;
    md_status status;

    doc = md_document_create(arena, 0);
    if (doc == NULL) {
        return NULL;
    }
    if (inlines) {
        refs = md_refmap_new(arena);
        if (refs == NULL) {
            return NULL;
        }
    }
    /* The document records the mask it was parsed with, so JSON can tell
     * whether "footnotes" and "checked" belong to this tree. */
    doc->root->extensions = opts != NULL ? opts->extensions : MD_EXT_NONE;
    err.off = 0u;
    err.set = 0;
    status = parse_blocks_into_ex(arena, lines, doc->root, 0u, refs, inlines,
                                  opts, doc->root, &err);
    md_refmap_destroy(refs);
    if (status != MD_OK) {
        if (err.set) {
            size_t line;
            size_t col;

            err_offset_to_line(lines, err.off, &line, &col);
            md_error_set_position(line, col);
        }
        if (md_error_message()[0] == '\0') {
            md_set_error_kindf(MD_ERROR_PARSE, "parse failed: %s",
                               md_status_string(status));
        }
        return NULL;
    }
    return doc;
}

static md_document *parse_document_in(md_arena *arena, const char *src,
                                      size_t len, int inlines,
                                      const md_options *opts)
{
    md_lines lines;

    if (md_lines_build(arena, src, len, &lines) != 0) {
        return NULL;
    }
    return parse_lines_in(arena, &lines, inlines, opts);
}

md_document *md_parse_blocks_in(md_arena *arena, const char *src, size_t len)
{
    if (arena == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_parse_blocks_in");
        return NULL;
    }
    return parse_document_in(arena, src, len, 0, NULL);
}

md_document *md_parse_blocks_n(const char *src, size_t len)
{
    md_arena *arena;
    md_document *doc;

    arena = md_arena_create();
    if (arena == NULL) {
        return NULL;
    }
    doc = parse_document_in(arena, src, len, 0, NULL);
    if (doc == NULL) {
        md_arena_destroy(arena);
        return NULL;
    }
    doc->owns_arena = 1;
    return doc;
}

md_document *md_parse(const char *src)
{
    if (src == NULL) {
        md_set_error("invalid argument: md_parse(NULL)");
        return NULL;
    }
    return md_parse_n(src, strlen(src));
}

md_document *md_parse_in(md_arena *arena, const char *src, size_t len)
{
    if (arena == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_parse_in");
        return NULL;
    }
    return parse_document_in(arena, src, len, 1, NULL);
}

md_document *md_parse_n(const char *src, size_t len)
{
    md_arena *arena;
    md_document *doc;

    arena = md_arena_create();
    if (arena == NULL) {
        return NULL;
    }
    doc = parse_document_in(arena, src, len, 1, NULL);
    if (doc == NULL) {
        md_arena_destroy(arena);
        return NULL;
    }
    doc->owns_arena = 1;
    return doc;
}

/*
 * The checkpoint C4 entry points. Each takes the extension mask and the
 * custom registry to apply; the entry points above are exactly these with an
 * empty md_options, which is why C1-C3 output does not move.
 */
md_document *md_parse_opts(const md_options *options, const char *src)
{
    if (src == NULL) {
        md_set_error("invalid argument: md_parse_opts(NULL)");
        return NULL;
    }
    return md_parse_opts_n(options, src, strlen(src));
}

md_document *md_parse_opts_n(const md_options *options, const char *src,
                             size_t len)
{
    md_arena *arena;
    md_document *doc;

    arena = md_arena_create_limit(md_limits_bytes(options));
    if (arena == NULL) {
        return NULL;
    }
    doc = parse_document_in(arena, src, len, 1, options);
    if (doc == NULL) {
        md_arena_destroy(arena);
        return NULL;
    }
    doc->owns_arena = 1;
    return doc;
}

md_document *md_parse_opts_in(md_arena *arena, const md_options *options,
                              const char *src, size_t len)
{
    if (arena == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_parse_opts_in");
        return NULL;
    }
    return parse_document_in(arena, src, len, 1, options);
}

md_document *md_parse_blocks(const char *src)
{
    if (src == NULL) {
        md_set_error("invalid argument: md_parse_blocks(NULL)");
        return NULL;
    }
    return md_parse_blocks_n(src, strlen(src));
}

/* ------------------------------------------------------------------ */
/* Streaming (checkpoint C5)                                           */
/* ------------------------------------------------------------------ */

/*
 * Parse the rest of `in` as a Markdown document.
 *
 * The stream is read forward in fixed-size chunks and never held whole, so a
 * document that does not fit in memory is a bounded-memory parse up to the
 * point where a cap stops it, not a second copy of itself next to the file.
 * `in` is not rewound and not closed; the caller owns it, and a stream
 * positioned part-way through parses exactly the remainder.
 *
 * A NULL `options` is an empty option set, so md_parse_stream(f, NULL) is
 * the streaming form of md_parse(f's contents). The lines, the block parser
 * and the options are shared with the buffered entry points, so the same
 * bytes through either path produce the same AST and the same JSON.
 *
 * `max_bytes` bounds the input that will be read. The arena memory cap in
 * `options` applies on top of it and covers the whole parse, not just the
 * read; 0 selects the library default of MD_MAX_INPUT_BYTES.
 */
md_document *md_parse_stream_in(md_arena *arena, FILE *in, size_t max_bytes,
                                const md_options *options)
{
    md_lines lines;

    if (arena == NULL || in == NULL) {
        md_set_error_kind(MD_ERROR_ARGUMENT, "invalid argument: md_parse_stream_in");
        return NULL;
    }
    if (md_lines_read(arena, in, max_bytes, &lines) != 0) {
        return NULL;
    }
    return parse_lines_in(arena, &lines, 1, options);
}

md_document *md_parse_stream(FILE *in, const MDParseOptions *options)
{
    md_arena *arena;
    md_document *doc;

    if (in == NULL) {
        md_set_error_kind(MD_ERROR_ARGUMENT, "invalid argument: md_parse_stream");
        return NULL;
    }
    /* The input cap defaults to the library limit; the memory cap, when one
     * is configured, is the tighter of the two in practice because the
     * normalized text alone is charged against it. */
    arena = md_arena_create_limit(md_limits_bytes(options));
    if (arena == NULL) {
        return NULL;
    }
    doc = md_parse_stream_in(arena, in, 0u, options);
    if (doc == NULL) {
        md_arena_destroy(arena);
        return NULL;
    }
    doc->owns_arena = 1;
    return doc;
}
