"""
CTR (Cut The Rope) command for security game analysis.
This command provides access to the Cut The Rope game-theoretic security analysis.
"""

import os
import json
import asyncio
import threading
from typing import List, Optional

from rich.console import Console
from rich.table import Table

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN

# Heavy imports moved to lazy loading
# These will be imported only when CTR command is actually used
ctr_experiment = None
visualize_baseline_results = None
get_ctr_output_base_dir = None

def _ensure_ctr_imports():
    """Lazily import heavy CTR modules only when needed."""
    global ctr_experiment, visualize_baseline_results, get_ctr_output_base_dir
    if ctr_experiment is None:
        from cai.ctr import experiment as _ctr_experiment
        ctr_experiment = _ctr_experiment
    if visualize_baseline_results is None:
        from cai.ctr.visualization import visualize_baseline_results as _vis
        visualize_baseline_results = _vis
    if get_ctr_output_base_dir is None:
        from cai.ctr.paths import get_ctr_output_base_dir as _get_dir
        get_ctr_output_base_dir = _get_dir


def _ctr_sorted_run_directories(output_dir: str) -> list[str]:
    """Collect ``run_*`` dirs at base and one nesting level; newest first by mtime."""
    if not os.path.isdir(output_dir):
        return []
    all_run_dirs: list[str] = []
    for item in os.listdir(output_dir):
        path = os.path.join(output_dir, item)
        if item.startswith("run_") and os.path.isdir(path):
            all_run_dirs.append(path)
        elif os.path.isdir(path):
            try:
                for subitem in os.listdir(path):
                    if subitem.startswith("run_"):
                        subpath = os.path.join(path, subitem)
                        if os.path.isdir(subpath):
                            all_run_dirs.append(subpath)
            except (OSError, PermissionError):
                continue
    all_run_dirs.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    return all_run_dirs


def _ctr_resolve_run_directory(output_dir: str) -> Optional[str]:
    """Symlink ``latest`` if valid, else newest ``run_*`` from ``_ctr_sorted_run_directories``."""
    if not os.path.isdir(output_dir):
        return None
    latest_link = os.path.join(output_dir, "latest")
    if os.path.islink(latest_link) and os.path.exists(latest_link):
        return os.path.realpath(latest_link)
    runs = _ctr_sorted_run_directories(output_dir)
    return runs[0] if runs else None


console = Console()


