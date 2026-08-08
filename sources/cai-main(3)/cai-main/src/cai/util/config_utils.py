"""
Configuration, environment, and setup utilities for CAI.
"""

import os
import pathlib
import shutil
import sys
from pathlib import Path

from cai import is_pentestperf_available

if is_pentestperf_available():
    import cai.caibench as ptt


def get_config_dir() -> Path:
    """
    Returns the cai configuration directory, creating it if it doesn't exist.
    The directory is located at ~/.cai
    """
    config_dir = Path.home() / ".cai"
    config_dir.mkdir(parents=True, exist_ok=True)
    return config_dir


def get_session_logs_dir() -> Path:
    """Directory where :class:`cai.sdk.agents.run_to_jsonl.DataRecorder` writes session JSONL.

    Kept in sync with ``DataRecorder`` (``~/.cai/logs``). Used by ``/sessions``,
    ``/resume``, and related helpers so listing matches actual capture paths.
    """
    d = get_config_dir() / "logs"
    d.mkdir(parents=True, exist_ok=True)
    return d


def get_debug_log_dir() -> Path:
    """Return ~/.cai/debug/, creating it if needed."""
    d = Path.home() / ".cai" / "debug"
    d.mkdir(parents=True, exist_ok=True)
    return d


def get_pricings_dir() -> Path:
    """
    Returns the local pricings directory, checking in order:
    1. CAI_PRICINGS_DIR env var if set
    2. Package directory (cai/pricings - for pip-installed packages)
    3. Development directory (../../pricings from src/cai - for editable installs)
    4. Current working directory (./pricings - fallback)

    Creates the directory if needed and writable.
    """
    # Allow override via env if needed
    override = os.getenv("CAI_PRICINGS_DIR")
    if override:
        base = pathlib.Path(override)
        try:
            base.mkdir(parents=True, exist_ok=True)
        except Exception:
            pass
        return base

    # Try package directory first (for pip-installed packages)
    try:
        package_dir = pathlib.Path(__file__).parent.parent  # src/cai/util -> src/cai
        package_pricings = package_dir / "pricings"
        if package_pricings.exists():
            return package_pricings
    except Exception:
        pass

    # Try development directory (for editable installs from git repo)
    try:
        package_dir = pathlib.Path(__file__).parent.parent  # src/cai/util -> src/cai
        dev_pricings = package_dir.parent.parent / "pricings"
        if dev_pricings.exists():
            return dev_pricings
    except Exception:
        pass

    # Fall back to CWD
    base = pathlib.Path("pricings")
    try:
        base.mkdir(parents=True, exist_ok=True)
    except Exception:
        pass
    return base


_PRICINGS_DIR_INITIALIZED = False


def _seed_pricings_dir() -> None:
    """Seed pricings dir with pricing.json if present and missing there.

    - For pip-installed packages, pricing.json should already be in cai/pricings/
    - For development, copies from workspace root pricings/ if needed
    - Does nothing if destination already exists
    """
    global _PRICINGS_DIR_INITIALIZED
    if _PRICINGS_DIR_INITIALIZED:
        return
    _PRICINGS_DIR_INITIALIZED = True

    pricings_dir = get_pricings_dir()
    dst = pricings_dir / "pricing.json"

    # Skip if destination already exists
    if dst.exists():
        return

    # Try to find source pricing.json for development environments
    try:
        package_dir = pathlib.Path(__file__).parent.parent  # src/cai/util -> src/cai
        src_candidates = [
            package_dir / "pricings" / "pricing.json",  # Package-local copy
            package_dir.parent.parent / "pricings" / "pricing.json",  # Dev workspace
            pathlib.Path("pricing.json"),  # CWD
        ]

        for src in src_candidates:
            if src.exists():
                # Make sure destination directory exists
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dst)
                break
    except Exception:
        # Non-fatal; continue without seeding
        pass


def get_ollama_api_base():
    """Get the Ollama API base URL from environment variable or default to localhost:8000.

    Supports both:
    - OLLAMA_API_BASE: For local Ollama instances (e.g., http://localhost:8000/v1)
    - OPENAI_BASE_URL: For Ollama Cloud or other OpenAI-compatible services (e.g., https://ollama.com/api/v1)
    """
    # First check OLLAMA_API_BASE for local Ollama
    ollama_base = os.environ.get("OLLAMA_API_BASE")
    if ollama_base:
        return ollama_base

    # Then check OPENAI_BASE_URL for Ollama Cloud or other services
    openai_base = os.environ.get("OPENAI_BASE_URL")
    if openai_base and "ollama.com" in openai_base:
        return openai_base

    # Default to local Ollama
    return "http://localhost:8000/v1"


