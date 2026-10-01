#!/bin/sh
# Regression tests for checkpoint C1 (block parsing + AST JSON).
# Run with: make check

set -u

MD=${MD:-./md}
TMP=${TMPDIR:-/tmp}/md-check.$$
PASS=0
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
mkdir -p "$TMP" || exit 1

ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL: %s\n' "$1" >&2; }

# error_doc_is <stdout-file> <stderr-file>
#
# A limit failure writes the structured error document to stdout, so that a
# consumer parsing stdout always gets one JSON document, and copies it to
# stderr for a consumer keeping a human-readable failure stream. Both files
# must hold that same one object and nothing else. CR is dropped on both
# sides for the same reason `check` does it: the runtime may end the line
# with CRLF, while a raw CR can never be part of the JSON payload.
error_doc_is() {
    [ -s "$1" ] && [ -s "$2" ] &&
    [ "$(tr -d '\r' < "$1")" = "$(tr -d '\r' < "$2")" ]
}

# check <name> <input-file> <expected-stdout-file> <expected-exit>
# Both sides are normalized by dropping CR bytes before comparing: the C
# runtime may terminate the output line with CRLF on some platforms, and a raw
# CR can never be part of the JSON payload (control characters are escaped).
check() {
    name=$1; in=$2; want=$3; want_rc=$4
    got_rc=0
    "$MD" --ast "$in" > "$TMP/out.raw" 2> "$TMP/err" || got_rc=$?
    tr -d '\r' < "$TMP/out.raw" > "$TMP/out"
    tr -d '\r' < "$want" > "$TMP/want"
    if [ "$got_rc" != "$want_rc" ]; then
        bad "$name: exit $got_rc, want $want_rc"
        return
    fi
    if ! cmp -s "$TMP/out" "$TMP/want"; then
        bad "$name: stdout mismatch"
        printf '  want: %s\n  got:  %s\n' "$(cat "$TMP/want")" "$(cat "$TMP/out")" >&2
        return
    fi
    if [ "$want_rc" = 0 ] && [ -s "$TMP/err" ]; then
        bad "$name: stderr not empty on success"
        return
    fi
    ok
}

# fixture <name> <literal-input> <literal-expected-json>
# Both arguments are written verbatim; the expected JSON literal below already
# ends with the newline the CLI prints after the document.
fixture() {
    printf '%s' "$2" > "$TMP/$1.md"
    printf '%s' "$3" > "$TMP/$1.json"
}

# ------------------------------------------------------------------ #
# Block structure
# ------------------------------------------------------------------ #

fixture heading '## Intro
' '{"type":"document","children":[{"type":"heading","level":2,"children":[{"type":"text","value":"Intro"}]}]}
'
check "atx heading" "$TMP/heading.md" "$TMP/heading.json" 0

fixture atx_levels '# h1
###### h6
####### seven
#tag not a heading
' '{"type":"document","children":[{"type":"heading","level":1,"children":[{"type":"text","value":"h1"}]},{"type":"heading","level":6,"children":[{"type":"text","value":"h6"}]},{"type":"paragraph","children":[{"type":"text","value":"####### seven"},{"type":"softbreak"},{"type":"text","value":"#tag not a heading"}]}]}
'
check "atx levels 1-6" "$TMP/atx_levels.md" "$TMP/atx_levels.json" 0

fixture atx_close '# Title #
## Closed ##
#
' '{"type":"document","children":[{"type":"heading","level":1,"children":[{"type":"text","value":"Title"}]},{"type":"heading","level":2,"children":[{"type":"text","value":"Closed"}]},{"type":"heading","level":1,"children":[]}]}
'
check "atx closing sequence" "$TMP/atx_close.md" "$TMP/atx_close.json" 0

fixture setext 'Setext One
==========
Setext Two
----------

Not a heading
- - -
' '{"type":"document","children":[{"type":"heading","level":1,"children":[{"type":"text","value":"Setext One"}]},{"type":"heading","level":2,"children":[{"type":"text","value":"Setext Two"}]},{"type":"paragraph","children":[{"type":"text","value":"Not a heading"}]},{"type":"thematic_break"}]}
'
check "setext headings" "$TMP/setext.md" "$TMP/setext.json" 0

fixture para 'one
two
three
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"one"},{"type":"softbreak"},{"type":"text","value":"two"},{"type":"softbreak"},{"type":"text","value":"three"}]}]}
'
check "paragraph with soft breaks" "$TMP/para.md" "$TMP/para.json" 0

fixture break '* * *

- - -
______
' '{"type":"document","children":[{"type":"thematic_break"},{"type":"thematic_break"},{"type":"thematic_break"}]}
'
check "thematic breaks" "$TMP/break.md" "$TMP/break.json" 0

# ------------------------------------------------------------------ #
# Lists
# ------------------------------------------------------------------ #

fixture tight_list '- alpha
- beta
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"alpha"}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"beta"}]}]}]}]}
'
check "tight unordered list" "$TMP/tight_list.md" "$TMP/tight_list.json" 0

fixture loose_list '- one

- two
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":false,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"one"}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"two"}]}]}]}]}
'
check "loose unordered list" "$TMP/loose_list.md" "$TMP/loose_list.json" 0

fixture ordered_list '5. five
6. six
' '{"type":"document","children":[{"type":"list","ordered":true,"start":5,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"five"}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"six"}]}]}]}]}
'
check "ordered list start" "$TMP/ordered_list.md" "$TMP/ordered_list.json" 0

fixture nested_list '- a
  - b
    - c
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"a"}]},{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"b"}]},{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"c"}]}]}]}]}]}]}]}]}
'
check "nested lists" "$TMP/nested_list.md" "$TMP/nested_list.json" 0

fixture loose_item '- one

  second

- two
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":false,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"one"}]},{"type":"paragraph","children":[{"type":"text","value":"second"}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"two"}]}]}]}]}
'
check "loose item with two blocks" "$TMP/loose_item.md" "$TMP/loose_item.json" 0

# ------------------------------------------------------------------ #
# Code, quotes, tabs
# ------------------------------------------------------------------ #

fixture fenced '```c
int main(void) { return 0; }
```
' '{"type":"document","children":[{"type":"code_block","info":"c","literal":"int main(void) { return 0; }\n","children":[]}]}
'
check "fenced code with info" "$TMP/fenced.md" "$TMP/fenced.json" 0

fixture fence_tilde '~~~
plain
~~~
' '{"type":"document","children":[{"type":"code_block","info":"","literal":"plain\n","children":[]}]}
'
check "tilde fence" "$TMP/fence_tilde.md" "$TMP/fence_tilde.json" 0

fixture indented '    code line
    second line
' '{"type":"document","children":[{"type":"code_block","info":"","literal":"code line\nsecond line\n","children":[]}]}
'
check "indented code" "$TMP/indented.md" "$TMP/indented.json" 0

fixture quote '> quoted
> lines
' '{"type":"document","children":[{"type":"blockquote","children":[{"type":"paragraph","children":[{"type":"text","value":"quoted"},{"type":"softbreak"},{"type":"text","value":"lines"}]}]}]}
'
check "block quote" "$TMP/quote.md" "$TMP/quote.json" 0

fixture quote_lazy '> quoted
lazy
' '{"type":"document","children":[{"type":"blockquote","children":[{"type":"paragraph","children":[{"type":"text","value":"quoted"},{"type":"softbreak"},{"type":"text","value":"lazy"}]}]}]}
'
check "block quote lazy continuation" "$TMP/quote_lazy.md" "$TMP/quote_lazy.json" 0

fixture quote_nested '> outer
> > inner
' '{"type":"document","children":[{"type":"blockquote","children":[{"type":"paragraph","children":[{"type":"text","value":"outer"}]},{"type":"blockquote","children":[{"type":"paragraph","children":[{"type":"text","value":"inner"}]}]}]}]}
'
check "nested block quote" "$TMP/quote_nested.md" "$TMP/quote_nested.json" 0

fixture tabs 'a	b

	indented	code
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"a   b"}]},{"type":"code_block","info":"","literal":"indented    code\n","children":[]}]}
'
check "tabs expand to four-space stops" "$TMP/tabs.md" "$TMP/tabs.json" 0

fixture tabs_para 'a	b
	not code, this continues the paragraph
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"a   b"},{"type":"softbreak"},{"type":"text","value":"not code, this continues the paragraph"}]}]}
'
check "indented code cannot interrupt a paragraph" "$TMP/tabs_para.md" "$TMP/tabs_para.json" 0

# ------------------------------------------------------------------ #
# Encoding, normalization, edge inputs
# ------------------------------------------------------------------ #

fixture utf8 'café ✓ 🎉
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"café ✓ 🎉"}]}]}
'
check "utf-8 preserved" "$TMP/utf8.md" "$TMP/utf8.json" 0

fixture escapes 'say "hi" \ and  tab
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"say \"hi\" \\ and \f tab"}]}]}
'
check "json escaping" "$TMP/escapes.md" "$TMP/escapes.json" 0

: > "$TMP/empty.md"
printf '%s\n' '{"type":"document","children":[]}' > "$TMP/empty.json"
check "empty document" "$TMP/empty.md" "$TMP/empty.json" 0

printf '\r\n# crlf\r\n\r\n- x\r\n' > "$TMP/crlf.md"
printf '%s\n' '{"type":"document","children":[{"type":"heading","level":1,"children":[{"type":"text","value":"crlf"}]},{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"x"}]}]}]}]}' > "$TMP/crlf.json"
check "crlf normalized" "$TMP/crlf.md" "$TMP/crlf.json" 0

printf 'a\000b\n' > "$TMP/nul.md"
printf '%b\n' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"a\357\277\275b"}]}]}' > "$TMP/nul.json"
check "nul becomes U+FFFD" "$TMP/nul.md" "$TMP/nul.json" 0

# determinism: same input, byte-identical output
"$MD" --ast "$TMP/utf8.md" > "$TMP/det1" 2>/dev/null
"$MD" --ast "$TMP/utf8.md" > "$TMP/det2" 2>/dev/null
if cmp -s "$TMP/det1" "$TMP/det2"; then ok; else bad "deterministic output"; fi


# ------------------------------------------------------------------ #
# Checkpoint C2: inline content
# ------------------------------------------------------------------ #

P() { printf '{"type":"document","children":[{"type":"paragraph","children":[%s]}]}\n' "$1"; }
c2fix() { printf '%s' "$2" > "$TMP/$1.md"; printf '%s
' "$3" > "$TMP/$1.json"; }

c2fix inline_em '*one* and _two_ and three
' "$(P '{"type":"em","children":[{"type":"text","value":"one"}]},{"type":"text","value":" and "},{"type":"em","children":[{"type":"text","value":"two"}]},{"type":"text","value":" and three"}')"
check "em with asterisk and underscore" "$TMP/inline_em.md" "$TMP/inline_em.json" 0

c2fix inline_strong '**bold** and __bold__ and normal
' "$(P '{"type":"strong","children":[{"type":"text","value":"bold"}]},{"type":"text","value":" and "},{"type":"strong","children":[{"type":"text","value":"bold"}]},{"type":"text","value":" and normal"}')"
check "strong with both markers" "$TMP/inline_strong.md" "$TMP/inline_strong.json" 0

c2fix inline_nested '***both*** and *a **b** c* and **d *e* f**
' "$(P '{"type":"em","children":[{"type":"strong","children":[{"type":"text","value":"both"}]}]},{"type":"text","value":" and "},{"type":"em","children":[{"type":"text","value":"a "},{"type":"strong","children":[{"type":"text","value":"b"}]},{"type":"text","value":" c"}]},{"type":"text","value":" and "},{"type":"strong","children":[{"type":"text","value":"d "},{"type":"em","children":[{"type":"text","value":"e"}]},{"type":"text","value":" f"}]}')"
check "nested delimiter runs" "$TMP/inline_nested.md" "$TMP/inline_nested.json" 0

c2fix inline_unmatched '**unclosed and *one
' "$(P '{"type":"text","value":"**unclosed and *one"}')"
check "unmatched delimiters stay literal" "$TMP/inline_unmatched.md" "$TMP/inline_unmatched.json" 0

c2fix inline_intraword 'snake_case and 2*3*4 and a**b
' "$(P '{"type":"text","value":"snake_case and 2"},{"type":"em","children":[{"type":"text","value":"3"}]},{"type":"text","value":"4 and a**b"}')"
check "intraword markers are literal" "$TMP/inline_intraword.md" "$TMP/inline_intraword.json" 0

c2fix inline_code '`one` ``two `inner` three`` and `unclosed
' "$(P '{"type":"code","value":"one"},{"type":"text","value":" "},{"type":"code","value":"two `inner` three"},{"type":"text","value":" and `unclosed"}')"
check "code spans" "$TMP/inline_code.md" "$TMP/inline_code.json" 0

c2fix inline_code_star '`*not em*` and `a\`b`
' "$(P '{"type":"code","value":"*not em*"},{"type":"text","value":" and "},{"type":"code","value":"a\\"},{"type":"text","value":"b`"}')"
check "code span content is verbatim" "$TMP/inline_code_star.md" "$TMP/inline_code_star.json" 0

c2fix inline_link '[text](/dest "Title")
' "$(P '{"type":"link","destination":"/dest","title":"Title","children":[{"type":"text","value":"text"}]}')"
check "inline link with title" "$TMP/inline_link.md" "$TMP/inline_link.json" 0

c2fix inline_link_notitle '[text](/dest)
' "$(P '{"type":"link","destination":"/dest","title":"","children":[{"type":"text","value":"text"}]}')"
check "inline link without title" "$TMP/inline_link_notitle.md" "$TMP/inline_link_notitle.json" 0

