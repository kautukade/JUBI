"""
Auth command for CAI REPL.

This command allows adding users and devices to the persistent
authentication database used by the CAI API (`AuthManager`).

Typical usage:

    /auth add-user <username> <password>
        Create a new user in the shared auth DB.

    /auth add-ip <device_ip[:port]>
        Create a random user and session token for the given device IP
        and push the credentials to a TCP listener on the device
        (e.g. the iOS app) via a simple JSON handshake.
"""

from __future__ import annotations

import json
import os
import socket
from typing import List, Optional

from rich.console import Console  # type: ignore[import]
from rich.panel import Panel  # type: ignore[import]

from cai.api.auth import AuthManager
from cai.repl.commands.base import Command, register_command

console = Console()


class AuthCommand(Command):
    """Command for managing API users and device pairing."""

    def __init__(self) -> None:
        super().__init__(
            name="/auth",
            description="Manage API auth users and pair devices with the CAI server",
            aliases=[],
        )
        self.add_subcommand(
            "add-user",
            "Add a user to the auth database: /auth add-user <username> <password>",
            self.handle_add_user,
        )
        self.add_subcommand(
            "add-ip",
            "Pair a device by IP and push credentials over TCP: /auth add-ip <ip[:port]>",
            self.handle_add_ip,
        )

    # /auth add-user <username> <password>
    def handle_add_user(self, args: Optional[List[str]] = None) -> bool:
        if not args or len(args) < 2:
            console.print(
                "[red]Usage:[/red] /auth add-user <username> <password>",
            )
            return False

        username, password = args[0], args[1]
        manager = AuthManager()
        try:
            user = manager.create_user(username, password)
        except Exception as exc:  # pragma: no cover - defensive
            console.print(f"[red]Failed to create user:[/red] {exc}")
            return False

        console.print(
            Panel(
                f"User [bold]{user.username}[/bold] added to auth database.",
                title="Auth",
                border_style="green",
            )
        )
        return True

    # /auth add-ip <ip[:port]>
    def handle_add_ip(self, args: Optional[List[str]] = None) -> bool:
        if not args or not args[0]:
            console.print("[red]Usage:[/red] /auth add-ip <ip[:port]>")
            console.print("Example: /auth add-ip 192.168.1.50 or /auth add-ip 192.168.1.50:10101")
            return False

        target = args[0]
        if ":" in target:
            host, port_str = target.rsplit(":", 1)
            try:
                port = int(port_str)
            except ValueError:
                console.print(f"[red]Invalid port in {target}[/red]")
                return False
        else:
            host = target
            port = int(os.getenv("CAI_AUTH_DEVICE_PORT", "10101"))

        # Determine API base URL to send to the device.
        # Priority:
        #   1. Explicit CAI_AUTH_BASE_URL
        #   2. CAI_AUTH_PUBLIC_HOST (if set)
        #   3. IP of the local interface used to reach the device
        #   4. CAI_API_HOST / 127.0.0.1 (last-resort fallback)
        base_url_env = os.getenv("CAI_AUTH_BASE_URL")
        api_host_env = os.getenv("CAI_AUTH_PUBLIC_HOST")
        api_port = int(os.getenv("CAI_AUTH_PUBLIC_PORT") or os.getenv("CAI_API_PORT", "8000"))
        if base_url_env:
            base_url = base_url_env
        else:
            if api_host_env:
                api_host = api_host_env
            else:
                # Try to infer the local IP address used to talk to the device.
                try:
                    tmp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    tmp_sock.connect((host, 1))
                    api_host = tmp_sock.getsockname()[0]
                    tmp_sock.close()
                except Exception:
                    api_host = os.getenv("CAI_API_HOST", "127.0.0.1")
            base_url = f"http://{api_host}:{api_port}/api/v1"

        manager = AuthManager()
        try:
            user, plain_password, session = manager.create_random_user_and_session_for_ip(host)
        except Exception as exc:  # pragma: no cover - defensive
            console.print(f"[red]Failed to create user/session:[/red] {exc}")
            return False

        payload = {
            "base_url": base_url,
            "username": user.username,
            "password": plain_password,
            "session_token": session.token,
        }

        console.print(
            f"[cyan]Connecting to device at[/cyan] [bold]{host}:{port}[/bold] "
            f"to deliver credentials...",
        )

        try:
            with socket.create_connection((host, port), timeout=10) as sock:
                data = json.dumps(payload).encode("utf-8")
                sock.sendall(data)
        except OSError as exc:
            console.print(
                Panel(
                    f"Failed to connect to device at {host}:{port}\n\n{exc}",
                    title="Auth pairing failed",
                    border_style="red",
                )
            )
            console.print(
                "[yellow]Make sure the device is listening (e.g. the iOS app in "
                '"Connect server" mode) and reachable on the network.[/yellow]'
            )
            return False

        console.print(
            Panel(
                "[green]Device pairing request sent successfully.[/green]\n\n"
                f"Assigned username: [bold]{user.username}[/bold]\n"
                f"Random password: [bold]{plain_password}[/bold]\n"
                f"Session token: [dim](hidden, stored in server auth DB)[/dim]\n\n"
                f"API base URL for the device: [bold]{base_url}[/bold]",
                title="Auth pairing",
                border_style="green",
            )
        )
        return True


# Register command on import
register_command(AuthCommand())