class CTRCommand(Command):
    """CTR command for security game analysis.

    This command serves as the primary interface between CAI's REPL and the CTR
    (Cut The Rope) game-theoretic security analysis system. It handles command
    registration, context gathering, and orchestrates the execution of CTR experiments.
    """

    def __init__(self):
        super().__init__(
            name="/ctr",
            description="Cut The Rope security game analysis",
        )
        
        # Add subcommands
        self.add_subcommand("show", "Show defender/attacker strategies and equilibrium", self.handle_show)
        self.add_subcommand("graph", "Display the attack graph", self.handle_graph)
        self.add_subcommand("list", "List available CTR runs", self.handle_list)
        self.add_subcommand("use", "Select a CTR run by index or name", self.handle_use)
        self.add_subcommand("open", "Open the folder containing CTR runs", self.handle_open)
        
        # Store the last run results
        self.last_results_dir = None

    def handle_no_args(self) -> bool:
        """Run full CTR analysis on the current context (invoked as bare ``/ctr``)."""
        _ensure_ctr_imports()
        return self.run_full_analysis()

    def run_full_analysis(self, log_path: Optional[str] = None) -> bool:
        """Run the complete CTR analysis.

        Main Orchestrator
        This method coordinates the entire CTR analysis pipeline:
        1. Context gathering (3-tier fallback for conversation data)
        2. Execution mode detection (TUI vs CLI, async vs sync)
        3. Experiment invocation via ctr_experiment.run()
        4. Result handling and UI updates
        """
        console.print(f"[bold {_CAI_GREEN}]Running Cut The Rope analysis...[/bold {_CAI_GREEN}]")

        # Context Gathering - Priority 1
        # Try to fetch in-memory history from the active agent
        # This is the preferred source as it contains the most current conversation state
        in_memory_history = None
        try:
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            active_agent = AGENT_MANAGER.get_active_agent()
            active_name = getattr(active_agent, 'name', None)
            # Prefer the model's live history when available
            if active_agent and hasattr(active_agent, 'model') and hasattr(active_agent.model, 'message_history'):
                if active_agent.model.message_history:
                    in_memory_history = list(active_agent.model.message_history)
            # Fallback to manager's history mapping
            if in_memory_history is None and active_name:
                hist = AGENT_MANAGER.get_message_history(active_name)
                if hist:
                    in_memory_history = list(hist)
        except Exception:
            in_memory_history = None

        # Context Gathering - Priority 2
        # If no in-memory history (or caller forced a file), discover the current session log path
        # This fallback uses the session recorder which writes conversations to JSONL files
        if not in_memory_history and not log_path:
            try:
                from cai.sdk.agents.run_to_jsonl import get_session_recorder
                recorder = get_session_recorder()
                if hasattr(recorder, 'filename'):
                    log_path = recorder.filename
                    console.print(f"[dim]Using current session log: {log_path}[/dim]")
            except Exception:
                pass
        
        # Context Gathering - Priority 3
        # If still no log path, try to find recent logs in the standard CAI logs directory
        # This is the final fallback, using the most recently modified log file
        if not log_path:
            import glob
            log_dir = os.path.expanduser("~/.cai/logs")
            if os.path.exists(log_dir):
                logs = glob.glob(os.path.join(log_dir, "*.jsonl"))
                if logs:
                    logs.sort(key=os.path.getmtime, reverse=True)
                    log_path = logs[0]
                    console.print(f"[dim]Using most recent log: {log_path}[/dim]")
        
        if not in_memory_history and not log_path:
            console.print("[red]Error: No conversation history found (in-memory or log).[/red]")
            return False
        
        def _execute_experiment_sync():
            """Run experiment in this thread via ``asyncio.run(ctr_experiment.run(...))``."""
            if in_memory_history:
                # Extract token counts from active_agent.model if available
                token_counts = None
                try:
                    if active_agent and hasattr(active_agent, 'model'):
                        model = active_agent.model
                        if hasattr(model, 'total_input_tokens') and hasattr(model, 'total_output_tokens'):
                            token_counts = {
                                'input_tokens': getattr(model, 'total_input_tokens', 0),
                                'output_tokens': getattr(model, 'total_output_tokens', 0),
                                'total_tokens': getattr(model, 'total_input_tokens', 0) + getattr(model, 'total_output_tokens', 0)
                            }
                            # Try to get the last response usage for more detail if available
                            if hasattr(model, '_last_response_usage') and model._last_response_usage:
                                last_usage = model._last_response_usage
                                if hasattr(last_usage, 'prompt_tokens'):
                                    token_counts['last_prompt_tokens'] = last_usage.prompt_tokens
                                if hasattr(last_usage, 'completion_tokens'):
                                    token_counts['last_completion_tokens'] = last_usage.completion_tokens
                except Exception:
                    pass  # Silently fail if token extraction doesn't work

                asyncio.run(ctr_experiment.run(messages=in_memory_history, token_counts=token_counts))
            else:
                asyncio.run(ctr_experiment.run(input_log=log_path))

        def _resolve_results_dir() -> Optional[str]:
            return _ctr_resolve_run_directory(get_ctr_output_base_dir())

        def _focus_ctr_tab_and_load(run_dir: Optional[str]) -> None:
            """Switch to CTR tab and load the given run in the CTR canvas (TUI only).

            TUI Integration
            This function handles the interaction with CAI's Terminal User Interface.
            It switches to the CTR tab and loads the results for visualization.
            Uses thread-safe UI updates via app.call_from_thread() when available.
            """
            try:
                from cai.tui.cai_terminal import CAITerminal
                from cai.tui.components.graph_canvas import CTRCanvas
                from textual.widgets import Select
            except Exception:
                return

            app = getattr(CAITerminal, "_current_app", None)
            if not app:
                # Try Textual API as fallback
                try:
                    from textual.app import App
                    app = App.get_running_app()
                except Exception:
                    app = None
            if not app:
                return

            def _ui_update():
                try:
                    # Switch to CTR tab
                    try:
                        app.action_show_ctr()
                    except Exception:
                        try:
                            app.switch_to_tab("ctr")
                        except Exception:
                            pass

                    # Find canvas and reload runs
                    canvas = app.query_one("#ctr-canvas", CTRCanvas)
                    # Refresh available runs and select the new one if present
                    try:
                        canvas._load_runs_into_select()  # noqa: SLF001
                    except Exception:
                        pass
                    try:
                        sel = canvas.query_one("#run-select", Select)
                        if run_dir and os.path.isdir(run_dir):
                            # If this run is in options, pick it
                            options = getattr(sel, "options", [])
                            # options are list of (label, value)
                            values = [getattr(o, "value", None) if hasattr(o, "value") else (o[1] if isinstance(o, tuple) else None) for o in options]
                            if run_dir in values:
                                sel.value = run_dir
                            elif options:
                                sel.value = options[-1].value if hasattr(options[-1], "value") else options[-1][1]
                        # Load the selected run into viewport
                        canvas._load_selected_run()  # noqa: SLF001
                    except Exception:
                        pass
                except Exception:
                    pass

            # Ensure execution on UI thread when possible
            try:
                if hasattr(app, "call_from_thread"):
                    app.call_from_thread(_ui_update)
                else:
                    _ui_update()
            except Exception:
                _ui_update()

        # Execution Mode Detection
        # Determines whether to run CTR synchronously or in a background thread.
        # This is critical for proper integration with both CLI and TUI modes.
        # - TUI mode: Runs in background to avoid blocking the UI
        # - Async context: Runs in background to avoid event loop conflicts
        # - CLI mode: Runs synchronously for immediate feedback
        in_tui = os.getenv("CAI_TUI_MODE") == "true"
        loop_running = False
        try:
            loop = asyncio.get_running_loop()
            loop_running = True if loop and loop.is_running() else False
        except RuntimeError:
            loop_running = False

        if in_tui or loop_running:
            # Async/Background Execution
            # In TUI or async contexts, CTR runs in a daemon thread to avoid blocking.
            # This allows the UI to remain responsive while CTR analysis proceeds.
            console.print("[dim]Processing CTR in background...[/dim]\n")

            def _run_and_report():
                try:
                    _execute_experiment_sync()  # Calls ctr_experiment.run()
                    results_dir = _resolve_results_dir()
                    self.last_results_dir = results_dir

                    def _notify_success():
                        if results_dir:
                            console.print(
                                f"[bold {_CAI_GREEN}]✓ CTR analysis complete. Results saved to:[/bold {_CAI_GREEN}] {results_dir}"
                            )
                            console.print(
                                "[dim]Use '/ctr show' to view results or '/ctr graph' to see the attack graph[/dim]"
                            )
                            # Auto-focus CTR tab and load latest run
                            _focus_ctr_tab_and_load(results_dir)
                        else:
                            console.print("[red]Error: No results found after analysis[/red]")

                    # If in TUI, marshal back to UI thread when possible
                    # Run UI update; rely on CAITerminal app
                    _notify_success()
                except Exception as e:  # noqa: BLE001
                    import traceback

                    def _notify_error():
                        console.print(f"[red]Error running CTR analysis: {e}[/red]")
                        console.print(f"[dim]{traceback.format_exc()}[/dim]")

                    _notify_error()

            t = threading.Thread(target=_run_and_report, daemon=True)
            t.start()
            return True

        # Synchronous Execution
        # In CLI mode without an async context, CTR runs synchronously.
        # This provides immediate feedback in command-line environments.
        try:
            _execute_experiment_sync()  # Blocks until CTR analysis completes
            results_dir = _resolve_results_dir()
            self.last_results_dir = results_dir
            if results_dir:
                console.print(
                    f"[bold {_CAI_GREEN}]✓ CTR analysis complete. Results saved to:[/bold {_CAI_GREEN}] {results_dir}"
                )
                console.print(
                    "[dim]Use '/ctr show' to view results or '/ctr graph' to see the attack graph[/dim]"
                )
                # If in TUI, auto load CTR tab as well
                if os.getenv("CAI_TUI_MODE") == "true":
                    _focus_ctr_tab_and_load(results_dir)
                return True
            console.print("[red]Error: No results found after analysis[/red]")
            return False
        except Exception as e:  # noqa: BLE001
            import traceback
            console.print(f"[red]Error running CTR analysis: {e}[/red]")
            console.print(f"[dim]{traceback.format_exc()}[/dim]")
            return False

    def handle_show(self, args: Optional[List[str]] = None) -> bool:
        """Display defender/attacker strategies and equilibrium (CLI); TUI focuses CTR tab."""
        _ensure_ctr_imports()
        # In TUI, just focus CTR tab and load the most recent run
        if os.getenv("CAI_TUI_MODE") == "true":
            # Try to resolve last results directory if not set
            if not self.last_results_dir or not os.path.exists(self.last_results_dir):
                output_dir = get_ctr_output_base_dir()
                resolved = _ctr_resolve_run_directory(output_dir) if os.path.exists(output_dir) else None
                if resolved:
                    self.last_results_dir = resolved
            # Switch UI to CTR and load
            try:
                # Reuse helper via local implementation
                # Minimal inline to avoid duplication
                from cai.tui.cai_terminal import CAITerminal
                app = getattr(CAITerminal, "_current_app", None)
                if app:
                    if hasattr(app, "call_from_thread"):
                        app.call_from_thread(lambda: app.action_show_ctr())
                    else:
                        app.action_show_ctr()
            except Exception:
                pass
            return True

        if not self.last_results_dir:
            output_dir = get_ctr_output_base_dir()
            if os.path.exists(output_dir):
                self.last_results_dir = _ctr_resolve_run_directory(output_dir)

        if not self.last_results_dir or not os.path.exists(self.last_results_dir):
            console.print("[dim]No CTR results found. Run '/ctr' first to generate analysis.[/dim]")
            return False
        
        try:
            # Prefer JSON baseline if present; otherwise parse ctr_baseline.txt
            nash_data = None
            nash_file = os.path.join(self.last_results_dir, 'nash_equilibrium.json')
            if os.path.exists(nash_file):
                with open(nash_file, 'r') as f:
                    nash_data = json.load(f)

            if not nash_data:
                import re
                baseline_file = os.path.join(self.last_results_dir, 'ctr_baseline.txt')
                if os.path.exists(baseline_file):
                    with open(baseline_file, 'r') as f:
                        content = f.read()
                    match = re.search(r'BASELINE RESULT DICTIONARY:\n-+\n(\{[\s\S]*?\})', content)
                    if match:
                        nash_data = json.loads(match.group(1))
                        console.print(f"[dim]Loaded data from ctr_baseline.txt[/dim]")

            if not nash_data:
                console.print("[red]Nash equilibrium results not found or analysis failed[/red]")
                return False

            if nash_data.get('error') and not nash_data.get('optimal_defense'):
                console.print(f"[red]CTR analysis error: {nash_data['error']}[/red]")
                return False

            # Load attack path sequences if available
            paths = None
            paths_json = os.path.join(self.last_results_dir, 'attack_paths.json')
            if os.path.exists(paths_json):
                try:
                    with open(paths_json, 'r') as pf:
                        paths = json.load(pf).get('paths')
                except Exception:
                    paths = None

            # Unified, improved visualization (defense + attacker path sequences + equilibrium)
            visualize_baseline_results(nash_data, paths=paths, print_to_console=True)
            return True

        except Exception as e:
            console.print(f"[red]Error displaying results: {e}[/red]")
            return False

    def handle_graph(self, args: Optional[List[str]] = None) -> bool:
        """Display attack graph assets (CLI); TUI focuses CTR tab without external viewers."""
        _ensure_ctr_imports()
        # In TUI, never open external viewers; focus the CTR tab and load latest
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = getattr(CAITerminal, "_current_app", None)
                if app:
                    if hasattr(app, "call_from_thread"):
                        app.call_from_thread(lambda: app.action_show_ctr())
                    else:
                        app.action_show_ctr()
            except Exception:
                pass
            return True

        if not self.last_results_dir:
            output_dir = get_ctr_output_base_dir()
            if os.path.exists(output_dir):
                self.last_results_dir = _ctr_resolve_run_directory(output_dir)

        if not self.last_results_dir or not os.path.exists(self.last_results_dir):
            console.print("[dim]No CTR results found. Run '/ctr' first to generate analysis.[/dim]")
            return False
        
        try:
            # Check for graph files - use the actual filenames being generated
            graph_llm_png = os.path.join(self.last_results_dir, 'attack_graph_llm.png')
            graph_individual_png = os.path.join(self.last_results_dir, 'attack_graph_individual.png')
            graph_cleaned_png = os.path.join(self.last_results_dir, 'attack_graph_individual_cleaned.png')
            # Check which graph files exist and open the best one
            graph_to_open = None
            if os.path.exists(graph_llm_png):
                graph_to_open = graph_llm_png
                console.print(
                    f"[bold {_CAI_GREEN}]✓ LLM attack graph visualization found:[/bold {_CAI_GREEN}] {graph_llm_png}"
                )
            elif os.path.exists(graph_individual_png):
                graph_to_open = graph_individual_png
                console.print(
                    f"[bold {_CAI_GREEN}]✓ Individual attack graph visualization found:[/bold {_CAI_GREEN}] {graph_individual_png}"
                )
            elif os.path.exists(graph_cleaned_png):
                graph_to_open = graph_cleaned_png
                console.print(
                    f"[bold {_CAI_GREEN}]✓ Cleaned attack graph visualization found:[/bold {_CAI_GREEN}] {graph_cleaned_png}"
                )
            
            if graph_to_open:
                console.print("[dim]Opening the graph visualization...[/dim]")
                import platform, subprocess
                try:
                    if platform.system() == 'Darwin':  # macOS
                        subprocess.run(["open", graph_to_open], check=False)
                    elif platform.system() == 'Linux':
                        subprocess.run(["xdg-open", graph_to_open], check=False)
                    elif platform.system() == 'Windows':
                        os.startfile(graph_to_open)  # type: ignore[attr-defined]
                except Exception:
                    pass
            
            # Try to display graph structure from graph_information.txt
            graph_info_file = os.path.join(self.last_results_dir, 'graph_information.txt')
            if os.path.exists(graph_info_file):
                import re
                with open(graph_info_file, 'r') as f:
                    content = f.read()
                
                # Extract the JSON graph structure from LLM output
                match = re.search(r'LLM Output:\n-+\n(\{[\s\S]*?\})\n-+', content)
                if match:
                    try:
                        graph_data = json.loads(match.group(1))
                        
                        console.print(f"\n[bold {_CAI_GREEN}]═══ Attack Graph Structure ═══[/bold {_CAI_GREEN}]")

                        # Show nodes
                        nodes = graph_data.get('nodes', [])
                        console.print(f"\n[bold white]Nodes:[/bold white] {len(nodes)} total")

                        node_table = Table(
                            show_header=True, header_style=f"bold {_CAI_GREEN}"
                        )
                        node_table.add_column("Node ID", style="white")
                        node_table.add_column("Name", style="dim")
                        node_table.add_column("Vulnerable", justify="center", style="red")
                        
                        for node in nodes[:10]:  # Show first 10 nodes
                            node_id = node.get('id', '')
                            node_name = node.get('name', 'unknown')
                            vulnerable = "Yes" if node.get('vulnerability') else "No"
                            node_table.add_row(node_id, node_name, vulnerable)
                        
                        console.print(node_table)
                        
                        if len(nodes) > 10:
                            console.print(f"[dim]... and {len(nodes) - 10} more nodes[/dim]")
                        
                        # Show edges
                        edges = graph_data.get('edges', [])
                        console.print(f"\n[bold white]Edges:[/bold white] {len(edges)} total")

                        edge_table = Table(
                            show_header=True, header_style=f"bold {_CAI_GREEN}"
                        )
                        edge_table.add_column("Source", style="white")
                        edge_table.add_column("Target", style="white")
                        
                        for edge in edges[:10]:  # Show first 10 edges
                            source = edge.get('source', '')
                            target = edge.get('target', '')
                            edge_table.add_row(source, target)
                        
                        console.print(edge_table)
                        
                        if len(edges) > 10:
                            console.print(f"[dim]... and {len(edges) - 10} more edges[/dim]")
                    except json.JSONDecodeError:
                        console.print("[dim]Could not parse graph structure data[/dim]")
            
            return True

        except Exception as e:
            console.print(f"[red]Error displaying graph: {e}[/red]")
            return False

    def handle_list(self, args: Optional[List[str]] = None) -> bool:
        """List CTR runs (newest first); indices match ``/ctr use <n>``."""
        _ensure_ctr_imports()
        base = get_ctr_output_base_dir()
        if not os.path.isdir(base):
            console.print("[dim]No CTR output directory found.[/dim]")
            return False
        runs = _ctr_sorted_run_directories(base)
        if not runs:
            console.print("[dim]No CTR runs found.[/dim]")
            return False
        table = Table(show_header=True, header_style=f"bold {_CAI_GREEN}")
        table.add_column("#", justify="right", style="dim")
        table.add_column("Run", justify="left", style="white")
        table.add_column("Path", justify="left", style="dim")
        for idx, path in enumerate(runs, 1):
            name = os.path.basename(path)
            marker = (
                " (active)"
                if self.last_results_dir
                and os.path.exists(self.last_results_dir)
                and os.path.samefile(self.last_results_dir, path)
                else ""
            )
            table.add_row(str(idx), name + marker, path)
        console.print(table)
        return True

    def handle_use(self, args: Optional[List[str]] = None) -> bool:
        """Select active run by index (as in ``/ctr list``), run folder name, or path."""
        _ensure_ctr_imports()
        token = (args or [None])[0]
        base = get_ctr_output_base_dir()
        if not token:
            console.print("[dim]Usage: /ctr use <index|run_name|path>[/dim]")
            return False
        entries = _ctr_sorted_run_directories(base) if os.path.isdir(base) else []
        selected = None
        if token.isdigit():
            idx = int(token)
            if 1 <= idx <= len(entries):
                selected = entries[idx - 1]
        if not selected:
            cand = os.path.join(base, token)
            if os.path.isdir(cand):
                selected = cand
        if not selected and os.path.isdir(base):
            for p in entries:
                if os.path.basename(p) == token:
                    selected = p
                    break
        if not selected and os.path.isdir(token):
            selected = token
        if not selected:
            console.print("[red]Run not found. Try '/ctr list' first.[/red]")
            return False
        self.last_results_dir = selected
        console.print(f"[bold {_CAI_GREEN}]Active CTR run set to:[/bold {_CAI_GREEN}] {selected}")
        return True

    def handle_open(self, args: Optional[List[str]] = None) -> bool:
        """Open the folder containing CTR runs (parent of active or newest run)."""
        _ensure_ctr_imports()
        base = get_ctr_output_base_dir()
        if not os.path.isdir(base):
            console.print("[dim]No CTR output directory found to open.[/dim]")
            return False

        parent = None
        if self.last_results_dir and os.path.isdir(self.last_results_dir):
            parent = os.path.dirname(self.last_results_dir)
        else:
            runs = _ctr_sorted_run_directories(base)
            if runs:
                parent = os.path.dirname(runs[0])
        parent = parent or base

        try:
            import platform
            import subprocess

            system = platform.system()
            if system == "Darwin":
                subprocess.run(["open", parent], check=False)
            elif system == "Linux":
                subprocess.run(["xdg-open", parent], check=False)
            elif system == "Windows":
                os.startfile(parent)  # type: ignore[attr-defined]
            console.print(f"[bold {_CAI_GREEN}]Opened CTR runs folder:[/bold {_CAI_GREEN}] {parent}")
            return True
        except Exception as e:
            console.print(f"[red]Failed to open folder: {e}[/red]")
            console.print(f"[dim]Path: {parent}[/dim]")
            return False


register_command(CTRCommand())
