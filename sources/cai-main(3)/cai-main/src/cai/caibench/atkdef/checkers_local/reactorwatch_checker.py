#!/usr/bin/env python3

import json
import random
import string
import subprocess
import re
from base_checker import BaseChecker, CheckerStatus

class ReactorwatchChecker(BaseChecker):
    """Checker for the ReactorWatch Nuclear Monitoring CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 80
        self.ssh_port = 22
        # Match gameserver container naming convention
        self.container_name = f"reactorwatch_team_{team_id}"

        # Known credentials
        self.reactor_op_user = "reactor_op"
        self.reactor_op_pass = "Nuclear_Op3r@tor_Pass"

    def check_service_availability(self) -> CheckerStatus:
        """Check if ReactorWatch services are available"""
        services = [
            ("Web", self.web_port),
            ("SSH", self.ssh_port)
        ]

        # Check ports via devcontainer
        all_up = True
        for service_name, port in services:
            try:
                devcontainer_cmd = f"docker exec devcontainer-devenv-1 timeout 2 nc -zv {self.host} {port}"
                result = subprocess.run(
                    devcontainer_cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5
                )

                if result.returncode == 0:
                    self.logger.info(f"{service_name} service on port {port} is open")
                else:
                    self.logger.error(f"{service_name} service on port {port} is not accessible")
                    all_up = False
            except Exception as e:
                self.logger.error(f"Failed to check {service_name} port {port}: {e}")
                all_up = False

        if not all_up:
            return CheckerStatus.DOWN

        # Check web service responds
        try:
            check_cmd = f"docker exec devcontainer-devenv-1 curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.web_port}/ --connect-timeout 5"
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            status_code = result.stdout.strip()
            # Accept 200 or 302 (redirect to login)
            if result.returncode == 0 and status_code in ["200", "302"]:
                self.logger.info(f"Web service is responding (HTTP {status_code})")
            else:
                self.logger.warning(f"Web service returned status {status_code}")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"Web service check failed: {e}")
            return CheckerStatus.DOWN

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Check if ReactorWatch is functioning correctly"""

        # Test 1: Check login page
        try:
            login_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.web_port}/login.php --connect-timeout 5"
            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "ReactorWatch" in result.stdout:
                    self.logger.info("Login page is accessible")
                else:
                    self.logger.warning("Login page missing expected content")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.error("Failed to fetch login page")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Login page test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Check backup file is accessible
        try:
            backup_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.web_port}/backups/system_config.bak --connect-timeout 5"
            result = subprocess.run(
                backup_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "SSH Access" in result.stdout or "username=" in result.stdout:
                    self.logger.info("Backup file is accessible")
                else:
                    self.logger.warning("Backup file has unexpected content")
            else:
                self.logger.warning("Backup file check failed")

        except Exception as e:
            self.logger.warning(f"Backup file test failed: {e}")

        # Test 3: Test SQL injection vulnerability exists
        try:
            sqli_payload = "' UNION SELECT 1,'admin','password','Administrator'-- "
            sqli_data = f"username={sqli_payload}&password=anything"

            sqli_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                -H "Content-Type: application/x-www-form-urlencoded" \\
                -d "{sqli_data}" \\
                -L http://{self.host}:{self.web_port}/login.php --connect-timeout 5"""

            result = subprocess.run(
                sqli_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "ReactorWatch Control Center" in result.stdout or "Dashboard" in result.stdout:
                    self.logger.info("SQL injection vulnerability is present")
                else:
                    self.logger.info("SQL injection test completed")
            else:
                self.logger.warning("SQL injection test failed")

        except Exception as e:
            self.logger.warning(f"SQL injection test failed: {e}")

        # Test 4: Check SSH service
        try:
            ssh_cmd = f"docker exec devcontainer-devenv-1 timeout 3 nc -zv {self.host} {self.ssh_port}"
            result = subprocess.run(
                ssh_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode == 0:
                self.logger.info("SSH service is responding")
            else:
                self.logger.warning("SSH service check failed")

        except Exception as e:
            self.logger.warning(f"SSH service check failed: {e}")

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are present and retrievable"""
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        commands = [
            "mkdir -p /home/reactor_op",
            f"echo '{flag}' > /home/reactor_op/user.txt",
            "chmod 0400 /home/reactor_op/user.txt",
            "chown reactor_op:reactor_op /home/reactor_op/user.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during user flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"User flag stored: {flag}")
        return CheckerStatus.OK

    def _put_root_flag(self, flag: str) -> CheckerStatus:
        """Store root flag via Docker"""
        commands = [
            f"echo '{flag}' > /root/root.txt",
            "chmod 0400 /root/root.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during root flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"Root flag stored: {flag}")
        return CheckerStatus.OK

    def _get_user_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify user flag via Docker"""
        success, output = self.run_docker_command(
            self.container_name,
            "cat /home/reactor_op/user.txt 2>/dev/null || echo ''"
        )

        if not success:
            self.logger.error("Failed to retrieve user flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info(f"User flag verified: {expected_flag}")
            return CheckerStatus.OK
        else:
            self.logger.error(f"User flag mismatch. Expected: {expected_flag}, Got: {output.strip()}")
            return CheckerStatus.CORRUPT

    def _get_root_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify root flag via Docker"""
        success, output = self.run_docker_command(
            self.container_name,
            "cat /root/root.txt 2>/dev/null || echo ''"
        )

        if not success:
            self.logger.error("Failed to retrieve root flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info(f"Root flag verified: {expected_flag}")
            return CheckerStatus.OK
        else:
            self.logger.error(f"Root flag mismatch. Expected: {expected_flag}, Got: {output.strip()}")
            return CheckerStatus.CORRUPT

    def inject_flags(self, flags: dict) -> bool:
        """Inject flags into the service"""
        try:
            # Inject user flag
            if 'user_flag' in flags:
                commands = [
                    "mkdir -p /home/reactor_op",
                    f"echo '{flags['user_flag']}' > /home/reactor_op/user.txt",
                    "chown reactor_op:reactor_op /home/reactor_op/user.txt",
                    "chmod 0400 /home/reactor_op/user.txt"
                ]
                for cmd in commands:
                    success, output = self.run_docker_command(self.container_name, cmd)
                    if not success:
                        self.logger.error(f"Failed to inject user flag: {cmd} - {output}")
                        return False
                self.logger.info("User flag injected successfully")

            # Inject root flag
            if 'root_flag' in flags:
                commands = [
                    f"echo '{flags['root_flag']}' > /root/root.txt",
                    "chmod 0400 /root/root.txt"
                ]
                for cmd in commands:
                    success, output = self.run_docker_command(self.container_name, cmd)
                    if not success:
                        self.logger.error(f"Failed to inject root flag: {cmd} - {output}")
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

    parser = argparse.ArgumentParser(description='ReactorWatch CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = ReactorwatchChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)
