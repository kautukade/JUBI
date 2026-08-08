"""
AgentController -- agent lifecycle operations extracted from CAITerminal.

Manages adding/removing terminals, initializing runners, cancelling agents,
startup-config application, parallel pattern handling, team selection, and
cleanup.  Operates on a :class:`TUIState` instance rather than
reaching into the Textual App directly, keeping the controller testable
in isolation.

Methods here were originally inlined in ``cai_terminal.py``; the App now
delegates to ``AgentController`` for all agent-lifecycle work.
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
import time
from typing import TYPE_CHECKING, Any, Dict, List, Optional

if TYPE_CHECKING:
    from cai.tui.model.state import TUIState

_log = logging.getLogger("AgentController")


class AgentController:
    """Thin controller that owns agent-lifecycle operations.

    Parameters
    ----------
    state:
        The shared :class:`TUIState` instance that holds widget references
        and mutable flags.
    exit_callback:
        A callable (typically ``App.exit``) invoked when the controller
        decides the application should terminate.
    """

    def __init__(self, state: "TUIState", exit_callback: Any = None) -> None:
        self._state = state
        self._exit = exit_callback  # App.exit or a test stub

    # -- public API -----------------------------------------------------------

    async def add_terminal_with_defaults(
        self,
        default_agent: str = "redteam_agent",
        default_model: str = "alias1",
    ) -> None:
        """Add a new terminal pre-configured with *default_agent* / *default_model*."""
        grid = self._state.terminal_grid
        sm = self._state.session_manager
        if grid is None:
            return

        try:
            new_terminal = grid.add_agent_terminal(default_agent)
            if new_terminal is None or sm is None:
                return

            runner = sm.add_terminal_runner(
                new_terminal.terminal_number, new_terminal
            )
            runner.config.model = default_model

            await sm.initialize_terminal(new_terminal.terminal_number)
            await sm.update_terminal_agent(
                new_terminal.terminal_number, default_agent
            )

            if hasattr(new_terminal, "update_model_display"):
                new_terminal.update_model_display(default_model)
        except Exception as exc:
            self._write_main(f"[red]Error adding new terminal: {exc}[/red]")

    async def initialize_terminal_runner(self, terminal_number: int) -> None:
        """Initialise the runner for *terminal_number* via the session manager."""
        sm = self._state.session_manager
        if sm is not None:
            await sm.initialize_terminal(terminal_number)

    async def cancel_all_agents(self) -> None:
        """Cancel every running agent across all terminals."""
        sm = self._state.session_manager
        if sm is None:
            return
        try:
            await sm.cancel_all_tasks()
        except asyncio.CancelledError:
            pass  # expected during cancellation
        except RuntimeError as exc:
            if "cannot reuse already awaited coroutine" not in str(exc):
                _log.error("Runtime error cancelling agents: %s", exc)
        except Exception as exc:
            _log.error("Error cancelling agents: %s", exc)

    async def cancel_selected_agent(self) -> None:
        """Cancel execution in the currently focused terminal."""
        grid = self._state.terminal_grid
        sm = self._state.session_manager

        if grid is None or sm is None:
            if self._state.prompt_input is not None:
                self._state.prompt_input.clear()
            return

        focused = grid.get_focused_terminal()
        if focused is None:
            if self._state.prompt_input is not None:
                self._state.prompt_input.clear()
            return

        tn = focused.terminal_number
        if tn in sm.terminal_runners:
            runner = sm.terminal_runners[tn]
            if runner.is_running:
                await runner.cancel_current_task()
                focused.write(
                    f"[yellow]Cancelled execution in Terminal {tn}[/yellow]"
                )
            else:
                focused.write(
                    f"[dim]No active execution in Terminal {tn}[/dim]"
                )

    def close_focused_terminal(self) -> None:
        """Close the currently focused terminal (unless it is the main one)."""
        grid = self._state.terminal_grid
        sm = self._state.session_manager
        if grid is None:
            return

        focused = grid.get_focused_terminal()
        if focused is None:
            return

        if focused.terminal_id == grid.main_terminal_id:
            focused.write("[yellow]Cannot close the main terminal (T1)[/yellow]")
            return

        if grid.terminal_count <= 1:
            focused.write("[yellow]Cannot close the only terminal[/yellow]")
            return

        tn = focused.terminal_number
        tid = focused.terminal_id

        # Clean up runner / agent before removing the widget.
        if sm is not None and tn in sm.terminal_runners:
            runner = sm.terminal_runners[tn]
            asyncio.create_task(runner.cleanup())
            del sm.terminal_runners[tn]

            try:
                from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

                AGENT_MANAGER.decrement_terminal_count()
                AGENT_MANAGER.cleanup_orphaned_parallel_agents()
            except Exception:
                pass

        if grid.remove_terminal(tid):
            self._write_main(f"[red]Closed Terminal {tn}[/red]")
            try:
                from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

                AGENT_MANAGER.cleanup_tui_orphaned_agents()
            except Exception:
                pass
        else:
            focused.write("[red]Failed to close terminal[/red]")

    async def cleanup_before_exit(self) -> None:
        """Cancel running tasks, tear down session, and exit the app."""
        try:
            sm = self._state.session_manager
            if sm is not None:
                await sm.cancel_all_tasks()
                sm.cleanup()
        except Exception as exc:
            _log.error("Error during cleanup: %s", exc)
        finally:
            os.environ.pop("CAI_TUI_MODE", None)
            if self._exit is not None:
                self._exit()

    # -- parallel pattern handling -------------------------------------------

    async def handle_parallel_pattern(self, pattern_num: int, initial_terminal: Any) -> None:
        """Handle parallel pattern selection (agent numbers >= 20).

        Spawns additional terminals for each agent in the pattern.
        """
        grid = self._state.terminal_grid
        sm = self._state.session_manager
        main_terminal = grid.get_main_terminal() if grid else None

        try:
            from cai.agents.patterns import get_parallel_patterns
            parallel_patterns = get_parallel_patterns()
            pattern_list = list(parallel_patterns.values())
            pattern_idx = pattern_num - 19

            if pattern_idx < 1 or pattern_idx > len(pattern_list):
                if main_terminal:
                    main_terminal.write(f"[red]Pattern {pattern_num} not found[/red]")
                return

            pattern = pattern_list[pattern_idx - 1]

            agents: List[Dict[str, str]] = []
            if hasattr(pattern, "configs"):
                for config in pattern.configs:
                    try:
                        agents.append({"agent": config.agent_name})
                    except AttributeError:
                        agents.append({"agent": str(config)})
            elif hasattr(pattern, "agents"):
                for agent in pattern.agents:
                    agents.append({"agent": getattr(agent, "name", str(agent))})

            if not agents:
                return

            # Load first agent in initial terminal
            if initial_terminal and agents:
                first_agent = agents[0]["agent"]
                tn = initial_terminal.terminal_number
                if sm and tn not in sm.terminal_runners:
                    sm.add_terminal_runner(tn, initial_terminal)
                    await self.initialize_terminal_runner(tn)
                if sm:
                    await sm.update_terminal_agent(tn, first_agent)

            start_idx = 1 if initial_terminal else 0
            for i, agent_config in enumerate(agents[start_idx:], start_idx):
                agent_name = agent_config["agent"]
                if grid:
                    grid.add_agent_terminal(agent_name)
                    new_terminal = self._find_terminal_by_number(
                        grid, len(grid.terminals)
                    )
                    if new_terminal and sm:
                        sm.add_terminal_runner(new_terminal.terminal_number, new_terminal)
                        await self.initialize_terminal_runner(new_terminal.terminal_number)
                        await sm.update_terminal_agent(new_terminal.terminal_number, agent_name)

        except ImportError as exc:
            if main_terminal:
                main_terminal.write(f"[red]Could not import parallel patterns: {exc}[/red]")
        except Exception as exc:
            if main_terminal:
                main_terminal.write(
                    f"[red]Error in handle_parallel_pattern: {type(exc).__name__}: {exc}[/red]"
                )

    # -- team selection -------------------------------------------------------

    async def apply_team_selection(
        self,
        team_name: str,
        team_agents: List[str],
        switch_tab_callback: Any = None,
    ) -> None:
        """Open/reuse up to 4 terminals and assign agents from *team_agents*."""
        desired_n = min(4, len(team_agents))
        grid = self._state.terminal_grid
        sm = self._state.session_manager
        if grid is None or sm is None:
            return

        if switch_tab_callback:
            try:
                switch_tab_callback("terminal")
            except Exception:
                pass

        current_n = len(grid.terminals)
        for i in range(current_n, desired_n):
            next_agent = team_agents[min(i, len(team_agents) - 1)]
            grid.add_agent_terminal(next_agent)
            await asyncio.sleep(0)

        all_terms = sorted(grid.active_terminals, key=lambda t: t.terminal_number)
        for idx in range(desired_n):
            term_num = idx + 1
            term = next((t for t in all_terms if t.terminal_number == term_num), None)
            if not term:
                continue
            agent_for_term = team_agents[idx if idx < len(team_agents) else -1]
            if term_num not in sm.terminal_runners:
                sm.add_terminal_runner(term_num, term)
            await sm.update_terminal_agent(term_num, agent_for_term)

            # Sync TUI dropdown (best-effort)
            try:
                from cai.repl.commands.agent import _sync_tui_agent_selection
                old_env = os.getenv("CAI_ACTIVE_COMMAND_TERMINAL", "")
                os.environ["CAI_ACTIVE_COMMAND_TERMINAL"] = str(term_num)
                _sync_tui_agent_selection(agent_for_term)
                if old_env:
                    os.environ["CAI_ACTIVE_COMMAND_TERMINAL"] = old_env
                else:
                    os.environ.pop("CAI_ACTIVE_COMMAND_TERMINAL", None)
            except Exception:
                pass

        main_term = grid.get_main_terminal()
        if main_term:
            grid.focus_terminal(main_term.terminal_id)
            mapping = []
            for i in range(1, desired_n + 1):
                eff = team_agents[i - 1] if i - 1 < len(team_agents) else team_agents[-1]
                mapping.append(f"T{i}->{eff}")
            main_term.write(f"[bold green]Team selected: {team_name}[/bold green]")
            main_term.write("Assignment: " + ", ".join(mapping))
            main_term.write("")

    # -- runner wait helpers --------------------------------------------------

    async def wait_for_runner_idle(self, runner: Any, timeout: float = 5.0) -> None:
        """Wait until *runner* is idle and initialized."""
        if not runner:
            return
        start = time.monotonic()
        while runner.is_running or runner.agent is None:
            if time.monotonic() - start >= timeout:
                return
            await asyncio.sleep(0.05)

    async def wait_for_all_runners(
        self,
        terminal_numbers: List[int],
        timeout: float = 5.0,
    ) -> None:
        """Wait until all specified terminal runners are ready."""
        sm = self._state.session_manager
        if not sm or not terminal_numbers:
            return
        start = time.monotonic()
        pending = set(terminal_numbers)
        while pending:
            done = []
            for tn in list(pending):
                runner = sm.terminal_runners.get(tn)
                if runner and runner.agent is not None and not runner.is_running:
                    done.append(tn)
            for tn in done:
                pending.discard(tn)
            if not pending:
                return
            if time.monotonic() - start >= timeout:
                self._write_main(
                    f"[yellow]Auto-run warning: timeout waiting for terminals "
                    f"{sorted(pending)} to initialize.[/yellow]"
                )
                return
            await asyncio.sleep(0.05)

    async def wait_for_terminals_visible(
        self,
        terminal_numbers: List[int],
        timeout: float = 3.0,
    ) -> None:
        """Wait until terminals are mounted and visible."""
        grid = self._state.terminal_grid
        if not grid or not terminal_numbers:
            return
        start = time.monotonic()
        pending = set(terminal_numbers)
        while pending:
            done = []
            for tn in list(pending):
                terminal = grid.get_terminal_by_number(tn)
                if terminal and terminal.is_mounted and getattr(terminal, "output", None):
                    done.append(tn)
            for tn in done:
                pending.discard(tn)
            if not pending:
                return
            if time.monotonic() - start >= timeout:
                self._write_main(
                    f"[yellow]Auto-run warning: terminals {sorted(pending)} "
                    f"not visible after {timeout:.1f}s.[/yellow]"
                )
                return
            await asyncio.sleep(0.05)

    # -- internal helpers -----------------------------------------------------

    def _write_main(self, markup: str) -> None:
        """Write *markup* to the main terminal, if available."""
        grid = self._state.terminal_grid
        if grid is None:
            return
        try:
            main = grid.get_main_terminal()
            if main is not None:
                main.write(markup)
        except Exception:
            pass

    @staticmethod
    def _find_terminal_by_number(grid: Any, number: int) -> Any:
        """Find a terminal widget by number in the grid."""
        for t in grid.active_terminals:
            if t.terminal_number == number:
                return t
        return None
