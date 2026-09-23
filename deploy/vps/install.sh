#!/usr/bin/env bash
set -euo pipefail

PREFIX="/opt/jubi"
SERVICE_USER="jubi"
PORT="8877"
OLLAMA_URL="http://127.0.0.1:11434"
START_SERVICE=0
INSTALL_PACKAGES=1
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
  cat <<'EOF'
Usage: sudo bash deploy/vps/install.sh [options]

Options:
  --source PATH        JUBI checkout to install (default: current checkout)
  --prefix PATH        Install prefix (default: /opt/jubi)
  --user NAME          Dedicated service user (default: jubi)
  --port PORT          Loopback dashboard port (default: 8877)
  --ollama-url URL     Local Ollama URL (default: http://127.0.0.1:11434)
  --start              Enable and start jubi.service after installation
  --no-packages        Do not install OS prerequisites
  -h, --help           Show this help

This installer never downloads an AI model. Model acquisition remains an
explicit user action. It also never exposes the Jubi HTTP server on 0.0.0.0.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --user) SERVICE_USER="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --ollama-url) OLLAMA_URL="$2"; shift 2 ;;
    --start) START_SERVICE=1; shift ;;
    --no-packages) INSTALL_PACKAGES=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run this installer as root (sudo)." >&2
  exit 1
fi
if [[ ! "$SERVICE_USER" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]]; then
  echo "Invalid service user name." >&2
  exit 2