def get_ollama_auth_headers():
    """Get authentication headers for Ollama Cloud if API key is set.

    Returns:
        Dictionary with Authorization header if API key exists, empty dict otherwise
    """
    api_key = os.getenv("OLLAMA_API_KEY") or os.getenv("OPENAI_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


def ensure_litellm_transcription_support():
    """
    Ensure transcription kwargs detection works even if __annotations__ is missing in LiteLLM.
    """
    try:
        import litellm.litellm_core_utils.model_param_helper as model_param_helper

        # Override the problematic method to avoid the error
        original_get_transcription_kwargs = (
            model_param_helper.ModelParamHelper._get_litellm_supported_transcription_kwargs
        )

        def safe_get_transcription_kwargs():
            """A safer version that doesn't rely on __annotations__."""
            return set(
                [
                    "file",
                    "model",
                    "language",
                    "prompt",
                    "response_format",
                    "temperature",
                    "api_base",
                    "api_key",
                    "api_version",
                    "timeout",
                    "custom_llm_provider",
                ]
            )

        # Apply the monkey patch
        model_param_helper.ModelParamHelper._get_litellm_supported_transcription_kwargs = (
            safe_get_transcription_kwargs
        )
        return True
    except (ImportError, AttributeError):
        # If LiteLLM isn't present or structure changed, report unsupported
        return False


def ensure_litellm_logging_worker_loop_safety():
    """
    Ensure LiteLLM's global logging worker rebinds to the current asyncio loop.

    LiteLLM keeps a singleton ``LoggingWorker`` with an ``asyncio.Queue`` that is
    bound to the loop that was running when logging first started. The CLI spins
    up multiple event loops (several ``asyncio.run`` invocations), so the worker
    can end up holding a queue from a now-closed loop, which triggers
    ``RuntimeError: <Queue ...> is bound to a different event loop``. This helper
    installs a small guard that recreates the queue/semaphore/worker task whenever
    the active event loop changes.
    """

    try:
        import asyncio
        from litellm.litellm_core_utils import logging_worker
    except ImportError:
        # LiteLLM not installed; nothing to patch
        return False

    worker = logging_worker.GLOBAL_LOGGING_WORKER

    # Idempotent installation
    if getattr(worker, "_loop_guard_installed", False):
        return True

    original_start = worker.start

    def start_with_loop_guard():
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            # No running loop; fall back to LiteLLM's original behaviour
            return original_start()

        # If the queue is tied to a different loop, drop it so we build a fresh one
        queue = getattr(worker, "_queue", None)
        if queue is not None:
            try:
                # Will raise if current loop differs
                queue._get_loop()
            except Exception:
                queue = None
            if queue is None:
                worker._queue = None

        # Recreate semaphore alongside the queue when we switch loops
        if worker._queue is None and getattr(worker, "_sem", None) is not None:
            worker._sem = None

        # Cancel worker task if it belongs to another loop so start() can spawn a new one
        if getattr(worker, "_worker_task", None) is not None:
            try:
                if worker._worker_task.get_loop() is not loop:
                    worker._worker_task.cancel()
                    worker._worker_task = None
            except Exception:
                worker._worker_task = None

        return original_start()

    # Install guard
    worker.start = start_with_loop_guard  # type: ignore[assignment]
    worker._loop_guard_installed = True
    return True


def visualize_agent_graph(start_agent):
    """
    Visualize agent graph showing all bidirectional connections between agents.
    Uses Rich library for pretty printing.

    Palette: brand green (``CAI_GREEN``), white body text, grey/dim chrome only —
    no blue/yellow/magenta/red in this tree so it matches headless REPL branding.
    """
    from rich.console import Console
    from rich.tree import Tree

    from cai.util.cli_palette import CAI_GREEN, GREY_HINT, GREY_TEXT

    console = Console()
    if start_agent is None:
        console.print(f"[dim {GREY_TEXT}]No agent provided to visualize.[/]")
        return

    root_label = (
        f"[bold {CAI_GREEN}]Agent:[/bold {CAI_GREEN}] "
        f"[white]{start_agent.name}[/white] "
        f"[dim {GREY_TEXT}](Current Agent)[/]"
    )
    tree = Tree(root_label, guide_style=f"dim {GREY_HINT}")

    visited = set()
    agent_nodes = {}
    agent_positions = {}
    position_counter = 0

    def add_agent_node(agent, parent=None, is_transfer=False):
        """Add an agent node and track for cross-connections."""
        nonlocal position_counter
        if agent is None:
            return None
        aid = id(agent)
        if aid in visited:
            if is_transfer and parent:
                original_pos = agent_positions.get(aid)
                parent.add(
                    f"[dim {GREY_TEXT}]Return to[/dim {GREY_TEXT}] [white]{agent.name}[/white]"
                    f"[dim {GREY_TEXT}] (Agent #{original_pos})[/]"
                )
            return agent_nodes.get(aid)

        visited.add(aid)
        position_counter += 1
        agent_positions[aid] = position_counter

        if is_transfer and parent:
            node = parent
        elif parent:
            node = parent.add(
                f"[bold {CAI_GREEN}]{agent.name}[/bold {CAI_GREEN}] "
                f"[dim {GREY_TEXT}](#{position_counter})[/]"
            )
        else:
            node = tree
        agent_nodes[aid] = node

        # Add tools
        tools_node = node.add(f"[bold {CAI_GREEN}]Tools[/bold {CAI_GREEN}]")

        # Get all tools from the agent
        all_tools = getattr(agent, "tools", [])

        # Import necessary modules for MCP checking
        from cai.repl.commands.mcp import get_mcp_tools_for_agent, _GLOBAL_MCP_SERVERS
        from cai.sdk.agents.tool import FunctionTool

        # Separate regular tools from MCP tools
        regular_tools = []
        mcp_tools = []

        # Get the agent's name for MCP association lookup
        agent_name = getattr(agent, "name", "")

        # Get MCP tools from the associations
        try:
            associated_mcp_tools = get_mcp_tools_for_agent(agent_name)
            mcp_tool_names = {tool.name for tool in associated_mcp_tools}
        except Exception:
            mcp_tool_names = set()

        # Categorize tools
        for tool in all_tools:
            tool_name = getattr(tool, "name", None) or getattr(tool, "__name__", "")
            # Check if this tool is an MCP tool by checking if it's in the MCP associations
            # or if it has certain MCP-related attributes
            if tool_name in mcp_tool_names or (hasattr(tool, "_is_mcp_tool") and tool._is_mcp_tool):
                mcp_tools.append(tool)
            else:
                regular_tools.append(tool)

        # Show regular tools first
        for tool in regular_tools:
            tool_name = getattr(tool, "name", None) or getattr(tool, "__name__", "")
            tools_node.add(f"[white]{tool_name}[/white]")

        # Show MCP tools with a different color/prefix
        if mcp_tools:
            for tool in mcp_tools:
                tool_name = getattr(tool, "name", None) or getattr(tool, "__name__", "")
                tools_node.add(
                    f"[dim {GREY_TEXT}]MCP ·[/dim {GREY_TEXT}] [white]{tool_name}[/white]"
                )

        # Add a summary line if we have both types
        if regular_tools and mcp_tools:
            summary_text = (
                f"[dim {GREY_TEXT}]({len(regular_tools)} regular, "
                f"{len(mcp_tools)} MCP tools)[/]"
            )
            tools_node.add(summary_text)
        elif mcp_tools and not regular_tools:
            summary_text = f"[dim {GREY_TEXT}]({len(mcp_tools)} MCP tools)[/]"
            tools_node.add(summary_text)
        elif regular_tools and not mcp_tools:
            summary_text = f"[dim {GREY_TEXT}]({len(regular_tools)} regular tools)[/]"
            tools_node.add(summary_text)
        elif not regular_tools and not mcp_tools:
            tools_node.add(f"[dim {GREY_TEXT}](No tools)[/]")

        # Add handoffs
        transfers_node = node.add(f"[bold {CAI_GREEN}]Handoffs[/bold {CAI_GREEN}]")

        # First, handle old-style handoffs through handoffs list
        for handoff_fn in getattr(agent, "handoffs", []):
            if callable(handoff_fn) and not hasattr(handoff_fn, "agent_name"):
                try:
                    next_agent = handoff_fn()
                    if next_agent:
                        transfer_node = transfers_node.add(
                            f"[bold {CAI_GREEN}]Agent:[/bold {CAI_GREEN}] "
                            f"[white]{next_agent.name}[/white]"
                        )
                        add_agent_node(next_agent, transfer_node, True)
                except Exception:
                    continue
            elif hasattr(handoff_fn, "agent_name"):
                # Handle SDK handoff objects
                try:
                    handoff_name = handoff_fn.agent_name
                    # Find the actual agent instance if available
                    next_agent = None

                    # Try to find the agent by name in the global namespace
                    # This is a heuristic and might not always work
                    import sys

                    for module_name, module in sys.modules.items():
                        if module_name.startswith("cai.agents"):
                            agent_var_name = handoff_name.lower().replace(" ", "_") + "_agent"
                            if hasattr(module, agent_var_name):
                                next_agent = getattr(module, agent_var_name)
                                break

                    if next_agent:
                        transfer_node = transfers_node.add(
                            f"[white]Agent: {handoff_name}[/white]"
                            f"[dim {GREY_TEXT}] via {handoff_fn.tool_name}[/]"
                        )
                        add_agent_node(next_agent, transfer_node, True)
                    else:
                        # If we can't find the agent, just show the name
                        transfers_node.add(
                            f"[white]Agent: {handoff_name}[/white]"
                            f"[dim {GREY_TEXT}] via {handoff_fn.tool_name}[/]"
                        )
                except Exception as e:
                    transfers_node.add(
                        f"[dim {GREY_TEXT}]Error:[/dim {GREY_TEXT}] [white]{str(e)}[/white]"
                    )
            elif isinstance(handoff_fn, dict) and "agent_name" in handoff_fn:
                # Handle dictionary handoff objects
                handoff_name = handoff_fn["agent_name"]
                tool_name = handoff_fn.get("tool_name", f"transfer_to_{handoff_name}")
                transfers_node.add(
                    f"[white]Agent: {handoff_name}[/white][dim {GREY_TEXT}] via {tool_name}[/]"
                )

        return node

    # Start traversal from the root agent
    add_agent_node(start_agent)
    console.print(tree)


def setup_ctf():
    """Setup CTF environment if CTF_NAME is provided

    Supports parallel execution via CTF_INSTANCE_ID environment variable.
    When CTF_INSTANCE_ID is set (e.g., "_1", "_2"), containers will be named
    ctf_target_1, ctf_target_2, etc., and assigned unique IPs on the shared network.
    """
    from mako.template import Template
    from wasabi import color

    ctf_name = os.getenv("CTF_NAME", None)
    if not ctf_name:
        print(color("CTF name not provided, necessary to run CTF", fg="white", bg="red"))
        sys.exit(1)

    instance_id = os.getenv("CTF_INSTANCE_ID", "")
    instance_suffix = f" (Instance {instance_id})" if instance_id else ""

    print(
        color(f"Setting up CTF{instance_suffix}: ", fg="black", bg="yellow")
        + color(ctf_name, fg="black", bg="yellow")
    )

    # Let ctf.py handle container naming and IP assignment based on CTF_INSTANCE_ID
    # Only pass container_name if explicitly overridden
    ctf_kwargs = {
        "subnet": os.getenv("CTF_SUBNET", "192.168.3.0/24"),
    }

    # Only override container_name if CTF_CONTAINER_NAME is explicitly set
    if os.getenv("CTF_CONTAINER_NAME"):
        ctf_kwargs["container_name"] = os.getenv("CTF_CONTAINER_NAME")
    else:
        # Use default that ctf.py will make unique with instance_id
        ctf_kwargs["container_name"] = "ctf_target"

    # Only override IP if CTF_IP is explicitly set and no instance_id
    # (parallel instances auto-assign IPs)
    if os.getenv("CTF_IP") and not instance_id:
        custom_ip = os.getenv("CTF_IP")
        # Validate against reserved IPs (192.168.3.5 is reserved for attacker)
        if custom_ip.endswith(".5"):
            print(
                color(
                    f"WARNING: IP {custom_ip} is reserved for the attacker/agent. "
                    "This may cause conflicts. Consider using a different IP or let the system auto-assign.",
                    fg="yellow",
                    bg="red",
                    bold=True
                )
            )
        ctf_kwargs["ip_address"] = custom_ip

    ctf = ptt.ctf(ctf_name, **ctf_kwargs)  # pylint: disable=I1101  # noqa
    ctf.start_ctf()

    # Only set CAI_ACTIVE_CONTAINER if CTF_INSIDE is true
    if os.getenv("CTF_INSIDE", "true").lower() == "true":
        try:
            import subprocess
            # Use the actual container name (which may include instance_id)
            container_name = ctf.container_name
            # Get the container ID for the specific container
            result = subprocess.run(
                ["docker", "ps", "--filter", f"name={container_name}", "--format", "{{.ID}}"],
                capture_output=True,
                text=True,
                check=False
            )

            if result.returncode == 0 and result.stdout.strip():
                container_id = result.stdout.strip()
                # Set the CTF container as the active container
                os.environ["CAI_ACTIVE_CONTAINER"] = container_id
                print(
                    color(f"CTF container {container_name} ({container_id[:12]}) set as active environment", fg="black", bg="green")
                )
            else:
                print(
                    color(f"Warning: Could not find {container_name} container ID", fg="white", bg="yellow")
                )
        except Exception as e:
            print(
                color(f"Warning: Could not set CTF container as active: {str(e)}", fg="white", bg="yellow")
            )

    # Get the challenge from the environment variable or default to the
    # first challenge
    challenge_key = os.getenv("CTF_CHALLENGE")  # TODO:
    challenges = list(ctf.get_challenges().keys())
    challenge = (
        challenge_key
        if challenge_key in challenges
        else (challenges[0] if len(challenges) > 0 else None)
    )

    # Use the user master template
    template_path = pathlib.Path(__file__).parent.parent / "prompts" / "core" / "user_master_template.md"
    messages = Template(filename=str(template_path)).render(
        ctf=ctf,
        challenge=challenge,
        ip=ctf.get_ip() if ctf else None,
    )

    print(
        color("Testing CTF: ", fg="black", bg="yellow") + color(ctf.name, fg="black", bg="yellow")
    )
    if not challenge_key or challenge_key not in challenges:
        print(
            color(
                "No challenge provided or challenge not found. Attempting to use the first challenge.",
                fg="white",
                bg="blue",
            )
        )
    if challenge:
        print(
            color("Testing challenge: ", fg="white", bg="blue")
            + color(
                "'" + challenge + "' (" + repr(ctf.flags[challenge]) + ")", fg="white", bg="blue"
            )
        )

    return ctf, messages


def update_agent_models_recursively(agent, new_model, visited=None):
    """
    Recursively update the model for an agent and all agents in its handoffs.

    Args:
        agent: The agent to update
        new_model: The new model string to set
        visited: Set of agent names already visited to prevent infinite loops
    """
    if visited is None:
        visited = set()

    # Avoid infinite loops by tracking visited agents
    if agent.name in visited:
        return
    visited.add(agent.name)

    # Update the main agent's model
    if hasattr(agent, "model"):
        # If agent.model is a string, update it directly
        if isinstance(agent.model, str):
            agent.model = new_model
        # If agent.model is a Model object, update its model attribute
        elif hasattr(agent.model, "model"):
            agent.model.model = new_model
            # Also ensure the agent name is set correctly in the model
            if hasattr(agent.model, "agent_name"):
                agent.model.agent_name = agent.name

            # IMPORTANT: Clear any cached state in the model that might be model-specific
            # This ensures the model doesn't have stale state from the previous model
            if hasattr(agent.model, "_client"):
                # Force recreation of the client on next use
                agent.model._client = None
            if hasattr(agent.model, "_converter"):
                # Reset the converter's state
                if hasattr(agent.model._converter, "recent_tool_calls"):
                    agent.model._converter.recent_tool_calls.clear()
                if hasattr(agent.model._converter, "tool_outputs"):
                    agent.model._converter.tool_outputs.clear()

    # Update models for all handoff agents
    if hasattr(agent, "handoffs"):
        for handoff_item in agent.handoffs:
            # Handle both direct Agent references and Handoff objects
            if hasattr(handoff_item, "on_invoke_handoff"):
                # This is a Handoff object
                # For handoffs created with the handoff() function, the agent is stored
                # in the closure of the on_invoke_handoff function
                # We can try to extract it from the function's closure
                try:
                    # Get the closure variables of the handoff function
                    if (
                        hasattr(handoff_item.on_invoke_handoff, "__closure__")
                        and handoff_item.on_invoke_handoff.__closure__
                    ):
                        for cell in handoff_item.on_invoke_handoff.__closure__:
                            if hasattr(cell.cell_contents, "model") and hasattr(
                                cell.cell_contents, "name"
                            ):
                                # This looks like an agent
                                handoff_agent = cell.cell_contents
                                update_agent_models_recursively(handoff_agent, new_model, visited)
                                break
                except Exception:
                    # If we can't extract the agent from closure, skip it
                    pass
            elif hasattr(handoff_item, "model"):
                # This is a direct Agent reference
                update_agent_models_recursively(handoff_item, new_model, visited)
