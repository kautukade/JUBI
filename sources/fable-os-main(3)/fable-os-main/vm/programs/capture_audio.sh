#!/bin/sh
# Boot the kernel with an AC'97 attached, record what the codec plays, and
# analyse it. This is the objective half of the driver-VM proof: the serial log
# says the program drove the registers, this says the hardware made sound.
#
#   make EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE      <-- REQUIRED FIRST
#   vm/programs/capture_audio.sh [seconds]      (run from , or anywhere)
#
# THE DEFAULT KERNEL MAKES NO SOUND, ON PURPOSE. fable-os must know nothing about
# an attached sound card — the model is supposed to write that driver at run time
# — so the reference bring-up this script listens to is a test fixture
# (tests/qemu/fixtures/ac97_boot.c) that only exists in a kernel built with
# -DFABLEOS_AC97_REFERENCE. Rather than rebuild someone's tree behind their back,
# this script checks kernel.bin for that fixture and says what to run. It does
# not tell you whether a MODEL-written driver made sound; for that, run this
# against a kernel the model has driven and read the WAV the same way.
#
# Outputs (override with OUT_PREFIX):
#   $OUT_PREFIX.wav   everything the emulated codec was fed
#   $OUT_PREFIX.log   the serial log, including the [dvm ...] execution trace
#
# QEMU's `wav` audiodev is a real audio backend: the samples in the file are
# the ones the AC'97 DMA engine fetched out of guest RAM, resampled by nothing
# because the backend is configured at the same 48 kHz stereo s16 the program
# asks the codec for. Two caveats, both handled below:
#   * QEMU does not backpatch the RIFF/data size fields unless it exits its own
#     way, so the analysis derives the frame count from the file length rather
#     than trusting the header.
#   * The backend keeps writing (silence) for as long as the machine runs, so
#     the tone is a short non-silent span near the start, not the whole file.
#
# CONCURRENCY: like capture.sh, this only ever kills the QEMU it started, and
# every path it writes is unique to this run (the PID), so it is safe alongside
# other agents' VMs and the tests/qemu harness. It opens no listening socket at
# all, so unlike capture.sh it has no monitor to be confused with anyone else's.
set -e

SECONDS_TO_RUN="${1:-14}"
DIR="$(cd "$(dirname "$0")/../.." && pwd)"       # 
OUT_PREFIX="${OUT_PREFIX:-/tmp/fableos-audio-$$}"
WAV="${OUT_PREFIX}.wav"
LOG="${OUT_PREFIX}.log"
QEMU="${FABLEOS_TEST_QEMU:-qemu-system-x86_64}"

if [ ! -f "$DIR/kernel.bin" ]; then
    echo "error: $DIR/kernel.bin does not exist. Build the fixture kernel first:" >&2
    echo "         make -C $DIR EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE" >&2
    exit 1
fi
# Every line the fixture prints starts with "ac97:", so its absence from the
# image is exactly the absence of the bring-up. Checking the binary beats
# checking .build-flags: the stamp says what the last `make` was ASKED for, this
# says what is actually in the kernel about to boot.
if ! LC_ALL=C grep -q "ac97:" "$DIR/kernel.bin" 2>/dev/null; then
    echo "error: this kernel has no audio driver in it, so it will make no sound." >&2
    echo "       That is the DEFAULT and it is deliberate: fable-os ships knowing" >&2
    echo "       nothing about sound cards. To capture the reference bring-up:" >&2
    echo "         make -C $DIR EXTRA_CFLAGS=-DFABLEOS_AC97_REFERENCE" >&2
    echo "       ...and afterwards, 'make -C $DIR' to get the normal kernel back." >&2
    exit 1
fi

rm -f "$WAV" "$LOG"

QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# -audiodev wav is the whole point; `none` for the two capture streams QEMU's
# AC97 also creates is not expressible, hence the harmless "Can not open
# `ac97.pi'" notes on stderr.
"$QEMU" -kernel "$DIR/kernel.bin" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -audiodev "wav,id=snd0,path=$WAV,out.frequency=48000,out.channels=2,out.format=s16" \
    -device AC97,audiodev=snd0 \
    -serial "file:$LOG" -display none -no-reboot 2>/dev/null &
