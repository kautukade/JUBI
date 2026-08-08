"""
CAI Terminal - Textual App that wires together Model, View, and Controller.

This file was reduced from ~4,500 LOC to ~800 LOC by extracting:
- State management     -> cai.tui.model.state (TUIState)
- Session / history    -> cai.tui.model.session (SessionState)
- Input routing        -> cai.tui.controller.input_controller (InputController)
- Agent lifecycle      -> cai.tui.controller.agent_controller (AgentController)
- Layout / CSS / help  -> cai.tui.view.main_view
"""

import asyncio
import os
import re
import time
from typing import Optional, Tuple

# -- MVC layers ---------------------------------------------------------------
from cai.tui.model.state import TUIState
from cai.tui.model.session import SessionState
from cai.tui.controller.agent_controller import AgentController
from cai.tui.controller.input_controller import InputController, RouteKind
from cai.tui.view.main_view import (
    CAI_TERMINAL_CSS,
    compose_main_layout,
    register_cai_themes,
    update_tab_appearance,
)

# -- Textual -------------------------------------------------------------------
from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Container
from textual.reactive import reactive
from textual.widgets import Input, ListView, Button, Static, TabbedContent
from textual.events import Unmount

# -- Theme / config ------------------------------------------------------------
from cai.tui.theme import ThemeManager, THEMES
from cai.tui.config import TUIConfig
from cai.config import get_config as _get_cai_config

# -- Startup YAML config loader ------------------------------------------------
from cai.config_loader import (
    AgentsConfigError,
    extract_agent_definitions,
    load_agents_config,
)

# -- Simple recursion prevention -----------------------------------------------
class RecursionGuard:
    def __init__(self, max_attempts=3):
        self.attempts = {}
        self.max_attempts = max_attempts

    def can_proceed(self, key):
        if key not in self.attempts:
            self.attempts[key] = 0
        if self.attempts[key] >= self.max_attempts:
            return False
        self.attempts[key] += 1
        return True

_recursion_guard = RecursionGuard()

# -- UI components (lightweight imports) ---------------------------------------
from cai.tui.components.stable_grid import StableTerminalGrid
from cai.tui.components.universal_terminal import UniversalTerminal
from cai.tui.components.sidebar import Sidebar, AgentDoubleClicked, TeamSelected
from cai.tui.components.prompt_input import PromptInput
from cai.tui.components.command_handler import CommandHandler
from cai.tui.components.agent_manager import AgentManager
from pathlib import Path
from cai.tui.components.agent_selector_panel import (
    AgentSelectorPanel,
    AgentSelectionConfirmed,
    AgentSelectionCancelled,
)
from cai.tui.components.agent_creator_panel import (
    AgentCreatorPanel,
    AgentCreationConfirmed,
    AgentCreationCancelled,
)
from cai.tui.components.info_status_bar import InfoStatusBar
from cai.repl.commands.parallel import ParallelConfig, PARALLEL_CONFIGS
from cai.sdk.agents.models.openai_chatcompletions_integration import (
    integrate_openai_chatcompletions_display,
)
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.tui.core.session_manager import SessionManager
from cai.tui.core.prompt_queue import PROMPT_QUEUE

# -- Context preservation (must run at import time) ----------------------------
from cai.tui.display.context_preservation import enable_task_context_propagation
enable_task_context_propagation()

# -- Module-level singletons --------------------------------------------------
_tui_config = TUIConfig()
_theme_manager = ThemeManager()
# Theme resolved from CAIConfig (replaces os.getenv("CAI_THEME"))
_theme_name = _get_cai_config().tui_theme
if _theme_name in THEMES:
    _theme_manager.set_theme(_theme_name)


def is_tui_mode() -> bool:
    """Check if we're running in TUI mode."""
    return _get_cai_config().tui_enabled


# =============================================================================
# CAITerminal -- the thin Textual App
# =============================================================================