fi
if [[ "$PREFIX" != /* || "$PREFIX" == *
  echo "Port must be an unprivileged TCP port between 1024 and 65535." >&2
  exit 2
fi
python3 - "$OLLAMA_URL" <<'PY'
import sys, urllib.parse
u=urllib.parse.urlsplit(sys.argv[1])
try:
    port=u.port
except ValueError:
    raise SystemExit("Invalid Ollama port")
if (u.scheme != "http" or u.hostname not in {"127.0.0.1","localhost"}
        or u.username or u.password or u.path not in {"","/"}
        or u.query or u.fragment or port is None or not 1 <= port <= 65535):
    raise SystemExit("JUBI VPS policy requires a literal loopback Ollama HTTP endpoint with an explicit port")
PY
for required in README.md config/production.json jubi/server.py sarus/server.py; do
  [[ -f "$SOURCE_DIR/$required" ]] || { echo "Missing JUBI source file: $SOURCE_DIR/$required" >&2; exit 2; }
done

if (( INSTALL_PACKAGES )); then
  if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends python3 python3-venv ca-certificates curl rsync
  else
    echo "Automatic package installation currently supports apt-based Linux." >&2
    echo "Install Python 3.11+, venv, curl and rsync, then re-run with --no-packages." >&2
    exit 3
  fi
fi

for command in python3 curl rsync systemctl; do
  command -v "$command" >/dev/null 2>&1 || { echo "Required command not found: $command" >&2; exit 3; }
done

python3 - <<'PY'
import sys
if sys.version_info < (3, 11):
    raise SystemExit("Jubi requires Python 3.11 or newer")
print("Python:", sys.version.split()[0])
PY

if ! getent group "$SERVICE_USER" >/dev/null 2>&1; then
  groupadd --system "$SERVICE_USER"
fi
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --gid "$SERVICE_USER" --create-home --home-dir "/var/lib/$SERVICE_USER" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_USER" "$PREFIX"
rsync -a --delete   --exclude '.git/'   --exclude '.venv/'   --exclude '.sarus-venv/'   --exclude 'data/'   --exclude 'workspace/'   --exclude 'logs/'   --exclude 'node_modules/'   --exclude 'dist-installer/'   "$SOURCE_DIR/" "$PREFIX/"

chmod 0755 "$PREFIX/deploy/vps/"*.sh
install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_USER"   "$PREFIX/data" "$PREFIX/workspace" "$PREFIX/logs"

rm -rf "$PREFIX/.venv"
python3 -m venv "$PREFIX/.venv"
"$PREFIX/.venv/bin/python" -m compileall -q "$PREFIX/jubi" "$PREFIX/sarus"
chown -R "$SERVICE_USER:$SERVICE_USER" "$PREFIX"

install -d -m 0750 /etc/jubi
if [[ ! -f /etc/jubi/jubi.env ]]; then
  cat >/etc/jubi/jubi.env <<EOF
JUBI_HOST=127.0.0.1
JUBI_PORT=$PORT
JUBI_OLLAMA_URL=$OLLAMA_URL
JUBI_DEBUG=0
JUBI_HTTP_LOG=1
PYTHONUNBUFFERED=1
PYTHONDONTWRITEBYTECODE=1
EOF
  chmod 0640 /etc/jubi/jubi.env
  chown root:"$SERVICE_USER" /etc/jubi/jubi.env
else
  echo "Keeping existing /etc/jubi/jubi.env"
fi

sed   -e "s|@JUBI_PREFIX@|$PREFIX|g"   -e "s|@JUBI_USER@|$SERVICE_USER|g"   "$SOURCE_DIR/deploy/vps/jubi.service" >/etc/systemd/system/jubi.service
chmod 0644 /etc/systemd/system/jubi.service

systemctl daemon-reload
systemctl enable jubi.service >/dev/null

if (( START_SERVICE )); then
  systemctl restart jubi.service
  sleep 2
  bash "$PREFIX/deploy/vps/healthcheck.sh"
fi

cat <<EOF

Jubi VPS installation completed.

Backend: 127.0.0.1:$PORT (loopback only)
Service: systemctl status jubi
Logs:    journalctl -u jubi -f

Safe remote dashboard from your PC:
  ssh -N -L $PORT:127.0.0.1:$PORT <ssh-user>@<vps-ip>
Then open:
  http://127.0.0.1:$PORT

Do not open TCP $PORT in the AWS Security Group.
Ollama/model installation is intentionally separate and requires your explicit action.
EOF
\n'* ]]; then
  echo "Install prefix must be an absolute path without newlines." >&2
  exit 2
fi
if [[ ! "$PORT" =~ ^[0-9]+$ ]] || (( PORT < 1024 || PORT > 65535 )); then
  echo "Port must be an unprivileged TCP port between 1024 and 65535." >&2
  exit 2
fi
if [[ "$OLLAMA_URL" != http://127.0.0.1:* && "$OLLAMA_URL" != http://localhost:* ]]; then
  echo "JUBI VPS policy requires Ollama on loopback, not a remote inference endpoint." >&2
  exit 2
fi
for required in README.md config/production.json jubi/server.py sarus/server.py; do
  [[ -f "$SOURCE_DIR/$required" ]] || { echo "Missing JUBI source file: $SOURCE_DIR/$required" >&2; exit 2; }
done

if (( INSTALL_PACKAGES )); then
  if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends python3 python3-venv ca-certificates curl rsync
  else
    echo "Automatic package installation currently supports apt-based Linux." >&2
    echo "Install Python 3.11+, venv, curl and rsync, then re-run with --no-packages." >&2
    exit 3
  fi
fi

for command in python3 curl rsync systemctl; do
  command -v "$command" >/dev/null 2>&1 || { echo "Required command not found: $command" >&2; exit 3; }
done

python3 - <<'PY'
import sys
if sys.version_info < (3, 11):
    raise SystemExit("Jubi requires Python 3.11 or newer")
print("Python:", sys.version.split()[0])
PY

if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --create-home --home-dir "/var/lib/$SERVICE_USER" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_USER" "$PREFIX"
rsync -a --delete   --exclude '.git/'   --exclude '.venv/'   --exclude '.sarus-venv/'   --exclude 'data/'   --exclude 'workspace/'   --exclude 'logs/'   --exclude 'node_modules/'   --exclude 'dist-installer/'   "$SOURCE_DIR/" "$PREFIX/"

install -d -m 0750 -o "$SERVICE_USER" -g "$SERVICE_USER"   "$PREFIX/data" "$PREFIX/workspace" "$PREFIX/logs"

rm -rf "$PREFIX/.venv"
python3 -m venv "$PREFIX/.venv"
"$PREFIX/.venv/bin/python" -m compileall -q "$PREFIX/jubi" "$PREFIX/sarus"
chown -R "$SERVICE_USER:$SERVICE_USER" "$PREFIX"

install -d -m 0750 /etc/jubi
if [[ ! -f /etc/jubi/jubi.env ]]; then
  cat >/etc/jubi/jubi.env <<EOF
JUBI_HOST=127.0.0.1
JUBI_PORT=$PORT
JUBI_OLLAMA_URL=$OLLAMA_URL
JUBI_DEBUG=0
JUBI_HTTP_LOG=1
PYTHONUNBUFFERED=1
EOF
  chmod 0640 /etc/jubi/jubi.env
  chown root:"$SERVICE_USER" /etc/jubi/jubi.env
else
  echo "Keeping existing /etc/jubi/jubi.env"
fi

sed   -e "s|@JUBI_PREFIX@|$PREFIX|g"   -e "s|@JUBI_USER@|$SERVICE_USER|g"   "$SOURCE_DIR/deploy/vps/jubi.service" >/etc/systemd/system/jubi.service
chmod 0644 /etc/systemd/system/jubi.service

systemctl daemon-reload
systemctl enable jubi.service >/dev/null

if (( START_SERVICE )); then
  systemctl restart jubi.service
  sleep 2
  "$PREFIX/deploy/vps/healthcheck.sh"
fi

cat <<EOF

Jubi VPS installation completed.

Backend: 127.0.0.1:$PORT (loopback only)
Service: systemctl status jubi
Logs:    journalctl -u jubi -f

Safe remote dashboard from your PC:
  ssh -N -L $PORT:127.0.0.1:$PORT <ssh-user>@<vps-ip>
Then open:
  http://127.0.0.1:$PORT

Do not open TCP $PORT in the AWS Security Group.
Ollama/model installation is intentionally separate and requires your explicit action.
EOF
