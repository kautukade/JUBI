#!/usr/bin/env bash
set -euo pipefail

PREFIX="${JUBI_PREFIX:-/opt/jubi}"
BACKUP_DIR="${JUBI_BACKUP_DIR:-/var/backups/jubi}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TARGET="$BACKUP_DIR/jubi-$STAMP.tar.gz"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

install -d -m 0700 "$BACKUP_DIR"
install -d -m 0700 "$TMP/data" "$TMP/workspace"

DB="$PREFIX/data/sarus.db"
if [[ -f "$DB" ]]; then
  python3 - "$DB" "$TMP/data/sarus.db" <<'PY'
import sqlite3,sys
src=sqlite3.connect(sys.argv[1])
dst=sqlite3.connect(sys.argv[2])
with dst:
    src.backup(dst)
dst.close()
src.close()
PY
fi

if [[ -d "$PREFIX/data" ]]; then
  rsync -a --exclude 'sarus.db' --exclude 'sarus.db-wal' --exclude 'sarus.db-shm'     "$PREFIX/data/" "$TMP/data/"
fi
if [[ -d "$PREFIX/workspace" ]]; then
  rsync -a "$PREFIX/workspace/" "$TMP/workspace/"
fi

tar -C "$TMP" -czf "$TARGET" data workspace
chmod 0600 "$TARGET"
echo "$TARGET"
