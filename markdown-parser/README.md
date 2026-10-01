# Markdown parser

A small CommonMark-flavored Markdown parser written in C11 (standard library
only). It parses a document into an arena-owned AST and can print that AST as
JSON, as HTML, or as plain text.

## Build

```sh
make          # builds ./md
make lib      # builds libmd.a (everything except the CLI)
make example  # builds ./example from examples/toc_example.c against libmd.a
make check    # builds ./md and runs include, regression, API/leak and
              # example tests
make clean
```

`make clean` works under both a POSIX shell and `cmd.exe`. On Windows it
runs `$(COMSPEC) /c del /f /q`, because `del` is a cmd.exe builtin and
therefore no program that make can start directly; cmd.exe is a real
executable, so it is what has to be named in the recipe. The compiler decides
whether an executable is called `md` or `md.exe`, so both spellings are
removed, and only files that were actually built are named at all.

`make check` runs the harnesses, which are POSIX shell scripts, so it needs a
POSIX shell. Override the name with `SH=...` if it is not called `sh`.

## CLI

```sh
md --ast FILE     # print the block AST as JSON on stdout ('-' reads stdin)
md --html FILE    # print the document as HTML on stdout ('-' reads stdin)
md --text FILE    # print the document as plain text ('-' reads stdin)
md --toc FILE     # print the table of contents as JSON on stdout
md2ast FILE       # a spelling of --ast
md2html FILE      # a spelling of --html
md --help
md --version
```

