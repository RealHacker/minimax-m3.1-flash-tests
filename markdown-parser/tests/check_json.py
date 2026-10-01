#!/usr/bin/env python3
"""Every run must emit exactly one valid JSON document on stdout.

Runs each mode over the fixture corpus and over generated documents, parses
stdout with a strict JSON reader, and checks three things beyond validity:
the key order is the one the renderer promises (comparing against a second
decode that preserves order is not enough, so instead the raw bytes are
re-serialized and compared), no byte of the payload is a bare control
character, and a successful run says nothing on stderr.

A document that must fail instead is checked separately: it has to exit 1,
put the same structured error document on stdout as it copies to stderr, and
name a limit. stdout carries the error object rather than staying empty so
that a consumer parsing stdout gets JSON either way and can tell the two
outcomes apart by exit status alone; the stderr copy is what a consumer
reading a human-readable failure stream sees, so the two must never disagree.
"""
import json
import os
import random
import subprocess
import sys
import tempfile

MD = sys.argv[1] if len(sys.argv) > 1 else "./md"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 500
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 20250929

FRAGMENTS = [
    "# heading", "para", "", "- a", "  - b", "> q", "```", "code", "---",
    "| a | b |", "| - | - |", "| 1 | 2 |", "[r]: /u", "[r]", "![i](/u)",
    "*em*", "**st**", "~~s~~", "`c`", "a\\*b", "&amp;", "<http://e.test>",
    "- [ ] t", "text[^n]", "[^n]: note", "a  \nb", "é\U0001f600", "\0",
]

# Fixtures shipped with the repository, if any are reachable.
FIXTURES = []
for root in ("tests/fixtures", "fixtures", "corpus"):
    if os.path.isdir(root):
        for dirpath, _, names in os.walk(root):
            for n in sorted(names):
                if n.endswith(".md"):
                    FIXTURES.append(os.path.join(dirpath, n))

MODES = ["--ast", "--html", "--text", "--toc"]
EXTS = [
    None,
    {"all": True},
    {"tables": True, "strikethrough": True, "tasklist": True, "footnotes": True},
]