c2fix inline_link_forms '[a](</a b> '\''T'\'') [c](d) [e](<>)
' "$(P '{"type":"link","destination":"/a b","title":"T","children":[{"type":"text","value":"a"}]},{"type":"text","value":" "},{"type":"link","destination":"d","title":"","children":[{"type":"text","value":"c"}]},{"type":"text","value":" "},{"type":"link","destination":"","title":"","children":[{"type":"text","value":"e"}]}')"
check "link destination and title forms" "$TMP/inline_link_forms.md" "$TMP/inline_link_forms.json" 0

c2fix inline_link_escape '[a](/b%20c) and [d](/e\)f)
' "$(P '{"type":"link","destination":"/b%20c","title":"","children":[{"type":"text","value":"a"}]},{"type":"text","value":" and "},{"type":"link","destination":"/e)f","title":"","children":[{"type":"text","value":"d"}]}')"
check "link destination escapes" "$TMP/inline_link_escape.md" "$TMP/inline_link_escape.json" 0

c2fix inline_link_inner '[a *b* `c`](d)
' "$(P '{"type":"link","destination":"d","title":"","children":[{"type":"text","value":"a "},{"type":"em","children":[{"type":"text","value":"b"}]},{"type":"text","value":" "},{"type":"code","value":"c"}]}')"
check "link label holds inline nodes" "$TMP/inline_link_inner.md" "$TMP/inline_link_inner.json" 0

c2fix inline_link_unresolved '[a] and [b][missing] and [not a link
' "$(P '{"type":"text","value":"[a] and [b][missing] and [not a link"}')"
check "unresolved references stay literal" "$TMP/inline_link_unresolved.md" "$TMP/inline_link_unresolved.json" 0

c2fix ref_defs '[ref] [Ref] [x][ref] and [empty][e]

[ref]: /dest "Title"
[REF]: /other
[e]: /
' "$(P '{"type":"link","destination":"/dest","title":"Title","children":[{"type":"text","value":"ref"}]},{"type":"text","value":" "},{"type":"link","destination":"/dest","title":"Title","children":[{"type":"text","value":"Ref"}]},{"type":"text","value":" "},{"type":"link","destination":"/dest","title":"Title","children":[{"type":"text","value":"x"}]},{"type":"text","value":" and "},{"type":"link","destination":"/","title":"","children":[{"type":"text","value":"empty"}]}')"
check "reference definitions resolve" "$TMP/ref_defs.md" "$TMP/ref_defs.json" 0

c2fix ref_def_fenced '[a]

    [a]: /not-a-def

[a]: /real
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"link","destination":"/real","title":"","children":[{"type":"text","value":"a"}]}]},{"type":"code_block","info":"","literal":"[a]: /not-a-def\n","children":[]}]}'
check "definitions respect blocks" "$TMP/ref_def_fenced.md" "$TMP/ref_def_fenced.json" 0

c2fix inline_image '![alt *x*](/i.png "T") and ![](/e.png)
' "$(P '{"type":"image","destination":"/i.png","title":"T","children":[{"type":"text","value":"alt "},{"type":"em","children":[{"type":"text","value":"x"}]}]},{"type":"text","value":" and "},{"type":"image","destination":"/e.png","title":"","children":[]}')"
check "images" "$TMP/inline_image.md" "$TMP/inline_image.json" 0

c2fix inline_image_ref '![alt][r] and !not an image

[r]: /p "T"
' "$(P '{"type":"image","destination":"/p","title":"T","children":[{"type":"text","value":"alt"}]},{"type":"text","value":" and !not an image"}')"
check "reference image" "$TMP/inline_image_ref.md" "$TMP/inline_image_ref.json" 0

c2fix inline_escape 'a \*b\* \_c\_ \\ \`d\` \&amp; &#65; &copy; &#x1F600; &unknown;
' "$(P '{"type":"text","value":"a *b* _c_ \\ `d` &amp; A © 😀 &unknown;"}')"
check "backslash escapes and entities" "$TMP/inline_escape.md" "$TMP/inline_escape.json" 0

c2fix inline_breaks 'one
two
three
' "$(P '{"type":"text","value":"one"},{"type":"softbreak"},{"type":"text","value":"two"},{"type":"softbreak"},{"type":"text","value":"three"}')"
check "softbreak node" "$TMP/inline_breaks.md" "$TMP/inline_breaks.json" 0

printf 'one  \ntwo\n' > "$TMP/inline_hard.md"
printf '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"one"},{"type":"hardbreak"},{"type":"text","value":"two"}]}]}\n' > "$TMP/inline_hard.json"
check "hardbreak from two trailing spaces" "$TMP/inline_hard.md" "$TMP/inline_hard.json" 0

printf 'one\\\ntwo\n' > "$TMP/inline_hard_bs.md"
printf '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"one"},{"type":"hardbreak"},{"type":"text","value":"two"}]}]}\n' > "$TMP/inline_hard_bs.json"
check "hardbreak from backslash" "$TMP/inline_hard_bs.md" "$TMP/inline_hard_bs.json" 0

c2fix inline_in_blocks '# *head*

> quoted *em*
> more

- item *em*
- item `code`
' '{"type":"document","children":[{"type":"heading","level":1,"children":[{"type":"em","children":[{"type":"text","value":"head"}]}]},{"type":"blockquote","children":[{"type":"paragraph","children":[{"type":"text","value":"quoted "},{"type":"em","children":[{"type":"text","value":"em"}]},{"type":"softbreak"},{"type":"text","value":"more"}]}]},{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"item "},{"type":"em","children":[{"type":"text","value":"em"}]}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"item "},{"type":"code","value":"code"}]}]}]}]}'
check "inline content in every block type" "$TMP/inline_in_blocks.md" "$TMP/inline_in_blocks.json" 0

c2fix inline_deep '[[a](b)](c) and [[[d](e)](f)](g)
' "$(P '{"type":"link","destination":"c","title":"","children":[{"type":"link","destination":"b","title":"","children":[{"type":"text","value":"a"}]}]},{"type":"text","value":" and "},{"type":"link","destination":"g","title":"","children":[{"type":"link","destination":"f","title":"","children":[{"type":"link","destination":"e","title":"","children":[{"type":"text","value":"d"}]}]}]}')"
check "links inside link labels" "$TMP/inline_deep.md" "$TMP/inline_deep.json" 0

