#ifndef MD_STATUS_H
#define MD_STATUS_H

/*
 * Shared status codes and the thread-local diagnostic message slot.
 *
 * Diagnostics never reach stdout: the CLI prints md_error_message() on stderr.
 */

#include <stddef.h>

typedef enum md_status {
    MD_OK = 0,
    MD_ERR_NOMEM,   /* out of memory */
    MD_ERR_INVAL,   /* invalid argument / precondition violated */
    MD_ERR_LIMIT,   /* input or nesting limit exceeded */
    MD_ERR_PARSE,   /* document could not be parsed */
    MD_ERR_IO       /* I/O failure */
} md_status;

#define MD_ERROR_MAX 256

const char *md_status_string(md_status status);

/*
 * The kind of a recorded failure (checkpoint C5). md_set_error() and
 * md_set_errorf() record MD_ERROR_GENERIC, so a caller that never opts into
 * the structured form keeps the C1-C4 behavior exactly: the message text is
 * the same string md_error_message() has always returned. The parsers that
 * can say *why* a document was rejected record a specific kind and, when the
 * failure has a place in the input, the 1-based line and column of it.
 */
typedef enum md_error_kind {
    MD_ERROR_NONE = 0,
    MD_ERROR_GENERIC,   /* md_set_error()/md_set_errorf() and plain failures */
    MD_ERROR_ARGUMENT,  /* invalid argument or violated precondition */
    MD_ERROR_IO,        /* a read or write failed */
    MD_ERROR_MEMORY,    /* the arena memory cap was reached */
    MD_ERROR_NESTING,   /* the nesting depth limit was reached */
    MD_ERROR_PARSE      /* the document could not be parsed */
} md_error_kind;

/*
 * The last recorded failure. line and col are 1-based; both are 0 when the
 * failure is not tied to a place in the input (an unreadable file, for
 * instance). The struct is a copy, not a pointer to anything temporary, so a
 * caller may hold it past the next md_set_error().
 */
typedef struct md_error {
    md_error_kind kind;
    size_t line;
    size_t col;
    char message[MD_ERROR_MAX];
} md_error;

/* Record a diagnostic. The last message wins; always non-NULL when queried. */
void md_set_error(const char *message);
void md_set_errorf(const char *fmt, ...);

/* As md_set_error(), but records the kind the caller knows about. */
void md_set_error_kind(md_error_kind kind, const char *message);
void md_set_error_kindf(md_error_kind kind, const char *fmt, ...);

/*
 * Attach a 1-based position to the recorded failure, without disturbing its
 * message. A position that is already set is kept, so the innermost place
 * that knew the answer wins. This is how a failure raised deep inside the
 * parser (an arena that ran out, say) ends up pointing at the line the
 * document was being read at.
 */
void md_error_set_position(size_t line, size_t col);

/* The recorded failure, never NULL. Reset with md_error_reset(). */
const md_error *md_last_error(void);
void md_error_reset(void);

const char *md_error_message(void);
void md_clear_error(void);

#endif