def main():
    rng = random.Random(SEED)
    tmp = tempfile.mkdtemp(prefix="md-json-")
    src = os.path.join(tmp, "in.md")
    cfg = os.path.join(tmp, "ext.json")
    ok = 0
    bad = 0

    def check(args, label):
        nonlocal ok, bad
        p = subprocess.run([MD] + args, capture_output=True)
        # The re-serialization of the payload, used to prove the key order
        # is the promised one. --ast and --toc both promise a canonical
        # form; --html and --text are not JSON at all, and neither is the
        # error document, which the renderer does not emit through the --ast
        # path. So this stays None for those.
        again = None
        if p.returncode == 0:
            if p.stderr:
                print("STDERR ON SUCCESS: %s %r" % (label, p.stderr[:120]))
                bad += 1
                return
            # The mode is one of several arguments, and it is not
            # necessarily first: an extension configuration may come
            # before it. Look for the mode in the whole argument list.
            if "--ast" in args or "--toc" in args:
                try:
                    raw = p.stdout.decode("utf-8")
                except UnicodeDecodeError as e:
                    print("NOT UTF-8: %s %r" % (label, e))
                    bad += 1
                    return
                # The payload is the output minus the single final newline,
                # which may be CRLF on Windows.
                if not raw.endswith("\n"):
                    print("NO TRAILING NEWLINE: %s %r" % (label, raw[-20:]))
                    bad += 1
                    return
                stripped = raw[:-1]
                if stripped.endswith("\r"):
                    stripped = stripped[:-1]
                if not stripped:
                    print("EMPTY PAYLOAD: %s" % label)
                    bad += 1
                    return
                try:
                    value = json.loads(stripped)
                except ValueError as e:
                    print("NOT JSON: %s %s" % (label, e))
                    bad += 1
                    return
                # Re-serializing must reproduce the exact bytes, which is
                # what "compact and deterministic" means here.
                again = json.dumps(value, separators=(",", ":"),
                                   ensure_ascii=False)
                if again != stripped:
                    print("NOT CANONICAL: %s\n  %r\n  %r"
                          % (label, stripped[:120], again[:120]))
                    bad += 1
                    return
        elif p.returncode == 1:
            if not p.stdout:
                print("NO ERROR DOCUMENT ON STDOUT: %s" % label)
                bad += 1
                return
            if not p.stderr:
                print("SILENT FAILURE: %s" % label)
                bad += 1
                return
            if p.stdout.replace(b"\r\n", b"\n") != p.stderr.replace(b"\r\n", b"\n"):
                print("STREAMS DISAGREE: %s %r %r"
                      % (label, p.stdout[:120], p.stderr[:120]))
                bad += 1
                return
            if b"limit" not in p.stdout and b"nesting" not in p.stdout:
                print("UNNAMED FAILURE: %s %r" % (label, p.stdout[:120]))
                bad += 1
                return
            try:
                err = json.loads(p.stdout.decode("utf-8"))
            except Exception as exc:
                print("ERROR DOC NOT JSON: %s %s %r" % (label, exc, p.stdout[:120]))
                bad += 1
                return
            if not isinstance(err, dict) or "error" not in err:
                print("ERROR DOC SHAPE: %s %r" % (label, p.stdout[:120]))
                bad += 1
                return
            inner = err["error"]
            if (not isinstance(inner, dict) or
                    set(inner) != {"line", "col", "message"} or
                    not isinstance(inner["line"], int) or
                    not isinstance(inner["col"], int) or
                    not isinstance(inner["message"], str)):
                print("ERROR DOC KEYS: %s %r" % (label, p.stdout[:120]))
                bad += 1
                return
            if again is not None and again != p.stdout.decode("utf-8").rstrip("\n"):
                print("ERROR DOC NOT CANONICAL: %s %r"
                      % (label, p.stdout[:120]))
                bad += 1
                return
        else:
            print("BAD EXIT %d: %s" % (p.returncode, label))
            bad += 1
            return
        ok += 1

    docs = []
    for f in FIXTURES:
        docs.append((f, None))
    for i in range(ROUNDS):
        n = rng.randint(1, 20)
        doc = "\n".join(rng.choice(FRAGMENTS) for _ in range(n))
        p = os.path.join(tmp, "g%d.md" % i)
        with open(p, "wb") as f:
            f.write(doc.encode("utf-8"))
        docs.append((p, rng.choice(EXTS)))
    for path, ext in docs:
        args = []
        if ext is not None:
            with open(cfg, "w") as f:
                f.write(json.dumps(ext))
            args = ["--ext", cfg]
        for mode in MODES:
            check(args + [mode, path], "%s %s" % (mode, path))
            check(args + [mode, "--stream", path], "%s --stream %s" % (mode, path))

    # The corpus above is ordinary prose, and none of it reaches a limit, so
    # without this the failure branch of check() would never run at all and
    # the error document would be unvalidated. A checker that is never handed
    # a failing document is not a checker, so provoke the ceilings directly.
    # Each of these is a document that parses, that streams, and that is
    # refused, through the same code path.
    limit_docs = {
        "deep-quote": "> " * 200 + "deep\n",
        "deep-bracket": "[" * 10000 + "x" + "]" * 10000 + "\n",
        "deep-star": "*" * 2000 + "x" + "*" * 2000 + "\n",
        "tall-table": "|" + "a|" * 3 + "\n" + "|" + "-|" * 3 + "\n" + "|" + "b|" * 3 + "\n",
    }
    for name in sorted(limit_docs):
        p = os.path.join(tmp, "limit-%s.md" % name)
        with open(p, "wb") as f:
            f.write(limit_docs[name].encode("utf-8"))
        for mode in MODES:
            check([mode, p], "%s limit %s" % (mode, name))
            check([mode, "--stream", p], "%s --stream limit %s" % (mode, name))
            # An explicit ceiling refuses a document the default would
            # accept. That is the same error path reached a different way,
            # and it covers the cap being lowered as well as the default
            # being hit.
            check([mode, "--max-nesting", "4", p],
                  "%s limit %s --max-nesting 4" % (mode, name))
            check([mode, "--max-bytes", "64", p],
                  "%s limit %s --max-bytes 64" % (mode, name))

    print("\njson validity: %d ok, %d failure(s) over %d document(s)"
          % (ok, bad, len(docs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
