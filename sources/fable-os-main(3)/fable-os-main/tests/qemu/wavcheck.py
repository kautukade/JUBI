#!/usr/bin/env python3
"""wavcheck.py — measure what a QEMU audio backend was actually fed.

    tests/qemu/wavcheck.py FILE.wav [--json]

This is the objective half of an audio proof. A serial log can say a driver
programmed every register correctly and still be describing silence; this reads
the samples QEMU's `wav` audiodev wrote, which are the ones the emulated codec
was handed, and reports numbers.

Two properties of QEMU's wav backend shape everything here, both learned the
hard way in vm/programs/capture_audio.sh:

  * IT DOES NOT BACKPATCH THE RIFF HEADER unless QEMU exits through its own
    shutdown path. A killed QEMU leaves data-size = 0 (or a stale value), so the
    frame count is derived from the FILE LENGTH and the header's size fields are
    ignored. Trusting them reports "0 frames" over a file full of music.
  * IT WRITES CONTINUOUSLY, silence included, for as long as the machine runs.
    A one-second tone inside a twenty-second boot is a short non-silent span, so
    everything interesting is measured over [first non-zero .. last non-zero]
    rather than over the file.

Frequency is measured two independent ways because they fail differently:

  * ZERO CROSSINGS over the span — cheap, exact for a clean periodic wave, and
    wrong in a specific readable way for noise (it reports something absurdly
    near rate/2). Reported per 50 ms window as well as overall, so a sweep, a
    truncated tone or a burst of garbage does not average into a plausible
    single number.
  * A GOERTZEL FILTER at the crossing-derived estimate and at a coarse sweep of
    candidate bins, giving the FRACTION OF TOTAL ENERGY at the strongest bin.
    That fraction is what separates a tone from noise: a real sine puts
    essentially all of its energy in one bin, white noise puts ~nothing there.
    It is also what catches the interesting failure mode of an FM chip driven
    with a wrong register — a buzz at the right pitch is not the same as a tone.

Exit status: 0 if the file contains any non-silence, 1 if it is digitally
silent or unreadable. A verdict a script can branch on.
"""

import json
import math
import os
import struct
import sys


def load(path):
    """Return (rate, channels, bits, [samples per channel]) from a WAV file.

    Walks the RIFF chunk list rather than assuming the canonical 44-byte
    header, because a backend is free to insert LIST/fact chunks, but takes the
    PCM length from what is really in the file.
    """
    with open(path, "rb") as f:
        b = f.read()
    if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file (%d bytes)" % len(b))

    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]
        csz = struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt " and body + 16 <= len(b):
            fmt = struct.unpack("<HHIIHH", b[body:body + 16])
        elif cid == b"data":
            # DELIBERATELY IGNORING csz: see the module docstring.
            pcm = b[body:]
            break
        pos = body + csz + (csz & 1)

    if fmt is None:
        raise ValueError("no fmt chunk")
    if pcm is None:
        raise ValueError("no data chunk")

    _tag, ch, rate, _brate, _align, bits = fmt
    if bits != 16:
        raise ValueError("only 16-bit PCM is understood, file says %d" % bits)
    if ch < 1:
        raise ValueError("channel count is %d" % ch)

    n = len(pcm) // (2 * ch)
    if n == 0:
        return rate, ch, bits, [[] for _ in range(ch)]
    flat = struct.unpack("<%dh" % (n * ch), pcm[:n * ch * 2])
    return rate, ch, bits, [list(flat[c::ch]) for c in range(ch)]


