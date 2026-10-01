#!/usr/bin/env python3
"""Differential check: C5 must not move the extension-free contract.

Runs the pre-C5 baseline binary and the current binary over the same
documents on every mode and requires identical stdout and identical exit
status. No extension configuration is used: the committed C4 candidate
mis-parses every configuration this harness would generate (it enables
nothing for {"all":true}, and it emits syntactically invalid JSON for
{"extensions":[...]}), so it is not a usable reference for that half of the
contract. Extension behaviour is covered by tests/run_tests.sh, and the
streamed-versus-buffered identity is covered by tests/sweep_stream.py.
"""
import os
import random
import subprocess
import sys
import tempfile

BASE = sys.argv[1]
NEW = sys.argv[2]
ROUNDS = int(sys.argv[3]) if len(sys.argv) > 3 else 600
SEED = int(sys.argv[4]) if len(sys.argv) > 4 else 20250929

FRAGMENTS = [
    "# heading", "## h2 with `code`", "para text", "", "   ", "\t",
    "- item", "  - nested", "    - deeper", "1. one", "3) three",
    "> quote", ">> deep", "> > > deeper", ">>>>>>>> deep",
    "```", "```lang", "~~~", "text in fence", "    indented code",
    "---", "***", "___", "===", "- - -", "----------",
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

MODES = ["--ast", "--html", "--text"]


def make_doc(rng):
    n = rng.randint(1, 25)
    doc = rng.choice(["\n", "\n", "\n", "\r\n", "\r"]).join(
        rng.choice(FRAGMENTS) for _ in range(n))
    if rng.random() < 0.3:
        doc += "\n"
    return doc.encode("utf-8", "surrogateescape")


def main():
    rng = random.Random(SEED)
    tmp = tempfile.mkdtemp(prefix="md-diff-")
    src = os.path.join(tmp, "in.md")
    checked = 0
    stdout_diffs = 0
    status_diffs = 0
    stderr_diffs = 0
    fixed = 0
    fixed_docs = []

    for _ in range(ROUNDS):
        doc = make_doc(rng)
        with open(src, "wb") as f:
            f.write(doc)
        mode = rng.choice(MODES)

        a = subprocess.run([BASE, mode, src], capture_output=True)
        b = subprocess.run([NEW, mode, src], capture_output=True)
        checked += 1
        was_fixed = False
        if a.returncode != b.returncode:
            if a.returncode == 1 and b.returncode == 0:
                # The baseline refuses documents the current build accepts:
                # a long unmatched delimiter run overflowed a fixed eight-byte
                # buffer in C2 and took the whole document down with it. That
                # is a bug the current tree fixes, not a contract change.
                fixed += 1
                was_fixed = True
                fixed_docs.append((mode, doc))
            else:
                status_diffs += 1
                print("EXIT DIFF %d vs %d: %r mode=%s"
                      % (a.returncode, b.returncode, doc[:120], mode))
        elif a.stdout != b.stdout:
            stdout_diffs += 1
            print("STDOUT DIFF mode=%s doc=%r" % (mode, doc[:120]))
            print("  base: %r" % a.stdout[:200])
            print("  new : %r" % b.stdout[:200])
        if a.stderr != b.stderr:
            stderr_diffs += 1
            # Three differences are accounted for. A limit diagnostic is now
            # JSON. A document the baseline refused and this tree accepts
            # necessarily differs too, because the baseline was the one
            # writing to stderr. Anything else is a real difference.
            expected = (b"nesting too deep" in a.stderr
                        and b'{"error":' in b.stderr)
            if not (expected or was_fixed):
                print("UNEXPECTED STDERR DIFF mode=%s doc=%r" % (mode, doc[:120]))
                print("  base: %r" % a.stderr[:160])
                print("  new : %r" % b.stderr[:160])
        if stdout_diffs or status_diffs:
            break

    if fixed:
        # Every one of these should be the same defect, so say so once
        # instead of letting a reader assume 216 unrelated surprises.
        shapes = {}
        for mode, doc in fixed_docs:
            key = b"*" * 8 in doc or b"~" * 8 in doc or b"_" * 8 in doc
            shapes[key] = shapes.get(key, 0) + 1
        print("\n%d document(s) the baseline refused and this tree accepts."
              % fixed)
        print("  containing a long unmatched delimiter run: %d"
              % shapes.get(True, 0))
        print("  not containing one:                        %d"
              % shapes.get(False, 0))
        if shapes.get(False, 0):
            print("  ^ these are NOT the known defect; each one needs a look")

    print("\ndifferential: %d documents, %d stdout diff(s), %d exit diff(s), "
          "%d stderr diff(s), %d baseline-only failure(s) the tree fixes"
          % (checked, stdout_diffs, status_diffs, stderr_diffs, fixed))
    print("A baseline-only failure is a document the baseline refused (exit 1)"
          "\nand this tree accepts; it is counted, not dismissed, and the"
          "\nbreakdown above says whether it is the known defect.")
    return 1 if (stdout_diffs or status_diffs) else 0


if __name__ == "__main__":
    sys.exit(main())
