#!/usr/bin/env python3

import random
import string
import subprocess
import hashlib
import base64
from base_checker import BaseChecker, CheckerStatus

class FortressChecker(BaseChecker):
    """Checker for the Fortress CTF service"""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 3000
        self.ssh_port = 22
        # Match gameserver container naming convention
        self.container_name = f"fortress_team_{team_id}"

        # Known credentials from the service
        self.root_user = "root"
        self.root_pass = "F0rtr3ss_R00t_2024!"
        self.diego_user = "diego"
        self.diego_pass = "d13go_F0rtr3ss_2024!"

        # Generate random test data
        self.test_user_id = f"user_{random.randint(1000, 9999)}"
        self.test_config = ''.join(random.choices(string.ascii_letters + string.digits, k=16))

    def check_service_availability(self) -> CheckerStatus:
        """Check if all Fortress services are available"""
        services = [
            ("Web", self.web_port),
            ("SSH", self.ssh_port)
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

        # Check web service responds via devcontainer
        try:
            check_cmd = f"curl -s -o /dev/null -w '%{{http_code}}' http://{self.host}:{self.web_port}/health --connect-timeout 5"
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

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Check if Fortress service is functioning correctly"""

        # Test 1: Check web health endpoint via devcontainer
        try:
            health_cmd = f"curl -s http://{self.host}:{self.web_port}/health --connect-timeout 5"
            result = subprocess.run(
                health_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0 and '"status":"ok"' in result.stdout:
                self.logger.info("Health check passed")
            else:
                self.logger.error("Health check failed")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"Health check failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 2: Check API config endpoint (prototype pollution vector)
        try:
            config_json = '{' + f'"userId":"{self.test_user_id}","config":{{"test":true}}' + '}'
            config_cmd = [
                'bash', '-c',
                f'curl -s -X POST http://{self.host}:{self.web_port}/api/config -H "Content-Type: application/json" -d \'{config_json}\' --connect-timeout 5'
            ]
            result = subprocess.run(
                config_cmd,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0 and '"success":true' in result.stdout:
                self.logger.info("API config endpoint responding")
                # Check for artifact in response
                if "artifact" in result.stdout:
                    self.logger.info("Artifact returned from config endpoint")
            else:
                self.logger.warning(f"API config endpoint not working correctly: {result.stdout}")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.error(f"API config test failed: {e}")
            return CheckerStatus.MUMBLE

        # Test 3: Check API render endpoint (template injection vector)
        try:
            render_json = '{"template":"<%= 2+2 %>","data":{}}'
            render_cmd = [
                'bash', '-c',
                f'curl -s -X POST http://{self.host}:{self.web_port}/api/render -H "Content-Type: application/json" -d \'{render_json}\' --connect-timeout 5'
            ]
            result = subprocess.run(
                render_cmd,
                capture_output=True,
                text=True,
                timeout=10
            )

            if result.returncode == 0 and '"rendered":"4"' in result.stdout:
                self.logger.info("API render endpoint functioning")
            else:
                self.logger.warning(f"API render endpoint not working correctly: {result.stdout}")
        except Exception as e:
            self.logger.warning(f"API render test failed: {e}")

        # Test 4: Check SSH service via devcontainer
        try:
            ssh_cmd = f"timeout 3 nc -zv {self.host} {self.ssh_port}"
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

        # Test 5: Verify password backup file exists
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "test -f /opt/backups/passwords.bak && echo 'exists'"
            )
            if success and "exists" in output:
                self.logger.info("Password backup file present")
            else:
                self.logger.warning("Password backup file not found")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.warning(f"Password backup check failed: {e}")

        # Test 6: Verify PAM module exists
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "test -f /lib/security/pam_fortress.so && echo 'exists'"
            )
            if success and "exists" in output:
                self.logger.info("PAM module present")
            else:
                self.logger.warning("PAM module not found")
                return CheckerStatus.MUMBLE
        except Exception as e:
            self.logger.warning(f"PAM module check failed: {e}")

        return CheckerStatus.OK

    def check_flags(self) -> CheckerStatus:
        """Check if flags are accessible through the vulnerabilities"""
        # Verify user flag exists and is readable by diego
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "test -f /home/diego/user.txt && echo 'exists'"
            )
            if success and "exists" in output:
                self.logger.info("User flag file exists")
            else:
                self.logger.error("User flag file not found")
                return CheckerStatus.CORRUPT
        except Exception as e:
            self.logger.error(f"User flag check failed: {e}")
            return CheckerStatus.CORRUPT

        # Verify root flag exists (encrypted)
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "test -f /root/root.txt.enc && echo 'exists'"
            )
            if success and "exists" in output:
                self.logger.info("Root flag file exists (encrypted)")
            else:
                self.logger.error("Root flag file not found")
                return CheckerStatus.CORRUPT
        except Exception as e:
            self.logger.error(f"Root flag check failed: {e}")
            return CheckerStatus.CORRUPT

        # Verify decrypt script exists
        try:
            success, output = self.run_docker_command(
                self.container_name,
                "test -f /root/decrypt_flag.py && echo 'exists'"
            )
            if success and "exists" in output:
                self.logger.info("Decrypt script exists")
            else:
                self.logger.warning("Decrypt script not found")
        except Exception as e:
            self.logger.warning(f"Decrypt script check failed: {e}")

        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store user flag via Docker"""
        commands = [
            "touch /home/diego/user.txt",
            "chown diego:diego /home/diego/user.txt",
            "chmod 600 /home/diego/user.txt",
            f"echo '{flag}' > /home/diego/user.txt",
            "chmod 400 /home/diego/user.txt"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during flag operation: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"User flag stored: {flag}")
        return CheckerStatus.OK

    def _put_root_flag(self, flag: str) -> CheckerStatus:
        """Store root flag via Docker (encrypted format)"""
        # Encrypt the flag using the fortress artifacts
        artifacts = [
            "web_token_a7f3c9d2e1b4",
            "pam_fragment_8e7d3c2a",
            "db_artifact_f4e3d2c1b0a9",
            "rsync_timestamp_1234567890"
        ]

        # Derive key
        combined = "".join(artifacts)
        key_material = hashlib.sha256(combined.encode()).digest()

        # Encrypt flag locally using pycryptodome
        try:
            from Crypto.Cipher import AES

            cipher = AES.new(key_material, AES.MODE_CBC)
            iv = cipher.iv
            pad_length = 16 - (len(flag) % 16)
            padded_flag = flag + (chr(pad_length) * pad_length)
            ciphertext = cipher.encrypt(padded_flag.encode())
            encrypted = base64.b64encode(iv + ciphertext).decode()
        except ImportError:
            self.logger.error("pycryptodome not installed - cannot encrypt flag")
            return CheckerStatus.ERROR

        # Write encrypted flag to container
        commands = [
            f"echo '{encrypted}' > /root/root.txt.enc",
            "chmod 600 /root/root.txt.enc"
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(f"Failed during root flag write: {cmd} - {output}")
                return CheckerStatus.ERROR

        self.logger.info(f"Root flag encrypted and stored: {flag}")
        return CheckerStatus.OK

    def _get_user_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify user flag"""
        # In production, this would involve:
        # 1. Exploit prototype pollution + template injection for RCE
        # 2. Extract PAM hash from binary
        # 3. Decode Caesar cipher passwords
        # 4. Crack hash and SSH as diego
        # 5. Read /home/diego/user.txt

        # For checker, we'll verify via Docker
        success, output = self.run_docker_command(
            self.container_name,
            "cat /home/diego/user.txt"
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
        # Decrypt the flag using the decrypt script with artifacts
        artifacts = [
            "web_token_a7f3c9d2e1b4",
            "pam_fragment_8e7d3c2a",
            "db_artifact_f4e3d2c1b0a9",
            "rsync_timestamp_1234567890"
        ]

        # Run decrypt script
        decrypt_cmd = f"python3 /root/decrypt_flag.py {' '.join(artifacts)}"
        success, output = self.run_docker_command(
            self.container_name,
            decrypt_cmd
        )

        if not success:
            self.logger.error("Failed to decrypt root flag")
            return CheckerStatus.CORRUPT

        # Extract flag from output (format: "Flag: <flag>")
        if "Flag:" in output:
            decrypted_flag = output.split("Flag:")[1].strip()
        else:
            decrypted_flag = output.strip()

        if expected_flag.strip() in decrypted_flag:
            self.logger.info(f"Root flag verified: {expected_flag}")
            return CheckerStatus.OK
        else:
            self.logger.error(f"Root flag mismatch. Expected: {expected_flag}, Got: {decrypted_flag}")
            return CheckerStatus.CORRUPT

if __name__ == "__main__":
    import sys
    import argparse

    parser = argparse.ArgumentParser(description='Fortress CTF Service Checker')
    parser.add_argument('action', choices=['check', 'put_user', 'put_root', 'get_user', 'get_root'])
    parser.add_argument('host', help='Target host IP')
    parser.add_argument('--team-id', type=int, default=1, help='Team ID')
    parser.add_argument('--flag', help='Flag to put/get')

    args = parser.parse_args()

    checker = FortressChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)
