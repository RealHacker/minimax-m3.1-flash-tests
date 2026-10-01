/*
 * Table of contents (checkpoint C6).
 *
 * Walks a parsed document in document order, collects the headings, and
 * renders them as compact JSON. Two properties are load-bearing and are
 * asserted in the tests: the same document always produces the same bytes,
 * and every anchor in one document is unique.
 *
 * The entries and the strings they point at are allocated in the caller's
 * arena, so there is no per-entry free: md_arena_destroy() releases the lot.
 */
#include "toc.h"
#include "slug.h"

#include <stdio.h>
#include <string.h>

/* The anchor given to a heading whose text anchors to nothing, e.g. "###". */
#define MD_TOC_FALLBACK_ANCHOR "section"

/* ------------------------------------------------------------------ */
/* A string -> size_t map, used to deduplicate anchors                */
/* ------------------------------------------------------------------ */

/*
 * Anchor uniqueness is what stops two identical headings from sharing one
 * link target, and checking it with a linear scan over the anchors already
 * handed out would make a document of N repeated headings cost O(N^2).
 * A document can easily carry 100k headings -- a generated API reference
 * can -- so that is a way to make the parser take unbounded time, which is
 * exactly what the C5 limits exist to prevent. This open-addressed table
 * turns each question into a hash lookup.
 *
 * The table never influences the output: anchors are handed out in document
 * order, and the first free suffix is chosen by scanning from 1 (or from
 * where the last duplicate of this base left off), so the bytes depend on
 * the input alone and not on the hash.
 */
typedef struct md_strtab {
    const char **keys; /* arena-owned, NULL in an empty slot */
    size_t *vals;       /* parallel to keys */
    size_t cap;         /* a power of two, 0 before the first insert */
    size_t count;
} md_strtab;

/* FNV-1a, computed in size_t. The width does not matter: it only decides
 * which slot a key lands in, never the order the keys were added. */
static size_t strtab_hash(const char *s)
{
    size_t h = (size_t)2166136261u;

    while (*s != '\0') {
        h ^= (size_t)(unsigned char)*s++;
        h *= (size_t)16777619u;
    }
    return h;
}

static md_status strtab_grow(md_arena *arena, md_strtab *tab, size_t cap)
{
    const char **keys;
    size_t *vals;
    size_t i;

    keys = md_arena_calloc(arena, cap, sizeof *keys);
    if (keys == NULL) {
        return MD_ERR_NOMEM;
    }
    vals = md_arena_calloc(arena, cap, sizeof *vals);
    if (vals == NULL) {
        return MD_ERR_NOMEM;
    }
    for (i = 0; i < tab->cap; i++) {
        size_t slot;

        if (tab->keys[i] == NULL) {
            continue;
        }
        slot = strtab_hash(tab->keys[i]) & (cap - 1u);
        while (keys[slot] != NULL) {
            slot = (slot + 1u) & (cap - 1u);
        }
        keys[slot] = tab->keys[i];
        vals[slot] = tab->vals[i];
    }
    tab->keys = keys;
    tab->vals = vals;
    tab->cap = cap;
    return MD_OK;
}

/* Look up `key`. Returns 1 and sets *val when present, 0 when absent. */
static int strtab_get(const md_strtab *tab, const char *key, size_t *val)
{
    size_t slot;

    if (tab->cap == 0u) {
        return 0;
    }
    slot = strtab_hash(key) & (tab->cap - 1u);
    while (tab->keys[slot] != NULL) {
        if (strcmp(tab->keys[slot], key) == 0) {
            if (val != NULL) {
                *val = tab->vals[slot];
            }
            return 1;
        }
        slot = (slot + 1u) & (tab->cap - 1u);
    }
    return 0;
}

/*
 * Insert or update `key`. `key` is NOT copied: the table keeps the pointer,
 * so the key must be an arena string that outlives the table. Passing a
 * stack buffer here does not corrupt the table immediately -- it stores a
 * pointer to a dead frame, and the entry is simply never found again, so a
 * duplicate anchor slips through as if it were unique. Every caller below
 * passes a string the arena owns.
 */
