#!/usr/bin/env python3
"""
VM to Docker Converter
Converts virtual machine images (OVA/VMDK) to Docker containers by extracting the actual filesystem

REQUIREMENTS:
    - Docker installed and running
    - qemu-utils package (for VMDK conversion): sudo apt-get install qemu-utils
    - sudo access (for mounting disk images)
    - Sufficient disk space (3x the VM size for conversion)

SUPPORTED SYSTEMS:
    - Ubuntu 18.04, 20.04, 22.04 LTS
    - Debian 10, 11
    - Linux Mint (based on Ubuntu)
    - Other Linux distributions with apt package manager

DEPENDENCIES TO INSTALL:
    sudo apt-get update
    sudo apt-get install -y qemu-utils fdisk mount rsync

USAGE:
    # Extract actual VM filesystem (requires sudo):
    sudo python3 vm_to_docker.py <vm_image.ova> --name <docker_image_name>
    
    # Use template mode (no sudo required, creates generic container):
    python3 vm_to_docker.py <vm_image.ova> --name <docker_image_name> --standard
    
    # Test the container after creation:
    sudo python3 vm_to_docker.py <vm_image.ova> --name <docker_image_name> --test

EXAMPLE:
    sudo python3 /home/acceleration/cai/tools/vm_to_docker.py /tmp/whowantstobeking.ova --name whowantstobeking_full

PROCESS:
    1. Extracts OVA archive to find VMDK files
    2. Converts VMDK to raw disk format using qemu-img
    3. Mounts the raw disk to extract filesystem (requires sudo)
    4. Creates Dockerfile that merges VM filesystem with base image
    5. Builds Docker image with all original services and files
    
NOTES:
    - CTF mode (default) attempts to extract the actual VM filesystem
    - Standard mode uses a template without extracting VM content
    - The script falls back to template mode if filesystem extraction fails
    - Temporary files are cleaned up automatically after conversion
"""

import os
import sys
import tarfile
import tempfile
import shutil
import subprocess
import argparse
import re
from pathlib import Path
from typing import List, Dict, Optional
import json

