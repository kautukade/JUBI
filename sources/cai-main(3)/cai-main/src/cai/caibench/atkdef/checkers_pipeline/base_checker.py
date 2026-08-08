#!/usr/bin/env python3

import sys
import time
import json
import socket
import subprocess
import logging
from abc import ABC, abstractmethod
from enum import IntEnum
from typing import Optional, Dict, Any, Tuple
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

class CheckerStatus(IntEnum):
    """Standard A/D CTF checker status codes"""
    OK = 101          # Service is OK, flag is correct
    CORRUPT = 102     # Service works but flag is missing/wrong
    MUMBLE = 103      # Service is not working correctly
    DOWN = 104        # Service is completely down
    ERROR = 110       # Internal checker error

class BaseChecker(ABC):
    """Base class for CTF service checkers"""

    def __init__(self, host: str, team_id: int = 1):
        self.host = host
        self.team_id = team_id
        self.logger = self._setup_logger()

        # Flag storage paths (these should be consistent across services)
        self.user_flag_path = "/home/*/user.txt"
        self.root_flag_path = "/root/root.txt"

        # HTTP session with retries
        self.session = self._create_http_session()

    def _setup_logger(self) -> logging.Logger:
        """Setup logging for the checker"""
        logger = logging.getLogger(f"{self.__class__.__name__}_{self.team_id}")
        logger.setLevel(logging.DEBUG)

        # Console handler
        ch = logging.StreamHandler()
        ch.setLevel(logging.INFO)
        formatter = logging.Formatter(
            '%(asctime)s - Team%(team_id)s - %(name)s - %(levelname)s - %(message)s',
            defaults={'team_id': self.team_id}
        )
        ch.setFormatter(formatter)
        logger.addHandler(ch)

        return logger

    def _create_http_session(self) -> requests.Session:
        """Create HTTP session with retry logic"""
        session = requests.Session()
        retry = Retry(
            total=3,
            backoff_factor=0.3,
            status_forcelist=[500, 502, 503, 504]
        )
        adapter = HTTPAdapter(max_retries=retry)
        session.mount('http://', adapter)
        session.mount('https://', adapter)
        return session

    def check_network_connectivity(self) -> CheckerStatus:
        """Check if the host is reachable via ping"""
        try:
            # Ping directly from the host OS (no devcontainer wrapper)
            result = subprocess.run(
                ['ping', '-c', '1', '-W', '5', self.host],
                capture_output=True,
                text=True,
                timeout=10
            )
            if result.returncode == 0:
                self.logger.info(f"Host {self.host} is reachable")
                return CheckerStatus.OK
            else:
                self.logger.warning(f"Host {self.host} is not responding to ping")
                return CheckerStatus.DOWN
        except subprocess.TimeoutExpired:
            self.logger.error(f"Ping to {self.host} timed out")
            return CheckerStatus.DOWN
        except Exception as e:
            self.logger.error(f"Error checking connectivity: {e}")
            return CheckerStatus.ERROR

    def check_port(self, port: int) -> bool:
        """Check if a specific port is open"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        try:
            result = sock.connect_ex((self.host, port))
            return result == 0
        except Exception as e:
            self.logger.error(f"Error checking port {port}: {e}")
            return False
        finally:
            sock.close()

    def put_flag(self, flag: str, flag_type: str = "user") -> CheckerStatus:
        """Store a flag in the service"""
        try:
            if flag_type == "user":
                return self._put_user_flag(flag)
            elif flag_type == "root":
                return self._put_root_flag(flag)
            else:
                self.logger.error(f"Unknown flag type: {flag_type}")
                return CheckerStatus.ERROR
        except Exception as e:
            self.logger.error(f"Error putting {flag_type} flag: {e}")
            return CheckerStatus.ERROR

    def get_flag(self, flag: str, flag_type: str = "user") -> CheckerStatus:
        """Retrieve and verify a flag from the service"""
        try:
            if flag_type == "user":
                return self._get_user_flag(flag)
            elif flag_type == "root":
                return self._get_root_flag(flag)
            else:
                self.logger.error(f"Unknown flag type: {flag_type}")
                return CheckerStatus.ERROR
        except Exception as e:
            self.logger.error(f"Error getting {flag_type} flag: {e}")
            return CheckerStatus.ERROR

    def run_docker_command(self, container_name: str, command: str) -> Tuple[bool, str]:
        """Execute a command inside a Docker container"""
        try:
            # Use sh -c to properly handle shell operators like > and |
            cmd = f"docker exec {container_name} sh -c \"{command}\""
            result = subprocess.run(
                cmd,
                shell=True,
                capture_output=True,
                text=True,
                timeout=10
            )
            success = result.returncode == 0
            output = result.stdout if success else result.stderr
            return success, output
        except subprocess.TimeoutExpired:
            self.logger.error(f"Docker command timed out: {command}")
            return False, "Command timed out"
        except Exception as e:
            self.logger.error(f"Error running docker command: {e}")
            return False, str(e)

    def check(self) -> CheckerStatus:
        """Main check routine - runs all checks in sequence"""
        self.logger.info(f"Starting check for team {self.team_id}")

        # Check network connectivity
        if self.check_network_connectivity() != CheckerStatus.OK:
            return CheckerStatus.DOWN

        # Check service availability
        status = self.check_service_availability()
        if status != CheckerStatus.OK:
            return status

        # Check service functionality
        status = self.check_service_functionality()
        if status != CheckerStatus.OK:
            return status

        # Check flags if they exist
        status = self.check_flags()

        self.logger.info(f"Check completed with status: {status.name}")
        return status

    @abstractmethod
    def check_service_availability(self) -> CheckerStatus:
        """Check if the service is available and responding"""
        pass

    @abstractmethod
    def check_service_functionality(self) -> CheckerStatus:
        """Check if the service is functioning correctly"""
        pass

    @abstractmethod
    def check_flags(self) -> CheckerStatus:
        """Check if flags are present and retrievable"""
        pass

    @abstractmethod
    def _put_user_flag(self, flag: str) -> CheckerStatus:
        """Implementation for storing user flag"""
        pass

    @abstractmethod
    def _put_root_flag(self, flag: str) -> CheckerStatus:
        """Implementation for storing root flag"""
        pass

    @abstractmethod
    def _get_user_flag(self, flag: str) -> CheckerStatus:
        """Implementation for retrieving user flag"""
        pass

    @abstractmethod
    def _get_root_flag(self, flag: str) -> CheckerStatus:
        """Implementation for retrieving root flag"""
        pass

    def run(self, action: str, flag: str = None) -> int:
        """Main entry point for the checker"""
        try:
            if action == "check":
                status = self.check()
            elif action == "put_user":
                if not flag:
                    self.logger.error("Flag required for put_user action")
                    return CheckerStatus.ERROR
                status = self.put_flag(flag, "user")
            elif action == "put_root":
                if not flag:
                    self.logger.error("Flag required for put_root action")
                    return CheckerStatus.ERROR
                status = self.put_flag(flag, "root")
            elif action == "get_user":
                if not flag:
                    self.logger.error("Flag required for get_user action")
                    return CheckerStatus.ERROR
                status = self.get_flag(flag, "user")
            elif action == "get_root":
                if not flag:
                    self.logger.error("Flag required for get_root action")
                    return CheckerStatus.ERROR
                status = self.get_flag(flag, "root")
            else:
                self.logger.error(f"Unknown action: {action}")
                return CheckerStatus.ERROR

            return status

        except Exception as e:
            self.logger.error(f"Checker failed with exception: {e}")
            return CheckerStatus.ERROR