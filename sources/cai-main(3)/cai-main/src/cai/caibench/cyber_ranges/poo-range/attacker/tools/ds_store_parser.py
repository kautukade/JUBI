#!/usr/bin/env python3
"""
DS_Store Parser - Extract directory structure from .DS_Store files
Similar to https://github.com/Keramas/DS_Walk
"""

import requests
import sys
import re

def fetch_ds_store(url):
    """Fetch and parse .DS_Store file"""
    try:
        ds_url = f"{url.rstrip('/')}/.DS_Store"
        response = requests.get(ds_url, timeout=10)
        if response.status_code == 200:
            return response.text
        return None
    except Exception as e:
        print(f"Error fetching .DS_Store: {e}")
        return None

def parse_ds_store(content):
    """Extract entries from DS_Store content"""
    # Simple extraction of readable strings
    entries = []
    # Find potential directory/file names (alphanumeric + some special chars)
    matches = re.findall(r'[a-zA-Z0-9_\-\.]{3,}', content)
    for match in matches:
        if match not in ['Bud1', 'bplist', 'META', 'INFO']:
            entries.append(match)
    return list(set(entries))

def enumerate_directories(base_url, entries):
    """Check which directories exist"""
    print(f"\n[+] Enumerating directories based on .DS_Store file:")
    for entry in entries:
        url = f"{base_url.rstrip('/')}/{entry}"
        try:
            response = requests.get(url, timeout=5)
            if response.status_code != 404:
                print(f"[!] {url} - Status: {response.status_code}")
        except:
            pass

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <target_url>")
        print(f"Example: {sys.argv[0]} http://192.168.3.11")
        sys.exit(1)

    target = sys.argv[1]
    print(f"[*] Fetching .DS_Store from {target}")

    content = fetch_ds_store(target)
    if content:
        print("[+] .DS_Store file found!")
        entries = parse_ds_store(content)
        print(f"[+] Found entries: {entries}")
        enumerate_directories(target, entries)
    else:
        print("[-] .DS_Store file not found")

if __name__ == "__main__":
    main()
