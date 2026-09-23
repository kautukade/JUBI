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

echo "[1/9] systemd service"
systemctl is-active --quiet jubi.service
echo "  active"

echo "[2/9] Jubi HTTP health"
"$PREFIX/deploy/vps/healthcheck.sh"

echo "[3/9] listener exposure"
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

echo "[4/9] Ollama local endpoint"
case "$OLLAMA_URL" in
  http://127.0.0.1:*|http://localhost:*) ;;
  *) echo "Remote Ollama endpoint is forbidden in the VPS local-only profile." >&2; exit 5 ;;
esac
curl --fail --silent --show-error --max-time 5 "$OLLAMA_URL/api/tags"   | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  models:", len(d.get("models",[])))'
if [[ "${JUBI_REQUIRE_FULL_MODELS:-0}" == "1" ]]; then
  curl --fail --silent --show-error --max-time 5 "$OLLAMA_URL/api/tags" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); have={x.get("name") for x in d.get("models",[])}; need={"qwen3:8b","qwen2.5vl:3b","qwen3-embedding:0.6b"}; missing=sorted(need-have); print("  full-profile models:", ", ".join(sorted(need))); sys.exit("missing: "+", ".join(missing)) if missing else None'
fi

echo "[5/9] Python source compilation"
"$PREFIX/.venv/bin/python" -m compileall -q "$PREFIX/jubi" "$PREFIX/sarus"

echo "[6/9] Hermes pilot dependencies"
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
echo "[7/9] VPS browser runtime"
if [[ "${JUBI_REQUIRE_BROWSER:-0}" == "1" ]]; then
  "$PREFIX/.venv/bin/python" - <<'PY'
from sarus.core.browser import VPSBrowser
class App: pass
status = VPSBrowser(App()).status()
if not status.get("ready"):
    raise SystemExit("Playwright browser dependency is required but unavailable")
print("  browser:", status["engine"])
PY
  PLAYWRIGHT_BROWSERS_PATH="${PLAYWRIGHT_BROWSERS_PATH:-$PREFIX/.playwright}"     "$PREFIX/.venv/bin/python" -m playwright install --dry-run chromium >/dev/null
else
  echo "  optional/not-required"
fi

echo "[8/9] Offline voice runtime"
if [[ "${JUBI_REQUIRE_VOICE:-0}" == "1" ]]; then
  "$PREFIX/.venv/bin/python" - <<'PY'
from pathlib import Path
from sarus.core.voice import VPSVoice
class App: pass
status = VPSVoice(App()).status()
if not status.get("ready"):
    raise SystemExit("Offline voice is required but unavailable: " + str(status))
model = status.get("whisper_model")
if not model or not Path(model).is_dir():
    raise SystemExit("Local Whisper model directory is missing")
if status.get("cloud_speech") or status.get("microphone_capture"):
    raise SystemExit("VPS voice boundary is not local/headless as expected")
print("  STT/TTS ready:", status.get("mode"))
PY
else
  echo "  optional/not-required"
fi

echo "[9/9] Jubi consolidated VPS readiness"
READINESS="$(curl --fail --silent --show-error --max-time 10 "http://127.0.0.1:$PORT/api/vps/readiness")"
printf '%s' "$READINESS" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  ready:", d.get("ready"), "failed:", ", ".join(d.get("failed_required",[]))); sys.exit(0 if d.get("ready") else 1)'

echo "VPS verification: PASS"
