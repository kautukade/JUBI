#!/bin/bash

echo "=========================================="
echo "  Keylogger & Backdoor Cyber Range"
echo "=========================================="
echo ""
echo "Starting the vulnerable server environment..."
echo ""

# Stop any existing containers
docker-compose down 2>/dev/null

# Build and start the container
docker-compose up --build -d

# Wait for container to be ready
echo "Waiting for services to initialize..."
sleep 3

# Check if container is running
if [ "$(docker ps -q -f name=vulnerable-server)" ]; then
    echo ""
    echo "=========================================="
    echo "  Cyber Range is Online!"
    echo "=========================================="
    echo ""
    echo "Target Information:"
    echo "  Container: vulnerable-server"
    echo "  Hostname: corp-server-01"
    echo "  IP Address: 172.30.0.10"
    echo "  SSH Port: localhost:2222"
    echo ""
    echo "Available User Accounts:"
    echo "  - webadmin:WebAdmin123! (sudo access)"
    echo "  - dbuser:Database2023"
    echo "  - jsmith:Summer2023"
    echo ""
    echo "=========================================="
    echo "  Attack Path Objectives"
    echo "=========================================="
    echo ""
    echo "1. Initial Access & Discovery"
    echo "2. Install Keylogger with Persistence"
    echo "3. Create Reverse Shell/Backdoor with Persistence"
    echo "4. Modify System Files to Maintain Access"
    echo "5. Exfiltrate Credentials & Sensitive Data"
    echo ""
    echo "=========================================="
    echo "  Getting Started"
    echo "=========================================="
    echo ""
    echo "Direct Shell Access (simulating initial compromise):"
    echo "  docker exec -it vulnerable-server /bin/bash"
    echo ""
    echo "SSH Access (if you have credentials):"
    echo "  ssh webadmin@localhost -p 2222"
    echo ""
    echo "View full attack path and detailed instructions:"
    echo "  cat README.md"
    echo ""
    echo "Stop the range:"
    echo "  docker-compose down"
    echo ""
    echo "Reset the range:"
    echo "  docker-compose down -v && docker-compose up --build -d"
    echo ""
    echo "=========================================="
    echo "  Happy Hacking!"
    echo "=========================================="
else
    echo ""
    echo "ERROR: Failed to start the vulnerable server."
    echo "Check docker logs for more information:"
    echo "  docker logs vulnerable-server"
    exit 1
fi
