#!/usr/bin/env bash
set -euo pipefail

PREFIX="\${JUBI_PREFIX:-/opt/jubi}"
ENV_FILE="\${JUBI_ENV_FILE:-/etc/jubi/jubi.env}"

if [[ -r "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

SERVICE_USER="\${JUBI_SERVICE_USER:-jubi}"
PY="$PREFIX/.venv/bin/python"

[[ -x "$PY" ]] || { echo "Jubi Python runtime not found: $PY" >&2; exit 2; }
[[ -d "$PREFIX/jubi" ]] || { echo "Jubi install not found: $PREFIX" >&2; exit 2; }

run_acceptance() {
  cd "$PREFIX"
  exec "$PY" -m jubi.vps_acceptance "$@"
}

if [[ "$(id -u)" -eq 0 ]]; then
  command -v runuser >/dev/null 2>&1 || {
    echo "runuser is required to execute acceptance as the Jubi service user" >&2
    exit 3
  }
  cd "$PREFIX"
  exec runuser -u "$SERVICE_USER" -- \
    env \
      JUBI_HOST="\${JUBI_HOST:-127.0.0.1}" \
      JUBI_PORT="\${JUBI_PORT:-8877}" \
      JUBI_OLLAMA_URL="\${JUBI_OLLAMA_URL:-http://127.0.0.1:11434}" \
      JUBI_DEPLOYMENT_PROFILE=linux_vps \
      JUBI_REQUIRE_HERMES="\${JUBI_REQUIRE_HERMES:-1}" \
      JUBI_REQUIRE_AUTONOMY="\${JUBI_REQUIRE_AUTONOMY:-1}" \
      PYTHONUNBUFFERED=1 \
      PYTHONDONTWRITEBYTECODE=1 \
      PYTHONNOUSERSITE=1 \
      "$PY" -m jubi.vps_acceptance "$@"
fi

if [[ "$(id -un)" != "$SERVICE_USER" ]]; then
  echo "Run as root (the wrapper will drop to $SERVICE_USER) or directly as $SERVICE_USER." >&2
  exit 3
fi

run_acceptance "$@"
