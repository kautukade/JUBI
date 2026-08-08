#!/bin/bash

# Script to create a user with root permissions and switch to that user
# Usage: ./create_root_user.sh [username] [password]

set -e

# Default values
USERNAME="${1:-rootuser}"
PASSWORD="${2:-rootpass}"

echo "Creating user: $USERNAME"

# Create the user with home directory and bash shell
if id "$USERNAME" &>/dev/null; then
    echo "User $USERNAME already exists"
else
    sudo useradd -m -s /bin/bash "$USERNAME"
    echo "User $USERNAME created"
fi

# Set password for the user
echo "$USERNAME:$PASSWORD" | sudo chpasswd
echo "Password set for $USERNAME"

# Add user to sudo group for root permissions
sudo usermod -aG sudo "$USERNAME"
echo "User $USERNAME added to sudo group"

# Grant passwordless sudo access (full root capabilities)
echo "$USERNAME ALL=(ALL) NOPASSWD:ALL" | sudo tee /etc/sudoers.d/$USERNAME > /dev/null
sudo chmod 0440 /etc/sudoers.d/$USERNAME
echo "Passwordless sudo access granted to $USERNAME"

echo ""
echo "========================================="
echo "User $USERNAME created with root permissions"
echo "Password: $PASSWORD"
echo "========================================="
echo ""

# Create a startup script for the new user
STARTUP_SCRIPT="/tmp/${USERNAME}_startup.sh"
cat > "$STARTUP_SCRIPT" << 'SCRIPT_EOF'
#!/bin/bash

echo "========================================="
echo "Navigating to /workspace..."
echo "========================================="
cd /workspace || { echo "Failed to navigate to /workspace"; exit 1; }

echo ""
echo "========================================="
echo "Installing CLI tools from ./tools/cli.bash..."
echo "========================================="
if [ -f "./tools/cli.bash" ]; then
    bash ./tools/cli.bash
    echo ""
    echo "========================================="
    echo "CLI installation complete!"
    echo "========================================="
else
    echo "Warning: ./tools/cli.bash not found"
fi

echo ""
echo "========================================="
echo "Setup complete! You are now logged in as $(whoami)"
echo "Current directory: $(pwd)"
echo "========================================="
echo ""

# Start an interactive shell
exec bash -i
SCRIPT_EOF

chmod +x "$STARTUP_SCRIPT"

echo "Switching to user $USERNAME and running setup..."
echo ""

# Switch to the new user and run the startup script
exec sudo -i -u "$USERNAME" bash -c "bash $STARTUP_SCRIPT"
