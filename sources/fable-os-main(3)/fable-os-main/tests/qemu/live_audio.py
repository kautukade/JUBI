#!/usr/bin/env python3
"""live_audio.py — drive a LIVE model turn against a real sound card.

    tests/qemu/live_audio.py --card ac97 --out /tmp/run1 \
        --turn "there's a sound card in this machine with no driver..." \
        --turn "now play me a tone through it"

This is the experiment harness for the goal in the project brief: attach an
audio device the kernel knows nothing about, ask the model in the operator's own
register to write a driver for it, and then measure whether sound came out.

WHY IT EXISTS AS A SCRIPT. A live turn costs the operator real money, so the
mechanics around it must not be improvised at the moment of spending. Every run
made through this file produces the same four artefacts under one prefix:

    $OUT.wav    everything the emulated codec was fed  (analyse with wavcheck.py)
    $OUT.log    the complete serial transcript, kernel voice included
    $OUT.png    the framebuffer at the end of the session
    $OUT.json   turn boundaries, timings, and what was typed

THE KEY IS NEVER TOUCHED. This spawns `make run-nox`, which reads .env
itself and passes the key to the guest over fw_cfg. This file never opens .env,
never names an environment variable holding a key, and never puts one on a
command line. QEMU arguments are appended through QEMU_EXTRA, which the Makefile
documents as the supported way to attach hardware.

TURN COMPLETION. The REPL prints "\\n> " and nothing else when it wants a
sentence (kernel/main.c). A turn is over when that prompt comes back, so the
harness waits for the prompt, types, and then waits for the NEXT prompt. It does
not parse the model's prose at all: prose is a generation, the prompt is a fact.
A turn that produces no prompt within --turn-timeout is reported as a timeout
with its partial output kept, because a hung turn is itself a finding.

CONCURRENCY. Every path is unique to this run, the monitor is a unix socket
created by this process, and the only process ever signalled is the child this
script started. Safe alongside other QEMU instances and tests/qemu/harness.py.
"""

import argparse
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))        # tests/qemu
OSDIR = os.path.dirname(os.path.dirname(HERE))           #   (where the Makefile is)

# The cards this experiment is about. Nothing here is knowledge the KERNEL has —
# it is QEMU command-line syntax, on the host side of the machine boundary.
#
#   adlib     OPL2 FM synthesis behind two I/O ports. An ISA device: it has no
#             PCI config space at all, which turns out to be the whole story of
#             whether the driver VM can be pointed at it.
#   ac97      PCI class 04:01. Bus-master DMA over a scatter-gather list.
#   hda       PCI class 04:03 plus a codec. Ring buffers, a buffer descriptor
#             list, and codec verbs over CORB/RIRB or the immediate registers.
CARDS = {
    "adlib": ["-device", "adlib,audiodev=snd0"],
    "ac97":  ["-device", "AC97,audiodev=snd0"],
    "hda":   ["-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0"],
    "none":  [],
}

PROMPT = "\n> "


def shquote(a):
    return "'" + a.replace("'", "'\\''") + "'"


