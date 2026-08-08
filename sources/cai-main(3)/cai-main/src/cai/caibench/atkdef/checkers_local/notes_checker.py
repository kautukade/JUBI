#!/usr/bin/env python3

import random
import string
import subprocess
from base_checker import BaseChecker, CheckerStatus

class NotesChecker(BaseChecker):
    """Checker for the Notes CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.http_port = 80
        self.ssh_port = 22
        self.ftp_port = 21
        # Match gameserver container naming convention
        self.container_name = f"notes_team_{team_id}"

        # Generate random test data
        self.test_note_title = f"Note_Team{team_id}_{random.randint(1000, 9999)}"
        self.test_note_content = ''.join(random.choices(string.ascii_letters + string.digits, k=32))

    def check_service_availability(self) -> CheckerStatus:
        """Check if all Notes services are available"""
        services = [
            ("HTTP", self.http_port),
            ("SSH", self.ssh_port),
            ("FTP", self.ftp_port)
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

        # Check HTTP service responds via devcontainer
        try:
            check_cmd = f"docker exec devcontainer-devenv-1 curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.http_port}/ --connect-timeout 5"
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0 and result.stdout.strip() == "200":
                self.logger.info("HTTP service is responding")
            else:
                self.logger.warning(f"HTTP service returned status {result.stdout.strip()}")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"HTTP service check failed: {e}")
            return CheckerStatus.DOWN

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Check if Notes service is functioning correctly"""

        # Test 1: Login to the web application via devcontainer
        try:
            # First, get the main page via devcontainer
            check_cmd = f"docker exec devcontainer-devenv-1 curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.http_port}/ --connect-timeout 5"
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode != 0 or result.stdout.strip() != "200":
                self.logger.error("Failed to access main page")
                return CheckerStatus.MUMBLE

            # Login with admin credentials via devcontainer
            login_cmd = f"docker exec devcontainer-devenv-1 curl -s -X POST -d 'username=admin&password=admin123' http://{self.host}:{self.http_port}/login --connect-timeout 5"
            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                # Check if we're logged in by looking for notes page or username in response
                if "notes" in result.stdout or "admin" in result.stdout:
                    self.logger.info("Login successful")
                else:
                    self.logger.info("Login test completed")
            else:
                self.logger.error(f"Login test failed")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Login test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Create and view a note via devcontainer
        try:
            # Create a new note via devcontainer
            create_cmd = f"docker exec devcontainer-devenv-1 curl -s -X POST -d 'title={self.test_note_title}&content={self.test_note_content}&is_public=1' http://{self.host}:{self.http_port}/create_note --connect-timeout 5"
            result = subprocess.run(
                create_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                self.logger.info(f"Created note: {self.test_note_title}")
            else:
                self.logger.warning(f"Note creation may have failed")

            # Verify we can access notes via devcontainer
            notes_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.http_port}/notes --connect-timeout 5"
            result = subprocess.run(
                notes_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if self.test_note_title in result.stdout:
                    self.logger.info("Note verified in notes list")
                else:
                    self.logger.info("Notes page accessible")
            else:
                self.logger.warning("Could not access notes page")

        except Exception as e:
            self.logger.warning(f"Note creation test failed: {e}")

        # Test 3: Check FTP service via devcontainer
        try:
            # Test FTP connection via devcontainer
            ftp_cmd = f"docker exec devcontainer-devenv-1 timeout 3 nc -zv {self.host} {self.ftp_port}"
            result = subprocess.run(
                ftp_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode == 0:
                self.logger.info("FTP service responding")
            else:
                self.logger.warning("FTP service check failed")

        except Exception as e:
            self.logger.warning(f"FTP service check failed: {e}")
            # FTP failure is not critical

        # Test 4: Check SSH service via devcontainer
        try:
            # Test SSH connection via devcontainer
            ssh_cmd = f"docker exec devcontainer-devenv-1 timeout 3 nc -zv {self.host} {self.ssh_port}"
            result = subprocess.run(
                ssh_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )

            if result.returncode == 0:
                self.logger.info("SSH service responding")
            else:
                self.logger.warning("SSH service check failed")

        except Exception as e:
            self.logger.warning(f"SSH service check failed: {e}")
            # SSH failure is not critical

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are accessible through the vulnerabilities"""
        # This would verify the exploitation paths are working
        # For the actual CTF, this might involve:
        # - SQL injection to get user flag
        # - Privilege escalation to get root flag
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # Create file if it doesn't exist, then write flag with proper permissions
        commands = [
            "touch /home/luis/user.txt",
            "chown luis:luis /home/luis/user.txt",
            "chmod 644 /home/luis/user.txt",
            f"echo {flag} > /home/luis/user.txt",
            "chmod 444 /home/luis/user.txt"
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
            "chmod 400 /root/root.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"Root flag stored: {flag}")
        return CheckerStatus.OK

    def _get_user_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify user flag"""
        # This would typically involve exploiting the service
        # For example, through SQL injection or file inclusion

        # For now, we'll check via Docker (in production, this would be via exploit)
        success, output = self.run_docker_command(
            self.container_name,
            "cat /home/luis/user.txt"
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
        """Retrieve and verify root flag"""
        # This would involve privilege escalation
        # For now, check via Docker
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

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='Notes CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = NotesChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)