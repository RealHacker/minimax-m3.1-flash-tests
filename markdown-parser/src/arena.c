#include "arena.h"

#include "status.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* First chunk size; later chunks grow so that a large document does not
 * degenerate into thousands of tiny allocations. */
#define MD_ARENA_CHUNK_MIN ((size_t)16u * 1024u)
#define MD_ARENA_CHUNK_MAX ((size_t)1024u * 1024u)

/* Refuse absurd single allocations instead of failing confusingly later. */
#define MD_ARENA_MAX_ALLOC ((size_t)512u * 1024u * 1024u)

/* Alignment good enough for any object the AST holds (max_align_t is 16 on
 * every target we care about; the union makes that explicit). */
typedef union md_align {
    max_align_t align_;
    long double ld;
    void *p;
} md_align;

#define MD_ALIGN (sizeof(md_align))

struct md_arena_chunk {
    struct md_arena_chunk *next;
    unsigned char *data;
    size_t cap;
    size_t used;
};

/*
 * Would handing out `need` more bytes stay inside the cap? A request larger
 * than the whole cap is refused outright and the comparison is written as a
 * subtraction, so a total near SIZE_MAX cannot wrap past the check.
 */
static int md_arena_fits(const md_arena *arena, size_t need)
{
    if (arena->cap_bytes == 0u) {
        return 1;
    }
    if (need > arena->cap_bytes) {
        return 0;
    }
    return arena->total <= arena->cap_bytes - need;
}

static int md_round_up(size_t value, size_t multiple, size_t *out)
{
    size_t slack = value % multiple;

    if (slack == 0) {
        *out = value;
        return 0;
    }
    if (value > SIZE_MAX - (multiple - slack)) {
        return -1;
    }
    *out = value + (multiple - slack);
    return 0;
}

static md_arena_chunk *md_chunk_new(size_t hint)
{
    md_arena_chunk *chunk = (md_arena_chunk *)malloc(sizeof *chunk);

    if (chunk == NULL) {
        return NULL;
    }
    chunk->data = (unsigned char *)malloc(hint);
    if (chunk->data == NULL) {
        free(chunk);
        return NULL;
    }
    chunk->next = NULL;
    chunk->cap = hint;
    chunk->used = 0;
    return chunk;
}

md_arena *md_arena_create(void)
{
    return md_arena_create_limit(0u);
}

md_arena *md_arena_create_limit(size_t max_bytes)
{
    md_arena *arena = (md_arena *)malloc(sizeof *arena);

    if (arena == NULL) {
        md_set_error("out of memory creating arena");
        return NULL;
    }
    arena->head = NULL;
    arena->total = 0;
    arena->cap_bytes = max_bytes;
    return arena;
}

size_t md_arena_limit(const md_arena *arena)
{
    if (arena == NULL) {
        return 0u;
    }
    return arena->cap_bytes;
}