class CAITerminal(App):
    """Main CAI Terminal application wiring Model / View / Controller."""

    # Reactive state mirrored for Textual watchers
    current_mode = reactive("single")
    current_view = reactive("terminal")
    sidebar_visible = reactive(True)

    CSS = CAI_TERMINAL_CSS

    ENABLE_COMMAND_PALETTE = True
    mouse_over_widget = None
    captured_widget = None

    BINDINGS = [
        Binding("ctrl+c", "cancel_selected", "Cancel Selected"),
        Binding("ctrl+q", "quit", "Exit"),
        Binding("ctrl+l", "clear", "Clear"),
        Binding("ctrl+p", "command_palette", "Command Palette"),
        Binding("ctrl+shift+a", "parallel_prompt", "Prompt All"),
        Binding("ctrl+s", "toggle_sidebar", "Toggle Sidebar"),
        Binding("ctrl+n", "next_terminal", "Next Terminal"),
        Binding("ctrl+b", "prev_terminal", "Previous Terminal"),
        Binding("escape", "cancel_all", "Cancel All"),
        Binding("ctrl+shift+q", "show_queue", "Show Queue"),
        Binding("ctrl+e", "close_terminal", "Close Terminal"),
        Binding("ctrl+t", "toggle_terminal_view", "Toggle View"),
        Binding("ctrl+shift+t", "cycle_theme", "Cycle Theme"),
        Binding("ctrl+u", "clear_input", "Clear Input"),
        Binding("ctrl+1", "show_terminal", "Terminal Tab"),
        Binding("ctrl+2", "show_ctr", "CTR Tab"),
        Binding("ctrl+tab", "cycle_tabs", "Next Tab"),
    ]

    def __init__(self):
        super().__init__()
        # MVC instances
        self._state = TUIState()
        self._session = SessionState()
        self._agent_ctl = AgentController(self._state, exit_callback=self.exit)
        self._input_ctl = InputController(self._state)

        CAITerminal._current_app = self
        CAITerminal._instance = self

    # -- Backward-compat property aliases (delegate to TUIState) ---------------
    # Existing code references self.terminal_grid, self.sidebar, etc.
    # These thin properties keep compatibility while state lives in TUIState.

    @property
    def terminal_grid(self):
        return self._state.terminal_grid

    @terminal_grid.setter
    def terminal_grid(self, v):
        self._state.terminal_grid = v

    @property
    def sidebar(self):
        return self._state.sidebar

    @sidebar.setter
    def sidebar(self, v):
        self._state.sidebar = v

    @property
    def command_handler(self):
        return self._state.command_handler

    @command_handler.setter
    def command_handler(self, v):
        self._state.command_handler = v

    @property
    def agent_manager(self):
        return self._state.agent_manager

    @agent_manager.setter
    def agent_manager(self, v):
        self._state.agent_manager = v

    @property
    def prompt_input(self):
        return self._state.prompt_input

    @prompt_input.setter
    def prompt_input(self, v):
        self._state.prompt_input = v

    @property
    def session_manager(self):
        return self._state.session_manager

    @session_manager.setter
    def session_manager(self, v):
        self._state.session_manager = v

    @property
    def _terminal_agent_map(self):
        return self._state.terminal_agent_map

    @_terminal_agent_map.setter
    def _terminal_agent_map(self, v):
        self._state.terminal_agent_map = v

    @property
    def _show_all_terminals(self):
        return self._state.show_all_terminals

    @_show_all_terminals.setter
    def _show_all_terminals(self, v):
        self._state.show_all_terminals = v

    @property
    def _last_esc_time(self):
        return self._state.last_esc_time

    @_last_esc_time.setter
    def _last_esc_time(self, v):
        self._state.last_esc_time = v

    @property
    def _esc_exit_threshold(self):
        return self._state.esc_exit_threshold

    @property
    def history_file(self):
        return self._state.history_file

    @history_file.setter
    def history_file(self, v):
        self._state.history_file = v

    @property
    def _cancelling(self):
        return self._state.cancelling

    @_cancelling.setter
    def _cancelling(self, v):
        self._state.cancelling = v

    @property
    def _cancel_task(self):
        return self._state.cancel_task

    @_cancel_task.setter
    def _cancel_task(self, v):
        self._state.cancel_task = v

    @property
    def _startup_config_applied(self):
        return self._state.startup_config_applied

    @_startup_config_applied.setter
    def _startup_config_applied(self, v):
        self._state.startup_config_applied = v

    @property
    def _session_start_time(self):
        return self._state.session_start_time

    @_session_start_time.setter
    def _session_start_time(self, v):
        self._state.session_start_time = v

    # =========================================================================
    # Compose & Mount -- delegates to view layer
    # =========================================================================

    def compose(self) -> ComposeResult:
        yield from compose_main_layout()

    def on_mount(self) -> None:
        os.environ["CAI_TUI_MODE"] = "true"

        # Register and apply theme
        try:
            register_cai_themes(self.register_theme)
        except Exception:
            pass
        preferred = _get_cai_config().tui_theme or _tui_config.get_theme() or "tokyo-night"
        if isinstance(preferred, str) and "light" in preferred.lower():
            preferred = "tokyo-night"
        try:
            self.theme = preferred
        except Exception:
            self.theme = "tokyo-night"

        # Display manager
        from cai.tui.display import DisplayManager, DisplayMode
        display_manager = DisplayManager()
        display_manager.set_mode(DisplayMode.TUI)
        integrate_openai_chatcompletions_display()

        from cai.sdk.agents.models.openai_chatcompletions_info_bar_integration import (
            integrate_openai_chatcompletions_info_bar,
        )
        integrate_openai_chatcompletions_info_bar()

        # Query widgets
        self.terminal_grid = self.query_one("#terminal-grid-container", StableTerminalGrid)
        self.sidebar = self.query_one("#sidebar", Sidebar)
        self.prompt_input = self.query_one("#main-input", PromptInput)
        self.prompt_input.focus()
        try:
            self.query_one("#prompt-input-field").can_focus = True
        except Exception:
            pass

        self.call_after_refresh(self._initialize_after_mount)

        # Command history (delegate to SessionState)
        self._session.init_history()
        self._state.history_file = self._session.history_file
        self._load_history_into_widget()

        self.set_interval(1.0, self.update_layout_indicator)

        try:
            self.sidebar_visible = self.query_one("#sidebar", Sidebar).visible
        except Exception:
            self.sidebar_visible = True

    def _load_history_into_widget(self) -> None:
        """Load command history into the PromptInput widget."""
        prompt_widget = self.query_one("#main-input", PromptInput)
        if prompt_widget and prompt_widget._input_widget:
            history = self._session.load_history()
            prompt_widget._input_widget.command_history = history

    # =========================================================================
    # Post-mount initialisation
    # =========================================================================

    def _initialize_after_mount(self) -> None:
        if not _recursion_guard.can_proceed("initialize_after_mount"):
            return

        self.session_manager = SessionManager()
        PROMPT_QUEUE.set_process_callback(self._process_queued_prompt)

        main_terminal = self.terminal_grid.get_main_terminal()
        if main_terminal and main_terminal.output:
            self.command_handler = CommandHandler(
                main_terminal.output, 1, main_terminal.terminal_id
            )
            self.agent_manager = AgentManager(main_terminal.output)
            self.command_handler.session_manager = self.session_manager

            main_terminal.state.agent_name = self.command_handler.current_agent_name
            main_terminal._update_header()
            self.session_manager.add_terminal_runner(1, main_terminal)

        try:
            self.query_one("#info-status-bar", InfoStatusBar)._update_info()
        except Exception:
            pass

        asyncio.create_task(self._agent_ctl.initialize_terminal_runner(1))

        agent_type = "redteam_agent"
        self.agent_manager.current_agent_name = agent_type
        asyncio.create_task(self.agent_manager.initialize_agent(agent_type))
        asyncio.create_task(self._maybe_apply_startup_config())

        queue_file = _get_cai_config().queue_file
        if queue_file:
            main_terminal.write(f"[dim]Queue file detected: {queue_file}[/dim]")
            asyncio.create_task(self._load_and_process_queue_file(queue_file))
        else:
            if _recursion_guard.attempts.get("initialize_after_mount", 0) < 2:
                asyncio.create_task(self._delayed_initialize())
            return

    async def _delayed_initialize(self) -> None:
        await asyncio.sleep(0.5)
        self._initialize_after_mount()
        self._check_mode()

    # =========================================================================
    # Startup config (YAML-based multi-agent)
    # =========================================================================

    async def _maybe_apply_startup_config(self) -> None:
        if self._startup_config_applied:
            return
        yaml_path = _get_cai_config().tui_startup_yaml
        if not yaml_path:
            return
        await asyncio.sleep(0.3)
        await self._apply_startup_config(yaml_path)

    async def _apply_startup_config(self, yaml_path: str) -> None:
        """Configure terminals and agents from YAML startup definitions."""
        if self._startup_config_applied:
            return
        grid = self.terminal_grid
        sm = self.session_manager
        if not grid or not sm:
            return
        main_terminal = grid.get_main_terminal()

        try:
            data, resolved_path = load_agents_config(yaml_path)
        except AgentsConfigError as exc:
            if main_terminal:
                main_terminal.write(f"[red]Failed to load agents YAML: {exc}[/red]")
            return

        if not resolved_path:
            if main_terminal:
                main_terminal.write(f"[yellow]Startup YAML not found: {yaml_path}[/yellow]")
            return

        agents, metadata, origin = extract_agent_definitions(data)
        if not agents:
            if main_terminal:
                main_terminal.write(f"[yellow]No agent definitions found in {resolved_path}[/yellow]")
            return

        runtime_prompt_override = _get_cai_config().tui_shared_prompt
        runtime_prompt = runtime_prompt_override.strip() if isinstance(runtime_prompt_override, str) else None
        shared_prompt = metadata.get("shared_prompt")
        if isinstance(shared_prompt, str):
            shared_prompt = shared_prompt.strip()
        else:
            shared_prompt = None
        auto_run_default = bool(metadata.get("auto_run", True))

        default_team_name = "Parallel Agents"
        team_indices: dict[str, int] = {}
        team_agent_counts: dict[str, int] = {}
        agent_slots = []

        for seq_idx, agent in enumerate(agents, start=1):
            agent_name = agent.get("agent_name")
            if not agent_name:
                continue
            raw_team = agent.get("team")
            team_name = raw_team.strip() if isinstance(raw_team, str) else None
            if not team_name:
                team_name = default_team_name
            team_index = agent.get("team_index")
            if isinstance(team_index, int):
                team_indices.setdefault(team_name, team_index)
            else:
                team_index = team_indices.setdefault(team_name, len(team_indices) + 1)
            team_agent_counts.setdefault(team_name, 0)
            agent_index = agent.get("agent_index")
            if not isinstance(agent_index, int):
                team_agent_counts[team_name] += 1
                agent_index = team_agent_counts[team_name]

            raw_env = agent.get("env") if isinstance(agent.get("env"), dict) else {}
            normalized_env: dict[str, str] = {}
            for key, value in raw_env.items():
                normalized_env[str(key)] = "true" if isinstance(value, bool) and value else (
                    "false" if isinstance(value, bool) else str(value)
                )

            slot_prompt = agent.get("prompt")
            if isinstance(slot_prompt, str):
                slot_prompt = slot_prompt.strip()
            if runtime_prompt:
                prompt_to_use, prompt_source = runtime_prompt, "runtime"
            elif slot_prompt:
                prompt_to_use, prompt_source = slot_prompt, "agent"
            else:
                prompt_to_use = shared_prompt
                prompt_source = "shared" if shared_prompt else ""

            agent_auto = agent.get("auto_run")
            if not isinstance(agent_auto, bool):
                agent_auto = auto_run_default

            agent_slots.append({
                "agent_name": agent_name, "team_name": team_name,
                "prompt": prompt_to_use, "prompt_source": prompt_source,
                "env": normalized_env, "model": agent.get("model"),
                "auto_run": bool(agent_auto),
                "team_index": team_index, "agent_index": agent_index,
            })

        if not agent_slots:
            if main_terminal:
                main_terminal.write(f"[yellow]No valid agents found in {resolved_path}[/yellow]")
            return

        grid.remove_agent_terminals()
        await asyncio.sleep(0.05)
        stale = [n for n in sm.terminal_runners if n > 1]
        for n in stale:
            runner = sm.terminal_runners.pop(n)
            try:
                await runner.cancel_current_task()
            except Exception:
                pass
            AGENT_MANAGER.decrement_terminal_count()

        for idx in range(2, len(agent_slots) + 1):
            grid.add_agent_terminal(agent_slots[idx - 1]["agent_name"])
        await grid.wait_for_pending_mounts()
        await self._agent_ctl.wait_for_terminals_visible(list(range(1, len(agent_slots) + 1)))

        summary_lines = []
        self._terminal_agent_map = {}
        for idx, slot in enumerate(agent_slots, start=1):
            terminal = grid.get_terminal_by_number(idx)
            if not terminal:
                if main_terminal:
                    main_terminal.write(f"[red]Failed to provision terminal {idx} for {slot['agent_name']}[/red]")
                continue
            if hasattr(terminal, "state"):
                terminal.state.agent_name = slot["agent_name"]
                terminal.state.team_name = slot["team_name"]
                terminal._update_header()
            self._terminal_agent_map[idx] = slot["agent_name"]

            runner = sm.terminal_runners.get(idx)
            if not runner:
                runner = sm.add_terminal_runner(idx, terminal)
            else:
                runner.terminal = terminal
                runner.config.terminal_id = terminal.terminal_id
            runner.config.is_parallel = len(agent_slots) > 1
            parallel_cfg = ParallelConfig(
                slot["agent_name"], slot.get("model"), slot.get("prompt"), unified_context=False,
            )
            parallel_cfg.id = f"auto-{idx}"
            runner.config.parallel_config = parallel_cfg
            runner.config.agent_name = slot["agent_name"]
            runner.config.env = slot["env"] or None
            if slot["model"]:
                runner.config.model = slot["model"]
            await sm.initialize_terminal(idx)
            await sm.update_terminal_agent(idx, slot["agent_name"])
            if slot["model"]:
                await runner.update_model(slot["model"])
            env_summary = ", ".join(f"{k}={v}" for k, v in slot["env"].items()) or "default env"
            summary_lines.append(f"T{idx}: {slot['agent_name']} [{slot['team_name']}] ({env_summary})")

        sm.set_parallel_mode(len(agent_slots) > 1)
        if main_terminal:
            main_terminal.write("")
            origin_label = origin or "parallel_agents"
            header = f"[bold green]Startup agents loaded from {resolved_path}[/bold green]"
            if origin_label != "parallel_agents":
                header += f" ({origin_label})"
            main_terminal.write(header)
            desc = metadata.get("description")
            if isinstance(desc, str) and desc.strip():
                main_terminal.write(f"[dim]{desc.strip()}[/dim]")
            for line in summary_lines:
                main_terminal.write(f"[green]- {line}[/green]")
            if runtime_prompt:
                main_terminal.write(f"[cyan]Runtime prompt override applied:[/cyan] {runtime_prompt}")
            elif shared_prompt:
                main_terminal.write(f"[cyan]Shared prompt:[/cyan] {shared_prompt}")
            main_terminal.write("")

        try:
            await grid.wait_for_pending_mounts()
        except Exception:
            pass
        await self._agent_ctl.wait_for_terminals_visible(list(range(1, len(agent_slots) + 1)))
        self._startup_config_applied = True

        auto_run_slots = [
            (idx, slot) for idx, slot in enumerate(agent_slots, start=1)
            if slot.get("auto_run", auto_run_default) and slot.get("prompt")
        ]
        if auto_run_slots:
            await self._agent_ctl.wait_for_all_runners([i for i, _ in auto_run_slots])
            tasks = [
                asyncio.create_task(
                    self._run_auto_prompt(sm, idx, slot.get("prompt"),
                                         agent_name=slot.get("agent_name", f"T{idx}"),
                                         delay=0.05 * off)
                )
                for off, (idx, slot) in enumerate(auto_run_slots)
            ]
            results = await asyncio.gather(*tasks, return_exceptions=True)
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                for i, result in enumerate(results):
                    if isinstance(result, Exception):
                        si, sl = auto_run_slots[i]
                        mt.write(f"[red]Auto-run error for T{si}: {result}[/red]")

    async def _run_auto_prompt(self, sm, terminal_number, prompt, *, agent_name="", delay=0.0):
        """Execute an auto-run prompt."""
        try:
            if delay > 0:
                await asyncio.sleep(delay)
            runner = sm.terminal_runners.get(terminal_number)
            if not runner:
                return
            await self._agent_ctl.wait_for_runner_idle(runner)
            commands = []
            raw = (prompt or "").strip()
            if raw:
                if ";" in raw or "\n" in raw:
                    segs = [s.strip() for s in re.split(r"[;\n]", raw) if s.strip()]
                    if len(segs) > 1 and any(s.startswith(("$", "/")) for s in segs):
                        commands = segs
                if not commands:
                    commands = [raw]
            target = runner.terminal
            for i, cmd in enumerate(commands):
                if target:
                    target.write_command(cmd)
                if cmd.startswith("/") or cmd.startswith("$"):
                    if target:
                        self._handle_cli_command_with_terminal(cmd, target)
                    else:
                        self._handle_cli_command(cmd)
                else:
                    await runner.execute_command(cmd, show_command=False)
                if sm:
                    await sm._process_terminal_queue(terminal_number)
                if i < len(commands) - 1:
                    await asyncio.sleep(0.01)
        except Exception as exc:
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                mt.write(f"[red]Auto-run failed for T{terminal_number}: {exc}[/red]")

    async def _load_and_process_queue_file(self, queue_file: str) -> None:
        await asyncio.sleep(1.0)
        mt = self.terminal_grid.get_main_terminal()
        if not mt:
            return
        try:
            from cai.repl.commands.queue import load_queue_from_file
            queue_file = os.path.expanduser(queue_file)
            if not os.path.exists(queue_file):
                mt.write(f"[yellow]Queue file not found: {queue_file}[/yellow]")
                return
            mt.write(f"[dim]Loading queue from {queue_file}...[/dim]")
            loaded = load_queue_from_file(queue_file)
            if loaded > 0:
                mt.write(f"[cyan]Auto-loaded {loaded} prompts from CAI_QUEUE_FILE[/cyan]")
                self.sidebar.refresh_queue()
                await asyncio.sleep(0.5)
                if PROMPT_QUEUE.get_queue_size() > 0 and not PROMPT_QUEUE.is_processing():
                    asyncio.create_task(PROMPT_QUEUE._process_queue())
            else:
                mt.write(f"[yellow]No prompts loaded from {queue_file}[/yellow]")
        except Exception as e:
            import traceback
            mt.write(f"[red]Failed to load queue file: {e}[/red]")
            mt.write(f"[dim]{traceback.format_exc()}[/dim]")

    # =========================================================================
    # Mode switching
    # =========================================================================

    def _check_mode(self) -> None:
        new_mode = "parallel" if len(PARALLEL_CONFIGS) >= 2 else "single"
        if new_mode != self.current_mode:
            self.current_mode = new_mode
            self._update_mode(new_mode)
        elif new_mode == "parallel" and self.terminal_grid.terminal_count != len(PARALLEL_CONFIGS) + 1:
            self._update_mode(new_mode)

    def _update_mode(self, mode: str) -> None:
        mt = self.terminal_grid.get_main_terminal()
        if mode == "parallel":
            if mt:
                mt.write(f"\n[bold cyan]{'='*70}[/bold cyan]")
                mt.write(f"[bold cyan]PARALLEL MODE ACTIVATED - {len(PARALLEL_CONFIGS)} agents[/bold cyan]")
                mt.write(f"[bold cyan]{'='*70}[/bold cyan]\n")
            self.terminal_grid.remove_agent_terminals()
            for i in range(1, len(PARALLEL_CONFIGS)):
                name = PARALLEL_CONFIGS[i].agent_name if i < len(PARALLEL_CONFIGS) else f"Agent {i+1}"
                self.terminal_grid.add_agent_terminal(name)
            if self.session_manager:
                for idx, config in enumerate(PARALLEL_CONFIGS):
                    tn = idx + 1
                    terms = self.terminal_grid.active_terminals
                    if idx < len(terms):
                        term = terms[idx]
                        term.state.agent_name = config.agent_name
                        term._update_header()
                        if tn in self.session_manager.terminal_runners:
                            r = self.session_manager.terminal_runners[tn]
                            r.config.agent_name = config.agent_name
                            r.config.is_parallel = True
                            r.config.parallel_config = config
                        else:
                            r = self.session_manager.add_terminal_runner(tn, term)
                            r.config.agent_name = config.agent_name
                            r.config.is_parallel = True
                            r.config.parallel_config = config
                            asyncio.create_task(self._agent_ctl.initialize_terminal_runner(tn))
                self.session_manager.set_parallel_mode(True)
        else:
            self.terminal_grid.clear_agents()
            if mt:
                mt.write(f"\n[bold cyan]{'='*70}[/bold cyan]")
                mt.write("[bold cyan]SINGLE MODE ACTIVATED[/bold cyan]")
                mt.write(f"[bold cyan]{'='*70}[/bold cyan]\n")
            if self.session_manager:
                self.session_manager.set_parallel_mode(False)
        self.update_layout_indicator()

    # =========================================================================
    # Input submission -> routing (delegates to InputController)
    # =========================================================================

    @on(Input.Submitted, "#prompt-input-field")
    def on_input_submitted(self, event: Input.Submitted) -> None:
        command = event.value.strip()
        if not command:
            return
        pw = self.query_one("#main-input", PromptInput)
        pw.add_to_history(command)
        self._session.save_command(command)
        try:
            event.input.value = ""
            self.query_one("#prompt-input-field").value = ""
        except Exception:
            pass
        asyncio.create_task(self._process_command(command))
        try:
            pw.focus()
        except Exception:
            pass

    async def _process_command(self, command: str) -> None:
        """Central command dispatcher -- uses InputController for routing."""
        decision = self._input_ctl.route(command)
        kind = decision.kind

        if kind == RouteKind.NOP:
            return

        if kind == RouteKind.EXIT:
            self.exit()
            return

        if kind == RouteKind.DEBUG:
            self._debug_terminal_state()
            return

        if kind == RouteKind.META_AGENT:
            from cai.tui.meta_agent_controller import get_meta_agent_controller
            mc = get_meta_agent_controller()
            if mc:
                if not mc.tui_app_ref:
                    mc.set_tui_app(self)
                await mc.process_user_request_async(decision.command)
            return

        if kind == RouteKind.PARALLEL_PATTERN:
            mt = self.terminal_grid.get_main_terminal()
            await self._agent_ctl.handle_parallel_pattern(decision.pattern_num, mt)
            return

        # --- CLI commands ---
        if kind in (RouteKind.CLI_SINGLE, RouteKind.CLI_BROADCAST):
            await self._dispatch_cli(decision)
            return

        # --- Chat prompts ---
        if kind == RouteKind.CHAT_BROADCAST:
            await self._dispatch_chat_broadcast(decision)
            return

        if kind == RouteKind.CHAT_SELECT:
            self._show_agent_selector_for_prompt(decision.command)
            return

        if kind == RouteKind.CHAT_SINGLE:
            await self._dispatch_chat_single(decision)
            return

    # -- dispatch helpers (kept in App because they touch widgets) --------------

    async def _dispatch_cli(self, d) -> None:
        """Dispatch a CLI command to one or all terminals."""
        if d.broadcast:
            await self._broadcast_cli_command(d.command, d.active_agents)
        else:
            target = self._resolve_terminal(d.target_terminal_num, cli=True)
            if target:
                self._handle_cli_command_with_terminal(d.command, target)

    async def _dispatch_chat_single(self, d) -> None:
        target = self._resolve_terminal(d.target_terminal_num, cli=False)
        if target:
            if hasattr(target, "state") and target.state.agent_name:
                if self.session_manager:
                    tn = target.terminal_number
                    if tn not in self.session_manager.terminal_runners:
                        self.session_manager.add_terminal_runner(tn, target)
                        await self.session_manager.initialize_terminal(tn)
                    target.write_command(d.command)
                    await self.session_manager.execute_command(d.command, terminal_number=tn)
                    return
            else:
                target.write("[yellow]No agent loaded. Use /agent select <name>[/yellow]")
                return
        # Fallback: single active agent
        active = d.active_agents
        if len(active) == 1 and self.session_manager:
            tn = active[0][0]
            r = self.session_manager.terminal_runners.get(tn)
            if r:
                r.terminal.write_command(d.command)
            await self.session_manager.execute_command(d.command, terminal_number=tn)
        elif self.session_manager:
            self.session_manager.set_parallel_mode(False)
            await self.session_manager.execute_command(d.command, terminal_number=1)

    async def _dispatch_chat_broadcast(self, d) -> None:
        if not self.session_manager:
            return
        tasks, info = [], []
        for tn, name, _ in d.active_agents:
            if tn in self.session_manager.terminal_runners:
                self.session_manager.terminal_runners[tn].terminal.write_command(d.command)
                tasks.append(self.session_manager.execute_command(d.command, terminal_number=tn))
                info.append((tn, name))
        if tasks:
            results = await asyncio.gather(*tasks, return_exceptions=True)
            mt = self.terminal_grid.get_main_terminal()
            for i, r in enumerate(results):
                if isinstance(r, Exception) and mt:
                    mt.write(f"[red]Error in T{info[i][0]} ({info[i][1]}): {r}[/red]")

    async def _broadcast_cli_command(self, command, active_agents) -> None:
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(active_agents)) as pool:
            futs, infos = [], []
            for tn, name, _ in active_agents:
                term = self._find_terminal(tn)
                if term:
                    def _exec(t=term, n=tn, c=command):
                        os.environ["CAI_BROADCAST_MODE"] = "true"
                        try:
                            h = CommandHandler(t.output, n, t.terminal_id)
                            if hasattr(self.command_handler, "session_manager"):
                                h.session_manager = self.command_handler.session_manager
                            h.handle_command(c)
                        finally:
                            os.environ.pop("CAI_BROADCAST_MODE", None)
                    futs.append(pool.submit(_exec))
                    infos.append((tn, name))
            for i, f in enumerate(futs):
                try:
                    f.result(timeout=60)
                except Exception as e:
                    mt = self.terminal_grid.get_main_terminal()
                    if mt:
                        mt.write(f"[red]Error in T{infos[i][0]} ({infos[i][1]}): {e}[/red]")

    def _resolve_terminal(self, num, *, cli=False):
        """Resolve a target terminal by number, falling back to focused or main."""
        if num is not None:
            return self._find_terminal(num)
        if cli:
            t = self.terminal_grid.get_focused_terminal()
            return t if t else self.terminal_grid.get_main_terminal()
        return None

    def _find_terminal(self, num):
        for t in self.terminal_grid.active_terminals:
            if t.terminal_number == num:
                return t
        return None

    # =========================================================================
    # CLI command handlers (unchanged logic, just slimmer)
    # =========================================================================

    def _handle_cli_command_with_terminal(self, command: str, target_terminal) -> None:
        if command.strip() == "/agent new" or command.strip().startswith("/agent new "):
            try:
                self.query_one("#agent-creator-panel", AgentCreatorPanel).show()
                return
            except Exception:
                pass
        if target_terminal and target_terminal.terminal_number != 1:
            h = CommandHandler(target_terminal.output, target_terminal.terminal_number, target_terminal.terminal_id)
            if hasattr(self.command_handler, "session_manager"):
                h.session_manager = self.command_handler.session_manager
            h.handle_command(command)
        else:
            if self.command_handler:
                self.command_handler.handle_command(command)
        self.call_after_refresh(self._check_mode)
        # Sync agent manager after /agent commands
        if self.command_handler and self.command_handler.current_agent and command.startswith("/agent"):
            self.agent_manager.agent = self.command_handler.current_agent
            self.agent_manager.current_agent_name = self.command_handler.current_agent_name
            mt = self.terminal_grid.get_main_terminal()
            if mt and self.session_manager:
                mt.state.agent_name = self.command_handler.current_agent_name
                mt._update_header()
                if mt.terminal_number not in self.session_manager.terminal_runners:
                    self.session_manager.add_terminal_runner(mt.terminal_number, mt)
                    asyncio.create_task(self._agent_ctl.initialize_terminal_runner(mt.terminal_number))
                asyncio.create_task(
                    self.session_manager.update_terminal_agent(
                        mt.terminal_number, self.command_handler.current_agent_name
                    )
                )

    def _handle_cli_command(self, command: str) -> None:
        if self.command_handler:
            self.command_handler.handle_command(command)

    # =========================================================================
    # Sidebar / widget event handlers
    # =========================================================================

    @on(AgentDoubleClicked)
    def on_agent_double_clicked(self, event: AgentDoubleClicked) -> None:
        name = event.agent_name
        if not name:
            return
        tc = len(self.terminal_grid.terminals)
        self.terminal_grid.add_agent_terminal(name)
        new = [t for t in self.terminal_grid.active_terminals if t.terminal_number == tc + 1]
        if new and self.session_manager:
            nt = new[0]
            self.session_manager.add_terminal_runner(nt.terminal_number, nt)
            asyncio.create_task(self.session_manager.update_terminal_agent(nt.terminal_number, name))
            self.terminal_grid.focus_terminal(nt.terminal_id)
            nt.write(f"[bold green]Agent '{name}' spawned in Terminal {nt.terminal_number}[/bold green]\n")

    @on(TeamSelected)
    def on_team_selected(self, event: TeamSelected) -> None:
        try:
            agents = list(event.agents or [])
            if agents:
                asyncio.create_task(
                    self._agent_ctl.apply_team_selection(
                        event.team_name, agents, switch_tab_callback=self.switch_to_tab,
                    )
                )
        except Exception as e:
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                mt.write(f"[red]Error applying team: {type(e).__name__}: {e}[/red]")

    @on(AgentSelectionConfirmed)
    def on_agent_selection_confirmed(self, event: AgentSelectionConfirmed) -> None:
        tam = getattr(self, "_terminal_agent_map", {})
        if self.session_manager:
            for dn in event.selected_agents:
                if dn in tam:
                    tn, _ = tam[dn]
                    asyncio.create_task(self.session_manager.execute_command(event.prompt, terminal_number=tn))

    @on(AgentSelectionCancelled)
    def on_agent_selection_cancelled(self, event: AgentSelectionCancelled) -> None:
        pass

    @on(AgentCreationConfirmed)
    def on_agent_creation_confirmed(self, event: AgentCreationConfirmed) -> None:
        from cai.agents.agent_builder import AgentBuilder
        mt = self.terminal_grid.get_main_terminal()
        if mt:
            try:
                fp = AgentBuilder.save_agent_file(event.agent_config)
                mt.write(f"\n[green]Agent created successfully![/green]\n[green]File: {fp}[/green]")
            except Exception as e:
                mt.write(f"\n[red]Error creating agent: {e}[/red]")

    @on(AgentCreationCancelled)
    def on_agent_creation_cancelled(self, event: AgentCreationCancelled) -> None:
        pass

    # =========================================================================
    # Tab, sidebar, and UI actions
    # =========================================================================

    def switch_to_tab(self, tab_id: str) -> None:
        self.query_one("#main-tabs", TabbedContent).active = tab_id
        self.current_view = tab_id
        if tab_id == "terminal":
            self.set_timer(0.1, lambda: self.query_one("#main-input").focus())

    def action_show_terminal(self) -> None:
        self.switch_to_tab("terminal")

    def action_show_ctr(self) -> None:
        self.switch_to_tab("ctr")

    def action_cycle_tabs(self) -> None:
        nxt = self._state.tab_state.cycle()
        self.switch_to_tab(nxt.value)
        update_tab_appearance(self.query_one, nxt.value)

    def action_toggle_sidebar(self) -> None:
        if not self.sidebar:
            try:
                self.sidebar = self.query_one("#sidebar", Sidebar)
            except Exception:
                pass
        if self.sidebar:
            self.sidebar.toggle()
            self.sidebar_visible = self.sidebar.visible
        else:
            def _deferred():
                s = self.query_one("#sidebar", Sidebar)
                s.toggle()
                self.sidebar_visible = s.visible
            self.call_after_refresh(_deferred)

    def watch_sidebar_visible(self, visible: bool) -> None:
        try:
            tb = self.query_one("#top-bar", Container)
            tb.remove_class("sidebar-collapsed") if visible else tb.add_class("sidebar-collapsed")
        except Exception:
            pass

    def action_cycle_theme(self) -> None:
        nxt = self._state.next_theme(getattr(self, "theme", None))
        try:
            self.theme = nxt
            _tui_config.set_theme(nxt)
        except Exception:
            pass

    def on_click(self, event) -> None:
        if hasattr(event, "widget"):
            wid = event.widget.id
            if wid == "sidebar-toggle-btn":
                self.action_toggle_sidebar()
                event.stop()
            elif wid == "app-close-btn":
                self.action_quit()
                event.stop()
            elif wid == "add-terminal-btn":
                asyncio.create_task(self._agent_ctl.add_terminal_with_defaults())
                event.stop()

    @on(Button.Pressed, "#tab-terminal-btn")
    def on_terminal_tab_pressed(self, event: Button.Pressed) -> None:
        self.switch_to_tab("terminal")
        update_tab_appearance(self.query_one, "terminal")
        event.stop()

    @on(Button.Pressed, "#tab-graph-btn")
    def on_graph_tab_pressed(self, event: Button.Pressed) -> None:
        self.switch_to_tab("ctr")
        update_tab_appearance(self.query_one, "ctr")
        event.stop()

    @on(Button.Pressed, "#tab-help-btn")
    def on_help_tab_pressed(self, event: Button.Pressed) -> None:
        self.switch_to_tab("help")
        update_tab_appearance(self.query_one, "help")
        event.stop()

    def _show_agent_selector_for_prompt(self, prompt: str) -> None:
        available, tam = [], {}
        for t in self.terminal_grid.active_terminals:
            if t.state.agent_name:
                dn = f"{t.state.agent_name} (Terminal {t.terminal_number})"
                available.append(dn)
                tam[dn] = (t.terminal_number, t.state.agent_name)
        if not available:
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                mt.write("[yellow]No terminals have agents loaded.[/yellow]")
            return
        self._terminal_agent_map = tam
        try:
            self.query_one("#agent-selector-panel", AgentSelectorPanel).show_for_prompt(prompt, available)
        except Exception as e:
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                mt.write(f"[red]ERROR showing agent selector: {e}[/red]")

    # =========================================================================
    # Keyboard actions
    # =========================================================================

    def action_clear(self) -> None:
        self.terminal_grid.clear_all()

    def action_parallel_prompt(self) -> None:
        if self.current_mode != "parallel":
            mt = self.terminal_grid.get_main_terminal()
            if mt:
                mt.write("[yellow]Not in parallel mode[/yellow]")
            return
        asyncio.create_task(self.terminal_grid.broadcast_command("Tell me about your capabilities", "agent"))

    def action_next_terminal(self) -> None:
        self.terminal_grid.cycle_focus(forward=True)

    def action_prev_terminal(self) -> None:
        self.terminal_grid.cycle_focus(forward=False)

    def action_cancel_all(self) -> None:
        if self._state.record_esc():
            self.exit()
        else:
            if not self._cancelling:
                self._cancelling = True
                if self._cancel_task and not self._cancel_task.done():
                    return
                self._cancel_task = asyncio.create_task(self._do_cancel_all())

    async def _do_cancel_all(self) -> None:
        try:
            await self._agent_ctl.cancel_all_agents()
        finally:
            self._cancelling = False

    def action_cancel_selected(self) -> None:
        asyncio.create_task(self._agent_ctl.cancel_selected_agent())

    def action_close_terminal(self) -> None:
        self._agent_ctl.close_focused_terminal()
        if self._state.terminal_grid is not None:
            self.update_layout_indicator()

    def action_clear_input(self) -> None:
        if self.prompt_input:
            self.prompt_input.clear()
            self.prompt_input.focus()

    def action_toggle_terminal_view(self) -> None:
        if not self.terminal_grid:
            return
        if self.terminal_grid.is_showing_only_focused():
            self.terminal_grid.show_all_terminals()
        else:
            self.terminal_grid.show_only_focused()

    def action_show_queue(self) -> None:
        status = PROMPT_QUEUE.get_queue_status()
        mt = self.terminal_grid.get_main_terminal()
        if mt:
            mt.write("[bold cyan]Prompt Queue Status[/bold cyan]")
            mt.write(f"[cyan]Queue Length:[/cyan] {status['queue_length']}")
            mt.write(f"[cyan]Processing:[/cyan] {'Yes' if status['processing'] else 'No'}")
            if status["prompts"]:
                for i, p in enumerate(status["prompts"], 1):
                    mt.write(f"  {i}. {p['prompt']} (priority: {p['priority']})")
            else:
                mt.write("[dim]No prompts queued[/dim]")
            mt.write("")

    def action_quit(self) -> None:
        asyncio.create_task(self._agent_ctl.cleanup_before_exit())

    # =========================================================================
    # Misc helpers
    # =========================================================================

    async def _process_queued_prompt(self, prompt, terminal_number=None):
        try:
            if self.session_manager:
                for r in self.session_manager.terminal_runners.values():
                    if r.is_running:
                        await PROMPT_QUEUE.add_prompt(prompt, terminal_number)
                        return
            await self._process_command(prompt)
        except Exception:
            pass

    def update_layout_indicator(self) -> None:
        tc = self.terminal_grid.terminal_count
        lm = self.terminal_grid.layout_mode.upper()
        self.title = f"CAI Terminal - {lm} Layout | Terminals: {tc}"

    def on_unmount(self, event: Unmount) -> None:
        if self.session_manager:
            self.session_manager.cleanup()

    def _debug_terminal_state(self) -> None:
        mt = self.terminal_grid.get_main_terminal()
        if not mt:
            return
        mt.write("[bold cyan]=== TERMINAL STATE DEBUG ===[/bold cyan]")
        mt.write(f"Total terminals: {len(self.terminal_grid.terminals)}")
        for tid, terminal in self.terminal_grid.terminals.items():
            mt.write(f"\nTerminal {terminal.terminal_number}:")
            mt.write(f"  - ID: {tid}")
            mt.write(f"  - Agent: {terminal.state.agent_name or 'None'}")
            if self.session_manager:
                mt.write(f"  - In session mgr: {terminal.terminal_number in self.session_manager.terminal_runners}")
        mt.write("[bold cyan]==========================[/bold cyan]\n")

    # -- Session summary helpers (used from run_cai_tui) -----------------------

    def _compute_session_time_seconds(self, active=None, idle=None):
        return SessionState.compute_session_time(active, idle, self._session.elapsed_seconds())

    @staticmethod
    def _ensure_time_breakdown(session_time, active, idle):
        return SessionState.ensure_time_breakdown(session_time, active, idle)