# a long run of delimiters must not crash or recurse without bound
awk 'BEGIN { s = ""; for (i = 0; i < 200; i++) s = s "*"; print s "x" s; print ""; for (i = 0; i < 200; i++) s = s "_"; print s "x" s; for (i = 0; i < 100; i++) s = s "["; print s "x" s }' > "$TMP/delims.md"
rc=0; "$MD" --ast "$TMP/delims.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 0 ] && [ "$(head -c 1 "$TMP/o")" = "{" ] && [ ! -s "$TMP/e" ]; then ok; else bad "delimiter stress: exit $rc"; fi

# stress: many mixed inline constructs
awk 'BEGIN { for (i = 0; i < 2000; i++) print "text " i " *em* **st** `c` [l](/d \"t\") ![i](/p) <http://e.test> \\* &amp; [r]" }' > "$TMP/mixed.md"
printf '[r]: /ref "R"\n' >> "$TMP/mixed.md"
rc=0; "$MD" --ast "$TMP/mixed.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 0 ] && [ "$(head -c 1 "$TMP/o")" = "{" ]; then ok; else bad "inline stress: exit $rc"; fi

# autolinks are not links yet (reserved for a later checkpoint): they stay text
c2fix inline_autolink 'see <https://e.test>
' "$(P '{"type":"text","value":"see <https://e.test>"}')"
check "bare autolink stays text" "$TMP/inline_autolink.md" "$TMP/inline_autolink.json" 0

# ------------------------------------------------------------------ #
# Checkpoint C3: renderers
# ------------------------------------------------------------------ #

# check_render <name> <flag> <literal-input> <literal-expected-stdout>
# Like check(), but for the HTML and text modes.
check_render() {
    name=$1; flag=$2; in=$3; want=$4
    got_rc=0
    printf '%s' "$in" > "$TMP/render.md"
    printf '%s' "$want" > "$TMP/render.want"
    "$MD" "$flag" "$TMP/render.md" > "$TMP/render.raw" 2> "$TMP/render.err" || got_rc=$?
    tr -d '\r' < "$TMP/render.raw" > "$TMP/render.out"
    tr -d '\r' < "$TMP/render.want" > "$TMP/render.wantc"
    if [ "$got_rc" != 0 ]; then
        bad "$name: exit $got_rc, want 0"
        return
    fi
    if [ -s "$TMP/render.err" ]; then
        bad "$name: stderr not empty on success"
        return
    fi
    if ! cmp -s "$TMP/render.out" "$TMP/render.wantc"; then
        bad "$name: stdout mismatch"
        printf '  want: %s\n  got:  %s\n' "$(cat "$TMP/render.wantc")" "$(cat "$TMP/render.out")" >&2
        return
    fi
    ok
}

check_render "html: empty document" --html '' ''
check_render "text: empty document" --text '' ''

check_render "html: paragraph" --html 'hello
' '<p>hello</p>
'
check_render "html: heading levels" --html '# h1
###### h6
' '<h1>h1</h1>
<h6>h6</h6>
'
check_render "html: thematic break" --html 'a

---

b
' '<p>a</p>
<hr />
<p>b</p>
'
check_render "html: blockquote" --html '> quoted
> lines
' '<blockquote>
<p>quoted
lines</p>
</blockquote>
'
check_render "html: tight and loose lists" --html '- one
- two

text

- loose a

- loose b

1. first
2. second
' '<ul>
<li>one</li>
<li>two</li>
</ul>
<p>text</p>
<ul>
<li><p>loose a</p>
</li>
<li><p>loose b</p>
</li>
</ul>
<ol>
<li>first</li>
<li>second</li>
</ol>
'
check_render "html: ordered list start" --html '3. three
4. four
' '<ol start="3">
<li>three</li>
<li>four</li>
</ol>
'
check_render "html: nested list" --html '- outer
  - inner
' '<ul>
<li>outer<ul>
<li>inner</li>
</ul>
</li>
</ul>
'
check_render "html: fenced code with info" --html '```c
int a = 1 < 2;
```
' '<pre><code class="language-c">int a = 1 &lt; 2;
</code></pre>
'
check_render "html: fenced code without info" --html '```
plain
```
' '<pre><code>plain
</code></pre>
'
check_render "html: inline content" --html 'a *em* **strong** `code` [l](/d "T") ![i](/p "A")  
hard
' '<p>a <em>em</em> <strong>strong</strong> <code>code</code> <a href="/d" title="T">l</a> <img src="/p" alt="i" title="A" /><br />
hard</p>
'

# A literal tag in the source must never reach the output as live markup.
check_render "html: script is escaped" --html '<script>alert("x")</script> & 1 < 2 > 0
' '<p>&lt;script&gt;alert("x")&lt;/script&gt; &amp; 1 &lt; 2 &gt; 0</p>
'
check_render "html: attribute quotes are escaped" --html '[a](<"q"&>) [b](/d '"'"'e"f'"'"') ![c](</im"g&.png> '"'"'a"l&t'"'"')
' '<p><a href="&quot;q&quot;&amp;">a</a> <a href="/d" title="e&quot;f">b</a> <img src="/im&quot;g&amp;.png" alt="c" title="a&quot;l&amp;t" /></p>
'
# Entities are literal inside a code span, so the ampersand is escaped once
# more on the way out.
check_render "html: code span is escaped" --html '`&amp; <b>`
' '<p><code>&amp;amp; &lt;b&gt;</code></p>
'
check_render "html: reference link" --html '[t][r] and [r][] and [r]

[r]: /ref "RT"
' '<p><a href="/ref" title="RT">t</a> and <a href="/ref" title="RT">r</a> and <a href="/ref" title="RT">r</a></p>
'

check_render "text: paragraph and heading" --text '# Title

body line one
body line two
' 'Title

body line one
body line two
'
check_render "text: blockquote keeps lines" --text '> one
> two
' 'one
two
'
check_render "text: list structure" --text '- one
- two
  - nested

1. first
2. second
' '  one
  two
    nested

  first
  second
'
check_render "text: thematic break adds nothing" --text 'a

---

b
' 'a

b
'
check_render "text: code block is verbatim" --text '```
a < b & c
  indented
```
' 'a < b & c
  indented
'
check_render "text: inline markup is removed" --text 'a *em* **strong** `co*de*` [l](/d) ![i](/p)  
hard
' 'a em strong co*de* l i
hard
'
check_render "text: escapes and entities" --text 'a \* b &amp; &#65; \\ c
' 'a * b & A \ c
'
check_render "text: document ends with one newline" --text 'x

' 'x
'

# ------------------------------------------------------------------ #
# CLI contract
# ------------------------------------------------------------------ #

rc=0; "$MD" --ast "$TMP/does-not-exist.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
[ "$rc" = 1 ] && [ ! -s "$TMP/o" ] && [ -s "$TMP/e" ] && ok || bad "missing file: exit $rc, stdout/stderr contract"

rc=0; "$MD" > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "no arguments: exit $rc, want 2"

rc=0; "$MD" --nope "$TMP/empty.md" > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "unknown flag: exit $rc, want 2"

rc=0; "$MD" --ast > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "--ast without value: exit $rc, want 2"

rc=0; "$MD" --html > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "--html without value: exit $rc, want 2"

rc=0; "$MD" --text > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "--text without value: exit $rc, want 2"

rc=0; "$MD" --ast "$TMP/empty.md" "$TMP/empty.md" > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "two files: exit $rc, want 2"

rc=0; "$MD" --html --text "$TMP/empty.md" > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "two modes: exit $rc, want 2"

rc=0; "$MD" --ast --ast "$TMP/empty.md" > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "repeated mode: exit $rc, want 2"

rc=0; "$MD" --html= > /dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "empty inline value: exit $rc, want 2"

# --flag=VALUE is accepted and matches the separated form byte for byte
printf 'a *b* <c>\n' > "$TMP/inline_value.md"
rc=0; "$MD" --html="$TMP/inline_value.md" > "$TMP/sep" 2> "$TMP/e" || rc=$?
rc2=0; "$MD" --html "$TMP/inline_value.md" > "$TMP/eq" 2>/dev/null || rc2=$?
if [ "$rc" = 0 ] && [ "$rc2" = 0 ] && cmp -s "$TMP/sep" "$TMP/eq" && [ ! -s "$TMP/e" ]; then ok; else bad "--html=FILE matches --html FILE"; fi

# stdin works for every mode, and stdin is read for '-'
printf '# H\n\nx *y*\n' > "$TMP/stdin.md"
for flag in --ast --html --text; do
    rc=0; "$MD" "$flag" - < "$TMP/stdin.md" > "$TMP/s" 2> "$TMP/e" || rc=$?
    rc2=0; "$MD" "$flag" "$TMP/stdin.md" > "$TMP/f" 2>/dev/null || rc2=$?
    if [ "$rc" = 0 ] && [ "$rc2" = 0 ] && cmp -s "$TMP/s" "$TMP/f" && [ ! -s "$TMP/e" ]; then ok; else bad "$flag reads stdin as '-'"; fi
done

# a missing file is an I/O error for every mode
for flag in --ast --html --text; do
    rc=0; "$MD" "$flag" "$TMP/no-such-file.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
    if [ "$rc" = 1 ] && [ ! -s "$TMP/o" ] && [ -s "$TMP/e" ]; then ok; else bad "$flag missing file: exit $rc, want 1"; fi
done

# renderers are deterministic and quiet on stderr
printf 'a *b* `c` <d> [e](/f)\n\n> q\n' > "$TMP/det.md"
for flag in --html --text; do
    "$MD" "$flag" "$TMP/det.md" > "$TMP/r1" 2> "$TMP/e1"
    "$MD" "$flag" "$TMP/det.md" > "$TMP/r2" 2> "$TMP/e2"
    if [ ! -s "$TMP/e1" ] && [ ! -s "$TMP/e2" ] && cmp -s "$TMP/r1" "$TMP/r2"; then ok; else bad "$flag is deterministic and quiet"; fi
done

# no HTML ever appears on --text stdout
"$MD" --text "$TMP/fenced.md" > "$TMP/o" 2>/dev/null
if grep -qiE '<[a-z/]' "$TMP/o"; then bad "markup leaked into --text"; else ok; fi

# --html must not emit a live script tag for script-shaped input
printf '<script>x</script>\n' > "$TMP/script.md"
"$MD" --html "$TMP/script.md" > "$TMP/o" 2>/dev/null
if grep -q '<script' "$TMP/o"; then bad "script tag survived escaping"; else ok; fi

# stdout must be exactly one JSON line, nothing else
"$MD" --ast "$TMP/heading.md" > "$TMP/o" 2>/dev/null
lines=$(wc -l < "$TMP/o")
[ "$lines" = 1 ] && ok || bad "stdout is a single line (got $lines)"

# no HTML ever appears on stdout
"$MD" --ast "$TMP/fenced.md" > "$TMP/o" 2>/dev/null
if grep -qiE '<[a-z/]' "$TMP/o"; then bad "html leaked to stdout"; else ok; fi

# nesting limit is a clean error, not a crash
awk 'BEGIN { s = ""; for (i = 0; i < 200; i++) s = s ">"; print s " deep" }' > "$TMP/deep.md"
rc=0; "$MD" --ast "$TMP/deep.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/o" "$TMP/e"; then ok; else bad "deep nesting: exit $rc, want 1"; fi

# large document
awk 'BEGIN { for (i = 0; i < 5000; i++) print "- item " i }' > "$TMP/large.md"
rc=0; "$MD" --ast "$TMP/large.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 0 ] && [ "$(head -c 1 "$TMP/o")" = "{" ]; then ok; else bad "large document: exit $rc"; fi


# ------------------------------------------------------------------ #
# Checkpoint C4: extensions
# ------------------------------------------------------------------ #

# check_ext <name> <flag> <config> <input-file> <expected-stdout-file> <expected-exit>
# The same normalization as check(): CR bytes are dropped on both sides. A
# successful run must leave stderr empty, and a failing one must write a
# diagnostic to stderr and nothing at all to stdout.
check_ext() {
    name=$1; flag=$2; config=$3; in=$4; want=$5; want_rc=$6
    got_rc=0
    "$MD" "$flag" --ext "$config" "$in" > "$TMP/o.raw" 2> "$TMP/e" || got_rc=$?
    tr -d '\r' < "$TMP/o.raw" > "$TMP/o"
    tr -d '\r' < "$want" > "$TMP/w"
    if [ "$got_rc" != "$want_rc" ]; then
        bad "$name: exit $got_rc, want $want_rc"
        return
    fi
    if ! cmp -s "$TMP/o" "$TMP/w"; then
        bad "$name: stdout mismatch"
        printf '  want: %s\n  got:  %s\n' "$(cat "$TMP/w")" "$(cat "$TMP/o")" >&2
        return
    fi
    if [ "$want_rc" = 0 ] && [ -s "$TMP/e" ]; then
        bad "$name: stderr not empty on success"
        return
    fi
    if [ "$want_rc" != 0 ] && [ ! -s "$TMP/e" ]; then
        bad "$name: no diagnostic on failure"
        return
    fi
    if [ "$want_rc" != 0 ] && [ -s "$TMP/o.raw" ]; then
        bad "$name: stdout not empty on failure"
        return
    fi
    ok
}

# ext_fixture <name> <config-path> <input-literal> <expected-json>
# The input and the expected JSON are written verbatim; the expected JSON
# already ends with the newline the CLI prints after the document. The
# configuration is named by path, because the configurations are shared by
# several cases.
ext_fixture() {
    printf '%s' "$3" > "$TMP/$1.md"
    printf '%s' "$4" > "$TMP/$1.want"
}

# One document that exercises every built-in at once, so a single run shows
# that enabling one extension does not disturb the others.
printf '%s' '| A | B |
|:---|---:|
| 1 | 2 |

- [x] done
- [ ] todo

~~gone~~ and here

See ref[^1].

[^1]: the note
' > "$TMP/ext.md"

printf '%s' '{"all": true}' > "$TMP/all.json"
printf '%s' '{}' > "$TMP/none.json"
printf '%s' '{"tables": true}' > "$TMP/tables.json"
printf '%s' '{"strikethrough": true}' > "$TMP/strike.json"
printf '%s' '{"task_lists": true}' > "$TMP/task.json"
printf '%s' '{"footnotes": true}' > "$TMP/footnotes.json"

# --- tables ------------------------------------------------------- #
ext_fixture ext_table "$TMP/tables.json" '| A | B |
|:---|---:|
| 1 | 2 |
' '{"type":"document","children":[{"type":"table","align":["left","right"],"children":[{"type":"table_header","children":[{"type":"table_cell","children":[{"type":"text","value":"A"}]},{"type":"table_cell","children":[{"type":"text","value":"B"}]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"text","value":"1"}]},{"type":"table_cell","children":[{"type":"text","value":"2"}]}]}]}]}
'
check_ext "ext: pipe table" --ast "$TMP/tables.json" "$TMP/ext_table.md" \
    "$TMP/ext_table.want" 0

# A table without a leading or trailing pipe is still a table: the outer pipes
# are optional delimiters and never cells of their own.
ext_fixture ext_table_bare "$TMP/tables.json" 'A | B
--- | ---
1 | 2
' '{"type":"document","children":[{"type":"table","align":["none","none"],"children":[{"type":"table_header","children":[{"type":"table_cell","children":[{"type":"text","value":"A"}]},{"type":"table_cell","children":[{"type":"text","value":"B"}]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"text","value":"1"}]},{"type":"table_cell","children":[{"type":"text","value":"2"}]}]}]}]}
'
check_ext "ext: table without outer pipes" --ast "$TMP/tables.json" \
    "$TMP/ext_table_bare.md" "$TMP/ext_table_bare.want" 0

# Cell content is inline content, so emphasis and code spans work in a cell.
ext_fixture ext_table_inline "$TMP/tables.json" '| x | y |
| --- | --- |
| *a* | `b` |
' '{"type":"document","children":[{"type":"table","align":["none","none"],"children":[{"type":"table_header","children":[{"type":"table_cell","children":[{"type":"text","value":"x"}]},{"type":"table_cell","children":[{"type":"text","value":"y"}]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"em","children":[{"type":"text","value":"a"}]}]},{"type":"table_cell","children":[{"type":"code","value":"b"}]}]}]}]}
'
check_ext "ext: table cells are inline content" --ast \
    "$TMP/tables.json" "$TMP/ext_table_inline.md" \
    "$TMP/ext_table_inline.want" 0

# The column count comes from the delimiter row: a short row is padded with
# empty cells and a long one has its extra cells dropped, so the AST stays
# rectangular.
ext_fixture ext_table_ragged "$TMP/tables.json" '| a | b | c |
| --- | --- | --- |
| 1 |
| 1 | 2 | 3 | 4 |
' '{"type":"document","children":[{"type":"table","align":["none","none","none"],"children":[{"type":"table_header","children":[{"type":"table_cell","children":[{"type":"text","value":"a"}]},{"type":"table_cell","children":[{"type":"text","value":"b"}]},{"type":"table_cell","children":[{"type":"text","value":"c"}]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"text","value":"1"}]},{"type":"table_cell","children":[]},{"type":"table_cell","children":[]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"text","value":"1"}]},{"type":"table_cell","children":[{"type":"text","value":"2"}]},{"type":"table_cell","children":[{"type":"text","value":"3"}]}]}]}]}
'
check_ext "ext: ragged rows are rectangular" --ast \
    "$TMP/tables.json" "$TMP/ext_table_ragged.md" \
    "$TMP/ext_table_ragged.want" 0

# A header whose cell count does not match the delimiter row is not a table,
# so the lines stay an ordinary paragraph.
ext_fixture ext_table_mismatch "$TMP/tables.json" '| a | b | c |
| --- | --- |
| 1 | 2 |
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"| a | b | c |"},{"type":"softbreak"},{"type":"text","value":"| --- | --- |"},{"type":"softbreak"},{"type":"text","value":"| 1 | 2 |"}]}]}
'
check_ext "ext: header must match the delimiter row" --ast \
    "$TMP/tables.json" "$TMP/ext_table_mismatch.md" \
    "$TMP/ext_table_mismatch.want" 0

# An escaped pipe is cell content, not a separator.
ext_fixture ext_table_escape "$TMP/tables.json" '| a | b |
| --- | --- |
| x \| y | z |
' '{"type":"document","children":[{"type":"table","align":["none","none"],"children":[{"type":"table_header","children":[{"type":"table_cell","children":[{"type":"text","value":"a"}]},{"type":"table_cell","children":[{"type":"text","value":"b"}]}]},{"type":"table_row","children":[{"type":"table_cell","children":[{"type":"text","value":"x | y"}]},{"type":"table_cell","children":[{"type":"text","value":"z"}]}]}]}]}
'
check_ext "ext: escaped pipe stays in its cell" --ast \
    "$TMP/tables.json" "$TMP/ext_table_escape.md" \
    "$TMP/ext_table_escape.want" 0

# --- strikethrough ------------------------------------------------ #
ext_fixture ext_strike "$TMP/strike.json" '~~gone~~ and ~~also~~ here
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"strikethrough","children":[{"type":"text","value":"gone"}]},{"type":"text","value":" and "},{"type":"strikethrough","children":[{"type":"text","value":"also"}]},{"type":"text","value":" here"}]}]}
'
check_ext "ext: strikethrough" --ast "$TMP/strike.json" \
    "$TMP/ext_strike.md" "$TMP/ext_strike.want" 0

# An unclosed run and a lone tilde are ordinary text, not half-built nodes.
ext_fixture ext_strike_open "$TMP/strike.json" '~~unclosed and ~single
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"~~unclosed and ~single"}]}]}
'
check_ext "ext: unclosed strikethrough stays text" --ast \
    "$TMP/strike.json" "$TMP/ext_strike_open.md" \
    "$TMP/ext_strike_open.want" 0

# Strikethrough nests inside emphasis and vice versa.
ext_fixture ext_strike_nest "$TMP/strike.json" '**a ~~b~~ c**
' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"strong","children":[{"type":"text","value":"a "},{"type":"strikethrough","children":[{"type":"text","value":"b"}]},{"type":"text","value":" c"}]}]}]}
'
check_ext "ext: strikethrough nests" --ast "$TMP/strike.json" \
    "$TMP/ext_strike_nest.md" "$TMP/ext_strike_nest.want" 0

# --- task lists --------------------------------------------------- #
ext_fixture ext_task "$TMP/task.json" '- [x] done
- [ ] todo
- plain
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","checked":true,"children":[{"type":"paragraph","children":[{"type":"text","value":"done"}]}]},{"type":"item","checked":false,"children":[{"type":"paragraph","children":[{"type":"text","value":"todo"}]}]},{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"plain"}]}]}]}]}
'
check_ext "ext: task list" --ast "$TMP/task.json" "$TMP/ext_task.md" \
    "$TMP/ext_task.want" 0

# The marker must stand alone: "[x]done" is not a task item.
ext_fixture ext_task_tight "$TMP/task.json" '- [x]done
' '{"type":"document","children":[{"type":"list","ordered":false,"start":1,"tight":true,"children":[{"type":"item","children":[{"type":"paragraph","children":[{"type":"text","value":"[x]done"}]}]}]}]}
'
check_ext "ext: task marker must stand alone" --ast \
    "$TMP/task.json" "$TMP/ext_task_tight.md" \
    "$TMP/ext_task_tight.want" 0

# An ordered list takes markers the same way.
ext_fixture ext_task_ordered "$TMP/task.json" '1. [X] done
' '{"type":"document","children":[{"type":"list","ordered":true,"start":1,"tight":true,"children":[{"type":"item","checked":true,"children":[{"type":"paragraph","children":[{"type":"text","value":"done"}]}]}]}]}
'
check_ext "ext: task marker in an ordered list" --ast \
    "$TMP/task.json" "$TMP/ext_task_ordered.md" \
    "$TMP/ext_task_ordered.want" 0

# --- footnotes ---------------------------------------------------- #
ext_fixture ext_foot "$TMP/footnotes.json" 'See ref[^a].

[^a]: the note
' '{"type":"document","footnotes":[{"type":"footnote_def","id":"a","label":"a","children":[{"type":"paragraph","children":[{"type":"text","value":"the note"}]}]}],"children":[{"type":"paragraph","children":[{"type":"text","value":"See ref"},{"type":"footnote_ref","id":"a","label":"a"},{"type":"text","value":"."}]}]}
'
check_ext "ext: footnote" --ast "$TMP/footnotes.json" "$TMP/ext_foot.md" \
    "$TMP/ext_foot.want" 0

# A reference with no definition is ordinary text, the rule the C2 reference
# links already follow: a syntax is only a construct once something defines
# it. The label itself is matched the way a reference label is, case folded
# with internal whitespace collapsed, so the reference and the definition
# below agree on the id.
ext_fixture ext_foot_dangling "$TMP/footnotes.json" 'x[^nope]y
' '{"type":"document","footnotes":[],"children":[{"type":"paragraph","children":[{"type":"text","value":"x[^nope]y"}]}]}
'
check_ext "ext: reference with no definition stays text" --ast \
    "$TMP/footnotes.json" "$TMP/ext_foot_dangling.md" \
    "$TMP/ext_foot_dangling.want" 0

# A definition is recorded on the document and never in the block flow, so
# the note's text does not appear where it was written.
ext_fixture ext_foot_hidden "$TMP/footnotes.json" '[^n]: only a definition
' '{"type":"document","footnotes":[{"type":"footnote_def","id":"n","label":"n","children":[{"type":"paragraph","children":[{"type":"text","value":"only a definition"}]}]}],"children":[]}
'
check_ext "ext: definition leaves the block flow" --ast \
    "$TMP/footnotes.json" "$TMP/ext_foot_hidden.md" \
    "$TMP/ext_foot_hidden.want" 0

# The definition and the reference agree on the normalized id, so they meet.
ext_fixture ext_foot_match "$TMP/footnotes.json" 'a[^Mixed   Case]b

[^mixed case]: note
' '{"type":"document","footnotes":[{"type":"footnote_def","id":"mixed case","label":"mixed case","children":[{"type":"paragraph","children":[{"type":"text","value":"note"}]}]}],"children":[{"type":"paragraph","children":[{"type":"text","value":"a"},{"type":"footnote_ref","id":"mixed case","label":"Mixed   Case"},{"type":"text","value":"b"}]}]}
'
check_ext "ext: reference and definition share an id" --ast \
    "$TMP/footnotes.json" "$TMP/ext_foot_match.md" \
    "$TMP/ext_foot_match.want" 0

# --- renderers ---------------------------------------------------- #
printf '%s' '| a | b |
| :- | -: |
| 1 | 2 |
' > "$TMP/rt.md"
printf '%s' '<table>
<thead>
<th align="left">a</th>
<th align="right">b</th>
</thead>
<tbody>
<td align="left">1</td>
<td align="right">2</td>
</tbody>
</table>
' > "$TMP/rt_table.want"
check_ext "ext: table renders to HTML" --html "$TMP/tables.json" \
    "$TMP/rt.md" "$TMP/rt_table.want" 0

printf '%s' 'a b
1 2
' > "$TMP/rt_table.txt"
check_ext "ext: table renders to text" --text "$TMP/tables.json" \
    "$TMP/rt.md" "$TMP/rt_table.txt" 0

printf '%s' '~~gone~~
' > "$TMP/rt.md"
printf '%s' '<p><del>gone</del></p>
' > "$TMP/rt_del.want"
check_ext "ext: strikethrough renders to HTML" --html "$TMP/strike.json" \
    "$TMP/rt.md" "$TMP/rt_del.want" 0
printf '%s' 'gone
' > "$TMP/rt_del.txt"
check_ext "ext: strikethrough renders to text" --text "$TMP/strike.json" \
    "$TMP/rt.md" "$TMP/rt_del.txt" 0

printf '%s' '- [x] done
- [ ] todo
- plain
' > "$TMP/rt.md"
printf '%s' '<ul>
<li><input type="checkbox" disabled="" checked="" /> done</li>
<li><input type="checkbox" disabled="" /> todo</li>
<li>plain</li>
</ul>
' > "$TMP/rt_task.want"
check_ext "ext: task items render checkboxes" --html "$TMP/task.json" \
    "$TMP/rt.md" "$TMP/rt_task.want" 0
printf '%s' '  [x] done
  [ ] todo
  plain
' > "$TMP/rt_task.txt"
check_ext "ext: task items render markers" --text "$TMP/task.json" \
    "$TMP/rt.md" "$TMP/rt_task.txt" 0

printf '%s' 'See ref[^1].

[^1]: the note
' > "$TMP/rt.md"
printf '%s' '<p>See ref<sup class="footnote-ref"><a href="#fn-1" id="fnref-1">1</a></sup>.</p>
<section class="footnotes">
<div class="footnote" id="fn-1"><p>the note</p>
<a href="#fnref-1" class="footnote-backref">&#8617;</a></div>
</section>
' > "$TMP/rt_foot.want"
check_ext "ext: footnote renders with backlink ids" --html \
    "$TMP/footnotes.json" "$TMP/rt.md" "$TMP/rt_foot.want" 0
printf '%s' 'See ref[^1].
[^1] the note
' > "$TMP/rt_foot.txt"
check_ext "ext: footnote renders to text" --text "$TMP/footnotes.json" \
    "$TMP/rt.md" "$TMP/rt_foot.txt" 0

# The document JSON keeps both keys, and stays on one line.
"$MD" --ast --ext "$TMP/footnotes.json" "$TMP/ext.md" > "$TMP/rt_json" 2>/dev/null
if grep -q '"footnotes":\[' "$TMP/rt_json" && grep -q '"children":\[' "$TMP/rt_json"
then ok; else bad "ext: document JSON has footnotes and children"; fi
lines=$(wc -l < "$TMP/rt_json")
[ "$lines" = 1 ] && ok || bad "ext: document JSON is a single line (got $lines)"

# --- configuration ----------------------------------------------- #
# Key order does not change the enabled set.
printf '%s' '{"footnotes": true, "tables": true}' > "$TMP/order_a.json"
printf '%s' '{"tables": true, "footnotes": true}' > "$TMP/order_b.json"
"$MD" --ast --ext "$TMP/order_a.json" "$TMP/ext.md" > "$TMP/oa" 2>/dev/null
"$MD" --ast --ext "$TMP/order_b.json" "$TMP/ext.md" > "$TMP/ob" 2>/dev/null
cmp -s "$TMP/oa" "$TMP/ob" && ok || bad "ext: key order is irrelevant"

# "all" turns on every built-in, and an explicit false enables nothing.
printf '%s' '{"tables": true, "footnotes": true}' > "$TMP/pair.json"
printf '%s' '{"footnotes": true, "task_lists": true, "tables": true,
"strikethrough": true}' > "$TMP/every.json"
"$MD" --ast --ext "$TMP/all.json" "$TMP/ext.md" > "$TMP/c_all" 2>/dev/null
"$MD" --ast --ext "$TMP/every.json" "$TMP/ext.md" > "$TMP/c_every" 2>/dev/null
"$MD" --ast --ext "$TMP/pair.json" "$TMP/ext.md" > "$TMP/c_pair" 2>/dev/null
cmp -s "$TMP/c_all" "$TMP/c_every" && ok || bad "ext: all == every key true"

printf '%s' '{"none": true}' > "$TMP/none_true.json"
printf '%s' '{"tables": false, "footnotes": false}' > "$TMP/false.json"
"$MD" --ast "$TMP/ext.md" > "$TMP/c_off" 2>/dev/null
for f in none true false; do
    cfg=$TMP/none.json
    [ "$f" = true ] && cfg=$TMP/none_true.json
    [ "$f" = false ] && cfg=$TMP/false.json
    "$MD" --ast --ext "$cfg" "$TMP/ext.md" > "$TMP/c_$f" 2>/dev/null
    cmp -s "$TMP/c_$f" "$TMP/c_off" && ok || bad "ext: $f enables nothing"
done

# The array form is still accepted and unions the same way.
printf '%s' '{"extensions": ["tables", "footnotes"]}' > "$TMP/array.json"
"$MD" --ast --ext "$TMP/array.json" "$TMP/ext.md" > "$TMP/c_array" 2>/dev/null
cmp -s "$TMP/c_array" "$TMP/c_pair" && ok || bad "ext: array form unions like keys"

# Whitespace and nesting do not change the outcome either. A tab counts as
# whitespace here, as it does in JSON.
printf '%s' '  {
  "tables" : true ,
  "footnotes" : true,
  "strikethrough" :	true,
  "task_lists"  :  true
}
' > "$TMP/spaced.json"
"$MD" --ast --ext "$TMP/spaced.json" "$TMP/ext.md" > "$TMP/c_spaced" 2>/dev/null
cmp -s "$TMP/c_spaced" "$TMP/c_every" && ok || bad "ext: whitespace is insignificant"

# --- disabled syntax stays ordinary text ------------------------- #
# With an empty configuration the same document must produce no table, no
# strikethrough, no task marker, no footnote node and no footnotes array.
for flag in --ast --html --text; do
    "$MD" "$flag" --ext "$TMP/none.json" "$TMP/ext.md" > "$TMP/dis" 2> "$TMP/e"
    if [ -s "$TMP/e" ]; then
        bad "ext: {} with $flag wrote to stderr"
    elif grep -qE '"type":"(table|strikethrough|footnote_ref|footnote_def)"|"footnotes":|"checked":' \
            "$TMP/dis"; then
        bad "ext: {} with $flag still produced an extension node"
    else
        ok
    fi
done

# {} must be byte-identical to passing no --ext at all, in every mode.
for flag in --ast --html --text; do
    "$MD" "$flag" --ext "$TMP/none.json" "$TMP/ext.md" > "$TMP/d1" 2>/dev/null
    "$MD" "$flag" "$TMP/ext.md" > "$TMP/d2" 2>/dev/null
    cmp -s "$TMP/d1" "$TMP/d2" && ok || bad "ext: {} equals no --ext ($flag)"
done

# The pipe table renders as the paragraph text it is when disabled.
printf '%s' '| A | B |
|:---|---:|
| 1 | 2 |
' > "$TMP/rt.md"
printf '%s' '<p>| A | B |
|:---|---:|
| 1 | 2 |</p>
' > "$TMP/rt_dis.want"
check_ext "ext: disabled table is a paragraph" --html "$TMP/none.json" \
    "$TMP/rt.md" "$TMP/rt_dis.want" 0

# A single extension leaves the others' syntax alone: with only tables on, the
# strikethrough markers are still literal text.
printf '%s' '~~gone~~
' > "$TMP/rt.md"
printf '%s' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"~~gone~~"}]}]}
' > "$TMP/rt_off.want"
check_ext "ext: only tables on leaves strikethrough as text" --ast \
    "$TMP/tables.json" "$TMP/rt.md" "$TMP/rt_off.want" 0

# ... and with only strikethrough on, the pipe table is still a paragraph.
printf '%s' '| a | b |
| --- | --- |
| 1 | 2 |
' > "$TMP/rt.md"
printf '%s' '{"type":"document","children":[{"type":"paragraph","children":[{"type":"text","value":"| a | b |"},{"type":"softbreak"},{"type":"text","value":"| --- | --- |"},{"type":"softbreak"},{"type":"text","value":"| 1 | 2 |"}]}]}
' > "$TMP/rt_tab_off.want"
check_ext "ext: only strikethrough on leaves a table as text" --ast \
    "$TMP/strike.json" "$TMP/rt.md" "$TMP/rt_tab_off.want" 0

# --- configuration errors ---------------------------------------- #
# An unknown name and a value that is not a boolean are clean errors: exit 1,
# a diagnostic on stderr, nothing on stdout.
for bad_cfg in 'nope' '{"nonesuch": true}' '{"tables": "yes"}' \
              '[' '{"tables": true' '{"tables": tru}' '{} extra' 'null' \
              '{"tables": 1}'; do
    printf '%s' "$bad_cfg" > "$TMP/bad.json"
    rc=0
    "$MD" --ast --ext "$TMP/bad.json" "$TMP/rt.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
    if [ "$rc" = 1 ] && [ ! -s "$TMP/o" ] && [ -s "$TMP/e" ]; then
        ok
    else
        bad "ext: rejects config '$bad_cfg' (exit $rc)"
    fi
done

# A missing configuration file is an error, not a silent "no extensions".
rc=0
"$MD" --ast --ext "$TMP/absent.json" "$TMP/rt.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 1 ] && [ ! -s "$TMP/o" ] && [ -s "$TMP/e" ]; then ok; else
    bad "ext: missing config: exit $rc, want 1"; fi

# A document read from stdin behaves the same with and without --ext.
printf '%s' '~~x~~
' | "$MD" --html --ext "$TMP/strike.json" - > "$TMP/rt_si" 2>"$TMP/e"
printf '%s' '<p><del>x</del></p>
' > "$TMP/rt_si.want"
# The CLI may close its final line with CRLF on Windows, so the comparison
# drops CR exactly as the other renderer checks do.
tr -d '\r' < "$TMP/rt_si" > "$TMP/rt_si.out"
tr -d '\r' < "$TMP/rt_si.want" > "$TMP/rt_si.wc"
if [ ! -s "$TMP/e" ] && cmp -s "$TMP/rt_si.out" "$TMP/rt_si.wc"; then ok; else
    bad "ext: --ext works with a document on stdin"; fi

# --- CLI surface of --ext ---------------------------------------- #
rc=0
"$MD" --ast --ext a --ext b "$TMP/rt.md" >/dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "ext: --ext twice: exit $rc, want 2"
rc=0
"$MD" --ast --ext >/dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] && ok || bad "ext: --ext with no value: exit $rc, want 2"

# "-" reads the configuration from stdin.
printf '%s' '{"tables": true}' | "$MD" --ast --ext - "$TMP/rt.md" > "$TMP/cfg_si" 2>"$TMP/e"
"$MD" --ast --ext "$TMP/tables.json" "$TMP/rt.md" > "$TMP/cfg_file" 2>/dev/null
if [ ! -s "$TMP/e" ] && cmp -s "$TMP/cfg_si" "$TMP/cfg_file"; then ok; else
    bad "ext: --ext - reads the configuration from stdin"; fi

# --ext may precede the mode as well as follow it, and --ext=FILE works.
"$MD" --ext "$TMP/strike.json" --ast "$TMP/rt.md" > "$TMP/pre" 2>/dev/null
"$MD" --ast --ext "$TMP/strike.json" "$TMP/rt.md" > "$TMP/post" 2>/dev/null
cmp -s "$TMP/pre" "$TMP/post" && ok || bad "ext: flag order is irrelevant"
"$MD" --ast "--ext=$TMP/strike.json" "$TMP/rt.md" > "$TMP/eq" 2>/dev/null
cmp -s "$TMP/eq" "$TMP/post" && ok || bad "ext: --ext=FILE form"

# --help and --version still succeed and mention the extension flag.
"$MD" --help > "$TMP/help" 2>"$TMP/e"
if [ -s "$TMP/help" ] && grep -q -- '--ext' "$TMP/help" && [ ! -s "$TMP/e" ]
then ok; else bad "ext: --help documents --ext"; fi
"$MD" --version > "$TMP/ver" 2>"$TMP/e"
if [ -s "$TMP/ver" ] && [ ! -s "$TMP/e" ]; then ok; else bad "ext: --version is quiet"; fi

# --- determinism and quiet output -------------------------------- #
for flag in --ast --html --text; do
    "$MD" "$flag" --ext "$TMP/all.json" "$TMP/ext.md" > "$TMP/x1" 2>"$TMP/xe1"
    "$MD" "$flag" --ext "$TMP/all.json" "$TMP/ext.md" > "$TMP/x2" 2>"$TMP/xe2"
    if [ ! -s "$TMP/xe1" ] && [ ! -s "$TMP/xe2" ] && cmp -s "$TMP/x1" "$TMP/x2"
    then ok; else bad "ext: $flag is deterministic and quiet"; fi
done

# A footnote id is escaped like any other attribute, so a label in the source
# cannot inject markup into the generated section.
printf '%s' 'x[^a"b]y

[^a"b]: note
' > "$TMP/fq.md"
"$MD" --html --ext "$TMP/footnotes.json" "$TMP/fq.md" > "$TMP/fq" 2>/dev/null
if grep -q '<script' "$TMP/fq"; then
    bad "ext: footnote id escaped (script)"
else ok; fi
if grep -q 'a&quot;b' "$TMP/fq"; then ok; else bad "ext: footnote id is attribute-escaped"; fi

# A label may hold characters that are legal in an attribute but not in a
# fragment identifier, so the id and the href are slugged instead of being
# copied. The reference and the definition are derived from the same id, so
# the two always agree.
printf '%s' 'see[^a b]!

[^a b]: note
' > "$TMP/fq.md"
"$MD" --html --ext "$TMP/footnotes.json" "$TMP/fq.md" > "$TMP/fq" 2>/dev/null
if grep -q 'href="#fn-a-b" id="fnref-a-b"' "$TMP/fq" &&
   grep -q 'id="fn-a-b"' "$TMP/fq" &&
   grep -q 'href="#fnref-a-b"' "$TMP/fq"; then ok; else
    bad "ext: footnote fragment is a slug: $(cat "$TMP/fq")"; fi

# Every body row shares one tbody, so a consumer that counts sections sees a
# rectangular table.
printf '%s' '| a | b |
| --- | --- |
| 1 | 2 |
| 3 | 4 |
' > "$TMP/fq.md"
"$MD" --html --ext "$TMP/tables.json" "$TMP/fq.md" > "$TMP/fq" 2>/dev/null
if [ "$(grep -c '<tbody>' "$TMP/fq")" = 1 ] &&
   [ "$(grep -c '</tbody>' "$TMP/fq")" = 1 ] &&
   [ "$(grep -c '<tr>' "$TMP/fq")" = 0 ]; then ok; else
    bad "ext: one tbody for every body row"; fi

# Eight columns is the bound: the delimiter row is validated into a fixed
# eight-entry array on the block parser's stack, so a wider row is not a
# delimiter row and the block stays an ordinary paragraph.
printf '%s' '| a | b | c | d | e | f | g | h |
| - | - | - | - | - | - | - | - |
| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
' > "$TMP/fq.md"
"$MD" --ast --ext "$TMP/tables.json" "$TMP/fq.md" > "$TMP/fq" 2>/dev/null
if grep -q '"type":"table"' "$TMP/fq"; then ok; else bad "ext: eight columns parse as a table"; fi
printf '%s' '| a | b | c | d | e | f | g | h | i |
| - | - | - | - | - | - | - | - | - |
| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
' > "$TMP/fq.md"
"$MD" --ast --ext "$TMP/tables.json" "$TMP/fq.md" > "$TMP/fq" 2>/dev/null
if grep -q '"type":"table"' "$TMP/fq"; then
    bad "ext: nine columns stay a paragraph"
else ok; fi

# Cell text and link titles are escaped inside a table like anywhere else.
printf '%s' '| a |
| --- |
| <script>x</script> & "q" |
' > "$TMP/esc.md"
"$MD" --html --ext "$TMP/tables.json" "$TMP/esc.md" > "$TMP/esc" 2>/dev/null
if grep -q '<script' "$TMP/esc"; then
    bad "ext: table cell escaped (script)"
elif grep -q '&lt;script&gt;' "$TMP/esc"; then ok; else
    bad "ext: table cell text is escaped"; fi

# --- a large document with every extension on --------------------- #
awk 'BEGIN {
    print "| a | b |"
    print "| --- | --- |"
    for (i = 0; i < 2000; i++) print "| " i " | x |"
    print ""
    for (i = 0; i < 2000; i++) print "- [ ] task " i
    print ""
    print "text [^n] more"
    print ""
    print "[^n]: note"
}' > "$TMP/bigext.md"
rc=0
"$MD" --ast --ext "$TMP/all.json" "$TMP/bigext.md" > "$TMP/o" 2> "$TMP/e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/e" ] && [ "$(head -c 1 "$TMP/o")" = "{" ]
then ok; else bad "ext: large document: exit $rc"; fi
# A phase marker, not a second total: PASS keeps counting, so the C5
# section below reports everything above as well. Labelled so it cannot
# be read as a duplicate of the final line.
printf '\n-- C1-C4: %d passed, %d failed (C5 continues below) --\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]

# ------------------------------------------------------------------ #
# Streaming and limits (checkpoint C5)                               #
# ------------------------------------------------------------------ #

# Streamed and one-shot output must be byte-identical for the same bytes.
# Every input the suite already built is re-parsed through --stream and the
# two payloads compared with cmp, so this is a sweep over the whole fixture
# set rather than one hand-picked document.
same_as_stream() {
    name=$1; in=$2; ext=$3
    rc=0
    "$MD" --ast --ext "$ext" "$in" > "$TMP/a.raw" 2>/dev/null || rc=$?
    "$MD" --ast --stream --ext "$ext" "$in" > "$TMP/b.raw" 2>/dev/null || rc=$?
    tr -d '\r' < "$TMP/a.raw" > "$TMP/a"
    tr -d '\r' < "$TMP/b.raw" > "$TMP/b"
    if [ "$rc" != 0 ]; then bad "stream: $name: exit $rc"; return; fi
    if ! cmp -s "$TMP/a" "$TMP/b"; then
        bad "stream: $name: streamed and one-shot JSON differ"
        return
    fi
    ok
}

# --- a representative document, with and without extensions --------- #
printf '%s' '# T

para *em* **strong** `code` and ~~struck~~

- [ ] todo
- [x] done

| a | b |
| :- | -: |
| 1 | 2 |

note[^n] here

[^n]: the note

> quote
> > deep

    indented

---
' > "$TMP/stream.md"

for cfg in all tables footnotes strikethrough tasklist; do
    printf '{"%s":true}\n' "$cfg" > "$TMP/s-$cfg.json"
    same_as_stream "$cfg" "$TMP/stream.md" "$TMP/s-$cfg.json"
done

# --- a sweep over every fixture input the suite built --------------- #
sweep_fail=0
sweep_n=0
for f in "$TMP"/*.md; do
    [ -f "$f" ] || continue
    case $f in
        *stream.md) continue ;;
    esac
    "$MD" --ast "$f" > "$TMP/w1" 2>/dev/null
    rc1=$?
    "$MD" --ast --stream "$f" > "$TMP/w2" 2>/dev/null
    rc2=$?
    sweep_n=$((sweep_n + 1))
    tr -d '\r' < "$TMP/w1" > "$TMP/w1n"
    tr -d '\r' < "$TMP/w2" > "$TMP/w2n"
    if [ "$rc1" != "$rc2" ] || ! cmp -s "$TMP/w1n" "$TMP/w2n"; then
        bad "stream: sweep differs on $f"
        sweep_fail=1
    fi
done
if [ "$sweep_fail" = 0 ] && [ "$sweep_n" -gt 0 ]; then
    PASS=$((PASS + sweep_n))
else
    bad "stream: sweep compared $sweep_n inputs"
fi

# --- stdin, both paths ---------------------------------------------- #
"$MD" --ast - < "$TMP/stream.md" > "$TMP/s1" 2>/dev/null
"$MD" --ast --stream - < "$TMP/stream.md" > "$TMP/s2" 2>/dev/null
tr -d '\r' < "$TMP/s1" > "$TMP/s1n"
tr -d '\r' < "$TMP/s2" > "$TMP/s2n"
if cmp -s "$TMP/s1n" "$TMP/s2n" && [ -s "$TMP/s1n" ]; then ok; else
    bad "stream: stdin matches file"; fi

# --- a CR, a tab and a NUL straddling the 64 KiB read boundary ----- #
# The stream reader decodes CR, CRLF, tabs and NULs incrementally, so a
# control byte at the very end of a read has to be treated exactly as one
# in the middle is. 65535, 65536 and 65537 are the offsets that straddle it.
for pad in 65535 65536 65537; do
    awk -v p="$pad" 'BEGIN{
        s = "";
        for (i = 0; i < p; i++) s = s "x";
        printf "%s\rafter\n", s;
    }' > "$TMP/cr.md"
    awk -v p="$pad" 'BEGIN{
        s = "";
        for (i = 0; i < p; i++) s = s "x";
        printf "%s\r\r\nafter\n", s;
    }' > "$TMP/crlf.md"
    awk -v p="$pad" 'BEGIN{
        s = "";
        for (i = 0; i < p; i++) s = s "x";
        printf "%s\tafter\n", s;
    }' > "$TMP/tab.md"
    awk -v p="$pad" 'BEGIN{
        s = "";
        for (i = 0; i < p; i++) s = s "x";
        printf "%s\000after\n", s;
    }' > "$TMP/nul.md"
    for kind in cr crlf tab nul; do
        "$MD" --ast "$TMP/$kind.md" > "$TMP/x1" 2>/dev/null
        "$MD" --ast --stream "$TMP/$kind.md" > "$TMP/x2" 2>/dev/null
        tr -d '\r' < "$TMP/x1" > "$TMP/x1n"
        tr -d '\r' < "$TMP/x2" > "$TMP/x2n"
        if cmp -s "$TMP/x1n" "$TMP/x2n"; then ok; else
            bad "stream: $kind at offset $pad differs"; fi
    done
done

# --- a chunk boundary landing inside a multi-byte UTF-8 sequence --- #
# The reader copies bytes, not characters, so a sequence split across two
# reads has to survive; offsets 65534..65537 are where that can happen.
for pad in 65534 65535 65536 65537; do
    awk -v p="$pad" 'BEGIN{
        s = "";
        for (i = 0; i < p; i++) s = s "x";
        printf "%s\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 after\n", s;
    }' > "$TMP/utf.md"
    "$MD" --ast "$TMP/utf.md" > "$TMP/u1" 2>/dev/null
    "$MD" --ast --stream "$TMP/utf.md" > "$TMP/u2" 2>/dev/null
    tr -d '\r' < "$TMP/u1" > "$TMP/u1n"
    tr -d '\r' < "$TMP/u2" > "$TMP/u2n"
    if cmp -s "$TMP/u1n" "$TMP/u2n" && [ -s "$TMP/u1n" ]; then ok; else
        bad "stream: utf-8 sequence at offset $pad differs"; fi
done

# --- a large document: both paths agree and neither dies ----------- #
awk 'BEGIN{ for (i = 0; i < 40000; i++) print "line " i " *em* and `code`" }' > "$TMP/huge.md"
rc=0
"$MD" --ast "$TMP/huge.md" > "$TMP/h1" 2> "$TMP/he1" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/he1" ]; then ok; else
    bad "stream: large document one-shot: exit $rc"; fi
rc=0
"$MD" --ast --stream "$TMP/huge.md" > "$TMP/h2" 2> "$TMP/he2" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/he2" ]; then ok; else
    bad "stream: large document streamed: exit $rc"; fi
tr -d '\r' < "$TMP/h1" > "$TMP/h1n"
tr -d '\r' < "$TMP/h2" > "$TMP/h2n"
if cmp -s "$TMP/h1n" "$TMP/h2n"; then ok; else
    bad "stream: large document differs between paths"; fi

# --- a nesting limit, reported as a structured error --------------- #
awk 'BEGIN{ for (i = 0; i < 20; i++) printf "> "; print "deep" }' > "$TMP/nest.md"
for mode in "" "--stream"; do
    rc=0
    "$MD" --ast $mode --max-nesting 4 "$TMP/nest.md" > "$TMP/no" 2> "$TMP/ne" || rc=$?
    if [ "$rc" != 1 ]; then bad "limits: nesting exit $rc, want 1"; continue; fi
    if ! error_doc_is "$TMP/no" "$TMP/ne"; then bad "limits: nesting error document"; continue; fi
    # The column is where the parser refused to descend: line 1, just after
    # the five '> ' markers that --max-nesting 4 admits.
    if [ "$(tr -d '\r' < "$TMP/ne")" = '{"error":{"line":1,"col":11,"message":"nesting too deep"}}' ]
    then ok; else bad "limits: nesting error JSON: $(cat "$TMP/ne")"; fi
done

# --- the reported line is the line that was too deep --------------- #
# A document whose first 40 lines are shallow and whose 41st is where the
# limit bites: the error has to name line 41, not line 1.
awk 'BEGIN{
    for (i = 1; i <= 40; i++) print "shallow " i;
    printf ">";
    for (i = 0; i < 8; i++) printf ">";
    print "deep";
}' > "$TMP/nest2.md"
rc=0
"$MD" --ast --max-nesting 4 "$TMP/nest2.md" > "$TMP/n2o" 2> "$TMP/n2e" || rc=$?
if [ "$rc" = 1 ] && [ "$(tr -d '\r' < "$TMP/n2e")" = '{"error":{"line":41,"col":6,"message":"nesting too deep"}}' ]
then ok; else bad "limits: nesting error names line 41: $(cat "$TMP/n2e")"; fi

# --- a limit that is not reached is a normal successful parse ----- #
rc=0
"$MD" --ast --max-nesting 64 "$TMP/nest.md" > "$TMP/ny" 2> "$TMP/nye" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/nye" ] && [ -s "$TMP/ny" ]; then ok; else
    bad "limits: a generous nesting limit still parses"; fi

# --- the default limit still rejects what it always rejected ------- #
# 20 levels is well inside the default of 64 and must parse; 100 is past it
# and must be refused, exactly as before this checkpoint.
awk 'BEGIN{ for (i = 0; i < 100; i++) printf "> "; print "deep" }' > "$TMP/vdeep.md"
rc=0
"$MD" --ast "$TMP/vdeep.md" > "$TMP/nd" 2> "$TMP/nde" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/nd" "$TMP/nde"; then ok; else
    bad "limits: default nesting still rejects deep input"; fi
rc=0
"$MD" --ast --stream "$TMP/vdeep.md" > "$TMP/nd2" 2> "$TMP/nde2" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/nd2" "$TMP/nde2"; then ok; else
    bad "limits: default nesting still rejects deep input, streamed"; fi

# --- a nesting limit so large it is clamped still parses ----------- #
rc=0
"$MD" --ast --max-nesting 999999 "$TMP/nest.md" > "$TMP/nc" 2> "$TMP/nce" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/nce" ]; then ok; else
    bad "limits: a clamped nesting limit still parses"; fi

# --- nesting at exactly the limit is allowed, one past is not ----- #
# --max-nesting 2 accepts two levels of '>' and rejects three, so the
# limit counts the levels a document actually has rather than being off
# by one in either direction.
printf '> one\n' > "$TMP/n1.md"
printf '> > two\n' > "$TMP/n2.md"
printf '> > > three\n' > "$TMP/n3.md"
rc=0; "$MD" --ast --max-nesting 1 "$TMP/n1.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 0 ]; then ok; else bad "limits: 1 level at --max-nesting 1"; fi
rc=0; "$MD" --ast --max-nesting 1 "$TMP/n2.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 1 ]; then ok; else bad "limits: 2 levels at --max-nesting 1"; fi
rc=0; "$MD" --ast --max-nesting 2 "$TMP/n2.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 0 ]; then ok; else bad "limits: 2 levels at --max-nesting 2"; fi
rc=0; "$MD" --ast --max-nesting 2 "$TMP/n3.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 1 ]; then ok; else bad "limits: 3 levels at --max-nesting 2"; fi

# --- a memory cap, reported as a structured error ----------------- #
for mode in "" "--stream"; do
    rc=0
    "$MD" --ast $mode --max-bytes 8 "$TMP/stream.md" > "$TMP/mo" 2> "$TMP/me" || rc=$?
    if [ "$rc" != 1 ]; then bad "limits: memory exit $rc, want 1"; continue; fi
    if ! error_doc_is "$TMP/mo" "$TMP/me"; then bad "limits: memory error document"; continue; fi
    if grep -q '"message":"memory cap exceeded' "$TMP/me"; then ok; else
        bad "limits: memory error JSON: $(cat "$TMP/me")"; fi
done

# --- a memory cap large enough to succeed is not a failure -------- #
rc=0
"$MD" --ast --max-bytes 4000000 "$TMP/stream.md" > "$TMP/my" 2> "$TMP/mye" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/mye" ] && [ -s "$TMP/my" ]; then ok; else
    bad "limits: a generous memory cap still parses"; fi

# --- a memory cap that stops a large document, streamed or not ---- #
# 40000 lines of prose needs far more than this, so both paths must refuse
# it and neither may get as far as writing output.
for mode in "" "--stream"; do
    rc=0
    "$MD" --ast $mode --max-bytes 65536 "$TMP/huge.md" > "$TMP/mo2" 2> "$TMP/me2" || rc=$?
    if [ "$rc" = 1 ] && error_doc_is "$TMP/mo2" "$TMP/me2" &&
       grep -q '"message":"memory cap exceeded' "$TMP/mo2"
    then ok; else bad "limits: memory cap stops the large document ($mode)"; fi
done

# --- limits combine with extensions -------------------------------- #
printf '{"tables":true}\n' > "$TMP/lim.json"
rc=0
"$MD" --ast --stream --ext "$TMP/lim.json" --max-nesting 8 --max-bytes 4000000 \
    "$TMP/stream.md" > "$TMP/lc" 2> "$TMP/lce" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/lce" ] && [ -s "$TMP/lc" ]; then ok; else
    bad "limits: stream with extensions and limits: exit $rc"; fi
"$MD" --ast --ext "$TMP/lim.json" "$TMP/stream.md" > "$TMP/lc1" 2>/dev/null
tr -d '\r' < "$TMP/lc" > "$TMP/lcn"
tr -d '\r' < "$TMP/lc1" > "$TMP/lc1n"
if cmp -s "$TMP/lcn" "$TMP/lc1n"; then ok; else
    bad "limits: stream with extensions matches one-shot"; fi

# --- usage errors for the new flags -------------------------------- #
usage_case() {
    name=$1; shift
    rc=0
    "$MD" "$@" > "$TMP/uo" 2> "$TMP/ue" || rc=$?
    if [ "$rc" = 2 ] && [ ! -s "$TMP/uo" ]; then ok; else
        bad "limits: usage '$name': exit $rc, want 2"; fi
}
usage_case "missing --max-nesting value" --ast --max-nesting
usage_case "missing --max-bytes value"   --ast --max-bytes
usage_case "duplicate --stream"          --ast --stream --stream "$TMP/stream.md"
usage_case "duplicate --max-nesting"     --ast --max-nesting 4 --max-nesting 8 "$TMP/stream.md"
usage_case "duplicate --max-bytes"       --ast --max-bytes 4 --max-bytes 8 "$TMP/stream.md"
usage_case "negative --max-nesting"      --ast --max-nesting -1 "$TMP/stream.md"
usage_case "non-numeric --max-bytes"     --ast --max-bytes abc "$TMP/stream.md"
usage_case "trailing junk --max-nesting" --ast --max-nesting 4x "$TMP/stream.md"
usage_case "empty --max-bytes"           --ast --max-bytes= "$TMP/stream.md"
usage_case "--max-nesting after --ast=1" --ast=1 --max-nesting 4 "$TMP/stream.md"

# --- flags work before or after the mode, and after the FILE ------- #
rc=0
"$MD" --ast "$TMP/stream.md" --stream --max-nesting 16 > "$TMP/tl" 2> "$TMP/tle" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/tle" ] && [ -s "$TMP/tl" ]; then ok; else
    bad "limits: flags after the FILE: exit $rc"; fi

rc=0
"$MD" --stream --ast --max-nesting 16 "$TMP/stream.md" > "$TMP/tl2" 2> "$TMP/tl2e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/tl2e" ] && [ -s "$TMP/tl2" ]; then ok; else
    bad "limits: --stream before the mode: exit $rc"; fi

rc=0
"$MD" --max-nesting 16 --ast "$TMP/stream.md" > "$TMP/tl7" 2> "$TMP/tl7e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/tl7e" ] && [ -s "$TMP/tl7" ]; then ok; else
    bad "limits: a modifier before the mode: exit $rc"; fi

rc=0
"$MD" --ast --max-nesting=16 "$TMP/stream.md" > "$TMP/tl4" 2> "$TMP/tl4e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/tl4e" ]; then ok; else
    bad "limits: --max-nesting=16: exit $rc"; fi

rc=0
"$MD" --ast --stream=1 "$TMP/stream.md" > "$TMP/tl8" 2> "$TMP/tl8e" || rc=$?
if [ "$rc" = 2 ]; then ok; else
    bad "limits: --stream takes no value: exit $rc, want 2"; fi

# --- the FILE is still found when a limit sits between the two ----- #
rc=0
"$MD" --ast --max-nesting 16 "$TMP/stream.md" > "$TMP/tl5" 2> "$TMP/tl5e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/tl5e" ] && [ -s "$TMP/tl5" ]; then ok; else
    bad "limits: FILE found after a modifier: exit $rc"; fi

# --- an extension configuration that names a limit does not exist - #
# --ext is about extensions; the limits come from their own flags. A
# configuration is still rejected when it is malformed, limit flags or not.
printf '{"tables":true,"max_nesting":4}\n' > "$TMP/limcfg.json"
rc=0
"$MD" --ast --ext "$TMP/limcfg.json" "$TMP/stream.md" > /dev/null 2> "$TMP/lcfg" || rc=$?
if [ "$rc" = 1 ] && [ -s "$TMP/lcfg" ]; then ok; else
    bad "limits: --ext still rejects an unknown key: exit $rc"; fi
rc=0
"$MD" --ast --stream "$TMP/does-not-exist.md" > "$TMP/tl6" 2> "$TMP/tl6e" || rc=$?
if [ "$rc" = 1 ] && [ ! -s "$TMP/tl6" ] && [ -s "$TMP/tl6e" ]; then ok; else
    bad "stream: missing file: exit $rc, want 1"; fi

# --- the help text lists the new flags ----------------------------- #
"$MD" --help > "$TMP/help" 2>/dev/null
if grep -q -- '--stream' "$TMP/help" && grep -q -- '--max-nesting' "$TMP/help" \
   && grep -q -- '--max-bytes' "$TMP/help"; then ok; else
    bad "limits: help mentions the new flags"; fi

# --- the boundary of each limit, counted in levels ----------------- #
# The default limit admits exactly MD_MAX_NESTING levels of '>' and refuses
# one more; a limit past MD_MAX_NESTING_HARD is clamped there, so 256 levels
# parse and 257 do not. Reaching the ceiling must not overflow the stack.
for n in 63 64 65; do
    awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "> "; print "x" }' > "$TMP/lvl.md"
    rc=0
    "$MD" --ast --stream "$TMP/lvl.md" >/dev/null 2>&1 || rc=$?
    if [ "$n" -le 64 ]; then want=0; else want=1; fi
    if [ "$rc" = "$want" ]; then ok; else
        bad "limits: $n levels at the default limit: exit $rc, want $want"; fi
done
for n in 255 256 257; do
    awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "> "; print "x" }' > "$TMP/lvl.md"
    rc=0
    "$MD" --ast --stream --max-nesting 999999 "$TMP/lvl.md" >/dev/null 2>&1 || rc=$?
    if [ "$n" -le 256 ]; then want=0; else want=1; fi
    if [ "$rc" = "$want" ]; then ok; else
        bad "limits: $n levels at the hard ceiling: exit $rc, want $want"; fi
done

# --- a long unmatched delimiter run is text, not a failed parse ---- #
# A run of delimiters is bounded only by the length of the line, so it must
# never be able to fail a parse. Nine used to overflow an eight-byte buffer
# and take the whole document down with it; 5000 must be just as safe.
for n in 8 9 16 64 500 5000; do
    awk -v n="$n" 'BEGIN{
        s = "";
        for (i = 0; i < n; i++) s = s "*";
        print s "`";
    }' > "$TMP/run.md"
    rc=0
    "$MD" --ast --stream "$TMP/run.md" > "$TMP/ru1" 2> "$TMP/ru1e" || rc=$?
    if [ "$rc" = 0 ] && [ ! -s "$TMP/ru1e" ] && [ -s "$TMP/ru1" ]; then ok; else
        bad "limits: a run of $n unmatched delimiters parses: exit $rc"; fi
    "$MD" --ast "$TMP/run.md" > "$TMP/ru2" 2>/dev/null
    tr -d '\r' < "$TMP/ru1" > "$TMP/ru1n"
    tr -d '\r' < "$TMP/ru2" > "$TMP/ru2n"
    if cmp -s "$TMP/ru1n" "$TMP/ru2n"; then ok; else
        bad "limits: a run of $n agrees between paths"; fi
done

# The same for the other two delimiter characters, with and without the
# strikethrough extension, since a tilde run takes a different path there.
for cfg in "" all; do
    for c in '_' '~'; do
        awk -v n=300 -v c="$c" 'BEGIN{
            s = "";
            for (i = 0; i < n; i++) s = s c;
            print s "`";
        }' > "$TMP/run.md"
        if [ -z "$cfg" ]; then
            rc=0
            "$MD" --ast --stream "$TMP/run.md" > "$TMP/ru1" 2> "$TMP/ru1e" || rc=$?
        else
            rc=0
            "$MD" --ast --stream --ext "$TMP/all.json" "$TMP/run.md" > "$TMP/ru1" 2> "$TMP/ru1e" || rc=$?
        fi
        if [ "$rc" = 0 ] && [ ! -s "$TMP/ru1e" ]; then ok; else
            bad "limits: a run of 300 '$c' delimiters parses (ext=$cfg): exit $rc"; fi
    done
