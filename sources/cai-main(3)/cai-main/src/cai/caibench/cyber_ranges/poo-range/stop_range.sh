#!/bin/bash

# P.O.O. Cyber Range Stop Script

echo "[*] Stopping P.O.O. Cyber Range..."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

docker-compose down -v

echo "[+] Cyber range stopped and cleaned up."
