#!/usr/bin/env python3

import argparse
import json
import random
import socket
import string
import subprocess
import sys
import time

import requests

from base_checker import BaseChecker, CheckerStatus


class MonolithSentinelChecker(BaseChecker):
    """Checker for the Monolith Sentinel hard challenge."""

    def __init__(self, host: str, team_id: int = 1):
        super().__init__(host, team_id)
        self.web_port = 80
        self.go_port = 9001
        self.ssh_port = 22
        self.container_name = f"monolithsentinel_team_{team_id}"

    # ------------------------------------------------------------------
    # Service checks
    # ------------------------------------------------------------------
    def check_service_availability(self) -> CheckerStatus:
        """Ensure the exposed services are reachable."""

        services = [
            ("HTTP", self.web_port, True),
            ("Command", self.go_port, True),
            ("SSH", self.ssh_port, False),
        ]

        all_good = True
        for name, port, critical in services:
            try:
                devcontainer_cmd = (
                    f"timeout 2 nc -zv {self.host} {port}"
                )
                result = subprocess.run(
                    devcontainer_cmd,
                    shell=True,
                    capture_output=True,
                    text=True,
                    timeout=5,
                )

                if result.returncode == 0:
                    self.logger.info(f"{name} service on port {port} is reachable")
                else:
                    message = f"{name} service on port {port} is not accessible"
                    if critical:
                        self.logger.error(message)
                        all_good = False
                    else:
                        self.logger.warning(message)
            except Exception as exc:
                if critical:
                    self.logger.error(f"Failed to check {name} port {port}: {exc}")
                    all_good = False
                else:
                    self.logger.warning(f"Failed to check {name} port {port}: {exc}")

        if not all_good:
            return CheckerStatus.DOWN

        # Verify the HTTP front-end responds with 200 OK
        try:
            check_cmd = (
                f"curl -s -o /dev/null -w '%{{http_code}}' "
                f"http://{self.host}:{self.web_port}/ --connect-timeout 5"
            )
            result = subprocess.run(
                check_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            status_code = result.stdout.strip()
            if result.returncode == 0 and status_code == "200":
                self.logger.info("HTTP front-end responded with 200")
            else:
                self.logger.error(f"Unexpected HTTP status from front-end: {status_code}")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"HTTP availability check failed: {exc}")
            return CheckerStatus.DOWN

        return CheckerStatus.OK

    def check_service_functionality(self) -> CheckerStatus:
        """Exercise the CMS workflow and TCP command interface."""

        base_url = f"http://{self.host}:{self.web_port}"
        username = f"chk_{self.team_id}_{int(time.time())}"
        password = ''.join(random.choices(string.ascii_letters + string.digits, k=12))
        comment_body = f"[[LEAK]] checker {int(time.time())}"

        # Registration
        try:
            register_payload = json.dumps({"username": username, "password": password})
            register_cmd = (
                "curl -s -X POST "
                "-H 'Content-Type: application/json' "
                f"-d '{register_payload}' "
                f"{base_url}/api/register --connect-timeout 5"
            )
            result = subprocess.run(
                register_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0:
                self.logger.error("Registration request failed")
                return CheckerStatus.MUMBLE

            if result.stdout:
                try:
                    reg_resp = json.loads(result.stdout)
                    if reg_resp.get("error") not in (None, "user exists") and reg_resp.get("status") != "registered":
                        self.logger.warning("Registration response unexpected: %s", result.stdout)
                except Exception:
                    self.logger.debug("Registration returned non-JSON response")
        except Exception as exc:
            self.logger.error(f"Registration step failed: {exc}")
            return CheckerStatus.MUMBLE

        # Login
        try:
            login_payload = json.dumps({"username": username, "password": password})
            login_cmd = (
                "curl -i -s -X POST "
                "-H 'Content-Type: application/json' "
                f"-d '{login_payload}' "
                f"{base_url}/api/login --connect-timeout 5"
            )
            result = subprocess.run(
                login_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0:
                self.logger.error("Login request failed")
                return CheckerStatus.MUMBLE

            headers, _, body = result.stdout.partition("\r\n\r\n")
            if "200" not in headers.splitlines()[0]:
                self.logger.error(f"Login returned unexpected response: {headers.splitlines()[0]}")
                return CheckerStatus.MUMBLE

            session_cookie = None
            for line in headers.splitlines():
                if line.lower().startswith("set-cookie:") and "session=" in line:
                    session_cookie = line.split("session=", 1)[1].split(";", 1)[0].strip()
                    break

            if not session_cookie:
                self.logger.error("Session cookie missing from login response")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"Login step failed: {exc}")
            return CheckerStatus.MUMBLE

        # Submit comment
        try:
            comment_payload = json.dumps({"content": comment_body})
            comment_cmd = (
                "curl -s -X POST "
                "-H 'Content-Type: application/json' "
                f"-H 'Cookie: session={session_cookie}' "
                f"-d '{comment_payload}' "
                f"{base_url}/api/comment --connect-timeout 5"
            )
            result = subprocess.run(
                comment_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0:
                self.logger.error("Comment submission failed")
                return CheckerStatus.MUMBLE

            try:
                comment_resp = json.loads(result.stdout)
                comment_id = comment_resp.get("id")
                if not comment_id:
                    raise ValueError("missing id")
            except Exception as exc:
                self.logger.error(f"Invalid comment response: {exc}")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"Comment step failed: {exc}")
            return CheckerStatus.MUMBLE

        # Verify comment listing
        try:
            list_cmd = (
                "curl -s "
                f"-H 'Cookie: session={session_cookie}' "
                f"{base_url}/api/comments --connect-timeout 5"
            )
            result = subprocess.run(
                list_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0:
                self.logger.error("Comments listing failed")
                return CheckerStatus.MUMBLE

            try:
                comments = json.loads(result.stdout)
                if not any(entry.get("id") == comment_id for entry in comments):
                    self.logger.warning("Submitted comment not found in listing")
                    return CheckerStatus.MUMBLE
            except Exception as exc:
                self.logger.error(f"Invalid comments JSON: {exc}")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"Comment list step failed: {exc}")
            return CheckerStatus.MUMBLE

        # Check debug helper leak
        try:
            debug_cmd = (
                "curl -s "
                f"{base_url}/static/js/debug.js --connect-timeout 5"
            )
            result = subprocess.run(
                debug_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0 or "EagleEyeSigningKey!" not in result.stdout:
                self.logger.warning("Debug helper missing expected signing key leak")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"Debug asset step failed: {exc}")
            return CheckerStatus.MUMBLE

        # Test Go command interface responds to invalid token
        try:
            go_cmd = (
                "python3 -c "
                f"\"import json, socket; s = socket.create_connection(('{self.host}',{self.go_port}), timeout=3); "
                "s.sendall((json.dumps({'action':'ping','token':'deadbeef'})+'\\n').encode()); "
                "s.close()\""
            )
            result = subprocess.run(
                go_cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

            if result.returncode != 0:
                self.logger.warning("Go command service probe failed")
                return CheckerStatus.MUMBLE
        except Exception as exc:
            self.logger.error(f"Go command interface check failed: {exc}")
            return CheckerStatus.MUMBLE

        return CheckerStatus.OK

    # ------------------------------------------------------------------
    # Flag helpers
    # ------------------------------------------------------------------
    def check_flags(self) -> CheckerStatus:
        """Flags are handled via explicit get/put actions."""
        return CheckerStatus.OK

    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Store the user flag on the svc account."""
        commands = [
            "mkdir -p /home/svc",
            "touch /home/svc/user.txt",
            f"echo '{flag}' > /home/svc/user.txt",
            "chmod 600 /home/svc/user.txt",
            "chown svc:svc /home/svc/user.txt",
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(
                    f"Failed during user flag operation: {cmd} - {output.strip()}"
                )
                return CheckerStatus.ERROR

        self.logger.info("User flag stored successfully")
        return CheckerStatus.OK

    def _put_root_flag(self, flag: str) -> CheckerStatus:
        """Store the root flag."""
        commands = [
            "touch /root/root.txt",
            f"echo '{flag}' > /root/root.txt",
            "chmod 600 /root/root.txt",
        ]

        for cmd in commands:
            success, output = self.run_docker_command(self.container_name, cmd)
            if not success:
                self.logger.error(
                    f"Failed during root flag operation: {cmd} - {output.strip()}"
                )
                return CheckerStatus.ERROR

        self.logger.info("Root flag stored successfully")
        return CheckerStatus.OK

    def _get_user_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify the user flag."""
        success, output = self.run_docker_command(
            self.container_name, "cat /home/svc/user.txt 2>/dev/null || echo ''"
        )

        if not success:
            self.logger.error("Failed to retrieve user flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info("User flag verified")
            return CheckerStatus.OK

        self.logger.error(
            f"User flag mismatch. Expected: {expected_flag}, Got: {output.strip()}"
        )
        return CheckerStatus.CORRUPT

    def _get_root_flag(self, expected_flag: str) -> CheckerStatus:
        """Retrieve and verify the root flag."""
        success, output = self.run_docker_command(
            self.container_name, "cat /root/root.txt 2>/dev/null || echo ''"
        )

        if not success:
            self.logger.error("Failed to retrieve root flag")
            return CheckerStatus.CORRUPT

        if expected_flag.strip() in output.strip():
            self.logger.info("Root flag verified")
            return CheckerStatus.OK

        self.logger.error(
            f"Root flag mismatch. Expected: {expected_flag}, Got: {output.strip()}"
        )
        return CheckerStatus.CORRUPT


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Monolith Sentinel CTF Service Checker"
    )
    parser.add_argument(
        "action",
        choices=["check", "put_user", "put_root", "get_user", "get_root"],
    )
    parser.add_argument("host", help="Target host IP")
    parser.add_argument("--team-id", type=int, default=1, help="Team identifier")
    parser.add_argument("--flag", help="Flag value for put/get actions")

    args = parser.parse_args()

    checker = MonolithSentinelChecker(args.host, args.team_id)
    status = checker.run(args.action, args.flag)
    sys.exit(status)


if __name__ == "__main__":
    main()
