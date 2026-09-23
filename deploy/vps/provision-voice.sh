#!/usr/bin/env bash
set -euo pipefail

PREFIX="${JUBI_PREFIX:-/opt/jubi}"
MODEL_SIZE=""
TARGET=""

usage() {
  cat <<'EOF'
Usage: sudo bash deploy/vps/provision-voice.sh --small [--prefix PATH]

Explicitly downloads one local faster-whisper model for offline VPS speech:
  small -> Systran/faster-whisper-small

The model is stored under the Jubi install tree and is loaded with
local_files_only=True at runtime. No paid speech API is used.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --small) MODEL_SIZE="small"; shift ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$(id -u)" -eq 0 ]] || { echo "Run with sudo." >&2; exit 1; }
[[ "$MODEL_SIZE" == "small" ]] || { usage >&2; exit 2; }
[[ "$PREFIX" == /* ]] || { echo "Prefix must be absolute." >&2; exit 2; }

PY="$PREFIX/.venv/bin/python"
[[ -x "$PY" ]] || { echo "Jubi virtual environment not found at $PY" >&2; exit 3; }

TARGET="$PREFIX/.voice/whisper-$MODEL_SIZE"
install -d -m 0750 "$PREFIX/.voice"
rm -rf "$TARGET"
install -d -m 0750 "$TARGET"

"$PY" - "$TARGET" <<'PY'
import pathlib
import sys
from huggingface_hub import snapshot_download

target = pathlib.Path(sys.argv[1])
snapshot_download(
    repo_id="Systran/faster-whisper-small",
    local_dir=str(target),
)
required = ["model.bin", "config.json", "tokenizer.json"]
missing = [name for name in required if not (target / name).is_file()]
if missing:
    raise SystemExit("Voice model download incomplete: " + ", ".join(missing))
print("Local faster-whisper model ready:", target)
PY

SERVICE_USER="$(systemctl show -p User --value jubi.service 2>/dev/null || true)"
SERVICE_USER="${SERVICE_USER:-jubi}"
chown -R "$SERVICE_USER:$SERVICE_USER" "$PREFIX/.voice"

ENV_FILE=/etc/jubi/jubi.env
[[ -f "$ENV_FILE" ]] || { echo "Missing $ENV_FILE" >&2; exit 4; }
if grep -q '^JUBI_WHISPER_MODEL=' "$ENV_FILE"; then
  sed -i "s|^JUBI_WHISPER_MODEL=.*|JUBI_WHISPER_MODEL=$TARGET|" "$ENV_FILE"
else
  echo "JUBI_WHISPER_MODEL=$TARGET" >>"$ENV_FILE"
fi
if grep -q '^JUBI_REQUIRE_VOICE=' "$ENV_FILE"; then
  sed -i 's/^JUBI_REQUIRE_VOICE=.*/JUBI_REQUIRE_VOICE=1/' "$ENV_FILE"
else
  echo 'JUBI_REQUIRE_VOICE=1' >>"$ENV_FILE"
fi
chmod 0640 "$ENV_FILE"

echo "Offline Jubi voice model provisioned: $TARGET"
