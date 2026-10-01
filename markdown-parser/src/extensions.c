/*
 * Syntax extensions module (checkpoint C4).
 *
 * This module owns the extension bitmask, the parse options, the runtime
 * registry, the JSON configuration reader, and the syntax primitives that the
 * block and inline parsers share. It builds no nodes itself: blocks.c and
 * inlines.c own the node construction and call in here for the recognition
 * rules, so the module boundary stays the same as in C1-C3.
 */
#include "extensions.h"

#include "md.h"
#include "refmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Configuration files are read with the same bound as the CLI. */
#define MD_EXT_CONFIG_MAX 1048576u

/* Upper bound on registered extensions, so a loop over them stays bounded. */
#define MD_EXT_MAX_REGISTERED 64u

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    md_ext_flags bit;
} md_ext_name;

static const md_ext_name md_ext_names[] = {
    { "tables", MD_EXT_TABLES },
    { "strikethrough", MD_EXT_STRIKETHROUGH },
    { "tasklist", MD_EXT_TASK_LIST },
    { "footnotes", MD_EXT_FOOTNOTES }
};

#define MD_EXT_NAME_COUNT (sizeof md_ext_names / sizeof md_ext_names[0])

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int name_equal(const char *a, size_t alen, const char *b)
{
    size_t i;

    if (strlen(b) != alen) {
        return 0;
    }
    for (i = 0; i < alen; i++) {
        if (ascii_lower((unsigned char)a[i]) != b[i]) {
            return 0;
        }
    }
    return 1;
}

int md_ext_flag_by_name(const char *name, size_t len, md_ext_flags *flags)
{
    size_t i;

    if (name == NULL || flags == NULL) {
        return -1;
    }
    for (i = 0; i < MD_EXT_NAME_COUNT; i++) {
        if (name_equal(name, len, md_ext_names[i].name)) {
            *flags = md_ext_names[i].bit;
            return 0;
        }
    }
    /* Spellings that read naturally in a configuration file. */
    if (name_equal(name, len, "table") || name_equal(name, len, "tables")) {
        *flags = MD_EXT_TABLES;
        return 0;
    }
    if (name_equal(name, len, "strike") || name_equal(name, len, "strikes") ||
        name_equal(name, len, "del")) {
        *flags = MD_EXT_STRIKETHROUGH;
        return 0;
    }
    if (name_equal(name, len, "task") || name_equal(name, len, "tasklist") ||
        name_equal(name, len, "task_list") || name_equal(name, len, "task-list") ||
        name_equal(name, len, "task_lists") || name_equal(name, len, "task-lists")) {
        *flags = MD_EXT_TASK_LIST;
        return 0;
    }
    if (name_equal(name, len, "footnote") || name_equal(name, len, "footnotes")) {
        *flags = MD_EXT_FOOTNOTES;
        return 0;
    }
    if (name_equal(name, len, "all") || name_equal(name, len, "builtin") ||
        name_equal(name, len, "builtins") || name_equal(name, len, "default")) {
        *flags = MD_EXT_BUILTIN_ALL;
        return 0;
    }
    if (name_equal(name, len, "none")) {
        *flags = MD_EXT_NONE;
        return 0;
    }
    return -1;
}

const char *md_ext_flag_name(md_ext_flags bit)
{
    size_t i;

    for (i = 0; i < MD_EXT_NAME_COUNT; i++) {
        if (md_ext_names[i].bit == bit) {
            return md_ext_names[i].name;
        }
    }
    return NULL;
}

void md_options_init(md_options *options){
    if (options == NULL) {
        return;
    }
    options->extensions = MD_EXT_NONE;
    options->custom = NULL;
    /* Zero means "library default" for both ceilings, so a default option
     * set is exactly the C1-C4 parse. */
    options->limits.max_nesting = 0u;
    options->limits.max_bytes = 0u;
}

unsigned md_limits_nesting(const md_options *options)
{
    size_t want = options != NULL ? options->limits.max_nesting : 0u;

    if (want == 0u) {
        return MD_MAX_NESTING;
    }
    if (want > MD_MAX_NESTING_HARD) {
        return MD_MAX_NESTING_HARD;
    }
    return (unsigned)want;
}

size_t md_limits_bytes(const md_options *options)
{
    return options != NULL ? options->limits.max_bytes : 0u;
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    md_ext_block_fn block;
    md_ext_inline_fn inl;
    void *user;
} md_ext_entry;

