"""
Memory command for CAI REPL.
Manages memory storage in .cai/memory for persistent context.
"""

from typing import List, Optional, Dict, Any
import os
import asyncio
import json
import datetime
from pathlib import Path
from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.tree import Tree

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title
from cai.sdk.agents.models.openai_chatcompletions import (
    get_all_agent_histories,
    get_agent_message_history,
    OpenAIChatCompletionsModel,
    get_current_active_model,
    ACTIVE_MODEL_INSTANCES,
    PERSISTENT_MESSAGE_HISTORIES,
)
from cai.sdk.agents import Agent, Runner
from cai.repl.commands.parallel import PARALLEL_CONFIGS
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER, DEFAULT_SESSION_AGENT_ID
from openai import AsyncOpenAI


def _async_openai_client_for_memory_summary(model_name: str) -> AsyncOpenAI:
    """Build the same style of AsyncOpenAI client as interactive agents (Azure base URL, keys).

    `/memory save` and related flows used to always use ``OPENAI_API_KEY`` only, ignoring
    ``OPENAI_API_BASE`` / Azure env vars and the global default client — which breaks or
    hangs against Azure OpenAI while normal turns work.
    """
    from cai.config import get_config
    from cai.sdk.agents.models import _openai_shared
    from cai.sdk.agents.models.openai_provider import OpenAIProvider
    from cai.util.llm_api_base import model_qualifies_for_alias_api_url

    existing = _openai_shared.get_default_openai_client()
    if existing is not None:
        return existing

    cfg = get_config()
    base_raw = (os.getenv("OPENAI_API_BASE") or "").strip() or None
    if not base_raw:
        base_raw = (os.getenv("AZURE_API_BASE") or os.getenv("AZURE_OPENAI_ENDPOINT") or "").strip() or None

    if model_qualifies_for_alias_api_url(model_name):
        api_key = (cfg.alias_api_key or cfg.openai_api_key or "").strip() or None
    else:
        api_key = (
            (cfg.openai_api_key or "").strip()
            or (os.getenv("AZURE_OPENAI_API_KEY") or os.getenv("AZURE_API_KEY") or "").strip()
            or (cfg.alias_api_key or "").strip()
            or None
        )
    if not api_key:
        api_key = (os.getenv("OPENAI_API_KEY") or "").strip() or None

    provider = OpenAIProvider(api_key=api_key, base_url=base_raw)
    return provider._get_client()


# Import get_compact_model function - imported later to avoid circular import
def get_compact_model():
    try:
        from cai.repl.commands.compact import get_compact_model as _get_compact_model

        return _get_compact_model()
    except ImportError:
        return None


console = Console()

# Memory directory path - use home directory for cross-platform compatibility
MEMORY_DIR = Path.home() / ".cai" / "memory"
MEMORY_INDEX_FILE = MEMORY_DIR / "index.json"

# Global storage for compacted summaries (deprecated - use file storage)
# Now supports multiple memories per agent
COMPACTED_SUMMARIES: Dict[str, List[str]] = {}

# Global storage for memory ID mappings per agent
# Now supports multiple memory IDs per agent
APPLIED_MEMORY_IDS: Dict[str, List[str]] = {}


