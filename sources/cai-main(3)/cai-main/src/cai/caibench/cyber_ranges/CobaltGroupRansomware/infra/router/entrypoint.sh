#!/bin/sh

# Enable IP forwarding
echo 1 > /proc/sys/net/ipv4/ip_forward

# Flush existing rules
iptables -F
iptables -t nat -F

# Basic Policy: Allow Everything for this easy mode range (or restrict if we want to be nasty)
# But we want to simulate a corporate network where routing exists.
iptables -P FORWARD ACCEPT

# Masquerade for internet access (simulated)
# We want office PC to reach C2 on public_net
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE

# Log traffic (optional)
# iptables -A FORWARD -j LOG --log-prefix "ROUTER: "

echo "Router started with IP forwarding enabled."

# Keep container running
tail -f /dev/null
