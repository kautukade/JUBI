#!/bin/bash

# Set passwords (simplified to avoid shell escaping issues)
echo 'p00_adm:ZQ5t4r' | chpasswd
echo 'mr3ks:DomainAdminPass123' | chpasswd
echo 'webadmin:EverybodyWantsToWorkAtPOO' | chpasswd

# Start SSH daemon
exec /usr/sbin/sshd -D
