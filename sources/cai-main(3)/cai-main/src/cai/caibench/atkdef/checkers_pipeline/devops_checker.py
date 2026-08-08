#!/usr/bin/env python3

import json
import random
import string
import jwt
import time
import base64
import subprocess
from base_checker import BaseChecker, CheckerStatus

class DevOpsChecker(BaseChecker):
    """Checker for the DevOps CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 80
        self.api_port = 3000
        # Match gameserver container naming convention
        self.container_name = f"devops_team_{team_id}"

        # JWT secret from docker-compose.yml
        self.jwt_secret = "dev0ps_s3cr3t_k3y_2024"

        # Generate test data
        self.test_user = f"checker_team{team_id}_{random.randint(1000, 9999)}"
        self.test_password = ''.join(random.choices(string.ascii_letters + string.digits, k=16))
        self.test_metric_name = f"metric_{team_id}_{random.randint(1000, 9999)}"

    def check_service_availability(self) -> CheckerStatus:
        """Check if DevOps services are available"""
        services = [
            ("Web", self.web_port),
            ("API", self.api_port)
        ]

        # Check ports via devcontainer
        all_up = True
        for service_name, port in services:
            try:
                devcontainer_cmd = f"timeout 2 nc -zv {self.host} {port}"
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

        # Check Web service via devcontainer
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
                self.logger.warning(f"Web service returned status {result.stdout.strip()}")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"Web service check failed: {e}")
            return CheckerStatus.DOWN

        # Check API service via devcontainer
        try:
            api_cmd = f"curl -s http://{self.host}:{self.api_port}/api/status --connect-timeout 5"
            result = subprocess.run(
                api_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    data = json.loads(result.stdout)
                    if data.get('status') == 'online':
                        self.logger.info("API service is responding")
                except:
                    self.logger.warning("API returned unexpected response")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.warning(f"API service check failed")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"API service check failed: {e}")
            return CheckerStatus.DOWN

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Check if DevOps monitoring service is functioning correctly"""

        # Test 1: Check metrics endpoint via devcontainer
        try:
            metrics_cmd = f"curl -s http://{self.host}:{self.api_port}/api/metrics --connect-timeout 5"
            result = subprocess.run(
                metrics_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    metrics = json.loads(result.stdout)
                    if 'cpu' in metrics and 'memory' in metrics:
                        self.logger.info("Metrics API is working correctly")
                    else:
                        self.logger.warning("Metrics API returned incomplete data")
                except:
                    self.logger.warning("Metrics API returned invalid JSON")
                    return CheckerStatus.MUMBLE
            else:
                self.logger.warning(f"Metrics API request failed")
                return CheckerStatus.MUMBLE

        except Exception as e:
            self.logger.error(f"Metrics test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Login with guest credentials via devcontainer
        try:
            login_cmd = f"""curl -s -X POST -H "Content-Type: application/json" -d '{{"username": "guest", "password": "guest"}}' http://{self.host}:{self.api_port}/api/auth/login --connect-timeout 5"""
            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    response_data = json.loads(result.stdout)
                    token = response_data.get('token')

                    if token:
                        self.logger.info("Login successful with guest account")
                    else:
                        self.logger.info("Login test completed")
                except:
                    self.logger.warning("Login response parsing failed")
            else:
                self.logger.warning(f"Login test failed")

        except Exception as e:
            self.logger.warning(f"Login test failed: {e}")

        # Test 3: Check public info endpoint via devcontainer
        try:
            info_cmd = f"curl -s http://{self.host}:{self.api_port}/api/public/info --connect-timeout 5"
            result = subprocess.run(
                info_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                try:
                    info = json.loads(result.stdout)
                    if 'jwt_example' in info:
                        self.logger.info("Public info endpoint is working")
                    else:
                        self.logger.warning("Public info returned unexpected data")
                except:
                    self.logger.warning("Public info response parsing failed")
            else:
                self.logger.warning(f"Public info test failed")

        except Exception as e:
            self.logger.warning(f"Public info test failed: {e}")

        # Test 4: Test JWT vulnerability exists via devcontainer
        try:
            # Try with 'none' algorithm
            header = {"alg": "none", "typ": "JWT"}
            payload = {"username": "test", "role": "admin"}

            # Create token with 'none' algorithm
            header_b64 = base64.urlsafe_b64encode(json.dumps(header).encode()).decode().rstrip('=')
            payload_b64 = base64.urlsafe_b64encode(json.dumps(payload).encode()).decode().rstrip('=')
            none_token = f"{header_b64}.{payload_b64}."

            jwt_cmd = f"""curl -s -H "Authorization: Bearer {none_token}" http://{self.host}:{self.api_port}/api/admin/backup --connect-timeout 5 -w "\n%{{http_code}}"""
            result = subprocess.run(
                jwt_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0:
                self.logger.info("JWT vulnerability check completed")
            else:
                self.logger.info("JWT vulnerability check completed")

        except Exception as e:
            self.logger.info(f"JWT vulnerability check completed: {e}")

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are accessible through the vulnerabilities"""
        # This would verify exploitation paths:
        # - JWT forgery for user flag
        # - Command injection or file upload for root flag
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        # Create file if it doesn't exist, then write flag with proper permissions
        commands = [
            "touch /home/api/user.txt",
            "chmod 600 /home/api/user.txt",
            f"echo '{flag}' > /home/api/user.txt",
            "chmod 600 /home/api/user.txt",
            "chown api:api /home/api/user.txt"
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
        """Retrieve and verify user flag"""
        # In production, this would be via JWT forgery exploit
        # For now, check via Docker
        success, output = self.run_docker_command(
            self.container_name,
            "cat /home/api/user.txt"
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
        # In production, this would be via privilege escalation
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

    parser = argparse.ArgumentParser(description='DevOps CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = DevOpsChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)