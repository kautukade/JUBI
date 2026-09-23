#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="/opt/jubi"
SERVICE_USER="jubi"
PORT="8877"

usage() {
  cat <<'EOF'
Usage: sudo bash deploy/vps/full-profile.sh [--source PATH] [--prefix PATH] [--user NAME] [--port PORT]

Prerequisite: Ollama must already be installed and reachable on
http://127.0.0.1:11434.

This explicit full profile installs Hermes dependencies, Playwright Chromium,
and the recommended local Ollama models, then requires all VPS readiness gates.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --user) SERVICE_USER="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$(id -u)" -eq 0 ]] || { echo "Run with sudo." >&2; exit 1; }
command -v ollama >/dev/null 2>&1 || {
  echo "Ollama must be installed before the full Jubi VPS profile." >&2
  exit 3
}
curl --fail --silent --show-error --max-time 5 http://127.0.0.1:11434/api/tags >/dev/null || {
  echo "Ollama is not reachable on 127.0.0.1:11434." >&2
  exit 3
}

bash "$SOURCE_DIR/deploy/vps/install.sh"   --source "$SOURCE_DIR"   --prefix "$PREFIX"   --user "$SERVICE_USER"   --port "$PORT"   --with-hermes   --with-browser   --with-voice

JUBI_OLLAMA_URL=http://127.0.0.1:11434   bash "$PREFIX/deploy/vps/provision-models.sh" --recommended
bash "$PREFIX/deploy/vps/provision-voice.sh" --small --prefix "$PREFIX"

ENV_FILE=/etc/jubi/jubi.env
if grep -q '^JUBI_REQUIRE_FULL_MODELS=' "$ENV_FILE"; then
  sed -i 's/^JUBI_REQUIRE_FULL_MODELS=.*/JUBI_REQUIRE_FULL_MODELS=1/' "$ENV_FILE"
else
  echo 'JUBI_REQUIRE_FULL_MODELS=1' >>"$ENV_FILE"
fi
if grep -q '^JUBI_REQUIRE_HERMES=' "$ENV_FILE"; then
  sed -i 's/^JUBI_REQUIRE_HERMES=.*/JUBI_REQUIRE_HERMES=1/' "$ENV_FILE"
fi
if grep -q '^JUBI_REQUIRE_BROWSER=' "$ENV_FILE"; then
  sed -i 's/^JUBI_REQUIRE_BROWSER=.*/JUBI_REQUIRE_BROWSER=1/' "$ENV_FILE"
fi
if grep -q '^JUBI_REQUIRE_VOICE=' "$ENV_FILE"; then
  sed -i 's/^JUBI_REQUIRE_VOICE=.*/JUBI_REQUIRE_VOICE=1/' "$ENV_FILE"
else
  echo 'JUBI_REQUIRE_VOICE=1' >>"$ENV_FILE"
fi
chmod 0640 "$ENV_FILE"
chown root:"$SERVICE_USER" "$ENV_FILE"

systemctl restart jubi.service
sleep 2
bash "$PREFIX/deploy/vps/verify.sh"

echo "Running real local-model VPS acceptance..."
sudo -u "$SERVICE_USER" env \
  JUBI_DEPLOYMENT_PROFILE=linux_vps \
  JUBI_REQUIRE_FULL_MODELS=1 \
  JUBI_REQUIRE_HERMES=1 \
  JUBI_REQUIRE_BROWSER=1 \
  JUBI_REQUIRE_VOICE=1 \
  JUBI_OLLAMA_URL=http://127.0.0.1:11434 \
  PLAYWRIGHT_BROWSERS_PATH="$PREFIX/.playwright" \
  "$PREFIX/.venv/bin/python" "$PREFIX/scripts/vps_live_acceptance.py"

echo "Jubi full VPS profile: PASS"
