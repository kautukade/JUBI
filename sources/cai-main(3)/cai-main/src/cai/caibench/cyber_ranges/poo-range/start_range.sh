#!/bin/bash

# P.O.O. Cyber Range Startup Script
# Based on HackTheBox P.O.O. machine

echo "========================================"
echo "   P.O.O. Cyber Range - Startup"
echo "   Professional Offensive Operations"
echo "========================================"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Generate proper .htpasswd file
echo "[*] Generating .htpasswd file..."
# Using openssl to generate Apache-compatible password hash
ADMIN_PASS="EverybodyWantsToWorkAtP.O.O."
HTPASSWD_HASH=$(openssl passwd -apr1 "$ADMIN_PASS")
echo "Administrator:$HTPASSWD_HASH" > webserver/config/.htpasswd
echo "[+] .htpasswd created for Administrator"

# Build and start containers
echo ""
echo "[*] Building Docker containers..."
docker-compose build --no-cache

echo ""
echo "[*] Starting containers..."
docker-compose up -d

# Wait for services to be ready
echo ""
echo "[*] Waiting for services to initialize..."
sleep 10

# Check container status
echo ""
echo "[*] Container Status:"
docker-compose ps

# Display network info
echo ""
echo "========================================"
echo "   Network Information"
echo "========================================"
echo "Attacker:    10.13.38.5  (poo-attacker)"
echo "Web Server:  10.13.38.11 (poo-webserver) - Port 80"
echo "Database:    10.13.38.12 (poo-database)  - Port 5432"
echo "Internal DC: 10.13.38.13 (poo-internal)  - Port 22"
echo ""
echo "Host access: http://localhost:8080"
echo ""
echo "========================================"
echo "   Attack Path Summary"
echo "========================================"
echo "1. Enumerate web server at 10.13.38.11"
echo "2. Find .DS_Store leak -> discover hidden directories"
echo "3. Access /dev/.../db/poo_connection.txt for DB creds"
echo "4. Connect to PostgreSQL, exploit linked server chain"
echo "5. Escalate to superuser via circular FDW link"
echo "6. Find SSH creds, lateral move to internal server"
echo "7. Exploit GenericAll to become Domain Admin"
echo ""
echo "========================================"
echo "   To start attacking:"
echo "========================================"
echo "docker exec -it poo-attacker /bin/bash"
echo ""
