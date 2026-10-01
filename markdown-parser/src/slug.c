/*
 * The shared anchor slug algorithm. See slug.h for the rule and for why it
 * is byte-wise.
 */
#include "slug.h"

int md_slug_write(md_buffer *buf, const char *s, size_t len, int lowercase)
{
    size_t i;
    int last_dash = 0;

    if (buf == NULL || (s == NULL && len > 0u)) {
        md_set_error("invalid argument: null slug source");
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char keep;

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-') {
            keep = (char)c;
        } else if (c >= 'A' && c <= 'Z') {
            keep = (char)(lowercase ? c - 'A' + 'a' : c);
        } else {
            /* A separator. Only the first of a run is written, so "a - b"
             * and "a b" land on the same anchor. */
            if (!last_dash) {
                if (md_buffer_append(buf, "-", 1u) != 0) {
                    return -1;
                }
                last_dash = 1;
            }
            continue;
        }
        /* A single byte, so it is appended with an explicit length. */
        if (md_buffer_append(buf, &keep, 1u) != 0) {
            return -1;
        }
        last_dash = 0;
    }
    return 0;
}