static md_status strtab_put(md_arena *arena, md_strtab *tab, const char *key,
                            size_t val)
{
    size_t slot;
    md_status status;

    if (tab->cap == 0u) {
        status = strtab_grow(arena, tab, 16u);
        if (status != MD_OK) {
            return status;
        }
    } else if ((tab->count + 1u) * 2u >= tab->cap) {
        status = strtab_grow(arena, tab, tab->cap * 2u);
        if (status != MD_OK) {
            return status;
        }
    }
    slot = strtab_hash(key) & (tab->cap - 1u);
    while (tab->keys[slot] != NULL) {
        if (strcmp(tab->keys[slot], key) == 0) {
            tab->vals[slot] = val;
            return MD_OK;
        }
        slot = (slot + 1u) & (tab->cap - 1u);
    }
    tab->keys[slot] = key;
    tab->vals[slot] = val;
    tab->count++;
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Heading text                                                        */
/* ------------------------------------------------------------------ */

/*
 * The plain text of a heading: what a reader would see, with the inline
 * markup removed. A text or code node contributes its value verbatim, a line
 * break contributes one space so two words on separate lines do not run
 * together, and anything else contributes the text of its children -- which
 * is what makes a heading that is entirely emphasis still produce its words.
 *
 * Recursion here is bounded by the inline nesting limit the C5 parser
 * enforces, and by one frame for the block-only C1 parse, which leaves a
 * heading as a single text child.
 */
static int append_text(md_buffer *buf, const md_node *node)
{
    size_t i;

    switch (node->type) {
    case MD_NODE_TEXT:
    case MD_NODE_CODE:
        if (node->value == NULL || node->value[0] == '\0') {
            return 0;
        }
        return md_buffer_append_cstr(buf, node->value);
    case MD_NODE_SOFTBREAK:
    case MD_NODE_HARDBREAK:
        return md_buffer_append_char(buf, ' ');
    default:
        for (i = 0; i < node->child_count; i++) {
            if (append_text(buf, node->children[i]) != 0) {
                return -1;
            }
        }
        return 0;
    }
}

/*
 * The anchor for one heading. The shared anchor rule lowercases and collapses
 * separators; on top of that this trims the leading and trailing '-', which
 * a heading like "Title --" would otherwise turn into "-title--", and falls
 * back to a fixed name when nothing is left, so that "###" and "!?" still get
 * an anchor instead of an empty one that no link could target.
 */
static char *build_anchor(md_arena *arena, const char *text)
{
    md_buffer raw;
    const char *base;
    size_t start = 0;
    size_t end;
    char *anchor;

    md_buffer_init(&raw);
    if (md_slug_write(&raw, text, strlen(text), 1) != 0) {
        md_buffer_free(&raw);
        return NULL;
    }
    /* md_buffer_append() keeps a NUL at data[len], so the anchor is a C string. */
    base = raw.data != NULL ? raw.data : "";
    end = raw.len;
    while (start < end && base[start] == '-') {
        start++;
    }
    while (end > start && base[end - 1u] == '-') {
        end--;
    }
    if (end == start) {
        md_buffer_free(&raw);
        return md_arena_strdup(arena, MD_TOC_FALLBACK_ANCHOR);
    }
    anchor = md_arena_strndup(arena, base + start, end - start);
    md_buffer_free(&raw);
    return anchor;
}

/* "base-7": the first free suffix of `base`, or 0 if `base` itself is free. */
static md_status unique_anchor(md_arena *arena, const char *base,
                             md_strtab *used, md_strtab *counter, char **out)
{
    size_t n;
    md_status status;

    *out = NULL;
    if (!strtab_get(used, base, NULL)) {
        status = strtab_put(arena, used, base, 1u);
        if (status != MD_OK) {
            return status;
        }
        status = strtab_put(arena, counter, base, 1u);
        if (status != MD_OK) {
            return status;
        }
        *out = (char *)base;
        return MD_OK;
    }
    /*
     * Resume from where this base's last duplicate stopped, so a run of
     * identical headings costs one lookup each. A literal heading that
     * occupies a candidate ("## Title 1" wants "title-1") still forces a
     * skip, and the skip is cheap because it is only as long as the number of
     * genuinely occupied slots.
     */
    if (!strtab_get(counter, base, &n)) {
        n = 1u;
    }
    for (;;) {
        char candidate[64];
        int written = snprintf(candidate, sizeof candidate, "%s-%zu", base, n);

        if (written < 0 || (size_t)written >= sizeof candidate) {
            /* A base long enough to overflow the buffer would need a heap
             * string; rather than grow one, refuse it as a diagnostic. A
             * heading long enough to get here is far past any real use. */
            md_set_error("heading anchor is too long to disambiguate");
            return MD_ERR_INVAL;
        }
        if (!strtab_get(used, candidate, NULL)) {
            /* Copy the candidate into the arena once and use that copy both
             * as the table key and as the entry's anchor. The table stores the
             * pointer, so handing it the stack buffer this was built in would
             * leave a key pointing at a dead frame. */
            char *owned = md_arena_strdup(arena, candidate);

            if (owned == NULL) {
                md_set_error("out of memory naming a heading anchor");
                return MD_ERR_NOMEM;
            }
            status = strtab_put(arena, used, owned, 1u);
            if (status != MD_OK) {
                return status;
            }
            status = strtab_put(arena, counter, base, n + 1u);
            if (status != MD_OK) {
                return status;
            }
            *out = owned;
            return MD_OK;
        }
        n++;
    }
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static size_t count_headings(const md_node *node)
{
    size_t i;
    size_t total = 0;

    for (i = 0; i < node->child_count; i++) {
        const md_node *child = node->children[i];

        if (child->type == MD_NODE_HEADING) {
            total++;
        }
        total += count_headings(child);
    }
    return total;
}

static md_status collect(md_arena *arena, const md_node *node, size_t *index,
                         md_toc_entry *out, md_strtab *used, md_strtab *counter)
{
    size_t i;

    for (i = 0; i < node->child_count; i++) {
        const md_node *child = node->children[i];

        if (child->type == MD_NODE_HEADING) {
            md_buffer text;
            char *heading_text;
            char *base;
            char *anchor;
            md_status status;

            md_buffer_init(&text);
            if (append_text(&text, child) != 0) {
                md_buffer_free(&text);
                return MD_ERR_NOMEM;
            }
            /* Copy into the arena before the buffer goes away: the entry has
             * to outlive this loop, and the buffer is released here. */
            heading_text = md_arena_strndup(arena,
                                            text.data != NULL ? text.data : "",
                                            text.data != NULL ? text.len : 0u);
            md_buffer_free(&text);
            if (heading_text == NULL) {
                md_set_error("out of memory collecting a heading");
                return MD_ERR_NOMEM;
            }
            base = build_anchor(arena, heading_text);
            if (base == NULL) {
                md_set_error("out of memory building a heading anchor");
                return MD_ERR_NOMEM;
            }
            status = unique_anchor(arena, base, used, counter, &anchor);
            if (status != MD_OK) {
                return status;
            }
            out[*index].level = child->level;
            out[*index].text = heading_text;
            out[*index].anchor = anchor;
            (*index)++;
        }
        {
            md_status status = collect(arena, child, index, out, used, counter);

            if (status != MD_OK) {
                return status;
            }
        }
    }
    return MD_OK;
}

md_status md_toc_build(md_arena *arena, const md_node *root, md_toc *out)
{
    md_strtab used = { NULL, NULL, 0u, 0u };
    md_strtab counter = { NULL, NULL, 0u, 0u };
    md_toc_entry *entries;
    size_t total;
    size_t index = 0;
    md_status status;

    if (arena == NULL || root == NULL || out == NULL) {
        md_set_error("invalid argument: null argument to md_toc_build");
        return MD_ERR_INVAL;
    }
    out->entries = NULL;
    out->count = 0;
    total = count_headings(root);
    if (total == 0u) {
        return MD_OK;
    }
    entries = md_arena_calloc(arena, total, sizeof *entries);
    if (entries == NULL) {
        return MD_ERR_NOMEM;
    }
    status = collect(arena, root, &index, entries, &used, &counter);
    if (status != MD_OK) {
        return status;
    }
    out->entries = entries;
    out->count = index;
    return MD_OK;
}

char *md_toc_to_json(md_arena *arena, const md_toc *toc)
{
    md_buffer buf;
    size_t i;
    char *json;

    if (arena == NULL || toc == NULL) {
        md_set_error("invalid argument: null argument to md_toc_to_json");
        return NULL;
    }
    md_buffer_init(&buf);
    if (md_buffer_append_char(&buf, '[') != 0) {
        md_buffer_free(&buf);
        return NULL;
    }
    for (i = 0; i < toc->count; i++) {
        char level[16];
        int written;

        if (i > 0u && md_buffer_append_char(&buf, ',') != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
        written = snprintf(level, sizeof level, "%d", toc->entries[i].level);
        if (written < 0 || (size_t)written >= sizeof level) {
            md_set_error("failed to format heading level");
            md_buffer_free(&buf);
            return NULL;
        }
        if (md_buffer_append_cstr(&buf, "{\"level\":") != 0 ||
            md_buffer_append_cstr(&buf, level) != 0 ||
            md_buffer_append_cstr(&buf, ",\"text\":") != 0 ||
            md_json_write_string(&buf, toc->entries[i].text,
                                 strlen(toc->entries[i].text)) != 0 ||
            md_buffer_append_cstr(&buf, ",\"anchor\":") != 0 ||
            md_json_write_string(&buf, toc->entries[i].anchor,
                                 strlen(toc->entries[i].anchor)) != 0 ||
            md_buffer_append_char(&buf, '}') != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
    }
    if (md_buffer_append_char(&buf, ']') != 0) {
        md_buffer_free(&buf);
        return NULL;
    }
    json = md_arena_strndup(arena, buf.data != NULL ? buf.data : "",
                            buf.data != NULL ? buf.len : 0u);
    md_buffer_free(&buf);
    if (json == NULL) {
        md_set_error("out of memory serializing the table of contents");
    }
    return json;
}
