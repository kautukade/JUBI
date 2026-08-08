"""
Simple Agent Manager - Manages the single active agent instance.

This module ensures that only ONE agent instance exists at a time,
unless explicitly configured for parallel execution.
"""

import os
import threading
import weakref
import logging
from typing import Optional, Dict, Any, List
from datetime import datetime

# Primary (non-parallel-slot) session agent in headless/REPL. Parallel workers use P1, P2, …
DEFAULT_SESSION_AGENT_ID = "P0"


class SimpleAgentManager:
    """Manages the single active agent instance."""

    def __init__(self):
        self._active_agent = None  # The ONE active agent
        self._agent_id = DEFAULT_SESSION_AGENT_ID
        self._message_history: Dict[str, list] = {}  # Agent name -> history
        self._agent_registry: Dict[str, str] = {}  # Agent name -> ID mapping
        self._id_counter = 0  # Counter for generating IDs
        self._parallel_agents: Dict[str, Any] = {}  # ID -> agent ref for parallel mode
        self._pending_history_transfer = None  # Temporary storage for history transfer
        self._active_agent_name = None  # Track the currently active agent name
        self._swarm_agents: Dict[str, str] = {}  # Track swarm pattern agents: agent_name -> ID
        self._swarm_counter = 0  # Counter for swarm agent IDs
        self._registry_lock = threading.Lock()  # Thread-safe registry operations
        self._terminal_count = 0  # Track active terminals in TUI mode
        self._p_id_to_agent_name = {}  # Map P-ID to actual agent name for TUI mode
        self.logger = logging.getLogger("SimpleAgentManager")
        
        # Session shared context - enables context sharing between agents
        self._session_shared_context: List[Dict[str, Any]] = []  # Shared context between agents
        self._session_metadata: Dict[str, Any] = {
            "session_id": None,
            "agents_used": [],  # List of agents that have participated in this session
        }

    def set_active_agent(self, agent, agent_name: str, agent_id: str = None):
        """Set the active agent instance.

        In TUI mode this additionally enforces:
        - One registry key per terminal prefix (Tn_*).
        - Stable reuse of the same P-ID for a terminal across switches.
        - Removal of stale terminal keys to prevent "dead agents".
        """
        with self._registry_lock:  # Thread-safe registration
            result = self._set_active_agent_internal(agent, agent_name, agent_id)

            # Enforce single key per terminal in TUI mode
            if os.getenv("CAI_TUI_MODE") == "true" and agent_id and agent_id.startswith("T"):
                try:
                    terminal_prefix = agent_id.split("_", 1)[0]  # e.g., T1
                    current_p_id = self._agent_registry.get(agent_id)
                    if current_p_id:
                        for key in list(self._agent_registry.keys()):
                            if key.startswith(terminal_prefix + "_") and key != agent_id:
                                old_p = self._agent_registry.get(key)
                                # Drop empty histories tied to old P-IDs
                                if old_p in self._message_history and not self._message_history[old_p]:
                                    del self._message_history[old_p]
                                del self._agent_registry[key]
                except Exception:
                    # Never fail the registration due to cleanup issues
                    pass

            return result
    
    def _set_active_agent_internal(self, agent, agent_name: str, agent_id: str = None):
        """Internal method for setting active agent (must be called with lock held)."""
        # Validate inputs
        if not agent or not agent_name:
            return  # Don't register empty agents
        # In single agent mode, use switch_to_single_agent for proper cleanup
        # IMPORTANT: Never use switch_to_single_agent in TUI mode as it deletes other terminals' agents
        if not self._parallel_agents and not agent_id and os.getenv("CAI_TUI_MODE") != "true":
            # If we're in single agent mode and no explicit ID is provided
            # Check if this is actually a switch (different agent than current)
            if self._active_agent_name and self._active_agent_name != agent_name:
                # This is a switch - use the proper method
                self.switch_to_single_agent(agent, agent_name)
                return

        # Otherwise, proceed with normal set_active_agent logic
        self._active_agent = weakref.ref(agent) if agent else None
        self._active_agent_name = agent_name  # Track the active agent name

        # Check if this agent is part of a swarm pattern
        is_swarm_agent = False
        if hasattr(agent, "pattern") and agent.pattern == "swarm":
            is_swarm_agent = True

        # In single agent mode, check for swarm patterns
        # Note: TUI mode with agent_id should not be treated as single agent mode
        if not self._parallel_agents and not (os.getenv("CAI_TUI_MODE") == "true" and agent_id):
            if is_swarm_agent:
                # For swarm agents, assign unique IDs like P1-1, P1-2, etc.
                if agent_name not in self._swarm_agents:
                    self._swarm_counter += 1
                    swarm_id = f"P1-{self._swarm_counter}"
                    self._swarm_agents[agent_name] = swarm_id
                    self._agent_registry[agent_name] = swarm_id
                else:
                    swarm_id = self._swarm_agents[agent_name]
                self._agent_id = swarm_id
            else:
                # Non-swarm single agents: session primary (parallel slots use P1, P2, …)
                self._agent_id = DEFAULT_SESSION_AGENT_ID
                self._agent_registry[agent_name] = DEFAULT_SESSION_AGENT_ID
        else:
            # For parallel mode or TUI mode
            if os.getenv("CAI_TUI_MODE") == "true" and agent_id and agent_id.startswith("T"):
                # In TUI mode with terminal-specific IDs (T1_agent_name)
                # Extract terminal number from agent_id
                terminal_num = agent_id.split("_")[0] if "_" in agent_id else agent_id
                
                # Check if this terminal already has a P-ID assigned
                existing_p_id = None
                for key, p_id in self._agent_registry.items():
                    if key.startswith(f"{terminal_num}_"):
                        existing_p_id = p_id
                        # Prefer an existing P-ID we already mapped to a display name
                        if p_id in self._p_id_to_agent_name:
                            break
                
                if existing_p_id:
                    # Reuse the same P-ID for this terminal
                    self._agent_id = existing_p_id
                    self._agent_registry[agent_id] = existing_p_id
                else:
                    # New terminal, increment counter
                    self._id_counter += 1
                    self._agent_id = f"P{self._id_counter}"
                    self._agent_registry[agent_id] = self._agent_id
            elif agent_id:
                # Explicit ID provided
                self._agent_id = agent_id
            else:
                # Only increment counter for new agents in parallel mode
                if agent_name not in self._agent_registry:
                    self._id_counter += 1
                    agent_id = f"P{self._id_counter}"
                else:
                    agent_id = self._agent_registry[agent_name]
                self._agent_id = agent_id
            # Registration already handled above for TUI mode
            if os.getenv("CAI_TUI_MODE") != "true":
                self._agent_registry[agent_name] = self._agent_id

        # Initialize message history for this agent if needed
        # In TUI mode, use the assigned P-ID as key for history isolation
        history_key = self._agent_id if os.getenv("CAI_TUI_MODE") == "true" else agent_name
        if history_key not in self._message_history:
            # Check if the agent's model already has a history and use that reference
            if hasattr(agent, "model") and hasattr(agent.model, "message_history"):
                self._message_history[history_key] = agent.model.message_history
            else:
                self._message_history[history_key] = []
        
        # In TUI mode, store the actual agent name for this P-ID
        if os.getenv("CAI_TUI_MODE") == "true" and agent:
            # Get the actual display name from the agent object
            if hasattr(agent, 'name'):
                display_name = agent.name
                # Remove terminal suffix if present (e.g., "Bug Bounter (T2)" -> "Bug Bounter")
                if " (T" in display_name and display_name.endswith(")"):
                    display_name = display_name[:display_name.rfind(" (T")]
                self._p_id_to_agent_name[self._agent_id] = display_name
                
                # Debug logging
                if os.getenv("CAI_DEBUG") == "1":
                    self.logger.info(f"Stored P-ID mapping: {self._agent_id} -> {display_name}")

    def get_active_agent(self):
        """Get the active agent instance."""
        if self._active_agent:
            return self._active_agent()
        return None

    def get_agent_id(self) -> str:
        """Get the ID of the active agent."""
        return self._agent_id

    def get_message_history(self, agent_name: str) -> list:
        """Get message history for an agent."""
        # In TUI mode, histories are keyed by P-ID, not agent name
        if os.getenv("CAI_TUI_MODE") == "true":
            # If the agent_name looks like a P-ID, use it directly
            if agent_name.startswith("P") and agent_name[1:].isdigit():
                return self._message_history.get(agent_name, [])
            
            # If agent_name has [P1] suffix, extract and use that
            if "[P" in agent_name and "]" in agent_name:
                start = agent_name.rfind("[P") + 1
                end = agent_name.find("]", start)
                p_id = agent_name[start:end]
                return self._message_history.get(p_id, [])
            
            # Otherwise, try to find the P-ID for this agent
            for key, agent_id in self._agent_registry.items():
                if key == agent_name or (key.startswith("T") and "_" in key and key.split("_", 1)[1] == agent_name):
                    return self._message_history.get(agent_id, [])
        return self._message_history.get(agent_name, [])

    def add_to_history(self, agent_name: str, message: dict):
        """Add a message to agent's history."""
        # In TUI mode, find the correct P-ID key
        history_key = agent_name
        if os.getenv("CAI_TUI_MODE") == "true":
            # If already a P-ID, use it
            if agent_name.startswith("P") and agent_name[1:].isdigit():
                history_key = agent_name
            else:
                # Find the P-ID for this agent
                for key, agent_id in self._agent_registry.items():
                    if key == agent_name or (key.startswith("T") and "_" in key and key.split("_", 1)[1] == agent_name):
                        history_key = agent_id
                        break
        
        if history_key not in self._message_history:
            self._message_history[history_key] = []
        self._message_history[history_key].append(message)

    def clear_history(self, agent_name: str):
        """Clear history for an agent."""
        # In TUI mode, find the correct P-ID key
        history_key = agent_name
        if os.getenv("CAI_TUI_MODE") == "true":
            # If already a P-ID, use it
            if agent_name.startswith("P") and agent_name[1:].isdigit():
                history_key = agent_name
            else:
                # Find the P-ID for this agent
                for key, agent_id in self._agent_registry.items():
                    if key == agent_name or (key.startswith("T") and "_" in key and key.split("_", 1)[1] == agent_name):
                        history_key = agent_id
                        break
        
        if history_key in self._message_history:
            # Clear the list in-place to maintain the same reference
            # This is critical when the model and manager share the same list
            self._message_history[history_key].clear()

        # Also clear the active agent's model instance history if it matches
        # This handles cases where they don't share the same reference
        if self._active_agent and self._active_agent_name == agent_name:
            agent = self._active_agent()
            if agent and hasattr(agent, "model") and hasattr(agent.model, "message_history"):
                agent.model.message_history.clear()

    def clear_all_histories(self):
        """Clear all message histories."""
        self._message_history.clear()

    def get_all_histories(self) -> Dict[str, list]:
        """Get all agent histories."""
        # Clean up duplicates first in single agent mode
        if not self._parallel_agents:
            self._cleanup_single_agent_duplicates()

        # Clean up any duplicate IDs in parallel mode
        if self._parallel_agents:
            self._cleanup_duplicate_ids()
        
        # Clean up orphaned agents in TUI mode
        if os.getenv("CAI_TUI_MODE") == "true":
            self.cleanup_tui_orphaned_agents()

        # Return histories for all registered agents
        result = {}

        # In TUI mode, we need to handle terminal-specific agent IDs
        if os.getenv("CAI_TUI_MODE") == "true":
            # Debug: show what's in message history
            if os.getenv("CAI_DEBUG", "1") == "1":
                self.logger.info(f"TUI mode _message_history keys: {list(self._message_history.keys())}")
                for key, history in self._message_history.items():
                    self.logger.info(f"  {key}: {len(history)} messages")
            
            # In TUI mode, histories are keyed by agent_id (T1_agent_name, T2_agent_name, etc.)
            # We need to show all registered agents regardless of history
            
            # Get unique agent IDs with their display names
            displayed_agents = set()  # Track which agents we've already displayed
            
            # Build a map of P-ID to terminal numbers
            p_id_to_terminals = {}  # P-ID -> list of terminal numbers
            
            for key, agent_id in self._agent_registry.items():
                if key.startswith("T") and "_" in key:
                    parts = key.split("_", 1)
                    terminal_num = parts[0]  # T1, T2, etc.
                    if agent_id not in p_id_to_terminals:
                        p_id_to_terminals[agent_id] = []
                    p_id_to_terminals[agent_id].append(terminal_num)
            
            # Build a map of terminal to current agent
            terminal_to_agent = {}  # Terminal -> (agent_name, P-ID)
            
            # Find the most recent agent for each terminal
            for key, agent_id in self._agent_registry.items():
                if key.startswith("T") and "_" in key:
                    parts = key.split("_", 1)
                    terminal_num = parts[0]  # T1, T2, etc.
                    key_suffix = parts[1]  # e.g., "bug_bounter_agent" or UUID
                    
                    # Try to get the actual agent name from the P-ID mapping first
                    actual_agent_name = self._p_id_to_agent_name.get(agent_id, key_suffix)
                    
                    # If not found in mapping, try other methods
                    if actual_agent_name == key_suffix:
                        # If we have an active agent with this ID, get its actual name
                        if agent_id == self._agent_id and self._active_agent:
                            agent = self._active_agent()
                            if agent and hasattr(agent, 'name'):
                                actual_agent_name = agent.name
                                # Remove terminal suffix if present (e.g., "Bug Bounter (T2)" -> "Bug Bounter")
                                if " (T" in actual_agent_name and actual_agent_name.endswith(")"):
                                    actual_agent_name = actual_agent_name[:actual_agent_name.rfind(" (T")]
                        else:
                            # Check in parallel agents
                            if agent_id in self._parallel_agents and self._parallel_agents[agent_id]:
                                agent_ref = self._parallel_agents[agent_id]
                                agent = agent_ref()
                                if agent and hasattr(agent, 'name'):
                                    actual_agent_name = agent.name
                                    # Remove terminal suffix if present
                                    if " (T" in actual_agent_name and actual_agent_name.endswith(")"):
                                        actual_agent_name = actual_agent_name[:actual_agent_name.rfind(" (T")]
                    
                    # Store the agent for this terminal (overwrites previous if exists)
                    # This ensures we only show the current agent for each terminal
                    terminal_to_agent[terminal_num] = (actual_agent_name, agent_id)
            
            # Now build results showing only current agents per terminal
            for terminal_num, (agent_name, agent_id) in sorted(terminal_to_agent.items()):
                # Get history using the agent_id (which is the actual key in TUI mode)
                history = self._message_history.get(agent_id, [])
                
                # Debug logging
                if os.getenv("CAI_DEBUG", "1") == "1" and agent_id in self._message_history:
                    self.logger.info(f"TUI history for {agent_id}: {len(self._message_history[agent_id])} messages")
                
                # Create display name with terminal number
                display_name = f"{agent_name} [{terminal_num}] [{agent_id}]"
                
                # Add to result
                result[display_name] = history
        elif not self._parallel_agents:
            # Single agent mode (non-TUI)
            # Always show the active agent, even if it has no history
            if self._active_agent_name and self._active_agent_name in self._agent_registry:
                agent_id = self._agent_registry[self._active_agent_name]
                history = self._message_history.get(self._active_agent_name, [])
                result[f"{self._active_agent_name} [{agent_id}]"] = history

            # Show all other registered agents that have history
            for agent_name, agent_id in sorted(self._agent_registry.items()):
                # Skip the active agent (already added above)
                if agent_name == self._active_agent_name:
                    continue

                history = self._message_history.get(agent_name, [])
                # Only include non-active agents if they have history
                if history:
                    result[f"{agent_name} [{agent_id}]"] = history
        else:
            # In parallel mode, show all registered agents
            for agent_name, agent_id in sorted(self._agent_registry.items()):
                history = self._message_history.get(agent_name, [])
                result[f"{agent_name} [{agent_id}]"] = history

        return result

    def get_agent_by_id(self, agent_id: str) -> Optional[str]:
        """Get agent name by ID."""
        # Check all registered agents
        for agent_name, aid in self._agent_registry.items():
            if aid == agent_id:
                return agent_name
        return None

    def get_id_by_name(self, agent_name: str) -> Optional[str]:
        """Get ID by agent name."""
        return self._agent_registry.get(agent_name)

    def reset_registry(self):
        """Reset the agent registry (for testing or clean start)."""
        # In TUI mode, be more careful about what we clear
        if os.getenv("CAI_TUI_MODE") == "true":
            # Only clear if this is truly a fresh start (no terminals active)
            # This is typically only called at SessionManager init
            # Individual terminals should manage their own agents
            if not hasattr(self, '_terminal_count') or self._terminal_count == 0:
                self._agent_registry.clear()
                self._message_history.clear()
            else:
                # If terminals are active, preserve their agents
                # Just reset counters and clear parallel agents
                self._parallel_agents.clear()
        else:
            # Keep agents with message history
            agents_to_keep = {}
            for agent_name, agent_id in self._agent_registry.items():
                if self._message_history.get(agent_name):
                    agents_to_keep[agent_name] = agent_id
            self._agent_registry = agents_to_keep
        
        self._id_counter = 0
        self._agent_id = DEFAULT_SESSION_AGENT_ID
        self._parallel_agents.clear()
        self._swarm_agents.clear()
        self._swarm_counter = 0

    def set_parallel_agent(self, agent_id: str, agent, agent_name: str):
        """Register a parallel agent."""
        # CRITICAL: Always use the agent's proper name, not the agent key
        # This prevents duplicate registrations like "blueteam_agent" and "Blue Team Agent"
        if hasattr(agent, 'name') and agent.name:
            agent_name = agent.name
        
        # Check if this ID is already registered to a different agent
        existing_agent_name = self.get_agent_by_id(agent_id)
        if existing_agent_name and existing_agent_name != agent_name:
            # Don't overwrite existing registration - just update the agent reference
            self._parallel_agents[agent_id] = weakref.ref(agent) if agent else None
            return

        self._parallel_agents[agent_id] = weakref.ref(agent) if agent else None
        self._agent_registry[agent_name] = agent_id

        # Initialize message history for this agent if needed
        if agent_name not in self._message_history:
            # Check if the agent's model already has a history and use that reference
            if hasattr(agent, "model") and hasattr(agent.model, "message_history"):
                self._message_history[agent_name] = agent.model.message_history
            else:
                self._message_history[agent_name] = []
        
        # Store the actual agent name for this P-ID
        if agent and hasattr(agent, 'name'):
            display_name = agent.name
            self._p_id_to_agent_name[agent_id] = display_name

    def clear_parallel_agents(self):
        """Clear all parallel agents (when switching to single agent mode)."""
        self._parallel_agents.clear()

    def clear_all_agents_except_pending_history(self):
        """Clear ALL agents from registry but preserve any pending history transfer.

        This is used when switching from parallel to single agent mode to ensure
        no lingering agents remain active.
        """
        # CRITICAL: In TUI mode, we should NOT clear all agents
        # as each terminal has its own agent that must be preserved
        if os.getenv("CAI_TUI_MODE") == "true":
            # In TUI mode, only clear parallel agents, not the entire registry
            self._parallel_agents.clear()
            return
            
        # Store any pending history transfer
        pending_history = self._pending_history_transfer

        # Store ALL existing message histories before clearing
        # This preserves histories from agents that existed before parallel mode
        existing_histories = dict(self._message_history)

        # Clear everything
        self._agent_registry.clear()
        self._parallel_agents.clear()
        self._active_agent = None
        self._active_agent_name = None
        self._agent_id = DEFAULT_SESSION_AGENT_ID
        self._id_counter = 0

        # Restore the message histories - they are needed for history preservation
        self._message_history = existing_histories

        # Restore pending history if any
        self._pending_history_transfer = pending_history

    def get_active_agents(self) -> Dict[str, str]:
        """Get only truly active agents with their IDs."""
        active = {}

        # In single agent mode
        if not self._parallel_agents:
            # Use the tracked active agent name
            if self._active_agent_name and self._active_agent_name in self._agent_registry:
                active[self._active_agent_name] = self._agent_registry[self._active_agent_name]
        else:
            # In parallel mode, check parallel agents
            for aid, agent_ref in list(self._parallel_agents.items()):
                if agent_ref and agent_ref():
                    # Find agent name for this ID
                    for name, registered_id in self._agent_registry.items():
                        if registered_id == aid:
                            active[name] = aid
                            break

        return active

    def get_registered_agents(self) -> Dict[str, str]:
        """Get all registered agents, whether active or not."""
        return dict(self._agent_registry)

    def _cleanup_stale_registrations(self):
        """Clean up stale agent registrations that no longer have active instances."""
        active_agents = self.get_active_agents()

        # Find agents to remove (not active and have no message history)
        to_remove = []
        for agent_name, agent_id in list(self._agent_registry.items()):
            if (
                agent_name not in active_agents
                and len(self._message_history.get(agent_name, [])) == 0
            ):
                to_remove.append(agent_name)

        # Remove stale registrations
        for agent_name in to_remove:
            del self._agent_registry[agent_name]
            if agent_name in self._message_history:
                del self._message_history[agent_name]

        # Reset ID counter to highest used ID
        if self._agent_registry:
            max_id = 0
            for agent_id in self._agent_registry.values():
                if agent_id.startswith("P") and agent_id[1:].isdigit():
                    max_id = max(max_id, int(agent_id[1:]))
            self._id_counter = max_id

    def _cleanup_single_agent_duplicates(self):
        """Clean up duplicate session-primary (P0) entries in single agent mode."""
        if self._parallel_agents:
            return  # Only cleanup in single agent mode

        # Find all agents sharing the default session ID
        dup_agents = [
            (name, aid)
            for name, aid in list(self._agent_registry.items())
            if aid == DEFAULT_SESSION_AGENT_ID
        ]

        if len(dup_agents) <= 1:
            return  # No duplicates

        # Use the tracked active agent name
        active_agent_name = self._active_agent_name

        # Keep only the active agent and those with message history
        for agent_name, agent_id in dup_agents:
            if agent_name != active_agent_name:
                # Check if this agent has any message history
                if not self._message_history.get(agent_name):
                    # No history, safe to remove
                    del self._agent_registry[agent_name]
                    if agent_name in self._message_history:
                        del self._message_history[agent_name]

    def _cleanup_duplicate_ids(self):
        """Clean up agents with duplicate IDs in parallel mode."""
        # Build a map of ID to agent names
        id_to_agents = {}
        for agent_name, agent_id in list(self._agent_registry.items()):
            if agent_id not in id_to_agents:
                id_to_agents[agent_id] = []
            id_to_agents[agent_id].append(agent_name)

        # For each ID with duplicates, keep only the one that should be active according to PARALLEL_CONFIGS
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        for agent_id, agent_names in id_to_agents.items():
            if len(agent_names) > 1:
                # Find which agent should have this ID based on PARALLEL_CONFIGS
                correct_agent_name = None

                # Check parallel configs for the correct mapping
                for config in PARALLEL_CONFIGS:
                    if config.id == agent_id:
                        # For pattern-based configs, we need to resolve to the actual agent name
                        if config.agent_name.endswith("_pattern"):
                            from cai.agents.patterns import get_pattern

                            pattern = get_pattern(config.agent_name)
                            if pattern and hasattr(pattern, "entry_agent"):
                                correct_agent_name = getattr(pattern.entry_agent, "name", None)
                                break
                        else:
                            from cai.agents import get_available_agents

                            available_agents = get_available_agents()
                            if config.agent_name in available_agents:
                                agent = available_agents[config.agent_name]
                                correct_agent_name = getattr(agent, "name", config.agent_name)
                                break

                # If we found the correct agent, keep only that one
                if correct_agent_name and correct_agent_name in agent_names:
                    for name in agent_names:
                        if name != correct_agent_name:
                            del self._agent_registry[name]
                else:
                    # Otherwise, keep the first one with an active parallel agent
                    active_name = None
                    for name in agent_names:
                        if agent_id in self._parallel_agents and self._parallel_agents[agent_id]:
                            agent_ref = self._parallel_agents[agent_id]
                            if agent_ref():  # Check if weakref is still valid
                                active_name = name
                                break

                    if not active_name:
                        active_name = agent_names[0]

                    # Remove all others
                    for name in agent_names:
                        if name != active_name:
                            del self._agent_registry[name]

    def switch_to_single_agent(self, agent, agent_name: str):
        """Switch to a new single agent, properly cleaning up the previous one."""
        # CRITICAL: This method should NEVER be called in TUI mode
        # as it deletes registry entries for other terminals
        if os.getenv("CAI_TUI_MODE") == "true":
            # In TUI mode, just update the agent without cleanup
            self._active_agent = weakref.ref(agent) if agent else None
            self._active_agent_name = agent_name
            return
            
        # Check for pending history transfer (from parallel mode)
        # This is ONLY used when switching from parallel to single agent mode
        transfer_history = None
        if hasattr(self, "_pending_history_transfer") and self._pending_history_transfer:
            transfer_history = self._pending_history_transfer
            self._pending_history_transfer = None

        # Clear parallel agents when switching to single agent mode
        self._parallel_agents.clear()

        # Only clean up agents that have no history
        # Keep agents with history or swarm agents in the registry
        old_agents = list(self._agent_registry.keys())
        for old_name in old_agents:
            if old_name != agent_name:
                # Check if this agent has any history
                if old_name in self._message_history and self._message_history[old_name]:
                    # Keep the agent in registry if it has history
                    continue
                # Also keep swarm agents in the registry
                elif old_name in self._swarm_agents:
                    continue
                else:
                    # Remove from registry only if no history and not a swarm agent
                    # In TUI mode, be extra careful not to delete other terminals' agents
                    if os.getenv("CAI_TUI_MODE") == "true":
                        # This should never happen due to earlier check, but add safety
                        self.logger.warning(f"Attempted to delete registry entry in TUI mode: {old_name}")
                    else:
                        del self._agent_registry[old_name]
                        # Clean up empty history entry
                        if old_name in self._message_history:
                            del self._message_history[old_name]

        # Clear any duplicate session-primary ID entries before setting new one
        self._cleanup_single_agent_duplicates()

        # Check if this agent is part of a swarm pattern
        is_swarm_agent = False
        if hasattr(agent, "pattern") and agent.pattern == "swarm":
            is_swarm_agent = True

        # Assign ID based on whether it's a swarm agent
        if is_swarm_agent:
            # For swarm agents, use unique IDs
            if agent_name not in self._swarm_agents:
                self._swarm_counter += 1
                swarm_id = f"P1-{self._swarm_counter}"
                self._swarm_agents[agent_name] = swarm_id
                self._agent_registry[agent_name] = swarm_id
            else:
                swarm_id = self._swarm_agents[agent_name]
            self._agent_id = swarm_id
        else:
            # Non-swarm single agents: session primary id P0
            self._agent_id = DEFAULT_SESSION_AGENT_ID
            self._agent_registry[agent_name] = DEFAULT_SESSION_AGENT_ID

        self._active_agent = weakref.ref(agent) if agent else None
        self._active_agent_name = agent_name  # Track active agent name

        # Initialize or update message history for this agent
        if agent_name not in self._message_history:
            # Only use transfer_history if we're coming from parallel mode
            if transfer_history:
                self._message_history[agent_name] = transfer_history
            else:
                # Otherwise, start with empty history (don't transfer from other agents)
                self._message_history[agent_name] = []
        else:
            # Agent already has a history entry
            # If there's a transfer_history, always use it (this is an explicit transfer request)
            if transfer_history:
                self._message_history[agent_name] = transfer_history

        # Reset ID counter for cleanliness
        self._id_counter = 1
        
    def share_swarm_history(self, agent1_name: str, agent2_name: str):
        """Share message history between two swarm agents.

        This ensures both agents share the same list reference,
        so changes made by one agent are visible to the other.
        """
        # Get the history from agent1 (or create if doesn't exist)
        if agent1_name in self._message_history:
            shared_history = self._message_history[agent1_name]
        else:
            shared_history = []
            self._message_history[agent1_name] = shared_history

        # Make agent2 share the same reference
        self._message_history[agent2_name] = shared_history
    
    def cleanup_orphaned_parallel_agents(self):
        """Clean up orphaned parallel agents in TUI mode."""
        if os.getenv("CAI_TUI_MODE") != "true":
            return
            
        with self._registry_lock:
            from cai.repl.commands.parallel import PARALLEL_CONFIGS
            
            # Get currently active parallel IDs
            active_parallel_ids = set()
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                active_parallel_ids.add(f"P{idx}")
            
            # Find all P-IDs in registry and their associated keys
            p_id_to_keys = {}  # Map P-ID to all keys that reference it
            terminal_to_p_id = {}  # Map terminal key to P-ID
            
            for key, agent_id in list(self._agent_registry.items()):
                if agent_id.startswith("P") and agent_id[1:].isdigit():
                    if agent_id not in p_id_to_keys:
                        p_id_to_keys[agent_id] = []
                    p_id_to_keys[agent_id].append(key)
                    
                    # Track which terminal uses which P-ID
                    if key.startswith("T") and "_" in key:
                        terminal = key.split("_")[0]
                        terminal_to_p_id[terminal] = agent_id
            
            # Clean up orphaned P-IDs
            for p_id, keys in p_id_to_keys.items():
                # Skip if it's currently active in parallel configs
                if p_id in active_parallel_ids:
                    continue
                    
                # Skip if it has message history
                if self._message_history.get(p_id, []):
                    continue
                    
                # Check if any terminal is actively using this P-ID
                in_use = False
                for terminal, terminal_p_id in terminal_to_p_id.items():
                    if terminal_p_id == p_id:
                        # Check if this terminal key is still in the registry
                        terminal_keys = [k for k in keys if k.startswith(terminal)]
                        if terminal_keys:
                            in_use = True
                            break
                
                if not in_use:
                    # This is an orphaned P-ID, remove all references to it
                    for key in keys:
                        if key in self._agent_registry:
                            del self._agent_registry[key]
                    
                    # Remove from message history if present
                    if p_id in self._message_history:
                        del self._message_history[p_id]
    
    def cleanup_tui_orphaned_agents(self):
        """Clean up orphaned agents in TUI mode (agents without active terminals)."""
        if os.getenv("CAI_TUI_MODE") != "true":
            return
            
        with self._registry_lock:
            # Build a map of which P-IDs have active terminals
            active_p_ids = set()
            terminal_to_latest_key = {}  # Terminal -> latest registry key
            
            # First pass: find all terminal assignments and their latest keys
            for key, agent_id in list(self._agent_registry.items()):
                if key.startswith("T") and "_" in key:
                    terminal_num = key.split("_")[0]
                    # Keep track of the latest key for each terminal
                    if terminal_num not in terminal_to_latest_key:
                        terminal_to_latest_key[terminal_num] = key
                    else:
                        # If we already have a key for this terminal, we have duplicates
                        # Keep the one that matches the current terminal count
                        terminal_number = int(terminal_num[1:])
                        if terminal_number <= self._terminal_count:
                            terminal_to_latest_key[terminal_num] = key
            
            # Second pass: collect active P-IDs
            for terminal_num, key in terminal_to_latest_key.items():
                if key in self._agent_registry:
                    active_p_ids.add(self._agent_registry[key])
            
            # Third pass: clean up orphaned entries
            for key, agent_id in list(self._agent_registry.items()):
                if key.startswith("T") and "_" in key:
                    terminal_num = key.split("_")[0]
                    # Remove if it's not the latest key for its terminal
                    if terminal_num in terminal_to_latest_key and key != terminal_to_latest_key[terminal_num]:
                        del self._agent_registry[key]
                    # Remove if the terminal number exceeds active terminal count
                    elif int(terminal_num[1:]) > self._terminal_count:
                        del self._agent_registry[key]
                        # Also remove the P-ID if it's not used by any active terminal
                        if agent_id not in active_p_ids:
                            if agent_id in self._message_history and not self._message_history[agent_id]:
                                del self._message_history[agent_id]
                            if agent_id in self._p_id_to_agent_name:
                                del self._p_id_to_agent_name[agent_id]

            # Final pass: ensure max one key per terminal
            per_terminal_latest = {}
            for key in list(self._agent_registry.keys()):
                if key.startswith("T") and "_" in key:
                    terminal_num = key.split("_")[0]
                    per_terminal_latest[terminal_num] = key
            for key in list(self._agent_registry.keys()):
                if key.startswith("T") and "_" in key:
                    terminal_num = key.split("_")[0]
                    if per_terminal_latest.get(terminal_num) != key:
                        del self._agent_registry[key]
    
    def increment_terminal_count(self):
        """Increment the count of active terminals."""
        with self._registry_lock:
            self._terminal_count += 1
    
    def decrement_terminal_count(self):
        """Decrement the count of active terminals."""
        with self._registry_lock:
            self._terminal_count = max(0, self._terminal_count - 1)
    
    def get_terminal_count(self) -> int:
        """Get the current count of active terminals."""
        with self._registry_lock:
            return self._terminal_count


    def increment_terminal_count(self):
        """Increment the count of active terminals."""
        with self._registry_lock:
            self._terminal_count += 1
    
    def decrement_terminal_count(self):
        """Decrement the count of active terminals."""
        with self._registry_lock:
            self._terminal_count = max(0, self._terminal_count - 1)
    
    def get_terminal_count(self) -> int:
        """Get the current count of active terminals."""
        return self._terminal_count
    
    def extract_shareable_context(self, agent_name: str, history: List[Dict[str, Any]]) -> None:
        """Extract shareable context from an agent's history before switching agents.
        
        This method extracts key information from the current agent's conversation
        and stores it in session_shared_context for the next agent to access.
        
        Args:
            agent_name: Name of the agent whose context is being extracted
            history: Message history list to extract context from
        """
        import re
        
        if not history:
            return
        
        shareable = {
            "agent_name": agent_name,
            "timestamp": datetime.now().isoformat(),
            "key_exchanges": [],
            "extracted_facts": [],
        }
        
        # Extract last 3 complete user-assistant exchanges
        recent_exchanges = []
        i = 0
        while i < len(history):
            msg = history[i]
            if msg.get("role") == "user":
                user_content = str(msg.get("content", ""))[:200]
                
                # Find the FINAL assistant response for this user message
                # (skip over tool calls and intermediate assistant messages)
                assistant_content = ""
                last_assistant_idx = -1
                
                # Look ahead to find all messages until next user or end
                for j in range(i + 1, len(history)):
                    if history[j].get("role") == "user":
                        break  # Stop at next user message
                    elif history[j].get("role") == "assistant":
                        last_assistant_idx = j
                
                # Extract content from the last assistant message in this exchange
                if last_assistant_idx >= 0:
                    assistant_msg = history[last_assistant_idx]
                    if "content" in assistant_msg and assistant_msg["content"]:
                        content = assistant_msg["content"]
                        if isinstance(content, list):
                            text_parts = []
                            for item in content:
                                if isinstance(item, dict) and "text" in item:
                                    text_parts.append(str(item["text"]))
                                elif isinstance(item, str):
                                    text_parts.append(item)
                            assistant_content = " ".join(text_parts)[:200]
                        else:
                            assistant_content = str(content)[:200]
                
                if assistant_content:
                    recent_exchanges.append({
                        "user": user_content,
                        "assistant": assistant_content,
                    })
                    
                    if len(recent_exchanges) >= 3:
                        break
                
                # Move to next user message
                i = last_assistant_idx + 1 if last_assistant_idx >= 0 else i + 1
            else:
                i += 1
        
        shareable["key_exchanges"] = list(reversed(recent_exchanges))
        
        # Extract key facts using regex patterns
        # Process messages in reverse to get most recent facts first
        for msg in reversed(history):
            if msg.get("role") == "assistant" and msg.get("content"):
                content = str(msg.get("content", ""))
                # Pattern for numeric results (prioritize "equals" or "is" statements)
                # Match patterns like "3" or "equals 3" or "result is 3"
                result_patterns = [
                    (r"(?:equals?|is|=)\s*(\d+)", "result"),  # "equals 3" or "is 3"
                    (r"^(\d+)$", "result"),  # Just a number by itself
                    (r"(\d+)\s*(?:\+|\-)", "operand"),  # Numbers in operations
                ]
                
                for pattern, fact_type in result_patterns:
                    matches = list(re.finditer(pattern, content, re.MULTILINE | re.IGNORECASE))
                    # Take the first match (most prominent result)
                    if matches:
                        shareable["extracted_facts"].append({
                            "type": fact_type,
                            "value": matches[0].group(1),
                        })
                        break  # Only take first result per message
                
                if shareable["extracted_facts"]:
                    break  # Stop after finding facts in most recent assistant message
        
        # Store in session shared context
        self._session_shared_context.append(shareable)
        
        # Track this agent in metadata
        if agent_name not in self._session_metadata["agents_used"]:
            self._session_metadata["agents_used"].append(agent_name)
        
        # Keep only last 5 agents' context to prevent unbounded growth
        if len(self._session_shared_context) > 5:
            self._session_shared_context = self._session_shared_context[-5:]
    
    def get_shared_context_injection(self) -> str:
        """Get formatted shared context to inject into system prompt.
        
        Returns:
            Formatted string containing shared session context from previous agents
        """
        if not self._session_shared_context:
            return ""
        
        context_lines = ["\n## SHARED SESSION CONTEXT"]
        context_lines.append(f"You are agent #{len(self._session_metadata['agents_used']) + 1} in this session.")
        
        if self._session_metadata["agents_used"]:
            context_lines.append(f"Previous agents: {', '.join(self._session_metadata['agents_used'])}")
        
        # Include context from last 3 agents only
        for ctx in self._session_shared_context[-3:]:
            context_lines.append(f"\n### {ctx['agent_name']}:")
            
            # Include last 2 exchanges per agent
            for exchange in ctx["key_exchanges"][-2:]:
                context_lines.append(f"- User: {exchange['user']}")
                context_lines.append(f"- Response: {exchange['assistant']}")
            
            # Include extracted facts
            if ctx["extracted_facts"]:
                facts_str = ", ".join([f"{f['type']}={f['value']}" for f in ctx["extracted_facts"][:5]])
                context_lines.append(f"- Key facts: {facts_str}")
        
        try:
            from cai.util.session_compact import shared_context_supplement

            supplement = shared_context_supplement()
            if supplement:
                context_lines.append(supplement)
        except Exception:
            pass

        return "\n".join(context_lines) + "\n"
    
    def clear_session_context(self) -> None:
        """Clear all session shared context. Used when starting a new session."""
        self._session_shared_context.clear()
        self._session_metadata["agents_used"].clear()


# Global instance
AGENT_MANAGER = SimpleAgentManager()