struct md_ext_registry {
    md_arena *arena;
    md_ext_entry *entries;
    size_t count;
    size_t capacity;
    size_t block_count;
    size_t inline_count;
};

/*
 * The registry header and its entry array are plain heap allocations, so
 * md_ext_registry_destroy() can release them; the name strings are arena
 * owned and go away with the arena the caller passed in. Making the header
 * arena owned instead would hand md_ext_registry_destroy() a pointer it may
 * not free.
 */
md_ext_registry *md_ext_registry_create(md_arena *arena)
{
    md_ext_registry *registry;

    if (arena == NULL) {
        md_set_error("invalid argument: md_ext_registry_create");
        return NULL;
    }
    registry = (md_ext_registry *)calloc(1u, sizeof *registry);
    if (registry == NULL) {
        return NULL;
    }
    registry->arena = arena;
    return registry;
}

void md_ext_registry_destroy(md_ext_registry *registry)
{
    if (registry == NULL) {
        return;
    }
    /* Names live in the arena; the array and the header are malloc'd. */
    free(registry->entries);
    free(registry);
}

static md_status registry_add(md_ext_registry *registry, const char *name,
                              md_ext_block_fn block, md_ext_inline_fn inl,
                              void *user)
{
    size_t i;
    char *copy;

    if (registry == NULL || name == NULL || name[0] == '\0' ||
        (block == NULL && inl == NULL)) {
        md_set_error("invalid argument: md_ext_register");
        return MD_ERR_INVAL;
    }
    if (registry->count >= MD_EXT_MAX_REGISTERED) {
        md_set_error("too many registered extensions");
        return MD_ERR_LIMIT;
    }
    /* Re-registering a name replaces the previous handler. */
    for (i = 0; i < registry->count; i++) {
        if (strcmp(registry->entries[i].name, name) == 0) {
            registry->entries[i].block = block;
            registry->entries[i].inl = inl;
            registry->entries[i].user = user;
            return MD_OK;
        }
    }
    if (registry->count == registry->capacity) {
        size_t want = registry->capacity ? registry->capacity * 2u : 4u;
        md_ext_entry *grown =
            (md_ext_entry *)realloc(registry->entries, want * sizeof *grown);

        if (grown == NULL) {
            md_set_error("out of memory");
            return MD_ERR_NOMEM;
        }
        registry->entries = grown;
        registry->capacity = want;
    }
    copy = md_arena_strdup(registry->arena, name);
    if (copy == NULL) {
        return MD_ERR_NOMEM;
    }
    registry->entries[registry->count].name = copy;
    registry->entries[registry->count].block = block;
    registry->entries[registry->count].inl = inl;
    registry->entries[registry->count].user = user;
    registry->count++;
    if (block != NULL) {
        registry->block_count++;
    }
    if (inl != NULL) {
        registry->inline_count++;
    }
    return MD_OK;
}

md_status md_ext_register_block(md_ext_registry *registry, const char *name,
                                md_ext_block_fn fn, void *user)
{
    return registry_add(registry, name, fn, NULL, user);
}

md_status md_ext_register_inline(md_ext_registry *registry, const char *name,
                                 md_ext_inline_fn fn, void *user)
{
    return registry_add(registry, name, NULL, fn, user);
}

size_t md_ext_registry_block_count(const md_ext_registry *registry)
{
    return registry != NULL ? registry->block_count : 0u;
}

size_t md_ext_registry_inline_count(const md_ext_registry *registry)
{
    return registry != NULL ? registry->inline_count : 0u;
}

/*
 * Invoke the nth registered block or inline handler. The parsers own the
 * dispatch loop; these keep the registry's private layout in this file.
 */
md_status md_ext_invoke_block(md_ext_registry *registry, size_t ordinal,
                              const md_ext_env *env, md_node *parent,
                              const char *text, size_t len,
                              const char *const *lines, const size_t *line_lens,
                              size_t line_count, size_t *lines_used,
                              int *consumed)
{
    size_t i;
    size_t seen = 0;

    if (registry == NULL || registry->entries == NULL) {
        md_set_error("invalid argument: md_ext_invoke_block");
        return MD_ERR_INVAL;
    }
    *consumed = 0;
    for (i = 0; i < registry->count; i++) {
        if (registry->entries[i].block == NULL) {
            continue;
        }
        if (seen++ != ordinal) {
            continue;
        }
        *lines_used = 0;
        return registry->entries[i].block(registry->entries[i].user, env, parent,
                                          text, len, lines, line_lens,
                                          line_count, lines_used, consumed);
    }
    return MD_OK;
}