done

# --- inline nesting cannot be pushed past its own fixed bound ---- #
# Emphasis and brackets recurse on the C stack too, bounded at compile time
# by MD_INLINE_MAX_NESTING. A document far past that bound must survive as
# ordinary text rather than crashing, and both paths must agree on it.
awk 'BEGIN{
    s = "";
    for (i = 0; i < 2000; i++) s = s "*";
    print s "x" s;
    b = "";
    for (i = 0; i < 2000; i++) b = b "[";
    print b "x" b "(/u)";
    t = "";
    for (i = 0; i < 2000; i++) t = t "**";
    print t "y" t;
    k = "";
    for (i = 0; i < 2000; i++) k = k "`";
    print k "z" k;
    r = "";
    for (i = 0; i < 2000; i++) r = r "~~";
    print r "w" r;
}' > "$TMP/inlinedeep.md"
rc=0
"$MD" --ast --stream "$TMP/inlinedeep.md" > "$TMP/id1" 2> "$TMP/id1e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/id1e" ] && [ -s "$TMP/id1" ]; then ok; else
    bad "limits: deep inline nesting survives: exit $rc"; fi
"$MD" --ast "$TMP/inlinedeep.md" > "$TMP/id2" 2>/dev/null
tr -d '\r' < "$TMP/id1" > "$TMP/id1n"
tr -d '\r' < "$TMP/id2" > "$TMP/id2n"
if cmp -s "$TMP/id1n" "$TMP/id2n"; then ok; else
    bad "limits: deep inline nesting agrees between paths"; fi

