#!/usr/bin/env bash
set -euo pipefail

OLLAMA_URL="${JUBI_OLLAMA_URL:-http://127.0.0.1:11434}"
MODE=""

usage() {
  cat <<'EOF'
Usage: bash deploy/vps/provision-models.sh --recommended

This is an explicit model-acquisition action. It pulls the compact VPS profile:
  qwen3:8b                  general + coding + tool-capable agent work
  qwen2.5vl:3b             vision
  qwen3-embedding:0.6b     semantic memory / RAG
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --recommended) MODE="recommended"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$MODE" == "recommended" ]] || { usage >&2; exit 2; }
case "$OLLAMA_URL" in
  http://127.0.0.1:*|http://localhost:*) ;;
  *) echo "Refusing non-loopback Ollama endpoint: $OLLAMA_URL" >&2; exit 3 ;;
esac

command -v ollama >/dev/null 2>&1 || {
  echo "Ollama CLI is required before model provisioning." >&2
  exit 4
}

curl --fail --silent --show-error --max-time 5 "$OLLAMA_URL/api/tags" >/dev/null
export OLLAMA_HOST="${OLLAMA_URL#http://}"

models=(
  "qwen3:8b"
  "qwen2.5vl:3b"
  "qwen3-embedding:0.6b"
)

for model in "${models[@]}"; do
  echo "Pulling $model ..."
  ollama pull "$model"
done

python3 - "$OLLAMA_URL" "${models[@]}" <<'PY'
import json, sys, urllib.request
base = sys.argv[1].rstrip("/")
required = set(sys.argv[2:])
with urllib.request.urlopen(base + "/api/tags", timeout=5) as response:
    data = json.load(response)
installed = {row.get("name") for row in data.get("models", [])}
missing = sorted(required - installed)
if missing:
    raise SystemExit("Missing after pull: " + ", ".join(missing))
print("Recommended Jubi VPS models ready:", ", ".join(sorted(required)))
PY