md_status md_ext_invoke_inline(md_ext_registry *registry, size_t ordinal,
                               const md_ext_env *env, md_node *parent,
                               const char *text, size_t len, size_t pos,
                               size_t *consumed)
{
    size_t i;
    size_t seen = 0;

    if (registry == NULL || registry->entries == NULL) {
        md_set_error("invalid argument: md_ext_invoke_inline");
        return MD_ERR_INVAL;
    }
    *consumed = 0;
    for (i = 0; i < registry->count; i++) {
        if (registry->entries[i].inl == NULL) {
            continue;
        }
        if (seen++ != ordinal) {
            continue;
        }
        return registry->entries[i].inl(registry->entries[i].user, env, parent,
                                       text, len, pos, consumed);
    }
    return MD_OK;
}

/* ------------------------------------------------------------------ */
/* Configuration files                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    md_arena *arena;
} md_json;

static void json_skip_ws(md_json *j)
{
    while (j->pos < j->len) {
        char c = j->s[j->pos];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->pos++;
            continue;
        }
        break;
    }
}

static int json_peek(const md_json *j)
{
    return j->pos < j->len ? (unsigned char)j->s[j->pos] : -1;
}

/* Read a JSON string into arena memory. Returns NULL on failure. */
static char *json_string(md_arena *arena, md_json *j, size_t *out_len)
{
    md_buffer buf;
    char *out;

    if (json_peek(j) != '"') {
        md_set_errorf("config: expected a string at byte %zu", j->pos);
        return NULL;
    }
    j->pos++;
    md_buffer_init(&buf);
    while (j->pos < j->len) {
        unsigned char c = (unsigned char)j->s[j->pos];

        if (c == '"') {
            char *copy;

            j->pos++;
            *out_len = buf.len;
            out = md_buffer_release(&buf);
            if (out == NULL) {
                return NULL;
            }
            copy = md_arena_strndup(arena, out, *out_len);
            free(out);
            return copy;
        }
        if (c == '\\') {
            unsigned char esc;

            j->pos++;
            if (j->pos >= j->len) {
                break;
            }
            esc = (unsigned char)j->s[j->pos++];
            switch (esc) {
            case '"': case '\\': case '/':
                c = esc;
                break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned long cp = 0;
                int i;

                if (j->pos + 4u > j->len) {
                    md_buffer_free(&buf);
                    md_set_error("config: truncated \\u escape");
                    return NULL;
                }
                for (i = 0; i < 4; i++) {
                    unsigned char h = (unsigned char)j->s[j->pos + (size_t)i];
                    unsigned long digit;

                    if (h >= '0' && h <= '9') {
                        digit = (unsigned long)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        digit = (unsigned long)(h - 'a') + 10uL;
                    } else if (h >= 'A' && h <= 'F') {
                        digit = (unsigned long)(h - 'A') + 10uL;
                    } else {
                        md_buffer_free(&buf);
                        md_set_error("config: bad \\u escape");
                        return NULL;
                    }
                    cp = cp * 16uL + digit;
                }
                j->pos += 4u;
                /* Only the basic plane is encoded; others become U+FFFD,
                 * which is what a Markdown document already does. */
                if (cp >= 0xD800uL && cp <= 0xDFFFuL) {
                    cp = 0xFFFDuL;
                } else if (cp > 0x10FFFFuL) {
                    cp = 0xFFFDuL;
                }
                if (cp < 0x80uL) {
                    char one = (char)cp;

                    if (md_buffer_append(&buf, &one, 1u) != 0) {
                        md_buffer_free(&buf);
                        return NULL;
                    }
                } else if (cp < 0x800uL) {
                    char two[2];

                    two[0] = (char)(0xC0uL | (cp >> 6));
                    two[1] = (char)(0x80uL | (cp & 0x3FuL));
                    if (md_buffer_append(&buf, two, 2u) != 0) {
                        md_buffer_free(&buf);
                        return NULL;
                    }
                } else {
                    char three[3];

                    three[0] = (char)(0xE0uL | (cp >> 12));
                    three[1] = (char)(0x80uL | ((cp >> 6) & 0x3FuL));
                    three[2] = (char)(0x80uL | (cp & 0x3FuL));
                    if (md_buffer_append(&buf, three, 3u) != 0) {
                        md_buffer_free(&buf);
                        return NULL;
                    }
                }
                continue;
            }
            default:
                md_buffer_free(&buf);
                md_set_errorf("config: unknown escape \\%c", (char)esc);
                return NULL;
            }
            if (md_buffer_append_char(&buf, (char)c) != 0) {
                md_buffer_free(&buf);
                return NULL;
            }
            continue;
        }
        if (c < 0x20u) {
            md_buffer_free(&buf);
            md_set_error("config: control character in string");
            return NULL;
        }
        if (md_buffer_append_char(&buf, (char)c) != 0) {
            md_buffer_free(&buf);
            return NULL;
        }
        j->pos++;
    }
    md_buffer_free(&buf);
    md_set_error("config: unterminated string");
    return NULL;
}

