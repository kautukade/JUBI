"""GCTR (Game-theoretic Cut The Rope) mixin for agents.

Provides CTRHooks and a factory function to wrap any base agent
with GCTR capabilities.  All four GCTR agent files
(red_teamer_gctr, blue_teamer_gctr, purple_teamer_gctr,
bug_bounter_gctr) delegate to this module for the shared logic.
"""

import asyncio
import json
import os
import re
from typing import List, Optional

from rich.console import Console

from cai.ctr.paths import get_ctr_output_base_dir
from cai.sdk.agents import Agent
from cai.sdk.agents.lifecycle import AgentHooks
from cai.sdk.agents.run_context import RunContextWrapper
from cai.sdk.agents.run_to_jsonl import get_session_recorder

console = Console()


# ---------------------------------------------------------------------------
# Helper utilities shared by display logic
# ---------------------------------------------------------------------------

def _normalize_paths(raw_paths):
    """Return a list of path metadata dictionaries."""
    normalized = []
    if not raw_paths:
        return normalized

    for idx, entry in enumerate(raw_paths, 1):
        name = f"Path {idx}"
        sequence = []
        probability = None

        if isinstance(entry, dict):
            sequence = entry.get('sequence') or entry.get('nodes') or entry.get('path') or []
            if isinstance(sequence, str):
                parts = re.split(r"\s*(?:->|\u2192|,)\s*", sequence.strip())
                sequence = [part for part in parts if part]
            sequence = [str(node) for node in sequence]
            name = entry.get('name') or entry.get('id') or entry.get('label') or name
            probability = entry.get('probability')
        elif isinstance(entry, (list, tuple)):
            sequence = [str(node) for node in entry]
        else:
            sequence = [str(entry)]

        normalized.append({
            'id': idx,
            'name': name,
            'sequence': sequence,
            'probability': probability,
        })

    return normalized


def _parse_probability_sequence(strategy_value):
    """Convert attacker strategy variants into a float list."""
    if strategy_value is None:
        return []
    if isinstance(strategy_value, dict):
        return []
    if isinstance(strategy_value, (list, tuple)):
        try:
            return [float(v) for v in strategy_value]
        except (TypeError, ValueError):
            return []
    if isinstance(strategy_value, str):
        values = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", strategy_value)
        try:
            return [float(v) for v in values]
        except ValueError:
            return []
    return []


# ---------------------------------------------------------------------------
# Core CTRHooks class (single-agent variant)
# ---------------------------------------------------------------------------

