/*
 * md - command line front end.
 *
 *   md --ast  FILE   print the block AST as JSON on stdout
 *   md --html FILE   print the document as HTML on stdout
 *   md --text FILE   print the document as plain text on stdout
 *   md --toc  FILE   print the table of contents as JSON on stdout
 *
 *   md md2ast  FILE  the same as --ast
 *   md md2html FILE  the same as --html
 *
 * Any mode accepts --ext CONFIG.json, which turns on the extensions the
 * configuration names; without it no extension is enabled and the output is
 * exactly what the previous checkpoint produced.
 *
 * Any mode also accepts --stream, --max-nesting N and --max-bytes N, which
 * select the checkpoint C5 streaming entry point and its two ceilings. A
 * document that reaches either ceiling is rejected with a structured error
 * and exit status 1.
 *
 * Exit status: 0 success, 1 I/O or parse error, 2 usage error. stdout carries
 * exactly one JSON document: the rendered document on success, or the
 * structured error document on a limit failure. That way a consumer reading
 * stdout always gets valid JSON and can tell success from failure by the exit
 * status alone, without having to merge a second stream.
 *
 * A copy of the error document is also written to stderr, so a consumer that
 * logs a human-readable failure stream keeps seeing one. A usage error has no
 * machine-readable form and is reported to stderr only, leaving stdout empty.
 *
 * The document is followed by a single newline, which the platform's C
 * runtime may terminate with CRLF. The JSON payload itself is byte-exact: all
 * control characters inside strings are escaped, so no raw CR or LF can ever
 * reach stdout from the document.
 */

#include "md.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MD_EXIT_OK 0
#define MD_EXIT_ERROR 1
#define MD_EXIT_USAGE 2

#define MD_READ_CHUNK 65536u
/* Refuse absurd inputs rather than exhausting memory; also bounds read time. */
#define MD_MAX_FILE_BYTES MD_MAX_INPUT_BYTES

/* Output modes, in the order they are listed by --help. */
typedef enum md_mode {
    MD_MODE_AST = 0,
    MD_MODE_HTML,
    MD_MODE_TEXT,
    MD_MODE_TOC,
    MD_MODE_COUNT
} md_mode;

static const char *mode_flag(md_mode mode)
{
    switch (mode) {
    case MD_MODE_HTML:
        return "--html";
    case MD_MODE_TEXT:
        return "--text";
    case MD_MODE_TOC:
        return "--toc";
    case MD_MODE_COUNT:
    case MD_MODE_AST:
        break;
    }
    return "--ast";
}

static const char *mode_help(md_mode mode)
{
    switch (mode) {
    case MD_MODE_HTML:
        return "print the document of FILE as HTML";
    case MD_MODE_TEXT:
        return "print the document of FILE as plain text";
    case MD_MODE_TOC:
        return "print the headings of FILE as JSON (md --toc)";
    case MD_MODE_COUNT:
    case MD_MODE_AST:
        break;
    }
    return "print the block AST of FILE as JSON";
}

static void usage(FILE *out)
{
    md_mode mode;

    (void)fputs(
        "usage: md (--ast | --html | --text | --toc) [--ext CONFIG.json]\n"
        "           [--stream] [--max-nesting N] [--max-bytes N] FILE\n"
        "       md (md2ast | md2html) [same options] FILE\n"
        "\n",
        out);
    /* One line per mode, generated from the same table the parser uses, and
     * one shared column so every description lines up. */
    for (mode = MD_MODE_AST; mode < MD_MODE_COUNT; mode++) {
        (void)fprintf(out, "  %-13s %s ('-' reads stdin)\n", mode_flag(mode),
                       mode_help(mode));
    }
    (void)fputs(
        "  --ext FILE    enable the extensions CONFIG.json names ('-' reads stdin)\n"
        "  --stream      read the input as a stream instead of buffering it\n"
        "  --max-nesting N  reject block nesting deeper than N (default 64)\n"
        "  --max-bytes N    give up after N bytes of parser memory (0 = no cap)\n"
        "  -h, --help    show this message\n"
        "  -V, --version show the version\n"
        "\n"
        "md2ast and md2html are subcommand spellings of --ast and --html;\n"
        "they take the same FILE and the same options and produce the same\n"
        "bytes. There is no md2text and no md2toc.\n"
        "\n"
        "Exactly one mode and one FILE are required.\n"
        "The built-in extensions are: tables, strikethrough, tasklist,\n"
        "footnotes. None is enabled unless --ext names it.\n"
        "Reaching a nesting or memory limit is reported as\n"
        "{\"error\":{\"line\":N,\"col\":M,\"message\":\"...\"}} on stdout,\n"
        "and copied to stderr. Exit status: 0 ok, 1 I/O or parse error,\n"
        "2 usage error.\n",
        out);
}

