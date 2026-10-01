#!/bin/sh
# Build and run the internal lexer test.
#
# tests/wrap_test.c includes only md.h, because it is the proof that a
# caller needs nothing else.  The normalizer is not public, so its contract
# -- tab stops, CR and CRLF collapsing, NUL becoming U+FFFD, other UTF-8
# surviving, and the memory cap landing on the normalized length rather than
# the raw one -- is pinned here, where a local header may be included.
set -eu

CC=${CC:-cc}
OUT=${OUT:-mdlexertest}
# Every library source: all of src/ except the CLI, which is a program and
# not part of the library. The list is derived rather than written out, so a
# module added to src/ is compiled in instead of showing up later as an
# undefined reference at link time -- which is how both harnesses failed when
# C6 added src/slug.c and src/toc.c to a hardcoded list they shared.
LIB=$(for f in src/*.c; do
    case $f in
        src/cli.c) continue ;;
    esac
    printf '%s ' "$f"
done)

if [ ! -f src/md.h ]; then
    echo "lexer: run from the project root" >&2
    exit 1
fi

build() {
    $CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc -o "$1" \
        tests/lexer_test.c $LIB
}

# Some Windows toolchains refuse an output name without the host executable
# suffix, so both spellings are attempted.
rm -f "$OUT" "$OUT.exe"
if ! build "$OUT" 2>"$OUT.log" && ! build "$OUT.exe" 2>>"$OUT.log"; then
    echo "lexer: FAILED to build tests/lexer_test.c" >&2
    cat "$OUT.log" >&2
    rm -f "$OUT" "$OUT.exe" "$OUT.log"
    exit 1
fi
rm -f "$OUT.log"

if [ -f "$OUT" ] && [ ! -d "$OUT" ]; then
    BIN=$OUT
else
    BIN=$OUT.exe
fi

set +e
"./$BIN"
status=$?
set -e
rm -f "$OUT" "$OUT.exe"
exit $status
