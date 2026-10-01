#ifndef MD_REFMAP_H
#define MD_REFMAP_H

/*
 * Link reference definitions.
 *
 * A definition such as
 *
 *     [ref]: https://example.test "Title"
 *
 * registers a label that later reference links and images resolve against.
 * Labels are matched after normalization: ASCII case is folded, surrounding
 * whitespace is removed and internal whitespace runs collapse to one space,
 * so "[Ref]", "[ ref ]" and "[r  e f]" all denote the same definition.
 *
 * The map is created per parse. Its buckets and label keys come from the
 * standard allocator and are released by md_refmap_destroy(); destinations,
 * titles and the map header itself live in the parse arena.
 */

#include "arena.h"

#include <stddef.h>

/* A label longer than this is never a valid reference. */
#define MD_REF_LABEL_MAX 999u

typedef struct md_refdef md_refdef;

struct md_refdef {
    char *label;            /* normalized label, owned by the map */
    size_t label_len;
    const char *destination;/* arena-owned, "" when the source was empty */
    const char *title;      /* arena-owned, NULL when the source had none */
    md_refdef *next;        /* bucket chain */
};

typedef struct md_refmap md_refmap;

md_refmap *md_refmap_new(md_arena *arena);
void md_refmap_destroy(md_refmap *map);

/*
 * Register a definition. label/dest/title are copied (labels into map-owned
 * memory, destination and title into the arena). Returns 0 on success,
 * -1 on allocation failure. Defining the same label twice keeps the first
 * definition, as CommonMark requires.
 */
int md_refmap_define(md_refmap *map, const char *label, size_t label_len,
                     const char *dest, size_t dest_len,
                     const char *title, size_t title_len);

/* Look up a label; returns NULL when undefined. */
const md_refdef *md_refmap_lookup(const md_refmap *map, const char *label,
                                  size_t label_len);

size_t md_refmap_count(const md_refmap *map);

/*
 * Normalize a label the way the map matches them: case folded, runs of
 * whitespace collapsed to one space, ends trimmed. Returns freshly
 * allocated NUL-terminated memory (released with free()) and writes its
 * length to *out_len, or NULL on failure. Extensions reuse it so that their
 * own labels resolve exactly like reference labels.
 */
char *md_refmap_normalize_label(const char *label, size_t len,
                                size_t *out_len);

#endif
