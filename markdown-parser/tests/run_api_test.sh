#!/bin/sh
# Build and run the library-level API and leak test.
#
# The test links the library against --wrap shims for malloc/calloc/realloc/
# free so that every allocation the library makes is counted; it also exercises
# the public entry points (md_parse, md_parse_n, md_parse_in, md_parse_inlines,
# the C1 md_parse_blocks* family), the C3 renderers (md_render_html and
# md_render_text), the C4 option and registration entry points, together with
# the arena ownership rules.
#
# --wrap is a GNU ld / lld feature. On a toolchain without it the check is
# reported as skipped rather than failing the suite.
set -eu

CC=${CC:-cc}
OUT=${OUT:-mdapitest}
# The library sources: all of src/ except the CLI, which is a program rather
# than part of the library. Derived rather than written out, so a module added
# to src/ is compiled in instead of showing up later as an undefined reference
# at link time -- which is how both harnesses failed when C6 added src/slug.c
# and src/toc.c to a hardcoded list they shared.
LIB=$(for f in src/*.c; do
    case $f in
        src/cli.c) continue ;;
    esac
    printf '%s ' "$f"
done)
WRAPS="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free"

if [ ! -f src/md.h ]; then
    echo "api: run from the project root" >&2
    exit 1
fi

build() {
    $CC -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc -o "$1" \
        tests/wrap_test.c $LIB $2
}

# Some Windows toolchains silently refuse an output name without the host
# executable suffix, so both spellings are attempted.
rm -f "$OUT" "$OUT.exe"

# A build failure is only a "skip" when the linker is what rejected --wrap.
# A compile error in the test itself has to surface, or a broken test file
# would quietly report itself as an unsupported toolchain.
build_once() {
    if build "$OUT" "$WRAPS" >"$OUT.log" 2>&1; then
        return 0
    fi
    build "$OUT.exe" "$WRAPS" >>"$OUT.log" 2>&1
}

if build_once; then
    :
else
    # Retry without the shims. If that compiles, --wrap is the problem.
    if build "$OUT" "" >"$OUT.log" 2>&1; then
        echo "api: skipped (linker without --wrap support)"
        rm -f "$OUT" "$OUT.exe" "$OUT.log"
        exit 0
    fi
    if build "$OUT.exe" "" >>"$OUT.log" 2>&1; then
        echo "api: skipped (linker without --wrap support)"
        rm -f "$OUT" "$OUT.exe" "$OUT.log"
        exit 0
    fi
    echo "api: FAILED to build tests/wrap_test.c" >&2
    cat "$OUT.log" >&2
    rm -f "$OUT" "$OUT.exe" "$OUT.log"
    exit 1
fi
rm -f "$OUT.log"
if [ -f "$OUT" ] && [ ! -d "$OUT" ]; then
    BIN=$OUT
elif [ -f "$OUT.exe" ]; then
    BIN=$OUT.exe
else
    echo "api: skipped (no test binary produced)"
    rm -f "$OUT" "$OUT.exe"
    exit 0
fi

set +e
"./$BIN"
status=$?
set -e
rm -f "$OUT" "$OUT.exe"
exit $status