/* Skip any JSON value; used to ignore members this checkpoint does not read. */
static int json_skip_value(md_json *j, unsigned depth);

static int json_skip_container(md_json *j, char open, char close,
                               unsigned depth)
{
    if (json_peek(j) != open) {
        return 0;
    }
    j->pos++;
    json_skip_ws(j);
    if (json_peek(j) == close) {
        j->pos++;
        return 1;
    }
    for (;;) {
        json_skip_ws(j);
        if (open == '{') {
            size_t n = 0;
            char *key = json_string(j->arena, j, &n);

            if (key == NULL) {
                return 0;
            }
            json_skip_ws(j);
            if (json_peek(j) != ':') {
                md_set_error("config: expected ':'");
                return 0;
            }
            j->pos++;
        }
        if (!json_skip_value(j, depth + 1u)) {
            return 0;
        }
        json_skip_ws(j);
        if (json_peek(j) == ',') {
            j->pos++;
            continue;
        }
        if (json_peek(j) == close) {
            j->pos++;
            return 1;
        }
        md_set_error("config: malformed container");
        return 0;
    }
}

static int json_skip_value(md_json *j, unsigned depth)
{
    int c;

    if (depth > 32u) {
        md_set_error("config: nesting too deep");
        return 0;
    }
    json_skip_ws(j);
    c = json_peek(j);
    if (c == '"') {
        size_t n = 0;
        char *s = json_string(j->arena, j, &n);

        return s != NULL;
    }
    if (c == '{') {
        return json_skip_container(j, '{', '}', depth);
    }
    if (c == '[') {
        return json_skip_container(j, '[', ']', depth);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = j->pos;

        if (c == '-') {
            j->pos++;
        }
        while (j->pos < j->len) {
            char d = j->s[j->pos];

            if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' ||
                d == '+' || d == '-') {
                j->pos++;
                continue;
            }
            break;
        }
        return j->pos > start;
    }
    if (j->len - j->pos >= 4u && memcmp(j->s + j->pos, "true", 4u) == 0) {
        j->pos += 4u;
        return 1;
    }
    if (j->len - j->pos >= 5u && memcmp(j->s + j->pos, "false", 5u) == 0) {
        j->pos += 5u;
        return 1;
    }
    if (j->len - j->pos >= 4u && memcmp(j->s + j->pos, "null", 4u) == 0) {
        j->pos += 4u;
        return 1;
    }
    md_set_errorf("config: unexpected byte at %zu", j->pos);
    return 0;
}

/* Read `true`, `false` or `null`. Returns 1 and sets *out (null reads as
 * false) on success, 0 when the value at hand is some other JSON value, and
 * -1 on a syntax error. */
static int json_boolean(md_json *j, int *out)
{
    if (j->len - j->pos >= 4u && memcmp(j->s + j->pos, "true", 4u) == 0) {
        j->pos += 4u;
        *out = 1;
        return 1;
    }
    if (j->len - j->pos >= 5u && memcmp(j->s + j->pos, "false", 5u) == 0) {
        j->pos += 5u;
        *out = 0;
        return 1;
    }
    if (j->len - j->pos >= 4u && memcmp(j->s + j->pos, "null", 4u) == 0) {
        j->pos += 4u;
        *out = 0;
        return 1;
    }
    return 0;
}

/* Read a JSON array of extension names and union them into *flags. A name
 * that is not an extension is an error rather than a silent no-op, so a typo
 * in a configuration can never look like a successful run that did less than
 * the file asked for. */