void md_arena_destroy(md_arena *arena)
{
    md_arena_chunk *chunk;

    if (arena == NULL) {
        return;
    }
    chunk = arena->head;
    while (chunk != NULL) {
        md_arena_chunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    free(arena);
}

void *md_arena_alloc(md_arena *arena, size_t size)
{
    size_t need;
    md_arena_chunk *chunk;

    if (arena == NULL || size == 0 || size > MD_ARENA_MAX_ALLOC) {
        md_set_error("invalid arena allocation size");
        return NULL;
    }
    if (md_round_up(size, MD_ALIGN, &need) != 0) {
        md_set_error("arena allocation size overflow");
        return NULL;
    }
    if (!md_arena_fits(arena, need)) {
        md_set_error_kindf(MD_ERROR_MEMORY,
                           "memory cap exceeded (limit %zu bytes, %zu already used)",
                           arena->cap_bytes, arena->total);
        return NULL;
    }

    if (arena->head != NULL && arena->head->cap - arena->head->used >= need) {
        void *ptr = arena->head->data + arena->head->used;
        arena->head->used += need;
        arena->total += need;
        return ptr;
    }

    {
        size_t hint = need > MD_ARENA_CHUNK_MIN ? need : MD_ARENA_CHUNK_MIN;
        size_t grown;

        if (arena->head != NULL) {
            if (md_round_up(arena->head->cap * 2u, MD_ARENA_CHUNK_MIN, &grown) == 0 &&
                grown <= MD_ARENA_CHUNK_MAX) {
                hint = grown;
            }
        }
        if (hint < need) {
            hint = need;
        }
        chunk = md_chunk_new(hint);
    }
    if (chunk == NULL) {
        md_set_error("out of memory growing arena");
        return NULL;
    }
    chunk->next = arena->head;
    arena->head = chunk;
    {
        void *ptr = chunk->data + chunk->used;
        chunk->used += need;
        arena->total += need;
        return ptr;
    }
}

void *md_arena_calloc(md_arena *arena, size_t count, size_t size)
{
    void *ptr;
    size_t total;

    if (count == 0 || size == 0) {
        md_set_error("invalid arena allocation size");
        return NULL;
    }
    if (count > SIZE_MAX / size) {
        md_set_error("arena allocation size overflow");
        return NULL;
    }
    total = count * size;
    ptr = md_arena_alloc(arena, total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

int md_arena_adopt(md_arena *arena, char *data, size_t len)
{
    md_arena_chunk *chunk;

    if (arena == NULL || data == NULL) {
        md_set_error("invalid argument: md_arena_adopt");
        return -1;
    }
    if (!md_arena_fits(arena, len)) {
        md_set_error_kindf(MD_ERROR_MEMORY,
                           "memory cap exceeded (limit %zu bytes, %zu already used)",
                           arena->cap_bytes, arena->total);
        return -1;
    }
    chunk = (md_arena_chunk *)malloc(sizeof *chunk);
    if (chunk == NULL) {
        md_set_error("out of memory adopting buffer");
        return -1;
    }
    chunk->data = (unsigned char *)data;
    chunk->cap = len;
    chunk->used = len;
    chunk->next = arena->head;
    arena->head = chunk;
    arena->total += len;
    return 0;
}

void *md_arena_memdup(md_arena *arena, const void *data, size_t len){
    void *ptr;

    if (len == 0) {
        return md_arena_alloc(arena, 1);
    }
    if (data == NULL) {
        md_set_error("invalid argument: null source");
        return NULL;
    }
    ptr = md_arena_alloc(arena, len);
    if (ptr == NULL) {
        return NULL;
    }
    memcpy(ptr, data, len);
    return ptr;
}

char *md_arena_strndup(md_arena *arena, const char *s, size_t len)
{
    char *out;

    if (s == NULL) {
        md_set_error("invalid argument: null string");
        return NULL;
    }
    out = (char *)md_arena_alloc(arena, len + 1u);
    if (out == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

char *md_arena_strdup(md_arena *arena, const char *s)
{
    if (s == NULL) {
        md_set_error("invalid argument: null string");
        return NULL;
    }
    return md_arena_strndup(arena, s, strlen(s));
}

size_t md_arena_bytes_used(const md_arena *arena)
{
    if (arena == NULL) {
        return 0;
    }
    return arena->total;
}

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */
/* ------------------------------------------------------------------ */

void md_buffer_init(md_buffer *buf)
{
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->failed = 0;
}

int md_buffer_reserve(md_buffer *buf, size_t extra)
{
    size_t need;
    size_t cap;
    char *data;

    if (buf == NULL) {
        return -1;
    }
    if (buf->failed) {
        return -1;
    }
    if (extra == 0) {
        return 0;
    }
    if (buf->len > SIZE_MAX - extra - 1u) {
        buf->failed = 1;
        md_set_error("buffer size overflow");
        return -1;
    }
    need = buf->len + extra + 1u; /* room for a terminating NUL */
    if (need <= buf->cap) {
        return 0;
    }
    cap = buf->cap != 0 ? buf->cap : 64u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) {
            cap = need;
            break;
        }
        cap *= 2u;
    }
    data = (char *)realloc(buf->data, cap);
    if (data == NULL) {
        buf->failed = 1;
        md_set_error("out of memory growing buffer");
        return -1;
    }
    buf->data = data;
    buf->cap = cap;
    return 0;
}

int md_buffer_append(md_buffer *buf, const char *data, size_t len)
{
    if (buf == NULL || (data == NULL && len > 0)) {
        return -1;
    }
    if (len == 0) {
        return buf->failed ? -1 : 0;
    }
    if (md_buffer_reserve(buf, len) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

int md_buffer_append_cstr(md_buffer *buf, const char *s)
{
    if (s == NULL) {
        return -1;
    }
    return md_buffer_append(buf, s, strlen(s));
}

int md_buffer_append_char(md_buffer *buf, char c)
{
    return md_buffer_append(buf, &c, 1u);
}

char *md_buffer_release(md_buffer *buf)
{
    char *data;

    if (buf == NULL) {
        return NULL;
    }
    if (buf->failed) {
        md_buffer_free(buf);
        return NULL;
    }
    if (buf->data == NULL) {
        data = (char *)malloc(1u);
        if (data == NULL) {
            md_set_error("out of memory");
            return NULL;
        }
        data[0] = '\0';
        md_buffer_init(buf);
        return data;
    }
    data = buf->data;
    md_buffer_init(buf);
    return data;
}

void md_buffer_free(md_buffer *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    md_buffer_init(buf);
}
