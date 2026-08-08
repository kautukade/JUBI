#!/usr/bin/env python3

import json
import random
import string
import subprocess
import time
import re
from base_checker import BaseChecker, CheckerStatus

class SecurevaultChecker(BaseChecker):
    """Checker for the SecureVault CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 80
        self.ssh_port = 22
        # Match gameserver container naming convention
        self.container_name = f"securevault_team_{team_id}"

        # Known admin credentials
        self.admin_user = "admin"
        self.admin_pass = "admin2024"

        # Generate test data
        self.test_service = f"TestService_{team_id}_{random.randint(1000, 9999)}"

    def check_service_availability(self) -> CheckerStatus:
        """Check if SecureVault web service is available"""

        # Check web port via devcontainer
        try:
            devcontainer_cmd = f"docker exec devcontainer-devenv-1 timeout 2 nc -zv {self.host} {self.web_port}"
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
            devcontainer_cmd = f"docker exec devcontainer-devenv-1 timeout 2 nc -zv {self.host} {self.ssh_port}"
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
            check_cmd = f"docker exec devcontainer-devenv-1 curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.web_port}/ --connect-timeout 5"
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
        """Check if SecureVault is functioning correctly"""

        # Test 1: Check homepage content
        try:
            homepage_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.web_port}/ --connect-timeout 5"
            result = subprocess.run(
                homepage_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                if "SecureVault" in result.stdout:
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

        # Test 2: Check health endpoint
        try:
            health_cmd = f"docker exec devcontainer-devenv-1 curl -s http://{self.host}:{self.web_port}/api/health.php --connect-timeout 5"
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

        # Test 3: Check login functionality with admin credentials
        try:
            login_data = json.dumps({"username": self.admin_user, "password": self.admin_pass})
            login_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                -H "Content-Type: application/json" \\
                -d '{login_data}' \\
                http://{self.host}:{self.web_port}/api/login.php --connect-timeout 5"""

            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    response = json.loads(result.stdout)
                    if response.get('success') and response.get('token'):
                        self.logger.info("Login functionality is working")
                        # Store token for later use
                        self.admin_token = response.get('token')
                    else:
                        self.logger.warning("Login may not be working correctly")
                except:
                    self.logger.warning("Login returned invalid JSON")
            else:
                self.logger.warning("Login test failed")

        except Exception as e:
            self.logger.warning(f"Login test failed: {e}")

        # Test 4: Check vault API endpoint
        try:
            if hasattr(self, 'admin_token'):
                vault_data = json.dumps({"token": self.admin_token, "action": "list"})
                vault_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                    -H "Content-Type: application/json" \\
                    -d '{vault_data}' \\
                    http://{self.host}:{self.web_port}/api/vault.php --connect-timeout 5"""

                result = subprocess.run(
                    vault_cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=10
                )

                if result.returncode == 0:
                    try:
                        response = json.loads(result.stdout)
                        if response.get('success'):
                            self.logger.info("Vault API is functional")
                        else:
                            self.logger.warning("Vault API returned error")
                    except:
                        self.logger.warning("Vault API returned invalid JSON")
                else:
                    self.logger.warning("Vault API test failed")

        except Exception as e:
            self.logger.warning(f"Vault API test failed: {e}")

        # Test 5: Check admin backup endpoint (vulnerable endpoint)
        try:
            if hasattr(self, 'admin_token'):
                # Test with 'action' parameter (supports both action and backup_action)
                backup_data = json.dumps({"token": self.admin_token, "action": "status"})
                backup_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                    -H "Content-Type: application/json" \\
                    -d '{backup_data}' \\
                    http://{self.host}:{self.web_port}/api/admin/backup.php --connect-timeout 5"""

                result = subprocess.run(
                    backup_cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=10
                )

                if result.returncode == 0:
                    try:
                        response = json.loads(result.stdout)
                        if response.get('success'):
                            self.logger.info("Admin backup endpoint is functional")
                            # Verify it mentions file upload capability
                            if 'upload' in str(response.get('available_actions', {})):
                                self.logger.info("File upload functionality is available")
                        else:
                            self.logger.warning("Admin backup endpoint returned error")
                            return CheckerStatus.MUMBLE
                    except:
                        self.logger.warning("Admin backup endpoint returned invalid JSON")
                        return CheckerStatus.MUMBLE
                else:
                    self.logger.error("Admin backup endpoint failed")
                    return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Admin backup test failed: {e}")
            return CheckerStatus.MUMBLE

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are present and retrievable"""
        # This will be checked via get_flag methods
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # User flag goes in vaultuser's home
        # Must be readable by www-data group after webshell upload
        commands = [
            "mkdir -p /home/vaultuser",
            "chmod 755 /home/vaultuser",
            f"echo '{flag}' > /home/vaultuser/user.txt",
            "chmod 644 /home/vaultuser/user.txt",
            "chown vaultuser:www-data /home/vaultuser/user.txt"
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
            "cat /home/vaultuser/user.txt 2>/dev/null || echo ''"
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
                    "mkdir -p /home/vaultuser",
                    "chmod 755 /home/vaultuser",
                    f"echo '{flags['user_flag']}' > /home/vaultuser/user.txt",
                    "chown vaultuser:www-data /home/vaultuser/user.txt",
                    "chmod 644 /home/vaultuser/user.txt"
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
        Optional: Test if the SQL injection vulnerability is still exploitable
        This doesn't fail the check but logs a warning if patched
        """
        try:
            # Test SQL injection pattern (comment-based bypass: admin'--)
            sqli_payload = json.dumps({"username": "admin'--", "password": "anything"})
            sqli_cmd = f"""docker exec devcontainer-devenv-1 curl -s -X POST \\
                -H "Content-Type: application/json" \\
                -d '{sqli_payload}' \\
                http://{self.host}:{self.web_port}/api/login.php --connect-timeout 5"""

            result = subprocess.run(
                sqli_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    response = json.loads(result.stdout)
                    if response.get('success') and response.get('role') == 'admin':
                        self.logger.info("SQL injection vulnerability is present and exploitable (admin'--)")
                        return True
                    else:
                        self.logger.warning("SQL injection vulnerability may be patched")
                        return False
                except:
                    self.logger.warning("Could not parse SQL injection test response")
                    return False
            else:
                self.logger.warning("Could not test SQL injection vulnerability")
                return False

        except Exception as e:
            self.logger.warning(f"SQL injection vulnerability test failed: {e}")
            return False

    def check_docker_socket_exposure(self) -> bool:
        """
        Optional: Test if Docker socket is still exposed
        This doesn't fail the check but logs a warning if patched
        """
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "ls -la /var/run/docker.sock 2>/dev/null || echo 'not found'"
            )

            if success and "not found" not in output.lower():
                self.logger.info("Docker socket is exposed (vulnerability present)")
                return True
            else:
                self.logger.info("Docker socket not mounted - checking for SUID backup_reader")
                # Check for alternative SUID privilege escalation method
                success, suid_output = self.run_docker_command(
                    self.container_name,
                    "ls -la /usr/local/bin/backup_reader 2>/dev/null || echo 'not found'"
                )

                if success and "rws" in suid_output:
                    self.logger.info("SUID backup_reader found (alternative privesc method)")
                    return True
                else:
                    self.logger.warning("Neither docker socket nor SUID binary found (no privesc path)")
                    return False

        except Exception as e:
            self.logger.warning(f"Privilege escalation check failed: {e}")
            return False

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='SecureVault CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = SecurevaultChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)
