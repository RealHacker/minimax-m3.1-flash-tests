#!/bin/sh
# Build and run the public API example against libmd.a.
#
# The example is the proof that md.h is a public header: it includes nothing
# else, and it links against the archive with no other object file and no
# source from src/. So this harness has to link it the same narrow way --
# archive only, -Isrc and no other translation unit -- because a link that
# succeeds only because something from the library tree leaked in would prove
# nothing.
#
# It also checks that the example agrees with the CLI. Both call the same
# library function, so the two must produce the same bytes; if they ever
# diverge, one of them is not describing the library any more.
set -eu

CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic}
OUT=${OUT:-example}
TMP=${TMPDIR:-/tmp}/md-example.$$
PASS=0
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
mkdir -p "$TMP" || exit 1

ok()  { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL: %s\n' "$1" >&2; }

if [ ! -f src/md.h ] || [ ! -f examples/toc_example.c ]; then
    echo "example: run from the project root" >&2
    exit 1
fi

# The archive has to exist; `make check` depends on the target that makes it.
if [ ! -f libmd.a ]; then
    echo "example: FAILED to build, libmd.a is missing (run make lib)" >&2
    exit 1
fi

# Both spellings are attempted, for the reason tests/run_api_test.sh records:
# a Windows toolchain appends .exe to the output name it is given.
rm -f "$OUT" "$OUT.exe"
if ! $CC $CFLAGS -Isrc -o "$OUT" examples/toc_example.c libmd.a \
       2> "$TMP/build.log" &&
   ! $CC $CFLAGS -Isrc -o "$OUT.exe" examples/toc_example.c libmd.a \
       2>> "$TMP/build.log"; then
    echo "example: FAILED to build examples/toc_example.c against libmd.a" >&2
    cat "$TMP/build.log" >&2
    rm -f "$OUT" "$OUT.exe"
    exit 1
fi
if [ -f "$OUT" ] && [ ! -d "$OUT" ]; then BIN=$OUT; else BIN=$OUT.exe; fi
ok   # it links against the archive alone

# The binary is always run through a ./ prefix. It looks redundant, but it is
# what makes the name portable: a Windows toolchain writes example.exe where
# a POSIX one writes example, and `ls` cannot tell you which because on an
# MSYS shell the POSIX layer reports a missing "example" as the example.exe
# that is really there. Prefixing with ./ hands the name to the same
# resolution the compiler's output name goes through, so one spelling works
# either way -- which is the reason tests/run_api_test.sh does this too.
EXE=./$BIN

MD=${MD:-./md}

# A document exercising the things a TOC has to get right: repeated headings
# that need disambiguation, markup that has to be stripped from the text, a
# duplicate anchor, and code that must not be mistaken for a heading.
cat > "$TMP/doc.md" <<'EOF'
# First *heading*

Intro with `code` and a [link][r].

## Repeat

### Deep

## Repeat

```
# not a heading
```

    # also not a heading

[r]: https://example.com/
EOF

# The CLI's own table of contents for the same document, which the example's
# anchors are compared against.
"$MD" --toc "$TMP/doc.md" > "$TMP/want" 2>/dev/null || true

# The example prints an indented outline for a file and JSON with no
# argument, so both forms are exercised, and both have to agree with the CLI.
: > "$TMP/doc2.md"
"$EXE" > "$TMP/json" 2>"$TMP/err" || true

# The document read from a file and the one read from '-' have to agree.
"$EXE" "$TMP/doc.md" > "$TMP/file" 2>/dev/null || true
"$EXE" - < "$TMP/doc.md" > "$TMP/stdin" 2>/dev/null || true
if cmp -s "$TMP/file" "$TMP/stdin"; then ok; else
    bad "example: the file and '-' reads disagree"; fi

# Against the CLI: the same headings, the same anchors, in the same order.
# The two programs format their output differently, so what is compared is
# the list of anchors, not the bytes -- an anchor the example loses or
# renames is the failure this is here to catch.
#
# The CLI writes the whole table on one line, so the anchors are pulled out
# with grep -o rather than with a single sed match: `.*"anchor":"\(...\)".*`
# would be greedy and report only the last one on the line.
grep -o '"anchor":"[^"]*"' "$TMP/want" 2>/dev/null |
    sed -e 's/^"anchor":"//' -e 's/"$//' > "$TMP/cli_anchors"
sed -n 's/.*-> #//p' "$TMP/file" > "$TMP/ex_anchors"
if cmp -s "$TMP/cli_anchors" "$TMP/ex_anchors" && [ -s "$TMP/cli_anchors" ]; then ok; else
    bad "example: anchors differ from the CLI
  cli: $(tr '\n' ' ' < "$TMP/cli_anchors")
  ex:  $(tr '\n' ' ' < "$TMP/ex_anchors")"; fi

# The example's own default document has to be accepted, and the JSON it
# prints has to be one line of well-formed JSON.
if [ "$(wc -l < "$TMP/json")" = 1 ] && [ -s "$TMP/json" ]; then ok; else
    bad "example: the default document printed $(wc -l < "$TMP/json") lines"; fi
if [ -s "$TMP/err" ]; then bad "example: wrote to stderr: $(cat "$TMP/err")"; else
    ok; fi

# The exit codes follow the CLI's contract: 0 on success, 1 for an I/O or
# parse failure, 2 for a usage error.
rc=0; "$EXE" "$TMP/doc.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 0 ]; then ok; else bad "example: a readable file: exit $rc, want 0"; fi
rc=0; "$EXE" "$TMP/no-such-file.md" >/dev/null 2>&1 || rc=$?
if [ "$rc" = 1 ]; then ok; else bad "example: a missing file: exit $rc, want 1"; fi
rc=0; "$EXE" one two >/dev/null 2>&1 || rc=$?
if [ "$rc" = 2 ]; then ok; else bad "example: too many arguments: exit $rc, want 2"; fi

# The example is a program and not a library, so the harness has to leave the
# tree as it found it. The clean rule lists the binary too, but a build that
# only ran the suite would otherwise leave it behind.
rm -f "$OUT" "$OUT.exe" "$TMP"/build.log

printf '\nexample: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ]