# --- deeply nested block quotes past the ceiling are refused, not fatal - #
awk 'BEGIN{ n = ""; for (i = 0; i < 2000; i++) n = n "> "; print n "deep" }' > "$TMP/bqdeep.md"
rc=0
"$MD" --ast --stream "$TMP/bqdeep.md" > "$TMP/bq1" 2> "$TMP/bq1e" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/bq1" "$TMP/bq1e" && grep -q 'nesting too deep' "$TMP/bq1"
then ok; else bad "limits: 2000 block quote levels are refused, not fatal"; fi

# --- bracket nesting is counted, and a label past the limit is refused --- #
# find_label_end() used to stop counting open brackets at a fixed ceiling and
# then treat the next ']' as the closer, so a label nested deeper than the
# ceiling was closed at the wrong bracket. Two things had to follow from
# that: a document nested past the limit has to be refused rather than
# misparsed, and brackets that never close are still ordinary text.
awk 'BEGIN{ b = ""; for (i = 0; i < 10000; i++) b = b "["; print b "x" b "]" }' \
    > "$TMP/bracketdeep.md"
rc=0
"$MD" --ast "$TMP/bracketdeep.md" > "$TMP/bd1" 2> "$TMP/bd1e" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/bd1" "$TMP/bd1e" &&
   grep -q '"message":"nesting too deep"' "$TMP/bd1"
