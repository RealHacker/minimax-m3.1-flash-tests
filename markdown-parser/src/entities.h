#ifndef MD_ENTITIES_H
#define MD_ENTITIES_H

/*
 * HTML entity references for the inline parser.
 *
 * Both numeric forms are supported: `&#35;` (decimal) and `&#x23;` (hex).
 * Named references come from the table below, which covers the entities that
 * appear in ordinary prose; an unknown name is left alone and stays literal
 * text, as does a reference without its closing semicolon.
 *
 * Code points that cannot be encoded (zero, above U+10FFFF, and the surrogate
 * range) decode to U+FFFD.
 */

#include <stddef.h>

/* Longest supported entity name, including '&' and ';'. */
#define MD_ENTITY_NAME_MAX 34u

/*
 * Try to decode the entity reference starting at src[pos] (which must be '&').
 * On success writes UTF-8 into out (at most 4 bytes), stores its length in
 * *out_len and returns the number of source bytes consumed including '&' and
 * ';'. Returns 0 when the text at pos is not a valid reference, in which case
 * out and *out_len are untouched.
 */
size_t md_entity_decode(const char *src, size_t len, size_t pos, char *out,
                        size_t out_cap, size_t *out_len);

#endif