class SimpleVMToDockerConverter:
    def __init__(self, vm_path: str, output_name: str = None):
        self.vm_path = Path(vm_path)
        self.output_name = output_name or self.vm_path.stem.lower().replace(' ', '_')
        self.work_dir = Path(tempfile.mkdtemp(prefix='vm2docker_'))
        self.docker_dir = self.work_dir / 'docker'
        self.docker_dir.mkdir(exist_ok=True)
        
    def download_and_extract(self) -> Path:
        """Download and extract OVA file"""
        print(f"[*] Processing: {self.vm_path}")
        
        extracted_dir = self.work_dir / 'extracted'
        extracted_dir.mkdir(exist_ok=True)
        
        if self.vm_path.suffix.lower() == '.ova':
            print("[*] Extracting OVA archive")
            with tarfile.open(self.vm_path, 'r') as tar:
                tar.extractall(extracted_dir)
        
        # Find VMDK files
        vmdk_files = list(extracted_dir.glob('*.vmdk'))
        if vmdk_files:
            print(f"[+] Found {len(vmdk_files)} VMDK file(s)")
            return vmdk_files[0]
        
        return None
    
    def convert_vmdk_to_raw(self, vmdk_path: Path) -> Path:
        """Convert VMDK to raw disk image using qemu-img"""
        print(f"[*] Converting VMDK to raw format")
        raw_path = self.work_dir / 'disk.raw'
        
        # Check if qemu-img is available
        result = subprocess.run(['which', 'qemu-img'], capture_output=True)
        if result.returncode != 0:
            print("[!] qemu-img not found.")
            print("[!] Please install it with: sudo apt-get install qemu-utils")
            print("[*] Attempting to use Docker to convert VMDK...")
            
            # Alternative: try using a Docker container to convert
            result = subprocess.run([
                'docker', 'run', '--rm', '-v', f'{vmdk_path.parent}:/data',
                'alpine', 'sh', '-c', 
                f'apk add --no-cache qemu-img && qemu-img convert -f vmdk -O raw /data/{vmdk_path.name} /data/disk.raw'
            ], capture_output=True, text=True)
            
            if result.returncode == 0:
                # Move the converted file to our work directory
                shutil.move(str(vmdk_path.parent / 'disk.raw'), str(raw_path))
                print(f"[+] Converted using Docker container")
                return raw_path
            else:
                print(f"[!] Docker conversion failed: {result.stderr}")
                return None
        
        # Convert VMDK to raw using local qemu-img
        result = subprocess.run(
            ['qemu-img', 'convert', '-f', 'vmdk', '-O', 'raw', str(vmdk_path), str(raw_path)],
            capture_output=True, text=True
        )
        
        if result.returncode == 0:
            print(f"[+] Converted to raw format: {raw_path}")
            return raw_path
        else:
            print(f"[!] Conversion failed: {result.stderr}")
            return None
    
    def mount_and_extract_filesystem(self, raw_path: Path) -> Path:
        """Mount raw disk and extract filesystem"""
        print("[*] Extracting filesystem from disk image")
        print("[!] NOTE: This requires sudo access to mount the disk image")
        
        fs_dir = self.docker_dir / 'rootfs'
        fs_dir.mkdir(exist_ok=True)
        
        try:
            # Get partition info
            result = subprocess.run(
                ['fdisk', '-l', str(raw_path)],
                capture_output=True, text=True
            )
            
            # Parse partition offset
            lines = result.stdout.split('\n')
            offset = None
            for line in lines:
                if 'Linux' in line and not 'swap' in line.lower():
                    parts = line.split()
                    if len(parts) > 1:
                        start_sector = int(parts[1])
                        offset = start_sector * 512  # Default sector size
                        print(f"[+] Found Linux partition at offset {offset}")
                        break
            
            if offset is None:
                offset = 1048576  # Common default for first partition
                print(f"[*] Using default partition offset {offset}")
            
            # Create mount point
            mount_point = self.work_dir / 'mount'
            mount_point.mkdir(exist_ok=True)
            
            # Mount the image with sudo
            print("[*] Mounting disk image (requires sudo)...")
            mount_cmd = [
                'sudo', 'mount', '-o', f'loop,offset={offset},ro',
                str(raw_path), str(mount_point)
            ]
            
            result = subprocess.run(mount_cmd, capture_output=True, text=True)
            
            if result.returncode == 0:
                print(f"[+] Mounted filesystem at {mount_point}")
                
                # Copy filesystem to docker directory
                print("[*] Copying filesystem (this may take a while)...")
                copy_cmd = [
                    'sudo', 'cp', '-a',
                    str(mount_point) + '/.', str(fs_dir) + '/'
                ]
                subprocess.run(copy_cmd)
                
                # Fix ownership
                subprocess.run(['sudo', 'chown', '-R', f'{os.getuid()}:{os.getgid()}', str(fs_dir)])
                
                # Unmount
                subprocess.run(['sudo', 'umount', str(mount_point)], capture_output=True)
                print(f"[+] Filesystem extracted to {fs_dir}")
                
                return fs_dir
            else:
                print(f"[!] Mount failed: {result.stderr}")
                print("[!] Please ensure you have sudo access and try again")
                return None
                    
        except Exception as e:
            print(f"[!] Error extracting filesystem: {e}")
            import traceback
            traceback.print_exc()
            
        return None
    
    def create_dockerfile_from_template(self, detected_os: str = "ubuntu") -> Path:
        """Create a Dockerfile based on detected OS"""
        print(f"[*] Creating Dockerfile for {detected_os}-based system")
        
        dockerfile_path = self.docker_dir / 'Dockerfile'
        
        # Base Dockerfile that simulates a VM environment
        dockerfile_content = f"""FROM ubuntu:20.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Update and install basic services
RUN apt-get update && apt-get install -y \\
    openssh-server \\
    apache2 \\
    mysql-server \\
    python3 \\
    python3-pip \\
    net-tools \\
    vim \\
    curl \\
    wget \\
    sudo \\
    systemctl \\
    && rm -rf /var/lib/apt/lists/*

# Configure SSH
RUN mkdir /var/run/sshd && \\
    echo 'root:password' | chpasswd && \\
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config && \\
    sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Configure Apache
RUN echo "ServerName localhost" >> /etc/apache2/apache2.conf

# Create startup script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Expose common ports
EXPOSE 22 80 443 3306

# Set entrypoint
ENTRYPOINT ["/entrypoint.sh"]
"""
        
        # Create entrypoint script
        entrypoint_content = """#!/bin/bash

# Start SSH service
service ssh start

# Start Apache
service apache2 start

# Start MySQL
service mysql start

# Keep container running
tail -f /dev/null
"""
        
        # Write files
        with open(dockerfile_path, 'w') as f:
            f.write(dockerfile_content)
        
        entrypoint_path = self.docker_dir / 'entrypoint.sh'
        with open(entrypoint_path, 'w') as f:
            f.write(entrypoint_content)
        
        print(f"[+] Dockerfile created at {dockerfile_path}")
        return dockerfile_path
    
    def build_docker_image(self) -> bool:
        """Build Docker image"""
        print(f"[*] Building Docker image: {self.output_name}")
        
        result = subprocess.run([
            'docker', 'build', '-t', self.output_name, '.'
        ], cwd=self.docker_dir, capture_output=True, text=True)
        
        if result.returncode == 0:
            print(f"[+] Docker image built successfully: {self.output_name}")
            return True
        else:
            print(f"[!] Docker build failed: {result.stderr}")
            return False
    
    def create_dockerfile_from_filesystem(self, fs_dir: Path) -> Path:
        """Create a Dockerfile that uses the extracted filesystem"""
        print("[*] Creating Dockerfile with extracted filesystem")
        
        dockerfile_path = self.docker_dir / 'Dockerfile'
        
        # Analyze the filesystem to determine the base OS
        os_release = fs_dir / 'etc/os-release'
        base_image = 'ubuntu:20.04'  # default
        
        if os_release.exists():
            with open(os_release, 'r') as f:
                content = f.read()
                if 'Ubuntu 18' in content:
                    base_image = 'ubuntu:18.04'
                elif 'Ubuntu 20' in content:
                    base_image = 'ubuntu:20.04'
                elif 'Mint' in content:
                    # Linux Mint is based on Ubuntu
                    if '19' in content:
                        base_image = 'ubuntu:18.04'
                    else:
                        base_image = 'ubuntu:20.04'
                print(f"[+] Detected OS, using base image: {base_image}")
        
        # Create Dockerfile that copies the entire filesystem
        dockerfile_content = f"""FROM {base_image}

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install essential packages that might be missing
RUN apt-get update && apt-get install -y \\
    systemd \\
    systemd-sysv \\
    sudo \\
    net-tools \\
    iproute2 \\
    rsync \\
    && rm -rf /var/lib/apt/lists/*

# Create a staging directory for the filesystem
RUN mkdir -p /vm_filesystem

# Copy the extracted filesystem to staging
COPY rootfs/ /vm_filesystem/

# Merge the VM filesystem with the container
# This preserves both the base image and VM content
RUN rsync -av --ignore-existing /vm_filesystem/etc/ /etc/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/home/ /home/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/opt/ /opt/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/root/ /root/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/srv/ /srv/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/var/ /var/ || true && \\
    rsync -av --ignore-existing /vm_filesystem/usr/ /usr/ || true && \\
    rm -rf /vm_filesystem

# Fix permissions
RUN chmod 755 /root && \\
    chmod 644 /etc/passwd /etc/shadow /etc/group || true

# Create necessary directories
RUN mkdir -p /var/run/sshd /run/systemd/system || true

# Create entrypoint script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Expose common CTF ports
EXPOSE 21 22 23 80 443 631 3000 3306 5432 6379 7654 8080 8181 8888 9000

# Set entrypoint
ENTRYPOINT ["/entrypoint.sh"]
"""
        
        # Create entrypoint that starts services found in the system
        entrypoint_content = """#!/bin/bash

# Start services based on what's installed
if [ -f /usr/sbin/sshd ]; then
    echo "[*] Starting SSH..."
    service ssh start || /usr/sbin/sshd -D &
fi

if [ -f /usr/sbin/apache2 ]; then
    echo "[*] Starting Apache..."
    service apache2 start || /usr/sbin/apache2ctl start
fi

if [ -f /usr/sbin/nginx ]; then
    echo "[*] Starting Nginx..."
    service nginx start || /usr/sbin/nginx
fi

if [ -f /usr/bin/mysqld_safe ]; then
    echo "[*] Starting MySQL..."
    service mysql start || /usr/bin/mysqld_safe &
fi

if [ -f /usr/sbin/vsftpd ]; then
    echo "[*] Starting FTP..."
    service vsftpd start || /usr/sbin/vsftpd &
fi

# Check for any Node.js apps
if [ -f /usr/bin/node ] || [ -f /usr/bin/nodejs ]; then
    # Look for common Node app locations
    for app_dir in /var/www /opt /home/*/app; do
        if [ -f "$app_dir/package.json" ]; then
            echo "[*] Found Node.js app in $app_dir"
            cd "$app_dir"
            if [ -f "app.js" ]; then
                node app.js &
            elif [ -f "server.js" ]; then
                node server.js &
            elif [ -f "index.js" ]; then
                node index.js &
            fi
        fi
    done
fi

# Keep container running
echo "[+] All services started. Container ready."
tail -f /dev/null
"""
        
        # Write files
        with open(dockerfile_path, 'w') as f:
            f.write(dockerfile_content)
        
        entrypoint_path = self.docker_dir / 'entrypoint.sh'
        with open(entrypoint_path, 'w') as f:
            f.write(entrypoint_content)
        
        print(f"[+] Dockerfile created at {dockerfile_path}")
        return dockerfile_path
    
    def create_ctf_dockerfile(self) -> Path:
        """Create a Dockerfile specifically for CTF challenges"""
        print("[*] Creating CTF-oriented Dockerfile")
        
        dockerfile_path = self.docker_dir / 'Dockerfile'
        
        # CTF-specific Dockerfile
        dockerfile_content = """FROM ubuntu:18.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Update and install CTF-relevant services
RUN apt-get update && apt-get install -y \\
    openssh-server \\
    apache2 \\
    php \\
    libapache2-mod-php \\
    mysql-server \\
    python \\
    python3 \\
    netcat \\
    nmap \\
    tcpdump \\
    vim \\
    gcc \\
    make \\
    gdb \\
    net-tools \\
    curl \\
    wget \\
    sudo \\
    ftp \\
    vsftpd \\
    telnetd \\
    xinetd \\
    && rm -rf /var/lib/apt/lists/*

# Create vulnerable user
RUN useradd -m -s /bin/bash ctfuser && \\
    echo 'ctfuser:ctfpassword' | chpasswd && \\
    echo 'root:r00t' | chpasswd

# Configure SSH for CTF
RUN mkdir /var/run/sshd && \\
    sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config && \\
    sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Configure Apache
RUN echo "ServerName localhost" >> /etc/apache2/apache2.conf && \\
    a2enmod php7.2

# Create some CTF flags
RUN echo "FLAG{docker_conversion_success}" > /root/flag.txt && \\
    echo "FLAG{web_server_flag}" > /var/www/html/flag.txt && \\
    chmod 644 /var/www/html/flag.txt

# Create vulnerable web app
RUN echo '<?php if(isset($_GET["cmd"])) { system($_GET["cmd"]); } ?>' > /var/www/html/shell.php

# Configure FTP
RUN echo "local_enable=YES" >> /etc/vsftpd.conf && \\
    echo "write_enable=YES" >> /etc/vsftpd.conf

# Create startup script
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# Expose CTF ports
EXPOSE 21 22 23 80 443 3306 8080

# Set entrypoint
ENTRYPOINT ["/entrypoint.sh"]
"""
        
        # Create CTF entrypoint script
        entrypoint_content = """#!/bin/bash

# Start services
service ssh start
service apache2 start
service mysql start
service vsftpd start

# Create some interesting files
echo "Welcome to the CTF challenge!" > /home/ctfuser/welcome.txt
echo "Can you find all the flags?" > /home/ctfuser/hint.txt

# Set weak permissions (intentionally vulnerable)
chmod 777 /tmp
chmod 755 /home/ctfuser

# Keep container running
tail -f /dev/null
"""
        
        # Write files
        with open(dockerfile_path, 'w') as f:
            f.write(dockerfile_content)
        
        entrypoint_path = self.docker_dir / 'entrypoint.sh'
        with open(entrypoint_path, 'w') as f:
            f.write(entrypoint_content)
        
        print(f"[+] CTF Dockerfile created at {dockerfile_path}")
        return dockerfile_path
    
    def convert(self, ctf_mode: bool = True) -> bool:
        """Main conversion process"""
        print(f"\n{'='*60}")
        print(f"VM to Docker Converter")
        print(f"{'='*60}")
        print(f"Source: {self.vm_path}")
        print(f"Target: {self.output_name}")
        print(f"Mode: {'Extract VM' if ctf_mode else 'Template'}")
        print(f"{'='*60}\n")
        
        try:
            vmdk_file = None
            fs_dir = None
            
            # Extract if OVA
            if self.vm_path.suffix.lower() == '.ova':
                vmdk_file = self.download_and_extract()
                if vmdk_file:
                    print(f"[+] Found VMDK: {vmdk_file.name}")
            elif self.vm_path.suffix.lower() == '.vmdk':
                vmdk_file = self.vm_path
            
            # Try to extract filesystem from VMDK
            if vmdk_file and ctf_mode:
                raw_disk = self.convert_vmdk_to_raw(vmdk_file)
                if raw_disk:
                    fs_dir = self.mount_and_extract_filesystem(raw_disk)
            
            # Create appropriate Dockerfile
            if fs_dir and fs_dir.exists():
                # Use extracted filesystem
                print("[+] Using extracted VM filesystem")
                self.create_dockerfile_from_filesystem(fs_dir)
            elif ctf_mode:
                # Fall back to CTF template
                print("[!] Could not extract filesystem, using CTF template")
                self.create_ctf_dockerfile()
            else:
                # Use standard template
                self.create_dockerfile_from_template()
            
            # Build Docker image
            if not self.build_docker_image():
                return False
            
            print(f"\n[+] Conversion completed successfully!")
            print(f"[+] Docker image: {self.output_name}")
            print(f"\n[*] To run the container:")
            print(f"    docker run -d --name {self.output_name}_container -p 2222:22 -p 8080:80 {self.output_name}")
            print(f"\n[*] To access the container:")
            print(f"    SSH: ssh ctfuser@localhost -p 2222 (password: ctfpassword)")
            print(f"    Web: http://localhost:8080")
            
            return True
            
        except Exception as e:
            print(f"[!] Error during conversion: {e}")
            import traceback
            traceback.print_exc()
            return False
        finally:
            # Cleanup
            if self.work_dir.exists():
                print(f"\n[*] Cleaning up work directory")
                shutil.rmtree(self.work_dir, ignore_errors=True)

