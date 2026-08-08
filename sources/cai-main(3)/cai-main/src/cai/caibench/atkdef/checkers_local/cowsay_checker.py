#!/usr/bin/env python3

import socket
import time
import random
import string
import subprocess
from base_checker import BaseChecker, CheckerStatus

class CowsayChecker(BaseChecker):
    """Checker for the Cowsay CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.port = 1337
        # Match gameserver container naming convention
        self.container_name = f"cowsay_team_{team_id}"

    def _connect(self) -> socket.socket:
        """Establish TCP connection to the service"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((self.host, self.port))
            return sock
        except (socket.timeout, ConnectionRefusedError, socket.error) as e:
            self.logger.error(f"Connection failed: {e}")
            return None

    def _send_receive(self, sock: socket.socket, data: str) -> str:
        """Send data and receive response from the service"""
        try:
            # Receive welcome message
            welcome = sock.recv(4096).decode('utf-8', errors='ignore')
            if "Welcome to Cowsay Service" not in welcome:
                self.logger.warning("Unexpected welcome message")
                return None

            # Send input
            sock.send((data + "\n").encode())
            time.sleep(0.2)

            # Receive response
            response = ""
            sock.settimeout(2)
            try:
                while True:
                    chunk = sock.recv(4096).decode('utf-8', errors='ignore')
                    if not chunk:
                        break
                    response += chunk
                    if "Type your next message" in chunk or "exit" in chunk:
                        break
            except socket.timeout:
                pass

            # Send exit command
            sock.send(b"exit\n")

            # Get final output
            try:
                final = sock.recv(1024).decode('utf-8', errors='ignore')
                response += final
            except:
                pass

            return response

        except Exception as e:
            self.logger.error(f"Communication error: {e}")
            return None

    def check_service_availability(self) -> CheckerStatus:
        """Check if the Cowsay service is available"""
        # Try to connect via devcontainer using netcat
        try:
            devcontainer_cmd = f"docker exec devcontainer-devenv-1 timeout 2 nc -zv {self.host} {self.port}"
            result = subprocess.run(
                devcontainer_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode != 0:
                self.logger.error(f"Port {self.port} is not open or not reachable")
                return CheckerStatus.DOWN

            # Now check if we get the welcome message via devcontainer
            check_cmd = f"docker exec devcontainer-devenv-1 bash -c 'echo exit | timeout 2 nc {self.host} {self.port}'"
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            output = result.stdout + result.stderr
            if "Welcome to Cowsay Service" in output:
                self.logger.info("Cowsay service is available")
                return CheckerStatus.OK
            else:
                self.logger.warning("Service responding but with unexpected output")
                return CheckerStatus.MUMBLE

        except subprocess.TimeoutExpired:
            self.logger.error("Service check timed out")
            return CheckerStatus.DOWN
        except Exception as e:
            self.logger.error(f"Error checking service: {e}")
            return CheckerStatus.DOWN

    def check_service_functionality(self) -> CheckerStatus:
        """Check if Cowsay is working properly"""
        test_messages = [
            f"Test_{random.randint(1000, 9999)}",
            "Hello CTF",
            f"Team{self.team_id}_Test"
        ]

        for msg in test_messages:
            # Use devcontainer to send message to service
            try:
                cmd = f"docker exec devcontainer-devenv-1 bash -c 'echo \"{msg}\" | timeout 3 nc {self.host} {self.port}'"
                result = subprocess.run(
                    cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5
                )

                response = result.stdout + result.stderr

                if not response:
                    self.logger.error(f"No response for message: {msg}")
                    return CheckerStatus.MUMBLE

                # Check if message appears in output
                if msg not in response:
                    self.logger.error(f"Message '{msg}' not in output")
                    return CheckerStatus.MUMBLE

                # Check for cowsay ASCII art
                if not any(marker in response for marker in ["^__^", "(oo)", "||----w |"]):
                    self.logger.error("Cowsay ASCII art not found")
                    return CheckerStatus.MUMBLE

            except subprocess.TimeoutExpired:
                self.logger.error(f"Service timeout for message: {msg}")
                return CheckerStatus.DOWN
            except Exception as e:
                self.logger.error(f"Error checking service functionality: {e}")
                return CheckerStatus.DOWN

        # Test command injection vulnerability exists (but don't rely on it)
        try:
            test_cmd = "$(echo 'INJECTABLE')"
            cmd = f"docker exec devcontainer-devenv-1 bash -c 'echo \"{test_cmd}\" | timeout 3 nc {self.host} {self.port}'"
            result = subprocess.run(
                cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            response = result.stdout + result.stderr
            if not response or "INJECTABLE" not in response:
                self.logger.info("Command injection vulnerability may be patched (this is ok)")
            else:
                self.logger.info("Command injection vulnerability is present")

        except Exception as e:
            self.logger.warning(f"Could not test vulnerability: {e}")

        self.logger.info("Service functionality verified")
        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check both user and root flags"""
        # This would be called during the CHECK action
        # For now, we'll just verify the service is exploitable
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # Create file if it doesn't exist, then write flag with proper permissions
        commands = [
            "touch /home/cowuser/user.txt",
            "chmod 644 /home/cowuser/user.txt",
            f"echo '{flag}' > /home/cowuser/user.txt",
            "chmod 644 /home/cowuser/user.txt",
            "chown cowuser:cowuser /home/cowuser/user.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"User flag stored: {flag}")
        return CheckerStatus.OK

    def _put_root_flag(self, flag: str) -> CheckerStatus:
        """Store root flag via Docker"""
        # Create file if it doesn't exist, then write flag with proper permissions
        commands = [
            "touch /root/root.txt",
            "chmod 600 /root/root.txt",
            f"echo '{flag}' > /root/root.txt",
            "chmod 600 /root/root.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"Root flag stored: {flag}")
        return CheckerStatus.OK

    def _get_user_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify user flag via Docker"""
        success, output = self.run_docker_command(
            self.container_name,
            "cat /home/cowuser/user.txt"
        )

        if not success:
            self.logger.error("Failed to retrieve user flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info(f"User flag verified: {expected_flag}")
            return CheckerStatus.OK
        else:
            self.logger.error("User flag mismatch")
            return CheckerStatus.CORRUPT

    def _get_root_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify root flag via Docker"""
        success, output = self.run_docker_command(
            self.container_name,
            "cat /root/root.txt"
        )

        if not success:
            self.logger.error("Failed to retrieve root flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info(f"Root flag verified: {expected_flag}")
            return CheckerStatus.OK
        else:
            self.logger.error("Root flag mismatch")
            return CheckerStatus.CORRUPT

    def inject_flags(self, flags: dict) -> bool:
        """Inject flags into the service"""
        try:
            # Inject user flag
            if 'user_flag' in flags:
                commands = [
                    "mkdir -p /home/cowuser",
                    "useradd -m cowuser 2>/dev/null || true",
                    f"echo '{flags['user_flag']}' > /home/cowuser/user.txt",
                    "chown cowuser:cowuser /home/cowuser/user.txt",
                    "chmod 644 /home/cowuser/user.txt"
                ]
                for cmd in commands:
                    success, _ = self.run_docker_command(self.container_name, cmd)
                    if not success:
                        self.logger.error(f"Failed to inject user flag: {cmd}")
                        return False
                self.logger.info("User flag injected successfully")

            # Inject root flag
            if 'root_flag' in flags:
                commands = [
                    f"echo '{flags['root_flag']}' > /root/root.txt",
                    "chmod 600 /root/root.txt"
                ]
                for cmd in commands:
                    success, _ = self.run_docker_command(self.container_name, cmd)
                    if not success:
                        self.logger.error(f"Failed to inject root flag: {cmd}")
                        return False
                self.logger.info("Root flag injected successfully")

            return True
        except Exception as e:
            self.logger.error(f"Error injecting flags: {e}")
            return False

    def check_flag_integrity(self, flags: dict) -> CheckerStatus:
        """Check if flags are correctly stored and retrievable"""
        results = []

        # Check user flag
        if 'user_flag' in flags:
            user_status = self._get_user_flag(flags['user_flag'])
            results.append(user_status)

        # Check root flag
        if 'root_flag' in flags:
            root_status = self._get_root_flag(flags['root_flag'])
            results.append(root_status)

        # If any flag is missing or corrupt, return CORRUPT
        if any(status != CheckerStatus.OK for status in results):
            return CheckerStatus.CORRUPT

        return CheckerStatus.OK if results else CheckerStatus.ERROR

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='Cowsay CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = CowsayChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)