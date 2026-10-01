/*
 * A standalone program built against the public library, libmd.a.
 *
 * It is deliberately a separate translation unit that includes nothing but
 * <md.h>. That is the point of it: if this file compiles and links against
 * the archive on its own, then a caller needs no private header, no source
 * file from src/, and no knowledge of how the library is split into modules
 * internally. Everything it uses is reachable from md.h.
 *
 * Build and run it with:
 *
 *     make example
 *     ./example            # reads the document below
 *     ./example FILE       # reads FILE ('-' means standard input)
 *
 * The library is used in the order its ownership rules require:
 *
 *   1. parse; the returned document owns an arena, and every node and every
 *      string the document exposes lives in it;
 *   2. read the result for as long as that arena is alive;
 *   3. md_document_destroy() releases the document and all of its text in
 *      one step.
 *
 * Nothing here is used after the destroy. A returned md_document and the
 * char pointers inside it are all borrowed from its arena, so keeping one
 * past md_document_destroy() is a use-after-free, and copying only part of a
 * result is the usual way to walk into that.
 */
#include "md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The document used when the caller gives none. It is here so that the
 * example has a known, checkable output, which is what makes it a test of
 * the library rather than a program that only demonstrates the API.
 */
static const char *const EXAMPLE_DOC =
    "# Markdown in ten minutes\n"
    "\n"
    "Some *emphasis* and a [link][ref].\n"
    "\n"
    "## Inline things\n"
    "\n"
    "### Code\n"
    "\n"
    "    not a heading\n"
    "\n"
    "## Inline things\n"
    "\n"
    "The second heading with the same text, which is what makes the\n"
    "anchors interesting.\n"
    "\n"
    "[ref]: https://example.com/\n";

/* Read a whole stream into the heap. Returns NULL on failure. */
static char *slurp(FILE *in, size_t *len)
{
    size_t cap = 8192;
    size_t used = 0;
    char *buf = malloc(cap);

    if (buf == NULL) {
        return NULL;
    }
    for (;;) {
        size_t got = fread(buf + used, 1, cap - used, in);

        used += got;
        if (got == 0) {
            break;
        }
        if (used == cap) {
            char *bigger = realloc(buf, cap * 2);

            if (bigger == NULL) {
                free(buf);
                return NULL;
            }
            buf = bigger;
            cap *= 2;
        }
    }
    if (ferror(in)) {
        free(buf);
        return NULL;
    }
    *len = used;
    return buf;
}

/*
 * Print the table of contents of `doc`.
 *
 * Both results are arena-owned and live exactly as long as the document: the
 * entry array through md_toc_build() and the JSON through md_toc_to_json().
 * There is nothing to free here, and nothing to copy out, and the one thing
 * not to do is keep `json` past md_document_destroy().
 */
static int print_toc(md_document *doc, int as_json)
{
    md_arena *arena = md_document_arena(doc);
    md_toc toc;
    size_t i;

    if (md_toc_build(arena, md_document_root(doc), &toc) != MD_OK) {
        fprintf(stderr, "example: toc: %s\n", md_error_message());
        return 1;
    }

    if (as_json) {
        /* The same bytes `md --toc` writes, from the same library call. */
        char *json = md_toc_to_json(arena, &toc);

        if (json == NULL) {
            fprintf(stderr, "example: toc json: %s\n", md_error_message());
            return 1;
        }
        printf("%s\n", json);
    } else {
        for (i = 0; i < toc.count; i++) {
            const md_toc_entry *e = &toc.entries[i];

            /* Two spaces per level below the first, so a level-1 heading
             * is flush left the way a table of contents is normally read. */
            printf("%*s%s -> #%s\n", (e->level - 1) * 2, "", e->text, e->anchor);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *src = EXAMPLE_DOC;
    char *owned = NULL;
    size_t len = strlen(EXAMPLE_DOC);
    md_document *doc;
    int status;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [FILE]\n", argv[0]);
        return 2;   /* a usage error, as in the CLI */
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-") == 0) {
            /* "-" means standard input, as it does in the CLI. */
            owned = slurp(stdin, &len);
            if (owned == NULL) {
                fprintf(stderr, "example: cannot read standard input\n");
                return 1;
            }
        } else {
            FILE *in = fopen(argv[1], "rb");

            if (in == NULL) {
                fprintf(stderr, "example: cannot open %s\n", argv[1]);
                return 1;   /* an I/O error, as in the CLI */
            }
            owned = slurp(in, &len);
            fclose(in);
            if (owned == NULL) {
                fprintf(stderr, "example: cannot read %s\n", argv[1]);
                return 1;
            }
        }
        src = owned;
    }

    /*
     * The parse owns its arena, and the normalized text it works on is a
     * copy the arena adopts, so the document never refers back to `src`.
     * That is why the input buffer can be released here and not merely
     * "eventually": everything below reads through `doc`, and `doc` does
     * not depend on `owned` outliving the call.
     */
    doc = md_parse_n(src, len);
    free(owned);
    if (doc == NULL) {
        /* One diagnostic, the same text the CLI prints on stderr. */
        fprintf(stderr, "example: %s\n", md_error_message());
        return 1;
    }

    status = print_toc(doc, argc == 1);

    /* One call releases the document and every string in it. */
    md_document_destroy(doc);
    return status;
}
