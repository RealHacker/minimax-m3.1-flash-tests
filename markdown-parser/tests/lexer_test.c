/*
 * Internal lexer test (checkpoint C5).
 *
 * The public API test includes only md.h, on purpose: it is the proof that
 * a caller needs nothing else.  The normalizer is not part of the public
 * surface, so its contract is pinned here instead, where the local header
 * may be included.
 *
 * What matters: normalization expands tabs to four-column stops, collapses
 * CR and CRLF to LF, replaces NUL with U+FFFD, and preserves every other
 * byte.  It both grows and shrinks, which is why a memory cap has to be
 * charged against the normalized length rather than the raw one: a cap
 * between the two sizes has to accept the text.  md_normalize() and
 * md_lines_read() share that rule, so the two produce the same answer for
 * the same bytes under the same cap.
 */
#include "arena.h"
#include "lexer.h"
#include "status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(int condition, const char *what)
{
    checks++;
    if (condition) {
        (void)printf("ok   %s\n", what);
    } else {
        (void)printf("FAIL %s\n", what);
        failures++;
    }
}

/* Normalize `len` bytes through md_normalize() and report the outcome. */
static int normalize_with_cap(size_t cap, const char *src, size_t len,
                              char **text, size_t *text_len)
{
    md_arena *arena = md_arena_create_limit(cap);
    int rc;

    *text = NULL;
    *text_len = 0u;
    if (arena == NULL) {
        md_set_error("out of memory");
        return -1;
    }
    rc = md_normalize(arena, src, len, text, text_len);
    md_arena_destroy(arena);
    return rc;
}

