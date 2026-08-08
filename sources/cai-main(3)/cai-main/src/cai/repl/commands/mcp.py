"""REPL ``/mcp`` command: load/list/add/remove MCP servers and attach tools to agents.

Authoritative syntax and notes are rendered by ``mcp_help_panel_markup()`` and shown for
``/mcp help``, ``/help mcp``, and ``/h mcp`` (same content).
"""

# Standard library imports
import asyncio
import atexit
import functools
import warnings
import logging
from typing import Any, Dict, List, Optional, cast

# Third-party imports
from rich import box
from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

# Local imports
from cai.agents import get_agent_by_name, get_available_agents
from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title
from cai.sdk.agents.mcp import (
    MCPServer,
    MCPServerSse,
    MCPServerSseParams,
    MCPServerStdio,
    MCPServerStdioParams,
    MCPUtil,
)
from cai.sdk.agents import Agent
from cai.sdk.agents.tool import FunctionTool

console = Console()

_MCP_TABLE_HEADER = f"bold {_CAI_GREEN}"
_MCP_COL_MUTED = "#9aa0a6"
_MCP_COL_BODY = "white"
_MCP_PANEL_ERROR_BORDER = "red"
_MCP_PANEL_WARN_BORDER = "#ccaa33"


def _mcp_emit_panel(
    body: str,
    *,
    title: str,
    border_style: str = _CAI_GREEN,
    padding: Any = (1, 1),
) -> None:
    """Rounded panel for MCP notices (palette-aligned with ``/h mcp``)."""
    console.print(
        Panel(
            Text.from_markup(body, overflow="fold"),
            title=_quick_guide_subpanel_title(title),
            title_align="left",
            border_style=border_style,
            box=box.ROUNDED,
            padding=padding,
        )
    )


def _mcp_emit_panel_table(table: Table, *, title: str) -> None:
    """Wrap a table in the same rounded chrome as MCP help panels."""
    console.print(
        Panel(
            table,
            title=_quick_guide_subpanel_title(title),
            title_align="left",
            border_style=_CAI_GREEN,
            box=box.ROUNDED,
            padding=(0, 1),
        )
    )


def _mcp_table_embedded(**kwargs: Any) -> Table:
    """Table body for use inside a ``Panel`` (avoids double heavy borders)."""
    defaults: Dict[str, Any] = {
        "box": box.MINIMAL,
        "show_header": True,
        "header_style": _MCP_TABLE_HEADER,
        "title_style": _MCP_TABLE_HEADER,
        "padding": (0, 0),
    }
    defaults.update(kwargs)
    return Table(**defaults)


def _mcp_table(**kwargs: Any) -> Table:
    """Rich table with rounded corners and CAI palette border (aligned with help panels)."""
    defaults: Dict[str, Any] = {
        "box": box.ROUNDED,
        "border_style": _CAI_GREEN,
        "show_header": True,
        "header_style": _MCP_TABLE_HEADER,
        "title_style": _MCP_TABLE_HEADER,
        "padding": (0, 1),
    }
    defaults.update(kwargs)
    return Table(**defaults)


def mcp_help_panel_markup() -> str:
    """Rich markup for ``/mcp help``, ``/help mcp``, and ``/h mcp`` (single source)."""
    z = _CAI_GREEN
    return (
        "[white]MCP: connect external tool servers and bind their tools to agents.[/white]\n\n"
        f"[bold {z}]Subcommands[/bold {z}]\n"
        f"• [bold {z}]/mcp load <url> <name>[/bold {z}] — SSE server\n"
        f"• [bold {z}]/mcp load sse <url> <name>[/bold {z}] — [dim]legacy SSE form[/dim]\n"
        f"• [bold {z}]/mcp load stdio <name> <command>[/bold {z}] [dim][args…][/dim] — stdio server\n"
        f"• [bold {z}]/mcp list[/bold {z}] — [dim]active servers ([/dim][bold {z}]/mcp[/bold {z}]"
        f"[dim] with no args is the same)[/dim]\n"
        f"• [bold {z}]/mcp add <server> <agent>[/bold {z}] — [dim]server name first, then agent name or #[/dim]\n"
        f"• [bold {z}]/mcp remove <server>[/bold {z}]\n"
        f"• [bold {z}]/mcp tools <server>[/bold {z}]\n"
        f"• [bold {z}]/mcp status[/bold {z}]\n"
        f"• [bold {z}]/mcp associations[/bold {z}]\n"
        f"• [bold {z}]/mcp test <server>[/bold {z}]\n"
        f"• [bold {z}]/mcp help[/bold {z}] [dim](same as /help mcp, /h mcp)[/dim]\n\n"
        f"[bold {z}]Examples[/bold {z}]\n"
        f"• [bold {z}]/mcp load stdio burp java -jar /path/to/mcp-proxy-all.jar --sse-url http://127.0.0.1:9876[/bold {z}]\n"
        f"  [dim]# Burp Suite MCP (PortSwigger): stdio proxy to the BApp SSE port; extract mcp-proxy-all.jar from the extension[/dim]\n"
        f"• [bold {z}]/mcp load http://127.0.0.1:8000/sse myserver[/bold {z}]\n"
        f"  [dim]# Direct SSE only for servers that return Content-Type: text/event-stream (many need stdio instead)[/dim]\n"
        f"• [bold {z}]/mcp tools burp[/bold {z}]\n"
        f"• [bold {z}]/mcp add burp redteam_agent[/bold {z}]\n\n"
        "[dim]Alias: /m[/dim]"
    )


# Global registry for persistent MCP connections
_GLOBAL_MCP_SERVERS: Dict[str, MCPServer] = {}

# Per-server locks to serialize tool invocations for persistent connections
_SERVER_INVOCATION_LOCKS: Dict[str, asyncio.Lock] = {}

# Global registry for agent-MCP associations
# Maps agent name to list of MCP server names
_AGENT_MCP_ASSOCIATIONS: Dict[str, List[str]] = {}


# Registry of tool name -> MCP server name for UI visualization
_MCP_TOOL_NAME_TO_SERVER: Dict[str, str] = {}


def register_mcp_tool_name(tool_name: str, server_name: str) -> None:
    """Register mapping used by the TUI to decorate MCP tools."""
    try:
        _MCP_TOOL_NAME_TO_SERVER[str(tool_name)] = str(server_name)
    except Exception:
        pass


def get_mcp_server_for_tool(tool_name: str) -> Optional[str]:
    """Return server name for a given tool if known."""
    try:
        return _MCP_TOOL_NAME_TO_SERVER.get(str(tool_name))
    except Exception:
        return None


def unregister_mcp_tools_for_server(server_name: str) -> None:
    """Drop tool-name registry entries for a removed MCP server."""
    try:
        to_del = [k for k, v in _MCP_TOOL_NAME_TO_SERVER.items() if v == server_name]
        for k in to_del:
            del _MCP_TOOL_NAME_TO_SERVER[k]
    except Exception:
        pass


def _strip_mcp_tools_for_server_from_agent(agent_obj: Any, server_name: str) -> None:
    """Remove GlobalMCPUtil tools tied to ``server_name`` from ``agent_obj.tools``."""
    tools = getattr(agent_obj, "tools", None)
    if not tools:
        return
    agent_obj.tools = [
        t
        for t in tools
        if getattr(t, "_mcp_server", None) != server_name
        and get_mcp_server_for_tool(getattr(t, "name", "")) != server_name
    ]


def merge_mcp_tools_into_session_agent(agent_type_key: str, tools: List[FunctionTool]) -> None:
    """Append MCP ``tools`` to the active session agent if its type matches ``agent_type_key``.

    The module singleton and the REPL session agent can diverge after ``get_agent_by_name``
    (factory clones replace ``.tools`` with a new list). ``/mcp add`` must update both.
    """
    if not tools:
        return
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
    except Exception:
        return
    active = AGENT_MANAGER.get_active_agent()
    if not active:
        return
    model = getattr(active, "model", None)
    active_type = getattr(model, "agent_type", None) if model else None
    if not active_type or str(active_type).lower() != agent_type_key.lower():
        return
    if not hasattr(active, "tools") or active.tools is None:
        active.tools = []
    new_names = {t.name for t in tools}
    active.tools = [t for t in active.tools if t.name not in new_names]
    active.tools.extend(tools)