def screenshot(mon_path, ppm_path, png_path):
    """Ask the QEMU monitor for a framebuffer dump and convert it if possible."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(15)
        s.connect(mon_path)
    except OSError as e:
        return "no monitor: %s" % e
    with s:
        time.sleep(0.3)
        try:
            s.recv(65536)
        except OSError:
            pass
        s.sendall(b"screendump %s\n" % ppm_path.encode())
        time.sleep(2.0)
        try:
            s.recv(65536)
        except OSError:
            pass
    if not os.path.exists(ppm_path) or os.path.getsize(ppm_path) == 0:
        return "screendump produced nothing"
    for cmd in (["sips", "-s", "format", "png", ppm_path, "--out", png_path],
                ["convert", ppm_path, png_path]):
        try:
            r = subprocess.run(cmd, capture_output=True)
            if r.returncode == 0 and os.path.exists(png_path):
                return None
        except FileNotFoundError:
            continue
    return "could not convert PPM to PNG"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", required=True, choices=sorted(CARDS))
    ap.add_argument("--out", required=True, help="output path prefix")
    ap.add_argument("--turn", action="append", default=[],
                    help="a sentence to type at the prompt; repeatable, in order")
    ap.add_argument("--turn-timeout", type=float, default=300.0,
                    help="seconds to wait for the prompt to come back (default 300)")
    ap.add_argument("--boot-timeout", type=float, default=90.0)
    ap.add_argument("--settle", type=float, default=6.0,
                    help="seconds to keep running after the last turn, so a "
                         "device that is still DMAing is captured (default 6)")
    ap.add_argument("--extra", default="", help="extra QEMU args, appended")
    args = ap.parse_args(argv[1:])

    wav = args.out + ".wav"
    log = args.out + ".log"
    png = args.out + ".png"
    meta = args.out + ".json"
    for p in (wav, log, png, meta):
        if os.path.exists(p):
            os.unlink(p)

    mon = tempfile.mktemp(prefix="fableos-live-mon-")
    ppm = tempfile.mktemp(prefix="fableos-live-shot-")

    qextra = [
        "-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,"
                     "out.channels=2,out.format=s16" % wav,
    ] + CARDS[args.card] + [
        "-monitor", "unix:%s,server,nowait" % mon,
    ]
    if args.extra:
        qextra += args.extra.split()

    cmd = ["make", "run-nox", "QEMU_EXTRA=" + " ".join(shquote(a) for a in qextra)]
    print("+ (cd %s && %s)" % (OSDIR, " ".join(shquote(c) for c in cmd)), flush=True)

    logf = open(log, "wb")
    # start_new_session puts make AND the qemu it forks in a process group of
    # their own. That is what makes the killpg below safe: without it the group
    # is this script's own group, and cleanup would signal the shell that
    # launched it. It also means no other agent's QEMU can ever be in the group.
    proc = subprocess.Popen(cmd, cwd=OSDIR, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            bufsize=0, start_new_session=True)

    buf = ""           # everything seen, as text
    events = []
    t0 = time.time()

    def pump(deadline, want_from):
        """Read until PROMPT appears at or after `want_from`, or the deadline.

        Returns the index just past the prompt, or -1 on timeout/EOF. Echoes
        live so a watching operator sees the turn happen.
        """
        nonlocal buf
        while True:
            i = buf.find(PROMPT, want_from)
            if i >= 0:
                return i + len(PROMPT)
            left = deadline - time.time()
            if left <= 0:
                return -1
            r, _, _ = select.select([proc.stdout], [], [], min(left, 1.0))
            if not r:
                if proc.poll() is not None:
                    return -1
                continue
            chunk = proc.stdout.read(4096)
            if not chunk:
                return -1
            logf.write(chunk)
            logf.flush()
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
            buf += chunk.decode("utf-8", "replace")

    rc = 0
    try:
        pos = pump(time.time() + args.boot_timeout, 0)
        if pos < 0:
            print("\n!! never reached the first prompt within %.0fs"
                  % args.boot_timeout, flush=True)
            rc = 1
        else:
            events.append({"event": "booted", "t": time.time() - t0})
            for n, text in enumerate(args.turn, 1):
                print("\n=== TURN %d (t=%.1fs): %s\n" % (n, time.time() - t0, text),
                      flush=True)
                start = len(buf)
                tstart = time.time()
                proc.stdin.write((text + "\n").encode())
                proc.stdin.flush()
                pos = pump(time.time() + args.turn_timeout, start)
                done = pos >= 0
                events.append({
                    "event": "turn", "n": n, "text": text,
                    "t_start": tstart - t0, "seconds": time.time() - tstart,
                    "completed": done, "output_chars": len(buf) - start,
                })
                if not done:
                    print("\n!! turn %d did not return to the prompt within %.0fs"
                          % (n, args.turn_timeout), flush=True)
                    rc = 1
                    break

        if args.settle > 0 and proc.poll() is None:
            print("\n=== settling %.1fs (a device may still be DMAing)\n"
                  % args.settle, flush=True)
            pump(time.time() + args.settle, len(buf) + 1)   # unmatchable: just read

        if proc.poll() is None:
            err = screenshot(mon, ppm, png)
            print("screenshot: %s" % (err or png), flush=True)
    finally:
        if proc.poll() is None:
            try:
                proc.stdin.close()
            except OSError:
                pass
            # make(1) is the direct child; QEMU is its child. Kill the group so
            # the emulator does not survive as an orphan holding the WAV open.
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except OSError:
                proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except OSError:
                    proc.kill()
        # Drain whatever the child wrote between the last pump and exiting.
        try:
            rest = proc.stdout.read()
            if rest:
                logf.write(rest)
                buf += rest.decode("utf-8", "replace")
        except (OSError, ValueError):
            pass
        logf.close()
        for p in (mon, ppm):
            try:
                os.unlink(p)
            except OSError:
                pass

    with open(meta, "w") as f:
        json.dump({"card": args.card, "turns": args.turn, "events": events,
                   "wav": wav, "log": log, "png": png}, f, indent=1)

    print("\n===== kernel voice: every bracketed trace line =====", flush=True)
    for line in buf.splitlines():
        if line.startswith("["):
            print("  " + line)

    print("\n===== audio =====", flush=True)
    sys.path.insert(0, HERE)
    import wavcheck                                    # noqa: E402
    try:
        wavcheck.report(wavcheck.analyse(wav))
    except (OSError, ValueError) as e:
        print("wavcheck: %s" % e)

    print("\nwav: %s\nlog: %s\npng: %s\nmeta: %s" % (wav, log, png, meta))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