static int json_name_array(md_json *j, md_ext_flags *flags)
{
    if (json_peek(j) != '[') {
        return -1;
    }
    j->pos++;
    json_skip_ws(j);
    if (json_peek(j) == ']') {
        j->pos++;
        return 0;
    }
    for (;;) {
        size_t name_len = 0;
        char *name;
        md_ext_flags bit;

        json_skip_ws(j);
        name = json_string(j->arena, j, &name_len);
        if (name == NULL) {
            return -1;
        }
        if (md_ext_flag_by_name(name, name_len, &bit) != 0) {
            md_set_errorf("config: unknown extension: %s", name);
            return -1;
        }
        /* The union of the named bits, so the order in the file cannot
         * change the outcome. */
        *flags |= bit;
        json_skip_ws(j);
        if (json_peek(j) == ',') {
            j->pos++;
            continue;
        }
        if (json_peek(j) == ']') {
            j->pos++;
            return 0;
        }
        md_set_error("config: malformed array");
        return -1;
    }
}

int md_options_from_json(md_arena *arena, const char *json, size_t len,
                         md_options *options)
{
    md_json j;
    md_ext_flags flags = MD_EXT_NONE;

    if (arena == NULL || json == NULL || options == NULL) {
        md_set_error("invalid argument: md_options_from_json");
        return -1;
    }
    j.s = json;
    j.len = len;
    j.pos = 0;
    j.arena = arena;
    json_skip_ws(&j);
    if (json_peek(&j) != '{') {
        md_set_error("config: expected a JSON object");
        return -1;
    }
    j.pos++;
    json_skip_ws(&j);
    if (json_peek(&j) == '}') {
        j.pos++;
    } else {
        for (;;) {
            size_t key_len = 0;
            char *key;
            md_ext_flags bit = MD_EXT_NONE;
            int value = 0;
            int rc;

            json_skip_ws(&j);
            key = json_string(arena, &j, &key_len);
            if (key == NULL) {
                return -1;
            }
            json_skip_ws(&j);
            if (json_peek(&j) != ':') {
                md_set_error("config: expected ':' after a member name");
                return -1;
            }
            j.pos++;
            json_skip_ws(&j);
            rc = json_boolean(&j, &value);
            if (rc == 0 && json_peek(&j) == '[') {
                /*
                 * The array form, { "extensions": ["tables"] }, is still
                 * accepted. Its members are unioned just like a set of true
                 * keys, so neither the order of the members nor the order of
                 * the names can change what ends up enabled.
                 */
                if (json_name_array(&j, &flags) != 0) {
                    return -1;
                }
            } else {
                if (md_ext_flag_by_name(key, key_len, &bit) != 0) {
                    md_set_errorf("config: unknown extension: %s", key);
                    return -1;
                }
                if (rc == 0) {
                    /*
                     * A real extension name with a value that is neither a
                     * boolean nor an array is a malformed configuration, not
                     * a silent "not enabled": a run must not quietly do less
                     * than the file asked for.
                     */
                    md_set_errorf("config: \"%s\" must be true or false", key);
                    return -1;
                }
                if (value) {
                    flags |= bit;
                }
            }
            json_skip_ws(&j);
            if (json_peek(&j) == ',') {
                j.pos++;
                continue;
            }
            if (json_peek(&j) == '}') {
                j.pos++;
                break;
            }
            md_set_error("config: malformed object");
            return -1;
        }
    }
    json_skip_ws(&j);
    if (j.pos != j.len) {
        md_set_error("config: trailing content after the JSON object");
        return -1;
    }
    options->extensions = flags;
    return 0;
}