int main(void)
{
    /* --- the basics, on an uncapped arena ---
     *
     * A tab expands to the next four-column stop, counting from column 0, so
     * "a\tb" is "a" plus three spaces and not four. */
    {
        md_arena *arena = md_arena_create();
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(arena, "a\tb", 3u, &text, &text_len) == 0,
               "md_normalize accepts a tab");
        expect(text_len == 5u, "a tab at column 1 stops at column 4");
        expect(text != NULL && memcmp(text, "a   b", 5u) == 0,
               "the expansion lands where the tab was");
        expect(text != NULL, "md_normalize returns a pointer, not a length");
        md_arena_destroy(arena);
    }

    /* A tab expands to the next stop rather than to four spaces: two already
     * there plus two of expansion. */
    {
        md_arena *arena = md_arena_create();
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(arena, "ab\tc", 4u, &text, &text_len) == 0,
               "md_normalize accepts a tab after two columns");
        expect(text_len == 5u, "a tab at column 2 becomes two spaces");
        expect(text != NULL && memcmp(text, "ab  c", 5u) == 0,
               "the expansion stops at the next four-column stop");
        md_arena_destroy(arena);
    }

    /* A tab past the last stop of its line, and a tab at the very start. */
    {
        md_arena *arena = md_arena_create();
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(arena, "\ta", 2u, &text, &text_len) == 0 &&
               text_len == 5u && text != NULL && memcmp(text, "    a", 5u) == 0,
               "a tab at column 0 becomes four spaces");
        expect(md_normalize(arena, "abc\ta", 5u, &text, &text_len) == 0 &&
               text_len == 5u && text != NULL && memcmp(text, "abc a", 5u) == 0,
               "a tab at column 3 becomes one space");
        md_arena_destroy(arena);
    }

    /* --- CR, CRLF and LF all become one LF --- */
    {
        static const struct {
            const char *in;
            size_t len;
            const char *want;
        } cases[] = {
            { "a\rb",     3u, "a\nb"   },
            { "a\r\nb",   4u, "a\nb"   },
            { "a\nb",     3u, "a\nb"   },
            { "a\r\r\nb", 5u, "a\n\nb" },
            { "a\n\rb",   4u, "a\n\nb" },
            { "\r\n\r\n", 4u, "\n\n"   },
            { "a\r\n",    3u, "a\n"    },
            { "\r",       1u, "\n"     }
        };
        size_t i;

        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            md_arena *arena = md_arena_create();
            char *text = NULL;
            size_t text_len = 0u;
            char what[96];

            (void)snprintf(what, sizeof what, "case %lu normalizes",
                           (unsigned long)i);
            expect(md_normalize(arena, cases[i].in, cases[i].len,
                                &text, &text_len) == 0, what);
            (void)snprintf(what, sizeof what,
                           "case %lu leaves no CR and the expected LFs",
                           (unsigned long)i);
            expect(text != NULL && text_len == strlen(cases[i].want) &&
                   memcmp(text, cases[i].want, text_len) == 0, what);
            md_arena_destroy(arena);
        }
    }

    /* --- NUL becomes U+FFFD, other UTF-8 is untouched --- */
    {
        md_arena *arena = md_arena_create();
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(arena, "a\0b", 3u, &text, &text_len) == 0,
               "md_normalize accepts an embedded NUL");
        expect(text_len == 5u, "a NUL becomes the three bytes of U+FFFD");
        expect(text != NULL && memcmp(text, "a\xef\xbf\xbd" "b", 5u) == 0,
               "the replacement is U+FFFD");        md_arena_destroy(arena);
    }
    {
        md_arena *arena = md_arena_create();
        static const char utf8[] = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
        char *text = NULL;
        size_t text_len = 0u;
        size_t raw = sizeof utf8 - 1u;

        expect(md_normalize(arena, utf8, raw, &text, &text_len) == 0,
               "md_normalize accepts multi-byte UTF-8");
        expect(text_len == raw, "multi-byte UTF-8 keeps its length");
        expect(text != NULL && memcmp(text, utf8, raw) == 0,
               "every non-ASCII byte survives unchanged");
        md_arena_destroy(arena);
    }

    /* --- the cap is charged against the normalized text ---
     *
     * Ten CRLF pairs are twenty raw bytes and ten normalized ones.  A cap of
     * sixteen sits between them: charging the cap against the raw length
     * would refuse text that fits, and would make this function disagree
     * with md_lines_read(), which has always charged the normalized one. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        char *text = NULL;
        size_t text_len = 0u;

        expect(raw == 60u, "the CRLF input is the length it claims to be");
        expect(normalize_with_cap(56u, crlf, raw, &text, &text_len) == 0,
               "a cap between the raw and the normalized size accepts the text");
        expect(text_len == 50u, "ten CRLF pairs normalize to ten LFs");
    }

    /* A cap the normalized text cannot fit is refused, as a memory error,
     * and the message names the limit rather than the raw length. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        char *text = NULL;
        size_t text_len = 0u;

        expect(normalize_with_cap(48u, crlf, raw, &text, &text_len) != 0,
               "a cap below the normalized size refuses the text");
        expect(md_last_error()->kind == MD_ERROR_MEMORY,
               "the refusal is a memory error");
        expect(strstr(md_last_error()->message, "memory cap exceeded") != NULL,
               "the refusal names the memory cap");
    }

    /* A cap of zero means "no cap", and the two paths agree with it. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        char *text = NULL;
        size_t text_len = 0u;
        FILE *in = tmpfile();
        md_arena *arena = md_arena_create();
        md_lines lines;

        expect(normalize_with_cap(0u, crlf, raw, &text, &text_len) == 0,
               "a zero cap means uncapped");
        if (in != NULL) {
            expect(fwrite(crlf, 1u, raw, in) == raw, "the input is written");
            rewind(in);
            expect(md_lines_read(arena, in, 0u, &lines) == 0,
                   "md_lines_read accepts the same bytes");
            expect(lines.count == 10u, "the stream reader sees ten lines");
            expect(arena != NULL && text != NULL &&
                   lines.text_len == text_len &&
                   memcmp(lines.text, text, text_len) == 0,
                   "the buffered and the streamed normalizer agree exactly");
            fclose(in);
        } else {
            expect(0, "a temporary file is created for the stream");
        }
        md_arena_destroy(arena);
    }

    /* The identity C5 actually promises: md_lines_read() gives a stream the
     * same lines md_lines_build() gives a buffer, byte for byte, and the
     * same verdict under the same cap.
     *
     * Comparing them rather than comparing md_lines_read() with
     * md_normalize() is deliberate.  The two do different amounts of work:
     * md_lines_read() also builds the span array, ten records of sixteen
     * bytes here, so it needs a larger arena than the bare normalizer and
     * would be refused where the normalizer succeeds.  That is not a
     * disagreement about the cap rule, it is a larger bill.  A build and a
     * read of the same bytes are charged the same and must agree exactly.
     *
     * The raw input bound is left at 0, so the only ceiling in play is the
     * arena's -- which is how md_parse_stream() calls it.  The separate raw
     * bound md_parse_stream_in() takes is a caller's choice to bound bytes
     * read, and is deliberately a different rule. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        size_t cap;

        for (cap = 8u; cap <= 512u; cap += 8u) {
            md_arena *ab = md_arena_create_limit(cap);
            md_arena *as = md_arena_create_limit(cap);
            md_lines buffered;
            md_lines streamed;
            FILE *in = tmpfile();
            int built;
            int read_it = -1;
            char what[96];

            built = md_lines_build(ab, crlf, raw, &buffered);
            if (in != NULL) {
                (void)fwrite(crlf, 1u, raw, in);
                rewind(in);
                read_it = md_lines_read(as, in, 0u, &streamed);
                fclose(in);
            }
            (void)snprintf(what, sizeof what,
                           "cap %lu: build and read return the same verdict",
                           (unsigned long)cap);
            expect(built == read_it, what);
            if (built == 0 && read_it == 0) {
                (void)snprintf(what, sizeof what,
                               "cap %lu: build and read produce the same text",
                               (unsigned long)cap);
                expect(buffered.text_len == streamed.text_len &&
                       memcmp(buffered.text, streamed.text,
                              buffered.text_len) == 0, what);
                (void)snprintf(what, sizeof what,
                               "cap %lu: build and read produce the same lines",
                               (unsigned long)cap);
                expect(buffered.count == streamed.count, what);
            }
            md_arena_destroy(ab);
            md_arena_destroy(as);
        }
    }

    /* A CRLF that straddles a chunk boundary.
     *
     * md_lines_read() reads 64 KiB at a time, so a CR on the last byte of a
     * chunk has to be held until the next chunk decides whether an LF
     * follows.  Building the file so that a CR lands on byte 65535 and its
     * LF on byte 65536 exercises exactly that, and the answer has to be the
     * same as for the buffer.  The four positions cover a CR alone, a CRLF
     * pair, a lone LF, and ordinary text, each on a chunk boundary. */
    {
        static const struct {
            size_t cr_at;   /* byte offset of the CR, from 65532 */
            const char *tail;
        } cases[] = {
            { 65532u, "x\r\ntail\n" },   /* CR one line in */
            { 65533u, "x\rYtail\n" },   /* CR with no LF after it */
            { 65534u, "x\n\rtail\n" },   /* LF then CR, both on a boundary */
            { 65535u, "x\r\ntail\n" }    /* CR on the very last byte */
        };
        size_t i;

        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            size_t fill = cases[i].cr_at;
            size_t tail_len = strlen(cases[i].tail);
            size_t raw = fill + tail_len;
            char *doc = (char *)malloc(raw);
            md_arena *ab = md_arena_create();
            md_arena *as = md_arena_create();
            md_lines buffered;
            md_lines streamed;
            FILE *in;
            char what[96];

            if (doc == NULL || ab == NULL || as == NULL) {
                expect(0, "the chunk-boundary case allocates");
                free(doc);
                md_arena_destroy(ab);
                md_arena_destroy(as);
                continue;
            }
            memset(doc, 'a', fill);
            memcpy(doc + fill, cases[i].tail, tail_len);
            in = tmpfile();
            expect(in != NULL, "a temporary file is created for the stream");
            if (in != NULL) {
                (void)fwrite(doc, 1u, raw, in);
                rewind(in);
            }
            expect(md_lines_build(ab, doc, raw, &buffered) == 0,
                   "the buffer builder accepts the case");
            expect(in != NULL && md_lines_read(as, in, 0u, &streamed) == 0,
                   "the stream reader accepts the case");
            (void)snprintf(what, sizeof what,
                           "case %lu: a CR on a chunk boundary reads the same",
                           (unsigned long)i);
            expect(buffered.text_len == streamed.text_len &&
                   memcmp(buffered.text, streamed.text,
                          buffered.text_len) == 0 &&
                   buffered.count == streamed.count, what);
            expect(buffered.text != NULL &&
                   memchr(buffered.text, '\r', buffered.text_len) == NULL,
                   "no CR survives the boundary");
            if (in != NULL) {
                fclose(in);
            }
            free(doc);
            md_arena_destroy(ab);
            md_arena_destroy(as);
        }
    }

    /* A cap between the raw length and the normalized length is where the
     * two rules disagree, and md_normalize() has to take the normalized
     * one.  The stream reader cannot show it: a ten-line document pays for
     * ten span records whatever the text costs, so under a small cap both
     * sides are refused, just for different reasons.  The equality loop
     * below is what covers the stream side. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        md_arena *arena = md_arena_create_limit(56u);
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(arena, crlf, raw, &text, &text_len) == 0,
               "a 60-byte input normalizes inside a 56-byte cap");
        expect(text_len == 50u, "and keeps the 50 bytes it became");
        md_arena_destroy(arena);
    }

    /* Invalid arguments are refused rather than dereferenced. */
    {
        char *text = NULL;
        size_t text_len = 0u;

        expect(md_normalize(NULL, "a", 1u, &text, &text_len) != 0,
               "md_normalize with no arena is refused");
        expect(md_normalize(md_arena_create(), "a", 1u, NULL, &text_len) != 0,
               "md_normalize with no output pointer is refused");
        expect(md_normalize(md_arena_create(), "a", 1u, &text, NULL) != 0,
               "md_normalize with no output length is refused");
        expect(md_normalize(md_arena_create(), NULL, 1u, &text, &text_len) != 0,
               "md_normalize with a NULL source and a length is refused");
    }

    /* An empty input is empty output, not a failure. */
    {
        md_arena *arena = md_arena_create();
        char *text = NULL;
        size_t text_len = 1u;

        expect(md_normalize(arena, "", 0u, &text, &text_len) == 0,
               "an empty input normalizes");
        expect(text_len == 0u, "an empty input normalizes to nothing");
        md_arena_destroy(arena);
    }

    (void)printf("\n%s: %d ok, %d failure(s)\n",
                 failures ? "FAILED" : "passed", checks, failures);
    return failures != 0;
}