static int read_stream(FILE *in, const char *name, md_buffer *buf)
{
    char chunk[MD_READ_CHUNK];

    for (;;) {
        size_t got = fread(chunk, 1u, sizeof chunk, in);

        if (got > 0u) {
            if (buf->len + got > MD_MAX_FILE_BYTES) {
                (void)fprintf(stderr, "md: %s: input too large (limit %zu bytes)\n",
                              name, MD_MAX_FILE_BYTES);
                return -1;
            }
            if (md_buffer_append(buf, chunk, got) != 0) {
                (void)fprintf(stderr, "md: out of memory reading %s\n", name);
                return -1;
            }
        }
        if (got < sizeof chunk) {
            if (ferror(in)) {
                (void)fprintf(stderr, "md: %s: read error: %s\n", name,
                              strerror(errno));
                return -1;
            }
            if (feof(in)) {
                break;
            }
        }
    }
    return 0;
}

static int read_input(const char *path, md_buffer *buf)
{
    int rc;

    if (strcmp(path, "-") == 0) {
        return read_stream(stdin, "<stdin>", buf);
    }
    {
        FILE *in = fopen(path, "rb");

        if (in == NULL) {
            (void)fprintf(stderr, "md: %s: %s\n", path, strerror(errno));
            return -1;
        }
        rc = read_stream(in, path, buf);
        if (fclose(in) != 0 && rc == 0) {
            (void)fprintf(stderr, "md: %s: close error: %s\n", path, strerror(errno));
            rc = -1;
        }
    }
    return rc;
}

/*
 * Write one rendered document to stdout. The renderers already end a
 * non-empty document with a newline, so a newline is only added when the
 * result is empty; an empty document therefore produces no output at all.
 */
static int emit_document(const char *path, const char *out)
{
    size_t len = strlen(out);
    int rc = MD_EXIT_OK;

    if (len > 0u && fwrite(out, 1u, len, stdout) != len) {
        (void)fprintf(stderr, "md: %s: write error: %s\n", path, strerror(errno));
        rc = MD_EXIT_ERROR;
    }
    if (fflush(stdout) != 0 && rc == MD_EXIT_OK) {
        (void)fprintf(stderr, "md: %s: write error: %s\n", path, strerror(errno));
        rc = MD_EXIT_ERROR;
    }
    return rc;
}

