#!/bin/bash

# CAI CTF Attack/Defense Game Server Startup Script

echo "================================="
echo "CAI CTF Attack/Defense GameServer"
echo "================================="
echo ""

# Check if Docker is running
docker ps > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "❌ Error: Docker is not running or accessible"
    echo "Please start Docker Desktop and try again"
    exit 1
fi

echo "✅ Docker is running"

# Handle pyproject.toml - copy from parent directory
#echo "Setting up pyproject.toml..."
#if [ -f "pyproject.toml" ]; then
#    echo "  Removing existing pyproject.toml"
#    rm -f pyproject.toml
#fi

# Copy from ../../../../pyproject.toml
#if [ -f "../../../../pyproject.toml" ]; then
#    cp ../../../../pyproject.toml .
#    echo "  ✓ Copied pyproject.toml from parent directory"
#else
#    echo "  ⚠️  Warning: ../../../../pyproject.toml not found"
#fi

# Check for --cleanup flag
if [ "$1" == "--cleanup" ] || [ "$2" == "--cleanup" ]; then
    echo ""
    echo "Running cleanup first..."
    ./cleanup.sh
    echo ""
fi

# Check Python dependencies
echo "Checking dependencies..."
pip install -q flask flask-cors docker pyyaml paramiko requests 2>/dev/null

# Parse arguments
AUTO_START=""
if [ "$1" == "--auto-start" ] || [ "$2" == "--auto-start" ]; then
    AUTO_START="--auto-start"
    echo "Auto-start mode enabled"
fi

# Start the game server
echo ""
echo "Starting game server..."
echo "Dashboard will be available at: http://localhost:12345"
echo ""
echo "Press Ctrl+C to stop the server"
echo "---------------------------------"
echo ""

python gameserver.py $AUTO_START