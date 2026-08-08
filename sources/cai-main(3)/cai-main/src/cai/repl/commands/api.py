"""REPL /api: read or write ALIAS_API_KEY in .env."""

import os
import re
from typing import List, Optional
from rich.console import Console  # pylint: disable=import-error
from rich.panel import Panel  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN
from cai.repl.ui.startup_hints import mask_key_for_hint

console = Console()

_GREY_SECONDARY = "#9aa0a6"


class ApiCommand(Command):
    def __init__(self):
        super().__init__(
            name="/api",
            description="Show or set ALIAS_API_KEY in .env (Alias / CAI PRO)",
            aliases=["/apikey"],
        )

        self.add_subcommand("show", "Masked ALIAS_API_KEY (.env, else env)", self.handle_show)
        self.add_subcommand("set", "Write ALIAS_API_KEY to .env + os.environ", self.handle_set)

    def handle(self, args: Optional[List[str]] = None) -> bool:
        if not args:
            return self.handle_show(args)
        if args[0] in self.get_subcommands():
            rem = args[1:] if len(args) > 1 else None
            return self.subcommands[args[0]]["handler"](rem)
        return self.handle_set(args)

    def handle_show(self, args: Optional[List[str]] = None) -> bool:
        try:
            env_file_path = self._get_env_file_path()
            from_file = self._get_current_api_key(env_file_path)
            from_env = (os.getenv("ALIAS_API_KEY") or "").strip()
            current_key = from_file or from_env or None
            source_note = ""
            if not from_file and from_env:
                source_note = f"\n[{_GREY_SECONDARY}]Source: process environment (not set in .env)[/{_GREY_SECONDARY}]"

            if current_key:
                masked_key = mask_key_for_hint(current_key)
                console.print(
                    Panel(
                        f"ALIAS_API_KEY (masked): [bold {_CAI_GREEN}]{masked_key}[/bold {_CAI_GREEN}]{source_note}",
                        border_style=_CAI_GREEN,
                        title="ALIAS_API_KEY",
                    )
                )
            else:
                console.print(
                    Panel(
                        f"[yellow]ALIAS_API_KEY is not set in .env or the environment.[/yellow]\n"
                        f"[{_GREY_SECONDARY}]Use [bold {_CAI_GREEN}]/api set <key>[/bold {_CAI_GREEN}] "
                        f"or [bold {_CAI_GREEN}]/api <key>[/bold {_CAI_GREEN}].[/]",
                        border_style="yellow",
                        title="ALIAS_API_KEY",
                    )
                )
            return True

        except Exception as e:
            console.print(f"[red]Error reading API key: {e}[/red]")
            return False

    def handle_set(self, args: Optional[List[str]] = None) -> bool:
        if not args or not args[0]:
            console.print("[red]Error: API key is required[/red]")
            console.print(f"Usage: [bold {_CAI_GREEN}]/api set <key>[/bold {_CAI_GREEN}]   or   [bold {_CAI_GREEN}]/api <key>[/bold {_CAI_GREEN}]")
            return False

        new_api_key = args[0].strip()

        if len(new_api_key) < 10:
            console.print("[red]Error: API key seems too short (minimum 10 characters)[/red]")
            return False

        try:
            env_file_path = self._get_env_file_path()
            success = self._update_env_file(env_file_path, new_api_key)

            if success:
                masked_key = mask_key_for_hint(new_api_key)
                console.print(
                    Panel(
                        f"ALIAS_API_KEY updated (masked): [bold {_CAI_GREEN}]{masked_key}[/bold {_CAI_GREEN}]\n"
                        f"[{_GREY_SECONDARY}]Applies on the next agent interaction; process env updated now.[/]",
                        border_style=_CAI_GREEN,
                        title="ALIAS_API_KEY",
                    )
                )

                os.environ["ALIAS_API_KEY"] = new_api_key
                self._update_sidebar_keys()

                return True
            console.print("[red]Error: Failed to update .env file[/red]")
            return False

        except Exception as e:
            console.print(f"[red]Error updating API key: {e}[/red]")
            return False

    def _get_env_file_path(self) -> str:
        current_dir = os.getcwd()
        env_path = os.path.join(current_dir, ".env")
        if os.path.exists(env_path):
            return env_path

        search_dir = current_dir
        for _ in range(5):
            if any(
                os.path.exists(os.path.join(search_dir, marker))
                for marker in ["pyproject.toml", "setup.py", ".git"]
            ):
                env_path = os.path.join(search_dir, ".env")
                if os.path.exists(env_path):
                    return env_path
            parent = os.path.dirname(search_dir)
            if parent == search_dir:
                break
            search_dir = parent
        return os.path.join(current_dir, ".env")

    def _get_current_api_key(self, env_file_path: str) -> Optional[str]:
        if not os.path.exists(env_file_path):
            return None

        try:
            with open(env_file_path, "r", encoding="utf-8") as file:
                content = file.read()

            match = re.search(r'^ALIAS_API_KEY\s*=\s*["\']?([^"\']*)["\']?', content, re.MULTILINE)
            if match:
                return match.group(1).strip()

            return None
        except Exception:
            return None

    def _create_env_backup(self, env_file_path: str) -> bool:
        try:
            import datetime
            import shutil

            if not os.path.exists(env_file_path):
                return True
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            backup_path = f"{env_file_path}.backup.{timestamp}"
            latest_backup_path = f"{env_file_path}.backup"
            shutil.copy2(env_file_path, backup_path)
            shutil.copy2(env_file_path, latest_backup_path)
            console.print(f"[dim].env backups: {backup_path} | {latest_backup_path}[/dim]")

            return True

        except Exception as e:
            console.print(f"[red]Error creating .env backup: {e}[/red]")
            return False

    def _update_env_file(self, env_file_path: str, new_api_key: str) -> bool:
        try:
            self._create_env_backup(env_file_path)
            if os.path.exists(env_file_path):
                with open(env_file_path, "r", encoding="utf-8") as file:
                    content = file.read()
            else:
                content = ""

            alias_key_pattern = r'^(ALIAS_API_KEY\s*=\s*)["\']?[^"\']*["\']?'
            new_line = f'ALIAS_API_KEY="{new_api_key}"'

            if re.search(alias_key_pattern, content, re.MULTILINE):
                content = re.sub(alias_key_pattern, new_line, content, flags=re.MULTILINE)
            else:
                if content and not content.endswith("\n"):
                    content += "\n"
                content += new_line + "\n"

            with open(env_file_path, "w", encoding="utf-8") as file:
                file.write(content)

            return True

        except Exception as e:
            console.print(f"[red]Error writing to .env file: {e}[/red]")
            return False

    def _update_sidebar_keys(self) -> None:
        def _refresh(app) -> bool:
            sb = getattr(app, "sidebar", None)
            if not sb:
                return False
            from cai.tui.components.sidebar import RefreshKeysMessage

            app.post_message(RefreshKeysMessage())
            sb.force_refresh_keys()
            return True

        try:
            from cai.tui.cai_terminal import CAITerminal

            if _refresh(CAITerminal._instance):
                return
        except Exception:
            pass
        try:
            from textual.app import App

            _refresh(App.get_running_app())
        except Exception:
            if os.getenv("CAI_DEBUG"):
                import traceback

                traceback.print_exc()


register_command(ApiCommand())
