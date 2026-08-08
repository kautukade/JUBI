#!/bin/bash

echo "Starting ADVANCED Cobalt Group Cyber Range..."
echo "Building and starting containers..."

docker-compose up --build -d

echo "Range is running."
echo "Architecture Overview:"
echo "  [Internet Segment] -> C2 Server (172.20.0.10)"
echo "  [DMZ Segment]      -> Public Web (172.21.0.10)"
echo "  [Office Segment]   -> HR PC (172.22.0.10) [Entry Point], Dev PC (172.22.0.20)"
echo "  [Server Segment]   -> Intranet, DB (172.23.0.20), Backup (172.23.0.30)"
echo "  [Router]           -> Connects all segments."

echo ""
echo "!!! ATTACK START !!!"
echo "Access the entry point (HR PC) to begin your operation:"
echo "  docker exec -it hr-pc /bin/bash"
echo ""
echo "Hint: Look for 'sticky_note.txt' or run 'python3 simulate_phish.py' to get the payload."