class MemoryCommand(Command):
    """Command for managing memory storage and application."""

    def __init__(self):
        """Initialize the memory command."""
        super().__init__(
            name="/memory", description="Manage memory storage for agents", aliases=["/mem"]
        )

        # Add subcommands
        self.add_subcommand("list", "List all stored memories", self.handle_list)
        self.add_subcommand("save", "Save current agent history as memory", self.handle_save)
        self.add_subcommand("apply", "Apply a memory to an agent", self.handle_apply)
        self.add_subcommand("show", "Show memory content", self.handle_show)
        self.add_subcommand("delete", "Delete a stored memory", self.handle_delete)
        self.add_subcommand("merge", "Merge multiple memories into one", self.handle_merge)
        self.add_subcommand("status", "Show memory status", self.handle_status)
        self.add_subcommand("compact", "Compact and save agent history", self.handle_compact)
        self.add_subcommand("remove", "Remove a specific memory from an agent", self.handle_remove)
        self.add_subcommand("clear", "Clear all memories from an agent", self.handle_clear)
        self.add_subcommand(
            "list-applied", "Show which memories are applied to an agent", self.handle_list_applied
        )

        # Remove local compact_model since we'll use the one from compact command

        # Ensure memory directory exists
        self._ensure_memory_dir()

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the memory command."""
        if not args:
            # Show control panel
            return self.handle_control_panel()

        # Check if first arg is a subcommand
        subcommand = args[0].lower()
        if subcommand in self.subcommands:
            handler = self.subcommands[subcommand]["handler"]
            return handler(args[1:] if len(args) > 1 else [])

        # Check if it's a memory ID (M001, M002, etc.) - if so, show the memory
        first_arg = args[0]
        if first_arg.upper().startswith("M") and len(first_arg) >= 4 and first_arg[1:].isdigit():
            return self.handle_show(args)

        # Otherwise show help
        console.print("[yellow]Unknown subcommand.[/yellow]")
        console.print("[#9aa0a6]Available commands:[/]")
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory list[/bold #00ff9d]"
            "[#9aa0a6]                - List all stored memories[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory save[/bold #00ff9d]"
            "[#9aa0a6]                - Save current agent history as memory[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory apply[/bold #00ff9d]"
            "[#9aa0a6]               - Apply a memory to an agent[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory show[/bold #00ff9d]"
            "[#9aa0a6]                - Show memory content[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory delete[/bold #00ff9d]"
            "[#9aa0a6]              - Delete a stored memory[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory merge[/bold #00ff9d]"
            "[#9aa0a6]               - Merge multiple memories into one[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory status[/bold #00ff9d]"
            "[#9aa0a6]              - Show memory status[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory compact[/bold #00ff9d]"
            "[#9aa0a6]             - Compact and save agent history[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory remove[/bold #00ff9d]"
            "[#9aa0a6]              - Remove a specific memory from an agent[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory clear[/bold #00ff9d]"
            "[#9aa0a6]               - Clear all memories from an agent[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory list-applied[/bold #00ff9d]"
            "[#9aa0a6]        - Show which memories are applied to an agent[/]"
        )
        return True

    def _ensure_memory_dir(self):
        """Ensure the memory directory exists."""
        MEMORY_DIR.mkdir(parents=True, exist_ok=True)

        # Initialize index file if it doesn't exist
        if not MEMORY_INDEX_FILE.exists():
            self._initialize_index()

    def _get_memory_id_by_filename(self, filename: str) -> Optional[str]:
        """Get the memory ID for a given filename."""
        index = self._load_index()
        for mem_id, mem_file in index.get("mappings", {}).items():
            if mem_file == filename:
                return mem_id
        return None

    def _get_memory_path(self, name_or_id: str) -> Path:
        """Get the path for a memory file, resolving ID if necessary."""
        # Check if it's an ID (M001, M002, etc.)
        if name_or_id.upper().startswith("M") and len(name_or_id) >= 4 and name_or_id[1:].isdigit():
            # Try to resolve ID to filename
            index = self._load_index()
            if name_or_id.upper() in index.get("mappings", {}):
                name = index["mappings"][name_or_id.upper()]
            else:
                raise ValueError(f"Memory ID '{name_or_id}' not found")
        else:
            name = name_or_id
            if not name.endswith(".md"):
                name += ".md"
        return MEMORY_DIR / name

    def _resolve_agent_name(self, identifier: str) -> Optional[str]:
        """Resolve an agent identifier (name or ID) to the actual agent name."""
        # Check if it's an ID (P1, P2, etc.)
        if identifier.upper().startswith("P") and len(identifier) >= 2 and identifier[1:].isdigit():
            agent_id = identifier.upper()

            # First check parallel configs if they exist - they are the authoritative source
            if PARALLEL_CONFIGS:
                from cai.agents import get_available_agents

                available_agents = get_available_agents()

                for config in PARALLEL_CONFIGS:
                    if config.id and config.id.upper() == agent_id:
                        # Special handling for patterns
                        if config.agent_name.endswith("_pattern"):
                            # For patterns, we need to get the actual entry agent
                            from cai.agents.patterns import get_pattern

                            pattern = get_pattern(config.agent_name)
                            if pattern:
                                if hasattr(pattern, "entry_agent"):
                                    # For swarm patterns like red_team_pattern
                                    agent = pattern.entry_agent
                                    display_name = getattr(agent, "name", config.agent_name)
                                elif hasattr(pattern, "name"):
                                    # For the pattern itself
                                    display_name = getattr(pattern, "name", config.agent_name)
                                else:
                                    display_name = config.agent_name
                            else:
                                display_name = config.agent_name
                        elif config.agent_name in available_agents:
                            agent = available_agents[config.agent_name]
                            display_name = getattr(agent, "name", config.agent_name)
                        else:
                            display_name = config.agent_name

                        # Count instances for proper naming
                        total_count = sum(
                            1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name
                        )
                        if total_count > 1:
                            # Find instance number
                            instance_num = 0
                            for c in PARALLEL_CONFIGS:
                                if c.agent_name == config.agent_name:
                                    instance_num += 1
                                    if c.id == config.id:
                                        break
                            return f"{display_name} #{instance_num}"
                        else:
                            return display_name

            # Fall back to AGENT_MANAGER if no parallel configs or not found
            agent_name = AGENT_MANAGER.get_agent_by_id(agent_id)
            if agent_name:
                return agent_name

        # Otherwise it's a direct agent name
        return identifier

    def _initialize_index(self):
        """Initialize the memory index file with existing memories."""
        index = {"next_id": 1, "mappings": {}}

        # Scan existing memory files and assign IDs
        existing_files = sorted(MEMORY_DIR.glob("*.md"))
        for idx, memory_file in enumerate(existing_files, 1):
            memory_id = f"M{idx:03d}"
            index["mappings"][memory_id] = memory_file.name
            index["next_id"] = idx + 1

        self._save_index(index)

    def _load_index(self) -> Dict[str, Any]:
        """Load the memory index from file."""
        if not MEMORY_INDEX_FILE.exists():
            self._initialize_index()

        try:
            with open(MEMORY_INDEX_FILE, "r") as f:
                return json.load(f)
        except Exception as e:
            console.print(f"[red]Error loading index: {e}[/red]")
            return {"next_id": 1, "mappings": {}}

    def _save_index(self, index: Dict[str, Any]):
        """Save the memory index to file."""
        try:
            with open(MEMORY_INDEX_FILE, "w") as f:
                json.dump(index, f, indent=2)
        except Exception as e:
            console.print(f"[red]Error saving index: {e}[/red]")

    def _get_next_memory_id(self) -> str:
        """Get the next available memory ID."""
        index = self._load_index()
        memory_id = f"M{index['next_id']:03d}"
        index["next_id"] += 1
        self._save_index(index)
        return memory_id

    def _register_memory(self, memory_id: str, filename: str):
        """Register a memory file with its ID in the index."""
        index = self._load_index()
        index["mappings"][memory_id] = filename
        self._save_index(index)

    def _unregister_memory(self, memory_id: str):
        """Remove a memory ID from the index."""
        index = self._load_index()
        if memory_id in index["mappings"]:
            del index["mappings"][memory_id]
            self._save_index(index)

    def handle_control_panel(self) -> bool:
        """Show a control panel view of memory status."""
        console.print(
            Panel(
                "[bold white]Memory management[/bold white]\n\n"
                "[#9aa0a6]Stored snapshots and applied memories for this workspace.[/]",
                title=_quick_guide_subpanel_title("Memory"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        )
        console.print()

        # Show stored memories
        memories = list(MEMORY_DIR.glob("*.md"))
        if memories:
            # Load index to get ID mappings
            index = self._load_index()
            file_to_id = {v: k for k, v in index.get("mappings", {}).items()}

            table = Table(
                title="[bold #00ff9d]Stored memories[/bold #00ff9d]",
                show_header=True,
                header_style="bold white",
                border_style=_CAI_GREEN,
                row_styles=["none", "#9aa0a6"],
                box=None,
            )
            table.add_column("ID", style="bold #00ff9d", width=6)
            table.add_column("Name", style="white")
            table.add_column("Agent", style="#9aa0a6")
            table.add_column("Size", style="white")
            table.add_column("Modified", style="#9aa0a6")

            for memory_file in sorted(memories):
                memory_id = file_to_id.get(memory_file.name, "---")

                # Try to extract agent name from file
                agent_name = "Unknown"
                try:
                    content = memory_file.read_text()
                    for line in content.split("\n"):
                        if line.startswith("Agent: "):
                            agent_name = line[7:]
                            break
                except:
                    pass

                size = memory_file.stat().st_size
                modified = datetime.datetime.fromtimestamp(memory_file.stat().st_mtime)
                table.add_row(
                    memory_id,
                    memory_file.stem,
                    agent_name,
                    f"{size:,} bytes",
                    modified.strftime("%Y-%m-%d %H:%M"),
                )

            console.print(table)
        else:
            console.print("[yellow]No memories stored yet[/yellow]")

        # Show applied memories
        if APPLIED_MEMORY_IDS:
            console.print("\n[bold white]Applied memories[/bold white]")
            for agent_name, memory_ids in APPLIED_MEMORY_IDS.items():
                if isinstance(memory_ids, list):
                    ids_str = ", ".join(memory_ids) if memory_ids else "None"
                    console.print(f"  • {agent_name}: [{ids_str}]")
                else:
                    # Backward compatibility for single memory ID
                    console.print(f"  • {agent_name}: {memory_ids}")

        # Show usage hints
        console.print("\n[#9aa0a6]Commands:[/]")
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory list[/bold #00ff9d]"
            "[#9aa0a6]                - List all stored memories[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory save <name>[/bold #00ff9d]"
            "[#9aa0a6]         - Save current agent as memory[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory apply <ID/name>[/bold #00ff9d]"
            "[#9aa0a6]     - Apply memory to P0 session agent (default)[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory apply <ID> all[/bold #00ff9d]"
            "[#9aa0a6]      - Apply to all active agents[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory show <ID/name>[/bold #00ff9d]"
            "[#9aa0a6]      - View memory content[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory delete <ID/name>[/bold #00ff9d]"
            "[#9aa0a6]    - Delete a memory[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory merge <ID1> <ID2>[/bold #00ff9d]"
            "[#9aa0a6]   - Merge multiple memories[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory compact <agent>[/bold #00ff9d]"
            "[#9aa0a6]     - Compact agent history to memory[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory remove <ID> <agent>[/bold #00ff9d]"
            "[#9aa0a6] - Remove a specific memory from agent[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory clear <agent>[/bold #00ff9d]"
            "[#9aa0a6]       - Clear all memories from agent[/]"
        )
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory list-applied[/bold #00ff9d]"
            "[#9aa0a6]        - Show applied memories by agent[/]"
        )
        console.print(
            "\n[#9aa0a6]Note: You can use memory IDs (e.g., M001) instead of full names.[/]"
        )
        console.print("[#9aa0a6]Agents support multiple memories.[/]")

        return True

    def handle_list(self, args: Optional[List[str]] = None) -> bool:
        """List all stored memories."""
        memories = list(MEMORY_DIR.glob("*.md"))

        if not memories:
            console.print("[yellow]No memories stored yet[/yellow]")
            console.print(
                "[dim]No saved memory snapshots were found in this workspace.[/dim]"
            )
            console.print(
                "[dim]To create one: chat with an agent first, then run "
                "'/memory save <name>' (or '/memory save' for auto-name).[/dim]"
            )
            console.print(
                "[dim]Tip: if '/memory save' says no history, send at least one non-command prompt first.[/dim]"
            )
            return True

        # Load index to get ID mappings
        index = self._load_index()
        id_to_file = index.get("mappings", {})
        file_to_id = {v: k for k, v in id_to_file.items()}

        # Create a table showing all memories
        table = Table(
            title="[bold #00ff9d]Stored Memories[/bold #00ff9d]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            row_styles=["none", "#9aa0a6"],
            box=None,
        )
        table.add_column("ID", style="bold #00ff9d", width=6)
        table.add_column("Name", style="white")
        table.add_column("Agent", style="#9aa0a6")
        table.add_column("Size", style="white")
        table.add_column("Created", style="#9aa0a6")

        for memory_file in sorted(memories):
            # Get ID for this memory
            memory_id = file_to_id.get(memory_file.name, "---")

            # Try to extract agent name from file
            content = memory_file.read_text()
            agent_name = "Unknown"
            created = "Unknown"

            # Parse metadata from memory file
            for line in content.split("\n"):
                if line.startswith("Agent: "):
                    agent_name = line[7:]
                elif line.startswith("Generated: "):
                    created = line[11:]
                if agent_name != "Unknown" and created != "Unknown":
                    break

            size = memory_file.stat().st_size
            table.add_row(memory_id, memory_file.stem, agent_name, f"{size:,} bytes", created)

        console.print(table)
        console.print("\n[#9aa0a6][CAI] Commands:[/]")
        console.print("[#9aa0a6]  • [/][bold #00ff9d]/memory show <ID/name>[/bold #00ff9d][#9aa0a6] - View memory content[/]")
        console.print(
            "[#9aa0a6]  • [/][bold #00ff9d]/memory apply <ID/name>[/bold #00ff9d]"
            "[#9aa0a6] - Apply memory to P0 session agent (default)[/]"
        )
        console.print("[#9aa0a6]  • [/][bold #00ff9d]/memory apply <ID/name> all[/bold #00ff9d][#9aa0a6] - Apply to all active agents[/]")
        console.print("[#9aa0a6]  • [/][bold #00ff9d]/memory delete <ID/name>[/bold #00ff9d][#9aa0a6] - Delete a memory[/]")
        console.print("[#9aa0a6]  • [/][bold #00ff9d]/memory merge <ID1> <ID2>[/bold #00ff9d][#9aa0a6] - Merge multiple memories[/]")
        console.print(
            "[#9aa0a6]\n[CAI] Note: You can use either the memory ID (e.g., M001) or the full name.[/]"
        )

        return True

    def handle_save(self, args: Optional[List[str]] = None, preserve_history: bool = True) -> bool:
        """Save current agent history as memory."""
        if os.getenv("CAI_DEBUG") == "1":
            console.print(f"[dim]Debug handle_save: args={args}, preserve_history={preserve_history}[/dim]")
        
        if not args:
            # Use current active agent
            agent_name = self._get_current_agent_name()
            if not agent_name:
                console.print("[red]Error: No active agent found[/red]")
                console.print("Usage: /memory save <memory_name> [agent_name]")
                return False
            memory_name = f"{agent_name.replace(' ', '_')}_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}"
        else:
            memory_name = args[0]
            if len(args) > 1:
                agent_identifier = " ".join(args[1:])
                agent_name = self._resolve_agent_name(agent_identifier)
            else:
                agent_name = self._get_current_agent_name()
                if not agent_name:
                    console.print("[red]Error: No active agent found[/red]")
                    return False

        # In TUI mode, we need to get history from the terminal runner
        history = []
        if os.getenv("CAI_TUI_MODE") == "true" and "(" in agent_name and ")" in agent_name:
            # Extract terminal number from agent name like "Agent Name (T1)"
            terminal_num = None
            if "(T" in agent_name and ")" in agent_name:
                start = agent_name.rfind("(T") + 2
                end = agent_name.find(")", start)
                if end > start:
                    terminal_num = agent_name[start:end]
            
            if terminal_num and terminal_num.isdigit():
                # In TUI, history is stored with P-ID (P1, P2, etc.)
                p_id = f"P{terminal_num}"
                if p_id in AGENT_MANAGER._message_history:
                    history = AGENT_MANAGER._message_history[p_id]
                    if os.getenv("CAI_DEBUG") == "1":
                        console.print(f"[dim]Found {len(history)} messages for {p_id}[/dim]")
                else:
                    # Fallback: try to get from terminal runner
                    try:
                        from cai.tui.core.session_manager import SessionManager
                        session_manager = SessionManager.get_instance()
                        
                        if session_manager:
                            terminal_runner = session_manager.terminal_runners.get(int(terminal_num))
                            if terminal_runner and terminal_runner.agent:
                                if hasattr(terminal_runner.agent, 'model') and hasattr(terminal_runner.agent.model, 'message_history'):
                                    history = terminal_runner.agent.model.message_history
                                    if os.getenv("CAI_DEBUG") == "1":
                                        console.print(f"[dim]Found {len(history)} messages from terminal {terminal_num} runner[/dim]")
                    except Exception as e:
                        if os.getenv("CAI_DEBUG") == "1":
                            console.print(f"[dim]Error getting terminal runner: {e}[/dim]")
        
        # If not found via terminal runner, try standard approach
        if not history:
            history = get_agent_message_history(agent_name)

        if not history:
            console.print(f"[yellow]No history found for agent '{agent_name}'[/yellow]")
            console.print(
                "[dim]Memory saves require existing conversation history for that agent.[/dim]"
            )
            console.print(
                "[dim]Next steps: (1) select/use the agent, (2) send one or more prompts, "
                "(3) run '/memory save <name>' again.[/dim]"
            )
            return True

        console.print(
            f"\n[#9aa0a6][CAI] Saving memory for [/][bold #00ff9d]{agent_name}[/bold #00ff9d][#9aa0a6]...[/]"
        )

        # Generate summary
        summary = self._run_async_in_sync(self._ai_summarize_history(agent_name))

        if summary:
            # Generate unique ID for this memory
            memory_id = self._get_next_memory_id()

            # Ensure memory_name has .md extension
            if not memory_name.endswith(".md"):
                memory_name += ".md"

            memory_path = MEMORY_DIR / memory_name

            # Create memory content with metadata including ID
            # For TUI mode, save with base agent name
            saved_agent_name = agent_name
            if os.getenv("CAI_TUI_MODE") == "true" and " (T" in agent_name:
                saved_agent_name = agent_name.split(" (T")[0]
                
            memory_content = f"""# Memory: {memory_name}
