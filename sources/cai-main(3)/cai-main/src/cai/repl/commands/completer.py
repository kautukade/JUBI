"""
Command completer for CAI REPL.
This module provides a fuzzy command completer with autocompletion menu and
command shadowing.
"""
# Standard library imports
import datetime
import os
import threading
import time
from functools import lru_cache
from html import escape as html_escape
from typing import (
    Collection,
    Dict,
    List,
    Optional,
    Set,
)

# Third-party imports
import requests  # pylint: disable=import-error,unused-import,line-too-long # noqa: E501
from prompt_toolkit.completion import (  # pylint: disable=import-error
    Completer,
    Completion
)
from prompt_toolkit.formatted_text import HTML  # pylint: disable=import-error
from prompt_toolkit.styles import Style  # pylint: disable=import-error
from rich.console import Console  # pylint: disable=import-error

from cai.util import get_ollama_api_base
from cai.repl.commands.base import (
    COMMANDS,
    COMMAND_ALIASES
)
from cai.repl.commands.model import get_predefined_model_names

console = Console()

# Global cache for command descriptions and subcommands
COMMAND_DESCRIPTIONS_CACHE = None
SUBCOMMAND_DESCRIPTIONS_CACHE = None
ALL_COMMANDS_CACHE = None


class FuzzyCommandCompleter(Completer):
    """Command completer with fuzzy matching for the REPL.

    This advanced completer provides intelligent suggestions for commands,
    subcommands, and arguments based on what the user is typing.
    It supports fuzzy matching to find commands even with typos.

    Features:
    - Fuzzy matching for commands and subcommands
    - Autocompletion menu with descriptions
    - Command shadowing (showing hints for previously used commands)
    - Model completion for the /model command
    - Agent completion for ``/agent select|info`` (third word); second word uses registered subcommands
    """

    # Class-level cache for models
    _cached_models = []
    _cached_model_numbers = {}
    _last_model_fetch = datetime.datetime.now() - datetime.timedelta(minutes=10)
    _fetch_lock = threading.Lock()

    # Class-level cache for agents (with proper threading and time-based caching)
    _cached_agents = []
    _cached_agent_numbers = {}
    _last_agent_fetch = datetime.datetime.now() - datetime.timedelta(minutes=10)
    _agent_fetch_lock = threading.Lock()

    # Class-level cache for ALL agents including patterns (for /parallel add)
    _cached_all_agents = []
    _last_all_agent_fetch = datetime.datetime.now() - datetime.timedelta(minutes=10)
    _all_agent_fetch_lock = threading.Lock()
    
    def __init__(self):
        """Initialize the command completer with cached model and agent information."""
        super().__init__()
        self.command_history = {}  # Store command usage frequency
        
        # Fetch models in background thread to avoid blocking
        threading.Thread(
            target=self._background_fetch_models,
            daemon=True
        ).start()

        # Fetch agents in background thread to avoid blocking
        threading.Thread(
            target=self._background_fetch_agents,
            daemon=True
        ).start()

        # Styling for the completion menu
        self.completion_style = Style.from_dict({
            'completion-menu': 'bg:#0f1b16 #e8efe9',
            'completion-menu.completion': 'bg:#0f1b16 #e8efe9',
            'completion-menu.completion.current': 'bg:#123526 #00ff9d bold',
            'scrollbar.background': 'bg:#0f1b16',
            'scrollbar.button': 'bg:#1f5a43',
        })

        # /virtualization set|run argument completion (Docker-backed, short TTL)
        self._virt_comp_lock = threading.Lock()
        self._virt_comp_last = 0.0
        self._virt_comp_containers: List[str] = []
        self._virt_comp_images: List[str] = []
    
    def _background_fetch_agents(self):
        """Fetch agents in background to avoid blocking the UI."""
        try:
            self.fetch_all_agents()
        except Exception:  # pylint: disable=broad-except
            # Silently fail if agent fetching is not available
            pass

    def fetch_all_agents(self):
        """Fetch all available agents to match /agent command."""
        # Only fetch every 60 seconds to avoid excessive calls
        now = datetime.datetime.now()
        
        # Use a lock to prevent multiple threads from fetching simultaneously
        with self._agent_fetch_lock:
            if (now - self._last_agent_fetch).total_seconds() < 60:
                return
            
            self._last_agent_fetch = now
            
            try:
                from cai.agents import get_available_agents
                
                # Get agents and filter out parallel patterns (like /agent list does)
                all_agents = get_available_agents()
                regular_agents = []
                
                for agent_key, agent in all_agents.items():
                    # Skip parallel patterns in completion (matches /agent list behavior)
                    if hasattr(agent, "_pattern"):
                        pattern = agent._pattern
                        if hasattr(pattern, "type"):
                            pattern_type_value = getattr(pattern.type, 'value', str(pattern.type))
                            if pattern_type_value == "parallel":
                                continue
                    regular_agents.append(agent_key)
                
                self._cached_agents = regular_agents
                
                # Create number mappings (1-based indexing)
                self._cached_agent_numbers = {}
                for i, agent_key in enumerate(self._cached_agents, 1):
                    self._cached_agent_numbers[str(i)] = agent_key
                    
            except Exception:  # pylint: disable=broad-except
                # Silently fail if agent fetching is not available
                pass
    
    def fetch_all_agents_with_patterns(self):
        """Fetch all available agents including patterns (for /parallel add)."""
        now = datetime.datetime.now()

        with self._all_agent_fetch_lock:
            if (now - self._last_all_agent_fetch).total_seconds() < 60:
                return

            self._last_all_agent_fetch = now

            try:
                from cai.agents import get_available_agents

                self._cached_all_agents = list(get_available_agents().keys())
            except Exception:  # pylint: disable=broad-except
                pass

    def _background_fetch_models(self):
        """Fetch models in background to avoid blocking the UI."""
        try:
            self.fetch_all_models()
        except Exception:  # pylint: disable=broad-except
            pass

    def fetch_all_models(self):  # pylint: disable=too-many-branches,too-many-statements,inconsistent-return-statements,line-too-long # noqa: E501
        """Fetch all available models (predefined + LiteLLM + Ollama) to match /model command."""
        # Only fetch every 60 seconds to avoid excessive API calls
        now = datetime.datetime.now()
        
        # Use a lock to prevent multiple threads from fetching simultaneously
        with self._fetch_lock:
            if (now - self._last_model_fetch).total_seconds() < 60:
                return
            
            self._last_model_fetch = now
            
            # Start with predefined models from the shared source of truth
            all_models = get_predefined_model_names()

            # Fetch LiteLLM models (matches the /model command behavior)
            try:
                litellm_url = (
                    "https://raw.githubusercontent.com/BerriAI/litellm/main/"
                    "model_prices_and_context_window.json"
                )
                response = requests.get(litellm_url, timeout=2)
                
                if response.status_code == 200:
                    litellm_data = response.json()
                    # Add LiteLLM models (sorted for consistency)
                    litellm_models = sorted(litellm_data.keys())
                    all_models.extend(litellm_models)
            except Exception:  # pylint: disable=broad-except
                # Silently fail if LiteLLM is not available
                pass

            # Fetch Ollama models
            try:
                # Get Ollama models with a short timeout to prevent hanging
                api_base = get_ollama_api_base()
                
                # Add authentication headers for Ollama Cloud if using OPENAI_BASE_URL
                headers = {}
                if "ollama.com" in api_base:
                    api_key = os.getenv("OPENAI_API_KEY")
                    if api_key:
                        headers["Authorization"] = f"Bearer {api_key}"
                
                response = requests.get(
                    f"{api_base.replace('/v1', '')}/api/tags",
                    headers=headers,
                    timeout=0.5)

                if response.status_code == 200:
                    data = response.json()
                    if 'models' in data:
                        models = data['models']
                    else:
                        # Fallback for older Ollama versions
                        models = data.get('items', [])

                    ollama_models = [model.get('name', '') for model in models]
                    all_models.extend(ollama_models)
            except Exception:  # pylint: disable=broad-except
                # Silently fail if Ollama is not available
                pass

            # Cache all models that the /model command can handle
            self._cached_models = all_models

            # Create number mappings for models (1-based indexing)
            # This matches the /model command numbering exactly
            self._cached_model_numbers = {}
            for i, model in enumerate(self._cached_models, 1):
                self._cached_model_numbers[str(i)] = model

    def record_command_usage(self, command: str):
        """Record command usage for command shadowing.

        Args:
            command: The command that was used
        """
        if command.startswith('/'):
            # Extract the main command
            parts = command.split()
            main_command = parts[0]

            # Update usage count
            if main_command in self.command_history:
                self.command_history[main_command] += 1
            else:
                self.command_history[main_command] = 1

    def get_command_descriptions(self):
        """Get descriptions for all commands.

        Returns:
            A dictionary mapping command names to descriptions
        """
        global COMMAND_DESCRIPTIONS_CACHE
        if COMMAND_DESCRIPTIONS_CACHE is None:
            from cai.repl.commands import _ensure_all_commands_loaded
            _ensure_all_commands_loaded()
            COMMAND_DESCRIPTIONS_CACHE = {cmd.name: cmd.description for cmd in COMMANDS.values()}
        return COMMAND_DESCRIPTIONS_CACHE

    def get_subcommand_descriptions(self):
        """Get descriptions for all subcommands.

        Returns:
            A dictionary mapping command paths to descriptions
        """
        global SUBCOMMAND_DESCRIPTIONS_CACHE
        if SUBCOMMAND_DESCRIPTIONS_CACHE is None:
            from cai.repl.commands import _ensure_all_commands_loaded
            _ensure_all_commands_loaded()
            descriptions = {}
            for cmd in COMMANDS.values():
                for subcmd in cmd.get_subcommands():
                    key = f"{cmd.name} {subcmd}"
                    descriptions[key] = cmd.get_subcommand_description(subcmd)
            SUBCOMMAND_DESCRIPTIONS_CACHE = descriptions
        return SUBCOMMAND_DESCRIPTIONS_CACHE

    def get_all_commands(self):
        """Get all commands and their subcommands.

        Returns:
            A dictionary mapping command names to lists of subcommand names
        """
        global ALL_COMMANDS_CACHE
        if ALL_COMMANDS_CACHE is None:
            from cai.repl.commands import _ensure_all_commands_loaded
            _ensure_all_commands_loaded()
            ALL_COMMANDS_CACHE = {cmd.name: cmd.get_subcommands() for cmd in COMMANDS.values()}
        return ALL_COMMANDS_CACHE

    # Cache for command suggestions to avoid recalculating
    _command_suggestions_cache = {}
    _command_suggestions_last_update = 0
    _command_suggestions_update_interval = 1.0  # Update every second

    def get_command_suggestions(self, current_word: str) -> List[Completion]:
        """Get command suggestions with fuzzy matching.

        Args:
            current_word: The current word being typed

        Returns:
            A list of completions for commands
        """
        # Check cache first
        current_time = time.time()
        cache_key = current_word
        
        if (cache_key in self._command_suggestions_cache and
                current_time - self._command_suggestions_last_update < 
                self._command_suggestions_update_interval):
            return self._command_suggestions_cache[cache_key]
        
        suggestions = []

        # Get command descriptions
        command_descriptions = self.get_command_descriptions()

        # Sort commands by usage frequency (for command shadowing)
        sorted_commands = sorted(
            command_descriptions.items(),
            key=lambda x: self.command_history.get(x[0], 0),
            reverse=True
        )

        # Add command completions
        for cmd, description in sorted_commands:
            safe_desc = html_escape(description)
            # Exact prefix match
            if cmd.startswith(current_word):
                suggestions.append(Completion(
                    cmd,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansicyan><b>{cmd:<15}</b></ansicyan> "
                        f"{safe_desc}"),
                    style="fg:ansicyan bold"
                ))
            # Fuzzy match (contains the substring)
            elif current_word in cmd and not cmd.startswith(current_word):
                suggestions.append(Completion(
                    cmd,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansicyan>{cmd:<15}</ansicyan> {safe_desc}"),
                    style="fg:ansicyan"
                ))

        # Add alias completions
        for alias, cmd in sorted(COMMAND_ALIASES.items()):
            cmd_description = html_escape(command_descriptions.get(cmd, ""))
            if alias.startswith(current_word):
                suggestions.append(Completion(
                    alias,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansigreen><b>{alias:<15}</b></ansigreen> "
                        f"{cmd} - {cmd_description}"),
                    style="fg:ansigreen bold"
                ))
            elif current_word in alias and not alias.startswith(current_word):
                suggestions.append(Completion(
                    alias,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansigreen>{alias:<15}</ansigreen> "
                        f"{cmd} - {cmd_description}"),
                    style="fg:ansigreen"
                ))
        
        # Update cache
        self._command_suggestions_cache[cache_key] = suggestions
        self._command_suggestions_last_update = current_time
        
        return suggestions

    # Cache for command shadow
    _command_shadow_cache = {}
    _command_shadow_last_update = 0
    _command_shadow_update_interval = 0.2  # Update every 200ms

    @lru_cache(maxsize=100)
    def _get_command_shadow_cached(self, text: str) -> Optional[str]:
        """Cached version of command shadow lookup."""
        if not text or not text.startswith('/'):
            return None

        # Find commands that start with the current input
        matching_commands = []
        for cmd, count in self.command_history.items():
            if cmd.startswith(text) and cmd != text:
                matching_commands.append((cmd, count))

        # Sort by usage count (descending)
        matching_commands.sort(key=lambda x: x[1], reverse=True)

        # Return the most frequently used command
        if matching_commands:
            return matching_commands[0][0]

        return None

    def get_command_shadow(self, text: str) -> Optional[str]:
        """Get a command shadow suggestion based on command history.

        This method returns a suggestion for command shadowing based on
        the current input and command usage history.

        Args:
            text: The current input text

        Returns:
            A suggested command completion or None if no suggestion
        """
        # Check cache first
        current_time = time.time()
        
        if (text in self._command_shadow_cache and
                current_time - self._command_shadow_last_update < 
                self._command_shadow_update_interval):
            return self._command_shadow_cache[text]
        
        # Get shadow from cached function
        result = self._get_command_shadow_cached(text)
        
        # Update cache
        self._command_shadow_cache[text] = result
        self._command_shadow_last_update = current_time
        
        return result

    # Cache for subcommand suggestions
    _subcommand_suggestions_cache = {}
    _subcommand_suggestions_last_update = 0
    _subcommand_suggestions_update_interval = 1.0  # Update every second
    
    def get_subcommand_suggestions(
            self, cmd: str, current_word: str) -> List[Completion]:
        """Get subcommand suggestions with fuzzy matching.

        Args:
            cmd: The main command
            current_word: The current word being typed

        Returns:
            A list of completions for subcommands
        """
        # Check cache first
        current_time = time.time()
        cache_key = f"{cmd}:{current_word}"
        
        if (cache_key in self._subcommand_suggestions_cache and
                current_time - self._subcommand_suggestions_last_update < 
                self._subcommand_suggestions_update_interval):
            return self._subcommand_suggestions_cache[cache_key]
            
        suggestions = []

        # If using an alias, get the real command
        cmd = COMMAND_ALIASES.get(cmd, cmd)

        all_commands = self.get_all_commands()
        subcommand_descriptions = self.get_subcommand_descriptions()

        if cmd in all_commands:
            for subcmd in sorted(all_commands[cmd]):
                # Get description for this subcommand if available
                subcmd_description = html_escape(
                    subcommand_descriptions.get(f"{cmd} {subcmd}", ""))

                # Exact prefix match
                if subcmd.startswith(current_word):
                    suggestions.append(Completion(
                        subcmd,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansiyellow><b>{subcmd:<15}</b></ansiyellow> "
                            f"{subcmd_description}"),
                        style="fg:ansiyellow bold"
                    ))
                # Fuzzy match
                elif (current_word in subcmd and
                      not subcmd.startswith(current_word)):
                    suggestions.append(Completion(
                        subcmd,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansiyellow>{subcmd:<15}</ansiyellow> "
                            f"{subcmd_description}"),
                        style="fg:ansiyellow"
                    ))
        
        # Update cache
        self._subcommand_suggestions_cache[cache_key] = suggestions
        self._subcommand_suggestions_last_update = current_time
        
        return suggestions

    def _resume_arg_completions(self, current_word: str):
        """First argument after ``/resume`` / ``/r`` (subcommand, paths, session id prefixes)."""
        cw = (current_word or "").lower()
        pos = -len(current_word or "")

        # Subcommand (same accent family as other /command <subcmd> rows: yellow)
        if not cw or "last".startswith(cw):
            yield Completion(
                "last",
                start_position=pos,
                display=HTML(
                    "<ansiyellow><b>last</b></ansiyellow> "
                    "<ansiwhite>Newest JSONL with messages under ./logs</ansiwhite> "
                    "<span style='color:#6b7c74'>(subcommand)</span>"
                ),
                style="fg:ansiyellow bold",
            )

        # Path-like arguments (cyan — distinct from subcommand and id tokens)
        for label, desc in (
            ("logs/", "Logs directory"),
            ("logs/last", "Symlink to last capture"),
        ):
            if cw and not label.lower().startswith(cw):
                continue
            if not cw or label.lower().startswith(cw):
                yield Completion(
                    label,
                    start_position=pos,
                    display=HTML(
                        f"<ansicyan><b>{html_escape(label)}</b></ansicyan> "
                        f"<ansiwhite>{html_escape(desc)}</ansiwhite> "
                        "<span style='color:#6b7c74'>(path)</span>"
                    ),
                    style="fg:ansicyan bold",
                )

        try:
            from cai.repl.session_resume import (
                DEFAULT_RECENT_SESSION_COUNT,
                list_recent_sessions,
            )

            for sess in list_recent_sessions(DEFAULT_RECENT_SESSION_COUNT):
                sid = (sess.get("session_id") or "")[:8]
                if not sid:
                    continue
                fn = html_escape(str(sess.get("file_name") or ""))
                if cw and not sid.lower().startswith(cw):
                    continue
                yield Completion(
                    sid,
                    start_position=pos,
                    display=HTML(
                        f"<ansimagenta><b>{html_escape(sid)}</b></ansimagenta> "
                        f"<ansiwhite>{fn}</ansiwhite> "
                        "<span style='color:#6b7c74'>(session id)</span>"
                    ),
                    style="fg:ansimagenta bold",
                )
        except Exception:  # pylint: disable=broad-except
            pass

    def _resume_dir_token_completions(self, dir_arg: str, current_word: str):
        """Third token after ``/resume <dir>``: values for ``find_jsonl_by_token_in_dir``."""
        from pathlib import Path

        if (dir_arg or "").strip().lower() == "last":
            return
        pos = -len(current_word or "")
        cw = (current_word or "").lower()
        p = Path(dir_arg).expanduser()
        if not p.is_dir():
            return
        try:
            files = [f for f in p.rglob("*.jsonl") if f.is_file()]
        except OSError:
            return
        if not files:
            return
        files.sort(key=lambda fp: fp.stat().st_mtime, reverse=True)
        seen: Set[str] = set()
        cap = 80
        for f in files:
            name = f.name
            nl = name.lower()
            if cw and cw not in nl and not nl.startswith(cw):
                continue
            if name in seen:
                continue
            seen.add(name)
            yield Completion(
                name,
                start_position=pos,
                display=HTML(
                    f"<ansigreen><b>{html_escape(name)}</b></ansigreen> "
                    "<ansiwhite>Substring in filename → newest match</ansiwhite> "
                    "<span style='color:#6b7c74'>(token)</span>"
                ),
                style="fg:ansigreen bold",
            )
            if len(seen) >= cap:
                break

    def _sessions_arg_completions(self, current_word: str):
        """First argument after ``/sessions`` / ``/sess`` (count or id prefix)."""
        cw = (current_word or "").lower()
        for label, desc in (
            ("10", "Show last 10 sessions"),
            ("20", "Show last 20 sessions"),
            ("50", "Show last 50 sessions"),
        ):
            if cw and not label.startswith(cw):
                continue
            if not cw or label.startswith(cw):
                yield Completion(
                    label,
                    start_position=-len(current_word or ""),
                    display=HTML(
                        f"<span style='color:#00ff9d'><b>{html_escape(label)}</b></span> "
                        f"<span style='color:#9aa0a6'>{html_escape(desc)}</span>"
                    ),
                )

    def _model_show_filter_completions(self, current_word: str) -> List[Completion]:
        """Suggest ``supported`` as the next token after ``/model show``."""
        cw = (current_word or "").lower()
        out: List[Completion] = []
        if not cw or "supported".startswith(cw):
            out.append(
                Completion(
                    "supported",
                    start_position=-len(current_word),
                    display=HTML(
                        "<ansiyellow><b>supported</b></ansiyellow> "
                        "function-calling models only"
                    ),
                    style="fg:ansiyellow bold",
                )
            )
        return out

    def get_model_suggestions(self, current_word: str) -> List[Completion]:
        """Get model suggestions for the /model command.

        Args:
            current_word: The current word being typed

        Returns:
            A list of completions for models
        """
        suggestions = []
        cw = (current_word or "").lower()
        if not cw or "show".startswith(cw):
            suggestions.append(
                Completion(
                    "show",
                    start_position=-len(current_word),
                    display=HTML("<ansiyellow><b>show</b></ansiyellow> full catalog"),
                    style="fg:ansiyellow bold",
                )
            )

        # First try to complete model numbers
        for num, model_name in self._cached_model_numbers.items():
            if num.startswith(current_word):
                suggestions.append(Completion(
                    num,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansiwhite><b>{num:<3}</b></ansiwhite> "
                        f"{model_name}"),
                    style="fg:ansiwhite bold"
                ))

        # Then try to complete model names
        for model in self._cached_models:
            model_name = model[0] if isinstance(model, tuple) else model            
            if model_name.startswith(current_word):
                suggestions.append(Completion(
                    model_name,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansimagenta><b>{model_name}</b></ansimagenta>"),
                    style="fg:ansimagenta bold"
                ))
            elif (current_word.lower() in model_name.lower() and
                  not model_name.startswith(current_word)):
                suggestions.append(Completion(
                    model_name,
                    start_position=-len(current_word),
                    display=HTML(f"<ansimagenta>{model_name}</ansimagenta>"),
                    style="fg:ansimagenta"
                ))

        return suggestions

    def get_agent_suggestions(self, current_word: str) -> List[Completion]:
        """Get agent suggestions for the /agent command."""
        suggestions = []

        # Refresh agents if needed (non-blocking due to time-based caching)
        self.fetch_all_agents()

        # First try to complete agent numbers
        for num, agent_name in self._cached_agent_numbers.items():
            if num.startswith(current_word):
                # Get agent display name for better UX
                try:
                    from cai.agents import get_available_agents
                    agents = get_available_agents()
                    agent_obj = agents.get(agent_name)
                    display_name = getattr(agent_obj, "name", agent_name) if agent_obj else agent_name
                except (ImportError, AttributeError, KeyError):  # pylint: disable=broad-except
                    display_name = agent_name
                    
                safe_display = html_escape(str(display_name))
                suggestions.append(Completion(
                    num,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansiwhite><b>{num:<3}</b></ansiwhite> "
                        f"{safe_display}"),
                    style="fg:ansiwhite bold"
                ))

        # Then try to complete agent names
        for agent_key in self._cached_agents:
            safe_key = html_escape(str(agent_key))
            if agent_key.startswith(current_word):
                suggestions.append(Completion(
                    agent_key,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansimagenta><b>{safe_key}</b></ansimagenta>"),
                    style="fg:ansimagenta bold"
                ))
            elif (current_word.lower() in agent_key.lower() and
                  not agent_key.startswith(current_word)):
                suggestions.append(Completion(
                    agent_key,
                    start_position=-len(current_word),
                    display=HTML(f"<ansimagenta>{safe_key}</ansimagenta>"),
                    style="fg:ansimagenta"
                ))

        return suggestions

    def get_flush_agent_nonempty_suggestions(
        self,
        current_word: str,
        *,
        exclude_labels: Optional[Collection[str]] = None,
    ) -> List[Completion]:
        """``/flush agent`` / ``/clear agent``: session agents with non-empty history (REPL).

        One suggestion per target: ``DisplayName [Pn]`` only (no bare ``Pn`` duplicates).

        ``exclude_labels``: full labels to omit (e.g. agents already picked for ``/merge``).
        """
        if os.getenv("CAI_TUI_MODE") == "true":
            return []

        suggestions: List[Completion] = []
        try:
            from cai.repl.commands.flush import ordered_nonempty_flush_agent_labels_repl

            labels = list(ordered_nonempty_flush_agent_labels_repl())
        except (ImportError, AttributeError):
            return suggestions

        if exclude_labels:
            banned = frozenset(exclude_labels)
            labels = [lb for lb in labels if lb not in banned]

        cw = current_word
        for label in labels:
            if label.startswith(cw):
                suggestions.append(
                    Completion(
                        label,
                        start_position=-len(cw),
                        display=HTML(f"<ansimagenta><b>{html_escape(label)}</b></ansimagenta>"),
                        style="fg:ansimagenta bold",
                    )
                )
            elif cw.lower() in label.lower() and not label.startswith(cw):
                suggestions.append(
                    Completion(
                        label,
                        start_position=-len(cw),
                        display=HTML(f"<ansimagenta>{html_escape(label)}</ansimagenta>"),
                        style="fg:ansimagenta",
                    )
                )

        return suggestions

    _MERGE_STRATEGY_VALUES = frozenset({"chronological", "by-agent", "interleaved"})

    @staticmethod
    def _merge_flag_split(tokens: List[str]) -> List[str]:
        pos: List[str] = []
        for t in tokens:
            if t.startswith("--"):
                break
            pos.append(t)
        return pos

    @staticmethod
    def _merge_label_slot_id(label: str) -> Optional[str]:
        if "[" not in label or not label.endswith("]"):
            return None
        return label.rsplit("[", 1)[-1].rstrip("]")

    def _merge_committed_positionals(
        self, words: List[str], merge_start: int, has_trailing_space: bool
    ) -> List[str]:
        """Tokens already chosen for merge slots (excludes the word being typed)."""
        raw = self._merge_flag_split(words[merge_start:])
        if not raw:
            return []
        if has_trailing_space:
            return raw
        if len(raw) == 1:
            return []
        return raw[:-1]

    def _merge_excluded_labels_from_committed(
        self, committed: List[str], labels: List[str]
    ) -> Set[str]:
        """Labels to hide: already selected by slot id (P1), full label, or multi-word slice."""
        excluded: Set[str] = set()
        if not committed:
            return excluded
        n = len(committed)
        for lbl in labels:
            for i in range(n):
                for j in range(i + 1, n + 1):
                    if " ".join(committed[i:j]) == lbl:
                        excluded.add(lbl)
                        break
                else:
                    continue
                break
            if lbl in excluded:
                continue
            slot = self._merge_label_slot_id(lbl)
            if not slot:
                continue
            for tok in committed:
                if tok.strip().upper() == slot.upper():
                    excluded.add(lbl)
                    break
        return excluded

    def _merge_strategy_blocks_flush_agent_suggestions(self, words: List[str]) -> bool:
        if "--strategy" not in words:
            return False
        i = words.index("--strategy")
        if i + 1 >= len(words):
            return True
        return words[i + 1] not in self._MERGE_STRATEGY_VALUES

    def _yield_merge_agent_arg_completions(
        self,
        words: List[str],
        current_word: str,
        *,
        parallel_merge: bool,
        has_trailing_space: bool,
    ):
        """Merge / parallel merge agent args: same pool as flush, minus already-picked agents."""
        if current_word.startswith("--"):
            return
        if parallel_merge:
            if len(words) < 2 or words[1] != "merge":
                return
            merge_start = 2
        else:
            merge_start = 1
        if self._merge_strategy_blocks_flush_agent_suggestions(words):
            return
        try:
            from cai.repl.commands.flush import ordered_nonempty_flush_agent_labels_repl

            all_labels = list(ordered_nonempty_flush_agent_labels_repl())
        except (ImportError, AttributeError):
            all_labels = []
        committed = self._merge_committed_positionals(
            words, merge_start, has_trailing_space
        )
        excluded = self._merge_excluded_labels_from_committed(committed, all_labels)
        yield from self.get_flush_agent_nonempty_suggestions(
            current_word, exclude_labels=excluded
        )

    def get_env_catalog_target_suggestions(self, current_word: str) -> List[Completion]:
        """Complete catalog # or variable name for ``/env get`` / ``/env set`` / ``/help var``."""
        suggestions: List[Completion] = []
        try:
            from cai.repl.commands.env_catalog import ENV_VARS
        except (ImportError, AttributeError):
            return suggestions

        cw = (current_word or "").strip()
        cw_lower = cw.lower()

        for num, var_info in ENV_VARS.items():
            num_s = str(num)
            var_name = str(var_info.get("name", ""))
            safe_name = html_escape(var_name)

            if not cw:
                suggestions.append(
                    Completion(
                        var_name,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansigreen><b>{safe_name}</b></ansigreen> "
                            f"<ansiwhite>({num_s})</ansiwhite>"
                        ),
                        style="fg:ansigreen bold",
                    )
                )
                continue

            if cw.isdigit():
                if num_s.startswith(cw):
                    suggestions.append(
                        Completion(
                            num_s,
                            start_position=-len(current_word),
                            display=HTML(
                                f"<ansiwhite><b>{num_s:<4}</b></ansiwhite> {safe_name}"
                            ),
                            style="fg:ansiwhite bold",
                        )
                    )
                continue

            if cw_lower in var_name.lower() or var_name.upper().startswith(cw.upper()):
                suggestions.append(
                    Completion(
                        var_name,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansigreen><b>{safe_name}</b></ansigreen> "
                            f"<ansiwhite>({num_s})</ansiwhite>"
                        ),
                        style="fg:ansigreen bold",
                    )
                )

        return suggestions

    @staticmethod
    def _resolved_env_set_target_is_model(words: List[str]) -> bool:
        """True if ``/env set <spec>`` resolves to a model-type catalog variable."""
        if len(words) < 3:
            return False
        try:
            from cai.repl.commands.env_catalog import ENV_VARS
            from cai.repl.commands.env_catalog_validate import (
                is_model_catalog_var,
                resolve_catalog_spec,
            )
        except (ImportError, AttributeError):
            return False
        spec = words[2].strip()
        if not spec:
            return False
        r = resolve_catalog_spec(spec, ENV_VARS)
        if not r:
            return False
        _n, _info, var_name = r
        return is_model_catalog_var(var_name)

    @staticmethod
    def _resolved_env_set_target_is_ctf_name(words: List[str]) -> bool:
        """True if ``/env set <spec>`` resolves to ``CTF_NAME``."""
        if len(words) < 3:
            return False
        try:
            from cai.repl.commands.env_catalog import ENV_VARS
            from cai.repl.commands.env_catalog_validate import resolve_catalog_spec
        except (ImportError, AttributeError):
            return False
        spec = words[2].strip()
        if not spec:
            return False
        r = resolve_catalog_spec(spec, ENV_VARS)
        if not r:
            return False
        _n, _info, var_name = r
        return var_name == "CTF_NAME"

    def get_ctf_name_suggestions(self, current_word: str) -> List[Completion]:
        """Suggest CAIBench CTF ids for ``/env set CTF_NAME …`` (same pool as validation)."""
        from cai.repl.commands.env_catalog_validate import get_caibench_ctf_names_for_completion_cached

        suggestions: List[Completion] = []
        names = list(get_caibench_ctf_names_for_completion_cached())
        if not names:
            return suggestions

        cw = current_word or ""
        cw_stripped = cw.strip()
        cw_lower = cw_stripped.lower()
        # Thousands of CTFs: cap when the user has not typed a filter yet.
        max_blank = 400

        shown = 0
        for n in names:
            safe = html_escape(n)
            if not cw_lower:
                if shown >= max_blank:
                    break
                suggestions.append(
                    Completion(
                        n,
                        start_position=-len(current_word),
                        display=HTML(f"<ansicyan><b>{safe}</b></ansicyan>"),
                        style="fg:ansicyan bold",
                    )
                )
                shown += 1
                continue
            if n.startswith(cw_stripped):
                suggestions.append(
                    Completion(
                        n,
                        start_position=-len(current_word),
                        display=HTML(f"<ansicyan><b>{safe}</b></ansicyan>"),
                        style="fg:ansicyan bold",
                    )
                )
            elif cw_lower in n.lower():
                suggestions.append(
                    Completion(
                        n,
                        start_position=-len(current_word),
                        display=HTML(f"<ansicyan>{safe}</ansicyan>"),
                        style="fg:ansicyan",
                    )
                )
        return suggestions

    def get_all_agent_suggestions(self, current_word: str) -> list[Completion]:
        """Get all agent suggestions including patterns (for /parallel add).

        Unlike get_agent_suggestions(), this does NOT filter out parallel
        patterns and does NOT offer numeric shortcuts, since /parallel add
        expects a plain agent key name.
        """
        suggestions: list[Completion] = []

        self.fetch_all_agents_with_patterns()

        for agent_key in self._cached_all_agents:
            if agent_key.startswith(current_word):
                suggestions.append(Completion(
                    agent_key,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansimagenta><b>{agent_key}</b></ansimagenta>"),
                    style="fg:ansimagenta bold",
                ))
            elif (current_word.lower() in agent_key.lower()
                  and not agent_key.startswith(current_word)):
                suggestions.append(Completion(
                    agent_key,
                    start_position=-len(current_word),
                    display=HTML(
                        f"<ansimagenta>{agent_key}</ansimagenta>"),
                    style="fg:ansimagenta",
                ))

        return suggestions

    def get_parallel_config_suggestions(self, current_word: str) -> list[Completion]:
        """Get suggestions for /parallel remove from currently configured agents.

        Shows each configured agent with its ID (P1, P2...) and numeric index
        so that duplicate agent names are distinguishable.
        """
        suggestions: list[Completion] = []

        try:
            from cai.repl.commands._parallel_monolith import PARALLEL_CONFIGS

            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                pid = config.id or f"P{idx}"
                model_label = config.model or "default"
                display_text = f"{config.agent_name} ({model_label})"

                # Offer the ID (e.g. "P1") as a completion
                if pid.lower().startswith(current_word.lower()):
                    suggestions.append(Completion(
                        pid,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansiwhite><b>{pid:<4}</b></ansiwhite> "
                            f"<ansimagenta>{html_escape(display_text)}"
                            f"</ansimagenta>"),
                        style="fg:ansiwhite bold",
                    ))

                # Also offer the numeric index (e.g. "1")
                idx_str = str(idx)
                if idx_str.startswith(current_word):
                    suggestions.append(Completion(
                        idx_str,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansiwhite><b>{idx_str:<4}</b></ansiwhite> "
                            f"<ansimagenta>{html_escape(display_text)}"
                            f"</ansimagenta>"),
                        style="fg:ansiwhite bold",
                    ))
        except (ImportError, AttributeError):
            pass

        return suggestions

    def get_mcp_server_suggestions(self, current_word: str) -> List[Completion]:
        """Get MCP server name suggestions.
        
        Args:
            current_word: The current word being typed
            
        Returns:
            A list of completions for MCP servers
        """
        suggestions = []
        
        try:
            # Import the global MCP servers registry
            from cai.repl.commands.mcp import _GLOBAL_MCP_SERVERS
            
            # Get all active MCP server names
            for server_name in _GLOBAL_MCP_SERVERS.keys():
                # Get server type for display
                server = _GLOBAL_MCP_SERVERS[server_name]
                server_type = type(server).__name__.replace("MCPServer", "")
                
                # Exact prefix match
                if server_name.startswith(current_word):
                    suggestions.append(Completion(
                        server_name,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansicyan><b>{server_name}</b></ansicyan> "
                            f"<ansiwhite>({server_type})</ansiwhite>"),
                        style="fg:ansicyan bold"
                    ))
                # Fuzzy match
                elif (current_word.lower() in server_name.lower() and
                      not server_name.startswith(current_word)):
                    suggestions.append(Completion(
                        server_name,
                        start_position=-len(current_word),
                        display=HTML(
                            f"<ansicyan>{server_name}</ansicyan> "
                            f"<ansiwhite>({server_type})</ansiwhite>"),
                        style="fg:ansicyan"
                    ))
        except (ImportError, AttributeError):
            pass  # No MCP servers available
            
        return suggestions

    def _refresh_virtualization_completion_cache(self) -> None:
        """Populate container IDs and image names for /virtualization completions."""
        from cai.repl.commands._virtualization_monolith import DEFAULT_IMAGES, DockerManager

        def _default_image_suggestions() -> List[str]:
            names = set(DEFAULT_IMAGES.keys())
            for meta in DEFAULT_IMAGES.values():
                sid = meta.get("id")
                if isinstance(sid, str) and sid:
                    names.add(sid)
            return sorted(names)

        now = time.monotonic()
        with self._virt_comp_lock:
            if now - self._virt_comp_last < 20.0 and (self._virt_comp_containers or self._virt_comp_images):
                return
            self._virt_comp_last = now
            self._virt_comp_containers = []
            self._virt_comp_images = []
            try:
                self._virt_comp_images = _default_image_suggestions()
                dm = DockerManager()
                if not dm.is_docker_installed() or not dm.is_docker_running():
                    return
                seen_c: Set[str] = set()
                for c in dm.get_container_list():
                    cid = (c.get("ID") or "").strip()
                    if not cid:
                        continue
                    short = cid[:12]
                    for token in (short, cid):
                        if token not in seen_c:
                            seen_c.add(token)
                            self._virt_comp_containers.append(token)
                self._virt_comp_containers.sort()
                seen_i: Set[str] = set(self._virt_comp_images)
                for img in dm.get_images_list():
                    repo = (img.get("Repository") or "").strip()
                    if not repo or repo == "<none>":
                        continue
                    tag = (img.get("Tag") or "").strip()
                    if tag and tag != "<none>" and tag != "latest":
                        label = f"{repo}:{tag}"
                    else:
                        label = repo
                    if label not in seen_i:
                        seen_i.add(label)
                        self._virt_comp_images.append(label)
                self._virt_comp_images.sort()
            except Exception:  # pylint: disable=broad-except
                self._virt_comp_containers = []
                self._virt_comp_images = _default_image_suggestions()

    def get_virtualization_arg_completions(self, subcommand: str, current_word: str):
        """Complete container IDs after `set`, image names after `run`."""
        if subcommand not in ("set", "run"):
            return
        self._refresh_virtualization_completion_cache()
        cw = (current_word or "").lower()
        if subcommand == "set":
            for cid in self._virt_comp_containers:
                if cid.lower().startswith(cw):
                    yield Completion(
                        cid,
                        start_position=-len(current_word),
                        display=HTML(
                            f'<span style="color:#00ff9d"><b>{html_escape(cid)}</b></span> '
                            "<span style='color:#b8d4c9'>container</span>"
                        ),
                        style="fg:#00ff9d bold",
                    )
        elif subcommand == "run":
            for name in self._virt_comp_images:
                if name.lower().startswith(cw):
                    yield Completion(
                        name,
                        start_position=-len(current_word),
                        display=HTML(
                            f'<span style="color:#00ff9d"><b>{html_escape(name)}</b></span> '
                            "<span style='color:#b8d4c9'>image</span>"
                        ),
                        style="fg:#00ff9d bold",
                    )

    def get_mcp_suggestions(self, words: List[str], current_word: str) -> List[Completion]:
        """Get context-aware MCP command completions.
        
        Args:
            words: List of words including empty string if trailing space
            current_word: The current word being typed (empty if trailing space)
        
        Returns:
            List of completion suggestions
        """
        suggestions = []
        
        # Get the actual typed words (excluding empty strings from trailing spaces)
        actual_words = [w for w in words if w]
        
        # Position 2: Completing subcommand (e.g., "/mcp <tab>")
        # Use the default subcommand handler - no need to duplicate!
        if len(words) == 2:
            return self.get_subcommand_suggestions(words[0], current_word)
        
        # Position 3: After subcommand (e.g., "/mcp load <tab>")
        elif len(words) == 3 and len(actual_words) > 1:
            subcommand = actual_words[1]
            
            if subcommand == "load":
                # Suggest transport types for load command
                if not current_word.startswith("http"):  # Don't suggest if typing URL
                    transports = [
                        ("stdio", "Local process communication"),
                        ("sse", "Server-Sent Events (HTTP)"),
                    ]
                    for transport, desc in transports:
                        if transport.startswith(current_word):
                            suggestions.append(Completion(
                                transport,
                                start_position=-len(current_word),
                                display=HTML(
                                    f"<ansiyellow><b>{transport}</b></ansiyellow> "
                                    f"<ansiwhite>- {desc}</ansiwhite>"),
                                style="fg:ansiyellow bold"
                            ))
                            
            elif subcommand in ["add", "remove", "tools", "test"]:
                # These commands need an MCP server name
                suggestions.extend(self.get_mcp_server_suggestions(current_word))
        
        # Position 4: After server name in add command (e.g., "/mcp add server <tab>")
        elif len(words) == 4 and len(actual_words) > 1:
            subcommand = actual_words[1]
            
            if subcommand == "add":
                # After server name, suggest agent names
                suggestions.extend(self.get_agent_suggestions(current_word))
                
        return suggestions

    # pylint: disable=unused-argument
    def get_completions(self, document, complete_event):
        """Get completions for the current document
        with fuzzy matching support.

        Args:
            document: The document to complete
            complete_event: The completion event

        Returns:
            A generator of completions
        """
        # Keep original text to detect trailing spaces
        text_original = document.text_before_cursor
        text = text_original.strip()
        words = text.split()
        
        # Check if there's a trailing space (user finished typing a word)
        has_trailing_space = text_original and text_original[-1] == ' '

        # Refresh Ollama models and agents periodically
        self.fetch_all_models()
        self.fetch_all_agents()

        if not text:
            # Show all main commands with descriptions
            command_descriptions = self.get_command_descriptions()

            # Sort commands by usage frequency (for command shadowing)
            sorted_commands = sorted(
                command_descriptions.items(),
                key=lambda x: self.command_history.get(x[0], 0),
                reverse=True
            )

            for cmd, description in sorted_commands:
                yield Completion(
                    cmd,
                    start_position=0,
                    display=HTML(
                        f"<ansicyan><b>{cmd:<15}</b></ansicyan> "
                        f"{html_escape(description)}"),
                    style="fg:ansicyan bold"
                )
            return

        if text.startswith('/'):
            # Determine current word and effective word count based on trailing space
            # Example: "/mcp " has trailing space, so current_word="" and we add empty string to words
            # Example: "/mcp" has no trailing space, so current_word="/mcp" 
            if has_trailing_space:
                current_word = ""
                effective_words = words + [""]  # Add empty string to represent new word position
            else:
                current_word = words[-1] if words else ""
                effective_words = words

            # Main command completion (first word)
            if len(effective_words) == 1 and not has_trailing_space:
                # Get command suggestions
                yield from self.get_command_suggestions(current_word)

            # Subcommand completion (second word)
            elif len(effective_words) == 2:
                cmd = words[0]
                resolved_cmd = COMMAND_ALIASES.get(cmd, cmd)

                # Special handling for model command
                if cmd in ["/model", "/mod"]:
                    yield from self.get_model_suggestions(current_word)
                elif resolved_cmd == "/resume":
                    yield from self._resume_arg_completions(current_word)
                elif resolved_cmd == "/sessions":
                    yield from self._sessions_arg_completions(current_word)
                # Add special handling for MCP command
                elif cmd in ["/mcp", "/m"]:
                    yield from self.get_mcp_suggestions(effective_words, current_word)
                elif COMMAND_ALIASES.get(cmd, cmd) == "/merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=False,
                        has_trailing_space=has_trailing_space,
                    )
                else:
                    # Get subcommand suggestions
                    yield from self.get_subcommand_suggestions(cmd, current_word)

            # Third word completion
            elif len(effective_words) == 3:
                cmd = words[0]
                subcommand = words[1] if len(words) > 1 else ""
                resolved_cmd = COMMAND_ALIASES.get(cmd, cmd)

                if resolved_cmd == "/merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=False,
                        has_trailing_space=has_trailing_space,
                    )
                elif cmd in ["/parallel", "/par", "/p"] and len(words) >= 2 and words[1] == "merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=True,
                        has_trailing_space=has_trailing_space,
                    )
                elif cmd in ["/model", "/mod"] and subcommand == "show":
                    yield from self._model_show_filter_completions(current_word)
                # Compact model / --model: reuse model suggestions
                elif cmd in ["/compact", "/cmp"] and subcommand in ["model", "--model"]:
                    yield from self.get_model_suggestions(current_word)
                # Agent select completion
                elif cmd in ["/agent", "/a"] and subcommand in ["select", "info"]:
                    yield from self.get_agent_suggestions(current_word)
                elif resolved_cmd == "/flush" and subcommand.lower() == "agent":
                    yield from self.get_flush_agent_nonempty_suggestions(current_word)
                elif resolved_cmd == "/env" and subcommand == "default":
                    return
                elif (resolved_cmd == "/help" and subcommand == "var") or (
                    resolved_cmd == "/env" and subcommand in ("get", "set")
                ):
                    yield from self.get_env_catalog_target_suggestions(current_word)
                elif resolved_cmd == "/virtualization" and subcommand in ("set", "run"):
                    yield from self.get_virtualization_arg_completions(subcommand, current_word)
                # Parallel add: all agents including patterns
                elif cmd in ["/parallel", "/par", "/p"] and subcommand == "add":
                    yield from self.get_all_agent_suggestions(current_word)
                # Parallel remove: only currently configured agents
                elif cmd in ["/parallel", "/par", "/p"] and subcommand == "remove":
                    yield from self.get_parallel_config_suggestions(current_word)
                # Queue add: suggest --agent flag
                elif cmd in ["/queue", "/que"] and subcommand == "add":
                    flag = "--agent"
                    if flag.startswith(current_word):
                        yield Completion(
                            flag,
                            start_position=-len(current_word),
                            display=HTML(
                                f"<ansiyellow><b>{flag}</b></ansiyellow> "
                                "Specify agent for this prompt"),
                            style="fg:ansiyellow bold",
                        )
                # MCP command completion for third word
                elif cmd in ["/mcp", "/m"]:
                    yield from self.get_mcp_suggestions(effective_words, current_word)
                elif resolved_cmd == "/resume" and len(words) >= 2:
                    yield from self._resume_dir_token_completions(words[1], current_word)

            # Fourth word completion
            elif len(effective_words) == 4:
                cmd = words[0]
                subcommand = words[1] if len(words) > 1 else ""
                third_word = words[2] if len(words) > 2 else ""
                resolved_cmd = COMMAND_ALIASES.get(cmd, cmd)

                if resolved_cmd == "/merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=False,
                        has_trailing_space=has_trailing_space,
                    )
                elif cmd in ["/parallel", "/par", "/p"] and len(words) >= 2 and words[1] == "merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=True,
                        has_trailing_space=has_trailing_space,
                    )
                # Queue add --agent: suggest agent names
                elif (cmd in ["/queue", "/que"]
                        and subcommand == "add"
                        and third_word == "--agent"):
                    yield from self.get_all_agent_suggestions(current_word)
                # MCP add command needs agent name as fourth word
                elif cmd in ["/mcp", "/m"]:
                    yield from self.get_mcp_suggestions(effective_words, current_word)
                elif resolved_cmd == "/env" and subcommand == "set":
                    if self._resolved_env_set_target_is_ctf_name(words):
                        yield from self.get_ctf_name_suggestions(current_word)
                    elif self._resolved_env_set_target_is_model(words):
                        yield from self.get_model_suggestions(current_word)

            # Fifth+ word: /merge and /parallel merge agent arguments
            elif len(effective_words) >= 5:
                cmd = words[0]
                resolved_cmd = COMMAND_ALIASES.get(cmd, cmd)
                if resolved_cmd == "/merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=False,
                        has_trailing_space=has_trailing_space,
                    )
                elif cmd in ["/parallel", "/par", "/p"] and len(words) >= 2 and words[1] == "merge":
                    yield from self._yield_merge_agent_arg_completions(
                        words,
                        current_word,
                        parallel_merge=True,
                        has_trailing_space=has_trailing_space,
                    )
                elif resolved_cmd == "/env" and len(words) >= 2 and words[1] == "set":
                    if self._resolved_env_set_target_is_ctf_name(words):
                        yield from self.get_ctf_name_suggestions(current_word)
                    elif self._resolved_env_set_target_is_model(words):
                        yield from self.get_model_suggestions(current_word)
