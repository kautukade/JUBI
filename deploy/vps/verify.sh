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

echo "[1/6] systemd service"
systemctl is-active --quiet jubi.service
echo "  active"

echo "[2/6] Jubi HTTP health"
"$PREFIX/deploy/vps/healthcheck.sh"

echo "[3/6] listener exposure"
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

echo "[4/6] Ollama local endpoint"
case "$OLLAMA_URL" in
  http://127.0.0.1:*|http://localhost:*) ;;
  *) echo "Remote Ollama endpoint is forbidden in the VPS local-only profile." >&2; exit 5 ;;
esac
curl --fail --silent --show-error --max-time 5 "$OLLAMA_URL/api/tags"   | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  models:", len(d.get("models",[])))'

echo "[5/6] Python source compilation"
"$PREFIX/.venv/bin/python" -m compileall -q "$PREFIX/jubi" "$PREFIX/sarus"

echo "[6/6] Hermes pilot dependencies"
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
echo "VPS verification: PASS"