ID: {memory_id}
Generated: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
Agent: {saved_agent_name}
Model: {get_compact_model() or os.environ.get("CAI_MODEL", "gpt-4")}

{summary}

## Metadata
- Original messages: {len(history)}
- Saved by: User request
"""

            memory_path.write_text(memory_content)

            # Register the memory in the index
            self._register_memory(memory_id, memory_name)

            console.print(f"[green]✓ Saved memory: {memory_name} (ID: {memory_id})[/green]")

            # For TUI mode, we need to store memory with base agent name (without terminal suffix)
            memory_agent_key = agent_name
            base_agent_name = agent_name
            
            # Extract base agent name without terminal suffix for TUI mode
            if os.getenv("CAI_TUI_MODE") == "true" and " (T" in agent_name:
                # Remove terminal suffix like " (T1)"
                base_agent_name = agent_name.split(" (T")[0]
                memory_agent_key = base_agent_name
                
                if os.getenv("CAI_DEBUG") == "1":
                    console.print(f"[dim]TUI Mode: Using base agent name '{base_agent_name}' for memory[/dim]")
            
            # Automatically apply the memory to the agent's system prompt
            if memory_agent_key not in COMPACTED_SUMMARIES:
                COMPACTED_SUMMARIES[memory_agent_key] = []
                APPLIED_MEMORY_IDS[memory_agent_key] = []

            # Clear existing memories and add new one (maintain single memory behavior for save)
            COMPACTED_SUMMARIES[memory_agent_key] = [summary]
            APPLIED_MEMORY_IDS[memory_agent_key] = [memory_id]
            console.print(
                f"[green]✓ Memory {memory_id} automatically applied to {base_agent_name}'s system prompt[/green]"
            )
            os.environ["CAI_COMPACTED_MEMORY"] = "true"

            # Reload the agent with the new memory
            # In TUI mode, we don't reload immediately as each terminal manages its own instance
            if os.getenv("CAI_TUI_MODE") != "true":
                self._reload_agent_with_memory(agent_name, preserve_history=preserve_history)
            else:
                # Just ensure the memory is available for future loads
                console.print(f"[dim]Memory will be applied when agents are reloaded[/dim]")

            # Show memory panel
            console.print(
                Panel(
                    summary[:500] + "..." if len(summary) > 500 else summary,
                    title=_quick_guide_subpanel_title(f"{memory_name} ({memory_id})"),
                    title_align="left",
                    border_style=_CAI_GREEN,
                    padding=(1, 1),
                )
            )
            return True

        console.print(f"[red]✗ Failed to save memory[/red]")
        # Must be False so /compact does not clear history when summarization failed (e.g. 502).
        return False

    def handle_apply(self, args: Optional[List[str]] = None) -> bool:
        """Apply a memory to an agent by injecting it into the system prompt."""
        if not args:
            console.print("[red]Error: Memory ID or name required[/red]")
            console.print("Usage: /memory apply <memory_id_or_name> [agent_name|all]")
            console.print(
                "       /memory apply <memory_id_or_name>        - Applies to P0 session agent by default"
            )
            console.print(
                "       /memory apply <memory_id_or_name> all    - Applies to all active agents"
            )
            return False

        memory_identifier = args[0]

        try:
            memory_path = self._get_memory_path(memory_identifier)
        except ValueError as e:
            console.print(f"[red]Error: {e}[/red]")
            return False

        if not memory_path.exists():
            console.print(f"[red]Error: Memory '{memory_identifier}' not found[/red]")
            return False

        # Determine target agent(s)
        target_agents = []

        if len(args) > 1:
            agent_identifier = " ".join(args[1:])

            # Check if user wants to apply to all agents
            if agent_identifier.lower() == "all":
                # Get all active agents
                from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

                active_agents = AGENT_MANAGER.get_active_agents()

                if not active_agents:
                    console.print("[yellow]No active agents found[/yellow]")
                    return False

                # Apply to all active agents
                for agent_name, agent_id in active_agents.items():
                    target_agents.append(agent_name)

                console.print(
                    f"[#9aa0a6]Applying memory to {len(target_agents)} agents...[/]"
                )
            else:
                # Specific agent requested
                agent_name = self._resolve_agent_name(agent_identifier)
                if agent_name:
                    target_agents.append(agent_name)
                else:
                    console.print(f"[red]Error: Could not resolve agent '{agent_identifier}'[/red]")
                    return False
        else:
            # No agent specified - default to session primary (P0)
            # Try to get the session-primary agent
            p0_agent_name = AGENT_MANAGER.get_agent_by_id(DEFAULT_SESSION_AGENT_ID)
            if p0_agent_name:
                target_agents.append(p0_agent_name)
                console.print(
                    f"[dim]No agent specified, applying to {DEFAULT_SESSION_AGENT_ID} "
                    f"({p0_agent_name}) by default[/dim]"
                )
            else:
                # Fallback to current active agent
                agent_name = self._get_current_agent_name()
                if agent_name:
                    target_agents.append(agent_name)
                else:
                    console.print(f"[red]Error: No {DEFAULT_SESSION_AGENT_ID} agent found[/red]")
                    console.print(
                        "[dim]Specify an agent name or use 'all' to apply to all agents[/dim]"
                    )
                    return False

        # Read memory content - just use the entire content without filtering
        memory_content = memory_path.read_text()

        # Use the entire memory content as the summary
        summary = memory_content.strip()

        if not summary:
            console.print(f"[red]Error: Memory file is empty[/red]")
            return False

        # Get memory ID from the path or identifier
        memory_id = None
        if memory_identifier.upper().startswith("M") and memory_identifier[1:].isdigit():
            memory_id = memory_identifier.upper()
        else:
            # Try to find ID from index
            index = self._load_index()
            for mid, mfile in index.get("mappings", {}).items():
                if mfile == memory_path.name:
                    memory_id = mid
                    break

        # Apply memory to each target agent
        success_count = 0
        for agent_name in target_agents:
            try:
                # Initialize lists if not present
                if agent_name not in COMPACTED_SUMMARIES:
                    COMPACTED_SUMMARIES[agent_name] = []
                    APPLIED_MEMORY_IDS[agent_name] = []

                # Check if memory already applied
                if memory_id and memory_id in APPLIED_MEMORY_IDS[agent_name]:
                    console.print(
                        f"[yellow]Memory {memory_id} already applied to {agent_name}[/yellow]"
                    )
                    continue

                # Append memory (supports multiple memories)
                COMPACTED_SUMMARIES[agent_name].append(summary)

                # Store the memory ID for this agent
                if memory_id:
                    APPLIED_MEMORY_IDS[agent_name].append(memory_id)
                    console.print(f"[green]✓ Applied memory {memory_id} to {agent_name}[/green]")
                else:
                    console.print(
                        f"[green]✓ Applied memory '{memory_identifier}' to {agent_name}[/green]"
                    )

                # Reload the agent to apply the memory to system prompt
                self._reload_agent_with_memory(agent_name)
                success_count += 1

            except Exception as e:
                console.print(f"[red]Error applying memory to {agent_name}: {e}[/red]")

        if success_count > 0:
            os.environ["CAI_COMPACTED_MEMORY"] = "true"
            console.print("[dim]The memory will be included in the agents' system prompts[/dim]")

            # Show summary with ID if available (only once)
            applied_title = f"Applied{f' {memory_id}' if memory_id else ''}"
            console.print(
                Panel(
                    summary[:300] + "..." if len(summary) > 300 else summary,
                    title=_quick_guide_subpanel_title(applied_title),
                    title_align="left",
                    border_style=_CAI_GREEN,
                    padding=(1, 1),
                )
            )

            if len(target_agents) > 1:
                console.print(
                    f"\n[bold green]Successfully applied memory to {success_count}/{len(target_agents)} agents[/bold green]"
                )
        else:
            console.print(f"[red]Failed to apply memory to any agents[/red]")

        return True

    def _run_async_in_sync(self, coro):
        """Run async coroutine in sync context, handling existing event loops."""
        import concurrent.futures
        import threading
        
        try:
            # Check if there's a running loop
            asyncio.get_running_loop()
            # There is a running loop, execute in a thread
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(asyncio.run, coro)
                return future.result()
        except RuntimeError:
            # No running loop, safe to use asyncio.run directly
            return asyncio.run(coro)

    def handle_show(self, args: Optional[List[str]] = None) -> bool:
        """Show memory content."""
        if not args:
            console.print("[red]Error: Memory ID or name required[/red]")
            console.print("Usage: /memory show <memory_id_or_name>")
            return False

        memory_identifier = args[0]

        try:
            memory_path = self._get_memory_path(memory_identifier)
        except ValueError as e:
            console.print(f"[red]Error: {e}[/red]")
            return False

        if not memory_path.exists():
            console.print(f"[red]Error: Memory '{memory_identifier}' not found[/red]")
            return False

        # Read and display memory content
        content = memory_path.read_text()

        # Extract ID from content if present
        memory_id = None
        for line in content.split("\n"):
            if line.startswith("ID: "):
                memory_id = line[4:]
                break

        stem = memory_path.stem
        label = f"{stem} ({memory_id})" if memory_id else stem
        console.print(
            Panel(
                content,
                title=_quick_guide_subpanel_title(label),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        )

        return True

    def handle_delete(self, args: Optional[List[str]] = None) -> bool:
        """Delete a stored memory."""
        if not args:
            console.print("[red]Error: Memory ID or name required[/red]")
            console.print("Usage: /memory delete <memory_id_or_name>")
            return False

        memory_identifier = args[0]

        try:
            memory_path = self._get_memory_path(memory_identifier)
        except ValueError as e:
            console.print(f"[red]Error: {e}[/red]")
            return False

        if not memory_path.exists():
            console.print(f"[red]Error: Memory '{memory_identifier}' not found[/red]")
            return False

        # Get the memory ID if we used a name
        index = self._load_index()
        memory_id = None
        for mid, fname in index.get("mappings", {}).items():
            if fname == memory_path.name:
                memory_id = mid
                break

        # Ask for confirmation
        display_name = f"{memory_path.stem}" + (f" (ID: {memory_id})" if memory_id else "")
        confirm = console.input(f"Delete memory '{display_name}'? (y/N): ")
        if confirm.lower() == "y":
            memory_path.unlink()

            # Remove from index if it has an ID
            if memory_id:
                self._unregister_memory(memory_id)

            console.print(f"[green]✓ Deleted memory '{display_name}'[/green]")
        else:
            console.print("[dim]Cancelled[/dim]")

        return True

    def handle_merge(self, args: Optional[List[str]] = None) -> bool:
        """Merge multiple memories into one."""
        if not args or len(args) < 2:
            console.print("[red]Error: At least 2 memory IDs or names required[/red]")
            console.print("Usage: /memory merge <memory1> <memory2> [memory3...] [into:<new_name>]")
            console.print("Example: /memory merge M001 M002 M003 into:combined_memory")
            return False

        # Parse arguments - look for "into:" prefix for output name
        memory_identifiers = []
        output_name = None

        for arg in args:
            if arg.startswith("into:"):
                output_name = arg[5:]
            else:
                memory_identifiers.append(arg)

        if len(memory_identifiers) < 2:
            console.print("[red]Error: At least 2 memories required to merge[/red]")
            return False

        # Generate default output name if not provided
        if not output_name:
            output_name = f"merged_{len(memory_identifiers)}_memories_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}"

        # Load all memories
        summaries = []
        agent_names = set()
        total_messages = 0

        for identifier in memory_identifiers:
            try:
                memory_path = self._get_memory_path(identifier)
                if not memory_path.exists():
                    console.print(f"[red]Error: Memory '{identifier}' not found[/red]")
                    return False

                # Read memory content
                content = memory_path.read_text()

                # Extract summary and metadata
                summary = ""
                in_summary = False
                agent_name = None
                msg_count = 0

                for line in content.split("\n"):
                    if line.startswith("Agent: "):
                        agent_name = line[7:]
                        agent_names.add(agent_name)
                    elif "Original messages: " in line:
                        try:
                            msg_count = int(line.split("Original messages: ")[1].split()[0])
                            total_messages += msg_count
                        except:
                            pass
                    elif line.strip() == "## Summary":
                        in_summary = True
                        continue
                    elif line.strip().startswith("## ") and in_summary:
                        break
                    elif in_summary:
                        summary += line + "\n"

                if summary.strip():
                    summaries.append(f"### Memory: {identifier}\n{summary.strip()}")
                    console.print(f"[green]✓ Loaded memory '{identifier}'[/green]")
                else:
                    console.print(
                        f"[yellow]Warning: No summary found in memory '{identifier}'[/yellow]"
                    )

            except Exception as e:
                console.print(f"[red]Error loading memory '{identifier}': {e}[/red]")
                return False

        if not summaries:
            console.print("[red]Error: No valid summaries found to merge[/red]")
            return False

        # Combine summaries
        combined_summary = "\n\n".join(summaries)

        # Generate unique ID for the merged memory
        memory_id = self._get_next_memory_id()

        # Ensure output_name has .md extension
        if not output_name.endswith(".md"):
            output_name += ".md"

        memory_path = MEMORY_DIR / output_name

        # Create merged memory content
        agents_str = ", ".join(sorted(agent_names)) if agent_names else "Multiple Agents"
        memory_content = f"""# Memory: Merged Memory
