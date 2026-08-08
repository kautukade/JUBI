import argparse
from . import ctf
import json

def start_ctf(args):
    # Auto-detect subnet from IP address
    ip_parts = args.ip_address.split('.')
    subnet = f"{ip_parts[0]}.{ip_parts[1]}.{ip_parts[2]}.0/24"
    ctf_instance = ctf(args.name, subnet=subnet, ip_address=args.ip_address)
    ctf_instance.start_ctf()
    print(f"Started CTF: {args.name}")
    print(f"IP Address: {ctf_instance.get_ip()}")
    print("Flag:")
    print(ctf_instance.get_flag())
  
    
    return ctf_instance

def stop_ctf(args):
    ctf_instance = ctf(args.name)
    ctf_instance.stop_ctf()
    print(f"Stopped CTF: {args.name}")

def get_shell(args):
    ctf_instance = ctf(args.name)
    result = ctf_instance.get_shell(args.command)
    print(f"Shell command result:")
    print(result)

def main():
    parser = argparse.ArgumentParser(description="CLI tool for managing CTFs")
    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Start CTF parser
    start_parser = subparsers.add_parser("start", help="Start a CTF")
    start_parser.add_argument("name", help="Name of the CTF")
    start_parser.add_argument("--ip-address", required=True, help="IP address for the CTF")

    # Stop CTF parser
    stop_parser = subparsers.add_parser("stop", help="Stop a CTF")
    stop_parser.add_argument("name", help="Name of the CTF to stop")

    # Get shell parser
    shell_parser = subparsers.add_parser("shell", help="Execute a shell command in a CTF")
    shell_parser.add_argument("name", help="Name of the CTF")
    shell_parser.add_argument("command", help="Shell command to execute")

    args = parser.parse_args()

    if args.command == "start":
        start_ctf(args)
    elif args.command == "stop":
        stop_ctf(args)
    elif args.command == "shell":
        get_shell(args)
    else:
        parser.print_help()

if __name__ == "__main__":
    main()

# Example usage:
# 1. HackableII
# cai-bench start hackableII --ip-address 192.168.2.11
# cai-bench shell hackableII "ls"
# cai-bench stop hackableII

# 2. Bob
# cai-bench start bob --ip-address 192.168.2.10
# cai-bench stop bob

# 3. KiddoCTF
# cai-bench start kiddoctf --ip-address 192.168.2.12
# cai-bench stop kiddoctf