static int emit(const char *path, md_document *doc, md_mode mode)
{
    md_arena *arena = md_document_arena(doc);
    char *out;

    switch (mode) {
    case MD_MODE_HTML:
        out = md_render_html(arena, md_document_root(doc));
        break;
    case MD_MODE_TEXT:
        out = md_render_text(arena, md_document_root(doc));
        break;
    case MD_MODE_TOC: {
        /* The table of contents is built into the document's own arena, so
         * the entries and both of their strings live exactly as long as the
         * document does and there is nothing extra to release. */
        md_toc toc;

        if (md_toc_build(arena, md_document_root(doc), &toc) != MD_OK) {
            (void)fprintf(stderr, "md: %s: %s\n", path,
                          md_error_message()[0] != '\0' ? md_error_message()
                                                       : "cannot read headings");
            return MD_EXIT_ERROR;
        }
        out = md_toc_to_json(arena, &toc);
        break;
    }
    case MD_MODE_AST:
    default:
        out = md_ast_to_json(arena, md_document_root(doc));
        break;
    }
    if (out == NULL) {
        (void)fprintf(stderr, "md: %s: %s\n", path,
                      md_error_message()[0] != '\0' ? md_error_message()
                                                   : "failed to render document");
        return MD_EXIT_ERROR;
    }
    if (mode == MD_MODE_AST || mode == MD_MODE_TOC) {
        int rc = MD_EXIT_OK;

        /* The JSON payload is byte-exact and always gets one final newline. */
        if (fputs(out, stdout) == EOF || fputc('\n', stdout) == EOF) {
            (void)fprintf(stderr, "md: %s: write error: %s\n", path, strerror(errno));
            rc = MD_EXIT_ERROR;
        }
        if (fflush(stdout) != 0 && rc == MD_EXIT_OK) {
            (void)fprintf(stderr, "md: %s: write error: %s\n", path, strerror(errno));
            rc = MD_EXIT_ERROR;
        }
        return rc;
    }
    return emit_document(path, out);
}

/* Match "--flag" or "--flag=VALUE" and copy the value out. */
static int match_flag(const char *arg, const char *flag, const char **value)
{
    size_t len = strlen(flag);

    if (strncmp(arg, flag, len) != 0) {
        return 0;
    }
    if (arg[len] == '\0') {
        *value = NULL;
        return 1;
    }
    if (arg[len] != '=' || arg[len + 1u] == '\0') {
        return -1; /* present but malformed */
    }
    *value = arg + len + 1u;
    return 1;
}

/*
 * Does `arg` look like a flag rather than a value? A bare "-" is the
 * stdin convention and is a value, so it never counts as a flag.
 */
static int looks_like_flag(const char *arg)
{
    return arg != NULL && arg[0] == '-' && arg[1] != '\0';
}

/*
 * The arguments that select a mode: the four mode flags, plus the two
 * subcommand spellings of the two modes that have one. A subcommand is not a
 * separate argument position, it is matched here by the same rule and with
 * the same errors as a flag, so `md md2html FILE` and `md --html FILE`
 * cannot drift apart and a misspelling is caught the same way either.
 *
 * There is deliberately no md2text and no md2toc: the subcommands are an
 * alternative spelling of two modes, not a second naming scheme that has to
 * be kept in step with all four.
 */
typedef struct md_mode_alias {
    const char *spelling;
    md_mode mode;
} md_mode_alias;

static const md_mode_alias mode_aliases[] = {
    { "--ast", MD_MODE_AST },
    { "--html", MD_MODE_HTML },
    { "--text", MD_MODE_TEXT },
    { "--toc", MD_MODE_TOC },
    { "md2ast", MD_MODE_AST },
    { "md2html", MD_MODE_HTML }
};

/*
 * Does `arg` select a mode? Returns 1 and reports the mode, the spelling the
 * caller actually wrote (so a duplicate is reported in their words) and the
 * attached value if there was one. `*match` is 1 for a well-formed argument
 * and -1 for a malformed one, exactly as match_flag() defines it. Returns 0
 * when the argument is not a mode at all.
 */
static int select_mode(const char *arg, md_mode *mode, const char **spelling,
                       int *match, const char **value)
{
    size_t i;

    for (i = 0; i < sizeof mode_aliases / sizeof mode_aliases[0]; i++) {
        const char *found = NULL;
        int m = match_flag(arg, mode_aliases[i].spelling, &found);

        if (m == 0) {
            continue;
        }
        *mode = mode_aliases[i].mode;
        *spelling = mode_aliases[i].spelling;
        *match = m;
        *value = found;
        return 1;
    }
    return 0;
}

/* A non-negative decimal count, or -1. Leading blanks and a sign are refused
 * so a typo is a usage error rather than a silently different limit. */