ID: {memory_id}
Generated: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
Agent: {agents_str}
Model: Merged from {len(memory_identifiers)} memories

## Summary

{combined_summary}

## Metadata
- Source memories: {', '.join(memory_identifiers)}
- Total original messages: {total_messages}
- Merge date: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
"""

        memory_path.write_text(memory_content)

        # Register the memory in the index
        self._register_memory(memory_id, output_name)

        console.print(
            f"\n[bold green]✓ Successfully merged {len(memory_identifiers)} memories into '{output_name}' (ID: {memory_id})[/bold green]"
        )

        # Show merged memory panel
        console.print(
            Panel(
                combined_summary[:500] + "..." if len(combined_summary) > 500 else combined_summary,
                title=_quick_guide_subpanel_title(f"Merged {memory_id}"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        )

        # Ask if user wants to apply the merged memory
        apply = console.input("\nApply merged memory to current agent? (y/N): ")
        if apply.lower() == "y":
            agent_name = self._get_current_agent_name()
            if agent_name:
                # Initialize lists if not present
                if agent_name not in COMPACTED_SUMMARIES:
                    COMPACTED_SUMMARIES[agent_name] = []
                    APPLIED_MEMORY_IDS[agent_name] = []

                # Append the merged memory
                COMPACTED_SUMMARIES[agent_name].append(combined_summary)
                APPLIED_MEMORY_IDS[agent_name].append(memory_id)
                console.print(f"[green]✓ Applied merged memory {memory_id} to {agent_name}[/green]")
                # Reload the agent with the new memory
                self._reload_agent_with_memory(agent_name)
            else:
                console.print("[yellow]No active agent found to apply memory to[/yellow]")

        return True

    def handle_status(self, args: Optional[List[str]] = None) -> bool:
        """Show memory status."""
        console.print("\n[bold white]Memory status[/bold white]\n")

        # Show memory storage
        memories = list(MEMORY_DIR.glob("*.md"))
        console.print(f"[#9aa0a6]Stored memories:[/] [bold white]{len(memories)}[/bold white]")
        if memories:
            total_size = sum(m.stat().st_size for m in memories)
            console.print(f"Total Size: {total_size:,} bytes")

        # Show applied memories (from COMPACTED_SUMMARIES)
        if COMPACTED_SUMMARIES:
            console.print("\n[bold white]Applied memories[/bold white]")
            for agent_name, summaries in COMPACTED_SUMMARIES.items():
                memory_ids = APPLIED_MEMORY_IDS.get(agent_name, [])
                display_name = "Global" if agent_name == "__global__" else agent_name
                if isinstance(summaries, list):
                    total_chars = sum(len(s) for s in summaries)
                    ids_str = ", ".join(memory_ids) if memory_ids else "Unknown"
                    console.print(
                        f"  - {display_name}: {len(summaries)} memories, {total_chars} chars (IDs: {ids_str})"
                    )
                else:
                    # Backward compatibility
                    memory_id = memory_ids if isinstance(memory_ids, str) else "Unknown"
                    console.print(f"  - {display_name}: {len(summaries)} chars (ID: {memory_id})")
        else:
            console.print("\n[#9aa0a6]No memories currently applied[/]")

        # Show context usage for all agents
        console.print("\n[bold white]Agent context usage[/bold white]")
        all_histories = get_all_agent_histories()
        total_tokens = 0
        for agent_name, history in all_histories.items():
            if history:
                # Estimate tokens
                total_chars = sum(len(str(msg.get("content", ""))) for msg in history)
                estimated_tokens = total_chars // 4  # Rough estimate
                total_tokens += estimated_tokens
                console.print(
                    f"  - {agent_name}: ~{estimated_tokens:,} tokens ({len(history)} messages)"
                )

        if total_tokens > 0:
            console.print(f"\n[bold]Total estimated tokens: ~{total_tokens:,}[/bold]")

        return True

    def handle_compact(self, args: Optional[List[str]] = None) -> bool:
        """Compact a specific agent's history or all agents."""
        if not args:
            console.print("[red]Error: Agent name/ID or 'all' required[/red]")
            console.print("Usage: /memory compact <agent_name/ID|all>")
            return False

        if args[0].lower() == "all":
            return self._compact_all_agents()
        else:
            # Join all args to handle agent names with spaces
            agent_identifier = " ".join(args)
            return self._compact_single_agent(agent_identifier)

    def _compact_all_agents(self) -> bool:
        """Compact all agent histories."""
        all_histories = get_all_agent_histories()

        if not all_histories:
            console.print("[yellow]No agent histories to compact[/yellow]")
            return True

        # Ask for confirmation
        console.print(
            "[yellow]This will compact all agent histories and save them as memories.[/yellow]"
        )
        confirm = console.input("Continue? (y/N): ")
        if confirm.lower() != "y":
            console.print("[dim]Cancelled[/dim]")
            return True

        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

        for agent_name in all_histories:
            console.print(f"\n[#9aa0a6]Compacting {agent_name}...[/]")
            # Generate summary for this agent
            summary = self._run_async_in_sync(self._ai_summarize_history(agent_name))

            if summary:
                # Generate unique ID for this memory
                memory_id = self._get_next_memory_id()

                # Save as memory
                memory_name = f"{agent_name.replace(' ', '_').replace('#', '')}_{timestamp}.md"
                memory_path = MEMORY_DIR / memory_name

                # Create memory content with metadata including ID
                memory_content = f"""# Memory: {agent_name}
ID: {memory_id}
Generated: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
Agent: {agent_name}
Model: {get_compact_model() or os.environ.get("CAI_MODEL", "gpt-4")}

{summary}

## Metadata
- Original messages: {len(all_histories[agent_name])}
- Compaction method: AI Summary
"""

                memory_path.write_text(memory_content)

                # Register the memory in the index
                self._register_memory(memory_id, memory_name)

                os.environ["CAI_COMPACTED_MEMORY"] = "true"
                console.print(f"[green]✓ Saved memory: {memory_name} (ID: {memory_id})[/green]")

                # Automatically apply the memory to the agent's system prompt
                if agent_name not in COMPACTED_SUMMARIES:
                    COMPACTED_SUMMARIES[agent_name] = []
                    APPLIED_MEMORY_IDS[agent_name] = []

                # Clear existing memories and add new one (maintain single memory behavior for compact all)
                COMPACTED_SUMMARIES[agent_name] = [summary]
                APPLIED_MEMORY_IDS[agent_name] = [memory_id]
                console.print(
                    f"[green]✓ Memory {memory_id} automatically applied to {agent_name}'s system prompt[/green]"
                )

                # Clear the agent's history after saving
                self._clear_agent_history(agent_name)

                # Reload the agent with the new memory
                self._reload_agent_with_memory(agent_name)
            else:
                console.print(f"[red]✗ Failed to compact {agent_name}[/red]")

        console.print("\n[bold green]All agents compacted and saved as memories[/bold green]")
        return True

    def _compact_single_agent(self, agent_identifier: str) -> bool:
        """Compact a single agent's history."""
        # Session primary (P0) or legacy P1 alias
        if agent_identifier.upper() in (DEFAULT_SESSION_AGENT_ID.upper(), "P1"):
            # Get the current active agent from environment or AGENT_MANAGER
            current_agent = AGENT_MANAGER.get_active_agent()
            if current_agent:
                agent_name = getattr(current_agent, "name", None)
                if not agent_name:
                    # Try to get from environment
                    import os

                    agent_type = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
                    from cai.agents import get_available_agents

                    agents = get_available_agents()
                    if agent_type in agents:
                        agent = agents[agent_type]
                        agent_name = getattr(agent, "name", agent_type)
            else:
                console.print(f"[red]No active agent found for {DEFAULT_SESSION_AGENT_ID}[/red]")
                return False
        else:
            agent_name = self._resolve_agent_name(agent_identifier)

        if not agent_name:
            console.print(f"[red]Error: Could not resolve agent '{agent_identifier}'[/red]")
            return False

        # Get history from the actual model instance if possible
        history = None

        # First try to get from ACTIVE_MODEL_INSTANCES
        for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
            if name == agent_name or (
                inst_id in (DEFAULT_SESSION_AGENT_ID, "P1")
                and agent_identifier.upper() in (DEFAULT_SESSION_AGENT_ID.upper(), "P1")
            ):
                model = model_ref() if model_ref else None
                if model and hasattr(model, "message_history"):
                    history = list(model.message_history)
                    break

        # If not found, try get_agent_message_history
        if not history:
            history = get_agent_message_history(agent_name)

        if not history:
            console.print(f"[yellow]No history found for agent '{agent_name}'[/yellow]")
            return True

        original_count = len(history)
        console.print(
            f"\n[#9aa0a6]Compacting {agent_name} ({original_count} messages)...[/]"
        )

        # Generate summary
        summary = self._run_async_in_sync(self._ai_summarize_history(agent_name))

        if summary:
            # Generate unique ID for this memory
            memory_id = self._get_next_memory_id()

            # Save as memory
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            memory_name = f"{agent_name.replace(' ', '_').replace('#', '')}_{timestamp}.md"
            memory_path = MEMORY_DIR / memory_name

            # Create memory content with metadata including ID
            memory_content = f"""# Memory: {agent_name}
ID: {memory_id}
Generated: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
Agent: {agent_name}
Model: {get_compact_model() or os.environ.get("CAI_MODEL", "gpt-4")}

{summary}

## Metadata
- Original messages: {original_count}
- Compaction method: AI Summary
"""

            memory_path.write_text(memory_content)

            # Register the memory in the index
            self._register_memory(memory_id, memory_name)

            console.print(f"[green]✓ Saved memory: {memory_name} (ID: {memory_id})[/green]")
            os.environ["CAI_COMPACTED_MEMORY"] = "true"
            # Automatically apply the memory to the agent's system prompt
            if agent_name not in COMPACTED_SUMMARIES:
                COMPACTED_SUMMARIES[agent_name] = []
                APPLIED_MEMORY_IDS[agent_name] = []

            # Clear existing memories and add new one (maintain single memory behavior for compact single)
            COMPACTED_SUMMARIES[agent_name] = [summary]
            APPLIED_MEMORY_IDS[agent_name] = [memory_id]
            console.print(
                f"[green]✓ Memory {memory_id} automatically applied to {agent_name}'s system prompt[/green]"
            )

            # Ask if user wants to clear history
            clear = console.input("\nClear agent history after compaction? (y/N): ")
            if clear.lower() == "y":
                self._clear_agent_history(agent_name)
                console.print(f"[green]✓ Cleared history for {agent_name}[/green]")

            # Reload the agent with the new memory
            self._reload_agent_with_memory(agent_name, preserve_history=preserve_history)

            # Show memory panel
            console.print(
                Panel(
                    summary[:500] + "..." if len(summary) > 500 else summary,
                    title=_quick_guide_subpanel_title(f"Compacted {memory_id}"),
                    title_align="left",
                    border_style=_CAI_GREEN,
                    padding=(1, 1),
                )
            )
        else:
            console.print(f"[red]✗ Failed to compact {agent_name}[/red]")

        return True

    def _clear_agent_history(self, agent_name: str):
        """Clear an agent's message history."""
        # Find the matching model instance
        model_instance = None
        for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
            if name == agent_name:
                model = model_ref() if model_ref else None
                if model:
                    model_instance = model
                    break

        if model_instance:
            # Clear the model's message history
            model_instance.message_history.clear()
            # Reset context usage since we cleared the history
            os.environ["CAI_CONTEXT_USAGE"] = "0.0"

        # Also clear persistent history
        if agent_name in PERSISTENT_MESSAGE_HISTORIES:
            PERSISTENT_MESSAGE_HISTORIES[agent_name].clear()

    async def _ai_summarize_history(self, agent_name: Optional[str] = None) -> Optional[str]:
        """Use an AI agent to summarize conversation history."""
        # Get history to summarize
        if agent_name:
            # In TUI mode, get history from terminal runner first
            history = []
            if os.getenv("CAI_TUI_MODE") == "true" and "(" in agent_name and ")" in agent_name:
                # Extract terminal number from agent name like "Agent Name (T1)"
                terminal_num = None
                if "(T" in agent_name and ")" in agent_name:
                    start = agent_name.rfind("(T") + 2
                    end = agent_name.find(")", start)
                    if end > start:
                        terminal_num = agent_name[start:end]
                
                if terminal_num and terminal_num.isdigit():
                    # In TUI, history is stored with P-ID (P1, P2, etc.)
                    p_id = f"P{terminal_num}"
                    if p_id in AGENT_MANAGER._message_history:
                        history = AGENT_MANAGER._message_history[p_id]
                    else:
                        # Fallback: try to get from terminal runner
                        try:
                            from cai.tui.core.session_manager import SessionManager
                            session_manager = SessionManager.get_instance()
                            
                            if session_manager:
                                terminal_runner = session_manager.terminal_runners.get(int(terminal_num))
                                if terminal_runner and terminal_runner.agent:
                                    if hasattr(terminal_runner.agent, 'model') and hasattr(terminal_runner.agent.model, 'message_history'):
                                        history = terminal_runner.agent.model.message_history
                        except Exception as e:
                            if os.getenv("CAI_DEBUG") == "1":
                                console.print(f"[dim]Error in _ai_summarize_history: {e}[/dim]")
            
            # If not found via terminal runner, try standard approach
            if not history:
                history = get_agent_message_history(agent_name)
                
            target = f"agent '{agent_name}'"
        else:
            # Get all histories
            all_histories = get_all_agent_histories()
            history = []
            for h in all_histories.values():
                history.extend(h)
            target = "all agents"

        if not history:
            console.print(f"[yellow]No history to summarize for {target}[/yellow]")
            return None

        # Prepare conversation for summarization
        conversation_text = self._format_history_for_summary(history)

        # Get compact settings from compact command
        from cai.repl.commands.compact import get_compact_model, get_custom_prompt

        # Create summary agent
        model_name = get_compact_model() or os.environ.get("CAI_MODEL", "alias1")

        # CRITICAL: Truncate conversation if it's too large for the model's context
        # This prevents the Summary Agent from exceeding its context and triggering
        # recursive compaction (which would create an infinite loop)
        try:
            from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
            # Get max tokens for the model (conservative estimate)
            max_context = OpenAIChatCompletionsModel._get_model_max_tokens(None, model_name)
            # Leave room for system prompt (~10k tokens) and output (~4k tokens)
            # Use 2 chars per token (conservative) - code/commands often have worse ratios
            # Target 70% of available context to stay well under the threshold
            available_tokens = int((max_context - 14000) * 0.7)
            max_input_chars = available_tokens * 2  # ~2 chars per token (conservative)
            if len(conversation_text) > max_input_chars:
                console.print(f"[yellow]Truncating conversation to fit {model_name} context window ({max_input_chars:,} chars)...[/yellow]")
                conversation_text = conversation_text[-max_input_chars:]
        except Exception:
            # If we can't determine model limits, use a safe default (30k chars ~ 15k tokens)
            if len(conversation_text) > 30000:
                console.print("[yellow]Truncating conversation to fit context window...[/yellow]")
                conversation_text = conversation_text[-30000:]

        # Use custom prompt if set, otherwise use default
        custom_prompt = get_custom_prompt()
        if custom_prompt:
            instructions = custom_prompt
        else:
            instructions = """Your task is to create a detailed OPERATIONAL CONTINUITY summary for a penetration testing engagement. This summary must allow the next agent to IMMEDIATELY continue work without ANY re-discovery or reconnaissance.

The agent reading this summary must be able to execute the EXACT NEXT COMMAND without needing to re-scan networks, re-discover services, re-find credentials, or ask clarifying questions.

Before providing your final summary, wrap your analysis in <analysis> tags to organize your thoughts and ensure you've covered all necessary points. In your analysis:

1. Chronologically analyze each message and action in the conversation. For each, identify:
   - The user's explicit requests and objectives (flags to find, hosts to compromise, etc.)
   - Commands executed and their exact outputs
   - Credentials, IPs, ports, and paths discovered
   - What worked and what failed (to avoid retrying failed approaches)
   - The current position in the attack chain

2. Double-check for technical accuracy - verify all IPs, ports, credentials, and paths are captured EXACTLY as they appeared.

3. Identify the precise point where work stopped and what the logical next action should be.

Your summary MUST include ALL of the following sections:

## REQUIRED SECTIONS

### 1. Primary Request and Intent
Capture the user's explicit objectives:
- Overall engagement goal (e.g., "find 18 flags", "achieve domain admin")
- Specific constraints mentioned (e.g., "don't use nmap", "use /tmp/fscan")
- Current sub-objective being worked on

### 2. Network & Host Inventory
```
TARGETS DISCOVERED:
- [IP]: [hostname] | Ports: [list] | Services: [list] | Status: [compromised/enumerated/unexplored]

NETWORK SEGMENTS:
- [CIDR]: [description] | Access method: [direct/via proxy/unreachable]
```

### 3. Active Sessions & Connections
```
SSH SESSIONS:
- [user]@[host] via [method] | Status: [active/closed]

PROXIES:
- [type]://[host]:[port] | Status: [running/stopped] | Use: [proxychains/--proxy flag]

SHELLS/CALLBACKS:
- [type] on [host] as [user] | How to interact: [command]
```

### 4. Credentials & Access
```
WORKING CREDENTIALS:
- [service]: [username]:[password] @ [host] | Verified: [yes/no]

HASHES FOUND:
- [username]:[hash_type]:[hash] @ [host]

KEYS/TOKENS:
- [type]: [value or path]

FAILED/BLOCKED CREDENTIALS (do not retry):
- [service]: [username]:[password] @ [host] | Error: [message]
```

### 5. Flags & Objectives Captured
```
FLAGS FOUND:
- Flag [N]: [exact value] | Location: [path] | Host: [IP] | Method: [how obtained]

OBJECTIVES COMPLETED:
- [description] | Evidence: [proof]
```

### 6. Vulnerabilities & Exploits
```
CONFIRMED EXPLOITABLE:
- [CVE/vuln name]: [service] @ [host]:[port] | Exploit: [tool/method] | Status: [exploited/ready to exploit]

FAILED EXPLOITS (DO NOT RETRY):
- [exploit]: [target] | Failure reason: [specific error]

POTENTIAL (UNTESTED):
- [vuln] @ [host] | Suggested test: [command]
```

### 7. Files & Artifacts
```
IMPORTANT FILES FOUND:
- [host]:[path]: [contents summary or why important]

FILES UPLOADED/CREATED:
- [host]:[path]: [purpose]

LOOT COLLECTED:
- [description]: [location or value]
```

### 8. Problem Solving & Troubleshooting
Document what was tried:
- Approaches that WORKED (with exact commands)
- Approaches that FAILED and WHY (to prevent retrying)
- Current blockers or challenges

### 9. Current Work (CRITICAL)
Describe PRECISELY what was being worked on immediately before this summary:
- Target host and service being attacked
- Specific vulnerability or access method being attempted
- Last 3-5 commands executed with their outputs
- Where exactly the work stopped

### 10. Immediate Next Step
```
EXACT NEXT ACTION:
- Target: [IP:port or host]
- Action: [what to do]
- Command: [exact command with all parameters]
- Expected result: [what success looks like]
- If it fails: [alternative approach]

REASONING: [why this is the logical next step]
```

### 11. User Constraints (VERBATIM)
Quote any specific instructions from the user that affect execution:
```
- "[exact quote from user about constraints]"
- "[exact quote about tool preferences]"
```

### 12. Attack Path Summary
```
ENTRY POINT: [initial access method]
ATTACK CHAIN: [host1/method] -> [host2/method] -> [current position] -> [next target]
CURRENT POSITION: [host], [user], [privilege level]
ULTIMATE GOAL: [final objective]
```

## OUTPUT FORMAT

<analysis>
[Your chronological analysis of the conversation, ensuring all technical details are captured accurately]
</analysis>

<summary>
[Structured summary following ALL sections above]
</summary>

## CRITICAL RULES

1. NEVER omit sections - write "None discovered" if empty
2. Use EXACT values - IPs, ports, credentials, paths must be verbatim
3. Include FULL command outputs for recent commands (not truncated)
4. The "Immediate Next Step" section is MOST IMPORTANT - be extremely specific
5. Include direct quotes from recent messages showing exactly what was being attempted
6. Missing ANY technical detail forces the agent to waste time re-discovering it

The goal: the next agent reads this summary and IMMEDIATELY executes the next command without preamble, questions, or reconnaissance."""

        summary_agent = Agent(
            name="Summary Agent",
            instructions=instructions,
            model=OpenAIChatCompletionsModel(
                model=model_name,
                openai_client=_async_openai_client_for_memory_summary(model_name),
                agent_name="Summary Agent",
            ),
        )

        # Generate summary
        console.print(
            f"[#9aa0a6][CAI] Generating summary for [/][bold #00ff9d]{target}[/bold #00ff9d]"
            f"[#9aa0a6] using [/][bold white]{model_name}[/bold white][#9aa0a6]...[/]"
        )
        console.print(
            f"[#9aa0a6][CAI] Processing [/][bold white]{len(history)}[/bold white]"
            f"[#9aa0a6] messages... this may take a moment.[/]"
        )

        try:
            # In TUI mode, use streaming to show progress
            if os.getenv("CAI_TUI_MODE") == "true" and os.getenv("CAI_STREAM", "false").lower() == "true":
                # Use streaming for visible progress
                summary_request = f"""Analyze and extract ALL operational data from this conversation.
The next agent MUST be able to continue work immediately without any re-discovery.

EXTRACT EVERYTHING: IPs, ports, credentials, flags, commands, paths, vulnerabilities, access methods.
Be EXHAUSTIVE with technical details. Missing ANY data point will cause the agent to waste time.

CONVERSATION TO ANALYZE:
{conversation_text}"""
                result = Runner.run_streamed(
                    starting_agent=summary_agent,
                    input=summary_request,
                    max_turns=1,
                )
                
                # Collect the streamed output
                final_output = ""
                async for event in result.stream_events():
                    if hasattr(event, "name") and event.name == "text_output":
                        if hasattr(event, "item") and hasattr(event.item, "content"):
                            final_output += event.item.content
                
                if final_output:
                    console.print("[green]✓ Summary generated successfully[/green]")
                    return final_output
                else:
                    console.print("[red]✗ No summary generated[/red]")
                    return None
            else:
                # Non-streaming execution
                summary_request = f"""Analyze and extract ALL operational data from this conversation.
The next agent MUST be able to continue work immediately without any re-discovery.

EXTRACT EVERYTHING: IPs, ports, credentials, flags, commands, paths, vulnerabilities, access methods.
Be EXHAUSTIVE with technical details. Missing ANY data point will cause the agent to waste time.

CONVERSATION TO ANALYZE:
{conversation_text}"""
                result = await Runner.run(
                    starting_agent=summary_agent,
                    input=summary_request,
                    max_turns=1,
                )

                if result.final_output:
                    console.print("[green]✓ Summary generated successfully[/green]")
                    return str(result.final_output)
                else:
                    console.print("[red]✗ No summary generated[/red]")
                    return None

        except Exception as e:
            console.print(f"[red]Error generating summary: {e}[/red]")
            return None

    def _format_history_for_summary(self, history: List[Dict[str, Any]]) -> str:
        """Format message history for summarization with emphasis on preserving critical technical data."""
        formatted_parts = []

        # Track unique technical artifacts to ensure they're not lost
        discovered_ips = set()
        discovered_creds = []
        discovered_flags = []

        for msg in history:
            role = msg.get("role", "unknown")
            content = msg.get("content", "")

            # Skip empty messages
            if not content:
                # But still capture tool calls even if content is empty
                if role == "assistant" and "tool_calls" in msg and msg["tool_calls"]:
                    tool_info = []
                    for tc in msg["tool_calls"]:
                        if hasattr(tc, "function"):
                            # Preserve full command arguments for technical commands
                            args = tc.function.arguments
                            tool_info.append(f"{tc.function.name}({args})")
                        elif isinstance(tc, dict) and "function" in tc:
                            args = tc["function"].get("arguments", "")
                            tool_info.append(f"{tc['function'].get('name', 'unknown')}({args})")
                    if tool_info:
                        formatted_parts.append(f"ASSISTANT (tools): {'; '.join(tool_info)}")
                continue

            # Format based on role
            if role == "user":
                formatted_parts.append(f"USER: {content}")
            elif role == "assistant":
                # Check for tool calls
                if "tool_calls" in msg and msg["tool_calls"]:
                    tool_info = []
                    for tc in msg["tool_calls"]:
                        if hasattr(tc, "function"):
                            args = tc.function.arguments
                            tool_info.append(f"{tc.function.name}({args})")
                        elif isinstance(tc, dict) and "function" in tc:
                            args = tc["function"].get("arguments", "")
                            tool_info.append(f"{tc['function'].get('name', 'unknown')}({args})")
                    if tool_info:
                        formatted_parts.append(f"ASSISTANT (tools): {'; '.join(tool_info)}")
                if content:
                    formatted_parts.append(f"ASSISTANT: {content}")
            elif role == "tool":
                content_str = str(content)
                # Preserve more output - increased from 500 to 3000 chars
                # Critical outputs like nmap, credentials, flags should not be truncated
                if len(content_str) < 3000:
                    formatted_parts.append(f"TOOL OUTPUT:\n{content_str}")
                else:
                    # For long outputs, keep beginning and end which often have critical info
                    # Also try to preserve lines with IPs, credentials, flags
                    important_lines = []
                    for line in content_str.split('\n'):
                        line_lower = line.lower()
                        # Preserve lines with critical patterns
                        if any(pattern in line_lower for pattern in [
                            'flag', 'password', 'credential', 'root', 'admin',
                            'hash', 'key', 'token', 'secret', 'access',
                            '172.16.', '10.10.', '192.168.', 'ssh', 'ftp',
                            'webmin', 'http', 'port', 'open', 'vulnerable',
                            'exploit', 'shell', 'reverse', 'connect'
                        ]):
                            important_lines.append(line)

                    # Build output: first 1500 chars + important lines + last 500 chars
                    truncated = content_str[:1500]
                    if important_lines:
                        truncated += f"\n[...]\n[CRITICAL LINES PRESERVED:]\n" + "\n".join(important_lines[:30])
                    truncated += f"\n[...]\n{content_str[-500:]}"
                    formatted_parts.append(f"TOOL OUTPUT:\n{truncated}")

        # Increase limit from 50 to 150 exchanges to preserve more context
        # For very long histories, prioritize recent messages but include early context
        if len(formatted_parts) > 150:
            # Keep first 20 messages (initial context) + last 130 messages (recent work)
            result_parts = formatted_parts[:20] + ["[... earlier messages omitted ...]"] + formatted_parts[-130:]
        else:
            result_parts = formatted_parts

        return "\n\n".join(result_parts)

    def _get_current_agent_name(self) -> Optional[str]:
        """Get the name of the current active agent."""
        # First check AGENT_MANAGER for the active agent
        active_agent = AGENT_MANAGER.get_active_agent()
        if active_agent:
            agent_name = getattr(active_agent, "name", None)
            if not agent_name:
                # If agent doesn't have a name attribute, try to get from environment
                import os

                agent_type = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
                from cai.agents import get_available_agents

                agents = get_available_agents()
                if agent_type in agents:
                    agent = agents[agent_type]
                    agent_name = getattr(agent, "name", agent_type)
            return agent_name

        # Check if there's an active agent name in AGENT_MANAGER
        if hasattr(AGENT_MANAGER, "_active_agent_name") and AGENT_MANAGER._active_agent_name:
            return AGENT_MANAGER._active_agent_name

        # Check registered agents
        registered = AGENT_MANAGER.get_registered_agents()
        if registered:
            # If there's only one registered agent, use it
            if len(registered) == 1:
                return list(registered.keys())[0]
            # Otherwise check session primary id (P0) or legacy P1
            for agent_name, aid in registered.items():
                if aid in (DEFAULT_SESSION_AGENT_ID, "P1"):
                    return agent_name

        # Try to get from environment and available agents
        import os

        agent_type = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
        from cai.agents import get_available_agents

        agents = get_available_agents()
        if agent_type in agents:
            agent = agents[agent_type]
            return getattr(agent, "name", agent_type)

        # Fallback to checking the model
        current_model = get_current_active_model()
        if current_model and hasattr(current_model, "agent_name"):
            return current_model.agent_name

        return None

    def _reload_agent_with_memory(self, agent_name: str, preserve_history: bool = True):
        """Reload an agent to apply memory changes.

        Args:
            agent_name: Name of the agent to reload
            preserve_history: Whether to preserve message history (default True).
                            Set to False when called from compact to avoid restoring cleared history.
        """
        try:
            # Get the current agent instance and its history
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            from cai.agents import get_agent_by_name, get_available_agents
            import os

            # ALWAYS skip reload when in parallel mode
            # Parallel agents are already configured and reloading causes duplicate registrations
            if PARALLEL_CONFIGS:
                console.print(
                    f"[dim]Agent '{agent_name}' memory applied without reload (parallel mode)[/dim]"
                )
                return

            # Find the agent type from available agents
            agent_type = None
            available_agents = get_available_agents()
            for atype, agent in available_agents.items():
                if hasattr(agent, "name") and agent.name == agent_name:
                    agent_type = atype
                    break

            if not agent_type:
                # For pattern-based agents or custom named agents, skip reload
                console.print(f"[dim]Agent '{agent_name}' memory applied without reload[/dim]")
                return

            # Get the current agent's message history before reloading
            history_backup = []
            if preserve_history:
                current_history = get_agent_message_history(agent_name)
                if current_history:
                    # Store a copy of the history
                    history_backup = list(current_history)
            else:
                # When not preserving history (e.g., from compact), clear it before creating new agent
                AGENT_MANAGER.clear_history(agent_name)

            # Get the agent ID
            agent_id = AGENT_MANAGER.get_id_by_name(agent_name)
            if not agent_id:
                agent_id = DEFAULT_SESSION_AGENT_ID

            # Create a new agent instance with memory already in system prompt
            new_agent = get_agent_by_name(agent_type, agent_id=agent_id)

            # Ensure the new agent has the memory applied in its system prompt
            # The get_agent_by_name function should already handle this via get_compacted_summary

            # Update the agent in AGENT_MANAGER based on mode
            if agent_id in (DEFAULT_SESSION_AGENT_ID, "P1") and not AGENT_MANAGER._parallel_agents:
                # Single agent mode - use switch_to_single_agent
                AGENT_MANAGER.switch_to_single_agent(new_agent, agent_name)
            else:
                # Parallel mode - use set_parallel_agent
                AGENT_MANAGER.set_parallel_agent(agent_id, new_agent, agent_name)

            # Restore the message history to the new agent instance
            if (
                preserve_history
                and hasattr(new_agent, "model")
                and hasattr(new_agent.model, "message_history")
            ):
                # The switch_to_single_agent might have already transferred history
                # Only restore if the new agent's history is empty or different
                if not new_agent.model.message_history and history_backup:
                    new_agent.model.message_history.extend(history_backup)
                elif len(new_agent.model.message_history) != len(history_backup):
                    # Replace with our backup if different
                    new_agent.model.message_history.clear()
                    new_agent.model.message_history.extend(history_backup)

            # Also update PERSISTENT_MESSAGE_HISTORIES if needed
            if agent_name in PERSISTENT_MESSAGE_HISTORIES:
                PERSISTENT_MESSAGE_HISTORIES[agent_name].clear()
                if preserve_history:
                    PERSISTENT_MESSAGE_HISTORIES[agent_name].extend(history_backup)

            # Update the global active agent in CLI if we're in single agent mode
            if agent_id in (DEFAULT_SESSION_AGENT_ID, "P1") and not AGENT_MANAGER._parallel_agents:
                # Import cli module to update the agent reference
                try:
                    import cai.cli

                    if hasattr(cai.cli, "agent"):
                        cai.cli.agent = new_agent
                except:
                    pass

            console.print(f"[green]✓ Reloaded agent '{agent_name}' with memory applied[/green]")
            console.print("[dim]The memory is now included in the agent's system prompt[/dim]")

        except Exception as e:
            console.print(f"[yellow]Warning: Could not reload agent automatically: {e}[/yellow]")
            console.print("[dim]The memory will be applied on the next agent interaction[/dim]")

    def handle_remove(self, args: Optional[List[str]] = None) -> bool:
        """Remove a specific memory from an agent."""
        if not args or len(args) < 2:
            console.print("[red]Error: Memory ID and agent name required[/red]")
            console.print("Usage: /memory remove <memory_id> <agent_name>")
            return False

        memory_id = args[0].upper()
        agent_identifier = " ".join(args[1:])
        agent_name = self._resolve_agent_name(agent_identifier)

        if not agent_name:
            console.print(f"[red]Error: Could not resolve agent '{agent_identifier}'[/red]")
            return False

        # Check if agent has memories applied
        if agent_name not in APPLIED_MEMORY_IDS:
            console.print(f"[yellow]Agent '{agent_name}' has no memories applied[/yellow]")
            return True

        memory_ids = APPLIED_MEMORY_IDS[agent_name]
        summaries = COMPACTED_SUMMARIES.get(agent_name, [])

        # Handle backward compatibility
        if isinstance(memory_ids, str):
            if memory_ids == memory_id:
                del APPLIED_MEMORY_IDS[agent_name]
                if agent_name in COMPACTED_SUMMARIES:
                    del COMPACTED_SUMMARIES[agent_name]
                console.print(f"[green]✓ Removed memory {memory_id} from {agent_name}[/green]")
                self._reload_agent_with_memory(agent_name)
                return True
            else:
                console.print(
                    f"[yellow]Memory {memory_id} not found for agent '{agent_name}'[/yellow]"
                )
                return True

        # Handle list of memories
        if memory_id not in memory_ids:
            console.print(f"[yellow]Memory {memory_id} not found for agent '{agent_name}'[/yellow]")
            return True

        # Find index and remove
        idx = memory_ids.index(memory_id)
        memory_ids.pop(idx)
        if isinstance(summaries, list) and idx < len(summaries):
            summaries.pop(idx)

        # Clean up if no memories left
        if not memory_ids:
            del APPLIED_MEMORY_IDS[agent_name]
            if agent_name in COMPACTED_SUMMARIES:
                del COMPACTED_SUMMARIES[agent_name]

        console.print(f"[green]✓ Removed memory {memory_id} from {agent_name}[/green]")
        self._reload_agent_with_memory(agent_name)

        return True

    def handle_clear(self, args: Optional[List[str]] = None) -> bool:
        """Clear all memories from an agent."""
        if not args:
            console.print("[red]Error: Agent name required[/red]")
            console.print("Usage: /memory clear <agent_name>")
            return False

        agent_identifier = " ".join(args)
        agent_name = self._resolve_agent_name(agent_identifier)

        if not agent_name:
            console.print(f"[red]Error: Could not resolve agent '{agent_identifier}'[/red]")
            return False

        # Check if agent has memories applied
        if agent_name not in APPLIED_MEMORY_IDS:
            console.print(f"[yellow]Agent '{agent_name}' has no memories applied[/yellow]")
            return True

        # Ask for confirmation
        memory_ids = APPLIED_MEMORY_IDS.get(agent_name)
        count = len(memory_ids) if isinstance(memory_ids, list) else 1
        confirm = console.input(f"Clear {count} memory(ies) from '{agent_name}'? (y/N): ")

        if confirm.lower() == "y":
            del APPLIED_MEMORY_IDS[agent_name]
            if agent_name in COMPACTED_SUMMARIES:
                del COMPACTED_SUMMARIES[agent_name]
            console.print(f"[green]✓ Cleared all memories from {agent_name}[/green]")
            self._reload_agent_with_memory(agent_name)
        else:
            console.print("[dim]Cancelled[/dim]")

        return True

    def handle_list_applied(self, args: Optional[List[str]] = None) -> bool:
        """Show which memories are applied to an agent."""
        if not args:
            # Show all agents with applied memories
            if not APPLIED_MEMORY_IDS:
                console.print("[yellow]No memories applied to any agents[/yellow]")
                return True

            console.print("[bold white]Applied memories by agent[/bold white]\n")

            for agent_name, memory_ids in APPLIED_MEMORY_IDS.items():
                console.print(f"[bold #00ff9d]{agent_name}:[/bold #00ff9d]")

                if isinstance(memory_ids, list):
                    for i, memory_id in enumerate(memory_ids):
                        # Try to get memory details
                        index = self._load_index()
                        memory_file = index.get("mappings", {}).get(memory_id, "Unknown")
                        console.print(f"  {i+1}. {memory_id} - {memory_file}")
                else:
                    # Backward compatibility
                    index = self._load_index()
                    memory_file = index.get("mappings", {}).get(memory_ids, "Unknown")
                    console.print(f"  1. {memory_ids} - {memory_file}")

                console.print()
        else:
            # Show memories for specific agent
            agent_identifier = " ".join(args)
            agent_name = self._resolve_agent_name(agent_identifier)

            if not agent_name:
                console.print(f"[red]Error: Could not resolve agent '{agent_identifier}'[/red]")
                return False

            if agent_name not in APPLIED_MEMORY_IDS:
                console.print(f"[yellow]No memories applied to '{agent_name}'[/yellow]")
                return True

            memory_ids = APPLIED_MEMORY_IDS[agent_name]
            summaries = COMPACTED_SUMMARIES.get(agent_name, [])

            console.print(f"[bold white]Memories applied to {agent_name}[/bold white]\n")

            if isinstance(memory_ids, list):
                for i, memory_id in enumerate(memory_ids):
                    # Get memory details
                    index = self._load_index()
                    memory_file = index.get("mappings", {}).get(memory_id, "Unknown")

                    # Show summary preview
                    summary_preview = ""
                    if isinstance(summaries, list) and i < len(summaries):
                        summary_preview = (
                            summaries[i][:100] + "..." if len(summaries[i]) > 100 else summaries[i]
                        )

                    console.print(
                        f"[bold #00ff9d]{i+1}. {memory_id}[/bold #00ff9d] - {memory_file}"
                    )
                    if summary_preview:
                        console.print(f"   [dim]{summary_preview}[/dim]")
                    console.print()
            else:
                # Backward compatibility
                index = self._load_index()
                memory_file = index.get("mappings", {}).get(memory_ids, "Unknown")
                console.print(
                    f"[bold #00ff9d]1. {memory_ids}[/bold #00ff9d] - {memory_file}"
                )
                if isinstance(summaries, str) and summaries:
                    summary_preview = summaries[:100] + "..." if len(summaries) > 100 else summaries
                    console.print(f"   [dim]{summary_preview}[/dim]")

        return True


