#!/usr/bin/env python3
"""Adversarial sweep: the streamed and the buffered path must never disagree.

Generates random documents out of a vocabulary of constructs that stress the
normalizer (CR, CRLF, tabs, NULs, UTF-8), the block parser (nesting, fences,
containers) and the inline parser (delimiters, brackets, code spans), then
compares the two paths byte for byte on every mode and with every extension
mask. A difference, a crash, an unexpected exit status, or output on stderr
for a successful run is a failure.
"""
import os
import random
import subprocess
import sys
import tempfile

MD = sys.argv[1] if len(sys.argv) > 1 else "./md"
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 400
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 20250929

FRAGMENTS = [
    "# heading", "## h2 with `code`", "para text", "", "   ", "\t",
    "- item", "  - nested", "    - deeper", "1. one", "3) three",
    "> quote", ">> deep", "> > > deeper", ">>>>>>>> deep",
    "```", "```lang", "~~~", "text in fence", "    indented code",
    "---", "***", "___", "===", "- - -",
    "| a | b |", "| --- | :-: |", "| 1 | 2 |", "|", "a | b",
    "[ref]: /url \"title\"", "[ref]", "![img](/u)", "[l](/u)", "[l][ref]",
    "*em*", "**strong**", "***both***", "_em_", "__strong__",
    "~~strike~~", "~~a **b** c~~", "`code`", "``a ` b``",
    "a\\*b", "&amp;", "&#65;", "&#x41;", "&nosuch;", "<http://e.test>",
    "bare http://e.test", "a  \nb", "a\\\nb",
    "- [ ] task", "- [x] done", "- [ ]", "[ ] not a list",
    "text[^n]", "[^n]: the note", "[^]: empty label",
    "!! note", "==marked==",
    "\r", "\r\r", "a\rb", "\t\ta", "a\tb", "\0", "a\0b", "\0\0",
    "é", "€", "\U0001f600", "aéb",
    "  *  ", " *", "* ", "***", "**", "`", "``", "[", "]", "(", ")",
    "| " + "x" * 40 + " |", ">" * 20 + " x", "[" * 30 + "x",
    "*" * 40 + "x", "`" * 30 + "x", "~~" * 20 + "x",
    "x" * 200, " " * 100 + "y",
]

# --toc is in this list because it is a mode like any other: it has to agree
# between the streamed and the buffered path, under every extension mask, and
# exit 0 or 1 with nothing on stderr when it succeeds. A mode that this list
# does not name is a mode nobody checks.
MODES = ["--ast", "--html", "--text", "--toc"]
EXTS = [
    None,
    {"tables": True},
    {"strikethrough": True},
    {"tasklist": True},
    {"footnotes": True},
    {"tables": True, "strikethrough": True, "tasklist": True, "footnotes": True},
]


def make_doc(rng):
    n = rng.randint(1, 25)
    parts = [rng.choice(FRAGMENTS) for _ in range(n)]
    sep = rng.choice(["\n", "\n", "\n", "\r\n", "\r"])
    doc = sep.join(parts)
    if rng.random() < 0.3:
        doc += sep
    return doc.encode("utf-8", "surrogateescape")


def run(args, data=None, path=None):
    if path is not None:
        with open(path, "wb") as f:
            f.write(data)
    return subprocess.run(args, capture_output=True)


def main():
    rng = random.Random(SEED)
    failures = 0
    checked = 0
    tmpdir = tempfile.mkdtemp(prefix="md-sweep-")
    src = os.path.join(tmpdir, "in.md")
    cfg = os.path.join(tmpdir, "ext.json")

    for round_no in range(ROUNDS):
        doc = make_doc(rng)
        with open(src, "wb") as f:
            f.write(doc)
        ext = rng.choice(EXTS)
        base = []
        if ext is not None:
            with open(cfg, "w") as f:
                import json
                f.write(json.dumps(ext))
            base = ["--ext", cfg]
        mode = rng.choice(MODES)

        a = run([MD, mode] + base + [src])
        b = run([MD, mode, "--stream"] + base + [src])
        checked += 1
        if a.returncode != b.returncode or a.stdout != b.stdout:
            failures += 1
            print("MISMATCH round %d mode=%s ext=%s" % (round_no, mode, ext))
            print("  bytes: %r" % doc[:200])
            print("  buffered rc=%d stream rc=%d" % (a.returncode, b.returncode))
            if a.stdout != b.stdout:
                print("  buffered out: %r" % a.stdout[:200])
                print("  stream   out: %r" % b.stdout[:200])
            if failures > 5:
                break
            continue
        if a.returncode == 0 and a.stderr:
            failures += 1
            print("STDERR ON SUCCESS round %d: %r" % (round_no, a.stderr[:200]))
        if a.returncode not in (0, 1):
            failures += 1
            print("BAD EXIT %d round %d" % (a.returncode, round_no))
        if a.returncode == 1 and not a.stderr:
            failures += 1
            print("SILENT FAILURE round %d" % round_no)
        if a.returncode == 1 and not a.stderr.strip().startswith(b"{"):
            failures += 1
            print("UNSTRUCTURED LIMIT ERROR round %d: %r" % (round_no, a.stderr[:120]))

    print("\nsweep: %d documents, %d failure(s)" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