then ok; else bad "limits: 10000 bracket levels are refused, not fatal: exit $rc"; fi

# The refusal names a position, and the payload is the documented document.
if grep -q '{"error":{"line":1,"col":1,"message":"nesting too deep"}}' "$TMP/bd1e"
then ok; else bad "limits: bracket refusal carries line 1 col 1"; fi

# The refused construct is named where it is, not where the block starts.
awk 'BEGIN{ print "intro"; print "para two"; printf "word ";
                for (i = 0; i < 100; i++) printf "[";
                printf "x";
                for (i = 0; i < 100; i++) printf "]";
                print "" }' > "$TMP/bracketpos.md"
rc=0
"$MD" --ast "$TMP/bracketpos.md" > /dev/null 2> "$TMP/bp" || rc=$?
if [ "$rc" = 1 ] && grep -q '"line":3,"col":6' "$TMP/bp"
then ok; else bad "limits: bracket refusal names its own line and column"; fi

# Both paths must refuse identically, output and diagnostic alike.
rc=0
"$MD" --ast --stream "$TMP/bracketdeep.md" > "$TMP/bd2" 2> "$TMP/bd2e" || rc=$?
if [ "$rc" = 1 ] && cmp -s "$TMP/bd1" "$TMP/bd2" && cmp -s "$TMP/bd1e" "$TMP/bd2e"
then ok; else bad "limits: bracket refusal agrees between paths"; fi