static long parse_count(const char *text, int *out)
{
    unsigned long value = 0;
    size_t i;

    *out = 0;
    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        if (value > (0x7fffffffUL - (unsigned long)(text[i] - '0')) / 10UL) {
            return -1; /* would not fit a long, and would be absurd anyway */
        }
        value = value * 10UL + (unsigned long)(text[i] - '0');
    }
    *out = (int)value;
    return 0;
}

/*
 * The modifiers that are pulled out of the argument list before the main
 * pass runs, so the parser's own flags and the FILE position are unaffected
 * by where they are written.
 *
 * Four entries are recorded at most: --stream takes no value, and each of
 * --ext, --max-nesting and --max-bytes takes one. A repeated flag is a usage
 * error rather than a silent last-one-wins.
 */
typedef struct md_modifiers {
    const char *ext_path;
    int stream;
    int max_nesting;
    int max_bytes;
    int skip_flag[4];
    int skip_value[4];
} md_modifiers;

static int scan_modifiers(int argc, char **argv, md_modifiers *mods)
{
    int i;

    mods->ext_path = NULL;
    mods->stream = 0;
    mods->max_nesting = 0;
    mods->max_bytes = 0;
    for (i = 0; i < 4; i++) {
        mods->skip_flag[i] = -1;
        mods->skip_value[i] = -1;
    }
    for (i = 1; i < argc; i++) {
        const char *value = NULL;
        int slot = -1;
        int match;

        if (strcmp(argv[i], "--stream") == 0) {
            if (mods->stream) {
                (void)fprintf(stderr, "md: --stream given more than once\n");
                return -1;
            }
            mods->stream = 1;
            mods->skip_flag[0] = i;
            slot = 0;
        } else {
            if ((match = match_flag(argv[i], "--ext", &value)) != 0) {
                if (match < 0) {
                    (void)fprintf(stderr, "md: --ext given without a value\n");
                    return -1;
                }
                if (mods->ext_path != NULL) {
                    (void)fprintf(stderr, "md: --ext given more than once\n");
                    return -1;
                }
                slot = 1;
            } else if ((match = match_flag(argv[i], "--max-nesting", &value)) != 0) {
                if (match < 0) {
                    (void)fprintf(stderr, "md: --max-nesting given without a value\n");
                    return -1;
                }
                if (mods->skip_flag[2] >= 0) {
                    (void)fprintf(stderr, "md: --max-nesting given more than once\n");
                    return -1;
                }
                slot = 2;
            } else if ((match = match_flag(argv[i], "--max-bytes", &value)) != 0) {
                if (match < 0) {
                    (void)fprintf(stderr, "md: --max-bytes given without a value\n");
                    return -1;
                }
                if (mods->skip_flag[3] >= 0) {
                    (void)fprintf(stderr, "md: --max-bytes given more than once\n");
                    return -1;
                }
                slot = 3;
            }
        }
        if (slot < 0) {
            continue;
        }
        mods->skip_flag[slot] = i;
        if (slot == 0) {
            continue; /* --stream carries no value */
        }
        if (value == NULL) {
            if (i + 1 >= argc || looks_like_flag(argv[i + 1])) {
                (void)fprintf(stderr, "md: %s requires a value\n", argv[i]);
                return -1;
            }
            value = argv[i + 1];
            mods->skip_value[slot] = i + 1;
        }
        if (slot == 1) {
            mods->ext_path = value;
        } else if (slot == 2) {
            if (parse_count(value, &mods->max_nesting) != 0) {
                (void)fprintf(stderr, "md: --max-nesting wants a count, got %s\n",
                              value);
                return -1;
            }
        } else {
            if (parse_count(value, &mods->max_bytes) != 0) {
                (void)fprintf(stderr, "md: --max-bytes wants a byte count, got %s\n",
                              value);
                return -1;
            }
        }
    }
    return 0;
}

