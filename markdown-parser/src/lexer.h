#ifndef MD_LEXER_H
#define MD_LEXER_H

/*
 * Input normalization and line scanning.
 *
 * Before any block parsing the source is normalized once: CR, CRLF and lone
 * CR become LF, tabs are expanded to four-space stops, and NUL bytes are
 * replaced by U+FFFD. Every other byte (including UTF-8 continuation bytes)
 * is preserved verbatim, so offsets and output are deterministic.
 */

#include "arena.h"
#include "status.h"

#include <stddef.h>
#include <stdio.h>

#define MD_TAB_STOP 4u
#define MD_MAX_INPUT_BYTES ((size_t)256u * 1024u * 1024u)

/*
 * Read chunk size of md_lines_read(). A fixed buffer is what makes streaming
 * bounded: the raw input is never held whole, only the normalized text the
 * parser genuinely needs, plus this much scratch space.
 */
#define MD_STREAM_CHUNK ((size_t)64u * 1024u)

typedef struct md_span {
    size_t off;
    size_t len;
} md_span;

typedef struct md_lines {
    char *text;      /* arena-owned, NUL-terminated, tab-free */
    size_t text_len;
    md_span *spans;  /* arena-owned array of text_len + 1 lines */
    size_t count;
} md_lines;

/* Normalize src[0..len) into arena memory. Returns 0 on success. */
int md_normalize(md_arena *arena, const char *src, size_t len, char **out, size_t *out_len);

/*
 * Read a whole stream, normalize it and split it into lines, exactly as
 * md_lines_build() does for a buffer.
 *
 * `in` is read forward in fixed-size chunks and is never rewound or closed;
 * the caller owns it. `max_bytes` bounds the input that will be accepted (0
 * for the MD_MAX_INPUT_BYTES default) and is reported as MD_ERROR_MEMORY when
 * exceeded, so a caller can impose a smaller ceiling than the library's own
 * input limit.
 *
 * Because this produces the same lines, from the same normalizer, as
 * md_lines_build(), a streamed document and a buffered one parse to the same
 * AST: the two entry points share every step after the read.
 *
 * Returns 0 on success, -1 on failure with a diagnostic recorded.
 */
int md_lines_read(md_arena *arena, FILE *in, size_t max_bytes, md_lines *out);

/* Normalize and split into lines. Returns 0 on success. */
int md_lines_build(md_arena *arena, const char *src, size_t len, md_lines *out);

const char *md_line_ptr(const md_lines *lines, size_t index);
md_span md_line_span(const md_lines *lines, size_t index);

/* --- line predicates (tabs already expanded, so spaces only) --- */

/* Count leading spaces, capped at max. */
size_t md_line_indent(const char *p, size_t len, size_t max);
int md_line_is_blank(const char *p, size_t len);
/* Length after removing trailing spaces. */
size_t md_line_rtrim_len(const char *p, size_t len);

#endif