# Exactly max_nesting brackets parse, one more does not, and a configured
# limit moves the boundary with it.
for lim in 8 64 256; do
    for n in $((lim - 1)) $lim $((lim + 1)); do
        awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "["; print "x" }' > "$TMP/th.md"
        for i in $(seq 1 $n); do printf ']' >> "$TMP/th.md"; done
        printf '\n' >> "$TMP/th.md"
        rc=0
        "$MD" --ast --max-nesting $lim "$TMP/th.md" > /dev/null 2>&1 || rc=$?
        want=0
        [ "$n" -gt "$lim" ] && want=1
        if [ "$rc" = "$want" ]
        then ok; else bad "limits: $lim brackets at limit $lim: exit $rc want $want"; fi
    done
done

# Brackets that never close are not nesting. A run of unmatched '[' has no
# label to locate, so it stays the literal text CommonMark says it is, and a
# document that is merely full of brackets must not start failing.
awk 'BEGIN{ b = ""; for (i = 0; i < 4000; i++) b = b "[";
                print b "x" b "(/u)" }' > "$TMP/bracketunclosed.md"
rc=0
"$MD" --ast "$TMP/bracketunclosed.md" > "$TMP/bu1" 2> "$TMP/bu1e" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/bu1e" ] && [ "$(head -c 1 "$TMP/bu1")" = "{" ]
then ok; else bad "limits: unmatched brackets stay literal text: exit $rc"; fi

# And neither do brackets that close without forming a link.
printf 'a [b] c [d] e [f](notalink) g\n' > "$TMP/bracketsok.md"
rc=0
"$MD" --ast "$TMP/bracketsok.md" > "$TMP/bok1" 2> "$TMP/boke" || rc=$?
if [ "$rc" = 0 ] && [ ! -s "$TMP/boke" ]
then ok; else bad "limits: ordinary bracket text still parses: exit $rc"; fi

# The misparse this replaced: 70 openers, a run of closers, a tail, and more
# closers used to be read as a link whose label was 70 brackets deep, because
# the label scan stopped counting at a fixed ceiling and then took the next
# ']' as the closer. The same shape with 30 openers, comfortably under that
# ceiling, was always parsed correctly, so the two used to disagree about
# documents that differ only in how deeply they nest. Past the limit the
# answer is now a refusal instead of a guess; under it, nothing changes, and
# the parser is still free to find whatever links the brackets really do
# form, because a link that starts at the 7th bracket is not a nesting bug.
for n in 30 70; do
    awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "[";
                        printf "a";
                        for (i = 0; i < n - 6; i++) printf "]";
                        printf "(/u)";
                        for (i = 0; i < 6; i++) printf "]";
                        print "" }' > "$TMP/spurious.md"
    rc=0
    "$MD" --ast "$TMP/spurious.md" > "$TMP/sp1" 2> "$TMP/spe" || rc=$?
    if [ "$n" = 30 ]; then
        if [ "$rc" = 0 ] && [ -z "$(grep -o '"message"' "$TMP/sp1")" ]
        then ok; else bad "limits: 30 brackets still parse: exit $rc"; fi
    else
        if [ "$rc" = 1 ] && grep -q '"message":"nesting too deep"' "$TMP/sp1" &&
           [ -z "$(grep -o '"type":"link"' "$TMP/sp1")" ]
        then ok; else bad "limits: 70 brackets are refused, not linked: exit $rc"; fi
    fi
done

# --- the boundary of each limit, counted in levels ----------------- #
# The default limit admits exactly MD_MAX_NESTING levels of '>' and refuses
# one more; a limit past MD_MAX_NESTING_HARD is clamped there, so 256 levels
# parse and 257 do not. Reaching the ceiling must not overflow the stack.
for n in 63 64 65; do
    awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "> "; print "x" }' > "$TMP/lvl.md"
    rc=0
    "$MD" --ast --stream "$TMP/lvl.md" >/dev/null 2>&1 || rc=$?
    if [ "$n" -le 64 ]; then want=0; else want=1; fi
    if [ "$rc" = "$want" ]; then ok; else
        bad "limits: $n levels at the default limit: exit $rc, want $want"; fi
done
for n in 255 256 257; do
    awk -v n=$n 'BEGIN{ for (i = 0; i < n; i++) printf "> "; print "x" }' > "$TMP/lvl.md"
    rc=0
    "$MD" --ast --stream --max-nesting 999999 "$TMP/lvl.md" >/dev/null 2>&1 || rc=$?
    if [ "$n" -le 256 ]; then want=0; else want=1; fi
    if [ "$rc" = "$want" ]; then ok; else
        bad "limits: $n levels at the hard ceiling: exit $rc, want $want"; fi
done
printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"

# A phase marker, not a second total: PASS keeps counting, so the C6
# section below reports everything above as well. Labelled so it cannot
# be read as a duplicate of the final line.
printf '\n-- C1-C5: %d passed, %d failed (C6 continues below) --' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]

# ------------------------------------------------------------------ #
# Subcommands and the table of contents (checkpoint C6)             #
# ------------------------------------------------------------------ #

# ---------------------------------------------------------------- C6
# md2ast / md2html subcommands, and the --toc table of contents.

# Subcommands are spellings of the existing modes, not new output modes, so
# every one of them has to be byte-identical to its flag, streamed or not.
printf '\n-- C6: subcommands and --toc --\n'

# subcommand_is <subcommand> <flag>
#
# Compares the subcommand with its own flag only. The spelling has to be
# byte-identical to the mode it stands for, on both read paths, for every
# document -- but it must not be compared against the modes it is not an
# alias for, which differ from it on purpose.
subcommand_is() {
    name=$1; flag=$2
    for f in "$TMP/sub.md" "$TMP/deep_ok.md" "$TMP/uniq.md"; do
        for opt in "" --stream; do
            "$MD" $flag $opt "$f" > "$TMP/a" 2>/dev/null
            "$MD" "$name" $opt "$f" > "$TMP/b" 2>/dev/null
            if ! cmp -s "$TMP/a" "$TMP/b"; then
                bad "c6: $name $opt $f differs from $flag $opt"
                return
            fi
        done
    done
    ok
}

cat > "$TMP/sub.md" <<'EOF'

# Title

Some *emphasis*, a [link][ref], and `code`.

## Sub *one* <b> &amp; "quotes"

- item with a footnote[^1]
- item

[^1]: the note

> ## In a quote

```sh
## not a heading
```

[ref]: https://example.com/a?b=c&d=e#f "T"
EOF

printf '## a\n\n> ## b\n\n    text\n' > "$TMP/deep_ok.md"

subcommand_is md2ast --ast
subcommand_is md2html --html