# Global instance for access from other modules
MEMORY_COMMAND_INSTANCE = MemoryCommand()

# Register the command
register_command(MEMORY_COMMAND_INSTANCE)


def get_compacted_summary(agent_name: Optional[str] = None) -> Optional[str]:
    """Get compacted summary for injection into system prompt.

    This retrieves any applied memory summaries for the agent.
    Now supports multiple memories per agent.

    Session-wide summaries (after auto-compact) are merged first so every
    agent sees the same context via ``CAI_SESSION_COMPACT_SUMMARY``.

    Args:
        agent_name: Specific agent name or None for global summary

    Returns:
        Summary text if available, None otherwise
    """
    parts: List[str] = []
    try:
        from cai.util.session_compact import get_session_compact_summary

        session_summary = get_session_compact_summary()
        if session_summary:
            parts.append(
                "## Session-wide compacted context (all agents)\n" + session_summary
            )
    except Exception:
        pass

    def _append_agent_summaries(key: str) -> None:
        if key not in COMPACTED_SUMMARIES:
            return
        summaries = COMPACTED_SUMMARIES[key]
        memory_ids = APPLIED_MEMORY_IDS.get(key, [])
        if isinstance(summaries, list) and summaries:
            for i, summary in enumerate(summaries):
                memory_id = memory_ids[i] if i < len(memory_ids) else "Unknown"
                agent_parts.append(
                    f"Agent memory {i+1}/{len(summaries)} (ID: {memory_id}):\n{summary}"
                )
        elif isinstance(summaries, str):
            agent_parts.append(summaries)

    agent_parts: List[str] = []
    if agent_name:
        _append_agent_summaries(agent_name)
        if " #" in agent_name:
            _append_agent_summaries(agent_name.split(" #")[0])

    if agent_parts:
        parts.append("## Agent-specific memory\n" + "\n\n---\n\n".join(agent_parts))

    if parts:
        return "\n\n---\n\n".join(parts)

    return None


def get_applied_memory_id(agent_name: str) -> Optional[str]:
    """Get the ID of the memory currently applied to an agent.

    For backward compatibility, returns first memory ID if multiple exist.

    Args:
        agent_name: The agent name to check

    Returns:
        Memory ID if applied, None otherwise
    """
    memory_ids = APPLIED_MEMORY_IDS.get(agent_name)
    if isinstance(memory_ids, list) and memory_ids:
        return memory_ids[0]  # Return first for backward compatibility
    elif isinstance(memory_ids, str):
        return memory_ids
    return None


def get_applied_memory_ids(agent_name: str) -> List[str]:
    """Get all memory IDs currently applied to an agent.

    Args:
        agent_name: The agent name to check

    Returns:
        List of memory IDs if applied, empty list otherwise
    """
    memory_ids = APPLIED_MEMORY_IDS.get(agent_name, [])
    if isinstance(memory_ids, list):
        return memory_ids
    elif isinstance(memory_ids, str):
        return [memory_ids]  # Convert single ID to list
    return []