def strip_mcp_server_from_session_agents(server_name: str) -> None:
    """Remove MCP tools for ``server_name`` from singleton agents and the active session agent."""
    for ag in get_available_agents().values():
        if isinstance(ag, Agent):
            _strip_mcp_tools_for_server_from_agent(ag, server_name)
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        active = AGENT_MANAGER.get_active_agent()
        if active:
            _strip_mcp_tools_for_server_from_agent(active, server_name)
    except Exception:
        pass


# Custom MCPUtil that uses global registry
class GlobalMCPUtil(MCPUtil):
    """Custom MCP utility that uses global server registry"""

    @classmethod
    def to_function_tool(cls, tool, server_name: str) -> FunctionTool:
        """Convert an MCP tool to a CAI function tool using server name instead of object."""

        # Store the server configuration instead of the server object
        server = _GLOBAL_MCP_SERVERS.get(server_name)
        if not server:
            raise ValueError(f"Server {server_name} not found in registry")

        # Capture server configuration
        server_config = {
            "name": server_name,
            "type": type(server).__name__,
            "tool_name": tool.name,
            "tool_schema": tool.inputSchema,
            "tool_description": tool.description,
            "persistent": isinstance(server, MCPServerStdio),
        }

        # For SSE servers, capture the URL
        if isinstance(server, MCPServerSse):
            server_config["url"] = server.params.get("url")
            server_config["headers"] = MCPUtil.get_default_auth_headers(
                server.params.get("headers")
            )
            server_config["timeout"] = server.params.get("timeout", 5)
            server_config["sse_read_timeout"] = server.params.get("sse_read_timeout", 60 * 5)
        # For STDIO servers, capture the command
        elif isinstance(server, MCPServerStdio):
            server_config["command"] = server.params.command
            server_config["args"] = server.params.args
            server_config["env"] = getattr(server.params, "env")
            server_config["cwd"] = getattr(server.params, "cwd")
            server_config["encoding"] = getattr(server.params, "encoding", "utf-8")
            server_config["encoding_error_handler"] = getattr(
                server.params, "encoding_error_handler", "strict"
            )

        # Create a custom invoke function that manages the server lifecycle per invocation
        async def invoke_with_fresh_connection(config, context, input_json):
            """Invoke an MCP tool, keeping STDIO transports persistent."""
            import asyncio
            import json
            import warnings

            from cai.sdk.agents.exceptions import AgentsException, ModelBehaviorError
            from cai.sdk.agents.mcp import MCPServerSse, MCPServerStdio

            # Parse JSON input
            try:
                json_data = json.loads(input_json) if input_json else {}
            except Exception as e:
                raise ModelBehaviorError(
                    f"Invalid JSON input for tool {config['tool_name']}: {input_json}"
                ) from e

            result = None
            max_retries = 2
            retry_count = 0
            server = None
            should_cleanup = False
            persistent = bool(config.get("persistent"))

            # Suppress warnings about async generator cleanup
            with warnings.catch_warnings():
                warnings.filterwarnings("ignore", category=RuntimeWarning)
                warnings.filterwarnings("ignore", message=".*asynchronous generator.*")
                warnings.filterwarnings("ignore", message=".*ClosedResourceError.*")

                try:
                    if persistent:
                        server_name = config["name"]
                        server = _GLOBAL_MCP_SERVERS.get(server_name)
                        if not server or not isinstance(server, MCPServerStdio):
                            raise AgentsException(
                                f"MCP server '{server_name}' is unavailable. Use /mcp status to verify it is loaded."
                            )

                        lock = _SERVER_INVOCATION_LOCKS.setdefault(
                            server_name, asyncio.Lock()
                        )

                        async with lock:
                            while retry_count < max_retries:
                                try:
                                    if not getattr(server, "session", None):
                                        try:
                                            await asyncio.wait_for(server.connect(), timeout=10.0)
                                        except asyncio.TimeoutError:
                                            raise AgentsException(
                                                f"Timeout connecting to MCP server for tool {config['tool_name']}. "
                                                "The server may be down or not responding."
                                            )

                                    result = await asyncio.wait_for(
                                        server.call_tool(config["tool_name"], json_data),
                                        timeout=30.0,
                                    )
                                    break
                                except asyncio.TimeoutError:
                                    raise AgentsException(
                                        f"Timeout calling MCP tool {config['tool_name']}. "
                                        f"The tool took too long to respond."
                                    )
                                except Exception:
                                    retry_count += 1
                                    if retry_count >= max_retries:
                                        raise
                                    import logging

                                    logging.debug(
                                        f"Retrying MCP tool {config['tool_name']} (attempt {retry_count}/{max_retries})"
                                    )
                                    try:
                                        await server.cleanup()
                                    except Exception:
                                        pass
                                    server.session = None
                                    await asyncio.sleep(0.5)
                    else:
                        if config["type"] == "MCPServerSse":
                            # Create new SSE server
                            headers = MCPUtil.get_default_auth_headers(config.get("headers"))
                            params = {
                                "url": config["url"],
                                "headers": headers,
                                "timeout": config.get("timeout", 5),
                                "sse_read_timeout": config.get("sse_read_timeout", 60 * 5),
                            }
                            # Remove None values
                            params = {k: v for k, v in params.items() if v is not None}

                            server = MCPServerSse(
                                params,
                                name=config["name"],
                                cache_tools_list=False,  # Don't cache since it's temporary
                            )
                        elif config["type"] == "MCPServerStdio":
                            # Create new STDIO server
                            params = {
                                "command": config["command"],
                                "args": config.get("args", []),
                                "env": config.get("env"),
                                "cwd": config.get("cwd"),
                                "encoding": config.get("encoding", "utf-8"),
                                "encoding_error_handler": config.get(
                                    "encoding_error_handler", "strict"
                                ),
                            }
                            # Remove None values
                            params = {k: v for k, v in params.items() if v is not None}

                            server = MCPServerStdio(
                                params, name=config["name"], cache_tools_list=False
                            )
                        else:
                            raise AgentsException(f"Unknown server type: {config['type']}")

                        should_cleanup = True

                        while retry_count < max_retries:
                            try:
                                try:
                                    await asyncio.wait_for(server.connect(), timeout=10.0)
                                except asyncio.TimeoutError:
                                    raise AgentsException(
                                        f"Timeout connecting to MCP server for tool {config['tool_name']}. "
                                        f"The server may be down or not responding."
                                    )

                                result = await asyncio.wait_for(
                                    server.call_tool(config["tool_name"], json_data),
                                    timeout=30.0,
                                )
                                break
                            except asyncio.TimeoutError:
                                raise AgentsException(
                                    f"Timeout calling MCP tool {config['tool_name']}. "
                                    f"The tool took too long to respond."
                                )
                            except Exception:
                                retry_count += 1
                                if retry_count >= max_retries:
                                    raise
                                import logging

                                logging.debug(
                                    f"Retrying MCP tool {config['tool_name']} (attempt {retry_count}/{max_retries})"
                                )
                                if isinstance(server, MCPServerSse) and hasattr(server, "session"):
                                    server.session = None
                                await asyncio.sleep(0.5)
                except Exception as e:
                    # Handle ClosedResourceError and connection issues
                    error_type = type(e).__name__
                    error_str = str(e).lower()

                    # Improved error messages for common issues
                    if (
                        error_type in ("ClosedResourceError", "ExceptionGroup")
                        or "closedresourceerror" in error_str
                        or "closed" in error_str
                        or "connection" in error_str
                    ):
                        raise AgentsException(
                            f"Connection lost to MCP server for tool {config['tool_name']}. "
                            "Use /mcp status to reconnect if the issue persists."
                        ) from e
                    raise AgentsException(
                        f"Error invoking MCP tool {config['tool_name']}: {type(e).__name__}: {str(e)}"
                    ) from e

                finally:
                    if should_cleanup and server:
                        if isinstance(server, MCPServerSse):
                            try:
                                await asyncio.wait_for(server.cleanup(), timeout=0.5)
                            except (asyncio.TimeoutError, RuntimeError, Exception):
                                pass
                            server.session = None
                        else:
                            try:
                                await asyncio.wait_for(server.cleanup(), timeout=5.0)
                            except (asyncio.TimeoutError, Exception):
                                pass

            # Format the result
            if not result:
                raise AgentsException(f"No result returned from MCP tool {config['tool_name']}")

            # Reuse the shared MCP formatting helper so tools return readable text
            tool_output = await MCPUtil._format_tool_result(result, tool, server)

            return tool_output

        # Use functools.partial to bind the server config
        invoke_func = functools.partial(invoke_with_fresh_connection, server_config)

        ft = FunctionTool(
            name=tool.name,
            description=tool.description or "",
            params_json_schema=tool.inputSchema,
            on_invoke_tool=invoke_func,
            strict_json_schema=False,
        )
        # Mark and register for UI
        try:
            setattr(ft, "_is_mcp_tool", True)
            setattr(ft, "_mcp_server", server_name)
            register_mcp_tool_name(tool.name, server_name)
        except Exception:
            pass
        return ft


