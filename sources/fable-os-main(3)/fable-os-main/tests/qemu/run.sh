#!/bin/sh
# QEMU boot-assertion suite for fable-os.  Entry point for `make test-qemu`.
#
#   tests/qemu/run.sh            run every case in tests/qemu/cases/
#   tests/qemu/run.sh boot       run only cases matching "boot"
#   tests/qemu/run.sh -v         keep/print the serial log path for passes
#
# Exits 0 if every case passed, 1 otherwise.  See harness.py for the env vars
# (FABLEOS_TEST_OFFLINE, FABLEOS_TEST_TIMEOUT, ...) and cases/boot.case for the
# case-file syntax.
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
OSDIR=$(cd "$DIR/../.." && pwd)

PYTHON=${FABLEOS_TEST_PYTHON:-python3}
command -v "$PYTHON" >/dev/null 2>&1 || { echo "error: $PYTHON not found"; exit 1; }

# Build the kernel if it isn't there. `make test-qemu` already depends on
# kernel.bin, but the script has to stand alone too.
if [ ! -f "$OSDIR/kernel.bin" ]; then
    echo "kernel.bin missing — building..."
    make -C "$OSDIR" -j8 >/dev/null || { echo "error: build failed"; exit 1; }
fi

exec "$PYTHON" "$DIR/harness.py" "$@"
