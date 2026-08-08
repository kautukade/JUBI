#!/usr/bin/env python3
"""Regenerate the AC97_BRINGUP_SRC block in ac97_bringup.h from the .dvm file.

The kernel cannot read files, so the reference driver program has to exist
twice: once as vm/programs/ac97_bringup.dvm (the artifact a human — or the next
phase's model — reads) and once as a C string literal inside the kernel. This
script derives the second from the first so they cannot drift, and
tests/host/test_dvm_ac97.c fails the build's test step if anyone skips it.

    python3 vm/programs/gen_header.py        # run from , or from anywhere
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DVM = os.path.join(HERE, "ac97_bringup.dvm")
HDR = os.path.join(HERE, "ac97_bringup.h")

BEGIN = "/* --- BEGIN GENERATED (do not edit by hand) --- */ \\\n"
END = "/* --- END GENERATED --- */"


def main():
    with open(DVM, "r", encoding="utf-8") as fh:
        src = fh.read()
    with open(HDR, "r", encoding="utf-8") as fh:
        hdr = fh.read()

    lines = src.split("\n")
    if lines and lines[-1] == "":
        lines.pop()                      # trailing newline, not a real line
    body = "".join(
        '    "%s\\n"%s\n' % (l.replace("\\", "\\\\").replace('"', '\\"'),
                             " \\" if i + 1 < len(lines) else "")
        for i, l in enumerate(lines))

    i = hdr.find(BEGIN)
    j = hdr.find(END)
    if i < 0 or j < 0 or j < i:
        print("gen_header: markers not found in %s" % HDR, file=sys.stderr)
        return 1
    out = hdr[:i + len(BEGIN)] + body + hdr[j:]
    if out == hdr:
        print("gen_header: already up to date")
        return 0
    with open(HDR, "w", encoding="utf-8") as fh:
        fh.write(out)
    print("gen_header: rewrote %d program lines into %s" % (len(lines), HDR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