# =============================================================================
# Entry point
# =============================================================================

def run_cai_tui():
    """Entry point for CAI TUI."""
    try:
        from cai.util_ext import check_system_dependencies, display_missing_dependencies_error
        all_ok, missing = check_system_dependencies()
        if not all_ok:
            display_missing_dependencies_error(missing)
            return
    except Exception:
        pass

    try:
        from cai.util_ext import _chk
        if not _chk():
            from rich.console import Console
            from rich.panel import Panel
            Console(stderr=True).print(
                Panel("[bold red]ALIAS_API_KEY is invalid or not set[/bold red]",
                      title="[red]Authentication Error[/red]", border_style="red")
            )
            return
    except Exception:
        pass

    os.environ["CAI_TUI_MODE"] = "true"
    os.environ["TEXTUAL_MOUSE"] = "1"

    from cai.util.cli_session_clock import reset_session_clock

    reset_session_clock()

    try:
        from cai.repl.ui.terminal_title import set_terminal_window_title

        set_terminal_window_title()
    except Exception:
        pass

    try:
        integrate_openai_chatcompletions_display()
    except ImportError:
        pass

    if os.environ.get("CAI_META_AGENT", "false").lower() == "true":
        from cai.tui.meta_agent_controller import get_meta_agent_controller
        get_meta_agent_controller()

    app = CAITerminal()
    app.run()

    try:
        from cai.repl.ui.terminal_title import restore_terminal_window_title

        restore_terminal_window_title()
    except Exception:
        pass

    # -- Post-exit session summary --
    os.environ["CAI_COST_DISPLAYED"] = "true"
    try:
        import sys, time as _t
        _t.sleep(0.1)
        sys.stdout.flush()
        sys.stderr.flush()

        active_s: Optional[float] = None
        idle_s: Optional[float] = None
        cost = 0.0
        try:
            from cai.util import COST_TRACKER, get_active_time_seconds, get_idle_time_seconds
            active_s = get_active_time_seconds()
            idle_s = get_idle_time_seconds()
            cost = COST_TRACKER.session_total_cost
        except Exception:
            pass

        session_time = app._compute_session_time_seconds(active_s, idle_s)
        active_s, idle_s = app._ensure_time_breakdown(session_time, active_s, idle_s)
        fmt = SessionState.format_time

        log_path = None
        try:
            from cai.sdk.agents.run_to_jsonl import get_session_recorder
            sr = get_session_recorder()
            if hasattr(sr, "filename"):
                log_path = sr.filename
        except Exception:
            pass

        from rich.box import ROUNDED
        from rich.console import Console, Group
        from rich.panel import Panel
        from rich.text import Text

        from cai.repl.ui.banner import CAI_GREEN, session_summary_panel_title

        _body = "dim white"
        pct = round(active_s / session_time * 100, 1) if session_time else 0
        parts = [
            Text(f"Session Time: {fmt(session_time)}", style=_body),
            Text(f"Active Time: {fmt(active_s)} ({pct}%)", style=_body),
            Text(f"Idle Time: {fmt(idle_s)}", style=_body),
        ]
        cost_line = Text()
        cost_line.append("Total Session Cost:", style=_body)
        cost_line.append(" ", style=_body)
        cost_line.append(f"${cost:.6f}", style=f"bold {CAI_GREEN}")
        parts.append(cost_line)
        if log_path:
            parts.append(Text("Log available at:", style=_body))
            parts.append(Text(log_path, style=_body))
        Console().print(
            Panel(
                Group(*parts),
                border_style=CAI_GREEN,
                box=ROUNDED,
                padding=(1, 1),
                title=session_summary_panel_title(),
                title_align="left",
            )
        )
    except Exception as e:
        print(f"\nError displaying session summary: {e}")
