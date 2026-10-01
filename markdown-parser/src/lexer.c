#include "lexer.h"

#include "status.h"

#include <stdlib.h>
#include <string.h>
/* U+FFFD, the replacement character CommonMark substitutes for NUL. */
#define MD_REPLACEMENT "\xef\xbf\xbd"

/*
 * Normalization state (checkpoint C5). md_normalize() and md_lines_read()
 * share this so that a stream and a buffer are normalized by the same code,
 * byte for byte, no matter where the chunk boundaries fall.
 *
 * The only state a chunk boundary can lose is the decision about a CR: a CR
 * at the very end of a chunk may or may not be followed by the LF that turns
 * it into a single line break, so it is held back until the next byte is
 * known. That is the one case where splitting the input mid-way would
 * otherwise change the output.
 */
typedef struct md_normalizer {
    md_buffer buf;   /* normalized bytes so far */
    size_t col;      /* column of the next byte, for tab stops */
    int pending_cr;  /* a CR was seen and is not yet decided */
    size_t stop_at;  /* stop once buf.len reaches this; 0 for no limit */
} md_normalizer;

static void md_normalizer_init(md_normalizer *norm)
{
    md_buffer_init(&norm->buf);
    norm->col = 0u;
    norm->pending_cr = 0;
    norm->stop_at = 0u;
}

/*
 * A memory ceiling on the normalized text itself.
 *
 * The arena charges the text the moment it is adopted, so without this the
 * stream reader would read an arbitrarily large document into a heap buffer
 * and only discover at the end that the arena cap could not take it. The
 * ceiling is checked after every chunk, which bounds a refused stream at one
 * chunk of overshoot rather than at the size of the whole input. The arena
 * is not asked to reserve anything: this is a stopping condition, and the
 * arena reports the failure afterwards, on adoption.
 */
static void md_normalizer_set_limit(md_normalizer *norm, size_t stop_at)
{
    norm->stop_at = stop_at;
}

/* Has the normalized text reached the point where it cannot be kept? */
static int md_normalizer_over_limit(const md_normalizer *norm)
{
    return norm->stop_at != 0u && norm->buf.len > norm->stop_at;
}

/*
 * Normalize src[0..len) into the buffer. Returns 0 on success, -1 on failure
 * with a diagnostic recorded. The caller must keep calling with consecutive
 * chunks and then md_normalizer_finish() exactly once.
 */
static int md_normalize_feed(md_normalizer *norm, const char *src, size_t len)
{
    size_t i = 0;

    if (norm->pending_cr) {
        /* The held CR becomes one line break, and a following LF is eaten. */
        norm->pending_cr = 0;
        if (md_buffer_append_char(&norm->buf, '\n') != 0) {
            return -1;
        }
        norm->col = 0u;
        if (len > 0u && src[0] == '\n') {
            i = 1u;
        }
    }
    while (i < len) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\r') {
            if (i + 1u < len) {
                /* Both halves of a CRLF are in this chunk, so it is decided. */
                if ((unsigned char)src[i + 1u] == '\n') {
                    i++;
                }
                if (md_buffer_append_char(&norm->buf, '\n') != 0) {
                    return -1;
                }
                i++;
                norm->col = 0u;
                continue;
            }
            /* The chunk ends here: decide on the next feed. */
            norm->pending_cr = 1;
            i++;
            continue;
        }
        if (c == '\n') {
            if (md_buffer_append_char(&norm->buf, '\n') != 0) {
                return -1;
            }
            i++;
            norm->col = 0u;
            continue;
        }
        if (c == '\t') {
            size_t width = MD_TAB_STOP - (norm->col % MD_TAB_STOP);
            size_t k;

            for (k = 0; k < width; k++) {
                if (md_buffer_append_char(&norm->buf, ' ') != 0) {
                    return -1;
                }
            }
            norm->col += width;
            i++;
            continue;
        }
        if (c == '\0') {
            if (md_buffer_append(&norm->buf, MD_REPLACEMENT, 3u) != 0) {
                return -1;
            }
            norm->col += 1u; /* one character wide, not three bytes */
            i++;
            continue;
        }
        if (md_buffer_append_char(&norm->buf, (char)c) != 0) {
            return -1;
        }
        /* Continuation bytes never widen the column: count UTF-8 characters,
         * not bytes, so tab stops line up with rendered text. */
        if ((c & 0xc0u) != 0x80u) {
            norm->col++;
        }
        i++;
    }
    return 0;
}