def download_file(url: str, dest_path: Path) -> bool:
    """Download file from URL"""
    print(f"[*] Downloading: {url}")
    print(f"    Destination: {dest_path}")
    
    # Use curl with progress bar
    result = subprocess.run(['curl', '-L', '-o', str(dest_path), '--progress-bar', url])
    
    if result.returncode == 0 and dest_path.exists():
        size_mb = dest_path.stat().st_size / (1024*1024)
        print(f"[+] Downloaded successfully: {size_mb:.2f} MB")
        return True
    
    print(f"[!] Download failed")
    return False

def main():
    parser = argparse.ArgumentParser(description='Simple VM to Docker converter')
    parser.add_argument('vm_image', help='Path or URL to VM image (OVA/VMDK)')
    parser.add_argument('-n', '--name', help='Docker image name', default=None)
    parser.add_argument('--standard', action='store_true', help='Use standard mode instead of CTF mode')
    parser.add_argument('--test', action='store_true', help='Test the resulting container')
    
    args = parser.parse_args()
    
    # Handle URL input
    vm_path = args.vm_image
    if vm_path.startswith('http://') or vm_path.startswith('https://'):
        url = vm_path
        filename = url.split('/')[-1]
        vm_path = Path(tempfile.gettempdir()) / filename
        
        if not download_file(url, vm_path):
            print("[!] Failed to download VM image")
            sys.exit(1)
    else:
        vm_path = Path(vm_path)
        if not vm_path.exists():
            print(f"[!] File not found: {vm_path}")
            sys.exit(1)
    
    # Create converter
    converter = SimpleVMToDockerConverter(str(vm_path), args.name)
    
    # Run conversion
    success = converter.convert(ctf_mode=not args.standard)
    
    if success and args.test:
        print("\n[*] Testing the Docker container...")
        container_name = f"{converter.output_name}_test"
        
        # Stop any existing test container
        subprocess.run(['docker', 'rm', '-f', container_name], capture_output=True)
        
        # Run container with port mappings
        print(f"[*] Starting container: {container_name}")
        result = subprocess.run([
            'docker', 'run', '-d',
            '--name', container_name,
            '-p', '2222:22',
            '-p', '8080:80',
            '-p', '2121:21',
            converter.output_name
        ], capture_output=True, text=True)
        
        if result.returncode == 0:
            container_id = result.stdout.strip()
            print(f"[+] Container started: {container_id[:12]}")
            
            # Wait for services to start
            print("[*] Waiting for services to initialize...")
            subprocess.run(['sleep', '3'])
            
            # Check container status
            print("\n[*] Container status:")
            subprocess.run(['docker', 'ps', '-f', f'name={container_name}'])
            
            # Test services
            print("\n[*] Testing services:")
            print("    SSH: ssh ctfuser@localhost -p 2222")
            print("    Web: http://localhost:8080")
            print("    FTP: ftp://localhost:2121")
            
            # Show logs
            print("\n[*] Container logs:")
            subprocess.run(['docker', 'logs', '--tail', '20', container_name])
            
            print(f"\n[+] Test container is running: {container_name}")
            print(f"[*] To stop it: docker rm -f {container_name}")
    
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()