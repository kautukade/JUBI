#!/usr/bin/env python3

import json
import random
import string
import subprocess
import time
import re
from base_checker import BaseChecker, CheckerStatus

class DocuflowChecker(BaseChecker):
    """Checker for the DocuFlow CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 8000
        self.ssh_port = 2222
        # Match gameserver container naming convention
        self.container_name = f"docuflow_team_{team_id}"

        # Known credentials from the service
        self.demo_user = "demo"
        self.demo_pass = "demo"
        self.developer_user = "developer"
        self.developer_pass = "DevP@ssw0rd!"

        # Generate test data
        self.test_document = f"TestDoc_{team_id}_{random.randint(1000, 9999)}"

    def check_service_availability(self) -> CheckerStatus:
        """Check if DocuFlow web service is available"""

        # Check web port via devcontainer
        try:
            devcontainer_cmd = f"timeout 2 nc -zv {self.host} {self.web_port}"
            result = subprocess.run(
                devcontainer_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode != 0:
                self.logger.error(f"Web port {self.web_port} is not accessible")
                return CheckerStatus.DOWN

            self.logger.info(f"Web service on port {self.web_port} is open")
        except Exception as e:
            self.logger.error(f"Failed to check web port: {e}")
            return CheckerStatus.DOWN

        # Check SSH port via devcontainer
        try:
            devcontainer_cmd = f"timeout 2 nc -zv {self.host} {self.ssh_port}"
            result = subprocess.run(
                devcontainer_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode != 0:
                self.logger.warning(f"SSH port {self.ssh_port} is not accessible")
                # SSH not being accessible is not critical for basic service
            else:
                self.logger.info(f"SSH service on port {self.ssh_port} is open")
        except Exception as e:
            self.logger.warning(f"Failed to check SSH port: {e}")

        # Check web service response via devcontainer
        try:
            check_cmd = f"curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.web_port}/ --connect-timeout 5"
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0 and result.stdout.strip() == "200":
                self.logger.info("Web service is responding")
            else:
                self.logger.error(f"Web service returned status {result.stdout.strip()}")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"Web service check failed: {e}")
            return CheckerStatus.DOWN

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Check if DocuFlow is functioning correctly"""

        # Test 1: Check homepage content
        try:
            homepage_cmd = f"curl -s http://{self.host}:{self.web_port}/ --connect-timeout 5"
            result = subprocess.run(
                homepage_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "DocuFlow" in result.stdout:
                    self.logger.info("Homepage content is correct")
                else:
                    self.logger.warning("Homepage missing expected content")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.error("Failed to fetch homepage")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Homepage test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Check login functionality
        try:
            login_data = json.dumps({"username": self.demo_user, "password": self.demo_pass})
            # Use URL encoding for form data
            login_cmd = f"""curl -s -X POST \\
                -H "Content-Type: application/x-www-form-urlencoded" \\
                -d "username={self.demo_user}&password={self.demo_pass}" \\
                -c /tmp/cookies_{self.team_id}.txt \\
                -L http://{self.host}:{self.web_port}/login --connect-timeout 5"""

            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                # Check if we got redirected to dashboard
                if "Dashboard" in result.stdout or "Welcome back" in result.stdout:
                    self.logger.info("Login functionality is working")
                else:
                    self.logger.warning("Login may not be working correctly")
                    # Not critical, continue checking
            else:
                self.logger.warning("Login test failed")

        except Exception as e:
            self.logger.warning(f"Login test failed: {e}")

        # Test 3: Check preview endpoint (the SSTI vulnerable endpoint)
        try:
            test_text = "<h1>Test</h1>"
            preview_cmd = f"""curl -s -G \\
                --data-urlencode "text={test_text}" \\
                http://{self.host}:{self.web_port}/preview --connect-timeout 5"""

            result = subprocess.run(
                preview_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "Preview Tool" in result.stdout:
                    self.logger.info("Preview endpoint is functional")
                else:
                    self.logger.warning("Preview endpoint returned unexpected content")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.error("Preview endpoint failed")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Preview test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 4: Check health endpoint
        try:
            health_cmd = f"curl -s http://{self.host}:{self.web_port}/health --connect-timeout 5"
            result = subprocess.run(
                health_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    health_data = json.loads(result.stdout)
                    if health_data.get('status') == 'healthy':
                        self.logger.info("Health endpoint confirms service is healthy")
                    else:
                        self.logger.warning("Health endpoint reports unhealthy status")
                except:
                    self.logger.warning("Health endpoint returned invalid JSON")
            else:
                self.logger.warning("Health endpoint check failed")

        except Exception as e:
            self.logger.warning(f"Health check failed: {e}")

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are present and retrievable"""
        # This will be checked via get_flag methods
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # User flag goes in developer's home
        commands = [
            "mkdir -p /home/developer",
            "touch /home/developer/user.txt",
            f"echo '{flag}' > /home/developer/user.txt",
            "chmod 600 /home/developer/user.txt",
            "chown developer:developer /home/developer/user.txt"
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
            "touch /root/root.txt",
            f"echo '{flag}' > /root/root.txt",
            "chmod 600 /root/root.txt"
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
            "cat /home/developer/user.txt 2>/dev/null || echo ''"
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
                    "mkdir -p /home/developer",
                    f"echo '{flags['user_flag']}' > /home/developer/user.txt",
                    "chown developer:developer /home/developer/user.txt",
                    "chmod 600 /home/developer/user.txt"
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
                    "chmod 600 /root/root.txt"
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

    def check_vulnerability_exploitability(self) -> bool:
        """
        Optional: Test if the SSTI vulnerability is still exploitable
        This doesn't fail the check but logs a warning if patched
        """
        try:
            # Test simple math expression
            ssti_test = "{{7*7}}"
            preview_cmd = f"""curl -s -G \\
                --data-urlencode "text={ssti_test}" \\
                http://{self.host}:{self.web_port}/preview --connect-timeout 5"""

            result = subprocess.run(
                preview_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "49" in result.stdout:
                    self.logger.info("SSTI vulnerability is present and exploitable")
                    return True
                else:
                    self.logger.warning("SSTI vulnerability may be patched")
                    return False
            else:
                self.logger.warning("Could not test SSTI vulnerability")
                return False

        except Exception as e:
            self.logger.warning(f"SSTI vulnerability test failed: {e}")
            return False

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='DocuFlow CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = DocuflowChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)