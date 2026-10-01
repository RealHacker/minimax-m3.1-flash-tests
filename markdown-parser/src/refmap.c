#include "refmap.h"

#include "status.h"

#include <stdlib.h>
#include <string.h>

#define MD_REFMAP_MIN_BUCKETS 16u

struct md_refmap {
    md_arena *arena;
    md_refdef **buckets;
    size_t bucket_count;
    size_t count;
};

static size_t hash_bytes(const char *s, size_t len)
{
    /* FNV-1a, 64-bit arithmetic truncated to size_t. */
    size_t h = (size_t)1469598103934665603ULL;

    for (; len > 0; len--, s++) {
        h ^= (size_t)(unsigned char)*s;
        h *= (size_t)1099511628211ULL;
    }
    return h;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static char fold(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/*
 * Normalize a label into a freshly allocated NUL-terminated key:
 * case folded, runs of whitespace collapsed to one space, ends trimmed.
 * Returns NULL on allocation failure.
 */
char *md_refmap_normalize_label(const char *label, size_t len, size_t *out_len)
{
    char *out;
    size_t i = 0;
    size_t n = 0;
    int pending_space = 0;
    int seen = 0;

    if (label == NULL) {
        md_set_error("invalid argument: md_refmap_normalize_label");
        return NULL;
    }
    if (len > MD_REF_LABEL_MAX) {
        md_set_error("reference label too long");
        return NULL;
    }
    out = (char *)malloc(len + 1u);
    if (out == NULL) {
        md_set_error("out of memory");
        return NULL;
    }
    for (i = 0; i < len; i++) {
        char c = label[i];

        if (is_space(c)) {
            /* Leading whitespace is dropped, inner runs collapse. */
            if (seen) {
                pending_space = 1;
            }
            continue;
        }
        if (pending_space) {
            out[n++] = ' ';
            pending_space = 0;
        }
        out[n++] = fold(c);
        seen = 1;
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

md_refmap *md_refmap_new(md_arena *arena)
{
    md_refmap *map;

    if (arena == NULL) {
        md_set_error("invalid argument: null arena");
        return NULL;
    }
    map = (md_refmap *)md_arena_calloc(arena, 1u, sizeof *map);
    if (map == NULL) {
        return NULL;
    }
    map->arena = arena;
    map->buckets = (md_refdef **)calloc(MD_REFMAP_MIN_BUCKETS,
                                        sizeof(md_refdef *));
    if (map->buckets == NULL) {
        md_set_error("out of memory");
        return NULL;
    }
    map->bucket_count = MD_REFMAP_MIN_BUCKETS;
    return map;
}

void md_refmap_destroy(md_refmap *map)
{
    size_t i;

    if (map == NULL) {
        return;
    }
    if (map->buckets != NULL) {
        for (i = 0; i < map->bucket_count; i++) {
            md_refdef *def = map->buckets[i];

            while (def != NULL) {
                md_refdef *next = def->next;

                free(def->label);
                free(def);
                def = next;
            }
        }
        free(map->buckets);
        map->buckets = NULL;
    }
    map->bucket_count = 0;
    map->count = 0;
}

static md_refdef *find_def(const md_refmap *map, const char *key, size_t key_len)
{
    md_refdef *def;

    if (map == NULL || map->buckets == NULL) {
        return NULL;
    }
    def = map->buckets[hash_bytes(key, key_len) % map->bucket_count];
    for (; def != NULL; def = def->next) {
        if (def->label_len == key_len &&
            memcmp(def->label, key, key_len) == 0) {
            return def;
        }
    }
    return NULL;
}

/* Grow to at least `wanted` buckets and rehash every definition. */
static int refmap_rehash(md_refmap *map, size_t wanted)
{
    md_refdef **fresh;
    size_t count = 0;
    size_t i;

    fresh = (md_refdef **)calloc(wanted, sizeof(md_refdef *));
    if (fresh == NULL) {
        md_set_error("out of memory");
        return -1;
    }
    for (i = 0; i < map->bucket_count; i++) {
        md_refdef *def = map->buckets[i];

        while (def != NULL) {
            md_refdef *next = def->next;
            size_t slot = hash_bytes(def->label, def->label_len) % wanted;

            def->next = fresh[slot];
            fresh[slot] = def;
            def = next;
            count++;
        }
    }
    free(map->buckets);
    map->buckets = fresh;
    map->bucket_count = wanted;
    map->count = count;
    return 0;
}

int md_refmap_define(md_refmap *map, const char *label, size_t label_len,
                     const char *dest, size_t dest_len,
                     const char *title, size_t title_len)
{
    char *key;
    size_t key_len = 0;
    md_refdef *def;
    size_t slot;
    const char *dest_copy;
    const char *title_copy = NULL;

    if (map == NULL || map->buckets == NULL) {
        md_set_error("invalid argument: md_refmap_define");
        return -1;
    }
    if (label_len == 0u) {
        return 0; /* an empty label is never a definition */
    }
    key = md_refmap_normalize_label(label, label_len, &key_len);
    if (key == NULL) {
        return -1;
    }
    if (key_len == 0u) {
        free(key);
        return 0; /* whitespace only */
    }
    if (find_def(map, key, key_len) != NULL) {
        free(key); /* first definition wins */
        return 0;
    }
    if (map->count + 1u > map->bucket_count - map->bucket_count / 4u) {
        if (map->bucket_count > ((size_t)-1) / 2u ||
            refmap_rehash(map, map->bucket_count * 2u) != 0) {
            free(key);
            return -1;
        }
    }
    dest_copy = md_arena_strndup(map->arena, dest != NULL ? dest : "", dest_len);
    if (dest_copy == NULL) {
        free(key);
        return -1;
    }
    if (title != NULL) {
        title_copy = md_arena_strndup(map->arena, title, title_len);
        if (title_copy == NULL) {
            free(key);
            return -1;
        }
    }
    def = (md_refdef *)calloc(1u, sizeof *def);
    if (def == NULL) {
        md_set_error("out of memory");
        free(key);
        return -1;
    }
    def->label = key;
    def->label_len = key_len;
    def->destination = dest_copy;
    def->title = title_copy;
    slot = hash_bytes(key, key_len) % map->bucket_count;
    def->next = map->buckets[slot];
    map->buckets[slot] = def;
    map->count++;
    return 0;
}

const md_refdef *md_refmap_lookup(const md_refmap *map, const char *label,
                                  size_t label_len)
{
    char *key;
    size_t key_len = 0;
    md_refdef *found;

    if (map == NULL || map->buckets == NULL || label == NULL ||
        label_len == 0u || label_len > MD_REF_LABEL_MAX) {
        return NULL;
    }
    key = md_refmap_normalize_label(label, label_len, &key_len);
    if (key == NULL) {
        /* A malformed label simply does not resolve. */
        md_clear_error();
        return NULL;
    }
    if (key_len == 0u) {
        free(key);
        return NULL;
    }
    found = find_def(map, key, key_len);
    free(key);
    return found;
}

size_t md_refmap_count(const md_refmap *map)
{
    return (map != NULL) ? map->count : 0u;
}
