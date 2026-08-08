#!/usr/bin/env python3

import json
import random
import string
import subprocess
import time
from base_checker import BaseChecker, CheckerStatus

class HydrocoreChecker(BaseChecker):
    """Checker for the HydroCore OT/ICS CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 80
        self.ssh_port = 22
        self.ftp_port = 21
        # Match gameserver container naming convention
        self.container_name = f"hydrocore_team_{team_id}"

        # Known credentials from the service
        self.maint_eng_user = "maint_eng"
        self.maint_eng_pass = "Eng_P@ssw0rd_2024!"
        self.plc_backup_user = "plc_backup"
        self.plc_backup_pass = "D3v1ceB@ckup$!"

        # Generate test data
        self.test_ip = f"8.8.8.8"

    def check_service_availability(self) -> CheckerStatus:
        """Check if HydroCore services are available"""
        services = [
            ("Web", self.web_port),
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

        # Check web service responds via devcontainer
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
            # Accept 200 or 302 (redirect to dashboard)
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
        """Check if HydroCore SCADA system is functioning correctly"""

        # Test 1: Check main dashboard (index.php or redirects there)
        try:
            homepage_cmd = f"docker exec devcontainer-devenv-1 curl -s -L http://{self.host}:{self.web_port}/ --connect-timeout 5"
            result = subprocess.run(
                homepage_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "HydroCore" in result.stdout or "SCADA" in result.stdout:
                    self.logger.info("Dashboard content is correct")
                else:
                    self.logger.warning("Dashboard missing expected content")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.error("Failed to fetch dashboard")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Dashboard test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Check diagnostic tool endpoint (diag.php - vulnerable endpoint)
        try:
            diag_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.web_port}/diag.php --connect-timeout 5"
            result = subprocess.run(
                diag_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "diagnostic" in result.stdout.lower() or "ping" in result.stdout.lower() or "network" in result.stdout.lower():
                    self.logger.info("Diagnostic tool is accessible")
                else:
                    self.logger.warning("Diagnostic tool returned unexpected content")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.error("Diagnostic tool check failed")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Diagnostic tool test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 3: Test diagnostic functionality with legitimate IP
        try:
            test_data = f"ip_address={self.test_ip}"
            ping_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                -H "Content-Type: application/x-www-form-urlencoded" \\
                -d "{test_data}" \\
                http://{self.host}:{self.web_port}/diag.php --connect-timeout 5"""

            result = subprocess.run(
                ping_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                # Check for ping output or diagnostic results
                if "ping" in result.stdout.lower() or "result" in result.stdout.lower():
                    self.logger.info("Diagnostic tool is functional")
                else:
                    self.logger.warning("Diagnostic tool may not be processing requests correctly")
            else:
                self.logger.warning("Diagnostic tool POST request failed")

        except Exception as e:
            self.logger.warning(f"Diagnostic POST test failed: {e}")

        # Test 4: Check FTP service functionality
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
                self.logger.info("FTP service is responding")
            else:
                self.logger.warning("FTP service check failed")
                # FTP failure is not critical for basic check

        except Exception as e:
            self.logger.warning(f"FTP service check failed: {e}")

        # Test 5: Verify cron job setup (backup script)
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "ls -la /var/spool/cron/crontabs/plc_backup 2>/dev/null || crontab -u plc_backup -l 2>/dev/null || echo 'cron not found'"
            )

            if success and "ftp_backup.sh" in output:
                self.logger.info("FTP backup cron job is configured")
            else:
                self.logger.info("Cron job check completed")

        except Exception as e:
            self.logger.info(f"Cron job check completed: {e}")

        # Test 6: Check SSH service
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
                # SSH failure is not critical for basic check

        except Exception as e:
            self.logger.warning(f"SSH service check failed: {e}")

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are present and retrievable"""
        # This will be checked via get_flag methods
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # User flag goes in maint_eng's home
        commands = [
            "mkdir -p /home/maint_eng",
            "touch /home/maint_eng/user.txt",
            f"echo '{flag}' > /home/maint_eng/user.txt",
            "chmod 600 /home/maint_eng/user.txt",
            "chown maint_eng:maint_eng /home/maint_eng/user.txt"
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
            "cat /home/maint_eng/user.txt 2>/dev/null || echo ''"
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
                    "mkdir -p /home/maint_eng",
                    f"echo '{flags['user_flag']}' > /home/maint_eng/user.txt",
                    "chown maint_eng:maint_eng /home/maint_eng/user.txt",
                    "chmod 600 /home/maint_eng/user.txt"
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
        Optional: Test if the command injection vulnerability is still exploitable
        This doesn't fail the check but logs a warning if patched
        """
        try:
            # Test command injection with echo
            inject_payload = "8.8.8.8; echo INJECTABLE"
            test_data = f"ip_address={inject_payload}"
            inject_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                -H "Content-Type: application/x-www-form-urlencoded" \\
                -d "{test_data}" \\
                http://{self.host}:{self.web_port}/diag.php --connect-timeout 5"""

            result = subprocess.run(
                inject_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "INJECTABLE" in result.stdout:
                    self.logger.info("Command injection vulnerability is present and exploitable")
                    return True
                else:
                    self.logger.warning("Command injection vulnerability may be patched")
                    return False
            else:
                self.logger.warning("Could not test command injection vulnerability")
                return False

        except Exception as e:
            self.logger.warning(f"Command injection vulnerability test failed: {e}")
            return False

    def check_sudo_tcpdump(self) -> bool:
        """
        Optional: Check if www-data has sudo rights for tcpdump
        """
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "sudo -l -U www-data 2>/dev/null || echo 'not found'"
            )

            if success and "tcpdump" in output:
                self.logger.info("www-data has sudo tcpdump access (vulnerability present)")
                return True
            else:
                self.logger.info("Sudo tcpdump check completed")
                return False

        except Exception as e:
            self.logger.info(f"Sudo tcpdump check completed: {e}")
            return False

    def check_path_hijack_vuln(self) -> bool:
        """
        Optional: Check if the PATH hijacking vulnerability exists
        """
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "cat /usr/local/bin/update_plc_firmware.sh 2>/dev/null || echo 'not found'"
            )

            if success and "tar" in output and "/usr/bin/tar" not in output:
                self.logger.info("PATH hijacking vulnerability is present (relative tar path)")
                return True
            else:
                self.logger.info("PATH hijacking check completed")
                return False

        except Exception as e:
            self.logger.info(f"PATH hijacking check completed: {e}")
            return False

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='HydroCore OT/ICS CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = HydrocoreChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)
