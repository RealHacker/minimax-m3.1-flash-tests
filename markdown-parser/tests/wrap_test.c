/*
 * Library-level checks for checkpoint C3: the public entry points, the
 * renderers, arena ownership, and a leak check built on --wrap so that every
 * allocation made by the library is accounted for.
 *
 * Build:  cc -std=c11 -O2 -Isrc -o wraptest tests/wrap_test.c \
 *          -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=realloc \
 *          -Wl,--wrap=calloc src/arena.c src/ast.c src/blocks.c \
 *          src/entities.c src/inlines.c src/lexer.c src/refmap.c src/status.c
 */
#include "md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Allocation accounting                                              */
/* ------------------------------------------------------------------ */

void *__real_malloc(size_t size);
void __real_free(void *p);
void *__real_realloc(void *p, size_t size);
void *__real_calloc(size_t n, size_t size);

/*
 * Only live block counts are compared: a realloc that grows a buffer does not
 * report a new block here, and the C runtime frees the old storage itself, so
 * the free side stays balanced without a size table.
 */
static long live_blocks;
static int tracking;

static void track_add(void)
{
    live_blocks++;
}

void *__wrap_malloc(size_t size)
{
    if (tracking) {
        track_add();
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t n, size_t size)
{
    if (tracking) {
        track_add();
    }
    return __real_calloc(n, size);
}

void *__wrap_realloc(void *p, size_t size)
{
    if (tracking && p == NULL) {
        track_add();
    }
    return __real_realloc(p, size);
}

void __wrap_free(void *p)
{
    if (tracking && p != NULL) {
        live_blocks--;
    }
    __real_free(p);
}

/* ------------------------------------------------------------------ */

/*
 * The C runtime allocates a few blocks of its own before main() and on the
 * first access to thread-local storage, so the checks compare a baseline
 * captured after a warm-up parse instead of expecting an absolute zero.
 */
static long baseline_blocks;

static int failures;

/* expect() must not disturb the accounting, so tracking is paused inside it. */
static void expect(int condition, const char *what)
{
    int saved = tracking;

    tracking = 0;
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
    tracking = saved;
}

static int count_type(const md_node *node, md_node_type type)
{
    int total = (node->type == type) ? 1 : 0;
    size_t i;

    for (i = 0; i < node->child_count; i++) {
        total += count_type(node->children[i], type);
    }
    return total;
}

static const md_node *find_first(const md_node *node, md_node_type type)
{
    size_t i;

    if (node->type == type) {
        return node;
    }
    for (i = 0; i < node->child_count; i++) {
        const md_node *hit = find_first(node->children[i], type);

        if (hit != NULL) {
            return hit;
        }
    }
    return NULL;
}

/*
 * The registered inline extension used by the C4 checks, and the example
 * documented in README.md: "==marked==" becomes a strong node. A handler is
 * offered every position the core parser did not claim, in registration
 * order, and claims one by setting *consumed; setting nothing leaves the
 * text exactly as it was.
 */
static md_status mark_inline(void *user, const md_ext_env *env, md_node *parent,
                             const char *text, size_t len, size_t pos,
                             size_t *consumed)
{
    size_t end;
    md_node *strong;
    md_node *child;
    md_status status;

    (void)user;
    *consumed = 0;
    if (env == NULL || parent == NULL || text == NULL) {
        return MD_OK;
    }
    if (pos + 4u > len || text[pos] != '=' || text[pos + 1u] != '=') {
        return MD_OK;
    }
    for (end = pos + 2u; end + 1u < len; end++) {
        if (text[end] == '\n') {
            return MD_OK; /* the marker does not cross a line */
        }
        if (text[end] == '=' && text[end + 1u] == '=') {
            break;
        }
    }
    if (end + 1u >= len || end == pos + 2u) {
        return MD_OK; /* unterminated, or empty */
    }
    strong = md_make_strong(env->arena);
    if (strong == NULL) {
        return MD_ERR_NOMEM;
    }
    child = md_make_text_n(env->arena, text + pos + 2u, end - pos - 2u);
    if (child == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(env->arena, strong, child);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(env->arena, parent, strong);
    if (status != MD_OK) {
        return status;
    }
    *consumed = end + 2u - pos;
    return MD_OK;
}

/*
 * The registered block extension used by the C4 checks: a line opening with
 * "!!! " becomes a block quote holding the rest of the line. A block handler
 * is offered the line no core or built-in construct claimed, and takes over
 * the block by setting *consumed and reporting how many lines it used.
 */
static md_status note_block(void *user, const md_ext_env *env, md_node *parent,
                            const char *text, size_t len,
                            const char *const *lines, const size_t *line_lens,
                            size_t line_count, size_t *lines_used, int *consumed)
{
    md_node *quote;
    md_node *para;
    md_node *body;
    md_status status;

    (void)user;
    (void)lines;
    (void)line_lens;
    (void)line_count;
    *lines_used = 0;
    *consumed = 0;
    if (env == NULL || parent == NULL || text == NULL) {
        return MD_OK;
    }
    if (len < 5u || memcmp(text, "!!! ", 4u) != 0) {
        return MD_OK;
    }
    quote = md_node_new(env->arena, MD_NODE_BLOCK_QUOTE);
    para = md_node_new(env->arena, MD_NODE_PARAGRAPH);
    body = md_make_text_n(env->arena, text + 4u, len - 4u);
    if (quote == NULL || para == NULL || body == NULL) {
        return MD_ERR_NOMEM;
    }
    status = md_node_append(env->arena, quote, para);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(env->arena, para, body);
    if (status != MD_OK) {
        return status;
    }
    status = md_node_append(env->arena, parent, quote);
    if (status != MD_OK) {
        return status;
    }
    *lines_used = 1;
    *consumed = 1;
    return MD_OK;
}

int main(void)
{
    md_document *doc;
    md_arena *arena;
    /* Warm up the runtime so the baseline is stable, then measure. */
    tracking = 1;
    doc = md_parse("warm up *state* &amp; [x](/y)\n");
    md_document_destroy(doc);
    baseline_blocks = live_blocks;

    /* md_parse owns and releases its own arena. */
    doc = md_parse("*a* [b](/c) ![d](/e) `f` &amp; \\* x  \ny\nz");
    expect(doc != NULL, "md_parse returns a document");
    if (doc == NULL) {
        tracking = 0;
        return 1;
    }
    expect(count_type(md_document_root(doc), MD_NODE_EM) == 1, "one em node");
    expect(count_type(md_document_root(doc), MD_NODE_LINK) == 1, "one link node");
    expect(count_type(md_document_root(doc), MD_NODE_IMAGE) == 1, "one image node");
    expect(count_type(md_document_root(doc), MD_NODE_CODE) == 1, "one code node");
    expect(count_type(md_document_root(doc), MD_NODE_HARDBREAK) == 1, "one hardbreak node");
    expect(count_type(md_document_root(doc), MD_NODE_SOFTBREAK) == 1, "one softbreak node");
    {
        const md_node *link = find_first(md_document_root(doc), MD_NODE_LINK);

        expect(link != NULL && strcmp(link->destination, "/c") == 0, "link destination");
        /* An absent title stays NULL in the AST and becomes "" in JSON. */
        expect(link != NULL && link->title == NULL, "absent title is NULL in the AST");
    }
    {
        char *json;

        json = md_ast_to_json(md_document_arena(doc), md_document_root(doc));
        expect(json != NULL && strstr(json, "\"title\":\"\"") != NULL,
               "absent title is \"\" in JSON");
    }
    md_document_destroy(doc);
    expect(live_blocks == baseline_blocks, "md_parse frees every block");

    /* md_parse_in against a caller-owned arena: the arena survives destroy. */
    arena = md_arena_create();
    doc = md_parse_in(arena, "## *head*\n\ntext **b** [l](/d)\n",
                      sizeof "## *head*\n\ntext **b** [l](/d)\n" - 1u);
    expect(doc != NULL, "md_parse_in returns a document");
    expect(count_type(md_document_root(doc), MD_NODE_EM) == 1, "heading em node");
    expect(count_type(md_document_root(doc), MD_NODE_STRONG) == 1, "strong node");
    md_document_destroy(doc);
    expect(md_arena_bytes_used(arena) > 0, "caller arena still owns the AST");
    md_arena_destroy(arena);
    expect(live_blocks == baseline_blocks, "caller arena frees every block");

    /* md_parse_inlines appends to any node. */
    arena = md_arena_create();
    {
        md_document *holder = md_document_create(arena, 0);

        expect(md_parse_inlines(arena, "a *b* c", 7, md_document_root(holder)) == MD_OK,
               "md_parse_inlines succeeds");
        expect(md_document_root(holder)->child_count == 3, "inline children appended");
        expect(count_type(md_document_root(holder), MD_NODE_EM) == 1, "inline em node");
        md_document_destroy(holder);
    }
    md_arena_destroy(arena);
    expect(live_blocks == baseline_blocks, "inline parse frees every block");

    /* The C1 entry points keep their block-only shape. */
    doc = md_parse_blocks("*not em* and [not a link](/x)\n");
    expect(doc != NULL, "md_parse_blocks returns a document");
    {
        const md_node *para = find_first(md_document_root(doc), MD_NODE_PARAGRAPH);

        expect(count_type(md_document_root(doc), MD_NODE_EM) == 0,
               "block-only parse has no em");
        expect(para != NULL && para->child_count == 1 &&
               strcmp(para->children[0]->value, "*not em* and [not a link](/x)") == 0,
               "block-only parse keeps one text child");
    }
    md_document_destroy(doc);
    expect(live_blocks == baseline_blocks, "block-only parse frees every block");

    /* Argument validation. */
    tracking = 0;
    expect(md_parse(NULL) == NULL, "null source rejected");
    expect(md_parse_in(NULL, "x", 1) == NULL, "null arena rejected");
    expect(md_parse_inlines(NULL, "x", 1, NULL) == MD_ERR_INVAL, "null arena rejected");
    expect(md_parse_n(NULL, 0) != NULL, "empty buffer is an empty document");
    {
        md_arena *scratch = md_arena_create();
        md_document *empty = md_parse_n("", 0);

        expect(md_parse_inlines(scratch, "x", 1, NULL) == MD_ERR_INVAL,
               "null parent rejected");
        expect(md_render_html(NULL, md_document_root(empty)) == NULL,
               "null arena rejected by md_render_html");
        expect(md_render_html(scratch, NULL) == NULL,
               "null node rejected by md_render_html");
        expect(md_render_text(NULL, md_document_root(empty)) == NULL,
               "null arena rejected by md_render_text");
        expect(md_render_text(scratch, NULL) == NULL,
               "null node rejected by md_render_text");
        /* An empty document renders to the empty string, not to markup. */
        expect(md_render_html(scratch, md_document_root(empty)) != NULL &&
               strcmp(md_render_html(scratch, md_document_root(empty)), "") == 0,
               "empty document renders to nothing");
        md_document_destroy(empty);
        md_arena_destroy(scratch);
    }

    /* A large document under the same accounting. */
    {
        static char big[400001];
        size_t used = 0;
        int links = 0;
        md_document *doc2;

        while (used + 62u < sizeof big - 1u) {
            used += (size_t)snprintf(big + used, sizeof big - used,
                                     "*em* **st** `c` [l](/d \"t\") ![i](/p) \\* &amp;\n"
                                     "<a@b.test>\n\n");
            links++;
        }
        big[used] = '\0';
        doc2 = md_parse(big);
        expect(doc2 != NULL, "large document parses");
        if (doc2 != NULL) {
            expect(count_type(md_document_root(doc2), MD_NODE_LINK) == links,
                   "large document has every link");
            expect(count_type(md_document_root(doc2), MD_NODE_IMAGE) == links,
                   "large document has every image");
            expect(count_type(md_document_root(doc2), MD_NODE_PARAGRAPH) == links,
                   "large document has every paragraph");
            expect(count_type(md_document_root(doc2), MD_NODE_CODE) == links,
                   "large document has every code span");
            expect(count_type(md_document_root(doc2), MD_NODE_EM) == links,
                   "large document has every em");
            expect(count_type(md_document_root(doc2), MD_NODE_STRONG) == links,
                   "large document has every strong");
            expect(count_type(md_document_root(doc2), MD_NODE_SOFTBREAK) == links,
                   "large document has every softbreak");
        }
        md_document_destroy(doc2);
        expect(live_blocks == baseline_blocks, "large parse frees every block");
    }

    /* The renderers: escaping, determinism, ownership, and no leaks. */
    {
        static const char src[] =
            "# Head *em*\n\n"
            "para with <script>alert(\"x\")</script> & 1 < 2 > 0, [l](<a\"b> 't\"i') ![i](/p \"A\") `c<d>`\n\n"
            "- one\n- two\n  - deep\n\n"
            "1. first\n\n2. second\n\n"
            "> quote\n> lines\n\n"
            "```c\nif (a < b && c > d) { }\n```\n\n"
            "---\n\n[l]: /ref \"RT\"\n\nsee [l] and [l][] and [l]\n";
        char *html;
        char *html2;
        char *text;
        char *text2;

        tracking = 1;
        doc = md_parse(src);
        expect(doc != NULL, "renderer document parses");
        if (doc != NULL) {
            html = md_render_html(md_document_arena(doc), md_document_root(doc));
            html2 = md_render_html(md_document_arena(doc), md_document_root(doc));
            text = md_render_text(md_document_arena(doc), md_document_root(doc));
            text2 = md_render_text(md_document_arena(doc), md_document_root(doc));

            expect(html != NULL && html2 != NULL && strcmp(html, html2) == 0,
                   "md_render_html is deterministic");
            expect(text != NULL && text2 != NULL && strcmp(text, text2) == 0,
                   "md_render_text is deterministic");
            expect(html != NULL && strstr(html, "<h1>Head <em>em</em></h1>") != NULL,
                   "html renders headings and emphasis");
            expect(html != NULL && strstr(html, "&lt;script&gt;") != NULL &&
                   strstr(html, "<script>") == NULL,
                   "html escapes a literal script tag");
            expect(html != NULL && strstr(html, "1 &lt; 2 &gt; 0") != NULL,
                   "html escapes comparison operators in text");
            expect(html != NULL && strstr(html, "href=\"a&quot;b\"") != NULL,
                   "html escapes a quote in a destination");
            expect(html != NULL && strstr(html, "title=\"t&quot;i\"") != NULL,
                   "html escapes a quote in a title");
            expect(html != NULL && strstr(html, "<code>c&lt;d&gt;</code>") != NULL,
                   "html escapes a code span");
            expect(html != NULL && strstr(html, "if (a &lt; b &amp;&amp; c &gt; d)") != NULL,
                   "html escapes a code block");
            expect(html != NULL && strstr(html, "class=\"language-c\"") != NULL,
                   "html renders the code block info string");
            expect(html != NULL && strstr(html, "<ul>") != NULL &&
                   strstr(html, "<ol>") != NULL && strstr(html, "<li>") != NULL,
                   "html renders lists");
            expect(html != NULL && strstr(html, "<blockquote>") != NULL,
                   "html renders blockquotes");
            expect(html != NULL && strstr(html, "<hr />") != NULL,
                   "html renders a thematic break");
            expect(html != NULL && strstr(html, "<img src=\"/p\" alt=\"i\" title=\"A\" />") != NULL,
                   "html renders an image with its alt text");
            expect(html != NULL && strstr(html, "<a href=\"/ref\" title=\"RT\">l</a>") != NULL,
                   "html renders a reference link");
            expect(html != NULL && html[0] != '\0' &&
                   html[strlen(html) - 1u] == '\n',
                   "html output ends with a newline");
            expect(html != NULL && strstr(html, "<li>one</li>") != NULL,
                   "a tight list item has no paragraph wrapper");
            expect(html != NULL && strstr(html, "<li><p>first</p>") != NULL,
                   "a loose list item keeps the paragraph wrapper");

            expect(text != NULL && strstr(text, "Head em") == text,
                   "text output drops heading markup");
            expect(text != NULL && strstr(text, "<script>alert(\"x\")</script>") != NULL,
                   "text output keeps literal source characters");
            expect(text != NULL && strstr(text, "*em*") == NULL &&
                   strstr(text, "`") == NULL && strstr(text, "](") == NULL,
                   "text output has no inline markup left");
            expect(text != NULL && strstr(text, "  one\n  two\n    deep\n") != NULL,
                   "text output keeps list line structure");
            expect(text != NULL && strstr(text, "quote\nlines\n") != NULL,
                   "text output keeps blockquote lines");
            expect(text != NULL && strstr(text, "if (a < b && c > d) { }\n") != NULL,
                   "text output keeps code block content");
            expect(text != NULL && text[0] != '\0' && text[strlen(text) - 1u] == '\n',
                   "text output ends with a newline");
            expect(text != NULL && text[strlen(text) - 2u] != '\n',
                   "text output has no trailing blank line");
        }
        md_document_destroy(doc);
        expect(live_blocks == baseline_blocks, "document arena render frees every block");
    }

    /* Rendered strings outlive the document when the arena is caller-owned. */
    {
        static const char src[] = "# *keep*\n\nbody [x](/y)\n";
        static char keep_html[128];
        static char keep_text[128];

        arena = md_arena_create();
        doc = md_parse_in(arena, src, sizeof src - 1u);
        expect(doc != NULL, "renderer document parses in a caller arena");
        if (doc != NULL) {
            char *html = md_render_html(arena, md_document_root(doc));
            char *text = md_render_text(arena, md_document_root(doc));

            /* Copy out, then destroy the document: the arena still holds bytes. */
            if (html != NULL) {
                size_t n = strlen(html);

                if (n >= sizeof keep_html) {
                    n = sizeof keep_html - 1u;
                }
                memcpy(keep_html, html, n);
                keep_html[n] = '\0';
            }
            if (text != NULL) {
                size_t n = strlen(text);

                if (n >= sizeof keep_text) {
                    n = sizeof keep_text - 1u;
                }
                memcpy(keep_text, text, n);
                keep_text[n] = '\0';
            }
            md_document_destroy(doc);
            /* The same arena is still usable, so the copies stay valid. */
            expect(strcmp(keep_html, "<h1><em>keep</em></h1>\n<p>body <a href=\"/y\">x</a></p>\n") == 0,
                   "html copy survives md_document_destroy");
            expect(strcmp(keep_text, "keep\n\nbody x\n") == 0,
                   "text copy survives md_document_destroy");
        }
        md_arena_destroy(arena);
        expect(live_blocks == baseline_blocks, "caller arena render frees every block");
    }

    /* A large document renders through the same accounting. */
    {
        static char big[400001];
        size_t used = 0;
        md_document *doc3;

        while (used + 62u < sizeof big - 1u) {
            used += (size_t)snprintf(big + used, sizeof big - used,
                                     "*em* [l](/d \"t\") ![i](/p) `<x>` &amp;\n"
                                     "- item\n\n> quote\n\n");
        }
        big[used] = '\0';
        tracking = 1;
        doc3 = md_parse(big);
        expect(doc3 != NULL, "large document parses for rendering");
        if (doc3 != NULL) {
            char *html = md_render_html(md_document_arena(doc3), md_document_root(doc3));
            char *text = md_render_text(md_document_arena(doc3), md_document_root(doc3));

            expect(html != NULL && strstr(html, "<em>em</em>") != NULL,
                   "large document renders html");
            expect(text != NULL && strstr(text, "em l i <x> &") != NULL,
                   "large document renders text");
        }
        md_document_destroy(doc3);
        expect(live_blocks == baseline_blocks, "large render frees every block");
    }

    /* The delimiter stack is bounded, and the overflow stays literal. */
    {
        char many[4096];
        size_t used = 0;
        int i;

        for (i = 0; i < 40; i++) {
            used += (size_t)snprintf(many + used, sizeof many - used, "*e%d* **s%d**\n", i, i);
        }
        many[used] = '\0';
        doc = md_parse(many);
        expect(doc != NULL, "delimiter overflow document parses");
        if (doc != NULL) {
            const md_node *root = md_document_root(doc);
            int em = count_type(root, MD_NODE_EM);
            int strong = count_type(root, MD_NODE_STRONG);
            char *json = md_ast_to_json(md_document_arena(doc), root);

            /* Every pair that fits in the bounded stack is still emphasis. */
            expect(em > 0 && em < 40, "bounded delimiter stack caps em");
            expect(strong > 0 && strong < 40, "bounded delimiter stack caps strong");
            /* The text beyond the bound survives as literal text. */
            expect(json != NULL && strstr(json, "*e39* **s39**") != NULL,
                   "delimiters past the bound stay literal");
        }
        md_document_destroy(doc);
        expect(live_blocks == baseline_blocks, "overflow parse frees every block");
    }

    /* ---------------- checkpoint C4: extensions ---------------- */

    /*
     * A registered inline extension. "==marked==" becomes strong, the same
     * way "**strong**" does, but only while the extension is registered:
     * this is the whole of the example documented in README.md.
     */
    {
        md_arena *ext_arena;
        MDParseOptions opts;
        md_ext_registry *registry;
        char *json;
        /* The extension arena and the registry stay alive across the checks
         * below, so whatever they own is captured as the baseline after the
         * registrations are done. */
        long ext_baseline;

        ext_arena = md_arena_create();
        expect(ext_arena != NULL, "extension arena is created");
        registry = md_ext_registry_create(ext_arena);
        expect(registry != NULL, "registry is created");
        expect(md_ext_registry_inline_count(registry) == 0u,
               "a fresh registry has no inline handlers");
        expect(md_ext_register_inline(registry, "mark", mark_inline, NULL) == MD_OK,
               "an inline extension registers");
        expect(md_ext_register_block(registry, "note", note_block, NULL) == MD_OK,
               "a block extension registers");
        expect(md_ext_registry_block_count(registry) == 1u,
               "the registered block handler is counted");
        expect(md_ext_registry_inline_count(registry) == 1u,
               "the registered inline handler is counted");
        expect(md_ext_register_inline(registry, "mark", mark_inline, NULL) == MD_OK &&
               md_ext_registry_inline_count(registry) == 1u,
               "re-registering a name replaces the handler");
        expect(md_ext_register_inline(registry, NULL, mark_inline, NULL) == MD_ERR_INVAL,
               "a null name is rejected");
        expect(md_ext_register_inline(registry, "bad", NULL, NULL) == MD_ERR_INVAL,
               "a null handler is rejected");
        ext_baseline = live_blocks;

        md_options_init(&opts);
        opts.custom = registry;

        /* Registered, so the marker is strong ... */
        doc = md_parse_opts_n(&opts, "a ==marked== b\n",
                              strlen("a ==marked== b\n"));
        expect(doc != NULL, "custom inline extension parses");
        if (doc != NULL) {
            json = md_ast_to_json(md_document_arena(doc), md_document_root(doc));
            expect(json != NULL && strstr(json, "==") == NULL,
                   "the custom marker is consumed, not kept as text");
            expect(json != NULL && count_type(md_document_root(doc), MD_NODE_STRONG) == 1,
                   "the custom marker produced a strong node");
        }
        md_document_destroy(doc);

        /* The block handler claims its own line and no other. */
        doc = md_parse_opts_n(&opts, "!!! aside\nplain\n", strlen("!!! aside\nplain\n"));
        expect(doc != NULL, "the custom block extension parses");
        if (doc != NULL) {
            const md_node *root = md_document_root(doc);
            char *html = md_render_html(md_document_arena(doc), root);

            expect(count_type(root, MD_NODE_BLOCK_QUOTE) == 1,
                   "the custom block marker produced one block quote");
            expect(html != NULL && strstr(html, "<blockquote>\n<p>aside</p>") != NULL,
                   "the custom block renders as a block quote");
            expect(html != NULL && strstr(html, "<p>plain</p>") != NULL,
                   "the line the custom block declined stays a paragraph");
        }
        md_document_destroy(doc);

        /* A registered handler is only offered a line nothing else claimed,
         * so it can never shadow core syntax: ">>> " stays three block
         * quotes even with the handler installed. */
        doc = md_parse_opts_n(&opts, ">>> deep\n", strlen(">>> deep\n"));
        expect(doc != NULL, "a core block line parses with the registry set");
        if (doc != NULL) {
            expect(count_type(md_document_root(doc), MD_NODE_BLOCK_QUOTE) == 3,
                   "a registered block extension cannot shadow core syntax");
        }
        md_document_destroy(doc);

        /* The same document without the registry is ordinary text. */
        {
            MDParseOptions plain;

            md_options_init(&plain);
            doc = md_parse_opts_n(&plain, "a ==marked== b\n",
                                  strlen("a ==marked== b\n"));
            expect(doc != NULL, "the same input parses without a registry");
            if (doc != NULL) {
                json = md_ast_to_json(md_document_arena(doc), md_document_root(doc));
                expect(json != NULL && strstr(json, "==marked==") != NULL,
                       "an unregistered marker stays literal text");
                expect(count_type(md_document_root(doc), MD_NODE_STRONG) == 0,
                       "an unregistered marker makes no strong node");
            }
            md_document_destroy(doc);
        }
        expect(live_blocks == ext_baseline,
               "custom extension parse frees every block");

        /* The built-in options, the configuration forms and the tree. */
        md_options_init(&opts);
        expect(opts.extensions == MD_EXT_NONE, "options default to no extension");
        opts.extensions = MD_EXT_TABLES | MD_EXT_STRIKETHROUGH;
        doc = md_parse_opts_n(&opts,
                              "| a | b |\n| :- | -: |\n| 1 | 2 |\n\n~~old~~\n",
                              strlen("| a | b |\n| :- | -: |\n| 1 | 2 |\n\n~~old~~\n"));
        expect(doc != NULL, "an extension document parses");
        if (doc != NULL) {
            const md_node *root = md_document_root(doc);
            char *html;

            expect(root->extensions == (MD_EXT_TABLES | MD_EXT_STRIKETHROUGH),
                   "the document records the mask it was parsed with");
            expect(count_type(root, MD_NODE_TABLE) == 1, "the table extension parsed a table");
            expect(count_type(root, MD_NODE_STRIKETHROUGH) == 1,
                   "the strikethrough extension parsed a run");
            expect(find_first(root, MD_NODE_TABLE_ROW) != NULL, "the table has rows");
            html = md_render_html(md_document_arena(doc), root);
            expect(html != NULL && strcmp(html,
                   "<table>\n<thead>\n<th align=\"left\">a</th>\n"
                   "<th align=\"right\">b</th>\n</thead>\n<tbody>\n"
                   "<td align=\"left\">1</td>\n<td align=\"right\">2</td>\n"
                   "</tbody>\n</table>\n<p><del>old</del></p>\n") == 0,
                   "tables and strikethrough render as expected");
        }
        md_document_destroy(doc);

        /* Task lists and footnotes, with the document-level footnotes array. */
        md_options_init(&opts);
        opts.extensions = MD_EXT_TASK_LIST | MD_EXT_FOOTNOTES;
        doc = md_parse_opts_n(&opts, "- [x] done\n- [ ] todo\n\nsee[^a]\n\n[^a]: note\n", strlen("- [x] done\n- [ ] todo\n\nsee[^a]\n\n[^a]: note\n"));
        expect(doc != NULL, "a task and footnote document parses");
        if (doc != NULL) {
            const md_node *root = md_document_root(doc);
            char *json2 = md_ast_to_json(md_document_arena(doc), root);

            expect(find_first(root, MD_NODE_FOOTNOTE_REF) != NULL,
                   "the footnote reference is a node");
            expect(root->footnote_count == 1u, "the definition is on the document");
            expect(json2 != NULL && strstr(json2, "\"checked\":true") != NULL,
                   "a checked item reports checked");
            expect(json2 != NULL && strstr(json2, "\"checked\":false") != NULL,
                   "an unchecked item reports unchecked");
            expect(json2 != NULL && strstr(json2, "\"footnotes\":[{") != NULL,
                   "the document JSON carries a footnotes array");
            expect(json2 != NULL && strstr(json2, "\"children\":[{") != NULL,
                   "the document JSON still carries its children");
            expect(json2 != NULL && strstr(json2, "id=\"fn-a\"") == NULL,
                   "the JSON is a tree, not HTML");
        }
        md_document_destroy(doc);
        expect(live_blocks == ext_baseline, "extension parse frees every block");

        /* With nothing enabled the same document is the C1-C3 tree. */
        doc = md_parse("| a | b |\n| :- | -: |\n| 1 | 2 |\n\n~~old~~\n");
        expect(doc != NULL, "the no-extension control parses");
        if (doc != NULL) {
            char *json3 = md_ast_to_json(md_document_arena(doc), md_document_root(doc));

            expect(json3 != NULL && strstr(json3, "~~old~~") != NULL,
                   "a disabled extension keeps its syntax as text");
            expect(json3 != NULL && strstr(json3, "\"footnotes\"") == NULL,
                   "a document parsed without extensions has no footnotes array");
        }
        md_document_destroy(doc);
        expect(live_blocks == ext_baseline, "control parse frees every block");

        /* The configuration forms agree with each other. */
        {
            static const char *forms[] = {
                "{\"tables\":true,\"footnotes\":true,\"strikethrough\":true,"
                "\"task_lists\":true}",
                "{ \"strikethrough\" : true , \"tables\" : true ,\n"
                "  \"footnotes\":true, \"task_lists\":true }",
                "{\"extensions\":[\"footnotes\",\"tables\",\"strikethrough\","
                "\"task_lists\"]}",
                "{\"all\":true}"
            };
            static const char *quiet[] = {
                "{}",
                "{\"tables\":false,\"footnotes\":false}",
                "{\"extensions\":[]}"
            };
            MDParseOptions a;
            MDParseOptions b;
            size_t i;

            for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
                md_options_init(&a);
                expect(md_options_from_json(ext_arena, forms[i], strlen(forms[i]), &a) == 0,
                       "a configuration form is accepted");
                expect(a.extensions == MD_EXT_BUILTIN_ALL,
                       "a configuration form enables every built-in");
            }
            for (i = 0; i < sizeof quiet / sizeof quiet[0]; i++) {
                md_options_init(&b);
                expect(md_options_from_json(ext_arena, quiet[i], strlen(quiet[i]), &b) == 0,
                       "an empty configuration is accepted");
                expect(b.extensions == MD_EXT_NONE,
                       "an empty configuration enables nothing");
            }
            expect(md_options_from_json(ext_arena, "{\"nope\":true}", 12u, &a) == -1,
                   "an unknown name is rejected");
            expect(md_options_from_json(ext_arena, "{\"tables\":1}", 12u, &a) == -1,
                   "a non-boolean value is rejected");
            expect(md_options_from_json(ext_arena, "[]", 2u, &a) == -1,
                   "a non-object configuration is rejected");
            expect(md_options_from_json(ext_arena, "{", 1u, &a) == -1,
                   "a truncated configuration is rejected");
        }

        /* The registry is released without touching the arena: the header
         * and the entry array are the registry's own two blocks. */
        {
            long before_destroy = live_blocks;

            md_ext_registry_destroy(registry);
            expect(live_blocks == before_destroy - 2,
                   "md_ext_registry_destroy frees the registry");
        }
        md_arena_destroy(ext_arena);
        expect(live_blocks == baseline_blocks, "the extension arena frees every block");
    }

    /* md_parse_inlines_opts() applies the extensions to a bare inline run. */
    {
        MDParseOptions opts;
        md_arena *owner;
        md_node *para;

        md_options_init(&opts);
        opts.extensions = MD_EXT_STRIKETHROUGH;
        owner = md_arena_create();
        para = md_node_new(owner, MD_NODE_PARAGRAPH);
        expect(para != NULL, "a paragraph node is created");
        expect(md_parse_inlines_opts(owner, "~~x~~", strlen("~~x~~"), para, &opts) == MD_OK,
               "md_parse_inlines_opts accepts extended inline content");
        expect(count_type(para, MD_NODE_STRIKETHROUGH) == 1,
               "md_parse_inlines_opts applied the extension");
        md_arena_destroy(owner);
        expect(live_blocks == baseline_blocks, "the inline options parse frees every block");
    }

    /* ---------------------------------------------------------------- */
    /* Streaming and limits (checkpoint C5)                             */
    /* ---------------------------------------------------------------- */

    /* A memory cap is charged against the normalized text, and both paths
     * have to agree about where the line falls.  The interesting case is
     * text that shrinks: a CRLF pair collapses to one byte, so a cap that
     * sits between the raw length and the normalized length has to let the
     * document through.  Both paths must agree; the normalizer's own view
     * of that cap is pinned by tests/lexer_test.c, which is where the
     * difference is actually observable -- in the parse path the per-line
     * span array costs more than a CRLF ever saves. */
    {
        static const char crlf[] =
            "aaaa\r\nbbbb\r\ncccc\r\ndddd\r\neeee\r\nffff\r\n"
            "gggg\r\nhhhh\r\niiii\r\njjjj\r\n";
        size_t raw = sizeof crlf - 1u;
        MDParseOptions opts;
        md_document *a;
        md_document *b;
        md_arena *out = md_arena_create();
        FILE *in = tmpfile();
        char *ea;
        char *eb;

        expect(raw == 60u, "the CRLF input is the length it claims to be");
        md_options_init(&opts);
        a = md_parse_opts_n(&opts, crlf, raw);
        expect(in != NULL, "a temporary file is created for the stream");
        if (in != NULL) {
            expect(fwrite(crlf, 1u, raw, in) == raw, "the input is written");
            rewind(in);
            b = md_parse_stream(in, &opts);
        } else {
            b = NULL;
        }
        ea = a != NULL ? md_ast_to_json(out, md_document_root(a)) : NULL;
        eb = b != NULL ? md_ast_to_json(out, md_document_root(b)) : NULL;
        expect(a != NULL, "a CRLF document parses");
        expect(ea != NULL && eb != NULL && strcmp(ea, eb) == 0,
               "a CRLF document parses the same on both paths");
        expect(ea != NULL && strstr(ea, "\\r") == NULL,
               "no CR reaches the AST");
        md_arena_destroy(out);
        md_document_destroy(a);
        md_document_destroy(b);
        if (in != NULL) {
            fclose(in);
        }
    }

    /* The resolved limits: 0 means "the library default", and a value past
     * the hard ceiling is clamped rather than honoured. */
    {
        MDParseOptions opts;

        md_options_init(&opts);
        expect(md_limits_nesting(NULL) == MD_MAX_NESTING,
               "a NULL option set parses at the default nesting limit");
        expect(md_limits_nesting(&opts) == MD_MAX_NESTING,
               "a default option set parses at the default nesting limit");
        expect(md_limits_bytes(NULL) == 0u, "no memory cap by default");
        expect(md_limits_bytes(&opts) == 0u, "a default option set has no memory cap");

        opts.limits.max_nesting = 8u;
        expect(md_limits_nesting(&opts) == 8u, "a configured nesting limit is used");
        opts.limits.max_nesting = 1000000u;
        expect(md_limits_nesting(&opts) == MD_MAX_NESTING_HARD,
               "a nesting limit past the hard ceiling is clamped");
        opts.limits.max_nesting = MD_MAX_NESTING_HARD;
        expect(md_limits_nesting(&opts) == MD_MAX_NESTING_HARD,
               "the hard ceiling itself is allowed");
        opts.limits.max_bytes = 4096u;
        expect(md_limits_bytes(&opts) == 4096u, "a configured memory cap is used");
    }

    /* Where each limit applies.
     *
     * Nesting is enforced by every entry point, because refusing to descend
     * costs nothing.  A memory cap is a property of the arena, so on the _in
     * forms it is the arena's cap that bounds memory and the option's value
     * that md_limits_bytes() reports.  Checking the option afterwards could
     * only report an overrun once the memory had been taken, so it is not
     * attempted; the documented contract is that the caller's arena is the
     * caller's memory policy. */
    {
        static const char deep[] = "> > > > > > > > > > > > > > x";
        MDParseOptions opts;
        md_arena *capped;
        md_document *doc;

        md_options_init(&opts);
        opts.limits.max_nesting = 4u;

        /* Nesting is refused even in an uncapped arena. */
        doc = md_parse_opts_n(&opts, deep, sizeof deep - 1u);
        expect(doc == NULL, "a nesting limit applies to md_parse_opts_n()");
        expect(md_last_error()->kind == MD_ERROR_NESTING,
               "and it is reported as a nesting failure");
        md_document_destroy(doc);

        capped = md_arena_create();
        doc = md_parse_opts_in(capped, &opts, deep, sizeof deep - 1u);
        expect(doc == NULL, "a nesting limit applies to md_parse_opts_in()");
        expect(md_last_error()->kind == MD_ERROR_NESTING,
               "in a caller arena too, because the stack is the reason");
        md_document_destroy(doc);
        md_arena_destroy(capped);

        /* A memory cap in the options does not cap a caller's arena: the
         * document parses, and the cap is still reported as configured. */
        md_options_init(&opts);
        opts.limits.max_bytes = 16u;
        capped = md_arena_create();
        doc = md_parse_opts_in(capped, &opts, "a small paragraph", 16u);
        expect(doc != NULL,
               "an option memory cap does not cap a caller's arena");
        expect(md_limits_bytes(&opts) == 16u,
               "the configured cap is still what md_limits_bytes() reports");
        md_document_destroy(doc);
        md_arena_destroy(capped);

        /* The arena's own cap does bound it. */
        capped = md_arena_create_limit(16u);
        doc = md_parse_opts_in(capped, &opts, "a small paragraph", 16u);
        expect(doc == NULL, "the arena's own cap bounds a caller-arena parse");
        expect(md_last_error()->kind == MD_ERROR_MEMORY,
               "and the refusal is a memory error");
        md_document_destroy(doc);
        md_arena_destroy(capped);
    }

    /* md_parse_stream() over a memory stream: the same bytes through the
     * streaming and the one-shot path must give the same JSON. */
    {
        static const char *text =
            "# H\n\npara *em* **strong** `code` [l](/u) ![i](/u)\n\n"
            "> quote\n\n- a\n- b\n\n    code\n\n---\n\n[l]: /ref \"t\"\n";
        FILE *in;
        md_document *a = NULL;
        md_document *b = NULL;
        char *ja = NULL;
        char *jb = NULL;

        a = md_parse_n(text, strlen(text));
        expect(a != NULL, "the buffered parse of the comparison text succeeds");
        in = tmpfile();
        expect(in != NULL, "a temporary stream is created");
        if (a != NULL && in != NULL) {
            expect(fwrite(text, 1u, strlen(text), in) == strlen(text),
                   "the comparison text is written");
            rewind(in);
            b = md_parse_stream(in, NULL);
            expect(b != NULL, "md_parse_stream returns a document");
            ja = md_ast_to_json(md_document_arena(a), md_document_root(a));
            jb = b != NULL ? md_ast_to_json(md_document_arena(b), md_document_root(b)) : NULL;
            expect(ja != NULL && jb != NULL && strcmp(ja, jb) == 0,
                   "streamed and one-shot AST JSON are byte-identical");
        }
        md_document_destroy(b);
        md_document_destroy(a);
        if (in != NULL) {
            fclose(in);
        }
    }

    /* An empty stream is an empty document, not a failure. */
    {
        FILE *in = tmpfile();
        md_document *doc;

        expect(in != NULL, "an empty temporary stream is created");
        if (in != NULL) {
            rewind(in);
            doc = md_parse_stream(in, NULL);
            expect(doc != NULL, "an empty stream parses");
            expect(doc != NULL && md_document_root(doc)->child_count == 0,
                   "an empty stream has no children");
            md_document_destroy(doc);
            fclose(in);
        }
    }

    /* md_parse_stream_in() leaves a caller-owned arena alone. */
    {
        FILE *in = tmpfile();
        md_arena *owner = md_arena_create();
        md_document *doc = NULL;

        if (in != NULL) {
            (void)fwrite("a *b*\n", 1u, 6u, in);
            rewind(in);
            doc = md_parse_stream_in(owner, in, 0u, NULL);
        }
        expect(doc != NULL, "md_parse_stream_in returns a document");
        expect(doc != NULL && count_type(md_document_root(doc), MD_NODE_EM) == 1,
               "md_parse_stream_in parsed inlines");
        md_document_destroy(doc);
        expect(md_arena_bytes_used(owner) > 0, "the caller arena still owns the AST");
        if (in != NULL) {
            fclose(in);
        }
        md_arena_destroy(owner);
        expect(live_blocks == baseline_blocks, "the streaming parse frees every block");
    }

    /* md_parse_stream_in() with max_bytes stops at that many raw bytes. */
    {
        FILE *in = tmpfile();
        md_arena *owner = md_arena_create();
        md_document *doc = NULL;

        if (in != NULL) {
            (void)fwrite("a *b* c\n", 1u, 8u, in);
            rewind(in);
            md_error_reset();
            doc = md_parse_stream_in(owner, in, 4u, NULL);
            expect(doc == NULL, "md_parse_stream_in enforces max_bytes");
            expect(md_last_error()->kind == MD_ERROR_MEMORY,
                   "a max_bytes failure is reported as a memory error");
        }
        md_document_destroy(doc);
        if (in != NULL) {
            fclose(in);
        }
        md_arena_destroy(owner);
        expect(live_blocks == baseline_blocks, "a capped stream frees every block");
    }

    /* A nesting limit produces a structured error, not a crash. */
    {
        static const char *deep = ">>>>>>>>> deep\n";
        MDParseOptions opts;
        md_document *doc;
        md_error record;

        md_options_init(&opts);
        opts.limits.max_nesting = 2u;
        md_error_reset();
        doc = md_parse_opts_n(&opts, deep, strlen(deep));
        expect(doc == NULL, "a document past the nesting limit is refused");
        record = *md_last_error();
        expect(record.kind == MD_ERROR_NESTING, "the failure is a nesting failure");
        expect(strcmp(record.message, "nesting too deep") == 0,
               "the nesting failure names itself");
        expect(record.line == 1u && record.col > 0u,
               "the nesting failure carries a position");
        md_document_destroy(doc);
        expect(live_blocks == baseline_blocks, "a refused parse frees every block");
    }

    /* The same document within the limit parses. */
    {
        MDParseOptions opts;
        md_document *doc;

        md_options_init(&opts);
        opts.limits.max_nesting = 3u;
        doc = md_parse_opts_n(&opts, "> > deep\n", strlen("> > deep\n"));
        expect(doc != NULL, "a document within the nesting limit parses");
        md_document_destroy(doc);
    }

    /* A memory cap produces a structured error too. */
    {
        MDParseOptions opts;
        md_document *doc;

        md_options_init(&opts);
        opts.limits.max_bytes = 16u;
        md_error_reset();
        doc = md_parse_opts_n(&opts, "a *b* c\n\nd *e* f\n", 15u);
        expect(doc == NULL, "a document past the memory cap is refused");
        expect(md_last_error()->kind == MD_ERROR_MEMORY,
               "the failure is a memory failure");
        expect(strstr(md_last_error()->message, "memory cap exceeded") != NULL,
               "the memory failure says so");
        md_document_destroy(doc);
        expect(live_blocks == baseline_blocks, "a capped parse frees every block");
    }

    /* md_error_to_json() renders the record, keys always in the same order. */
    {
        md_arena *out;
        char *json;
        md_error record;

        out = md_arena_create();
        memset(&record, 0, sizeof record);
        record.kind = MD_ERROR_NESTING;
        record.line = 12u;
        record.col = 5u;
        (void)snprintf(record.message, sizeof record.message, "%s", "nesting too deep");
        json = md_error_to_json(out, &record);
        expect(json != NULL &&
               strcmp(json, "{\"error\":{\"line\":12,\"col\":5,"
                            "\"message\":\"nesting too deep\"}}") == 0,
               "md_error_to_json renders line, col and message in order");
        /* A failure with no place in the input reports zero for both. */
        (void)snprintf(record.message, sizeof record.message, "%s", "read error");
        record.line = 0u;
        record.col = 0u;
        json = md_error_to_json(out, &record);
        expect(json != NULL &&
               strcmp(json, "{\"error\":{\"line\":0,\"col\":0,"
                            "\"message\":\"read error\"}}") == 0,
               "a failure with no position reports zero");
        /* A message that needs escaping is escaped. */
        (void)snprintf(record.message, sizeof record.message, "%s",
                       "a \"quoted\" path");
        json = md_error_to_json(out, &record);
        expect(json != NULL && strstr(json, "\\\"quoted\\\"") != NULL,
               "md_error_to_json escapes the message");
        /* A NULL record is the same as md_last_error(). */
        json = md_error_to_json(out, NULL);
        expect(json != NULL && strncmp(json, "{\"error\":{", 10u) == 0,
               "md_error_to_json accepts a NULL record");
        md_arena_destroy(out);
    }

    /* A limit applies to the streaming path as well, identically. */
    {
        static const char *text = "a *b* c\n\nd *e* f\n\n";
        MDParseOptions opts;
        md_document *a;
        md_document *b = NULL;
        char *ea = NULL;
        char *eb = NULL;
        md_arena *out;
        FILE *in;

        md_options_init(&opts);
        opts.limits.max_bytes = 4096u;

        a = md_parse_opts_n(&opts, text, strlen(text));
        in = tmpfile();
        if (in != NULL) {
            (void)fwrite(text, 1u, strlen(text), in);
            rewind(in);
            b = md_parse_stream(in, &opts);
        }
        expect(a != NULL && b != NULL, "a limited parse succeeds on both paths");
        out = md_arena_create();
        if (a != NULL) {
            ea = md_ast_to_json(out, md_document_root(a));
        }
        if (b != NULL) {
            eb = md_ast_to_json(out, md_document_root(b));
        }
        expect(ea != NULL && eb != NULL && strcmp(ea, eb) == 0,
               "a limited parse agrees on both paths");
        md_arena_destroy(out);
        md_document_destroy(a);
        md_document_destroy(b);
        if (in != NULL) {
            fclose(in);
        }
    }

    /* Invalid arguments are refused rather than dereferenced. */
    {
        expect(md_parse_stream(NULL, NULL) == NULL, "md_parse_stream(NULL) is refused");
        expect(md_parse_stream_in(NULL, NULL, 0u, NULL) == NULL,
               "md_parse_stream_in with no arena is refused");
        expect(md_error_to_json(NULL, NULL) == NULL,
               "md_error_to_json with no arena is refused");
    }

    tracking = 0;

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    return failures != 0;
}
