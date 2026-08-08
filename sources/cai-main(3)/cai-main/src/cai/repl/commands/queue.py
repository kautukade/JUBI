"""
Queue command for managing prompt queue
"""

from typing import List, Optional
from rich.table import Table
from rich.panel import Panel
from datetime import datetime
import os

from .base import Command, register_command, console
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title

# Try to import the TUI prompt queue if available
try:
    if os.getenv("CAI_TUI_MODE") == "true":
        from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_PROMPT_QUEUE
        from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
    else:
        TUI_PROMPT_QUEUE = None
        TERMINAL_QUEUE_MANAGER = None
except ImportError:
    TUI_PROMPT_QUEUE = None
    TERMINAL_QUEUE_MANAGER = None

# Fallback queue for non-TUI mode
FALLBACK_QUEUE = []

# Flag for cli_headless to detect /queue run trigger
_TRIGGER_QUEUE_RUN = False


class QueueCommand(Command):
    """Manage the prompt queue"""

    def __init__(self):
        super().__init__(
            name="/queue",
            aliases=["/que"],
            description="Manage prompt queue - show, add, remove, or clear prompts",
        )
        self.add_subcommand("show", "Show the current queue", self._show_queue)
        self.add_subcommand("list", "Show the current queue", self._show_queue)
        self.add_subcommand("add", "Add a prompt to the queue", self._handle_add)
        self.add_subcommand("run", "Execute all queued prompts", self._handle_run)
        self.add_subcommand("remove", "Remove a prompt by index", self._handle_remove_cmd)
        self.add_subcommand("clear", "Clear all queued prompts", self._handle_clear_cmd)
        self.add_subcommand("next", "Show the next prompt in queue", self._handle_next_cmd)
        self.add_subcommand("move", "Move a prompt to a new position", self._handle_move_cmd)
        self.add_subcommand("load", "Load prompts from a file", self._handle_load_cmd)

    def handle(self, args: Optional[list[str]] = None) -> bool:
        """Dispatch subcommands, with TUI terminal-queue interception."""
        if TERMINAL_QUEUE_MANAGER and os.getenv("CAI_TUI_MODE") == "true":
            return self._handle_terminal_queues(args)
        return super().handle(args)

    def handle_no_args(self) -> bool:
        """Show queue when invoked without arguments."""
        return self._show_queue()

    def handle_unknown_subcommand(self, subcommand: str) -> bool:
        """Show help when an unknown subcommand is used."""
        console.print(
            f"[red]Unknown /queue subcommand: {subcommand}[/red]"
        )
        self._show_help()
        return False

    def _handle_remove_cmd(self, args: Optional[list[str]] = None) -> bool:
        """Wrapper for remove that parses the index argument."""
        if not args:
            console.print("[red]Error: Index required. Usage: /queue remove <index>[/red]")
            return False
        try:
            index = int(args[0]) - 1
            return self._remove_from_queue(index)
        except ValueError:
            console.print(f"[red]Error: Invalid index '{args[0]}'[/red]")
            return False

    def _handle_clear_cmd(self, args: Optional[list[str]] = None) -> bool:
        """Wrapper so clear receives the standard (self, args) signature."""
        return self._clear_queue()

    def _handle_next_cmd(self, args: Optional[list[str]] = None) -> bool:
        """Wrapper so next receives the standard (self, args) signature."""
        return self._get_next()

    def _handle_load_cmd(self, args: Optional[list[str]] = None) -> bool:
        """Handle the load subcommand."""
        if not args:
            queue_file = os.getenv("CAI_QUEUE_FILE")
            if queue_file:
                load_queue_from_file(os.path.expanduser(queue_file))
                return True
            console.print(
                "[red]Error: File path required. "
                "Usage: /queue load <file_path>[/red]"
            )
            return False
        file_path = " ".join(args)
        load_queue_from_file(os.path.expanduser(file_path))
        return True

    def _handle_move_cmd(self, args: Optional[list[str]] = None) -> bool:
        """Wrapper for move that parses two index arguments (1-based)."""
        if not args or len(args) < 2:
            console.print(
                "[red]Error: Two indices required. "
                "Usage: /queue move <from> <to>[/red]"
            )
            return False
        try:
            from_idx = int(args[0]) - 1
            to_idx = int(args[1]) - 1
        except ValueError:
            console.print(
                f"[red]Error: Invalid indices '{args[0]}', '{args[1]}'. "
                "Both must be numbers.[/red]"
            )
            return False
        return self._move_in_queue(from_idx, to_idx)

    def _move_in_queue(self, from_idx: int, to_idx: int) -> bool:
        """Move a queue item from one position to another (0-based)."""
        queue_size = len(FALLBACK_QUEUE)

        if queue_size == 0:
            console.print("[yellow]Queue is empty, nothing to move.[/yellow]")
            return False

        if (
            from_idx < 0
            or from_idx >= queue_size
            or to_idx < 0
            or to_idx >= queue_size
        ):
            error_panel = Panel(
                "[bold red]Error: Index out of range[/bold red]\n\n"
                f"[dim]Valid range: 1 to {queue_size}[/dim]",
                border_style="red",
            )
            console.print(error_panel)
            return False

        if from_idx == to_idx:
            console.print("[yellow]Source and destination are the same, no change.[/yellow]")
            return True

        item = FALLBACK_QUEUE.pop(from_idx)
        FALLBACK_QUEUE.insert(to_idx, item)

        prompt_short = item["prompt"][:50] + "..." if len(item["prompt"]) > 50 else item["prompt"]
        move_panel = Panel(
            f"[bold #00ff9d]Item moved successfully[/bold #00ff9d]\n\n"
            f"[white]#{from_idx + 1} -> #{to_idx + 1}[/white]\n"
            f"[dim]{prompt_short}[/dim]",
            title=_quick_guide_subpanel_title("Queue Reordered"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 2),
        )
        console.print(move_panel)
        return True

    def _handle_add(self, args: Optional[list[str]] = None) -> bool:
        """Handle the add action with optional --agent flag.

        Syntax:
            /queue add <prompt>                    -- queue with active agent
            /queue add --agent <name> <prompt>     -- queue with specific agent
            /queue add --agent                     -- interactive agent selection
        """
        if not args:
            error_panel = Panel(
                "[bold red]Error: No prompt provided[/bold red]\n\n"
                "[dim]Usage: /queue add <prompt>[/dim]\n"
                "[dim]       /queue add --agent <agent_name> <prompt>[/dim]",
                border_style="red"
            )
            console.print(error_panel)
            return False

        agent_key: Optional[str] = None

        if args[0] == "--agent":
            from cai.agents import get_available_agents
            available = get_available_agents()

            if len(args) < 2:
                return self._interactive_agent_select(available)

            candidate = args[1]
            if candidate in available:
                agent_key = candidate
                remaining = args[2:]
            else:
                console.print(
                    f"[red]Unknown agent '[bold]{candidate}[/bold]'. "
                    "Available agents:[/red]"
                )
                for key in sorted(available):
                    name = getattr(available[key], "name", key)
                    console.print(f"  [bold #00ff9d]{key}[/bold #00ff9d] — {name}")
                return False

            if not remaining:
                error_panel = Panel(
                    "[bold red]Error: No prompt provided after agent[/bold red]\n\n"
                    f"[dim]Usage: /queue add --agent {agent_key} <prompt>[/dim]",
                    border_style="red"
                )
                console.print(error_panel)
                return False
            prompt = " ".join(remaining)
        else:
            prompt = " ".join(args)

        return self._add_to_queue(prompt, agent=agent_key)

    def _interactive_agent_select(self, available: dict) -> bool:
        """Show interactive agent list and prompt for selection + prompt text."""
        from rich.table import Table as RichTable

        table = RichTable(
            title="[bold #00ff9d]Available Agents[/bold #00ff9d]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            box=None,
        )
        table.add_column("#", style="#9aa0a6", width=4)
        table.add_column("Key", style="bold #00ff9d")
        table.add_column("Name", style="white")

        keys = sorted(available.keys())
        for idx, key in enumerate(keys, 1):
            name = getattr(available[key], "name", key)
            table.add_row(str(idx), key, name)

        console.print(table)
        console.print(
            "\n[dim]Select an agent by number or key, "
            "then provide the prompt.[/dim]"
        )
        console.print(
            "[dim]Example: /queue add --agent red_teamer "
            "scan the target[/dim]"
        )
        return True

    def _handle_run(self, args: Optional[list[str]] = None) -> bool:
        """Trigger sequential execution of queued prompts."""
        global _TRIGGER_QUEUE_RUN

        queue_items = self._get_queue_items()
        if not queue_items:
            console.print(
                "[yellow]Queue is empty. "
                "Add prompts with [bold]/queue add <prompt>[/bold] first.[/yellow]"
            )
            return True

        from rich.table import Table as RichTable
        table = RichTable(
            title="[bold #00ff9d]Executing Queue[/bold #00ff9d]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            box=None,
        )
        table.add_column("#", style="#9aa0a6", width=4)
        table.add_column("Agent", style="bold #00ff9d", width=20)
        table.add_column("Prompt", style="white")

        for idx, item in enumerate(queue_items, 1):
            agent_display = item.get("agent") or "[dim]active[/dim]"
            prompt_text = item.get("prompt", "")
            prompt_short = prompt_text[:60] + "..." if len(prompt_text) > 60 else prompt_text
            table.add_row(str(idx), agent_display, prompt_short)

        console.print(table)
        console.print(
            f"\n[bold #00ff9d]Starting sequential execution "
            f"of {len(queue_items)} prompt(s)...[/bold #00ff9d]"
        )

        _TRIGGER_QUEUE_RUN = True
        return True

    def _show_queue(self, args: Optional[list[str]] = None) -> bool:
        """Show the current queue"""
        queue_items = self._get_queue_items()
        
        if not queue_items:
            # Empty queue with styled panel
            empty_panel = Panel(
                "[dim italic]No prompts in queue[/dim italic]",
                title=_quick_guide_subpanel_title("Prompt Queue"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 2),
            )
            console.print(empty_panel)
            return True
        
        # Create a fancy table with modern styling
        table = Table(
            title="[bold #00ff9d]Prompt Queue[/bold #00ff9d]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            title_style="bold #00ff9d",
            caption=f"[dim]Total items: {len(queue_items)}[/dim]",
            caption_style="dim",
            row_styles=["none", "#9aa0a6"],
            pad_edge=True,
            box=None,
        )

        has_agents = any(item.get("agent") for item in queue_items)

        table.add_column("#", style="bold #00ff9d", width=4, justify="center")
        if has_agents:
            table.add_column("Agent", style="bold #00ff9d", width=18)
        table.add_column("Prompt", style="white", overflow="ellipsis")
        table.add_column("Time", style="#9aa0a6", width=12)

        for i, item in enumerate(queue_items, 1):
            prompt_text = item.get("prompt", "")

            if prompt_text.startswith("/"):
                prompt_display = f"[bold #00ff9d]{prompt_text}[/bold #00ff9d]"
            elif prompt_text.startswith("$"):
                prompt_display = f"[bold yellow]{prompt_text}[/bold yellow]"
            else:
                prompt_display = f"[white]{prompt_text}[/white]"

            timestamp = item.get("timestamp", datetime.now())
            time_str = timestamp.strftime("%H:%M:%S")

            row: list[str] = [f"[bold #00ff9d]{i}[/bold #00ff9d]"]
            if has_agents:
                agent_val = item.get("agent") or "[dim]active[/dim]"
                row.append(agent_val)
            row.extend([
                prompt_display,
                f"[#9aa0a6]{time_str}[/]",
            ])
            table.add_row(*row)
        
        # Wrap table in a panel for better visual
        queue_panel = Panel(
            table,
            border_style=_CAI_GREEN,
            padding=(0, 1),
        )

        console.print(queue_panel)

        # Add helpful tip
        console.print(
            "\n[#9aa0a6]Tip: [/][bold #00ff9d]/queue add <prompt>[/bold #00ff9d]"
            "[#9aa0a6] to add, [/][bold #00ff9d]/queue run[/bold #00ff9d]"
            "[#9aa0a6] to execute, [/][bold #00ff9d]/queue remove <index>[/bold #00ff9d]"
            "[#9aa0a6] to remove[/]"
        )
        
        return True
    
    def _get_queue_items(self) -> List[dict]:
        """Get queue items from the appropriate queue"""
        # Try to get the TUI queue dynamically
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
                # Get from TUI queue
                items = []
                for queued_prompt in TUI_QUEUE._queue:
                    items.append({
                        "prompt": queued_prompt.prompt,
                        "timestamp": queued_prompt.timestamp,
                    })
                return items
            except ImportError:
                pass
        
        # Fallback to module-level queue
        if TUI_PROMPT_QUEUE:
            # Get from TUI queue
            items = []
            for queued_prompt in TUI_PROMPT_QUEUE._queue:
                items.append({
                    "prompt": queued_prompt.prompt,
                    "timestamp": queued_prompt.timestamp,
                })
            return items
        else:
            # Use fallback queue
            return FALLBACK_QUEUE

    def _add_to_queue(self, prompt: str, agent: Optional[str] = None) -> bool:
        """Add a prompt to the queue with an optional agent association."""
        queue_len = 0

        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
                import asyncio
                asyncio.create_task(TUI_QUEUE.add_prompt(prompt))
                queue_len = len(TUI_QUEUE._queue) + 1
            except ImportError:
                pass

        if queue_len == 0:
            if TUI_PROMPT_QUEUE:
                import asyncio
                asyncio.create_task(TUI_PROMPT_QUEUE.add_prompt(prompt))
                queue_len = len(TUI_PROMPT_QUEUE._queue) + 1
            else:
                item = {
                    "prompt": prompt,
                    "timestamp": datetime.now(),
                    "agent": agent,
                }
                FALLBACK_QUEUE.append(item)
                queue_len = len(FALLBACK_QUEUE)

        agent_label = f" [magenta]({agent})[/magenta]" if agent else ""
        prompt_short = prompt[:50] + "..." if len(prompt) > 50 else prompt
        success_panel = Panel(
            f"[bold #00ff9d]Prompt added successfully![/bold #00ff9d]\n\n"
            f"[white]Position: [bold #00ff9d]#{queue_len}[/bold #00ff9d]{agent_label}[/white]\n"
            f"[dim]Prompt: {prompt_short}[/dim]",
            title=_quick_guide_subpanel_title("Queue Updated"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 2),
        )
        console.print(success_panel)
        return True

    def _remove_from_queue(self, index: int) -> bool:
        """Remove a prompt from the queue"""
        queue_items = self._get_queue_items()
        
        if index < 0 or index >= len(queue_items):
            error_panel = Panel(
                "[bold red]Error: Index out of range[/bold red]\n\n"
                f"[dim]Valid range: 1 to {len(queue_items)}[/dim]",
                border_style="red",
            )
            console.print(error_panel)
            return False
        
        prompt_text = ""
        
        # Try to get the TUI queue dynamically
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
                # Remove from TUI queue
                if index < len(TUI_QUEUE._queue):
                    removed = TUI_QUEUE._queue.pop(index)
                    prompt_text = removed.prompt[:50]
                else:
                    prompt_text = "Unknown"
            except ImportError:
                pass
        
        # Fallback to module-level queue if needed
        if not prompt_text:
            if TUI_PROMPT_QUEUE:
                # Remove from TUI queue
                if index < len(TUI_PROMPT_QUEUE._queue):
                    removed = TUI_PROMPT_QUEUE._queue.pop(index)
                    prompt_text = removed.prompt[:50]
                else:
                    prompt_text = "Unknown"
            else:
                # Remove from fallback queue
                removed = FALLBACK_QUEUE.pop(index)
                prompt_text = removed["prompt"][:50]
        
        # Removal success message
        remove_panel = Panel(
            f"[bold #00ff9d]Item removed from queue[/bold #00ff9d]\n\n"
            f"[dim]Removed: {prompt_text}{'...' if len(prompt_text) == 50 else ''}[/dim]",
            title=_quick_guide_subpanel_title("Queue"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 2),
        )
        console.print(remove_panel)
        return True

    def _clear_queue(self) -> bool:
        """Clear the entire queue"""
        count = 0
        
        # Try to get the TUI queue dynamically
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
                count = len(TUI_QUEUE._queue)
                TUI_QUEUE._queue.clear()
            except ImportError:
                pass
        
        # Fallback to module-level queue if needed
        if count == 0:
            if TUI_PROMPT_QUEUE:
                count = len(TUI_PROMPT_QUEUE._queue)
                TUI_PROMPT_QUEUE._queue.clear()
            else:
                count = len(FALLBACK_QUEUE)
                FALLBACK_QUEUE.clear()
        
        # Clear success message
        clear_panel = Panel(
            f"[bold red]Queue cleared[/bold red]\n\n"
            f"[white]Removed [bold]{count}[/bold] item{'s' if count != 1 else ''} from the queue[/white]",
            border_style="red",
            padding=(1, 2),
        )
        console.print(clear_panel)
        return True

    def _get_next(self) -> bool:
        """Get the next prompt from the queue"""
        queue_items = self._get_queue_items()
        
        if not queue_items:
            # Empty queue message
            empty_panel = Panel(
                "[dim italic]No prompts in queue[/dim italic]",
                title=_quick_guide_subpanel_title("Queue Status"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 2),
            )
            console.print(empty_panel)
            return True
        
        next_item = queue_items[0]
        
        # Format the next item display
        timestamp = next_item.get("timestamp", datetime.now())
        time_str = timestamp.strftime("%H:%M:%S")

        # Create styled panel for next item
        content = f"[bold #00ff9d]Next in queue[/bold #00ff9d]\n\n"
        content += f"[bold white]{next_item['prompt']}[/bold white]\n\n"
        content += f"[dim]Added at: {time_str}[/dim]"

        next_panel = Panel(
            content,
            title=_quick_guide_subpanel_title("Next Prompt"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 2),
        )
        console.print(next_panel)
        return True

    def _show_help(self) -> None:
        """Show help for the queue command"""
        # Create a rich help panel
        help_table = Table(
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            box=None,
            padding=(0, 1),
        )

        help_table.add_column("Command", style="bold #00ff9d")
        help_table.add_column("Description", style="white")
        help_table.add_column("Example", style="dim")
        
        commands = [
            ("/queue", "Show the current queue", "/queue"),
            ("/queue show", "Show the current queue", "/queue show"),
            ("/queue add <prompt>", "Add a prompt (active agent)", "/queue add scan target"),
            (
                "/queue add --agent <name> <prompt>",
                "Add with specific agent",
                "/queue add --agent red_teamer scan target",
            ),
            ("/queue run", "Execute all queued prompts", "/queue run"),
            ("/queue remove <index>", "Remove prompt at index", "/queue remove 2"),
            ("/queue move <from> <to>", "Move prompt to new position", "/queue move 3 1"),
            ("/queue clear", "Clear all prompts", "/queue clear"),
            ("/queue next", "Show the next prompt", "/queue next"),
            ("/queue load <file>", "Load prompts from file", "/queue load prompts.txt"),
        ]
        
        for cmd, desc, example in commands:
            help_table.add_row(
                f"[bold #00ff9d]{cmd}[/bold #00ff9d]",
                desc,
                f"[dim]{example}[/dim]",
            )

        help_panel = Panel(
            help_table,
            title=_quick_guide_subpanel_title("Queue Command Help"),
            title_align="left",
            subtitle="[dim]Alias: /que[/dim]",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
        
        console.print(help_panel)
    
    def _handle_terminal_queues(self, args: Optional[List[str]] = None) -> bool:
        """Handle per-terminal queue commands"""
        if not args or args[0].lower() in ["show", "list", "ls", "status"]:
            return self._show_terminal_queues()
        
        action = args[0].lower()
        
        if action == "clear":
            if len(args) > 1:
                try:
                    terminal_num = int(args[1])
                    return self._clear_terminal_queue(terminal_num)
                except ValueError:
                    error_panel = Panel(
                        "[bold red]Invalid terminal number[/bold red]",
                        border_style="red",
                    )
                    console.print(error_panel)
                    return False
            else:
                return self._clear_all_terminal_queues()
        elif action == "help":
            self._show_terminal_queue_help()
            return True
        else:
            # For other actions, fall back to regular queue handling
            return self._show_queue()
    
    def _show_terminal_queues(self) -> bool:
        """Show status of all terminal queues"""
        all_status = TERMINAL_QUEUE_MANAGER.get_all_queues_status()
        
        if not all_status:
            success_panel = Panel(
                "[#9aa0a6]No terminal queues active[/]",
                title=_quick_guide_subpanel_title("Terminal Queue Status"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
            console.print(success_panel)
            return True
        
        # Create main table
        table = Table(
            title="[bold #00ff9d]Terminal Queue Status[/bold #00ff9d]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            row_styles=["none", "#9aa0a6"],
            box=None,
        )

        table.add_column("Terminal", style="bold #00ff9d", width=10)
        table.add_column("Status", style="white", width=12)
        table.add_column("Current", style="white", overflow="ellipsis")
        table.add_column("Queued", style="#9aa0a6", width=8, justify="center")
        
        for terminal_num in sorted(all_status.keys()):
            status = all_status[terminal_num]
            
            # Status icon and text
            if status["processing"]:
                status_text = "[yellow]Processing[/yellow]"
                current_text = status["current_prompt"][:40] + "..." if status["current_prompt"] and len(status["current_prompt"]) > 40 else status["current_prompt"] or ""
            else:
                status_text = "[#9aa0a6]Idle[/]"
                current_text = "[dim]-[/dim]"
            
            queue_len = status["queue_length"]
            queue_text = f"[bold #00ff9d]{queue_len}[/bold #00ff9d]" if queue_len > 0 else "[dim]0[/dim]"
            
            table.add_row(
                f"[bold #00ff9d]T{terminal_num}[/bold #00ff9d]",
                status_text,
                current_text,
                queue_text,
            )
        
        console.print(table)
        console.print()
        
        # Show detailed queue items if any
        has_queued_items = False
        for terminal_num in sorted(all_status.keys()):
            status = all_status[terminal_num]
            if status["prompts"]:
                has_queued_items = True
                console.print(f"[bold]Terminal {terminal_num} Queue:[/bold]")
                for i, prompt_info in enumerate(status["prompts"], 1):
                    console.print(f"  {i}. {prompt_info['prompt']} [dim](priority: {prompt_info['priority']})[/dim]")
                console.print()
        
        if not has_queued_items:
            console.print("[dim]No prompts queued in any terminal[/dim]")
        
        console.print(
            "[#9aa0a6]Tip: Each terminal has its own independent queue[/]"
        )
        return True
    
    def _clear_terminal_queue(self, terminal_num: int) -> bool:
        """Clear queue for specific terminal"""
        count = TERMINAL_QUEUE_MANAGER.clear_terminal_queue(terminal_num)
        if count > 0:
            success_panel = Panel(
                f"[bold #00ff9d]Cleared {count} prompt(s) from terminal {terminal_num} queue[/bold #00ff9d]",
                title=_quick_guide_subpanel_title("Terminal Queue"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        else:
            success_panel = Panel(
                f"[yellow]Terminal {terminal_num} queue was already empty[/yellow]",
                title=_quick_guide_subpanel_title("Terminal Queue"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        console.print(success_panel)
        return True
    
    def _clear_all_terminal_queues(self) -> bool:
        """Clear all terminal queues"""
        count = TERMINAL_QUEUE_MANAGER.clear_all_queues()
        if count > 0:
            success_panel = Panel(
                f"[bold #00ff9d]Cleared {count} prompt(s) from all terminal queues[/bold #00ff9d]",
                title=_quick_guide_subpanel_title("Terminal Queues"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        else:
            success_panel = Panel(
                "[yellow]All terminal queues were already empty[/yellow]",
                title=_quick_guide_subpanel_title("Terminal Queues"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        console.print(success_panel)
        return True
    
    def _show_terminal_queue_help(self) -> None:
        """Show help for terminal queue commands"""
        help_table = Table(
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            box=None,
        )

        help_table.add_column("Command", style="bold #00ff9d")
        help_table.add_column("Description", style="white")
        
        commands = [
            ("/queue", "Show terminal queue status"),
            ("/queue status", "Show terminal queue status"),
            ("/queue clear", "Clear all terminal queues"),
            ("/queue clear <num>", "Clear specific terminal queue"),
            ("/queue help", "Show this help message")
        ]
        
        for cmd, desc in commands:
            help_table.add_row(cmd, desc)
        
        help_panel = Panel(
            help_table,
            title=_quick_guide_subpanel_title("Terminal Queue Commands"),
            title_align="left",
            subtitle="[dim]Per-terminal queue management[/dim]",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
        
        console.print(help_panel)
        console.print("\n[dim]Each terminal has its own independent queue that processes commands sequentially.[/dim]")


# Register the command
register_command(QueueCommand())


def get_queue():
    """Get the current queue"""
    # Try to get the TUI queue dynamically
    if os.getenv("CAI_TUI_MODE") == "true":
        try:
            from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
            # Return TUI queue items as dict format
            items = []
            for queued_prompt in TUI_QUEUE._queue:
                items.append({
                    "prompt": queued_prompt.prompt,
                    "timestamp": queued_prompt.timestamp,
                })
            return items
        except ImportError:
            pass
    
    # Fallback to module-level queue
    if TUI_PROMPT_QUEUE:
        # Return TUI queue items as dict format
        items = []
        for queued_prompt in TUI_PROMPT_QUEUE._queue:
            items.append({
                "prompt": queued_prompt.prompt,
                "timestamp": queued_prompt.timestamp,
            })
        return items
    else:
        return FALLBACK_QUEUE


def add_to_queue(prompt: str, agent: Optional[str] = None):
    """Add a prompt to the queue.

    Args:
        prompt: The prompt text to enqueue.
        agent: Optional agent key to associate with this prompt.
    """
    if os.getenv("CAI_TUI_MODE") == "true":
        try:
            from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
            import asyncio
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    loop.create_task(TUI_QUEUE.add_prompt(prompt))
                else:
                    loop.run_until_complete(TUI_QUEUE.add_prompt(prompt))
            except RuntimeError:
                pass
            return len(TUI_QUEUE._queue)
        except ImportError:
            pass

    if TUI_PROMPT_QUEUE:
        import asyncio
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                loop.create_task(TUI_PROMPT_QUEUE.add_prompt(prompt))
            else:
                loop.run_until_complete(TUI_PROMPT_QUEUE.add_prompt(prompt))
        except RuntimeError:
            pass
        return len(TUI_PROMPT_QUEUE._queue)
    else:
        item = {
            "prompt": prompt,
            "timestamp": datetime.now(),
            "agent": agent,
        }
        FALLBACK_QUEUE.append(item)
        return len(FALLBACK_QUEUE)


def get_next_prompt():
    """Get and remove the next prompt from the queue.

    Returns:
        dict with keys "prompt" (str) and "agent" (str | None),
        a plain str for TUI queue items, or None when empty.
    """
    if os.getenv("CAI_TUI_MODE") == "true":
        try:
            from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE
            if TUI_QUEUE._queue:
                queued_item = TUI_QUEUE._queue.pop(0)
                return queued_item.prompt
        except ImportError:
            pass

    if TUI_PROMPT_QUEUE and TUI_PROMPT_QUEUE._queue:
        queued_item = TUI_PROMPT_QUEUE._queue.pop(0)
        return queued_item.prompt if hasattr(queued_item, 'prompt') else queued_item
    elif FALLBACK_QUEUE:
        item = FALLBACK_QUEUE.pop(0)
        if isinstance(item, dict):
            return {"prompt": item.get("prompt", ""), "agent": item.get("agent")}
        return item
    return None


def is_queue_empty():
    """Check if the queue is empty"""
    if TUI_PROMPT_QUEUE:
        return len(TUI_PROMPT_QUEUE._queue) == 0
    else:
        return len(FALLBACK_QUEUE) == 0


def load_queue_from_file(file_path: str) -> int:
    """Load prompts from a text file into the queue
    
    Args:
        file_path: Path to the text file containing prompts (one per line)
        
    Returns:
        Number of prompts loaded
    """
    loaded_count = 0
    
    try:
        prompts_to_load = []
        with open(file_path, 'r', encoding='utf-8') as f:
            for line in f:
                # Strip whitespace and skip empty lines
                prompt = line.strip()
                if prompt and not prompt.startswith('#'):  # Skip comments
                    prompts_to_load.append(prompt)
                    
        # Add all prompts to the appropriate queue
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.core.prompt_queue import PROMPT_QUEUE as TUI_QUEUE, QueuedPrompt
                # Add directly to the TUI queue without async
                for prompt in prompts_to_load:
                    queued_prompt = QueuedPrompt(
                        prompt=prompt,
                        terminal_number=None,
                        priority=0
                    )
                    TUI_QUEUE._queue.append(queued_prompt)
                    loaded_count += 1
                
                # Trigger processing if not already running
                if loaded_count > 0 and not TUI_QUEUE._processing:
                    import asyncio
                    try:
                        # If there's a running event loop, create a task
                        loop = asyncio.get_running_loop()
                        loop.create_task(TUI_QUEUE._process_queue())
                    except RuntimeError:
                        # No running event loop yet, processing will start when TUI is ready
                        pass
            except ImportError:
                # Fallback to regular add
                for prompt in prompts_to_load:
                    add_to_queue(prompt)
                    loaded_count += 1
        else:
            # Non-TUI mode
            for prompt in prompts_to_load:
                add_to_queue(prompt)
                loaded_count += 1
                    
        console.print(
            f"[bold #00ff9d]Loaded {loaded_count} prompts from {file_path}[/bold #00ff9d]"
        )
    except FileNotFoundError:
        console.print(f"[red]Queue file not found: {file_path}[/red]")
    except Exception as e:
        console.print(f"[red]Error loading queue file: {e}[/red]")
        
    return loaded_count


def load_queue_from_env():
    """Load queue from file specified in CAI_QUEUE_FILE environment variable"""
    queue_file = os.getenv("CAI_QUEUE_FILE")
    
    if queue_file:
        # Expand user home directory if needed
        queue_file = os.path.expanduser(queue_file)
        
        if os.path.exists(queue_file):
            loaded = load_queue_from_file(queue_file)
            if loaded > 0:
                console.print(
                    f"[#9aa0a6]Auto-loaded {loaded} prompts from CAI_QUEUE_FILE[/]"
                )
        else:
            console.print(
                f"[yellow]CAI_QUEUE_FILE set but file not found: {queue_file}[/yellow]"
            )


# Note: Auto-loading is handled by the main CLI and TUI on startup
# to avoid circular imports and ensure proper initialization