class CTRHooks(AgentHooks):
    """Hooks to integrate CTR analysis into agent execution."""

    def __init__(self, n_interactions: int = 5, team_label: str = "Agent"):
        self.n_interactions = n_interactions
        self.team_label = team_label
        self.interaction_counter = 0
        self.last_ctr_results = None
        self._ctr_output_dir = None
        self._agent_context = None
        self._tool_count = 0
        self._ctr_task = None
        self._ctr_running = False

    async def on_start(self, context: RunContextWrapper, agent: Agent) -> None:
        console.print(f"[bold cyan]CTR-Enhanced {self.team_label} Started[/bold cyan]")
        console.print(
            f"[yellow]Will analyze security game dynamics every "
            f"{self.n_interactions} tool uses (non-blocking)[/yellow]"
        )
        self._agent_context = agent

    async def on_end(self, context: RunContextWrapper, agent: Agent, final_output) -> None:
        if self._ctr_task and not self._ctr_task.done():
            console.print("[yellow]Waiting for background CTR analysis to complete...[/yellow]")
            try:
                await asyncio.wait_for(self._ctr_task, timeout=30.0)
            except asyncio.TimeoutError:
                console.print("[red]CTR background task timed out, cancelling...[/red]")
                self._ctr_task.cancel()
            except Exception as e:
                console.print(f"[red]Error waiting for CTR task: {e}[/red]")

    async def on_tool_start(self, context: RunContextWrapper, agent: Agent, tool) -> None:
        tool_name = getattr(tool, 'name', 'unknown')
        console.print(f"[dim]CTR: Tool '{tool_name}' starting...[/dim]")

    async def on_tool_end(self, context: RunContextWrapper, agent: Agent, tool, result: str) -> None:
        self._agent_context = agent
        tool_name = getattr(tool, 'name', 'unknown')

        self._tool_count += 1
        self.interaction_counter += 1
        console.print(
            f"[green]CTR: Tool '{tool_name}' completed "
            f"({self.interaction_counter}/{self.n_interactions})[/green]"
        )

        if self.interaction_counter >= self.n_interactions:
            console.print("[bold yellow]CTR ANALYSIS THRESHOLD REACHED![/bold yellow]")

            if not self._ctr_running:
                in_memory_history = _snapshot_history(agent)
                token_counts = _snapshot_tokens(agent)

                console.print("[dim]Launching CTR analysis in background (non-blocking)...[/dim]")
                self._ctr_running = True
                self._ctr_task = asyncio.create_task(
                    self._run_ctr_background(in_memory_history, token_counts)
                )
            else:
                console.print("[dim]CTR analysis already running in background, skipping this cycle...[/dim]")

            self.interaction_counter = 0

    # ---- background analysis ----

    async def _run_ctr_background(self, in_memory_history, token_counts) -> None:
        try:
            if not in_memory_history:
                console.print("[yellow]CTR: No conversation history available for analysis[/yellow]")
                return

            console.print(
                "\n[bold cyan]=== Automated CTR Security Analysis Triggered (Background) ===[/bold cyan]"
            )
            console.print("[yellow]Running game-theoretic security analysis in parallel...[/yellow]\n")

            self._ctr_output_dir = _ensure_ctr_output_dir(self._ctr_output_dir)

            from cai.ctr import experiment as ctr_experiment
            await ctr_experiment.run(
                messages=in_memory_history,
                token_counts=token_counts,
                output_base_dir=self._ctr_output_dir,
            )

            self.last_ctr_results = _find_latest_run(self._ctr_output_dir)
            if self.last_ctr_results:
                from cai.ctr.digest import mark_ctr_run_complete
                mark_ctr_run_complete(self.last_ctr_results)
                await display_and_inject_results(self.last_ctr_results, self._agent_context)

        except Exception as e:
            console.print(f"[red]CTR Background Error: {e}[/red]")
            import traceback
            console.print(f"[dim]{traceback.format_exc()}[/dim]")
        finally:
            self._ctr_running = False
            console.print("[dim]CTR background analysis complete, agent can continue...[/dim]\n")


# ---------------------------------------------------------------------------
# SharedCTRHooks class (purple / multi-agent variant)
# ---------------------------------------------------------------------------

class SharedCTRHooks(AgentHooks):
    """Shared hooks tracking combined tool usage across multiple agents."""

    def __init__(self, n_interactions: int = 5, team_name: str = "Unknown"):
        self.n_interactions = n_interactions
        self.team_name = team_name
        self.interaction_counter = 0
        self.last_ctr_results = None
        self._ctr_output_dir = None
        self._agent_context = None
        self._tool_count = 0
        self._ctr_task = None
        self._ctr_running = False
        self._lock = asyncio.Lock()
        self._tracked_agents: List[Agent] = []

    async def on_start(self, context: RunContextWrapper, agent: Agent) -> None:
        async with self._lock:
            if agent not in self._tracked_agents:
                self._tracked_agents.append(agent)
            if len(self._tracked_agents) == 1:
                console.print("[bold magenta]Purple Team GCTR Coordination Started[/bold magenta]")
                console.print(
                    f"[yellow]Tracking combined red/blue team activity - "
                    f"will analyze every {self.n_interactions} tool uses (non-blocking)[/yellow]"
                )
            else:
                console.print(f"[bold cyan]{self.team_name} Team joined purple team coordination[/bold cyan]")
            self._agent_context = agent

    async def on_end(self, context: RunContextWrapper, agent: Agent, final_output) -> None:
        await self._cleanup_background_tasks()

    async def _cleanup_background_tasks(self):
        if self._ctr_task and not self._ctr_task.done():
            console.print("[yellow]Cancelling background CTR analysis...[/yellow]")
            self._ctr_task.cancel()
            try:
                await self._ctr_task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
            finally:
                self._ctr_running = False
                self._ctr_task = None

    async def on_tool_start(self, context: RunContextWrapper, agent: Agent, tool) -> None:
        pass

    async def on_tool_end(self, context: RunContextWrapper, agent: Agent, tool, result: str) -> None:
        async with self._lock:
            self._tool_count += 1
            self.interaction_counter += 1

            if self.interaction_counter >= self.n_interactions:
                console.print("[bold yellow]PURPLE TEAM CTR ANALYSIS THRESHOLD REACHED![/bold yellow]")

                if not self._ctr_running:
                    combined_history = await self._get_combined_history()
                    token_counts = _snapshot_tokens(agent)
                    self._ctr_running = True
                    self._ctr_task = asyncio.create_task(
                        self._run_ctr_background(combined_history, token_counts)
                    )
                    self._ctr_task.add_done_callback(lambda _: None)
                    console.print("[dim]CTR analysis running in background - agents will continue immediately[/dim]")
                else:
                    console.print("[dim]CTR analysis already running in background, skipping this cycle[/dim]")

                self.interaction_counter = 0

    async def _get_combined_history(self) -> List:
        for agent in self._tracked_agents:
            if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
                if agent.model.message_history:
                    return list(agent.model.message_history)
        return []

    async def _run_ctr_background(self, in_memory_history, token_counts) -> None:
        try:
            if not in_memory_history:
                console.print("[yellow]Purple Team CTR: No conversation history available for analysis[/yellow]")
                return

            self._ctr_output_dir = _ensure_ctr_output_dir(self._ctr_output_dir)

            from cai.ctr import experiment as ctr_experiment
            await ctr_experiment.run(
                messages=in_memory_history,
                token_counts=token_counts,
                output_base_dir=self._ctr_output_dir,
            )

            self.last_ctr_results = _find_latest_run(self._ctr_output_dir)
            if self.last_ctr_results:
                from cai.ctr.digest import mark_ctr_run_complete
                mark_ctr_run_complete(self.last_ctr_results)
                await display_and_inject_results(self.last_ctr_results, show_visualization=False)

        except Exception as e:
            console.print(f"[red]Purple Team CTR Background Error: {e}[/red]")
            import traceback
            console.print(f"[dim]{traceback.format_exc()}[/dim]")
        finally:
            self._ctr_running = False


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _snapshot_history(agent: Agent) -> Optional[list]:
    if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
        if agent.model.message_history:
            return list(agent.model.message_history)
    return None