int md_options_from_file(md_arena *arena, const char *path, md_options *options)
{
    FILE *fp;
    md_buffer buf;
    char *text;
    int rc;
    int from_stdin;

    if (arena == NULL || path == NULL || options == NULL) {
        md_set_error("invalid argument: md_options_from_file");
        return -1;
    }
    from_stdin = (strcmp(path, "-") == 0);
    fp = from_stdin ? stdin : fopen(path, "rb");
    if (fp == NULL) {
        md_set_errorf("config: cannot open %s", path);
        return -1;
    }
    md_buffer_init(&buf);
    for (;;) {
        char chunk[4096];
        size_t got = fread(chunk, 1u, sizeof chunk, fp);

        if (got > 0u) {
            if (buf.len + got > MD_EXT_CONFIG_MAX) {
                md_buffer_free(&buf);
                if (!from_stdin) {
                    (void)fclose(fp);
                }
                md_set_errorf("config: %s is larger than %u bytes", path,
                              MD_EXT_CONFIG_MAX);
                return -1;
            }
            if (md_buffer_append(&buf, chunk, got) != 0) {
                md_buffer_free(&buf);
                if (!from_stdin) {
                    (void)fclose(fp);
                }
                return -1;
            }
        }
        if (got < sizeof chunk) {
            if (ferror(fp)) {
                md_buffer_free(&buf);
                if (!from_stdin) {
                    (void)fclose(fp);
                }
                md_set_errorf("config: read error on %s", path);
                return -1;
            }
            break;
        }
    }
    if (!from_stdin && fclose(fp) != 0) {
        md_buffer_free(&buf);
        md_set_errorf("config: close error on %s", path);
        return -1;
    }
    text = md_buffer_release(&buf);
    if (text == NULL) {
        return -1;
    }
    rc = md_options_from_json(arena, text, strlen(text), options);
    free(text);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Table syntax                                                        */
/* ------------------------------------------------------------------ */

static size_t skip_spaces(const char *s, size_t len, size_t pos)
{
    while (pos < len && s[pos] == ' ') {
        pos++;
    }
    return pos;
}

static size_t rtrim_pos(const char *s, size_t len)
{
    while (len > 0u && s[len - 1u] == ' ') {
        len--;
    }
    return len;
}

/* A cell is trimmed of surrounding spaces. A pipe that is not a separator
 * keeps its backslash, which the inline parser resolves into a literal "|",
 * so an escape is unescaped in exactly one place.
 *
 * The span is resolved from the original offsets first: trimming the start
 * and then re-deriving the end from the moved offset would run the end
 * past the cell, which is how a trailing "|" used to end up inside the last
 * cell's text. */
static void cell_trim(const char *row, size_t *off, size_t *len)
{
    size_t start = *off;
    size_t end = start + *len;
    size_t trimmed_start = skip_spaces(row, end, start);
    size_t trimmed_end = rtrim_pos(row, end);

    if (trimmed_end < trimmed_start) {
        trimmed_end = trimmed_start; /* a cell of nothing but spaces */
    }
    *off = trimmed_start;
    *len = trimmed_end - trimmed_start;
}

size_t md_ext_table_count(const char *row, size_t len)
{
    if (row == NULL) {
        return 0u;
    }
    return md_ext_table_split(row, len, NULL, NULL, 0u);
}

/*
 * Split one table row into its cells.
 *
 * A pipe is a delimiter, not a cell: a leading or trailing pipe is optional
 * and contributes no cell of its own, so "| a | b |" and "a | b" both yield
 * the two cells a reader sees. Only unescaped '|' separates, and a trailing
 * separator leaves no empty cell behind. Returns the number of cells found,
 * which is the full count even when `capacity` is too small to hold them all
 * (the extra ones are simply not written), so the counting mode used by the
 * table predicate stays exact.
 */
size_t md_ext_table_split(const char *row, size_t len, size_t *cell_off,
                          size_t *cell_len, size_t capacity)
{
    size_t start;
    size_t i;
    size_t count = 0;
    int counting;

    if (row == NULL) {
        return 0u;
    }
    counting = (cell_off == NULL || cell_len == NULL || capacity == 0u);
    len = rtrim_pos(row, len);
    start = skip_spaces(row, len, 0u);
    if (start >= len) {
        return 0u;
    }
    /* A leading pipe opens the row; the first cell starts after it. */
    if (row[start] == '|') {
        start++;
    }
    for (i = start; i < len; i++) {
        if (row[i] == '\\' && i + 1u < len && row[i + 1u] == '|') {
            i++; /* an escaped pipe never separates cells */
            continue;
        }
        if (row[i] != '|') {
            continue;
        }
        if (!counting && count < capacity) {
            cell_off[count] = start;
            cell_len[count] = i - start;
            cell_trim(row, &cell_off[count], &cell_len[count]);
        }
        count++;
        start = i + 1u;
    }
    /*
     * Text after the last separator is a cell; when the row ended with a
     * separator that text is empty and no cell is added.
     */
    if (start < len) {
        if (!counting && count < capacity) {
            cell_off[count] = start;
            cell_len[count] = len - start;
            cell_trim(row, &cell_off[count], &cell_len[count]);
        }
        count++;
    }
    return count;
}

size_t md_ext_table_delims(const char *row, size_t len, md_table_align *align,
                           size_t capacity)
{
    size_t off[8];
    size_t cel[8];
    md_table_align found[8];
    size_t count;
    size_t i;

    if (row == NULL) {
        return 0u;
    }
    if (align == NULL || capacity == 0u) {
        return 0u; /* the caller must say where the alignment goes */
    }
    if (capacity > 8u) {
        capacity = 8u;
    }
    count = md_ext_table_split(row, len, off, cel, capacity);
    if (count == 0u || count > capacity) {
        /* Either there is no delimiter at all, or the row has more columns
         * than the caller can hold. A row that wide is not a table here:
         * accepting it would mean reading past the fixed local arrays. */
        return 0u;
    }
    for (i = 0; i < count; i++) {
        const char *cell = row + off[i];
        size_t n = cel[i];
        size_t left = 0;
        size_t right = 0;
        size_t dashes = 0;
        md_table_align value = MD_TABLE_ALIGN_NONE;

        while (n > 0u && cell[left] == ':') {
            left++;
        }
        while (n > 0u && cell[n - 1u] == ':') {
            n--;
            right++;
        }
        while (n > left && cell[n - 1u] == '-') {
            n--;
            dashes++;
        }
        if (n != left || dashes == 0u) {
            return 0u; /* every cell is dashes, optionally wrapped in colons */
        }
        if (left > 1u || right > 1u) {
            return 0u; /* ":--" is a run of dashes, not an alignment marker */
        }
        if (left > 0u && right > 0u) {
            value = MD_TABLE_ALIGN_CENTER;
        } else if (left > 0u) {
            value = MD_TABLE_ALIGN_LEFT;
        } else if (right > 0u) {
            value = MD_TABLE_ALIGN_RIGHT;
        }
        found[i] = value;
    }
    memcpy(align, found, count * sizeof *align);
    return count;
}

/* ------------------------------------------------------------------ */
/* Task list marker                                                    */
/* ------------------------------------------------------------------ */

int md_ext_task_marker(const char *text, size_t len, size_t indent,
                       int *checked, size_t *rest)
{
    size_t pos = indent;

    if (text == NULL || checked == NULL || rest == NULL) {
        return 0;
    }
    if (pos + 3u > len || text[pos] != '[') {
        return 0;
    }
    if (text[pos + 1u] == ' ') {
        *checked = 0;
    } else if (text[pos + 1u] == 'x' || text[pos + 1u] == 'X') {
        *checked = 1;
    } else {
        return 0;
    }
    if (text[pos + 2u] != ']') {
        return 0;
    }
    pos += 3u;
    /* The marker must stand alone: a space, a tab, or nothing follows. */
    if (pos < len && text[pos] != ' ' && text[pos] != '\t') {
        return 0;
    }
    while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
        pos++;
    }
    *rest = pos;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Footnote labels                                                     */
/* ------------------------------------------------------------------ */

int md_ext_footnote_label(const char *text, size_t len, size_t off,
                          size_t *label_off, size_t *label_len,
                          size_t *end)
{
    size_t i;
    size_t start;

    if (text == NULL || label_off == NULL || label_len == NULL || end == NULL) {
        return 0;
    }
    if (off + 2u >= len || text[off] != '[' || text[off + 1u] != '^') {
        return 0;
    }
    start = off + 2u;
    for (i = start; i < len; i++) {
        if (text[i] == '\\' && i + 1u < len) {
            i++;
            continue;
        }
        if (text[i] == '[' || text[i] == '\n') {
            return 0; /* no nested bracket, no label across a line */
        }
        if (text[i] != ']') {
            continue;
        }
        if (i == start) {
            return 0; /* an empty label is not a footnote */
        }
        *label_off = start;
        *label_len = i - start;
        *end = i + 1u;
        return 1;
    }
    return 0;
}

char *md_ext_footnote_id(md_arena *arena, const char *label, size_t len)
{
    char *normalized;
    size_t n = 0;

    if (arena == NULL || label == NULL) {
        md_set_error("invalid argument: md_ext_footnote_id");
        return NULL;
    }
    /* Footnote labels resolve exactly like reference labels. */
    normalized = md_refmap_normalize_label(label, len, &n);
    if (normalized == NULL) {
        return NULL;
    }
    if (n == 0u) {
        free(normalized);
        md_set_error("footnote label is empty");
        return NULL;
    }
    {
        char *copy = md_arena_strndup(arena, normalized, n);

        free(normalized);
        return copy;
    }
}