/* Has this argv entry already been claimed by a modifier? */
static int is_skipped(int index, const md_modifiers *mods)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (index == mods->skip_flag[i] || index == mods->skip_value[i]) {
            return 1;
        }
    }
    return 0;
}

/*
 * Take the FILE that follows a mode flag, stepping over the entries the
 * modifier pass already claimed. Without the step "md --ast --ext CFG FILE"
 * would take the configuration as the document.
 */
static int take_file(int argc, char **argv, int *i, const md_modifiers *mods,
                     const char **value)
{
    int j = *i + 1;

    while (j < argc && is_skipped(j, mods)) {
        j++;
    }
    if (j >= argc) {
        return -1;
    }
    *value = argv[j];
    *i = j;
    return 0;
}

/*
 * Report a failed parse. A limit the parser reached -- nesting, or the arena
 * memory cap -- is written as the structured error document, because that is
 * a machine-readable answer about *where* the document was rejected and the
 * caller asked for limits in a machine-readable way.
 *
 * The document goes to stdout, so a caller reading stdout gets the same kind
 * of JSON it would have got on success and can tell the two apart by exit
 * status. A copy goes to stderr as well, for a caller that keeps a separate
 * human-readable failure stream. Nothing has been written to stdout yet on
 * this path: the renderer runs only after the parse succeeds, so the document
 * here is the only thing stdout will ever carry.
 */
static int report_parse_failure(const char *path)
{
    const md_error *err = md_last_error();
    md_arena *tmp;
    char *json;

    if (err->kind != MD_ERROR_NESTING && err->kind != MD_ERROR_MEMORY) {
        (void)fprintf(stderr, "md: %s: %s\n", path,
                      err->message[0] != '\0' ? err->message : "parse failed");
        return MD_EXIT_ERROR;
    }
    tmp = md_arena_create();
    if (tmp == NULL) {
        (void)fprintf(stderr, "md: %s: %s\n", path, err->message);
        return MD_EXIT_ERROR;
    }
    json = md_error_to_json(tmp, err);
    if (json == NULL) {
        (void)fprintf(stderr, "md: %s: %s\n", path, err->message);
        md_arena_destroy(tmp);
        return MD_EXIT_ERROR;
    }
    (void)fprintf(stderr, "%s\n", json);
    if (fputs(json, stdout) == EOF || fputc('\n', stdout) == EOF) {
        (void)fprintf(stderr, "md: %s: write error: %s\n", path,
                      strerror(errno));
    }
    md_arena_destroy(tmp);
    return MD_EXIT_ERROR;
}

/* Open FILE for reading, or stdin for the "-" convention. NULL on failure. */
static FILE *open_input(const char *path, int *close_it)
{
    FILE *in;

    if (strcmp(path, "-") == 0) {
        *close_it = 0;
        return stdin;
    }
    in = fopen(path, "rb");
    if (in == NULL) {
        (void)fprintf(stderr, "md: %s: %s\n", path, strerror(errno));
        *close_it = 0;
        return NULL;
    }
    *close_it = 1;
    return in;
}