def _snapshot_tokens(agent: Agent) -> Optional[dict]:
    if hasattr(agent.model, 'total_input_tokens') and hasattr(agent.model, 'total_output_tokens'):
        return {
            'input_tokens': getattr(agent.model, 'total_input_tokens', 0),
            'output_tokens': getattr(agent.model, 'total_output_tokens', 0),
            'total_tokens': (
                getattr(agent.model, 'total_input_tokens', 0)
                + getattr(agent.model, 'total_output_tokens', 0)
            ),
        }
    return None


def _ensure_ctr_output_dir(current_dir: Optional[str]) -> str:
    if current_dir is not None:
        return current_dir
    session_id = None
    recorder = get_session_recorder()
    if recorder is not None:
        session_id = getattr(recorder, "session_id", None)
    base_dir = get_ctr_output_base_dir()
    if session_id:
        base_dir = os.path.join(base_dir, session_id)
    os.makedirs(base_dir, exist_ok=True)
    return base_dir


def _find_latest_run(output_dir: str) -> Optional[str]:
    effective = output_dir or get_ctr_output_base_dir()
    if os.path.exists(effective):
        run_dirs = sorted(d for d in os.listdir(effective) if d.startswith('run_'))
        if run_dirs:
            return os.path.join(effective, run_dirs[-1])
    return None