def cleanup_mcp_servers():
    """Cleanup all MCP servers on exit"""
    try:
        if _GLOBAL_MCP_SERVERS:
            import warnings

            # Suppress async generator warnings during cleanup
            with warnings.catch_warnings():
                warnings.filterwarnings("ignore", category=RuntimeWarning)
                warnings.filterwarnings("ignore", message=".*asynchronous generator.*")

                # Create new event loop for cleanup if needed
                try:
                    loop = asyncio.get_running_loop()
                except RuntimeError:
                    loop = asyncio.new_event_loop()
                    asyncio.set_event_loop(loop)

                async def cleanup_all():
                    tasks = []
                    for name, server in _GLOBAL_MCP_SERVERS.items():
                        try:
                            # For SSE servers, use a very short timeout
                            if isinstance(server, MCPServerSse):
                                tasks.append(asyncio.wait_for(server.cleanup(), timeout=0.1))
                            else:
                                tasks.append(server.cleanup())
                        except Exception:
                            pass
                    if tasks:
                        await asyncio.gather(*tasks, return_exceptions=True)

                loop.run_until_complete(cleanup_all())
                # Only close the loop if it's not running
                if not loop.is_running():
                    loop.close()
        _SERVER_INVOCATION_LOCKS.clear()
    except Exception:
        pass


# Register cleanup on exit
atexit.register(cleanup_mcp_servers)


