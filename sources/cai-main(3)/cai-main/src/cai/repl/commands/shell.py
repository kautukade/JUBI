"""
Shell command for CAI REPL.
This module provides commands for executing shell commands.
"""

import os
import signal
import subprocess  # nosec B404
from typing import List, Optional
from rich.console import Console  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command
from cai.tools.common import _get_workspace_dir, _get_container_workspace_path
from cai.util.cli_palette import CAI_GREEN, GREY_TEXT, ORANGE_WARN, YELLOW_WARN

console = Console()


class ShellCommand(Command):
    """Command for executing shell commands."""

    def __init__(self):
        """Initialize the shell command."""
        super().__init__(
            name="/shell",
            description="Execute shell commands in the current environment",
            aliases=["/s", "$"],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the shell command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully, False otherwise
        """
        if not args:
            console.print("[bold red]Error: No command specified[/bold red]")
            return False

        return self.handle_shell_command(args)

    def handle_shell_command(self, command_args: List[str]) -> bool:
        if not command_args:
            console.print("[bold red]Error: No command specified[/bold red]")
            return False

        original_command = " ".join(command_args)
        active_container = os.getenv("CAI_ACTIVE_CONTAINER", "")

        # List of known async-style commands
        is_async = any(
            cmd in original_command
            for cmd in ["nc", "netcat", "ncat", "telnet", "ssh", "python -m http.server"]
        )

        def run_command(command, cwd=None):
            """Execute the given command, optionally in a different working directory (cwd).
            Handles output, async vs sync execution, and user interrupts (Ctrl+C).
            """
            try:
                # Temporary SIGINT handler to allow Ctrl+C to interrupt only this process
                signal.signal(
                    signal.SIGINT, lambda s, f: (_ for _ in ()).throw(KeyboardInterrupt())
                )

                if is_async:
                    console.print(
                        f"[{YELLOW_WARN}]Running in async mode (Ctrl+C to return to REPL)[/]"
                    )
                    os.system(command)
                    console.print(
                        f"[bold {CAI_GREEN}]Async command completed or detached[/bold {CAI_GREEN}]"
                    )
                    return True

                # Run synchronously and stream output
                process = subprocess.Popen(
                    command,
                    shell=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    universal_newlines=True,
                    bufsize=1,
                    cwd=cwd,
                )
                for line in iter(process.stdout.readline, ""):
                    print(line, end="")

                process.wait()

                if process.returncode == 0:
                    console.print(
                        f"[bold {CAI_GREEN}]Command completed successfully[/bold {CAI_GREEN}]"
                    )
                else:
                    console.print(
                        f"[{YELLOW_WARN}]Command exited with code {process.returncode}[/]"
                    )
                return True

            except KeyboardInterrupt:
                # Terminate process on user interrupt
                if not is_async:
                    process.terminate()
                console.print(f"\n[{YELLOW_WARN}]Command interrupted by user[/]")
                return True
            except Exception as e:
                # Handle general execution errors
                console.print(f"[bold red]Execution error: {e}[/bold red]")
                return False
            finally:
                # Restore original SIGINT behavior
                signal.signal(signal.SIGINT, signal.getsignal(signal.SIGINT))

        if active_container:
            # If running in a Docker container
            container_workspace = _get_container_workspace_path()
            console.print(
                f"[{GREY_TEXT}]Running in container: {active_container[:12]}...[/]"
            )
            docker_cmd = f"docker exec -w '{container_workspace}' {active_container} sh -c {original_command!r}"
            console.print(
                f"[bold {CAI_GREEN}]Container workspace[/bold {CAI_GREEN}] "
                f"[{GREY_TEXT}]{container_workspace}[/] — {original_command}"
            )

            success = run_command(docker_cmd)

            # Retry on host if container execution fails
            if not success and "Error response from daemon" in original_command:
                console.print(f"[{ORANGE_WARN}]Container error. Executing on local host.[/]")
                os.environ.pop("CAI_ACTIVE_CONTAINER", None)
                return self.handle_shell_command(command_args)

            return success

        # If no container, run command in local workspace
        host_workspace = _get_workspace_dir()
        console.print(f"[{GREY_TEXT}]Running in workspace: {host_workspace}[/]")

        return run_command(original_command, cwd=host_workspace)


# Register the command
register_command(ShellCommand())
