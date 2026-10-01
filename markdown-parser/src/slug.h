#ifndef MD_SLUG_H
#define MD_SLUG_H

/*
 * The anchor slug algorithm, in one place.
 *
 * Two features need an id derived from human text: the footnote ids in the
 * HTML renderer (C4) and the heading anchors in the table of contents (C6).
 * They are the same algorithm with one difference -- the table of contents
 * lowercases, the footnote ids do not -- so the algorithm lives here rather
 * than being written twice, where a later fix to one copy would leave the
 * other quietly disagreeing.
 */

#include "arena.h"
#include "status.h"

#include <stddef.h>

/*
 * Append the slug of `s` to `buf`: no quotes, no separators, just the slug.
 * Letters, digits, '_' and '-' are kept; every other byte is a separator, and
 * a run of separators collapses to a single '-' so that labels differing only
 * in punctuation produce the same anchor. Nothing is trimmed -- the footnote
 * id is built from a label that may begin or end with punctuation, while the
 * table of contents trims, and the two want different things.
 *
 * `lowercase` maps A-Z to a-z. The table of contents asks for that and the
 * footnote ids do not, which is why it is a parameter rather than a fixed
 * rule.
 *
 * The mapping is byte-wise and so the result is ASCII-only and independent of
 * the locale: a byte of a multi-byte UTF-8 sequence counts as a separator.
 * That is what makes the same document slug identically on every machine,
 * which a locale-sensitive case mapping could not promise.
 *
 * Returns 0, or -1 on invalid arguments or an allocation failure, recording a
 * diagnostic in the latter case.
 */
int md_slug_write(md_buffer *buf, const char *s, size_t len, int lowercase);

#endif