static int md_normalizer_finish(md_normalizer *norm)
{
    if (norm->pending_cr) {
        norm->pending_cr = 0;
        return md_buffer_append_char(&norm->buf, '\n');
    }
    return 0;
}

int md_normalize(md_arena *arena, const char *src, size_t len, char **out, size_t *out_len)
{
    md_normalizer norm;
    char *data;

    if (arena == NULL || out == NULL || out_len == NULL || (src == NULL && len > 0)) {
        md_set_error("invalid argument: md_normalize");
        return -1;
    }
    if (len > MD_MAX_INPUT_BYTES) {
        md_set_error_kindf(MD_ERROR_MEMORY, "input too large (%zu bytes, limit %zu)",
                           len, MD_MAX_INPUT_BYTES);
        return -1;
    }

    md_normalizer_init(&norm);
    /*
     * A capped arena caps the text too, and the check is the same one
     * md_lines_read() makes, so a refused buffer and a refused stream are
     * reported identically and at the same point.
     *
     * The cap is deliberately not checked against the raw length first.
     * Normalization both grows and shrinks the text -- a tab becomes up to
     * four spaces and a NUL becomes three bytes, but a CRLF pair collapses
     * to one -- so the raw length is an upper bound on the result, never a
     * lower one. Refusing early on it would turn a document that fits after
     * normalization into a false limit failure: a megabyte of CRLF text
     * normalizes to half a megabyte and belongs in a 600k arena.
     */
    md_normalizer_set_limit(&norm, md_arena_limit(arena));
    if (md_normalize_feed(&norm, src, len) != 0 || md_normalizer_finish(&norm) != 0) {
        md_buffer_free(&norm.buf);
        if (md_error_message()[0] == '\0') {
            md_set_error("out of memory normalizing input");
        }
        return -1;
    }
    if (md_normalizer_over_limit(&norm)) {
        md_set_error_kindf(MD_ERROR_MEMORY,
                           "memory cap exceeded (limit %zu bytes, %zu already used)",
                           md_arena_limit(arena), md_arena_bytes_used(arena));
        md_buffer_free(&norm.buf);
        return -1;
    }
    data = md_buffer_release(&norm.buf);
    if (data == NULL) {
        return -1;
    }
    /*
     * Adopt the buffer instead of copying it into the arena. The normalized
     * text is the largest single allocation a parse makes, and this is what
     * keeps a large document at one copy of itself rather than two.
     */
    {
        size_t n = strlen(data);

        if (md_arena_adopt(arena, data, n) != 0) {
            free(data);
            return -1;
        }
        *out = data;
        *out_len = n;
    }
    return 0;
}

/*
 * Build the line spans over an already normalized, arena-owned text. Shared
 * by the buffered and the streaming path, so a document that arrived in
 * chunks and the same document that arrived in one buffer are split by
 * exactly the same code and therefore produce the same lines.
 *
 * The span array holds text_len + 1 entries, the worst case being a text of
 * nothing but newlines, so a single allocation of that size is always enough
 * and no reallocation is ever needed.
 */
static int md_lines_split(md_arena *arena, md_lines *out, char *text,
                          size_t text_len)
{
    size_t count = 1;
    size_t i;
    size_t start = 0;
    md_span *spans;

    for (i = 0; i < text_len; i++) {
        if (text[i] == '\n') {
            count++;
        }
    }
    spans = (md_span *)md_arena_alloc(arena, count * sizeof *spans);
    if (spans == NULL) {
        return -1;
    }

    for (i = 0; i < text_len; i++) {
        if (text[i] == '\n') {
            spans[out->count].off = start;
            spans[out->count].len = i - start;
            out->count++;
            start = i + 1u;
        }
    }
    if (start < text_len) {
        spans[out->count].off = start;
        spans[out->count].len = text_len - start;
        out->count++;
    }
    /* A trailing newline ends the last line; do not emit an extra empty one. */

    out->text = text;
    out->text_len = text_len;
    out->spans = spans;
    return 0;
}

static void md_lines_clear(md_lines *out)
{
    if (out == NULL) {
        return;
    }
    out->text = NULL;
    out->text_len = 0u;
    out->spans = NULL;
    out->count = 0u;
}

int md_lines_build(md_arena *arena, const char *src, size_t len, md_lines *out)
{
    char *text = NULL;
    size_t text_len = 0;

    if (out == NULL) {
        md_set_error("invalid argument: md_lines_build");
        return -1;
    }
    md_lines_clear(out);

    if (md_normalize(arena, src, len, &text, &text_len) != 0) {
        return -1;
    }
    return md_lines_split(arena, out, text, text_len);
}