def goertzel(x, rate, hz):
    """Magnitude of `x` at `hz`, normalised so a full-amplitude sine gives its
    own amplitude back. Single-bin DFT: no numpy, no FFT, no dependencies."""
    n = len(x)
    if n == 0 or hz <= 0 or hz >= rate / 2.0:
        return 0.0
    w = 2.0 * math.cos(2.0 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + w * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - w * s1 * s2
    return 2.0 * math.sqrt(max(power, 0.0)) / n


def crossing_hz(x, rate):
    """Frequency from negative/positive sign changes. Two crossings per cycle."""
    if len(x) < 2:
        return 0.0
    c = sum(1 for i in range(1, len(x)) if (x[i - 1] < 0) != (x[i] < 0))
    return c / 2.0 / (len(x) / float(rate))


def best_bin(x, rate, seed):
    """Strongest bin: refine around `seed`, then fall back to a coarse sweep.

    The sweep exists so a wildly wrong seed (what crossing_hz returns for
    noise) cannot make the energy fraction look good by accident.
    """
    cands = set()
    for hz in (seed, seed / 2.0, seed * 2.0):
        if 15.0 < hz < rate / 2.0:
            for d in (-4, -2, -1, 0, 1, 2, 4):
                h = hz + d * max(1.0, hz * 0.002)
                if 15.0 < h < rate / 2.0:
                    cands.add(round(h, 2))
    hz = 20.0
    while hz < rate / 2.0:                 # ~1/12 octave sweep, 20 Hz .. Nyquist
        cands.add(round(hz, 2))
        hz *= 1.0595
    scored = [(goertzel(x, rate, h), h) for h in sorted(cands)]
    return max(scored) if scored else (0.0, 0.0)


def analyse(path, limit_frames=None):
    rate, ch, bits, chans = load(path)
    L = chans[0]
    n = len(L)
    if limit_frames:
        n = min(n, limit_frames)
        L = L[:n]
    out = {
        "file": path, "rate": rate, "channels": ch, "bits": bits,
        "frames": n, "seconds": (n / float(rate)) if rate else 0.0,
    }

    nz = [i for i, v in enumerate(L) if v]
    if not nz:
        out["silent"] = True
        return out
    out["silent"] = False

    a, z = nz[0], nz[-1]
    seg = L[a:z + 1]
    out["span_frames"] = len(seg)
    out["span_seconds"] = len(seg) / float(rate)
    out["span_start_s"] = a / float(rate)
    out["peak"] = max(max(seg), -min(seg))
    out["peak_pct_fs"] = 100.0 * out["peak"] / 32767.0
    out["rms"] = math.sqrt(sum(v * v for v in seg) / float(len(seg)))
    out["nonzero_frames"] = len(nz)
    # How much of the span is actually moving, as opposed to one DC step: a
    # bus-master that fetched one sample and stopped looks loud by peak alone.
    out["distinct_values"] = len(set(seg)) if len(seg) < 400000 else -1
    if ch > 1:
        R = chans[1][:n]
        out["stereo_identical"] = L[a:z + 1] == R[a:z + 1]

    # Measure on a middle slice: the first and last frames of a span often hold
    # a partial buffer or the DC tail QEMU leaves after the engine stops.
    mid_lo = a + len(seg) // 4
    mid_hi = a + (3 * len(seg)) // 4
    if mid_hi - mid_lo < 64:
        mid_lo, mid_hi = a, z + 1
    core = L[mid_lo:mid_hi]

    cross = crossing_hz(core, rate)
    mag, hz = best_bin(core, rate, cross)
    energy = sum(float(v) * v for v in core) / float(len(core))
    # (amplitude^2)/2 is the mean square of a sine of that amplitude.
    out["crossing_hz"] = cross
    out["detected_hz"] = hz
    out["detected_amplitude"] = mag
    out["tonal_fraction"] = ((mag * mag / 2.0) / energy) if energy > 0 else 0.0

    out["windows"] = []
    w = max(1, rate // 20)                             # 50 ms
    for i in range(a, z + 1, w):
        hi = min(i + w, z + 1)
        if hi - i < w // 2:
            break
        win = L[i:hi]
        wc = crossing_hz(win, rate)
        wm, wh = best_bin(win, rate, wc)
        we = sum(float(v) * v for v in win) / float(len(win))
        out["windows"].append({
            "t": i / float(rate),
            "crossing_hz": wc,
            "detected_hz": wh,
            "peak": max(max(win), -min(win)),
            "tonal_fraction": ((wm * wm / 2.0) / we) if we > 0 else 0.0,
        })

    # Collapse the windows into runs of one pitch. Without this the headline
    # "frequency" of a two-note melody is a number that was never played: the
    # core slice straddles the note change and the overall Goertzel lands
    # between the two. Runs are what a listener would report.
    runs = []
    for w in out["windows"]:
        # A window with no signal in it has no pitch. Reporting the Goertzel's
        # best guess for silence prints a confident 23 kHz, which reads like a
        # finding; it is an artefact of asking "which bin is loudest" of nothing.
        hz = w["detected_hz"] if w["peak"] > 0 else 0.0
        if runs and abs(hz - runs[-1]["hz"]) <= max(4.0, 0.03 * hz):
            runs[-1]["windows"] += 1
            runs[-1]["hz"] = (runs[-1]["hz"] * (runs[-1]["windows"] - 1) + hz) \
                             / runs[-1]["windows"]
            runs[-1]["tonal"] = max(runs[-1]["tonal"], w["tonal_fraction"])
            runs[-1]["peak"] = max(runs[-1]["peak"], w["peak"])
        else:
            runs.append({"t": w["t"], "hz": hz, "windows": 1,
                         "tonal": w["tonal_fraction"], "peak": w["peak"]})
    out["runs"] = [r for r in runs if r["windows"] >= 2] or runs

    tail = 0
    while a + tail < z and L[z - tail] == L[z]:
        tail += 1
    out["trailing_dc_frames"] = tail
    out["trailing_dc_ms"] = 1000.0 * tail / rate
    out["trailing_dc_value"] = L[z]
    return out


def report(r):
    print("file        : %s" % r["file"])
    print("format      : %d ch, %d-bit, %d Hz" % (r["channels"], r["bits"], r["rate"]))
    print("captured    : %d frames = %.3f s" % (r["frames"], r["seconds"]))
    if r.get("silent", True):
        print("SILENT      : not one non-zero sample. The codec was fed nothing.")
        return 1
    print("non-silence : %d frames = %.4f s, starting at t=%.3f s"
          % (r["span_frames"], r["span_seconds"], r["span_start_s"]))
    print("amplitude   : peak %d (%.1f%% full scale), rms %.0f"
          % (r["peak"], r["peak_pct_fs"], r["rms"]))
    print("moving      : %d non-zero frames, %d distinct sample values"
          % (r["nonzero_frames"], r["distinct_values"]))
    if "stereo_identical" in r:
        print("stereo      : left == right: %s" % r["stereo_identical"])
    print("frequency   : %.1f Hz by zero crossings, %.1f Hz by Goertzel"
          % (r["crossing_hz"], r["detected_hz"]))
    print("tonality    : %.1f%% of energy at %.1f Hz (amplitude %.0f there)"
          % (100.0 * r["tonal_fraction"], r["detected_hz"], r["detected_amplitude"]))
    if r.get("runs"):
        # The headline two lines average across note changes; these do not.
        print("pitch runs  : (the two lines above straddle any note change)")
        for run in r["runs"]:
            if run["peak"] == 0:
                print("  t=%6.3f s  %8s    for %6.0f ms"
                      % (run["t"], "silence", run["windows"] * 50.0))
                continue
            print("  t=%6.3f s  %8.1f Hz for %6.0f ms  peak %5d  tonal %5.1f%%"
                  % (run["t"], run["hz"], run["windows"] * 50.0, run["peak"],
                     100.0 * run["tonal"]))
    print("trailing DC : %d frames (%.1f ms) of constant %d"
          % (r["trailing_dc_frames"], r["trailing_dc_ms"], r["trailing_dc_value"]))
    if r["windows"]:
        print("per 50 ms   :")
        for w in r["windows"][:40]:
            print("  t=%6.3f s  %8.1f Hz (cross %8.1f)  peak %5d  tonal %5.1f%%"
                  % (w["t"], w["detected_hz"], w["crossing_hz"], w["peak"],
                     100.0 * w["tonal_fraction"]))
        if len(r["windows"]) > 40:
            print("  ... %d more windows" % (len(r["windows"]) - 40))
    return 0


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    try:
        r = analyse(args[0])
    except (OSError, ValueError) as e:
        print("wavcheck: %s" % e)
        return 1
    if "--json" in argv:
        print(json.dumps(r, indent=1))
        return 0 if not r.get("silent", True) else 1
    return report(r)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