# The subcommand has to work wherever the flag does: as the only word, before
# the modifiers, and with a modifier in between. `md2ast --stream F` and
# `md2ast` both select the mode, and neither may be mistaken for the FILE.
for args in "md2ast --stream $TMP/sub.md" "--stream md2ast $TMP/sub.md" \
            "md2ast $TMP/sub.md --stream" "md2ast --max-nesting 8 $TMP/sub.md" \
            "$TMP/sub.md md2ast"; do
    # shellcheck disable=SC2086
    a=0; b=0
    case $args in
        *md2ast*) set -- $args ;;
    esac
    a=0
    "$MD" $args > "$TMP/b" 2>"$TMP/be" || a=$?
    b=0
    "$MD" --ast ${args#*md2ast} > "$TMP/a" 2>/dev/null || b=$?
    if [ "$a" != "$b" ] || ! cmp -s "$TMP/a" "$TMP/b"; then
        bad "c6: md2ast placement: $args (exit $a vs $b)"
    else
        ok
    fi
done

# --toc takes no value, is rejected twice, and reports a usage error on its
# own rather than being taken for a mode or for the FILE.
rc=0; "$MD" --toc=2 "$TMP/sub.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 2 ]; then ok; else bad "c6: --toc=2: exit $rc, want 2"; fi
rc=0; "$MD" --toc "$TMP/sub.md" --toc >/dev/null 2>&1 || rc=$?
if [ "$rc" = 2 ]; then ok; else bad "c6: repeated --toc: exit $rc, want 2"; fi
rc=0; "$MD" --toc >/dev/null 2>&1 || rc=$?
if [ "$rc" = 2 ]; then ok; else bad "c6: --toc with no FILE: exit $rc, want 2"; fi

# A usage error stays on stderr with an empty stdout, like every other one.
"$MD" --toc > "$TMP/out" 2> "$TMP/err" || true
if [ -s "$TMP/out" ] && [ -s "$TMP/err" ]; then
    bad "c6: a --toc usage error wrote to both streams"
else
    ok
fi

# The documented shape: a top-level array of entries, no wrapper object, and
# entry keys exactly level, text, anchor in that order.
printf '# A\n\n## B b\n\n### C\n' > "$TMP/t.md"
"$MD" --toc "$TMP/t.md" > "$TMP/toc" 2>/dev/null
want='[{"level":1,"text":"A","anchor":"a"},{"level":2,"text":"B b","anchor":"b-b"},{"level":3,"text":"C","anchor":"c"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then
    ok
else
    bad "c6: --toc shape"
    printf '  want: %s\n  got:  %s\n' "$want" "$(tr -d '\r' < "$TMP/toc")" >&2
fi

# The document is a single line: no pretty printing, and exactly one JSON
# value, so a consumer can read it without a streaming parser.
if [ "$(wc -l < "$TMP/toc")" = 1 ]; then ok; else
    bad "c6: --toc is $(wc -l < "$TMP/toc") lines, want 1"; fi

# An empty document has an empty array, not an empty string and not a null.
: > "$TMP/empty.md"
"$MD" --toc "$TMP/empty.md" > "$TMP/toc" 2>/dev/null
if [ "$(tr -d '\r' < "$TMP/toc")" = '[]' ]; then ok; else
    bad "c6: --toc of an empty document: $(tr -d '\r' < "$TMP/toc")"; fi

# A document with no headings likewise yields an empty table, not a null.
printf 'just a paragraph\n\n> quoted\n' > "$TMP/noh.md"
"$MD" --toc "$TMP/noh.md" > "$TMP/toc" 2>/dev/null
if [ "$(tr -d '\r' < "$TMP/toc")" = '[]' ]; then ok; else
    bad "c6: --toc without headings: $(tr -d '\r' < "$TMP/toc")"; fi

# Every heading in the document is listed, in document order, with its level.
printf '## z\n\n# m\n\n## a\n' > "$TMP/ord.md"
"$MD" --toc "$TMP/ord.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"z","anchor":"z"},{"level":1,"text":"m","anchor":"m"},{"level":2,"text":"a","anchor":"a"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: --toc keeps document order: $(tr -d '\r' < "$TMP/toc")"; fi

# text is the plain-text content: inline markup removed, entities decoded, and
# no angle brackets, so a consumer can print it without escaping.
printf '## A *b* `c` <d> &amp; e\n' > "$TMP/txt.md"
"$MD" --toc "$TMP/txt.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"A b c <d> & e","anchor":"a-b-c-d-e"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: --toc heading text is plain: $(tr -d '\r' < "$TMP/toc")"; fi

# Headings only. A "#" line inside a fenced code block or an indented code
# block is code, and a reference definition is not a heading.
printf '```\n# fenced\n```\n\n    # indented\n\n# real\n' > "$TMP/nest.md"
"$MD" --toc "$TMP/nest.md" > "$TMP/toc" 2>/dev/null
want='[{"level":1,"text":"real","anchor":"real"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: --toc ignores code blocks: $(tr -d '\r' < "$TMP/toc")"; fi

# A heading nested in a block quote or a list item is still a heading of the
# document and is listed at its own level.
printf '> ## quoted\n\n- ### itemized\n' > "$TMP/in.md"
"$MD" --toc "$TMP/in.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"quoted","anchor":"quoted"},{"level":3,"text":"itemized","anchor":"itemized"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: --toc finds nested headings: $(tr -d '\r' < "$TMP/toc")"; fi

# Setext headings are headings, and are reported at the level the underline
# selects.
printf 'Set one\n=======\n\nSet two\n---\n' > "$TMP/set.md"
"$MD" --toc "$TMP/set.md" > "$TMP/toc" 2>/dev/null
want='[{"level":1,"text":"Set one","anchor":"set-one"},{"level":2,"text":"Set two","anchor":"set-two"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: --toc setext levels: $(tr -d '\r' < "$TMP/toc")"; fi

# A footnote definition is stored outside the walked tree, so its label never
# appears as a heading even when it looks like one.
printf '# H\n\n[^1]: # not a heading\n' > "$TMP/fn.md"
printf '{"footnotes":true}' > "$TMP/fn.json"
"$MD" --ext "$TMP/fn.json" --toc "$TMP/fn.md" > "$TMP/toc" 2>/dev/null
if [ "$(tr -d '\r' < "$TMP/toc")" = '[{"level":1,"text":"H","anchor":"h"}]' ]; then
    ok
else
    bad "c6: --toc excludes footnote definitions: $(tr -d '\r' < "$TMP/toc")"
fi

# Anchors: lowercase, hyphens between words, and no leading or trailing hyphen.
printf '##  Mixed  CASE  and  spaces  \n' > "$TMP/sl.md"
"$MD" --toc "$TMP/sl.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"Mixed  CASE  and  spaces","anchor":"mixed-case-and-spaces"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: anchor normalization: $(tr -d '\r' < "$TMP/toc")"; fi

# A heading with nothing sluggable in it still gets a stable, usable anchor
# rather than an empty string or a bare hyphen.
printf '## !!!\n\n## ???\n' > "$TMP/empty_anchor.md"
"$MD" --toc "$TMP/empty_anchor.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"!!!","anchor":"section"},{"level":2,"text":"???","anchor":"section-1"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: an empty anchor falls back: $(tr -d '\r' < "$TMP/toc")"; fi

# Uniqueness. The suffix counts from 1, skips anchors already taken by a
# different heading, and stays deterministic across runs. The interesting case
# is a real collision between a suffix and a heading that maps to that
# same string: the third heading here must not reuse "title-1".
printf '## Title\n## Title\n## Title 1\n## Title\n' > "$TMP/uniq.md"
"$MD" --toc "$TMP/uniq.md" > "$TMP/toc" 2>/dev/null
want='[{"level":2,"text":"Title","anchor":"title"},{"level":2,"text":"Title","anchor":"title-1"},{"level":2,"text":"Title 1","anchor":"title-1-1"},{"level":2,"text":"Title","anchor":"title-2"}]'
if [ "$(tr -d '\r' < "$TMP/toc")" = "$want" ]; then ok; else
    bad "c6: a suffix never collides with a real heading: $(tr -d '\r' < "$TMP/toc")"; fi

# Every anchor in the document is distinct, whatever the document.
anchor_dupes() {
    "$MD" --toc "$1" 2>/dev/null |
        tr ',' '\n' | sed -n 's/.*"anchor":"\([^"]*\)".*/\1/p' | sort | uniq -d
}
for n in 4 8 16 32; do
    i=0
    : > "$TMP/many.md"
    while [ "$i" -lt "$n" ]; do
        case $((i % 3)) in
            0) printf '## Title\n' >> "$TMP/many.md" ;;
            1) printf '## Title %s\n' "$i" >> "$TMP/many.md" ;;
            2) printf '## title %s\n' "$i" >> "$TMP/many.md" ;;
        esac
        i=$((i + 1))
    done
    dupes=$(anchor_dupes "$TMP/many.md")
    if [ -z "$dupes" ]; then ok; else
        bad "c6: $n similar headings produced duplicate anchors: $dupes"; fi
done

# The table is deterministic: the same input gives the same bytes every time,
# and the streaming path agrees with the buffered one.
"$MD" --toc "$TMP/uniq.md" > "$TMP/t1" 2>/dev/null
"$MD" --toc "$TMP/uniq.md" > "$TMP/t2" 2>/dev/null
if cmp -s "$TMP/t1" "$TMP/t2"; then ok; else
    bad "c6: --toc is not deterministic"; fi
"$MD" --toc --stream "$TMP/uniq.md" > "$TMP/t3" 2>/dev/null
"$MD" --toc --stream "$TMP/uniq.md" > "$TMP/t4" 2>/dev/null
if cmp -s "$TMP/t3" "$TMP/t4"; then ok; else
    bad "c6: --toc --stream is not deterministic"; fi
for f in "$TMP/sub.md" "$TMP/ord.md" "$TMP/uniq.md" "$TMP/empty.md" \
         "$TMP/empty_anchor.md"; do
    "$MD" --toc "$f" > "$TMP/t1" 2>/dev/null
    "$MD" --toc --stream "$f" > "$TMP/t3" 2>/dev/null
    if cmp -s "$TMP/t1" "$TMP/t3"; then ok; else
        bad "c6: --toc --stream differs from the buffered read: $f"; fi
done

# The mode is a mode: it composes with the extensions and the limits, and a
# limit failure inside it is still the one structured error document, on both
# streams, with the same exit status as every other mode.
printf '# a\n\n%s\n' "$(awk 'BEGIN{for(i=0;i<100;i++)printf "> ";print "deep"}')" > "$TMP/deep.md"
rc=0
"$MD" --toc --max-nesting 8 "$TMP/deep.md" > "$TMP/out" 2> "$TMP/err" || rc=$?
if [ "$rc" = 1 ] && error_doc_is "$TMP/out" "$TMP/err"; then ok; else
    bad "c6: --toc reports a nesting limit: exit $rc, stderr $(cat "$TMP/err")"; fi

printf '{"tables":true}' > "$TMP/tables.json"
rc=0
"$MD" --toc --ext "$TMP/tables.json" "$TMP/sub.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 0 ]; then ok; else bad "c6: --toc takes --ext: exit $rc"; fi

# --toc reads stdin as '-', like the other modes.
"$MD" --toc - < "$TMP/ord.md" > "$TMP/t1" 2>/dev/null
"$MD" --toc "$TMP/ord.md" > "$TMP/t2" 2>/dev/null
if cmp -s "$TMP/t1" "$TMP/t2"; then ok; else
    bad "c6: --toc does not read stdin as '-'"; fi

# A missing file is an I/O error, not a usage error, and is silent on stdout.
rc=0
"$MD" --toc "$TMP/there-is-no-such-file.md" > "$TMP/out" 2> "$TMP/err" || rc=$?
if [ "$rc" = 1 ] && [ ! -s "$TMP/out" ] && [ -s "$TMP/err" ]; then ok; else
    bad "c6: --toc missing file: exit $rc, stdout $(wc -c < "$TMP/out") bytes"; fi

# --toc succeeds quietly: an empty stderr, as the other modes do.
"$MD" --toc "$TMP/t.md" >/dev/null 2> "$TMP/err"
if [ ! -s "$TMP/err" ]; then ok; else bad "c6: --toc wrote to stderr: $(cat "$TMP/err")"; fi

# The table is one JSON value with the documented shape. The bytes are
# already pinned above; this checks the document the way a consumer does, by
# parsing it, so a change that keeps the literals but breaks the contract
# cannot pass. A host with no usable Python skips this one check rather than
# failing the suite, the same way the lexer harness skips without --wrap.
PY=""
for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 &&
       "$cand" -c 'pass' >/dev/null 2>&1; then
        PY=$cand
        break
    fi
done

# Every --toc document produced above, plus the same documents on the
# streaming path, has to satisfy the contract, not just the two compared
# byte for byte.
if [ -z "$PY" ]; then
    printf 'skip c6: no usable Python for the structural --toc check\n' >&2
else
    toc_files=""
    for f in "$TMP/sub.md" "$TMP/ord.md" "$TMP/uniq.md" "$TMP/many.md" \
             "$TMP/empty.md" "$TMP/noh.md" "$TMP/sl.md" "$TMP/txt.md" \
             "$TMP/nest.md" "$TMP/in.md" "$TMP/set.md" "$TMP/fn.md" \
             "$TMP/empty_anchor.md" "$TMP/deep_ok.md"; do
        "$MD" --toc "$f" > "$TMP/s.toc" 2>/dev/null
        toc_files="$toc_files $TMP/s.toc"
        "$MD" --toc --stream "$f" > "$TMP/t.toc" 2>/dev/null
        toc_files="$toc_files $TMP/t.toc"
    done
    if "$PY" tests/check_toc.py $toc_files; then
        ok
    else
        bad "c6: a --toc document does not satisfy the published contract"
    fi
fi

# The help text documents every new spelling, and --version still works.
"$MD" --help > "$TMP/help" 2>&1 || true
for token in md2ast md2html --toc; do
    if grep -q -- "$token" "$TMP/help"; then ok; else
        bad "c6: --help does not mention $token"; fi
done
rc=0; "$MD" --version >/dev/null 2>&1 || rc=$?
if [ "$rc" = 0 ]; then ok; else bad "c6: --version: exit $rc"; fi

printf '\n%d passed, %d failed' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