class MCPCommand(Command):
    """Command for managing MCP servers and their integration with agents."""

    def __init__(self):
        """Initialize the MCP command."""
        super().__init__(
            name="/mcp",
            description="Manage MCP servers and add their tools to agents",
            aliases=["/m"],
        )

        # Add subcommands manually
        self._subcommands = {
            "load": "Load an MCP server (SSE or stdio)",
            "list": "List active MCP connections",
            "add": "Add MCP tools to an agent",
            "remove": "Remove an MCP server connection",
            "tools": "List tools from an MCP server",
            "status": "Check MCP server connection status",
            "associations": "Show agent-MCP associations",
            "test": "Test MCP server connectivity",
            "help": "Show MCP command usage",
        }

    def get_subcommands(self) -> List[str]:
        """Get list of subcommand names.

        Returns:
            List of subcommand names
        """
        return list(self._subcommands.keys())

    def get_subcommand_description(self, subcommand: str) -> str:
        """Get description for a subcommand.

        Args:
            subcommand: Name of the subcommand

        Returns:
            Description of the subcommand
        """
        return self._subcommands.get(subcommand, "")

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the MCP command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully
        """
        if not args:
            return self.handle_list(args)

        subcommand = args[0]
        if subcommand in self._subcommands:
            handler = getattr(self, f"handle_{subcommand}", None)
            if handler:
                return handler(args[1:] if len(args) > 1 else None)

        _mcp_emit_panel(
            f"[red bold]Unknown subcommand[/red bold] [white]{escape(subcommand)}[/white]\n\n"
            f"[#9aa0a6]Supported commands are listed under[/] [bold {_CAI_GREEN}]/mcp help[/bold {_CAI_GREEN}]"
            f"[#9aa0a6].[/]",
            title="MCP",
            border_style=_MCP_PANEL_ERROR_BORDER,
        )
        self.show_usage()
        return False

    def show_usage(self):
        """Show usage information for the MCP command."""
        console.print(
            Panel(
                mcp_help_panel_markup(),
                title=_quick_guide_subpanel_title("MCP Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
                box=box.ROUNDED,
            )
        )

    def handle_help(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp help command.

        Args:
            args: Optional list of command arguments (not used)

        Returns:
            True
        """
        self.show_usage()
        return True

    def _run_async(self, coro):
        """Run async code properly in the CLI context.

        Args:
            coro: The coroutine to run

        Returns:
            The result of the coroutine
        """
        try:
            # Try to get existing loop
            loop = asyncio.get_running_loop()
            # If we're in a loop, we need to use a different approach
            import concurrent.futures
            import sys
            from io import StringIO

            def run_in_thread():
                # Suppress stderr in the thread too
                original_stderr = sys.stderr
                try:
                    sys.stderr = StringIO()
                    new_loop = asyncio.new_event_loop()
                    asyncio.set_event_loop(new_loop)
                    try:
                        return new_loop.run_until_complete(coro)
                    finally:
                        new_loop.close()
                finally:
                    sys.stderr = original_stderr

            with concurrent.futures.ThreadPoolExecutor() as executor:
                future = executor.submit(run_in_thread)
                return future.result(timeout=30)

        except RuntimeError:
            # No running loop, we can use asyncio.run
            import sys
            from io import StringIO

            # Suppress stderr during asyncio.run
            original_stderr = sys.stderr
            try:
                sys.stderr = StringIO()
                return asyncio.run(coro)
            finally:
                sys.stderr = original_stderr

    def handle_load(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp load command.

        Usage:
            /mcp load <url> <name>                 - Load SSE server
            /mcp load sse <url> <name>             - Load SSE server (legacy form, kept for compatibility)
            /mcp load stdio <name> <command> [args...] - Load stdio server

        Args:
            args: List of command arguments

        Returns:
            True if successful
        """
        if not args or len(args) < 2:
            _mcp_emit_panel(
                "[red bold]Invalid arguments for[/red bold] [bold]/mcp load[/bold]\n\n"
                "[white]SSE (default)[/white]\n"
                f"[bold {_CAI_GREEN}]/mcp load <url> <name>[/bold {_CAI_GREEN}]\n\n"
                "[white]SSE (legacy)[/white]\n"
                f"[bold {_CAI_GREEN}]/mcp load sse <url> <name>[/bold {_CAI_GREEN}]\n\n"
                "[white]stdio[/white]\n"
                f"[bold {_CAI_GREEN}]/mcp load stdio <name> <command>[/bold {_CAI_GREEN}] [dim][args…][/dim]",
                title="MCP — load",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        # Check if it's a stdio server
        if args[0] == "stdio":
            if len(args) < 3:
                _mcp_emit_panel(
                    "[red bold]stdio load needs a server name and a command[/red bold]\n\n"
                    f"[bold {_CAI_GREEN}]/mcp load stdio <name> <command>[/bold {_CAI_GREEN}] [dim][args…][/dim]",
                    title="MCP — load",
                    border_style=_MCP_PANEL_ERROR_BORDER,
                )
                return False

            name = args[1]
            command = args[2]
            cmd_args = args[3:] if len(args) > 3 else []

            return self._load_stdio_server(name, command, cmd_args)
        else:
            # SSE server
            # Support both:
            #   /mcp load <url> <name>
            #   /mcp load sse <url> <name>
            if args[0] == "sse":
                if len(args) < 3:
                    _mcp_emit_panel(
                        "[red bold]Missing URL or server name[/red bold]\n\n"
                        f"[bold {_CAI_GREEN}]/mcp load sse <url> <name>[/bold {_CAI_GREEN}]",
                        title="MCP — load",
                        border_style=_MCP_PANEL_ERROR_BORDER,
                    )
                    return False
                url = args[1]
                name = args[2]
            else:
                url = args[0]
                if len(args) < 2:
                    _mcp_emit_panel(
                        "[red bold]Missing local server name[/red bold]\n\n"
                        f"[bold {_CAI_GREEN}]/mcp load <url> <name>[/bold {_CAI_GREEN}]",
                        title="MCP — load",
                        border_style=_MCP_PANEL_ERROR_BORDER,
                    )
                    return False
                name = args[1]

            return self._load_sse_server(url, name)

    def _load_sse_server(self, url: str, name: str) -> bool:
        """Load an SSE MCP server.

        Args:
            url: URL of the SSE server
            name: Name to identify the server

        Returns:
            True if successful
        """
        if name in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[{_MCP_COL_MUTED}]Server[/] [bold {_CAI_GREEN}]{escape(name)}[/bold {_CAI_GREEN}] "
                f"[{_MCP_COL_MUTED}]is already loaded.[/]\n\n"
                f"[white]To reload, remove it first:[/white] [bold {_CAI_GREEN}]/mcp remove "
                f"{escape(name)}[/bold {_CAI_GREEN}]",
                title="MCP — load",
                border_style=_MCP_PANEL_WARN_BORDER,
            )
            return True

        console.print(
            f"[{_MCP_COL_MUTED}]Connecting to SSE[/] [bold {_CAI_GREEN}]{escape(url)}[/bold {_CAI_GREEN}]"
            f"[{_MCP_COL_MUTED}]…[/]"
        )

        # Preflight validation to catch broken SSE servers early
        def _preflight_sse(endpoint: str) -> bool:
            try:
                import requests
                headers = MCPUtil.get_default_auth_headers(
                    {
                        "Accept": "text/event-stream",
                        "Cache-Control": "no-cache",
                        "Connection": "keep-alive",
                    }
                )
                with requests.get(endpoint, headers=headers, stream=True, timeout=5) as resp:
                    # Validate headers (warn if missing)
                    ct = resp.headers.get("Content-Type", "")
                    cc = resp.headers.get("Cache-Control", "")
                    ka = resp.headers.get("Connection", "")
                    if "text/event-stream" not in ct:
                        console.print(
                            f"[{_MCP_PANEL_WARN_BORDER}][CAI] Warning:[/] "
                            f"[{_MCP_COL_MUTED}]Content-Type is not text/event-stream.[/]"
                        )
                    if "no-cache" not in cc.lower():
                        console.print(
                            f"[{_MCP_PANEL_WARN_BORDER}][CAI] Warning:[/] "
                            f"[{_MCP_COL_MUTED}]Missing Cache-Control: no-cache.[/]"
                        )
                    if "keep-alive" not in ka.lower():
                        console.print(
                            f"[{_MCP_PANEL_WARN_BORDER}][CAI] Warning:[/] "
                            f"[{_MCP_COL_MUTED}]Missing Connection: keep-alive.[/]"
                        )

                    # Read a small portion to validate SSE framing
                    buffer = b""
                    event_name: str | None = None
                    data_lines: list[str] = []
                    for i, raw in enumerate(resp.iter_lines(chunk_size=1024, decode_unicode=False)):
                        if i > 50:
                            break  # don't hang
                        if raw is None:
                            continue
                        line = raw.strip()
                        if not line:
                            # empty line signals end of event; try to validate accumulated buffer
                            if buffer:
                                try:
                                    text = buffer.decode("utf-8", errors="ignore")

                                    # Parse SSE event: may contain event: and multiple data: lines
                                    event_name = None
                                    data_lines = []
                                    for sse_line in text.splitlines():
                                        if sse_line.startswith("event:"):
                                            event_name = sse_line.split(":", 1)[1].strip()
                                        elif sse_line.startswith("data:"):
                                            data_lines.append(sse_line.split(":", 1)[1].strip())

                                    # SwiftMCP / spec-compliant servers often send an initial
                                    # `event: endpoint` with a URL in data:. Accept that as
                                    # a valid SSE handshake even though it's not JSON-RPC.
                                    if event_name == "endpoint" and data_lines:
                                        return True

                                    if not data_lines:
                                        return False

                                    # For JSON-RPC style servers, the data payload must be a
                                    # JSON-RPC 2.0 message.
                                    payload = data_lines[-1]
                                    import json
                                    obj = json.loads(payload)
                                    if obj.get("jsonrpc") != "2.0":
                                        return False
                                    return True
                                except Exception:
                                    return False
                            continue
                        # accumulate until blank line
                        buffer += line + b"\n"
                return False
            except Exception:
                return False

        ok = _preflight_sse(url)
        if not ok:
            example = (
                "data: {\"jsonrpc\":\"2.0\", \"id\":\"1\", \"method\":\"server.ready\", \"params\":{}}\n\n"
            )
            _mcp_emit_panel(
                "[red bold]SSE preflight failed[/red bold]\n\n"
                "[#9aa0a6]The endpoint may be up, but it is not serving a valid MCP SSE stream yet.[/]\n"
                "[#9aa0a6]Required: text/event-stream headers and JSON-RPC 2.0 event payloads.[/]\n\n"
                "[white]Example [bold]data:[/bold] line[/white]\n"
                f"[dim]{escape(example)}[/dim]\n\n"
                "[#9aa0a6]Headers: Content-Type: text/event-stream, Cache-Control: no-cache, "
                "Connection: keep-alive[/]",
                title="MCP — load (SSE)",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            # Continue anyway; the lower-level client may handle it or produce a clearer error

        async def connect_and_test():
            params: MCPServerSseParams = {
                "url": url,
                "timeout": 10,  # Connection timeout
                "sse_read_timeout": 300,  # 5 minutes for SSE reads
                # Request headers to encourage proper SSE behavior on server side
                "headers": MCPUtil.get_default_auth_headers(
                    {
                        "Accept": "text/event-stream",
                        "Cache-Control": "no-cache",
                        "Connection": "keep-alive",
                    }
                ),
            }
            server = MCPServerSse(params, name=name, cache_tools_list=True)

            # Connect to the server with retry logic
            max_connect_retries = 3
            for attempt in range(max_connect_retries):
                try:
                    await server.connect()
                    break
                except Exception as e:
                    if attempt < max_connect_retries - 1:
                        await asyncio.sleep(1)  # Wait before retry
                        continue
                    raise

            # Test by listing tools
            tools = await server.list_tools()

            return server, tools

        try:
            # Suppress all stderr output during SSE connection
            import sys
            from io import StringIO

            # Save the original stderr
            original_stderr = sys.stderr

            try:
                # Redirect stderr to null
                sys.stderr = StringIO()

                # Also suppress warnings
                with warnings.catch_warnings():
                    warnings.filterwarnings("ignore", category=RuntimeWarning)
                    warnings.filterwarnings("ignore", message=".*asynchronous generator.*")
                    warnings.filterwarnings("ignore", message=".*cancel scope.*")
                    warnings.filterwarnings("ignore", message=".*didn't stop after athrow.*")

                    server, tools = self._run_async(connect_and_test())
            finally:
                # Always restore stderr
                sys.stderr = original_stderr

            # Store the server globally
            _GLOBAL_MCP_SERVERS[name] = server

            console.print(
                f"[green][CAI] Connected to SSE server [/][bold #00ff9d]{name}[/bold #00ff9d]"
                f"[green] at [/][bold white]{url}[/bold white]"
            )
            console.print(
                f"[#9aa0a6][CAI] Available tools:[/] [bold #00ff9d]{len(tools)}[/bold #00ff9d]"
            )

            # Show some tool names if available
            if tools:
                tool_names = [tool.name for tool in tools[:5]]
                if len(tools) > 5:
                    tool_names.append(f"... and {len(tools) - 5} more")
                console.print(f"[#9aa0a6][CAI] Tools:[/] [white]{', '.join(tool_names)}[/white]")

            return True

        except Exception as e:
            _mcp_emit_panel(
                "[red bold]Could not connect to the SSE server[/red bold]\n\n"
                f"[white]{escape(str(e))}[/white]\n\n"
                "[#9aa0a6]If the service is running, confirm it speaks MCP over SSE (not plain HTML).[/]",
                title="MCP — load (SSE)",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            # Clean up if connection failed
            if name in _GLOBAL_MCP_SERVERS:
                del _GLOBAL_MCP_SERVERS[name]
            return False

    def _load_stdio_server(self, name: str, command: str, cmd_args: List[str]) -> bool:
        """Load a stdio MCP server.

        Args:
            name: Name to identify the server
            command: Command to execute
            cmd_args: Arguments for the command

        Returns:
            True if successful
        """
        if name in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[{_MCP_COL_MUTED}]Server[/] [bold {_CAI_GREEN}]{escape(name)}[/bold {_CAI_GREEN}] "
                f"[{_MCP_COL_MUTED}]is already loaded.[/]\n\n"
                f"[white]To reload, remove it first:[/white] [bold {_CAI_GREEN}]/mcp remove "
                f"{escape(name)}[/bold {_CAI_GREEN}]",
                title="MCP — load",
                border_style=_MCP_PANEL_WARN_BORDER,
            )
            return True

        cmd_preview = escape(f"{command} {' '.join(cmd_args)}".strip())
        _mcp_emit_panel(
            f"[white]Starting stdio server[/white] [bold {_CAI_GREEN}]{escape(name)}[/bold {_CAI_GREEN}]\n"
            f"[dim]{cmd_preview}[/dim]",
            title="MCP — load (stdio)",
            padding=(0, 1),
        )

        async def connect_and_test():
            params: MCPServerStdioParams = {"command": command, "args": cmd_args}
            # Add safe defaults for stdio servers
            # - Force unbuffered Python when applicable
            # - Use replace error handler to avoid fatal decoding errors
            server = MCPServerStdio(params, name=name, cache_tools_list=True)

            # Connect to the server
            await server.connect()

            # Test by listing tools
            tools = await server.list_tools()

            # Quick sanity-check: Ensure tool metadata looks valid
            for t in tools:
                if not getattr(t, "name", None):
                    raise RuntimeError("Invalid tool with empty name from stdio server")

            return server, tools

        try:
            server, tools = self._run_async(connect_and_test())

            # Store the server globally
            _GLOBAL_MCP_SERVERS[name] = server

            console.print(f"[green]✓ Started stdio server '{name}'[/green]")
            console.print(f"Available tools: {len(tools)}")

            # Show some tool names if available
            if tools:
                tool_names = [tool.name for tool in tools[:5]]
                if len(tools) > 5:
                    tool_names.append(f"... and {len(tools) - 5} more")
                console.print(f"Tools: {', '.join(tool_names)}")

            return True

        except Exception as e:
            _mcp_emit_panel(
                "[red bold]Could not start the stdio server[/red bold]\n\n"
                f"[white]{escape(str(e))}[/white]\n\n"
                "[#9aa0a6]Check the command, PATH, and that the MCP binary is installed.[/]",
                title="MCP — load (stdio)",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            # Clean up if connection failed
            if name in _GLOBAL_MCP_SERVERS:
                del _GLOBAL_MCP_SERVERS[name]
            return False

    def handle_list(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp list command.

        Args:
            args: Optional list of command arguments (not used)

        Returns:
            True
        """
        if not _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                "[#9aa0a6]No MCP servers are loaded in this session.[/]\n\n"
                f"[white]Load one with[/white] [bold {_CAI_GREEN}]/mcp load <url> <name>[/bold {_CAI_GREEN}] "
                f"[white]or[/white] [bold {_CAI_GREEN}]/mcp load stdio <name> <cmd>[/bold {_CAI_GREEN}] "
                f"[dim][args…][/dim]\n"
                f"[white]Full syntax:[/white] [bold {_CAI_GREEN}]/mcp help[/bold {_CAI_GREEN}]",
                title="MCP — list",
            )
            return True

        table = _mcp_table_embedded()
        table.add_column("Name", style=_MCP_TABLE_HEADER)
        table.add_column("Type", style=_MCP_COL_MUTED)
        table.add_column("Details", style=_MCP_COL_BODY)
        table.add_column("Tools", style=_MCP_COL_MUTED)

        for name, server in _GLOBAL_MCP_SERVERS.items():
            server_type = type(server).__name__.replace("MCPServer", "")

            # Get server details
            if isinstance(server, MCPServerSse):
                details = server.params.get("url", "N/A")
            elif isinstance(server, MCPServerStdio):
                cmd = server.params.command
                args = " ".join(server.params.args)
                details = f"{cmd} {args}".strip()
            else:
                details = "Unknown"

            # Get tool count
            try:

                async def get_tools():
                    return await server.list_tools()

                tools = self._run_async(get_tools())
                tool_count = str(len(tools))
            except Exception:
                tool_count = "Error"

            table.add_row(name, server_type, details, tool_count)

        _mcp_emit_panel_table(table, title="Active MCP connections")
        return True

    def handle_add(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp add command.

        Usage: /mcp add <server_name> <agent_name>

        Args:
            args: List of command arguments

        Returns:
            True if successful
        """
        if not args or len(args) < 2:
            _mcp_emit_panel(
                "[red bold]Invalid arguments for[/red bold] [bold]/mcp add[/bold]\n\n"
                f"[bold {_CAI_GREEN}]/mcp add <server_name> <agent_name_or_number>[/bold {_CAI_GREEN}]",
                title="MCP — add",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server_name = args[0]
        agent_identifier = args[1]

        # Check if server exists
        if server_name not in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[red bold]Unknown server[/red bold] [white]{escape(server_name)}[/white]\n\n"
                f"[#9aa0a6]Load the server first, then list loaded names with[/] "
                f"[bold {_CAI_GREEN}]/mcp list[/bold {_CAI_GREEN}][#9aa0a6].[/]",
                title="MCP — add",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        # Get the agent
        try:
            agent = get_available_agents()[agent_identifier]
            agent_display_name = getattr(agent, "name", agent_identifier)
        except KeyError:
            # Try by index
            try:
                agents = get_available_agents()
                agent_list = list(agents.items())

                if agent_identifier.isdigit():
                    idx = int(agent_identifier)
                    if 1 <= idx <= len(agent_list):
                        agent_key, agent = agent_list[idx - 1]
                        agent_display_name = getattr(agent, "name", agent_key)
                    else:
                        raise ValueError("Invalid index")
                else:
                    raise ValueError("Not found")
            except Exception:
                _mcp_emit_panel(
                    f"[red bold]Agent not found[/red bold] [white]{escape(agent_identifier)}[/white]\n\n"
                    f"[white]Pick a name from[/white] [bold {_CAI_GREEN}]/agent list[/bold {_CAI_GREEN}] "
                    f"[white]or an index from that list.[/white]",
                    title="MCP — add",
                    border_style=_MCP_PANEL_ERROR_BORDER,
                )
                return False

        # Add the MCP server to the agent
        server = _GLOBAL_MCP_SERVERS[server_name]

        _mcp_emit_panel(
            f"[{_MCP_COL_MUTED}]Adding tools from[/] [bold {_CAI_GREEN}]{escape(server_name)}[/bold {_CAI_GREEN}] "
            f"[{_MCP_COL_MUTED}]→[/] [bold {_CAI_GREEN}]{escape(agent_display_name)}[/bold {_CAI_GREEN}]"
            f"[{_MCP_COL_MUTED}] …[/]",
            title="MCP — add",
            padding=(0, 1),
        )

        # Validate the server connection before adding
        try:

            async def validate_connection():
                try:
                    # Try to list tools to validate connection
                    tools = await server.list_tools()
                    return tools
                except Exception:
                    console.print(
                        f"[{_MCP_PANEL_WARN_BORDER}]Connection lost; reconnecting…[/]"
                    )
                    # Try to reconnect
                    await server.connect()
                    tools = await server.list_tools()
                    console.print(
                        f"[green]✓ Reconnected to[/] [bold {_CAI_GREEN}]{escape(server_name)}[/bold {_CAI_GREEN}]"
                    )
                    return tools

            # Validate the connection and get tools
            mcp_tools = self._run_async(validate_connection())

        except Exception as e:
            _mcp_emit_panel(
                f"[red bold]Cannot reach server[/red bold] [white]{escape(server_name)}[/white]\n\n"
                f"[white]{escape(str(e))}[/white]\n\n"
                "[#9aa0a6]Try[/] [bold]/mcp remove[/bold] [#9aa0a6]and load again, or[/] [bold]/mcp status[/bold]"
                "[#9aa0a6].[/]",
                title="MCP — add",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        # Get and display the tools
        try:
            # Create function tools using GlobalMCPUtil
            tools = []
            for mcp_tool in mcp_tools:
                # Use GlobalMCPUtil to create tools that use the global registry
                function_tool = GlobalMCPUtil.to_function_tool(mcp_tool, server_name)
                tools.append(function_tool)

            # Display tools table
            table = _mcp_table_embedded()
            table.add_column("Tool", style=_MCP_TABLE_HEADER)
            table.add_column("Status", style=_MCP_COL_BODY)
            table.add_column("Details", style=_MCP_COL_MUTED)

            for tool in tools:
                table.add_row(tool.name, "Added", f"Available as: {tool.name}")

            _mcp_emit_panel_table(
                table, title=f"Tools added → {escape(str(agent_display_name))}"
            )

            # Add tools directly to agent.tools
            if not hasattr(agent, "tools"):
                agent.tools = []

            # Remove any existing tools with the same names to avoid duplicates
            existing_tool_names = {t.name for t in tools}
            agent.tools = [t for t in agent.tools if t.name not in existing_tool_names]

            # Add the new tools
            agent.tools.extend(tools)

            # Persist the association
            # Get the agent's real name (not display name)
            agent_real_name = agent_identifier.lower()
            if not agent_identifier.isdigit():
                # It's already a name
                agent_real_name = agent_identifier.lower()
            else:
                # It's an index, get the actual agent name
                agents = get_available_agents()
                agent_list = list(agents.items())
                idx = int(agent_identifier)
                if 1 <= idx <= len(agent_list):
                    agent_real_name, _ = agent_list[idx - 1]

            add_mcp_server_to_agent(agent_real_name, server_name)

            merge_mcp_tools_into_session_agent(agent_real_name, tools)

            console.print(
                f"[green]Added {len(tools)} tools from server "
                f"'{server_name}' to agent '{agent_display_name}'.[/green]"
            )

            # Test that the tools are accessible
            async def test_agent_tools():
                # Get all tools including MCP tools
                all_regular_tools = agent.tools if hasattr(agent, "tools") else []
                all_mcp_tools = (
                    await agent.get_mcp_tools()
                    if hasattr(agent, "mcp_servers") and agent.mcp_servers
                    else []
                )
                return all_regular_tools + all_mcp_tools

            all_tools = self._run_async(test_agent_tools())

            # Count different types of tools
            mcp_server_tools_count = (
                len([t for t in agent.mcp_servers if hasattr(agent, "mcp_servers")])
                if hasattr(agent, "mcp_servers")
                else 0
            )
            regular_tools_count = len(agent.tools) if hasattr(agent, "tools") else 0

            console.print(
                f"[{_MCP_COL_MUTED}]Agent now has {regular_tools_count} tools total[/]"
            )

            # Test a simple tool invocation to make sure everything works
            console.print(f"[{_MCP_COL_MUTED}]Testing MCP tool connectivity...[/]")
            try:
                if tools:
                    console.print("[green]✓ MCP tools are ready for use![/green]")
                else:
                    console.print("[yellow]Warning: No tools available from server[/yellow]")
            except Exception as e:
                console.print(f"[yellow]Warning: Tool connectivity test failed: {e}[/yellow]")

            return True

        except Exception as e:
            _mcp_emit_panel(
                f"[red bold]Could not add MCP tools[/red bold]\n\n[white]{escape(str(e))}[/white]",
                title="MCP — add",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

    def handle_remove(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp remove command.

        Args:
            args: List of command arguments

        Returns:
            True if successful
        """
        if not args:
            _mcp_emit_panel(
                "[red bold]Missing server name[/red bold]\n\n"
                f"[bold {_CAI_GREEN}]/mcp remove <server_name>[/bold {_CAI_GREEN}]",
                title="MCP — remove",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server_name = args[0]

        if server_name not in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[red bold]Unknown server[/red bold] [white]{escape(server_name)}[/white]\n\n"
                f"[#9aa0a6]Loaded servers:[/] [bold {_CAI_GREEN}]/mcp list[/bold {_CAI_GREEN}]",
                title="MCP — remove",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        # Cleanup the server
        server = _GLOBAL_MCP_SERVERS[server_name]

        for agent_name in list(_AGENT_MCP_ASSOCIATIONS.keys()):
            if server_name in _AGENT_MCP_ASSOCIATIONS.get(agent_name, []):
                remove_mcp_server_from_agent(agent_name, server_name)

        strip_mcp_server_from_session_agents(server_name)
        unregister_mcp_tools_for_server(server_name)

        try:

            async def cleanup_server():
                await server.cleanup()

            self._run_async(cleanup_server())
            del _GLOBAL_MCP_SERVERS[server_name]
            _SERVER_INVOCATION_LOCKS.pop(server_name, None)
            console.print(f"[green]✓ Removed MCP server '{server_name}'[/green]")
            return True
        except Exception as e:
            _mcp_emit_panel(
                f"[red bold]Remove failed[/red bold]\n\n[white]{escape(str(e))}[/white]",
                title="MCP — remove",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            # Remove from list anyway
            if server_name in _GLOBAL_MCP_SERVERS:
                del _GLOBAL_MCP_SERVERS[server_name]
            _SERVER_INVOCATION_LOCKS.pop(server_name, None)
            return False

    def handle_status(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp status command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if successful
        """
        if not _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                "[#9aa0a6]No MCP servers are loaded — nothing to check.[/]\n\n"
                f"[bold {_CAI_GREEN}]/mcp load …[/bold {_CAI_GREEN}] [#9aa0a6]then re-run[/] "
                f"[bold {_CAI_GREEN}]/mcp status[/bold {_CAI_GREEN}]",
                title="MCP — status",
            )
            return True

        _mcp_emit_panel(
            f"[{_MCP_COL_MUTED}]Checking connections for {len(_GLOBAL_MCP_SERVERS)} server(s)…[/]",
            title="MCP — status",
            padding=(0, 1),
        )

        table = _mcp_table_embedded()
        table.add_column("Name", style=_MCP_TABLE_HEADER)
        table.add_column("Type", style=_MCP_COL_MUTED)
        table.add_column("Status", style=_MCP_COL_BODY)
        table.add_column("Tools", style=_MCP_COL_MUTED)
        table.add_column("Details", style="dim")

        healthy_count = 0

        for name, server in _GLOBAL_MCP_SERVERS.items():
            server_type = type(server).__name__.replace("MCPServer", "")

            # Test server connection
            try:

                async def test_connection():
                    tools = await server.list_tools()
                    return len(tools), None

                tools_count, error = self._run_async(test_connection())
                status = "[green]✓ Healthy[/green]"
                tools_str = str(tools_count)
                details = "Connection active"
                healthy_count += 1

            except Exception as e:
                status = "[red]✗ Error[/red]"
                tools_str = "N/A"
                details = f"Error: {str(e)[:50]}..."

                # Try to reconnect
                try:
                    console.print(
                        f"[{_MCP_PANEL_WARN_BORDER}]Reconnecting[/] "
                        f"[bold {_CAI_GREEN}]{escape(name)}[/bold {_CAI_GREEN}]"
                        f"[{_MCP_PANEL_WARN_BORDER}]…[/]"
                    )

                    async def reconnect():
                        await server.connect()
                        tools = await server.list_tools()
                        return len(tools)

                    tools_count = self._run_async(reconnect())
                    status = "[green]✓ Reconnected[/green]"
                    tools_str = str(tools_count)
                    details = "Reconnected successfully"
                    healthy_count += 1

                except Exception as reconnect_error:
                    status = "[red]✗ Failed[/red]"
                    details = f"Reconnect failed: {str(reconnect_error)[:30]}..."

            table.add_row(name, server_type, status, tools_str, details)

        _mcp_emit_panel_table(table, title="MCP server status")

        # Summary
        total_servers = len(_GLOBAL_MCP_SERVERS)
        if healthy_count == total_servers:
            _mcp_emit_panel(
                f"[green bold]All {total_servers} server(s) healthy[/green bold]",
                title="MCP — status",
                border_style=_CAI_GREEN,
                padding=(0, 1),
            )
        else:
            failed_count = total_servers - healthy_count
            _mcp_emit_panel(
                f"[{_MCP_COL_MUTED}]{healthy_count}/{total_servers} healthy[/]"
                f"[white]; {failed_count} need attention[/white]",
                title="MCP — status",
                border_style=_MCP_PANEL_WARN_BORDER,
                padding=(0, 1),
            )

        return True

    def handle_tools(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp tools command.

        Args:
            args: List of command arguments

        Returns:
            True if successful
        """
        if not args:
            _mcp_emit_panel(
                "[red bold]Missing server name[/red bold]\n\n"
                f"[bold {_CAI_GREEN}]/mcp tools <server_name>[/bold {_CAI_GREEN}]",
                title="MCP — tools",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server_name = args[0]

        if server_name not in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[red bold]Unknown server[/red bold] [white]{escape(server_name)}[/white]\n\n"
                f"[#9aa0a6]See[/] [bold {_CAI_GREEN}]/mcp list[/bold {_CAI_GREEN}]",
                title="MCP — tools",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server = _GLOBAL_MCP_SERVERS[server_name]

        try:

            async def get_tools():
                return await server.list_tools()

            tools = self._run_async(get_tools())

            if not tools:
                _mcp_emit_panel(
                    f"[{_MCP_COL_MUTED}]Server[/] [bold {_CAI_GREEN}]{escape(server_name)}[/bold {_CAI_GREEN}] "
                    f"[{_MCP_COL_MUTED}]returned no tools (empty catalog or handshake issue).[/]",
                    title="MCP — tools",
                    border_style=_MCP_PANEL_WARN_BORDER,
                )
                return True

            table = _mcp_table_embedded()
            table.add_column("#", style="dim")
            table.add_column("Name", style=_MCP_TABLE_HEADER)
            table.add_column("Description", style=_MCP_COL_BODY)

            for idx, tool in enumerate(tools, 1):
                description = tool.description or "No description"
                if len(description) > 60:
                    description = description[:57] + "..."
                table.add_row(str(idx), tool.name, description)

            _mcp_emit_panel_table(table, title=f"Tools — {escape(server_name)}")
            return True

        except Exception as e:
            _mcp_emit_panel(
                f"[red bold]Could not list tools[/red bold]\n\n[white]{escape(str(e))}[/white]",
                title="MCP — tools",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

    def handle_associations(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp associations command to show agent-MCP associations.

        Args:
            args: Optional list of command arguments (not used)

        Returns:
            True
        """
        if not _AGENT_MCP_ASSOCIATIONS:
            _mcp_emit_panel(
                "[#9aa0a6]No agent–MCP associations are recorded yet.[/]\n\n"
                f"[white]After[/white] [bold {_CAI_GREEN}]/mcp load …[/bold {_CAI_GREEN}][white], attach tools with[/white]\n"
                f"[bold {_CAI_GREEN}]/mcp add <server> <agent>[/bold {_CAI_GREEN}]",
                title="MCP — associations",
            )
            return True

        table = _mcp_table_embedded()
        table.add_column("Agent", style=_MCP_TABLE_HEADER)
        table.add_column("MCP Servers", style=_MCP_COL_MUTED)
        table.add_column("Total Tools", style=_MCP_COL_BODY)

        for agent_name, server_names in _AGENT_MCP_ASSOCIATIONS.items():
            if server_names:
                # Count total tools
                total_tools = 0
                for server_name in server_names:
                    if server_name in _GLOBAL_MCP_SERVERS:
                        try:

                            async def count_tools(srv):
                                tools = await srv.list_tools()
                                return len(tools)

                            server = _GLOBAL_MCP_SERVERS[server_name]
                            tool_count = self._run_async(count_tools(server))
                            total_tools += tool_count
                        except Exception:
                            pass

                servers_str = ", ".join(server_names)
                table.add_row(agent_name, servers_str, str(total_tools))

        _mcp_emit_panel_table(table, title="Agent–MCP associations")
        return True

    def handle_test(self, args: Optional[List[str]] = None) -> bool:
        """Handle /mcp test command to test server connectivity.

        Args:
            args: List of command arguments

        Returns:
            True if successful
        """
        if not args:
            _mcp_emit_panel(
                "[red bold]Missing server name[/red bold]\n\n"
                f"[bold {_CAI_GREEN}]/mcp test <server_name>[/bold {_CAI_GREEN}]",
                title="MCP — test",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server_name = args[0]

        if server_name not in _GLOBAL_MCP_SERVERS:
            _mcp_emit_panel(
                f"[red bold]Unknown server[/red bold] [white]{escape(server_name)}[/white]\n\n"
                f"[#9aa0a6]Loaded servers:[/] [bold {_CAI_GREEN}]/mcp list[/bold {_CAI_GREEN}]",
                title="MCP — test",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False

        server = _GLOBAL_MCP_SERVERS[server_name]

        _mcp_emit_panel(
            f"[{_MCP_COL_MUTED}]Running connectivity checks on[/] "
            f"[bold {_CAI_GREEN}]{escape(server_name)}[/bold {_CAI_GREEN}][{_MCP_COL_MUTED}]…[/]",
            title="MCP — test",
            padding=(0, 1),
        )

        try:

            async def test_server():
                # Test 1: List tools
                console.print(f"[{_MCP_COL_MUTED}]1. Listing tools…[/]")
                tools = await server.list_tools()
                console.print(
                    f"[green]✓[/] [{_MCP_COL_MUTED}]Found[/] [bold {_CAI_GREEN}]{len(tools)}[/bold {_CAI_GREEN}]"
                )

                # Test 2: Test a simple tool if available
                if tools:
                    test_tool = tools[0]
                    console.print(
                        f"[{_MCP_COL_MUTED}]2. Invoking sample tool[/] "
                        f"[bold {_CAI_GREEN}]{escape(test_tool.name)}[/bold {_CAI_GREEN}]"
                        f"[{_MCP_COL_MUTED}]…[/]"
                    )

                    # Create a test invocation
                    try:
                        # Use empty input for testing
                        result = await server.call_tool(test_tool.name, {})
                        console.print(f"[green]✓ Tool invocation successful[/green]")
                        if result and result.content:
                            console.print(
                                f"[dim]Result preview: {str(result.content[0])[:100]}...[/dim]"
                            )
                    except Exception as tool_error:
                        console.print(
                            f"[{_MCP_PANEL_WARN_BORDER}]⚠ Tool call skipped or failed[/] "
                            f"[{_MCP_COL_MUTED}](often normal if the tool needs input).[/]"
                        )
                        console.print(
                            f"[dim]{escape(str(tool_error)[:100])}[/dim]"
                        )

                # Test 3: Test reconnection
                console.print(f"[{_MCP_COL_MUTED}]3. Reconnecting transport…[/]")
                if hasattr(server, "session"):
                    old_session = server.session
                    server.session = None
                await server.connect()
                console.print("[green]✓ Reconnection successful[/green]")

                return True

            self._run_async(test_server())
            _mcp_emit_panel(
                f"[green bold]All checks passed[/green bold] [white]for[/white] "
                f"[bold {_CAI_GREEN}]{escape(server_name)}[/bold {_CAI_GREEN}]",
                title="MCP — test",
                border_style=_CAI_GREEN,
                padding=(0, 1),
            )
            return True

        except Exception as e:
            _mcp_emit_panel(
                f"[red bold]Test run failed[/red bold]\n\n"
                f"[white]{escape(type(e).__name__)}:[/white] {escape(str(e))}",
                title="MCP — test",
                border_style=_MCP_PANEL_ERROR_BORDER,
            )
            return False


def get_mcp_servers_for_agent(agent_name: str) -> List[str]:
    """Get list of MCP server names associated with an agent.

    Args:
        agent_name: Name of the agent

    Returns:
        List of MCP server names
    """
    return _AGENT_MCP_ASSOCIATIONS.get(agent_name.lower(), [])


def add_mcp_server_to_agent(agent_name: str, server_name: str):
    """Associate an MCP server with an agent.

    Args:
        agent_name: Name of the agent
        server_name: Name of the MCP server
    """
    agent_name_lower = agent_name.lower()
    if agent_name_lower not in _AGENT_MCP_ASSOCIATIONS:
        _AGENT_MCP_ASSOCIATIONS[agent_name_lower] = []

    if server_name not in _AGENT_MCP_ASSOCIATIONS[agent_name_lower]:
        _AGENT_MCP_ASSOCIATIONS[agent_name_lower].append(server_name)


def remove_mcp_server_from_agent(agent_name: str, server_name: str):
    """Remove an MCP server association from an agent.

    Args:
        agent_name: Name of the agent
        server_name: Name of the MCP server
    """
    agent_name_lower = agent_name.lower()
    if agent_name_lower in _AGENT_MCP_ASSOCIATIONS:
        if server_name in _AGENT_MCP_ASSOCIATIONS[agent_name_lower]:
            _AGENT_MCP_ASSOCIATIONS[agent_name_lower].remove(server_name)


def get_mcp_tools_for_agent(agent_name: str) -> List[FunctionTool]:
    """Get all MCP tools for an agent based on associations.

    Args:
        agent_name: Name of the agent

    Returns:
        List of FunctionTool objects
    """
    tools = []
    server_names = get_mcp_servers_for_agent(agent_name)

    for server_name in server_names:
        if server_name in _GLOBAL_MCP_SERVERS:
            server = _GLOBAL_MCP_SERVERS[server_name]
            try:
                # Get tools from server synchronously
                import asyncio

                async def get_tools():
                    return await server.list_tools()

                # Try to get existing loop or create new one
                try:
                    loop = asyncio.get_running_loop()
                    import concurrent.futures

                    def run_in_thread():
                        new_loop = asyncio.new_event_loop()
                        asyncio.set_event_loop(new_loop)
                        try:
                            return new_loop.run_until_complete(get_tools())
                        finally:
                            new_loop.close()

                    with concurrent.futures.ThreadPoolExecutor() as executor:
                        future = executor.submit(run_in_thread)
                        mcp_tools = future.result(timeout=10)
                except RuntimeError:
                    mcp_tools = asyncio.run(get_tools())

                # Convert to function tools
                for mcp_tool in mcp_tools:
                    function_tool = GlobalMCPUtil.to_function_tool(mcp_tool, server_name)
                    tools.append(function_tool)

            except Exception as e:
                logging.warning(f"Failed to get tools from MCP server '{server_name}': {e}")

    return tools


def export_parallel_mcp_bootstrap_dict() -> Optional[Dict[str, Any]]:
    """Serialize MCP servers and /mcp agent associations for external parallel workers.

    External workers are separate OS processes: they do not share ``_GLOBAL_MCP_SERVERS``
    or ``_AGENT_MCP_ASSOCIATIONS`` with the main CLI unless we pass this payload.
    """
    if not _GLOBAL_MCP_SERVERS:
        return None
    out_servers: List[Dict[str, Any]] = []
    for reg_name, srv in _GLOBAL_MCP_SERVERS.items():
        try:
            if isinstance(srv, MCPServerSse):
                p = dict(srv.params)
                hdr = p.get("headers")
                if isinstance(hdr, dict):
                    p["headers"] = {str(k): str(v) for k, v in hdr.items()}
                out_servers.append({"kind": "sse", "name": reg_name, "params": p})
            elif isinstance(srv, MCPServerStdio):
                sp = srv.params
                cmd = getattr(sp, "command", None) or ""
                args = list(getattr(sp, "args", None) or [])
                stdio: Dict[str, Any] = {"command": cmd, "args": args}
                env = getattr(sp, "env", None)
                if env:
                    stdio["env"] = {str(k): str(v) for k, v in dict(env).items()}
                cwd = getattr(sp, "cwd", None)
                if cwd is not None:
                    stdio["cwd"] = str(cwd)
                enc = getattr(sp, "encoding", None)
                if enc:
                    stdio["encoding"] = enc
                eh = getattr(sp, "encoding_error_handler", None)
                if eh:
                    stdio["encoding_error_handler"] = eh
                out_servers.append({"kind": "stdio", "name": reg_name, "params": stdio})
        except Exception as e:
            logging.warning(
                "Skipping MCP server %r from parallel bootstrap export: %s", reg_name, e
            )
    if not out_servers:
        return None
    associations = {k: list(v) for k, v in _AGENT_MCP_ASSOCIATIONS.items()}
    return {"servers": out_servers, "associations": associations}


async def apply_parallel_mcp_bootstrap_dict(data: Dict[str, Any]) -> None:
    """Restore MCP servers and associations inside an external parallel worker process."""
    servers_data = data.get("servers") or []
    for entry in servers_data:
        name = entry.get("name")
        kind = entry.get("kind")
        params = entry.get("params") or {}
        if not name or not kind:
            continue
        try:
            if kind == "sse":
                srv = MCPServerSse(
                    cast(MCPServerSseParams, params), name=name, cache_tools_list=True
                )
                await srv.connect()
                _GLOBAL_MCP_SERVERS[name] = srv
            elif kind == "stdio":
                stdio_params: MCPServerStdioParams = {
                    "command": params["command"],
                    "args": list(params.get("args") or []),
                }
                if params.get("env"):
                    stdio_params["env"] = dict(params["env"])
                if params.get("cwd"):
                    stdio_params["cwd"] = params["cwd"]
                if params.get("encoding"):
                    stdio_params["encoding"] = params["encoding"]
                if params.get("encoding_error_handler"):
                    stdio_params["encoding_error_handler"] = params["encoding_error_handler"]
                srv = MCPServerStdio(stdio_params, name=name, cache_tools_list=True)
                await srv.connect()
                _GLOBAL_MCP_SERVERS[name] = srv
        except Exception as e:
            logging.warning(
                "Parallel worker MCP bootstrap: could not attach server %r: %s", name, e
            )
    for agent_name, s_names in (data.get("associations") or {}).items():
        if not isinstance(s_names, list):
            continue
        for sname in s_names:
            if isinstance(sname, str):
                add_mcp_server_to_agent(agent_name, sname)


async def apply_parallel_mcp_bootstrap_file(path: str) -> None:
    import json
    from pathlib import Path

    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        return
    await apply_parallel_mcp_bootstrap_dict(data)


# Register the command
register_command(MCPCommand())