async def display_and_inject_results(
    results_dir: str,
    agent: Optional[Agent] = None,
    show_visualization: bool = True,
) -> None:
    """Display CTR results in CLI with rich formatting."""
    try:
        from cai.ctr.visualization import visualize_baseline_results
        from cai.ctr.digest import get_ctr_digest
        from rich.panel import Panel
        from rich.markdown import Markdown

        nash_data = _load_nash_data(results_dir)
        if not nash_data:
            console.print("[yellow]CTR: No results to display[/yellow]")
            return

        if nash_data.get('error') and not nash_data.get('optimal_defense'):
            console.print(f"[red]CTR Analysis Error: {nash_data['error']}[/red]")
            console.print("[yellow]Skipping digest generation due to CTR analysis failure[/yellow]")
            return

        normalized_paths, viz_paths = _load_attack_paths(results_dir)

        if show_visualization:
            console.print("\n[bold green]CTR Analysis Complete[/bold green]")
            attacker_probs = _parse_probability_sequence(nash_data.get('attacker_strategy'))
            if attacker_probs:
                nash_data['attacker_strategy'] = attacker_probs
            visualize_baseline_results(nash_data, paths=viz_paths, print_to_console=True)

            console.print("\n" + "=" * 80)
            console.print("[bold cyan]CTR Intelligence Added to System Prompt[/bold cyan]")
            console.print("[dim]This strategic digest will be available to the agent on every subsequent turn:[/dim]")
            console.print("=" * 80 + "\n")
        else:
            console.print("\n[bold magenta]Purple Team CTR Digest Generated[/bold magenta]")
            console.print("[dim]Strategic intelligence available to both agents on next turn[/dim]\n")

        digest_mode = os.getenv("CAI_CTR_DIGEST_MODE", "llm")
        digest = get_ctr_digest(results_dir, mode=digest_mode)

        if digest:
            from cai.ctr.digest import update_session_digest
            session_id = None
            recorder = get_session_recorder()
            if recorder is not None:
                session_id = getattr(recorder, "session_id", None)
            if session_id:
                update_session_digest(session_id, digest, mode=digest_mode)

            if show_visualization:
                panel = Panel(
                    Markdown(digest),
                    title=f"[bold]System Prompt Injection Preview[/bold] [dim](mode: {digest_mode})[/dim]",
                    border_style="cyan",
                    padding=(1, 2),
                )
                console.print(panel)
                console.print("\n[green]The above CTR digest is now part of the agent's system prompt[/green]")
                console.print("[dim]It will guide the agent's decision-making on subsequent interactions[/dim]")
            else:
                console.print(f"[green]CTR digest injected into agents' system prompts (mode: {digest_mode})[/green]")
        else:
            console.print("[yellow]Note: Could not generate CTR digest (incomplete data)[/yellow]")

        if show_visualization:
            console.print(f"\n[dim]Full results saved to: {results_dir}[/dim]\n")
        else:
            console.print(f"[dim]Results: {results_dir}[/dim]")

        _update_latest_symlink(results_dir)

    except Exception as e:
        console.print(f"[red]Error displaying CTR results: {e}[/red]")


def _load_nash_data(results_dir: str) -> Optional[dict]:
    nash_file = os.path.join(results_dir, 'nash_equilibrium.json')
    if os.path.exists(nash_file):
        with open(nash_file, 'r') as f:
            return json.load(f)

    baseline_file = os.path.join(results_dir, 'ctr_baseline.txt')
    if os.path.exists(baseline_file):
        with open(baseline_file, 'r') as f:
            content = f.read()
        match = re.search(r'BASELINE RESULT DICTIONARY:\n-+\n(\{[\s\S]*?\})', content)
        if match:
            return json.loads(match.group(1))
    return None


def _load_attack_paths(results_dir: str):
    paths_json = os.path.join(results_dir, 'attack_paths.json')
    if os.path.exists(paths_json):
        try:
            with open(paths_json, 'r') as pf:
                raw_paths = json.load(pf).get('paths')
            normalized = _normalize_paths(raw_paths)
            viz_paths = [e['sequence'] for e in normalized] if normalized else None
            return normalized, viz_paths
        except Exception:
            pass
    return [], None


def _update_latest_symlink(results_dir: str) -> None:
    try:
        base_dir = get_ctr_output_base_dir()
        latest_link = os.path.join(base_dir, 'latest')
        if os.path.islink(latest_link):
            os.unlink(latest_link)
        elif os.path.exists(latest_link):
            return
        rel_path = os.path.relpath(results_dir, base_dir)
        os.symlink(rel_path, latest_link)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Factory: wrap any base agent with GCTR capabilities
# ---------------------------------------------------------------------------

def make_gctr_agent(
    base_agent: Agent,
    *,
    name: str,
    description: str,
    n_interactions: Optional[int] = None,
    team_label: str = "Agent",
) -> Agent:
    """Clone *base_agent* and attach CTRHooks for GCTR behaviour.

    Args:
        base_agent: The agent to wrap (will be cloned, not mutated).
        name: Display name for the GCTR variant.
        description: Agent description string.
        n_interactions: CTR trigger threshold (default: CAI_GCTR_NITERATIONS or 5).
        team_label: Label used in CTR console output.
    """
    if n_interactions is None:
        n_interactions = int(os.getenv("CAI_GCTR_NITERATIONS", "5"))

    return base_agent.clone(
        name=name,
        description=description,
        hooks=CTRHooks(n_interactions=n_interactions, team_label=team_label),
    )
