#include "status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__)
#define MD_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define MD_PRINTF(a, b)
#endif

/*
 * The diagnostic is thread-local and every public error path goes through
 * it, so a document parsed on one thread never sees another's message. The
 * C1-C4 slot is the message field of the same struct: md_error_message()
 * returns exactly the bytes it always did, so existing callers and tests are
 * unaffected by the structured fields added alongside.
 */
static _Thread_local md_error md_error_slot = { MD_ERROR_NONE, 0u, 0u, "" };

const char *md_status_string(md_status status)
{
    switch (status) {
    case MD_OK:         return "ok";
    case MD_ERR_NOMEM:  return "out of memory";
    case MD_ERR_INVAL:  return "invalid argument";
    case MD_ERR_LIMIT:  return "limit exceeded";
    case MD_ERR_PARSE:  return "parse error";
    case MD_ERR_IO:     return "i/o error";
    }
    return "unknown error";
}

/* Fill the slot; a NULL message is recorded as an empty one, as before. */
static void md_error_store(md_error_kind kind, const char *message, va_list ap,
                           int have_args)
{
    md_error_slot.kind = kind;
    md_error_slot.line = 0u;
    md_error_slot.col = 0u;
    if (message == NULL) {
        md_error_slot.message[0] = '\0';
        return;
    }
    if (have_args) {
        /* vsnprintf always terminates inside the buffer. */
        (void)vsnprintf(md_error_slot.message, sizeof md_error_slot.message,
                        message, ap);
    } else {
        (void)snprintf(md_error_slot.message, sizeof md_error_slot.message, "%s",
                       message);
    }
}

void md_set_error(const char *message)
{
    md_error_store(MD_ERROR_GENERIC, message, NULL, 0);
}

void md_set_errorf(const char *fmt, ...) MD_PRINTF(1, 2);

void md_set_errorf(const char *fmt, ...)
{
    va_list ap;

    if (fmt == NULL) {
        md_error_store(MD_ERROR_GENERIC, NULL, NULL, 0);
        return;
    }
    va_start(ap, fmt);
    md_error_store(MD_ERROR_GENERIC, fmt, ap, 1);
    va_end(ap);
}

void md_set_error_kind(md_error_kind kind, const char *message)
{
    md_error_store(kind, message, NULL, 0);
}

void md_set_error_kindf(md_error_kind kind, const char *fmt, ...) MD_PRINTF(2, 3);

void md_set_error_kindf(md_error_kind kind, const char *fmt, ...)
{
    va_list ap;

    if (fmt == NULL) {
        md_error_store(kind, NULL, NULL, 0);
        return;
    }
    va_start(ap, fmt);
    md_error_store(kind, fmt, ap, 1);
    va_end(ap);
}

void md_error_set_position(size_t line, size_t col)
{
    /*
     * First writer wins: the innermost frame that knew where the failure was
     * records it, and a caller further out that only knows "somewhere in
     * this document" cannot overwrite it with a worse guess.
     */
    if (md_error_slot.line != 0u) {
        return;
    }
    if (line == 0u) {
        return;
    }
    md_error_slot.line = line;
    md_error_slot.col = col;
}

const md_error *md_last_error(void)
{
    return &md_error_slot;
}

const char *md_error_message(void)
{
    return md_error_slot.message;
}

void md_clear_error(void)
{
    md_error_slot.kind = MD_ERROR_NONE;
    md_error_slot.line = 0u;
    md_error_slot.col = 0u;
    md_error_slot.message[0] = '\0';
}

void md_error_reset(void)
{
    md_clear_error();
}
