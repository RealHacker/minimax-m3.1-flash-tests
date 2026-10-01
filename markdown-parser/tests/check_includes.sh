#!/bin/sh
# Dependency policy check: every #include in src/*.c and src/*.h must be either
# an allowed C11 standard header or a header local to src/.
#
# Kept as a test so `make check` fails if a later checkpoint reaches for a
# non-conforming header (POSIX, platform, or third-party).

set -u

SRC_DIR=${SRC_DIR:-src}

# The allowed C11 standard headers.
ALLOWED="assert.h ctype.h errno.h float.h limits.h locale.h math.h setjmp.h
signal.h stdarg.h stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h
time.h wchar.h wctype.h"

FAIL=0

for file in "$SRC_DIR"/*.c "$SRC_DIR"/*.h; do
    [ -f "$file" ] || continue
    # Strip comments so that includes mentioned in prose are not counted.
    includes=$(sed -e 's://.*::' "$file" |
               sed -e 's:/\*.*\*/::g' |
               grep '^[[:space:]]*#[[:space:]]*include' || true)
    [ -n "$includes" ] || continue

    # shellcheck disable=SC2086
    echo "$includes" | while IFS= read -r line; do
        case $line in
        *'"'*)
            header=$(echo "$line" | sed -e 's/.*"\(.*\)".*/\1/')
            if [ -f "$SRC_DIR/$header" ]; then
                :
            else
                printf 'FAIL: %s: local header "%s" not found in %s/\n' \
                       "$file" "$header" "$SRC_DIR"
                exit 1
            fi
            ;;
        *'<'*)
            header=$(echo "$line" | sed -e 's/.*<\(.*\)>.*/\1/')
            found=0
            for ok in $ALLOWED; do
                [ "$header" = "$ok" ] && found=1 && break
            done
            if [ "$found" = 0 ]; then
                printf 'FAIL: %s: header <%s> is not in the allowed set\n' \
                       "$file" "$header"
                exit 1
            fi
            ;;
        esac
    done || FAIL=1
done

if [ "$FAIL" = 0 ]; then
    printf 'include policy: ok (%s standard, all local headers resolved)\n' \
           "$(grep -h '^[[:space:]]*#[[:space:]]*include' "$SRC_DIR"/*.c "$SRC_DIR"/*.h |
              grep -c '<')"
else
    printf 'include policy: FAILED\n' >&2
    exit 1
fi
