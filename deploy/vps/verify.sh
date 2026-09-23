#!/usr/bin/env bash
set -euo pipefail

PREFIX="${JUBI_PREFIX:-/opt/jubi}"
ENV_FILE="${JUBI_ENV_FILE:-/etc/jubi/jubi.env}"
if [[ -r "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi
PORT="${JUBI_PORT:-8877}"
OLLAMA_URL="${JUBI_OLLAMA_URL:-http://127.0.0.1:11434}"

echo "[1/8] systemd service"
systemctl is-active --quiet jubi.service
echo "  active"

echo "[2/8] Jubi HTTP health"
"$PREFIX/deploy/vps/healthcheck.sh"

echo "[3/8] listener exposure"
if ! command -v ss >/dev/null 2>&1; then
  echo "  ss not installed; listener inspection skipped"
else
  LISTENERS="$(ss -ltnH "( sport = :$PORT )" || true)"
  echo "$LISTENERS"
  if echo "$LISTENERS" | grep -Eq '(^|[[:space:]])(0\.0\.0\.0|\[?::\]?):'; then
    echo "Jubi is exposed on a wildcard address; refusing certification." >&2
    exit 4
  fi
  echo "$LISTENERS" | grep -Eq '127\.0\.0\.1:' || {
    echo "Expected loopback Jubi listener was not found." >&2
    exit 4
  }
fi

echo "[4/8] Ollama local endpoint"
case "$OLLAMA_URL" in
  http://127.0.0.1:*|http://localhost:*) ;;
  *) echo "Remote Ollama endpoint is forbidden in the VPS local-only profile." >&2; exit 5 ;;
esac
curl --fail --silent --show-error --max-time 5 "$OLLAMA_URL/api/tags"   | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  models:", len(d.get("models",[])))'

echo "[5/8] Python source compilation"
"$PREFIX/.venv/bin/python" -m compileall -q "$PREFIX/jubi" "$PREFIX/sarus"

echo "[6/8] Hermes pilot dependencies"
if [[ "${JUBI_REQUIRE_HERMES:-0}" == "1" ]]; then
  "$PREFIX/.venv/bin/python" - <<'PY'
import importlib.metadata
import openai, httpx, pydantic, yaml, rich, run_agent
print("  Hermes:", importlib.metadata.version("hermes-agent"))
PY
else
  if "$PREFIX/.venv/bin/python" -c 'import importlib.metadata; print(importlib.metadata.version("hermes-agent"))' >/dev/null 2>&1; then
    echo "  installed (optional for this profile)"
  else
    echo "  not installed (core VPS remains valid; re-run installer with --with-hermes to test Hermes)"
  fi
fi
echo "[7/8] VPS autonomy runtime"
if [[ "${JUBI_REQUIRE_AUTONOMY:-0}" == "1" ]]; then
  "$PREFIX/.venv/bin/python" -c 'import pytest; print("  pytest:", pytest.__version__)'
  "$PREFIX/.venv/bin/python" "$PREFIX/sarus/core/sandbox_exec.py"     --project "$PREFIX/workspace" -- /bin/true
  "$PREFIX/.venv/bin/python" - "$PREFIX" <<'PY'
import os
import sys
from pathlib import Path
os.environ["JUBI_DEPLOYMENT_PROFILE"]="linux_vps"
from sarus.core.development import DevelopmentWorkspace
root=Path(sys.argv[1]).resolve()
ws=DevelopmentWorkspace(root)
print("  development workspace:", ws.project)
PY
else
  echo "  optional/not required by this deployment"
fi

echo "[8/8] Full Jubi readiness"
if [[ "${JUBI_REQUIRE_AUTONOMY:-0}" == "1" ]]; then
  READINESS="$(curl --fail --silent --show-error --max-time 20 "http://127.0.0.1:$PORT/api/vps/readiness?full=1")"
  printf '%s' "$READINESS" | python3 -c '
import json,sys
data=json.load(sys.stdin)
failed=[x for x in data.get("checks",[]) if x.get("required") and not x.get("ok")]
if not data.get("ready"):
    print("Full VPS readiness: FAIL", file=sys.stderr)
    for item in failed:
        print(" -", item.get("name"), ":", item.get("detail"), file=sys.stderr)
    raise SystemExit(7)
print("  full readiness: PASS (%s/%s)" % (data.get("passed"), data.get("required")))
'
else
  echo "  full readiness not required by this deployment"
fi

echo "VPS verification: PASS"
