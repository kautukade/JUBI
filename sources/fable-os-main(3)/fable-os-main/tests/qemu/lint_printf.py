#!/usr/bin/env python3
"""lint_printf.py — find format strings the KERNEL's own formatter cannot render.

    tests/qemu/lint_printf.py            # scan what a default build compiles
    tests/qemu/lint_printf.py --all      # scan every first-party source
    tests/qemu/lint_printf.py --json

WHY THIS EXISTS, and why it is a source lint rather than a test.

The kernel does not use the host C library. lib/kfmt.c provides a small bounded
vsnprintf that implements a SUBSET of C99. Host suites link the real libc, where
every format string in the tree works perfectly, so a conversion the kernel
cannot render is invisible to `make test-host` by construction. That asymmetry is
the whole reason for a lint: it reads the source, so it does not care which
formatter is linked. tests/host/test_kfmt.c covers the other half — it links
lib/kfmt.c next to the host libc and diffs them conversion by conversion — but
it can only test the grammar kfmt claims to support. Only a lint can find a
caller reaching for something outside that grammar.

WHAT USED TO BE HERE, and why the check changed shape.

This script was originally written to find star width and precision (`%*d`,
`%.*s`), which the formatter did not support. The failure mode was not a
truncated string: because the '*' was never consumed, the formatter emitted the
literal characters "%*s" AND LEFT THE ARGUMENT LIST MISALIGNED, so every
conversion after it read the wrong argument.

    format : "%s: register \"%.*s\" does not exist (this machine has r0-r%d)"
    args   : "mov", 3, "r16", 15
    host   : mov: register "r16" does not exist (this machine has r0-r15)
    KERNEL : mov: register "%*s" does not exist (this machine has r0-r3)

Two harms in one line: the token the model needs is replaced by punctuation, and
the register count becomes a confident lie assembled from the token's length. A
model reading that is told this machine has four registers. Worse shapes existed
— where a `%s` followed the `%.*s`, the `%s` dereferenced the token LENGTH as a
pointer, and since boot/boot.asm identity-maps low memory present+writable from
physical 0 it did not fault; it read the real-mode IVT into a model-facing error
string and from there into the JSON request body as invalid UTF-8.

There were 27 such lines, all of them assembler errors in vm/dvm.c. They were
not fixed one at a time. lib/kfmt.c now IMPLEMENTS star width and precision, so
all 27 became correct at once and, more to the point, the next one written is
correct too. Star is therefore no longer a finding.

WHAT IS A FINDING NOW: a conversion, flag or length modifier that lib/kfmt.c
still does not implement. Each entry in UNSUPPORTED below names the reason. The
consequences are the same family as before — a `default` arm that echoes the
conversion literally and consumes no vararg, so everything after it shifts.

WHAT IS NOT A FINDING. Third-party trees (lwip, mbedtls) are skipped: they ship
their own formatters and are not printed through kfmt. Host test sources under
tests/ are skipped because they really do run against the host libc. lib/base.c
is IN scope but its kprintf is an even smaller formatter than kfmt — see the
note on KPRINTF_ONLY below.

EXIT STATUS: 0 if clean, 1 if any offender is found, 2 on usage error. Findings
are printed as `file:line: text`, so an editor can jump to them.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))        # tests/qemu
OSDIR = os.path.dirname(os.path.dirname(HERE))           # 

# Third-party trees bring their own printf. tests/ links the host libc.
SKIP_PREFIX = ("lwip/", "mbedtls/", "tests/", "port/")

# What lib/kfmt.c actually implements, as of this writing:
#   flags        '-' '0'
#   width        digits or '*'
#   precision    '.' digits or '.*'
#   length       'l', 'll', 'z'
#   conversions  d i u x X p c s %
# Anything else falls to the `default` arm, which echoes the conversion
# character literally AND CONSUMES NO VARARG, shifting every argument after it.
#
# Each pattern below is (regex, why it cannot be rendered). The regexes match
# inside a string literal, after "%%" has been removed, so an escaped percent
# followed by one of these characters is not a finding.
UNSUPPORTED = [
    (re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|j|t|L)[diuxXoefgacsn]"),
     "length modifier hh/h/j/t/L is not parsed; kfmt understands only l, ll and z"),
    (re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[feEgGaA]"),
     "floating point is not implemented (no float conversions, and no saved SSE state)"),
    (re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[o]"),
     "octal %o is not implemented; use %x or print the value in decimal"),
    (re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?n"),
     "%n writes through a pointer argument and is deliberately absent"),
    (re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?[SC]"),
     "wide-character %S/%C is not implemented"),
    (re.compile(r"%[-#0]*[+ ][-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[diuxX]"),
     "the '+' and ' ' sign flags are not parsed; they are echoed literally"),
    (re.compile(r"%[-+ 0]*#[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[xXo]"),
     "the '#' alternate-form flag is not parsed; it is echoed literally"),
]


def sources_from_depfiles():
    """Every first-party .c/.h a default build actually compiled.

    Read from the compiler's own -MMD output, so the set is what the build
    really pulled in rather than what a directory walk guesses. Returns None if
    the tree has not been built, so the caller can fall back.
    """
    found = set()
    seen_any = False
    for root, dirs, files in os.walk(OSDIR):
        dirs[:] = [d for d in dirs if d not in (".git", "tests/build")]
        for fn in files:
            if not fn.endswith(".d"):
                continue
            seen_any = True
            path = os.path.join(root, fn)
            try:
                with open(path) as f:
                    text = f.read()
            except OSError:
                continue
            # Both the prerequisite list and the phony per-header targets name
            # files; a bare token ending in .c/.h is all we need.
            for tok in re.split(r"[\s\\:]+", text):
                if tok.endswith((".c", ".h")):
                    found.add(os.path.normpath(tok))
    return found if seen_any else None


def sources_all():
    out = set()
    for root, dirs, files in os.walk(OSDIR):
        dirs[:] = [d for d in dirs if d != ".git"]
        for fn in files:
            if fn.endswith((".c", ".h")):
                rel = os.path.relpath(os.path.join(root, fn), OSDIR)
                out.add(os.path.normpath(rel))
    return out


def in_scope(rel):
    rel = rel.replace(os.sep, "/")
    if rel.startswith("/"):
        return False
    return not any(rel.startswith(p) for p in SKIP_PREFIX)


def scan(rel):
    """Offending lines in one file: (line_no, stripped_text, [conversions])."""
    path = os.path.join(OSDIR, rel)
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return []
    hits = []
    for i, line in enumerate(lines, 1):
        # Skip comment lines. This file's own explanation of the bug quotes the
        # broken format string verbatim, and a lint that reports the description
        # of a fix as an instance of the bug is a lint people switch off. The
        # test is deliberately crude — leading `*`, `//` or `/*`, which is every
        # comment in this tree's house style — because the alternative is a C
        # parser. A format string sharing a line with the end of a block comment
        # would be missed; nothing in this tree does that.
        stripped = line.lstrip()
        if stripped.startswith(("*", "//", "/*")):
            continue
        # Only look inside string literals: a bare `a %* b` in prose, or an
        # expression like `x = y %*p`, is not a format string. Cheap and good
        # enough — every real offender is a literal on the line.
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            body = lit.replace("%%", "")
            reasons = []
            for rx, why in UNSUPPORTED:
                for m in rx.findall(body):
                    reasons.append("%s -- %s" % (m if isinstance(m, str) else m[0], why))
            if reasons:
                hits.append((i, line.strip(), reasons))
                break
    return hits


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="scan every first-party source, not just what was built")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv[1:])

    srcs = None if args.all else sources_from_depfiles()
    how = "compiled by the current build (from -MMD .d files)"
    if srcs is None:
        srcs = sources_all()
        how = "every first-party source (tree was not built, or --all)"

    srcs = sorted(s for s in srcs if in_scope(s))

    findings = []
    for rel in srcs:
        for lineno, text, convs in scan(rel):
            findings.append({"file": rel, "line": lineno,
                             "conversions": convs, "text": text})

    if args.json:
        print(json.dumps({"scanned": len(srcs), "scope": how,
                          "findings": findings}, indent=1))
        return 1 if findings else 0

    print("lint_printf: scanned %d file(s): %s" % (len(srcs), how))
    print("looking for conversions lib/kfmt.c does not implement. Its `default`")
    print("arm echoes them literally AND consumes no vararg, so every argument")
    print("after one of these shifts by a slot.")
    print()
    if not findings:
        print("CLEAN: every kernel format string is inside the grammar")
        print("lib/kfmt.c implements (and tests/host/test_kfmt.c pins).")
        return 0

    byfile = {}
    for f in findings:
        byfile.setdefault(f["file"], []).append(f)
    for rel in sorted(byfile):
        print("%s  (%d)" % (rel, len(byfile[rel])))
        for f in byfile[rel]:
            print("  %s:%d: %s" % (rel, f["line"], f["text"][:96]))
            for r in f["conversions"]:
                print("      %s" % r)
    print()
    print("%d offending line(s) in %d file(s)." % (len(findings), len(byfile)))
    print("Each one reaches a model as punctuation plus a shifted argument list.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