int md_lines_read(md_arena *arena, FILE *in, size_t max_bytes, md_lines *out)
{
    md_normalizer norm;
    size_t raw = 0;
    char *chunk;
    char *text;
    int rc = -1;

    if (arena == NULL || in == NULL || out == NULL) {
        md_set_error("invalid argument: md_lines_read");
        return -1;
    }
    md_lines_clear(out);
    if (max_bytes == 0u) {
        max_bytes = MD_MAX_INPUT_BYTES;
    }
    chunk = (char *)malloc(MD_STREAM_CHUNK);
    if (chunk == NULL) {
        md_set_error("out of memory reading stream");
        return -1;
    }
    md_normalizer_init(&norm);
    /* A capped arena caps the text too, so an unbounded stream is refused
     * after a bounded amount of reading rather than after all of it. */
    md_normalizer_set_limit(&norm, md_arena_limit(arena));

    /*
     * The input is consumed in fixed-size chunks and only the normalized
     * text is kept, so the memory a stream costs is one copy of the
     * document plus this buffer -- not the raw bytes on top of it, which is
     * what a caller that slurped the whole stream first would hold.
     */
    for (;;) {
        size_t got = fread(chunk, 1u, MD_STREAM_CHUNK, in);

        /* Checked before use, and as a subtraction so the sum cannot wrap. */
        if (raw > max_bytes || got > max_bytes - raw) {
            md_set_error_kindf(MD_ERROR_MEMORY, "input too large (limit %zu bytes)",
                               max_bytes);
            goto done;
        }
        if (got > 0u) {
            if (md_normalize_feed(&norm, chunk, got) != 0) {
                if (md_error_message()[0] == '\0') {
                    md_set_error("out of memory normalizing stream");
                }
                goto done;
            }
            raw += got;
        }
        if (md_normalizer_over_limit(&norm)) {
            /*
             * The arena would not take this text even if the rest of the
             * stream were empty, so there is nothing to be gained by reading
             * it. The message is the one md_arena_adopt() would have
             * produced, so a refused stream and a refused buffer are
             * reported identically.
             */
            md_set_error_kindf(MD_ERROR_MEMORY,
                               "memory cap exceeded (limit %zu bytes, %zu already used)",
                               md_arena_limit(arena), md_arena_bytes_used(arena));
            goto done;
        }
        if (got < MD_STREAM_CHUNK) {
            if (ferror(in)) {
                md_set_error_kind(MD_ERROR_IO, "read error");
                goto done;
            }
            if (feof(in)) {
                break;
            }
        }
    }
    if (md_normalizer_finish(&norm) != 0) {
        if (md_error_message()[0] == '\0') {
            md_set_error("out of memory normalizing stream");
        }
        goto done;
    }
    text = md_buffer_release(&norm.buf);
    if (text == NULL) {
        goto done;
    }
    if (md_arena_adopt(arena, text, strlen(text)) != 0) {
        free(text);
        goto done;
    }
    /* From here the text belongs to the arena, and the line spans come from
     * the same splitter md_lines_build() uses. */
    rc = md_lines_split(arena, out, text, strlen(text));

done:
    free(chunk);
    if (rc != 0) {
        md_buffer_free(&norm.buf);
    }
    return rc;
}

const char *md_line_ptr(const md_lines *lines, size_t index)
{
    if (lines == NULL || index >= lines->count) {
        return NULL;
    }
    return lines->text + lines->spans[index].off;
}

md_span md_line_span(const md_lines *lines, size_t index)
{
    if (lines == NULL || index >= lines->count) {
        md_span empty = { 0u, 0u };
        return empty;
    }
    return lines->spans[index];
}

size_t md_line_indent(const char *p, size_t len, size_t max)
{
    size_t n = 0;

    if (p == NULL) {
        return 0;
    }
    while (n < len && n < max && p[n] == ' ') {
        n++;
    }
    return n;
}

int md_line_is_blank(const char *p, size_t len)
{
    size_t i;

    if (p == NULL) {
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (p[i] != ' ') {
            return 0;
        }
    }
    return 1;
}

size_t md_line_rtrim_len(const char *p, size_t len)
{
    if (p == NULL) {
        return 0;
    }
    while (len > 0 && p[len - 1u] == ' ') {
        len--;
    }
    return len;
}
