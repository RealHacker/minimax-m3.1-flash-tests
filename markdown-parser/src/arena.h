#ifndef MD_ARENA_H
#define MD_ARENA_H

/*
 * Bump allocator. Every parsed document owns one arena; a single
 * md_arena_destroy() releases the whole AST. All functions report failure by
 * returning NULL and by recording a diagnostic via md_set_error().
 */

#include <stddef.h>

typedef struct md_arena_chunk md_arena_chunk;

typedef struct md_arena {
    md_arena_chunk *head; /* most recent chunk */
    size_t total;         /* bytes handed out, for diagnostics */
    size_t cap_bytes;     /* memory cap in force, 0 when uncapped */
} md_arena;

md_arena *md_arena_create(void);

/*
 * As md_arena_create(), but with a memory cap (checkpoint C5). Once more than
 * max_bytes have been handed out, md_arena_alloc() fails with a diagnostic
 * of kind MD_ERROR_MEMORY instead of growing; max_bytes of 0 means no cap.
 *
 * The cap is what makes a parse of a hostile document bounded: the parser
 * stops with a structured error instead of asking the operating system for
 * more and more memory. It is an upper bound, not a reservation, so a parse
 * that stays well under the cap is unaffected.
 */
md_arena *md_arena_create_limit(size_t max_bytes);
void md_arena_destroy(md_arena *arena);

/* The cap in force, or 0 when the arena is uncapped. */
size_t md_arena_limit(const md_arena *arena);

void *md_arena_alloc(md_arena *arena, size_t size);
void *md_arena_calloc(md_arena *arena, size_t count, size_t size);
char *md_arena_strndup(md_arena *arena, const char *s, size_t len);
char *md_arena_strdup(md_arena *arena, const char *s);

/* Copy len bytes of possibly non-terminated, possibly unaligned data. */
void *md_arena_memdup(md_arena *arena, const void *data, size_t len);

/*
 * Take ownership of a len-byte block that came from the standard allocator
 * (malloc, or md_buffer_release) and use it as the arena's newest chunk.
 *
 * The document text is by far the largest allocation a parse makes, and it
 * arrives already correctly sized from the normalizer or the stream reader.
 * Adopting it avoids a second full-size copy, which is the difference between
 * holding the input once and holding it twice on a large document. The
 * arena's md_arena_destroy() frees the block either way.
 *
 * `data` must be NUL-terminated at data[len] (md_buffer_release guarantees
 * it) and must not be in use by the caller afterwards. The cap is charged
 * exactly as an allocation of the same size would be.
 */
int md_arena_adopt(md_arena *arena, char *data, size_t len);

size_t md_arena_bytes_used(const md_arena *arena);

/*
 * Growable byte buffer backed by the standard allocator. Used to assemble
 * strings that are copied into an arena once complete; free with
 * md_buffer_free() (or md_buffer_release() to take ownership of the data).
 */
typedef struct md_buffer {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} md_buffer;

void md_buffer_init(md_buffer *buf);
int md_buffer_reserve(md_buffer *buf, size_t extra);
int md_buffer_append(md_buffer *buf, const char *data, size_t len);
int md_buffer_append_cstr(md_buffer *buf, const char *s);
int md_buffer_append_char(md_buffer *buf, char c);
char *md_buffer_release(md_buffer *buf); /* caller owns the result */
void md_buffer_free(md_buffer *buf);

#endif
