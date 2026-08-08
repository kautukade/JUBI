#!/bin/bash

# CAI CTF Attack/Defense - Cleanup Script
# Removes any existing team containers before starting the game

echo "================================="
echo "CAI CTF Cleanup Script"
echo "================================="
echo ""

# Check if Docker is running
docker ps > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "❌ Error: Docker is not running or accessible"
    echo "Please start Docker Desktop and try again"
    exit 1
fi

echo "Looking for existing team containers..."

# Remove any existing team containers
for i in {1..10}; do
    for ctf in cowsay notes devops; do
        container_name="${ctf}_team_${i}"

        # Check if container exists
        if docker ps -a --format "{{.Names}}" | grep -q "^${container_name}$"; then
            echo "Found container: $container_name"

            # Stop container if running
            docker stop "$container_name" 2>/dev/null && echo "  ✓ Stopped $container_name"

            # Remove container
            docker rm "$container_name" 2>/dev/null && echo "  ✓ Removed $container_name"
        fi
    done
done

echo ""
echo "✅ Cleanup complete"
echo ""
echo "You can now start the game server with:"
echo "  ./start.sh"