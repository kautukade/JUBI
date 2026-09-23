#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${JUBI_ENV_FILE:-/etc/jubi/jubi.env}"
if [[ -r "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

HOST="${JUBI_HOST:-127.0.0.1}"
PORT="${JUBI_PORT:-8877}"

case "$HOST" in
  127.0.0.1|localhost|::1) ;;
  *) echo "Refusing health check: JUBI_HOST is not loopback ($HOST)" >&2; exit 2 ;;
esac

URL="http://127.0.0.1:$PORT/api/health"
BODY="$(curl --fail --silent --show-error --max-time 5 "$URL")"
printf '%s' "$BODY" | python3 -c '
import json,sys
data=json.load(sys.stdin)
if data.get("status")!="ok" or data.get("product")!="Jubi":
    raise SystemExit("unexpected Jubi health response")
print("Jubi health: OK", data.get("version",""))
'
