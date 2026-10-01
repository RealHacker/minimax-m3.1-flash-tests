#!/usr/bin/env python3
"""Structural check for the --toc table of contents.

The shell suite pins --toc byte for byte, which is what catches a change to
the output. This file checks the same document the way a consumer would --
by parsing it -- so that the promised contract is verified as a contract and
not only as a literal:

  * the file holds exactly one JSON value, so a reader knows where it ends;
  * the top level is an array of the entries themselves, with no wrapper
    object around them;
  * every entry is an object with exactly the keys "level", "text" and
    "anchor", in that order, since the key order is part of the published
    output (a repeated key is caught here too, which json.loads() would
    otherwise hide by keeping only the last one);
  * "level" is a JSON number in the heading range 1-6;
  * "text" and "anchor" are non-empty strings;
  * "anchor" is lowercase and has no leading or trailing hyphen, because both
    are what makes it usable as an anchor;
  * no two entries share an anchor, because a repeated anchor resolves to the
    wrong heading.

Usage: check_toc.py <toc-file> [...]
"""
import json
import sys


def no_duplicates(pairs):
    """Reject an object that repeats a key.

    json.loads() quietly keeps the last value for a repeated key, so a
    duplicated "anchor" would look like a well-formed entry to every
    comparison below. The published shape says each key appears once, so the
    duplicate is reported instead of being resolved.
    """
    seen = []
    for key, _ in pairs:
        if key in seen:
            raise ValueError("key %r appears more than once" % key)
        seen.append(key)
    return dict(pairs)


def check(path):
    """Return a list of problems; empty means the file is fine."""
    with open(path, "rb") as fh:
        raw = fh.read()

    # The runtime may terminate the line with CRLF on some platforms. A raw
    # CR can never be part of the JSON payload, so dropping it here matches
    # what the rest of the suite compares.
    text = raw.replace(b"\r", b"").decode("utf-8")
    problems = []

    try:
        entries = json.loads(text, object_pairs_hook=no_duplicates)
    except ValueError as exc:
        return ["%s: not one JSON value: %s" % (path, exc)]

    if not isinstance(entries, list):
        return ["%s: the top level is %s, want a list of entries"
                % (path, type(entries).__name__)]

    seen = {}
    for index, entry in enumerate(entries):
        where = "%s: entry %d" % (path, index)
        if not isinstance(entry, dict):
            problems.append("%s is not an object" % where)
            continue
        if list(entry) != ["level", "text", "anchor"]:
            problems.append("%s: keys are %s, want level/text/anchor"
                            % (where, list(entry)))
            continue

        level, text_, anchor = entry["level"], entry["text"], entry["anchor"]

        if not isinstance(level, int) or isinstance(level, bool):
            problems.append("%s: level is %r, want an integer" % (where, level))
        elif not 1 <= level <= 6:
            problems.append("%s: level %d is outside 1-6" % (where, level))

        if not isinstance(text_, str) or not text_:
            problems.append("%s: text is %r, want a non-empty string"
                            % (where, text_))
        if not isinstance(anchor, str) or not anchor:
            problems.append("%s: anchor is %r, want a non-empty string"
                            % (where, anchor))
            continue

        if anchor != anchor.lower():
            problems.append("%s: anchor %r is not lowercase" % (where, anchor))
        if anchor.strip("-") != anchor:
            problems.append("%s: anchor %r has an edge hyphen" % (where, anchor))
        if anchor in seen:
            problems.append("%s: anchor %r already used by entry %d"
                            % (where, anchor, seen[anchor]))
        else:
            seen[anchor] = index

    return problems


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    problems = []
    for path in argv[1:]:
        problems.extend(check(path))
    for problem in problems:
        sys.stderr.write("problem: %s\n" % problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
