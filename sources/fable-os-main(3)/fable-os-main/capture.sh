#!/bin/sh
# Boot the kernel headless, capture a framebuffer screenshot and the serial log.
#
#   ./capture.sh [seconds-before-shot]
#
# Outputs (override with OUT_PREFIX):
#   $OUT_PREFIX.png   framebuffer screenshot   (default /tmp/os.png)
#   $OUT_PREFIX.log   serial log               (default /tmp/os.log)
#
# ATTACHING HARDWARE: QEMU_EXTRA is appended after the NIC, exactly as it is for
# `make run`, so the same string works with both:
#
#   QEMU_EXTRA="-audiodev none,id=a0 -device AC97,audiodev=a0" ./capture.sh 20
#
# This is not a convenience. The whole premise of this project is that an
# operator attaches a device the kernel has never heard of and the model writes
# the driver, and a SCREENSHOT is the only honest evidence that a window rendered
# or that a device was left unclaimed. Without this the one script that produces
# screenshots could not see the one machine configuration that matters, so
# "a default kernel leaves the sound card alone" had to be verified by a
# different tool than the one used to verify everything else.
#
# EXIT STATUS IS MEANINGFUL. This script is how a boot gets verified, so it must
# not be able to print a screenshot path and exit 0 when QEMU never started. It
# exits non-zero, and shows QEMU's own stderr, if the guest produced no serial
# output at all; and it only claims a screenshot when the file exists.
#
# CONCURRENCY: safe to run while other QEMU instances (other developers, agents,
# or the tests/qemu harness) are running. It never kills processes it does not
# own, and every path — including the QEMU monitor socket — is created by mktemp
# inside this run, so two runs cannot collide however close their PIDs are. Set
# KILL_OTHERS=1 to opt into the old behaviour of pkill-ing every
# qemu-system-x86_64 first.
set -e

DELAY="${1:-3}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_PREFIX="${OUT_PREFIX:-/tmp/os}"

# Unique per run: a stale or concurrent instance can't steal our socket or files.
# No ".ppm" suffix: appending one would name a file mktemp did not create, and
# the cleanup trap below would then remove the name while leaking the inode.
# QEMU's screendump does not care about the extension.
PPM="$(mktemp -t fableos-shot)"
QLOG="$(mktemp -t fableos-qemu)"
SERIAL="${OUT_PREFIX}.log"
PNG="${OUT_PREFIX}.png"
# The monitor used to be a TCP port derived from the PID (49152 + PID % 16000).
# macOS PIDs run past 99000, so two shells 16000 apart got the same port — and
# the loser's helper then connected to the WINNER's monitor and sent `quit`,
# killing a VM it did not own. A unix socket under mktemp cannot alias.
MON="$(mktemp -u -t fableos-mon)"

if [ "${KILL_OTHERS:-0}" = "1" ]; then
    pkill -f qemu-system-x86_64 2>/dev/null || true
fi

rm -f "$PPM" "$PNG" "$SERIAL"

QPID=""
cleanup() {
    # Only ever kill the QEMU we started ourselves.
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    rm -f "$PPM" "$QLOG" "$MON" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# -no-reboot so a triple fault ends the run instead of quietly starting a second
# boot underneath the screenshot.
# QEMU_EXTRA is deliberately left unquoted so the shell splits it into separate
# arguments — a device specification is several words. This is a /bin/sh script
# with no arrays, and `set -e` is on, so the alternative (eval) would be both
# uglier and more dangerous. The string comes from the operator's own command
# line, which is already a shell, so word splitting adds no authority that was
# not already there. Nothing model-controlled reaches this variable.
# shellcheck disable=SC2086
qemu-system-x86_64 -kernel "$DIR/kernel.bin" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SERIAL" -no-reboot \
    -display none -monitor "unix:$MON,server,nowait" \
    $QEMU_EXTRA >"$QLOG" 2>&1 &
QPID=$!

PPM="$PPM" MON="$MON" python3 - "$DELAY" <<'EOF'
import os, socket, sys, time

delay = float(sys.argv[1])
ppm   = os.environ["PPM"]
mon   = os.environ["MON"]

time.sleep(delay)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
try:
    s.connect(mon)
except OSError as e:
    s.close()
    print(f"capture: could not reach the QEMU monitor at {mon}: {e}", file=sys.stderr)
    sys.exit(0)          # serial log is still useful; the shell decides the verdict
with s:
    time.sleep(0.3)
    try: s.recv(4096)
    except OSError: pass
    s.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    try: s.recv(4096)
    except OSError: pass
    s.sendall(b"quit\n")
    time.sleep(0.3)
EOF

wait "$QPID" 2>/dev/null || true
QPID=""

sips -s format png "$PPM" --out "$PNG" >/dev/null 2>&1 || true
if [ -s "$PNG" ]; then
    echo "screenshot: $PNG"
else
    echo "screenshot: none (no framebuffer dump was produced)"
fi
echo "serial log: $SERIAL"
echo "===== serial ====="
cat "$SERIAL" 2>/dev/null || true

# No serial output means the guest never ran: a bad kernel.bin, a QEMU that
# refused its arguments, a missing device model. The reason is in QEMU's own
# stderr, which this script used to capture and then delete unread.
if [ ! -s "$SERIAL" ]; then
    echo "capture: FAILED - the guest produced no serial output." >&2
    echo "===== qemu stderr =====" >&2
    cat "$QLOG" >&2 2>/dev/null || true
    exit 1
fi