`md2ast` and `md2html` are spellings of `--ast` and `--html`, not new
modes: they select the same output and are byte-identical to the flag on
every document and on both read paths. See
[Checkpoint C6](#checkpoint-c6).

Exit status: **0** on success, **1** on I/O or parse error, **2** on usage
error. stdout always carries exactly one JSON document: the rendered document
on success, or the structured error document on a limit failure. A copy of
that error document goes to stderr as well, and every other diagnostic goes
to stderr alone. Exactly one mode and one file are required, and
`--flag=FILE` is accepted as well as `--flag FILE`.

`--ext CONFIG.json` enables the extensions the configuration names; see
[Checkpoint C4](#checkpoint-c4). It may appear before or after the mode and
`--ext=FILE` is accepted too. `--ext -` reads the configuration from stdin.
Without `--ext`, no extension is enabled and the output is byte-identical to
C1-C3.

`--ast` prints one JSON line. The document is followed by a single newline,
which the C runtime may terminate with CRLF on Windows; the JSON payload
itself is byte-exact, because every control character inside a string is
escaped. `--html` and `--text` print the rendered document, which ends with
one final newline when the document is not empty; an empty document produces
no output at all. All three modes read `-` as stdin, enforce the same input
size limit (`MD_MAX_INPUT_BYTES`, 256 MiB) and report the same errors.

```console
$ cat doc.md
# md

A *small* parser, see [the repo](/md).

- C11 only
- no dependencies

$ md --html doc.md
<h1>md</h1>
<p>A <em>small</em> parser, see <a href="/md">the repo</a>.</p>
<ul>
<li>C11 only</li>
<li>no dependencies</li>
</ul>

$ md --text doc.md
md

A small parser, see the repo.

  C11 only
  no dependencies
```

Only C11 standard headers and headers local to `src/` are used. `make check`
enforces this along with the regression tests.

## Library

```c
#include "md.h"

md_document *doc = md_parse(source);          /* blocks + inlines */
md_arena *arena = md_document_arena(doc);
char *json = md_ast_to_json(arena, md_document_root(doc));
char *html = md_render_html(arena, md_document_root(doc));
char *text = md_render_text(arena, md_document_root(doc));
md_document_destroy(doc);                      /* one call frees everything */

/* C1 block-only parsing is still available and unchanged. */
md_document *blocks = md_parse_blocks(source);
```

| entry point                        | result                                       |
| ---------------------------------- | -------------------------------------------- |
| `md_parse(src)`                    | full parse in an arena owned by the document  |
| `md_parse_n(src, len)`             | same, for a buffer of `len` bytes             |
| `md_parse_in(arena, src, len)`     | full parse in a caller-owned arena             |
| `md_parse_blocks*`                 | C1 block-only parse, same three shapes         |
| `md_parse_inlines(arena, src, len, parent)` | inline nodes appended to `parent`       |
| `md_render_html(arena, node)`      | HTML fragment, allocated in `arena`            |
| `md_render_text(arena, node)`      | plain text, allocated in `arena`               |

`md_ast_to_json()`, `md_render_html()` and `md_render_text()` all take an
arena and a node, and all return arena-owned, NUL-terminated memory. Rendering
into a caller-owned arena is the usual choice: the string outlives
`md_document_destroy()` as long as that arena does.

Node and string data live in the document's arena and stay valid until
`md_document_destroy()`. No function returns a pointer to temporary stack
data. A document from `md_parse_in()` leaves the caller's arena alive;
`md_document_destroy()` only releases what it owns.

## Checkpoint C1

Implemented: ATX headings (levels 1-6), setext headings, paragraphs, ordered
and unordered lists (nesting plus tight/loose state), fenced and indented
code blocks, block quotes (nesting, lazy continuation) and thematic breaks.
Tabs are expanded to four-space stops before parsing; CR/CRLF are normalized
to LF and NUL bytes become U+FFFD, so output is deterministic and UTF-8 safe.

JSON node shapes (keys always emitted, always in this order):

| node            | shape                                                                 |
| --------------- | -------------------------------------------------------------------- |
| `document`      | `{"type":"document","children":[...]}`                                 |
| `paragraph`     | `{"type":"paragraph","children":[...]}`                                |
| `heading`       | `{"type":"heading","level":N,"children":[...]}`                        |
| `list`          | `{"type":"list","ordered":B,"start":N,"tight":B,"children":[...]}`     |
| `item`          | `{"type":"item","children":[...]}`                                     |
| `blockquote`    | `{"type":"blockquote","children":[...]}`                              |
| `code_block`    | `{"type":"code_block","info":"...","literal":"...","children":[]}`     |
| `thematic_break`| `{"type":"thematic_break"}`                                             |
| `text`          | `{"type":"text","value":"..."}`                                        |

Inline syntax (emphasis, code spans, links) is parsed verbatim as text here;
it arrives in the next checkpoint. HTML and plaintext renderers, and syntax
extensions, are later checkpoints too.

## Checkpoint C2

`md_parse*()` adds the inline layer on top of the C1 block layer. The C1
entry points keep their block-only behavior, so `md --ast` uses the full
parser and every heading and paragraph child is an inline node.

Implemented: emphasis and strong emphasis (including nested delimiter runs),
inline links, full/collapsed/shortcut reference links, images, code spans,
backslash escapes, entity references (common names plus decimal and
hexadecimal numeric forms), and soft and hard line breaks.

JSON node shapes (keys always emitted, always in this order):

| node       | shape                                                                     |
| ---------- | ------------------------------------------------------------------------- |
| `em`       | `{"type":"em","children":[...]}`                                          |
| `strong`   | `{"type":"strong","children":[...]}`                                      |
| `code`     | `{"type":"code","value":"..."}`                                           |
| `link`     | `{"type":"link","destination":"...","title":"...","children":[...]}`       |
| `image`    | `{"type":"image","destination":"...","title":"...","children":[...]}`      |
| `softbreak`| `{"type":"softbreak"}`                                                     |
| `hardbreak`| `{"type":"hardbreak"}`                                                     |

Titles: `title` is always present on `link` and `image`. In the AST the field
is `NULL` when the source had no title; in JSON the key is `""` so the shape
stays fixed.

Reference definitions are collected for the whole document before inline
parsing, so a definition may appear after the references that use it.
Definitions inside fenced or indented code are ignored, labels are matched
case-insensitively with internal whitespace collapsed, labels longer than 999
bytes do not match, and the first definition of a label wins.

Limits: inline nesting stops at `MD_INLINE_MAX_NESTING` (64) levels and the
delimiter stack holds `MD_MAX_DELIMS` (64) entries. Delimiters past the bound
stay literal text, as in CommonMark's "stack is full" rule. Bare autolinks are
not part of this checkpoint and stay text.

## Checkpoint C3

Two renderers are added, plus the `md --html` and `md --text` modes. Neither
renderer changes parsing: the AST produced by C1 and C2 is rendered as is,
and `md --ast` output is byte-identical to C2.

### `md_render_html()`

```c
char *md_render_html(md_arena *arena, const md_node *node);
```

| node              | HTML                                                      |
| ----------------- | --------------------------------------------------------- |
| `document`        | its children, one block per line                          |
| `paragraph`       | `<p>...</p>`                                              |
| `heading`         | `<h1>`, `<h2>` ... `<h6>` by level                        |
| `list`            | `<ul>...</ul>`, or `<ol start="N">...</ol>`                |
| `item`            | `<li>...</li>`                                            |
| `blockquote`      | `<blockquote>...</blockquote>`                            |
| `code_block`      | `<pre><code class="language-INFO">...</code></pre>`        |
| `thematic_break`  | `<hr />`                                                  |
| `text`            | escaped text                                              |
| `em` / `strong`   | `<em>...</em>` / `<strong>...</strong>`                  |
| `code`            | `<code>escaped</code>`                                    |
| `link`            | `<a href="..." title="...">...</a>`                       |
| `image`           | `<img src="..." alt="..." title="..." />`                  |
| `softbreak`       | a newline                                                 |
| `hardbreak`       | `<br />`                                                  |

Escaping is the whole safety story, and it is done in one place: the only way
bytes reach the output is through an escape helper, so source text can never
introduce a tag, an attribute or an entity.

- text content escapes `&` as `&amp;`, `<` as `&lt;` and `>` as `&gt;`
- attribute values additionally escape `"` as `&quot;` and `'` as `&#39;`,
  so a destination or title can never leave its quotes
- a literal `<script>alert(1)</script>` in the source is rendered as
  `&lt;script&gt;alert(1)&lt;/script&gt;`, so no source text can become
  executable markup
- every other byte, including UTF-8 continuation bytes, is copied through
  unchanged, so output is UTF-8 preserving

`title` is emitted only when the source carried a title, matching the AST
where an absent title is `NULL`. The `class` attribute is emitted only when
the code block has an info string, and uses its first whitespace-delimited
word, as CommonMark does. A paragraph inside a **tight** list item is
rendered without its `<p>` wrapper, as in CommonMark. Every block ends with a
newline, so a non-empty document ends with one.

### `md_render_text()`

```c
char *md_render_text(md_arena *arena, const md_node *node);
```

Markup is dropped and the block structure survives as line structure: one
blank line between top-level blocks, one line per item in a tight list, a
blank line between items in a loose list, nested items indented by two spaces
per level, verbatim lines for code blocks, and one line per line for soft and
hard breaks. Emphasis, strong emphasis, links and code spans contribute their
text; an image contributes its alt text; a thematic break contributes
nothing. Content bytes are copied through unchanged, so the output is valid
UTF-8. The result never ends with a blank line: trailing newlines are trimmed
and exactly one final newline is appended when the document has content, and
an empty document renders to the empty string.


## Checkpoint C4

Extensions are opt-in. Every entry point that existed before C4
(`md_parse()`, `md_parse_n()`, `md_parse_inlines()`, `md_parse_blocks*()`) is
a wrapper that enables nothing, so their output is byte-identical to C3. The
new entry points take an `MDParseOptions` with a bitmask, and `--ext` reads
that bitmask from a JSON configuration.

```c
MDParseOptions opts;
md_options_init(&opts);                 /* nothing enabled, no registry */
opts.extensions = MD_EXT_TABLES | MD_EXT_STRIKETHROUGH;
md_document *doc = md_parse_opts(&opts, src);
md_document_destroy(doc);
```

`md_options_init()` sets `extensions` to `MD_EXT_NONE` and `custom` to `NULL`.
The bits are `MD_EXT_TABLES`, `MD_EXT_STRIKETHROUGH`, `MD_EXT_TASK_LIST`,
`MD_EXT_FOOTNOTES`, and `MD_EXT_BUILTIN_ALL` for all four.

| entry point                       | result                                   |
| --------------------------------- | ---------------------------------------- |
| `md_parse_opts*()`                | full parse with the extensions applied    |
| `md_parse_inlines_opts()`         | inline nodes with the extensions applied  |
| `md_options_from_json()`          | apply a configuration string to the options |
| `md_options_from_file()`          | the same, from a file; `"-"` reads stdin  |
| `md_ext_flag_name()` / `md_ext_flag_by_name()` | name to bit lookup        |
| `md_ext_registry_create()` / `md_ext_registry_destroy()` | own a set of handlers |

The options and any registry are **borrowed for the duration of the call**:
the document records the mask it was parsed with (`md_node.extensions`) so
`md_ast_to_json()` can tell an extended tree from an ordinary one, but it
copies no function pointer and owns neither. Release them with
`md_options_init()` and `md_ext_registry_destroy()` when you are done.

### Configuration files

`--ext CONFIG.json` takes an object whose members are extension names mapped
to booleans:

```json
{ "tables": true, "strikethrough": true, "task_lists": true, "footnotes": true }
```

- the enabled set is the **union of the keys that are true**, so it does not
  depend on the order they are written in
- `{}` enables nothing, and a member set to `false` contributes nothing, so a
  configuration can never turn an extension on by accident
- `"all"` turns on every built-in, `"none"` on none, and the aliases
  `tasklist`, `strike`, `strikeout` and `fn` are accepted
- the array form `{ "extensions": ["tables", "footnotes"] }` is still accepted
  and unions the same way
- an unknown name, a value that is neither a boolean nor an array, and a
  document that is not an object are all rejected with a diagnostic on
  stderr and exit status 1; so is a file that cannot be read
- `md --ext -` reads the configuration from stdin

```console
$ printf '%s' '{"tables": true, "footnotes": true}' > ext.json
$ md --html --ext ext.json doc.md
```

The configuration is loaded into a temporary arena that is released before
parsing starts; only the resulting bitmask survives into the parse.

### Tables

A table is a header line, a delimiter line, and zero or more body lines. A
line is only a delimiter row when every cell is `---`, `-`, `:--`, `--:` or
`:-:` with at least one dash; the colons give the per-column alignment. The
header and the delimiter row must have the same number of columns, otherwise
the block stays an ordinary paragraph — the same rule that keeps a pipe
paragraph from turning into a table by accident.

Leading and trailing pipes are optional and delimit rather than open a cell,
`\|` is a literal pipe, and a body row with fewer or more cells than the
header is padded or truncated to the header's width, so the table stays
rectangular.

A table may have at most eight columns. The delimiter row is validated into a
fixed eight-entry array that the block parser keeps on its stack, and a wider
row is therefore not a delimiter row, so the block stays a paragraph. The
bound is what keeps a candidate line free of allocation while a paragraph is
deciding whether a table interrupts it.

```console
$ cat t.md
| Left | Center | Right |
| :--- | :----: | ----: |
| a    | b      | c     |

$ md --ast --ext ext.json t.md
{"type":"document","children":[{"type":"table","align":["left","center","right"],
```

The AST is `table` holding `table_header` and `table_row`, each holding
`table_cell`; HTML is `<table>` with one `<thead>` and one `<tbody>`, and the
alignment is an `align` attribute on every `<th>` and `<td>`; plain text is
one line per row with cells separated by a single space.

### Strikethrough

`~~text~~` becomes a `strikethrough` node, rendered as `<del>` in HTML and as
bare `text` in plain text. The run may hold any inline content, including
`~~**b**~~` and a softbreak, and the closing `~~` is the first one that is not
inside a code span. An unpaired `~~` with no closer, and an immediately
closed `~~~~` with no content at all, stay literal text.

### Task lists

A list item whose content opens with `[ ]`, `[x]` or `[X]` followed by a
space, a tab or the end of the line is a task item. The item node carries
`checked`; the JSON reports `"checked":true` or `"checked":false`, and the
key is **absent** for an ordinary item, so a C1-C3 document is unchanged.
HTML renders `<input type="checkbox" disabled="" checked="" />` and plain
text renders `[x] ` or `[ ] ` before the item's text.

### Footnotes

A definition is a `[^label]:` line at a block start; its content is the rest
of the line plus any following indented, or blank-separated, block.
Definitions are collected on the **document node** in document order and
never appear in the block flow. A `[^label]` reference is only a reference
when a definition provides that label — the same rule the C2 reference links
already follow, so a dangling `[^x]` stays literal text. Labels are matched
the way reference labels are: case folded with internal whitespace
collapsed, so `[^Mixed   Case]` and `[^mixed case]` are one footnote.

```console
$ md --ast --ext ext.json f.md
{"type":"document","footnotes":[{"type":"footnote_def","id":"1","label":"1",
```

`footnotes` is a sibling of `children` in the document JSON, and it appears
only when the document was parsed with `MD_EXT_FOOTNOTES`; the two keys are
independent, so a document that has definitions still serializes its
`children`. In HTML a reference is
`<sup class="footnote-ref"><a href="#fn-ID" id="fnref-ID">label</a></sup>`
and the collected definitions follow the block flow inside a
`<section class="footnotes">`, each with a back link. `ID` is a slug of the
normalized id — letters, digits, `_` and `-` are kept and any other run
collapses to a single `-` — because a normalized label may hold characters
that are legal in an attribute but not in a fragment identifier. Plain text
writes `[^label]` for a reference and `[^id] text` for a definition.

### Registered extensions

A caller can add its own block or inline construct at runtime. A handler is
offered every position that neither the core parser nor a built-in extension
claimed, in registration order, and claims one by setting `*consumed`;
setting nothing leaves the text exactly as it was. A registered extension can
therefore never shadow core or built-in syntax — with the block handler below
registered, `>>> x` is still three block quotes, because the core block quote
claimed that line first.

A block handler additionally reports `*lines_used`, the number of lines it
took; the parser rejects a value of zero or one past the end of the block, so
a handler cannot skip a line it did not account for. It receives the whole
container's line array alongside the line it is at, so it can inspect or
reconstruct following lines.

```c
/* ==marked== becomes strong. This is the whole extension. */
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
            return MD_OK;              /* the marker does not cross a line */
        }
        if (text[end] == '=' && text[end + 1u] == '=') {
            break;
        }
    }
    if (end + 1u >= len || end == pos + 2u) {
        return MD_OK;                  /* unterminated, or empty */
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
        return status;                 /* propagate, do not relabel */
    }
    status = md_node_append(env->arena, parent, strong);
    if (status != MD_OK) {
        return status;
    }
    *consumed = end + 2u - pos;
    return MD_OK;
}

/* ... and to use it: */
md_arena *arena = md_arena_create();
md_ext_registry *registry = md_ext_registry_create(arena);
md_ext_register_inline(registry, "mark", mark_inline, NULL);

MDParseOptions opts;
md_options_init(&opts);
opts.custom = registry;                 /* the registered handlers */
md_document *doc = md_parse_opts(&opts, src);

md_ext_registry_destroy(registry);
md_arena_destroy(arena);
```

Without the registry the same document is unchanged: `a ==marked== b` stays
literal text, which is the property the C4 tests check. The matching block
handler is shorter, because it is handed the whole line:

```c
/* "!!! aside" becomes a block quote holding "aside". */
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
        return MD_OK;                  /* not ours; the paragraph takes it */
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
    *lines_used = 1;                   /* one line became one block */
    *consumed = 1;
    return MD_OK;
}

/* ... and to use it, alongside the inline handler above: */
md_arena *arena = md_arena_create();
md_ext_registry *registry = md_ext_registry_create(arena);
md_ext_register_inline(registry, "mark", mark_inline, NULL);
md_ext_register_block(registry, "note", note_block, NULL);

MDParseOptions opts;
md_options_init(&opts);
opts.custom = registry;                 /* both registered handlers */
md_document *doc = md_parse_opts(&opts, src);

md_ext_registry_destroy(registry);
md_arena_destroy(arena);
```

Both handlers in this section are the ones the C4 test suite runs, and
`tests/wrap_test.c` holds them verbatim.

`md_ext_register_block()` and `md_ext_register_inline()` return `MD_OK`, or
`MD_ERR_INVAL` for a null name, a null handler or a null registry, and
`MD_ERR_NOMEM` when the name cannot be copied. Re-registering a name
replaces the previous handler, so a configuration file and a caller-supplied
default cannot collide silently.

Ownership is explicit: `md_ext_registry_create()` returns a header and an
entry array that are plain heap allocations, so `md_ext_registry_destroy()`
releases both; the name strings are arena-owned and go away with the arena
the caller passed in. `md_ext_registry_block_count()` and
`md_ext_registry_inline_count()` report the registrations, so a caller can
check what it installed.

### Disabled syntax stays text

Every construct above is inert unless its bit is set: with no `--ext`, a pipe
table is a paragraph, `~~text~~` is text, `[x]` is text, and `[^x]` is text.
`md --ast --ext '{}' FILE` and `md --ast FILE` produce identical bytes.

## Checkpoint C5

Checkpoint C5 adds a streaming entry point, two configurable limits, and a
structured error document. Nothing that existed before moves: the same bytes
through the old and the new path produce the same AST, the same JSON and the
same rendered output.

### `md_parse_stream()`

```c
FILE *f = fopen("doc.md", "rb");
md_document *doc = md_parse_stream(f, NULL);   /* NULL = no extensions */
fclose(f);
/* ... use the document ... */
md_document_destroy(doc);
```

The stream is consumed forward in 64 KiB chunks and is never held whole, so
a large document costs one copy of itself rather than a copy plus the raw
bytes. The file is not closed and not rewound; a stream already positioned
part-way through parses exactly its remainder. `md_parse_stream_in()` is the
same thing against a caller-owned arena, with a `max_bytes` argument that
bounds the input that will be read (`0` selects `MD_MAX_INPUT_BYTES`).

Streamed and one-shot output is byte-identical, and that is a structural
property rather than a promise: `md_lines_read()` and `md_normalize()` feed
the same resumable normalizer, the normalized text goes through the same
`md_lines_split()`, and both then reach the same `parse_lines_in()`. There
is one normalizer, one line splitter and one block parser, so the two paths
cannot drift.

```console
$ md --ast doc.md > a.json
$ md --ast --stream doc.md > b.json
$ cmp a.json b.json && echo identical
identical
```

The chunk boundary is the interesting case, because it is where a streaming
reader usually diverges from a buffered one. The normalizer holds state
across chunk boundaries: a CR at the end of a chunk is kept pending until the
next byte decides whether it was a lone CR or half a CRLF, and tabs, NULs and
UTF-8 sequences are handled byte-wise so a multi-byte character split across
two reads survives intact.

```console
$ printf 'aaaa\rafter\n' | md --ast -          # CR at a chunk boundary
{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"aaaa"},{"type":"softbreak"},{"type":"text","value":"after"}]}]}
```

### Limits

`MDParseOptions` gained a `limits` member. Both fields are `0` in a default
option set, which means "use the library default", so an options value built
the C1-C4 way parses exactly as it always did.

| Field | Meaning | Default |
| --- | --- | --- |
| `limits.max_nesting` | deepest block nesting a document may reach | `MD_MAX_NESTING` (64) |
| `limits.max_bytes` | most memory one parse may take from its arena | 0, uncapped |

```c
MDParseOptions opts;
md_options_init(&opts);
opts.extensions   = MD_EXT_TABLES;
opts.limits.max_nesting = 8;         /* give up at eight levels */
opts.limits.max_bytes   = 1u << 20;   /* one MiB of parse memory */
md_document *doc = md_parse_opts_n(&opts, src, len);
```

`max_nesting` is clamped to `[1, MD_MAX_NESTING_HARD]`, and `MD_MAX_NESTING_HARD`
(256) can never be raised by any configuration. That clamp is what makes
"never overflow the C stack" a property of the library rather than of the
caller: the block parser descends one frame per nesting level, and no option
value can push it past the ceiling. Inline nesting has its own fixed bound,
`MD_INLINE_MAX_NESTING` (64), enforced at compile time.

`max_bytes` caps the arena. The document text is charged against it as well,
so a cap bounds a large document rather than just the tree built from it. The
stream reader checks the cap after every chunk, so a stream too large for the
cap is refused after one chunk of overshoot instead of after being read
whole.

Where each limit applies is not quite symmetrical, and the difference is
deliberate:

- `max_nesting` is enforced by **every** entry point, because refusing to
  descend costs nothing and the stack has to be bounded whatever the caller
  does with the arena.
- `max_bytes` is a property of the arena, so it takes effect on the entry
  points that **create** one — `md_parse()`, `md_parse_opts*()`, and
  `md_parse_stream()`, which build the arena with `md_arena_create_limit()`.
  On the `_in` forms the arena is the caller's, and what bounds memory is the
  cap that arena was created with:

  ```c
  md_arena *a = md_arena_create_limit(1u << 20);
  opts.limits.max_bytes = 0;   /* irrelevant here: `a` is the cap */
  md_document *doc = md_parse_opts_in(a, &opts, src, len);
  ```

  Checking the option afterwards instead would only report an overrun once
  the memory had already been taken, so it would bound nothing while
  appearing to. `md_limits_bytes()` still reports what the option set asked
  for, and that is the value the library's own entry points will use.

Two further details worth stating, because both are easy to get wrong:

- The cap is charged against the **normalized** text, not the raw bytes.
  Normalization both grows and shrinks its input — a tab becomes up to four
  spaces and a NUL becomes three bytes, but a CRLF pair collapses to one — so
  a cap between the two sizes accepts the document. In the parse path the
  per-line span array costs more than a CRLF ever saves, so the difference
  shows up in `md_normalize()` itself rather than in a whole-document parse.
- The raw input bound `md_parse_stream_in()` takes is a separate parameter and
  a separate rule: it bounds *bytes read*, not memory held. `md_parse_stream()`
  passes `0` there and lets the library's own `MD_MAX_INPUT_BYTES` bound the
  read, which is what keeps the two paths answering identically.

### Structured errors

Reaching a limit is a reported failure, not a crash and not a bare `NULL`:

```console
$ md --ast --max-nesting 4 deep.md
$ echo "exit $?"
exit 1
```

with, on **stderr**:

```json
{"error":{"line":41,"col":6,"message":"nesting too deep"}}
```

The three keys are always present and always in that order. `line` and `col`
are 1-based and name where the document asked for one level too many; both
are `0` when the failure is not tied to a place in the input, as for an
unreadable file. The message is escaped like any other JSON string.

The structured error goes to **stdout** and is copied to **stderr**; the exit
status is **1**, the same as any other parse error.

stdout carries the error document rather than staying empty so that a
consumer parsing stdout always receives one JSON document, whatever the
outcome, and can tell success from failure by the exit status alone without
having to merge a second stream. The stderr copy is there for a consumer
that keeps a separate human-readable failure stream, so the two always hold
the same bytes. A usage error has no machine-readable form: it is reported on
stderr with a usage message, and stdout stays empty.

`md_last_error()` exposes the same record to a caller:

```c
MDParseOptions opts;
md_options_init(&opts);
opts.limits.max_nesting = 2;
md_document *doc = md_parse_opts(&opts, ">>>> deep\n");
if (doc == NULL) {
    const md_error *err = md_last_error();
    /* err->kind == MD_ERROR_NESTING
     * err->line, err->col == 1, 4
     * err->message == "nesting too deep" */
    md_arena *tmp = md_arena_create();
    char *json = md_error_to_json(tmp, err);
    /* {"error":{"line":1,"col":4,"message":"nesting too deep"}} */
    md_arena_destroy(tmp);
}
```

`md_error_to_json()` is arena-owned and NUL-terminated like every other
string this library returns, and a `NULL` record argument means the same as
`NULL md_last_error()`. `md_error_message()` still returns exactly the text
it always did, so C1-C4 callers see no change.

### CLI

Three flags select the streaming path and the two ceilings:

```console
$ md --ast --stream FILE             # read the input as a stream
$ md --ast --max-nesting 8 FILE      # reject nesting deeper than 8
$ md --ast --max-bytes 1048576 FILE  # give up after 1 MiB of parse memory
```

They may appear before or after the mode, and `--max-nesting=N` and
`--max-bytes=N` are accepted as well. `--max-bytes 0` and no flag at all both
mean "no memory cap"; a `--max-nesting` beyond `MD_MAX_NESTING_HARD` is
clamped rather than refused. A repeated flag, a missing value, or a value
that is not a plain decimal count is a **usage** error, exit status **2**,
with stdout empty — the same treatment `--ext` already had.

```console
$ md --ast --max-nesting; echo "exit $?"
md: --max-nesting requires a value
exit 2

$ md --ast --max-nesting abc FILE; echo "exit $?"
md: --max-nesting wants a count, got abc
exit 2
```

A value that starts with `-` is treated as the next flag rather than as the
count, so `md --ast --max-nesting --ast FILE` is the "requires a value" case
rather than a count of `-1`.

### What C5 did not change

The default limits are the C1-C4 limits, so `md --ast FILE` and
`md --ast --stream FILE` reject the same documents they always did, and the
`--stream` output of a document the parser accepts is byte-identical to the
buffered output. The only deliberate difference in observable output is the
wording of a nesting or memory diagnostic, which is now a structured JSON
document on stderr instead of a plain sentence.

Two bugs that predate C5 turned up while checking that claim, and both are
fixed here:

- A run of unmatched delimiters longer than eight characters overflowed a
  fixed buffer in the inline parser and failed the whole parse, so
  `*********``  was a fatal error rather than nine asterisks followed by a
  backtick. The run is now built in the arena, where the memory cap is the
  only limit, and a 5000-character run is ordinary text.
- `--ext` only parsed when it came *before* the mode. `md --text --ext
  cfg.json FILE` silently took `--ext` as the filename and then rejected the
  configuration as an unknown argument. Every modifier is now order
  independent.

### Verifying the claim

`make check` is the suite that has to pass. Three further targets need
`python3` and are not part of it, because they take seconds rather than
milliseconds:

```console
$ make sweep        # streamed vs buffered over random documents
$ make json-check   # every run is one canonical valid JSON document
$ make diff         # this tree vs a pre-C5 baseline binary
```

`make sweep` builds documents out of constructs that stress the normalizer
(CR, CRLF, tabs, NULs, UTF-8), the block parser (nesting, fences,
containers) and the inline parser (delimiters, brackets, code spans), then
requires the two paths to agree byte for byte on every mode and extension
mask, with no crash, no unexpected exit status, and no diagnostic on a
successful run.

`make diff` needs a baseline built from the previous checkpoint and compares
the two on the same documents. It is only meaningful for the extension-free
contract: the committed C4 candidate mis-parses every extension
configuration it is given — it enables nothing for `{"all": true}`, and it
emits syntactically invalid JSON for `{"extensions": [...]}` — so it cannot
serve as a reference for extension behaviour. It is still a valid reference
for documents with no extension enabled, and over 2500 random documents the
stdout and the exit status do not move at all. Where the two differ, the
difference is always the baseline refusing a document the current tree
accepts, which is the delimiter-run bug above.

## Checkpoint C6

Checkpoint C6 adds two CLI subcommands, a table-of-contents output mode, and
a public library entry point behind it. Nothing that existed before moves:
`md2ast` and `md2html` are spellings of modes that were already there, and
`--toc` is a fourth output mode rather than a modifier, so no invocation that
worked in C5 produces a different byte now.

### Subcommands

```console
$ md2ast doc.md | cmp - <(md --ast doc.md) && echo identical
identical

$ md2html doc.md | cmp - <(md --html doc.md) && echo identical
identical
```

A subcommand may stand anywhere the mode flag may: first, after a modifier,
or before the file. `md2ast --stream FILE`, `--stream md2ast FILE` and
`md2ast FILE --stream` all parse the same document, and a subcommand is never
mistaken for the file argument.

| command   | same as  | output |
| --------- | -------- | ------ |
| `md2ast`  | `--ast`  | the block AST as JSON |
| `md2html` | `--html` | the rendered document as HTML |

`md2text` is deliberately absent: `--text` has no subcommand spelling, so
there is nothing for it to disambiguate. Adding one later is a one-line
change, and the C6 tests pin the list so it cannot appear by accident.

### `--toc`

`--toc` prints the headings of a document as one JSON object on one line:

```console
$ cat t.md
# Getting started

Some *intro* text.

## Install

### From source

## Install

$ md --toc t.md
[{"level":1,"text":"Getting started","anchor":"getting-started"},{"level":2,"text":"Install","anchor":"install"},{"level":3,"text":"From source","anchor":"from-source"},{"level":2,"text":"Install","anchor":"install-1"}]
```

The top level is the array of entries itself, with no wrapper object around
it, and the key order inside each entry is part of the contract, the same way
it is for the AST:

```
[{"level":N,"text":"...","anchor":"..."}]
```

| key      | type   | meaning |
| -------- | ------ | ------- |
| `level`  | number | the heading level, 1 to 6 |
| `text`   | string | the heading's plain text, inline markup removed |
| `anchor` | string | the anchor id, unique within the document |

- the top level is a list of entries, never an object wrapping one
- exactly three keys per entry, always in the order `level`, `text`, `anchor`
- a document with no headings is `[]`, never an empty string and never a
  wrapper holding `null`
- one line, one trailing newline, no pretty printing, so a consumer can read
  it without a streaming JSON parser
- deterministic: the same bytes always produce the same bytes, and the
  `--stream` and buffered reads agree

The output is JSON like every other mode, so `--toc` composes with the rest
of the contract. It reads `-` as stdin, it takes `--ext`, it takes
`--stream`, `--max-nesting` and `--max-bytes`, and a limit failure is the
same structured error document on both streams with exit status 1.

```console
$ printf '# Title\n\nSome text.\n\n> > > > quoted\n' > deep.md
$ md --toc --max-nesting 3 deep.md; echo "exit $?"
{"error":{"line":5,"col":9,"message":"nesting too deep"}}
exit 1
```

A document parsed without extensions produces exactly what it always did, so
`--toc` is useful before any `--ext` work and needs no configuration.

#### What counts as a heading

Every heading in the document, in document order, wherever it sits in the
block tree. A heading inside a block quote or a list item is a heading of the
document and is listed at its own level. A `#` line inside a fenced or
indented code block is code, not a heading, and a link reference definition
is not a heading either. Setext headings are included at the level their
underline selects.

Footnote definitions are the one deliberate exclusion. They are stored
outside the walked tree, on the document node, so a `[^1]: # not a heading`
line contributes no entry: a table of contents describes the document, and a
definition is a citation rather than a part of it.

#### `text`

The heading's text with inline markup removed and entities decoded:

```console
$ printf '## A *b* `c` <d> &amp; e\n' | md --toc -
[{"level":2,"text":"A b c <d> & e","anchor":"a-b-c-d-e"}]
```

The angle brackets are the consumer's to escape; the field carries content,
not markup.

#### `anchor`

The anchor id for the heading, derived byte-wise and ASCII-only, so the same
document produces the same anchors on every machine regardless of locale:

- ASCII letters and digits, `_` and `-` are kept
- any other run of bytes collapses to a single `-`
- the result is lowercased
- leading and trailing hyphens are trimmed
- a heading that slugs to nothing at all, `### !!!`, is called `section`

This is the rule the HTML renderer already used for footnote ids, so both
features name a heading the same way. It lives in one place,
`md_slug_write()` in `src/slug.c`, and the C6 refactor made the renderer call
that function instead of keeping a second copy of the rule.

Uniqueness is part of the contract, because a repeated anchor resolves to the
wrong heading. A heading that slugs to a string already in use gets `-1`
appended, then `-2`, and so on:

```console
$ printf '## Title\n## Title\n## Title 1\n## Title\n' | md --toc -
[{"level":2,"text":"Title","anchor":"title"},{"level":2,"text":"Title","anchor":"title-1"},{"level":2,"text":"Title 1","anchor":"title-1-1"},{"level":2,"text":"Title","anchor":"title-2"}]
```

The third heading is the interesting one. It slugifies to `title-1` on its
own, which the second heading has already taken, so it becomes `title-1-1`
instead of colliding silently. No input can produce two entries with the same
anchor, and the suffixes are handed out in document order, so the table is
reproducible.

### The library entry point

```c
md_status md_toc_build(md_arena *arena, const md_node *root, md_toc *out);
char *md_toc_to_json(md_arena *arena, const md_toc *toc);
```

```c
md_document *doc = md_parse(source);
md_arena *arena = md_document_arena(doc);
md_toc toc;

if (md_toc_build(arena, md_document_root(doc), &toc) == MD_OK) {
    char *json = md_toc_to_json(arena, &toc);
    puts(json);                    /* the same bytes `md --toc` writes */
}
md_document_destroy(doc);
```

Both results are allocated in the caller's arena and are released with it.
The entry array and both strings belong to the document's lifetime, so there
is nothing to free individually and nothing to copy out. `md_toc` and
`md_toc_entry` are declared in `src/toc.h`, which `md.h` includes, so a
caller needs no other header.

| field | meaning |
| ----- | ------- |
| `toc.entries` | the array, or `NULL` when `count` is 0 |
| `toc.count` | how many headings the document has |
| `entry.level` | heading level, 1 to 6 |
| `entry.text` | the plain-text heading content |
| `entry.anchor` | the unique anchor |

`md_toc_build()` returns `MD_OK`, or `MD_ERR_INVAL` for a NULL arena or root
and `MD_ERR_NOMEM` when the entries cannot be allocated. `md_toc_to_json()`
returns `NULL` on failure with a diagnostic recorded, like every other
string-returning function here.

### The example

`examples/toc_example.c` is a standalone program built against the archive.
It includes nothing but `md.h` and links against `libmd.a` with no other
object file and no other source from `src/`, so building it is the proof
that `md.h` is a public header and that a caller needs nothing else from the
library tree.

```console
$ make example
$ ./example
[{"level":1,"text":"Markdown in ten minutes","anchor":"markdown-in-ten-minutes"},{"level":2,"text":"Inline things","anchor":"inline-things"},{"level":3,"text":"Code","anchor":"code"},{"level":2,"text":"Inline things","anchor":"inline-things-1"}]

$ printf '# Release notes\n\n## 1.0\n' > r.md
$ ./example r.md
Release notes -> #release-notes
  1.0 -> #1-0

$ ./example - < r.md
Release notes -> #release-notes
  1.0 -> #1-0
```
With no argument it reads its built-in document and prints the JSON table of
contents; given a file, or `-` for stdin, it prints an indented outline
instead. Its exit status follows the CLI's contract: 0 on success, 1 for a
file it cannot read, 2 for a usage error.

The example follows the ownership order the library requires: parse, use the
result while the document's arena is alive, then `md_document_destroy()`.
Everything it prints is borrowed from that arena, so the document is
destroyed last and nothing derived from it is used afterwards.

`make check` builds and runs the example as part of the suite, and checks
that the anchors it prints are the ones `md --toc` prints for the same
document, since both are the same library call.

### Verifying the claim

`make check` runs the include policy check, the C1-C5 regression suite, the
C6 section, the public API and leak suite, the internal lexer suite, and the
example suite. The optional randomized targets `make sweep`, `make json-check`
and `make diff` are described in the C5 section.

The `--toc` contract is checked by `tests/check_toc.py`, which parses a
`--toc` document and verifies that the top level is a list, the key set, the
key order, the level range, the anchor rule and the uniqueness this chapter
promises, rather than comparing the bytes against a literal. It also rejects
a repeated key, which a plain JSON parser hides by keeping the last one. The shell suite still pins the exact bytes for
the cases where the exact bytes are the contract.
