"""
Quickstart command for CAI REPL.
Provides essential setup information and guidance for new users.
Automatically runs on first launch if ~/.cai doesn't exist.
"""

import os
from pathlib import Path
from typing import List, Optional

from rich.console import Console, Group
from rich.panel import Panel
from rich.table import Table
from rich.text import Text
from rich import box

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title

console = Console()


def _qs_state_cell(ok: bool, label: str) -> Text:
    """Positive states in CAI green; missing/off in dim (not green)."""
    return Text(label, style=f"bold {_CAI_GREEN}" if ok else "dim white")


class QuickstartCommand(Command):
    """Command for displaying quickstart guide and setup information."""

    def __init__(self):
        """Initialize the quickstart command."""
        super().__init__(
            name="/quickstart",
            description="Display quickstart guide and setup information",
            aliases=["/qs", "/quick"],
        )

    def handle_no_args(self) -> bool:
        """Handle the command when no arguments are provided."""
        return self.show_quickstart()

    def check_local_endpoint(self, url: str) -> tuple[bool, str]:
        """Check if a local endpoint is accessible.

        Args:
            url: The endpoint URL to check

        Returns:
            Tuple of (is_accessible, message)
        """
        try:
            # Try using httpx which is already imported by the project
            import httpx

            with httpx.Client(timeout=2.0) as client:
                response = client.get(url)
                if response.status_code == 200:
                    return True, "OK"
                else:
                    return False, f"HTTP {response.status_code}"
        except httpx.ConnectError:
            return False, "Connection refused"
        except httpx.TimeoutException:
            return False, "Timeout"
        except ImportError:
            # Fallback if httpx not available
            try:
                import urllib.request
                import urllib.error

                with urllib.request.urlopen(url, timeout=2) as response:
                    if response.status == 200:
                        return True, "OK"
                    else:
                        return False, f"HTTP {response.status}"
            except urllib.error.URLError:
                return False, "Connection refused"
            except Exception:
                return False, "Error checking endpoint"
        except Exception as e:
            return False, str(e)

    def check_ollama_models(self) -> List[str]:
        """Check available Ollama models."""
        try:
            import httpx

            with httpx.Client(timeout=2.0) as client:
                response = client.get("http://localhost:11434/api/tags")
                if response.status_code == 200:
                    data = response.json()
                    return [model["name"] for model in data.get("models", [])]
        except ImportError:
            # Fallback if httpx not available
            try:
                import urllib.request
                import json

                with urllib.request.urlopen(
                    "http://localhost:11434/api/tags", timeout=2
                ) as response:
                    if response.status == 200:
                        data = json.loads(response.read())
                        return [model["name"] for model in data.get("models", [])]
            except:
                pass
        except:
            pass
        return []

    def check_api_keys(self) -> dict[str, bool]:
        """Check which API keys are configured dynamically."""
        keys = {}

        # Scan all environment variables for *_API_KEY pattern
        for env_var in os.environ:
            if env_var.endswith("_API_KEY"):
                # Check if the value is set and not empty
                keys[env_var] = bool(os.getenv(env_var))

        # Also check .env file for any API keys not in current environment
        try:
            from pathlib import Path

            env_file = Path.home() / "cai" / ".env"
            if not env_file.exists():
                # Try current directory
                env_file = Path(".env")

            if env_file.exists():
                with open(env_file, "r") as f:
                    for line in f:
                        line = line.strip()
                        if "=" in line and not line.startswith("#"):
                            key, _ = line.split("=", 1)
                            key = key.strip()
                            if key.endswith("_API_KEY") and key not in keys:
                                # Check if it's in environment (might be loaded)
                                keys[key] = bool(os.getenv(key))
        except:
            pass

        # Sort keys alphabetically for consistent display
        return dict(sorted(keys.items()))

    def show_quickstart(self) -> bool:
        """Quickstart: outer panel like bare /help, inner sub-panels + small tables."""
        z = _CAI_GREEN
        hdr = f"bold {z}"
        api_keys = self.check_api_keys()
        has_api_keys = any(api_keys.values())

        is_accessible, ollama_status = self.check_local_endpoint("http://localhost:11434")
        models = self.check_ollama_models() if is_accessible else []
        model_str = f"{len(models)} listed" if models else "N/A"
        docker_ok, docker_status = self.check_local_endpoint("http://host.docker.internal:11434")

        api_table = Table(show_header=True, header_style=hdr, box=box.SIMPLE, expand=True)
        api_table.add_column("Variable", style="white")
        api_table.add_column("Status", justify="left")

        if api_keys:
            for env_var, is_set in api_keys.items():
                api_table.add_row(
                    env_var,
                    _qs_state_cell(is_set, "Set" if is_set else "Not set"),
                )
        else:
            api_table.add_row(
                "(none detected)",
                _qs_state_cell(False, "Add *_API_KEY"),
            )

        api_inner = [api_table]
        if not has_api_keys:
            api_inner.insert(
                0,
                Text.from_markup(
                    f"[red]No API keys in use.[/red] [white]Set e.g.[/white] "
                    f"[bold {z}]export OPENAI_API_KEY=…[/bold {z}] [white]or add to `.env`.[/white]\n"
                ),
            )
        api_panel = Panel(
            Group(*api_inner),
            title=_quick_guide_subpanel_title("API keys"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        ollama_table = Table(show_header=True, header_style=hdr, box=box.SIMPLE, expand=True)
        ollama_table.add_column("Endpoint", style=f"bold {z}", no_wrap=True)
        ollama_table.add_column("Reachable", justify="left")
        ollama_table.add_column("Models", style="dim")
        ollama_ok = ollama_status == "OK"
        ollama_table.add_row(
            "http://localhost:11434",
            _qs_state_cell(ollama_ok, ollama_status),
            model_str,
        )
        ollama_table.add_row(
            "http://host.docker.internal:11434",
            _qs_state_cell(docker_ok, docker_status),
            "—",
        )
        ollama_inner = [ollama_table]
        ollama_inner.append(
            Text.from_markup(
                f"[white]Install[/white] [dim]curl -fsSL https://ollama.com/install.sh | sh[/dim]  "
                f"[white]· pull[/white] [dim]ollama pull llama3.1[/dim]  "
                f"[white]· `.env`[/white] [bold {z}]OLLAMA_API_BASE=http://127.0.0.1:11434/v1[/bold {z}]  "
                f"[white]· CAI[/white] [bold {z}]/model llama3.1[/bold {z}]"
            )
        )
        if is_accessible and models:
            tail = f"  [dim](+{len(models) - 5} more)[/dim]" if len(models) > 5 else ""
            ollama_inner.append(
                Text.from_markup(
                    f"[dim]Sample names:[/dim] [white]{', '.join(models[:5])}[/white]{tail}"
                )
            )
        ollama_panel = Panel(
            Group(*ollama_inner),
            title=_quick_guide_subpanel_title("Ollama"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        if has_api_keys:
            model_body = Text.from_markup(
                f"[white]1.[/white] [bold {z}]/model show[/bold {z}]  [dim]— full catalog[/dim]\n"
                f"[white]2.[/white] [bold {z}]/model show supported[/bold {z}]  "
                f"[dim]— function-calling subset[/dim]\n"
                f"[white]3.[/white] [bold {z}]/model <name>[/bold {z}]  "
                f"[dim]— id must exist in catalog[/dim]\n"
                "[dim]Pick a concrete model id if the default is not valid for your keys.[/dim]"
            )
        else:
            model_body = Text.from_markup(
                "[white]After an API key is set:[/white] "
                f"[bold {z}]/model show[/bold {z}] [white]then[/white] [bold {z}]/model <name>[/bold {z}]."
            )
        model_panel = Panel(
            model_body,
            title=_quick_guide_subpanel_title("Model"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        commands_table = Table(show_header=True, header_style=hdr, box=box.SIMPLE, expand=True)
        commands_table.add_column("Command", style=f"bold {z}", no_wrap=True)
        commands_table.add_column("Description", style="white")
        commands_table.add_column("Example", style="dim", no_wrap=True)
        for cmd, desc, example in [
            ("/agent list", "List agents", "/agent list"),
            ("/agent select <name>", "Switch agent", "/agent select red_teamer"),
            ("/model", "Current model", "/model"),
            ("/model show", "Full catalog", "/model show"),
            ("/model <name>", "Set model id", "/model gpt-4o"),
            ("/env list", "Env catalog", "/env list"),
            ("/help <topic>", "Topic help", "/help agent"),
            ("/shell <cmd>", "Shell in workspace", "/shell ls -la"),
            ("$ <cmd>", "Shell at line start only", "$ whoami"),
        ]:
            commands_table.add_row(cmd, desc, example)
        commands_panel = Panel(
            commands_table,
            title=_quick_guide_subpanel_title("Essential commands"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        examples_table = Table(show_header=True, header_style=hdr, box=box.SIMPLE, expand=True)
        examples_table.add_column("Flow", style="dim", width=8)
        examples_table.add_column("Command", style=f"bold {z}", no_wrap=True)
        examples_table.add_column("What to do", style="white")
        examples_table.add_row("CTF", "/agent select one_tool_agent", "Describe the challenge.")
        examples_table.add_row("Web", "/agent select bug_bounter", "Give a target URL to test.")
        examples_table.add_row("Net", "/agent select red_teamer", "Ask for recon on a host or subnet.")
        examples_panel = Panel(
            examples_table,
            title=_quick_guide_subpanel_title("Examples"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        features_table = Table(show_header=True, header_style=hdr, box=box.SIMPLE, expand=True)
        features_table.add_column("Area", style=f"bold {z}")
        features_table.add_column("Notes", style="white")
        features_table.add_row("Agents", "Specialized roles (red team, DFIR, …).")
        features_table.add_row("Tools", "Shell, MCP, Docker-backed runs when configured.")
        features_table.add_row("Parallel", "Several agents; merge or clear when done.")
        features_panel = Panel(
            features_table,
            title=_quick_guide_subpanel_title("Features"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        cai_dir = Path.home() / ".cai"
        cai_note = "will be created on first run" if not cai_dir.exists() else "exists"
        next_panel = Panel(
            Text.from_markup(
                "[white]Then open[/white] "
                f"[bold {z}]/help topics[/bold {z}] [white]for commands by category and /help <topic> hints.[/white]\n"
                f"[dim]Logs and state under {cai_dir} ({cai_note}). "
                "Run this guide again with[/dim] [bold]/quickstart[/bold]."
            ),
            title=_quick_guide_subpanel_title("Next"),
            title_align="left",
            border_style=z,
            padding=(1, 1),
        )

        intro = Text.from_markup(
            f"[white]CAI (Cybersecurity AI): pentest, bug bounty, CTF. "
            f"Same guide:[/white] [bold {z}]/quickstart[/bold {z}]"
            f"[dim] · [/dim][bold {z}]/qs[/bold {z}][dim] · [/dim][bold {z}]/quick[/bold {z}]."
        )

        body = Group(
            intro,
            Text(""),
            api_panel,
            Text(""),
            ollama_panel,
            Text(""),
            model_panel,
            Text(""),
            commands_panel,
            Text(""),
            examples_panel,
            Text(""),
            features_panel,
            Text(""),
            next_panel,
        )

        console.print(
            Panel(
                body,
                title=_quick_guide_subpanel_title("Quickstart"),
                title_align="left",
                border_style=z,
                padding=(1, 1),
                box=box.ROUNDED,
            )
        )
        return True


# Register the command
register_command(QuickstartCommand())
