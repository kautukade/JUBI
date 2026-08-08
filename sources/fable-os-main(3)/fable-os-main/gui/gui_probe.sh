#!/bin/sh
# gui_probe.sh — drive the running kernel's GUI from outside: real PS/2 mouse
#                motion and clicks through the QEMU monitor, real typed
#                sentences through a bidirectional serial socket, and
#                screenshots.
#
#   gui/gui_probe.sh <plan-file> [out-prefix]
#
# A plan is one directive per line:
#
#   wait   <seconds>          just wait
#   await  <regex>            wait (up to 60s) until the serial log matches
#   type   <text>             send "<text>\n" down the serial line, as if typed
#   moveto <x> <y>            put the pointer at absolute pixel x,y
#   click                     press and release the left button where it is
#   shot   <name>             screendump to <out-prefix>-<name>.png
#   echo   <text>             print a marker into this script's own output
#
# WHY A SCRIPT AND NOT capture.sh: capture.sh boots, waits, screenshots and
# quits. Proving a GUI needs the other direction — input — and needs it
# interleaved with output, which means one process holding both the monitor and
# the serial socket. This is that process.
#
# MOUSE MOTION IS RELATIVE. A PS/2 mouse has no absolute position: the monitor's
# `mouse_move dx dy` is a delta, and a delta larger than a 9-bit signed field
# sets the overflow bit, which drivers/input/mouse.c deliberately DROPS (an
# overflowed axis is motion of unknown size, so believing it would move the
# pointer a plausible distance in a possibly wrong direction). So `moveto` slams
# into the top-left corner, where the driver's clamp box pins the position, and
# then steps to the target in moves small enough to always fit.
#
# THE API KEY. Read inside this script's shell, written to a mktemp file with
# mode 0600 outside the repository, handed to the guest over fw_cfg exactly as
# the Makefile's run targets do, and removed however the run ends. It is never
# echoed, never put in argv, and never in a make variable. If there is no key
# the guest boots without one and the API answers 401, which is still enough to
# prove the turn loop runs.
set -eu

PLAN="${1:?usage: gui_probe.sh <plan-file> [out-prefix]}"
OUT="${2:-/tmp/gui}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENVFILE="${FABLEOS_ENV:-$DIR/.env}"

MPORT=$(( 40000 + ($$ % 9000) ))
SPORT=$(( 50000 + ($$ % 9000) ))
QLOG="$(mktemp -t fableos-gui-qemu)"
SERIAL="$OUT.log"
rm -f "$SERIAL"

QPID=""
KEYFILE=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
    [ -n "$KEYFILE" ] && rm -f "$KEYFILE" || true
    rm -f "$QLOG" 2>/dev/null || true
}
trap cleanup EXIT HUP INT QUIT TERM

key=
if [ -f "$ENVFILE" ]; then
    key=$(sed -n 's/^[[:space:]]*\(export[[:space:]]*\)*[A-Z_]*KEY[[:space:]]*=[[:space:]]*//p' \
          "$ENVFILE" | tail -n 1 | tr -d '\r\n' \
          | sed -e 's/^"//' -e 's/"$//' -e "s/^'//" -e "s/'$//")
fi
[ -n "$key" ] || key="${ANTHROPIC_API_KEY:-}"

set --
if [ -n "$key" ]; then
    KEYFILE=$(mktemp "${TMPDIR:-/tmp}/fableos-fwcfg.XXXXXXXX")
    chmod 600 "$KEYFILE"
    printf '%s' "$key" > "$KEYFILE"
    key=
    set -- -fw_cfg "name=opt/fableos/apikey,file=$KEYFILE"
    echo "probe: api key handed to the guest over fw_cfg (never printed)"
else
    echo "probe: no api key; the API will answer 401"
fi

qemu-system-x86_64 -kernel "$DIR/kernel.bin" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "tcp:127.0.0.1:$SPORT,server,nowait" \
    -display none -monitor "tcp:127.0.0.1:$MPORT,server,nowait" \
    "$@" >"$QLOG" 2>&1 &
QPID=$!

MPORT="$MPORT" SPORT="$SPORT" PLAN="$PLAN" OUT="$OUT" SERIAL="$SERIAL" \
python3 - <<'PY'
import os, re, socket, subprocess, sys, time, threading

mport  = int(os.environ["MPORT"]); sport = int(os.environ["SPORT"])
plan   = os.environ["PLAN"];       out   = os.environ["OUT"]
serial_path = os.environ["SERIAL"]

def connect(port, what):
    for _ in range(200):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.settimeout(0.2)
            return s
        except OSError:
            time.sleep(0.1)
    print(f"probe: could not reach the {what} on {port}", file=sys.stderr)
    sys.exit(1)

mon = connect(mport, "monitor")
ser = connect(sport, "serial")

log = bytearray()
stop = False
def reader():
    while not stop:
        try:
            b = ser.recv(4096)
            if not b: break
            log.extend(b)
        except socket.timeout:
            pass
        except OSError:
            break
threading.Thread(target=reader, daemon=True).start()

def mcmd(c):
    mon.sendall((c + "\n").encode())
    time.sleep(0.06)
    try: mon.recv(65536)
    except OSError: pass

def text():
    return log.decode("utf-8", "replace")

def await_re(rx, limit=60.0):
    pat = re.compile(rx, re.M)
    t0 = time.time()
    while time.time() - t0 < limit:
        if pat.search(text()): return True
        time.sleep(0.1)
    print(f"probe: TIMEOUT waiting for /{rx}/", file=sys.stderr)
    return False

# Absolute positioning on a relative device: see the header comment.
STEP = 90
pos = [None, None]
def moveto(x, y):
    for _ in range(14):
        mcmd(f"mouse_move -{STEP} -{STEP}")
    pos[0] = pos[1] = 0
    while pos[0] < x or pos[1] < y:
        dx = min(STEP, x - pos[0]); dy = min(STEP, y - pos[1])
        dx = max(dx, 0); dy = max(dy, 0)
        if dx == 0 and dy == 0: break
        mcmd(f"mouse_move {dx} {dy}")
        pos[0] += dx; pos[1] += dy
    time.sleep(0.15)

def shot(name):
    ppm = f"/tmp/gui-shot-{os.getpid()}.ppm"
    png = f"{out}-{name}.png"
    mcmd(f"screendump {ppm}")
    time.sleep(1.0)
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try: os.unlink(ppm)
    except OSError: pass
    print(f"probe: screenshot {png}")

ok = True
for raw in open(plan):
    line = raw.strip()
    if not line or line.startswith("#"): continue
    op, _, arg = line.partition(" ")
    arg = arg.strip()
    if   op == "wait":   time.sleep(float(arg))
    elif op == "await":  ok = await_re(arg) and ok
    elif op == "type":
        ser.sendall((arg + "\n").encode())
        time.sleep(0.4)
    elif op == "moveto":
        x, y = arg.split()
        moveto(int(x), int(y))
    elif op == "click":
        mcmd("mouse_button 1")
        time.sleep(0.25)
        mcmd("mouse_button 0")
        time.sleep(0.25)
    elif op == "shot":   shot(arg)
    elif op == "echo":   print(f"probe: {arg}")
    else:
        print(f"probe: unknown directive {op!r}", file=sys.stderr)
        ok = False

stop = True
time.sleep(0.3)
open(serial_path, "w").write(text())
mcmd("quit")
sys.exit(0 if ok else 1)
PY
rc=$?

wait "$QPID" 2>/dev/null || true
QPID=""
echo "probe: serial log $SERIAL"
exit $rc