QPID=$!

sleep "$SECONDS_TO_RUN"
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
QPID=""

echo "===== serial: the VM's own account ====="
grep -E "^(ac97:|\[dvm)" "$LOG" || echo "(no ac97/dvm lines - did the device attach?)"
echo
echo "===== audio: what the codec was actually fed ====="
WAV="$WAV" python3 - <<'EOF'
import os, struct, sys

path = os.environ["WAV"]
try:
    b = open(path, "rb").read()
except FileNotFoundError:
    print("no WAV file at %s" % path); sys.exit(1)
if b[:4] != b"RIFF" or b[8:12] != b"WAVE" or b[12:16] != b"fmt ":
    print("not a RIFF/WAVE file"); sys.exit(1)

fmt, ch, rate, _brate, _align, bits = struct.unpack("<HHIIHH", b[20:36])
pcm = b[44:]
n = len(pcm) // (2 * ch)
s = struct.unpack("<%dh" % (n * ch), pcm[:n * ch * 2])
L, R = s[0::ch], s[1::ch]
print("format      : %d ch, %d-bit, %d Hz (fmt tag %d)" % (ch, bits, rate, fmt))
print("captured    : %d bytes = %d frames = %.3f s" % (len(pcm), n, n / rate))

nz = [i for i, v in enumerate(L) if v]
if not nz:
    print("SILENT: not one non-zero sample. The codec was fed nothing.")
    sys.exit(1)
a, z = nz[0], nz[-1]
seg = L[a:z + 1]
peak = max(max(seg), -min(seg))
rms = (sum(v * v for v in seg) / len(seg)) ** 0.5
print("non-silence : frames %d..%d = %d frames = %.4f s, starting at t=%.3f s"
      % (a, z, len(seg), len(seg) / rate, a / rate))
print("amplitude   : peak %d (%.1f%% of full scale), rms %.0f"
      % (peak, 100.0 * peak / 32767, rms))
print("stereo      : left == right across the span: %s" % (L[a:z + 1] == R[a:z + 1]))


def crossings(lo, hi):
    """Sign changes in L[lo:hi], counting the one entering the window."""
    return sum(1 for i in range(max(lo, 1), hi) if (L[i - 1] < 0) != (L[i] < 0))


def halfperiods(lo, hi):
    """Run lengths between sign changes — for a square wave these ARE the
    half-period in samples, so the two most common ones pin the pitch."""
    out, last = [], lo
    for i in range(max(lo, 1), hi):
        if (L[i - 1] < 0) != (L[i] < 0):
            out.append(i - last)
            last = i
    hist = {}
    for v in out[1:-1] if len(out) > 2 else out:
        hist[v] = hist.get(v, 0) + 1
    return sorted(hist.items(), key=lambda kv: -kv[1])[:3]


mid = a + len(seg) // 2
for name, lo, hi in (("first half ", a, mid), ("second half", mid, z + 1)):
    dur = (hi - lo) / float(rate)
    print("%s: %6.1f Hz over %.3f s   half-periods (samples): %s"
          % (name, crossings(lo, hi) / 2.0 / dur, dur,
             ", ".join("%dx%d" % (n, v) for v, n in halfperiods(lo, hi))))

# QEMU inverts the descriptor's buffer-underrun-policy bit (see
# ac97_bringup.h), so between the last sample and the program stopping the bus
# master it holds that sample as DC. Measure it rather than hand-wave it.
tail = 0
while a + tail < z and L[z - tail] == L[z]:
    tail += 1
print("trailing DC : %d frames (%.1f ms) of constant %d before the engine stopped"
      % (tail, 1000.0 * tail / rate, L[z]))

print("per 50 ms window (Hz, then peak):")
w = rate // 20
for i in range(a, z + 1, w):
    hi = min(i + w, z + 1)
    if hi - i < w // 2:
        break
    win = L[i:hi]
    print("  t=%6.3f s  %7.1f Hz  peak %5d"
          % (i / float(rate), crossings(i, hi) / 2.0 / ((hi - i) / float(rate)),
             max(max(win), -min(win))))
EOF
echo
echo "wav: $WAV"
echo "log: $LOG"