int main(int argc, char **argv)
{
    md_buffer input;
    md_arena *arena = NULL;
    md_document *doc;
    const char *path = NULL;
    md_modifiers mods;
    md_options options;
    md_mode mode = MD_MODE_AST;
    int have_mode = 0;
    int status = MD_EXIT_OK;
    int i;

    md_options_init(&options);
    md_buffer_init(&input);
    if (scan_modifiers(argc, argv, &mods) != 0) {
        usage(stderr);
        md_buffer_free(&input);
        return MD_EXIT_USAGE;
    }
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        const char *flag = NULL;
        int match;
        md_mode picked;

        if (is_skipped(i, &mods)) {
            continue; /* already taken by the modifier pass */
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            md_buffer_free(&input);
            return MD_EXIT_OK;
        }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            (void)fputs("md 0.1.0 (checkpoint C6: subcommands and contents)\n",
                        stdout);
            md_buffer_free(&input);
            return MD_EXIT_OK;
        }

        if (!select_mode(arg, &picked, &flag, &match, &value)) {
            (void)fprintf(stderr, "md: unknown argument: %s\n", arg);
            usage(stderr);
            md_buffer_free(&input);
            return MD_EXIT_USAGE;
        }
        if (match < 0) {
            (void)fprintf(stderr, "md: %s given without a value\n", flag);
            usage(stderr);
            md_buffer_free(&input);
            return MD_EXIT_USAGE;
        }
        if (have_mode) {
            (void)fprintf(stderr, "md: %s given more than once\n", flag);
            usage(stderr);
            md_buffer_free(&input);
            return MD_EXIT_USAGE;
        }
        if (value == NULL) {
            if (take_file(argc, argv, &i, &mods, &value) != 0) {
                (void)fprintf(stderr, "md: %s requires a FILE argument\n", flag);
                usage(stderr);
                md_buffer_free(&input);
                return MD_EXIT_USAGE;
            }
        }
        mode = picked;
        have_mode = 1;
        path = value;
    }

    if (!have_mode) {
        (void)fputs("md: missing mode; expected one of --ast, --html, --text\n",
                    stderr);
        usage(stderr);
        md_buffer_free(&input);
        return MD_EXIT_USAGE;
    }
    if (path == NULL) {
        (void)fprintf(stderr, "md: %s requires a FILE argument\n", mode_flag(mode));
        usage(stderr);
        md_buffer_free(&input);
        return MD_EXIT_USAGE;
    }

    /*
     * The configuration is read into its own arena, which is released before
     * the document is built: only the bitmask it produced outlives it. It is
     * loaded before the limits, because a configuration file sets the option
     * struct wholesale and would otherwise discard the limits named here.
     */
    if (mods.ext_path != NULL) {
        arena = md_arena_create();
        if (arena == NULL) {
            md_buffer_free(&input);
            (void)fprintf(stderr, "md: out of memory\n");
            return MD_EXIT_ERROR;
        }
        if (md_options_from_file(arena, mods.ext_path, &options) != 0) {
            md_arena_destroy(arena);
            md_buffer_free(&input);
            (void)fprintf(stderr, "md: %s: %s\n",
                          mods.ext_path,
                          md_error_message()[0] != '\0'
                              ? md_error_message()
                              : "cannot read extension configuration");
            return MD_EXIT_ERROR;
        }
        md_arena_destroy(arena);
        arena = NULL;
    }

    /*
     * The limits are applied last, for the same reason. A 0 means "leave the
     * library default alone" rather than "use no limit", so naming neither
     * flag keeps the C1-C4 behaviour exactly.
     */
    if (mods.max_nesting > 0) {
        options.limits.max_nesting = (size_t)mods.max_nesting;
    }
    if (mods.max_bytes > 0) {
        options.limits.max_bytes = (size_t)mods.max_bytes;
    }

    /*
     * The streaming path hands the file straight to the parser, so the
     * buffered copy is never made and a large document is read once, in
     * bounded chunks, rather than being slurped whole and then copied again
     * by the normalizer. Without --stream the input is read whole, which is
     * what every previous checkpoint did.
     */
    if (mods.stream) {
        FILE *in;
        int close_it = 0;

        in = open_input(path, &close_it);
        if (in == NULL) {
            md_buffer_free(&input);
            return MD_EXIT_ERROR;
        }
        doc = md_parse_stream(in, &options);
        if (close_it) {
            (void)fclose(in);
        }
        md_buffer_free(&input);
        if (doc == NULL) {
            return report_parse_failure(path);
        }
    } else {
        if (read_input(path, &input) != 0) {
            md_buffer_free(&input);
            return MD_EXIT_ERROR;
        }
        doc = md_parse_opts_n(&options, input.data != NULL ? input.data : "",
                              input.len);
        md_buffer_free(&input);
        if (doc == NULL) {
            return report_parse_failure(path);
        }
    }

    status = emit(path, doc, mode);
    md_document_destroy(doc);
    return status;
}
