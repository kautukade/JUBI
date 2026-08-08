"""
Example Script: Running a CTF Environment

This script demonstrates how to start and interact with a Capture The Flag (CTF)
environment using the `CTF` class from `cai.caibench.ctf`.

Steps performed:
1. Load the desired CTF configuration from `ctf_configs.jsonl` by name.
2. Instantiate a `CTF` object with the selected configuration.
3. Start the CTF environment (e.g., container).
4. List available challenges.
5. Execute shell commands inside the container.
6. Retrieve the container's IP address.
7. Stop and clean up the environment when finished.

Usage:
    python3 src/cai/caibench/ctf_example.py <ctf_name>

Example:
    python3 src/cai/caibench/ctf_example.py picoctf_static_flag

"""
import sys
from cai.caibench.ctf import CTF
import json
import os

# Function to load a CTF configuration from ctf_configs.jsonl by its name
def load_ctf_config_by_name(ctf_name):
    config_path = os.path.join(os.path.dirname(__file__), 'ctf-jsons', 'ctf_configs.jsonl')
    with open(config_path, 'r') as f:
        configs = json.load(f)
    for config in configs:
        if config.get("name", "").lower() == ctf_name.lower():
            return config
    raise ValueError(f"CTF config with name '{ctf_name}' not found.")

if __name__ == "__main__":
    # Check if the user provided a CTF name as a command-line argument
    if len(sys.argv) < 2:
        print("Usage: python your_script.py <ctf_name>")
        sys.exit(1)
    ctf_name = sys.argv[1]

    # Load the configuration for the specified CTF
    ctf_config = load_ctf_config_by_name(ctf_name)

    # Create a CTF instance using the loaded config
    ctf = CTF(ctf_config)

    # Start the CTF environment 
    ctf.start_ctf()

    # Print the available challenges for this CTF
    print("Challenges:", ctf.get_challenges())

    # Run a command inside the container (example: list files)
    print("Listing files in container:", ctf.get_shell("ls"))

    # Print the container’s IP address
    print("IP Address:", ctf.get_ip())

    # Stop and clean up the CTF environment when finished
    ctf.stop_ctf()