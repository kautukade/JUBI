"""
Meta Agent Controller for TUI - Async-optimized version

This module provides a hidden meta-agent that operates above the TUI,
orchestrating workflows and managing multiple agents through command execution.
Only active when CAI_META_AGENT=True.

IMPORTANT: This version is optimized to prevent UI freezing with proper async handling.
"""

import os
import asyncio
import json
import time
from typing import Dict, List, Optional, Any, Callable
import litellm
from dataclasses import dataclass
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor

# Only activate if CAI_META_AGENT=True
META_AGENT_ENABLED = os.getenv("CAI_META_AGENT", "false").lower() == "true"

# Create a thread pool for blocking operations
executor = ThreadPoolExecutor(max_workers=2)

# Meta Agent System Prompt with comprehensive command documentation
META_AGENT_SYSTEM_PROMPT = """You are the Meta Agent, a hidden orchestrator that operates above the CAI TUI (Terminal User Interface).

Your PRIMARY roles are:
1. Route prompts to appropriate agents (DEFAULT: redteam_agent)
2. REFORMULATE prompts for each agent to maximize their effectiveness
3. Create CONVERGENT strategies where different agents work toward the same goal with specialized approaches

CRITICAL RULES:
1. DEFAULT ACTION: Use redteam_agent unless explicitly requested otherwise
2. DO NOT change models - the system uses CAI_MODEL automatically
3. ALWAYS reformulate the user's prompt for the specific agent's capabilities
4. When using multiple agents, give each a DIFFERENT but COMPLEMENTARY prompt

PROMPT REFORMULATION STRATEGY:
For each agent, create a specialized prompt that:
- Leverages that agent's specific strengths
- Approaches the problem from that agent's perspective
- Converges toward the overall objective
- Is different from prompts sent to other agents

## Available Agents (use with /agent or /parallel add):
Based on the actual agents in the system:
1. blueteam_agent - Defensive security analysis
2. bug_bounty_agent - Bug bounty hunting 
3. cybersecurity-engineer - Cybersecurity engineering and architecture
4. dfir_agent - Digital forensics and incident response
5. flag_discriminator - Flag discrimination
6. one_tool_agent - Basic agent with single tool execution (DEFAULT)
7. dns_smtp_agent - DNS and SMTP analysis
8. memory_analysis_agent - Memory analysis
9. network_security_analyzer_agent - Network security analysis
10. redteam_agent - Offensive security testing (RECOMMENDED DEFAULT)
11. replay_attack_agent - Replay attack analysis
12. reporting_agent - Report generation
13. retesting_agent - Retesting vulnerabilities
14. reverse_engineering_agent - Reverse engineering
15. subghz_sdr_agent - SubGHz and SDR analysis
16. thought_agent - Reasoning and analysis
17. use_case_agent - Use case analysis
18. wifi_security_agent - WiFi security
19. bb_triage_swarm_pattern - Bug bounty triage swarm
20. redteam_swarm_pattern - Red team swarm
21. offsec_pattern - Offensive security pattern
22. blue_team_red_team_shared_context - Blue/red team collaboration
23. blue_team_red_team_split_context - Blue/red team split analysis

IMPORTANT: Use exact agent names as listed above!

## Agent-Specific Prompt Reformulation Examples:

User: "test the security of 192.168.1.1"

Reformulated prompts for different agents:
- redteam_agent: "Perform comprehensive penetration testing on 192.168.1.1. Focus on identifying vulnerabilities, misconfigurations, and potential attack vectors. Use aggressive scanning techniques."
- blueteam_agent: "Analyze defensive measures and security posture of 192.168.1.1. Identify security controls, monitoring capabilities, and recommend hardening strategies."
- network_security_analyzer_agent: "Conduct deep network analysis of 192.168.1.1. Map open ports, services, protocols, and network topology. Identify network-level vulnerabilities."
- bug_bounty_agent: "Hunt for high-value vulnerabilities on 192.168.1.1 following bug bounty methodology. Focus on OWASP Top 10, authentication bypasses, and critical findings."

User: "analyze this application"

Reformulated prompts:
- redteam_agent: "Execute application penetration test focusing on authentication, authorization, injection flaws, and business logic vulnerabilities."
- reverse_engineering_agent: "Reverse engineer the application to understand its architecture, identify hardcoded secrets, and analyze binary protections."
- reporting_agent: "Document all security findings with clear risk ratings, proof-of-concept code, and remediation recommendations."

IMPORTANT: Each prompt must be DIFFERENT but work toward the SAME security objective!

## Complete Command Reference:

### Agent Management
- /agent [agent_name] or /a - Switch between agents or list all
- /parallel <agent1> [agent2]... or /p - Execute multiple agents in parallel
  - /parallel add <agent> - Add agent to parallel execution
  - /parallel remove <agent> - Remove agent from parallel
  - /parallel list - Show current parallel agents
  - /parallel results - Gather results from all parallel agents
  - /parallel status - Show running parallel agents
  - /parallel stop - Stop all parallel executions
  - /parallel focus <agent> - Focus on specific agent output
  - /parallel switch <agent> - Switch primary agent context

### Model Management (DO NOT USE - System manages CAI_MODEL automatically)
- Models are controlled by CAI_MODEL environment variable
- Meta Agent should NEVER change models

### History and Memory
- /history [number] [agent_name] or /h - Display conversation history
- /flush [agent_name|all] - Clear agent message history
- /load <filename> or /l - Load conversation JSONL (e.g. from /save *.jsonl or session logs)
- /save <filename> - Save all agent histories: use .jsonl for /load, .md for a readable Markdown report
 - /replay <jsonl> [delay] - Replay a JSONL in this terminal
 - /replay stop - Cancel an active replay in this terminal
- /memory [subcommand] or /mem - Memory management:
  - list - Show all saved memories
  - save [name] - Save current conversation as memory
  - apply <memory_id> - Apply memory to current agent
  - show <memory_id> - Display memory content
  - delete <memory_id> - Remove memory
  - merge <id1> <id2> [name] - Combine memories
  - compact - AI-powered memory summarization
  - status - Show memory system status

### Utilities
- /cost [agent_name] - Show API usage costs and tokens
- /help [command] or /? - Get help for commands
- /env - Show environment variables
- /shell or $ - Execute shell commands (e.g. kill <PID> to signal a host process)
- /exit or /quit - Exit the TUI

### MCP Integration
- /mcp load <config_file> - Load MCP server configuration
- /mcp list - Show configured servers
- /mcp tools [server_name] - List available tools
- /mcp status - Show server connection status

### Terminal Control (TUI-specific)
- T<num>:<command> - Send command to specific terminal (e.g., T2:/model gpt-4)
- /add - Add a new terminal
- /remove T<num> - Remove a terminal
- /focus T<num> - Focus on a terminal

## Simplified Workflow:
1. DEFAULT to redteam_agent for all security tasks
2. Only change agents when explicitly requested
3. Do NOT manage models - system handles this

## Common Patterns:
- Default security work: redteam_agent (handles most tasks)
- If user asks for defense: /agent blueteam_agent (only if explicitly requested)
- For reporting: /agent reporting_agent (only if explicitly requested)
- Parallel is rarely needed - use single agents

IMPORTANT: You intercept ALL user prompts. For each prompt:

1. Analyze what the user wants to do
2. Use the execute_workflow function with:
   - workflow_type: "single_agent" for one agent, "parallel_agents" for multiple
   - agents: Array of agents with their reformulated prompts
   - description: Clear description of the workflow

3. ALWAYS use execute_workflow in a SINGLE function call:
   - For single agent: workflow_type="single_agent", agents=[{"name": "redteam_agent", "prompt": "..."}]
   - For multiple agents: workflow_type="parallel_agents", agents=[{"name": "agent1", "prompt": "..."}, {"name": "agent2", "prompt": "..."}]

4. CRITICAL: Everything must be in ONE function call. The system will handle all command sequencing automatically.

Examples using execute_workflow:

User: "test the security of this network"
→ execute_workflow({
    "workflow_type": "single_agent",
    "agents": [{
        "name": "redteam_agent",
        "prompt": "Perform comprehensive penetration testing on the target network. Enumerate all hosts, scan for open ports and services, identify vulnerabilities, and attempt safe exploitation. Focus on network-level attacks, misconfigurations, and lateral movement opportunities."
    }],
    "description": "Network security testing with redteam_agent"
})

User: "analyze security from both red and blue team perspectives"
→ execute_workflow({
    "workflow_type": "parallel_agents",
    "agents": [
        {
            "name": "redteam_agent",
            "prompt": "Conduct aggressive security assessment focusing on exploitation paths, privilege escalation vectors, and data exfiltration possibilities. Identify weaknesses an attacker would target."
        },
        {
            "name": "blueteam_agent",
            "prompt": "Evaluate defensive controls, detection capabilities, and incident response readiness. Identify gaps in monitoring, logging, and security architecture. Recommend hardening measures."
        }
    ],
    "description": "Dual perspective security analysis with offensive and defensive viewpoints"
})

User: "comprehensive security test with network, app and infra"
→ execute_workflow({
    "workflow_type": "parallel_agents",
    "agents": [
        {
            "name": "network_security_analyzer_agent",
            "prompt": "Perform deep network security analysis. Map all network services, identify exposed ports, analyze traffic patterns, and detect network-level vulnerabilities."
        },
        {
            "name": "redteam_agent",
            "prompt": "Focus on application-layer attacks. Test for injection vulnerabilities, authentication bypasses, session management flaws, and business logic issues."
        },
        {
            "name": "bug_bounty_agent",
            "prompt": "Hunt for infrastructure vulnerabilities. Check for misconfigurations, outdated services, weak credentials, and privilege escalation paths."
        }
    ],
    "description": "Comprehensive security assessment across network, application, and infrastructure layers"
})

IMPORTANT: Each reformulated_prompt must be UNIQUE and leverage the specific agent's strengths!

BE CONCISE. Only output commands if agent switching is needed."""

# Tool definitions for command execution
EXECUTE_WORKFLOW_TOOL = {
    "type": "function",
    "function": {
        "name": "execute_workflow",
        "description": "Execute a complete workflow with multiple commands and agent-specific prompts in a single operation",
        "parameters": {
            "type": "object",
            "properties": {
                "workflow_type": {
                    "type": "string",
                    "description": "Type of workflow: 'single_agent', 'parallel_agents', or 'sequential_agents'"
                },
                "agents": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": {
                                "type": "string",
                                "description": "Agent name (e.g., 'redteam_agent')"
                            },
                            "prompt": {
                                "type": "string",
                                "description": "Reformulated prompt specific to this agent's capabilities"
                            }
                        },
                        "required": ["name", "prompt"]
                    },
                    "description": "List of agents with their specific prompts"
                },
                "description": {
                    "type": "string",
                    "description": "Overall description of what this workflow accomplishes"
                }
            },
            "required": ["workflow_type", "agents", "description"]
        }
    }
}

@dataclass
class MetaAgentCommand:
    """Represents a command to be executed by the meta agent"""
    command: str
    purpose: str
    reformulated_prompt: Optional[str] = None
    executed: bool = False
    result: Optional[str] = None
    timestamp: Optional[datetime] = None

class MetaAgentController:
    """Controller for the Meta Agent functionality - Async optimized"""
    
    def __init__(self):
        self.enabled = META_AGENT_ENABLED
        if not self.enabled:
            return
            
        # Use CAI_MODEL as default, fallback to CAI_META_MODEL, then to gpt-4o-mini
        self.model = os.getenv("CAI_META_MODEL", os.getenv("CAI_MODEL", "gpt-4o-mini"))
        self.message_history: List[Dict[str, Any]] = []
        self.execution_history: List[MetaAgentCommand] = []
        self.agent_contexts: Dict[str, Any] = {}
        self.current_plan: Optional[Dict[str, Any]] = None
        self.tui_app_ref = None  # Reference to TUI app for command execution
        self._processing = False  # Flag to prevent concurrent processing
        self._command_queue = asyncio.Queue()  # Queue for commands
        self._output_analysis_queue = asyncio.Queue()  # Queue for output analysis
        # Track agent/parallel operations to avoid duplicates and noisy UI
        self._last_selected_agent: Optional[str] = None
        self._parallel_added_names: set[str] = set()
        # Lightweight activity log for TUI visualizations
        self._activity_log: list[dict] = []  # {ts, kind, message}
        
        # Add system prompt to history
        self.message_history.append({
            "role": "system",
            "content": META_AGENT_SYSTEM_PROMPT
        })
        
        # Workers will be started when first needed
        self._workers_started = False
        
        # Debug info storage
        self._last_litellm_debug = {}

        # TUI global state and context merging
        self._tui_hooks_attached = False
        self._terminal_idle_since: Dict[str, float] = {}
        self._global_context_summary: List[Dict[str, Any]] = []
        self._context_limit = 40  # max merged messages to keep
        self._auto_close_grace = float(os.getenv("CAI_META_AUTOCLOSE_GRACE", "1.5"))
        # Make LiteLLM drop unsupported params automatically (e.g., temperature for gpt-5/o1)
        try:
            if hasattr(litellm, "drop_params"):
                litellm.drop_params = True
        except Exception:
            pass
        # Track terminals being closed to avoid races and duplicate operations
        self._closing_terminals: set[str] = set()
        # Keep references to background tasks for clean shutdown
        self._watcher_task: Optional[asyncio.Task] = None
    
    def set_tui_app(self, tui_app):
        """Set reference to TUI app for command execution"""
        self.tui_app_ref = tui_app
        # Attach hooks once we get the app
        try:
            if self.enabled and not self._tui_hooks_attached:
                self._attach_tui_hooks()
        except Exception:
            pass

    def _attach_tui_hooks(self) -> None:
        """Attach TUI event hooks and start watchers for state management."""
        if self._tui_hooks_attached or not self.tui_app_ref:
            return
        try:
            from cai.tui.patterns.observer import terminal_event_manager, EventType

            # Lightweight callbacks to update idle timers
            def _on_event(event):
                et = getattr(event, "event_type", None)
                tid = getattr(event, "terminal_id", None)
                if not tid:
                    return
                now = time.time()
                # Reset idle on output/command
                if et in (EventType.TERMINAL_OUTPUT, EventType.TERMINAL_COMMAND):
                    self._terminal_idle_since.pop(tid, None)
                elif et in (EventType.TERMINAL_CLEARED, EventType.TERMINAL_CONFIGURED):
                    self._terminal_idle_since[tid] = now
                elif et == EventType.TERMINAL_REMOVED:
                    self._terminal_idle_since.pop(tid, None)
                    # Best-effort cancellation of any activity tied to this terminal
                    asyncio.create_task(self._on_terminal_removed_async(tid))

            terminal_event_manager.attach_callback(_on_event, EventType.TERMINAL_OUTPUT)
            terminal_event_manager.attach_callback(_on_event, EventType.TERMINAL_COMMAND)
            terminal_event_manager.attach_callback(_on_event, EventType.TERMINAL_CLEARED)
            terminal_event_manager.attach_callback(_on_event, EventType.TERMINAL_CONFIGURED)
            terminal_event_manager.attach_callback(_on_event, EventType.TERMINAL_REMOVED)

            # Start watcher
            self._watcher_task = asyncio.create_task(self._tui_state_watcher())
            self._tui_hooks_attached = True
        except Exception:
            # Fail silently; we can retry later
            pass

    async def _tui_state_watcher(self):
        """Periodically manage TUI: auto-close finished terminals and merge contexts."""
        while True:
            try:
                if not self.tui_app_ref:
                    await asyncio.sleep(0.5)
                    continue

                grid = getattr(self.tui_app_ref, "terminal_grid", None)
                if not grid:
                    await asyncio.sleep(0.5)
                    continue

                # Iterate non-main terminals
                for tid, term in list(getattr(grid, "terminals", {}).items()):
                    if tid == getattr(grid, "main_terminal_id", None):
                        continue

                    # Determine running/streaming state
                    is_running = getattr(term, "is_running", False)
                    action_bar = getattr(term, "action_bar", None)
                    is_streaming = bool(getattr(action_bar, "_is_streaming", False)) if action_bar else False

                    # Check terminal queue emptiness if available
                    queue_empty = True
                    try:
                        from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                        if TERMINAL_QUEUE_MANAGER and hasattr(TERMINAL_QUEUE_MANAGER, "get_queue_status"):
                            st = TERMINAL_QUEUE_MANAGER.get_queue_status(term.terminal_number)
                            # Expecting dict with size or pending
                            if st and (st.get("size") or st.get("pending")):
                                queue_empty = (st.get("size", 0) == 0 and not st.get("pending", False))
                    except Exception:
                        pass

                    # If fully idle, start/maintain idle timer
                    if not is_running and not is_streaming and queue_empty:
                        if tid not in self._terminal_idle_since:
                            self._terminal_idle_since[tid] = time.time()
                        # If grace elapsed, merge and close
                        if time.time() - self._terminal_idle_since[tid] > self._auto_close_grace:
                            if tid in self._closing_terminals:
                                continue
                            self._closing_terminals.add(tid)
                            # Merge and cancel any running activity for this terminal
                            await self._merge_context_for_terminal(term)
                            await self._cancel_terminal_activity(term)
                            # Finally, remove from UI
                            try:
                                grid.remove_terminal(tid)
                            except Exception:
                                pass
                            # Clean up timer
                            self._terminal_idle_since.pop(tid, None)
                            self._closing_terminals.discard(tid)
                    else:
                        # Reset idle timer when active
                        self._terminal_idle_since.pop(tid, None)

                await asyncio.sleep(0.5)
            except asyncio.CancelledError:
                break
            except Exception:
                # Keep watcher alive on errors
                await asyncio.sleep(0.5)

    async def _merge_context_for_terminal(self, terminal) -> None:
        """Merge the finished terminal's agent context into a global concise summary."""
        try:
            agent_name = getattr(getattr(terminal, "agent", None), "name", None)
            if not agent_name:
                return
            # Pull history from AGENT_MANAGER
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            hist = AGENT_MANAGER.get_message_history(agent_name) or []
            if not hist:
                return
            # Compact: keep last 6 messages (user/assistant/tool) with command hints
            compact = self._compact_history(hist, keep=6)
            # Append to global summary with agent tag
            self._global_context_summary.append({
                "agent": agent_name,
                "timestamp": datetime.now().isoformat(timespec="seconds"),
                "summary": compact,
            })
            # Trim global summary to limit
            if len(self._global_context_summary) > self._context_limit:
                self._global_context_summary = self._global_context_summary[-self._context_limit:]
            # Optionally show a brief status
            await self._show_status(f"Merged context from {agent_name} (auto-close)", "complete")
        except Exception:
            pass

    def _compact_history(self, history: List[Dict[str, Any]], keep: int = 6) -> List[Dict[str, Any]]:
        """Return a compact representation of the tail of a history, annotating commands without over-bloating."""
        tail = history[-keep:]
        compact = []
        for msg in tail:
            role = msg.get("role")
            content = msg.get("content", "")
            # Detect command-like lines (best-effort)
            is_cmd = False
            if isinstance(content, str) and (content.strip().startswith("$") or content.strip().startswith("/")):
                is_cmd = True
            if isinstance(content, str):
                # Trim long content
                content = content.strip().splitlines()[0][:180]
            compact.append({
                "role": role,
                "text": content,
                "cmd": is_cmd,
            })
        return compact

    def get_merged_context(self) -> List[Dict[str, Any]]:
        """Expose the merged cross-agent context (for debugging or UI display)."""
        return list(self._global_context_summary)

    async def _on_terminal_removed_async(self, terminal_id: str) -> None:
        """Handle terminal removal event: cancel tasks, stop streaming, and flush queues safely."""
        try:
            if not self.tui_app_ref or not hasattr(self.tui_app_ref, 'session_manager'):
                return
            grid = getattr(self.tui_app_ref, 'terminal_grid', None)
            session_manager = self.tui_app_ref.session_manager
            if not grid or not session_manager:
                return
            # Find terminal widget by id (may no longer be present)
            term = None
            try:
                term = grid.terminals.get(terminal_id)
            except Exception:
                term = None
            # If widget is gone, try to locate runner by matching terminal_id
            term_number = getattr(term, 'terminal_number', None)
            if term_number is None:
                try:
                    for num, runner in session_manager.terminal_runners.items():
                        if getattr(runner.config, 'terminal_id', None) == terminal_id:
                            term_number = num
                            term = getattr(runner, 'terminal', None)
                            break
                except Exception:
                    pass
            # Cancel tasks and stop streaming for this terminal
            if term_number is not None:
                await self._safe_cancel_for_terminal(session_manager, term_number, term)
        except Exception:
            pass

    async def _cancel_terminal_activity(self, terminal) -> None:
        """Cancel any activity tied to a terminal before closing it."""
        try:
            if not self.tui_app_ref or not hasattr(self.tui_app_ref, 'session_manager'):
                return
            session_manager = self.tui_app_ref.session_manager
            term_number = getattr(terminal, 'terminal_number', None)
            await self._safe_cancel_for_terminal(session_manager, term_number, terminal)
        except Exception:
            pass

    async def _safe_cancel_for_terminal(self, session_manager, term_number: Optional[int], terminal) -> None:
        """Helper to cancel current task, stop streaming and flush queue for a specific terminal."""
        try:
            if term_number in getattr(session_manager, 'terminal_runners', {}):
                runner = session_manager.terminal_runners[term_number]
                # Cancel the current task if running
                try:
                    await runner.cancel_current_task()
                except Exception:
                    pass
            # Stop any streaming on the action bar
            try:
                if terminal and hasattr(terminal, 'action_bar') and getattr(terminal.action_bar, '_is_streaming', False):
                    terminal.action_bar.stop_streaming()
            except Exception:
                pass
            # Flush or mark queue completed
            try:
                from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                if term_number is not None:
                    await TERMINAL_QUEUE_MANAGER.mark_completed(term_number)
            except Exception:
                pass
        except Exception:
            pass
    
    async def _ensure_workers_started(self):
        """Ensure background workers are started"""
        if not self._workers_started:
            try:
                # Check if we're in an event loop
                loop = asyncio.get_running_loop()
                if loop:
                    await self._tui_debug("[Meta Agent] Starting background workers...")
                    # Start workers
                    asyncio.create_task(self._command_worker())
                    asyncio.create_task(self._output_analysis_worker())
                    self._workers_started = True
                    await self._tui_debug("[Meta Agent] Workers started successfully")
            except RuntimeError as e:
                # No event loop, workers will start later
                await self._tui_debug(f"[Meta Agent] Workers not started: {str(e)}")
                pass
    
    async def _command_worker(self):
        """Background worker to process workflows without blocking UI"""
        while True:
            try:
                workflow_data = await self._command_queue.get()
                if workflow_data is None:  # Shutdown signal
                    break
                
                workflow_type = workflow_data.get("workflow_type")
                
                if workflow_type == "single_agent":
                    # Execute single agent workflow
                    agent_name = workflow_data.get("agent_name", "redteam_agent")
                    prompt = workflow_data.get("prompt", "")
                    
                    await self._show_status(f"Activating {agent_name}", "executing")
                    
                    # Switch to the agent
                    # Avoid redundant agent switches
                    if self._last_selected_agent != agent_name:
                        await self._execute_tui_command(f"/agent {agent_name}")
                        self._last_selected_agent = agent_name
                    else:
                        await self._tui_debug(f"[Meta Agent] Skipping agent switch (already {agent_name})")
                    await asyncio.sleep(0.5)
                    
                    # Send the reformulated prompt
                    if prompt:
                        await self._show_reformulated_prompt(agent_name, prompt)
                        await self._execute_tui_command(prompt)
                        await self._show_status(f"Request dispatched to {agent_name}", "complete")
                        
                elif workflow_type == "parallel_agents":
                    # Execute parallel agents workflow
                    agents = workflow_data.get("agents", [])
                    
                    if agents:
                        # Clear parallel list first
                        await self._show_status("Setting up parallel execution", "executing")
                        await self._execute_tui_command("/parallel clear")
                        # Reset tracking set
                        self._parallel_added_names.clear()
                        await asyncio.sleep(0.3)
                        
                        # Deduplicate while preserving order
                        seen = set()
                        unique_agents = []
                        for agent in agents:
                            name = agent.get("name")
                            if name and name not in seen:
                                seen.add(name)
                                unique_agents.append(agent)

                        # Add all unique agents (and avoid re-adding already tracked ones)
                        for agent in unique_agents:
                            agent_name = agent.get("name")
                            if not agent_name:
                                continue
                            if agent_name in self._parallel_added_names:
                                await self._tui_debug(f"[Meta Agent] Skipping duplicate parallel add for {agent_name}")
                                continue
                            await self._show_status(f"Adding {agent_name} to parallel execution", "parallel")
                            await self._execute_tui_command(f"/parallel add {agent_name}")
                            self._parallel_added_names.add(agent_name)
                            await asyncio.sleep(0.3)
                        
                        # Run all agents
                        await self._show_status("Executing all parallel agents", "executing")
                        await self._execute_tui_command("/parallel run")
                        await asyncio.sleep(1.0)
                        
                        # Send prompts to each agent
                        for i, agent in enumerate(agents):
                            agent_name = agent.get("name")
                            prompt = agent.get("prompt")
                            
                            if prompt:
                                await self._show_reformulated_prompt(agent_name, prompt)
                                # For parallel mode, we might need to prefix with terminal number
                                # For now, just send the prompt
                                await self._execute_tui_command(prompt)
                                await asyncio.sleep(0.5)
                        
                        await self._show_status(f"All {len(agents)} agents activated with optimized prompts", "complete")
                
                else:
                    # Legacy single command support
                    command = workflow_data.get("command")
                    if command:
                        await self._execute_tui_command(command)
                
                # Small delay between workflows
                await asyncio.sleep(0.5)
                
            except Exception as e:
                await self._show_status(f"Error: {str(e)[:100]}", "error")
                pass
    
    async def _output_analysis_worker(self):
        """Background worker to analyze outputs without blocking UI"""
        while True:
            try:
                analysis_data = await self._output_analysis_queue.get()
                if analysis_data is None:  # Shutdown signal
                    break
                    
                agent_name = analysis_data["agent_name"]
                output = analysis_data["output"]
                
                # Analyze in thread pool to avoid blocking
                loop = asyncio.get_event_loop()
                await loop.run_in_executor(
                    executor,
                    self._sync_analyze_output,
                    agent_name,
                    output
                )
                
            except Exception:
                # Silently continue on errors
                pass
    
    def _sync_analyze_output(self, agent_name: str, output: str):
        """Synchronous version of output analysis for thread pool"""
        # Skip very short outputs
        if len(output) < 50:
            return
            
        try:
            # Quick analysis without blocking
            if "error" in output.lower() or "failed" in output.lower():
                self.update_agent_context(agent_name, {"has_errors": True})
            elif "success" in output.lower() or "completed" in output.lower():
                self.update_agent_context(agent_name, {"has_success": True})
        except:
            pass
    
    async def process_user_request_async(self, user_input: str) -> Optional[Dict[str, Any]]:
        """Process user request in background without blocking UI"""
        if not self.enabled or self._processing:
            return None
        
        # Ensure workers are started
        await self._ensure_workers_started()
            
        # Mark as processing
        self._processing = True
        
        try:
            # Process in background
            task = asyncio.create_task(self._process_request_background(user_input))
            
            # Return immediately to avoid blocking
            return {"status": "processing", "message": "Meta Agent analyzing request..."}
            
        finally:
            self._processing = False
    
    async def _process_request_background(self, user_input: str):
        """Background processing of user request"""
        try:
            # Show analyzing status
            await self._show_status(f"Analyzing request: \"{user_input[:50]}{'...' if len(user_input) > 50 else ''}\"", "analyzing")
            
            # Add user message to history
            self.message_history.append({
                "role": "user",
                "content": user_input
            })
            
            # Use thread pool for LiteLLM call to avoid blocking
            loop = asyncio.get_event_loop()
            
            # Check if we have API keys
            has_openai = bool(os.getenv("OPENAI_API_KEY"))
            has_anthropic = bool(os.getenv("ANTHROPIC_API_KEY"))
            
            if not has_openai and not has_anthropic:
                await self._show_status("No API keys configured. Please set OPENAI_API_KEY or ANTHROPIC_API_KEY", "error")
                return
            
            response = await loop.run_in_executor(
                executor,
                self._sync_litellm_completion,
                self.message_history,
                user_input
            )
            
            if response:
                await self._tui_debug(f"[Meta Agent] Got response with {len(response.get('commands', []))} commands")
            else:
                await self._tui_debug("[Meta Agent] No response from LiteLLM")
                # Check debug info
                if hasattr(self, '_last_litellm_debug'):
                    error = self._last_litellm_debug.get('error')
                    if error:
                        await self._tui_debug(f"[Meta Agent] Error details: {error[:100]}...")
            
            if response:
                # Check if there's a workflow to execute
                workflow = response.get("workflow")
                
                if workflow:
                    workflow_type = workflow.get("type", "single_agent")
                    agents = workflow.get("agents", [])
                    description = workflow.get("description", "")
                    
                    # Show workflow description
                    await self._show_status(description, "analyzing")
                    
                    if workflow_type == "single_agent" and agents:
                        # Single agent workflow
                        agent = agents[0]
                        agent_name = agent.get("name", "redteam_agent")
                        prompt = agent.get("prompt", user_input)
                        
                        await self._show_status(f"Routing to {agent_name}", "routing")
                        await self._show_status(f"Optimizing prompt for {agent_name}", "reformulating")
                        
                        # Queue the complete workflow
                        await self._command_queue.put({
                            "workflow_type": "single_agent",
                            "agent_name": agent_name,
                            "prompt": prompt
                        })
                        
                    elif workflow_type == "parallel_agents" and agents:
                        # Parallel agents workflow
                        await self._show_status(f"Orchestrating {len(agents)} agents for comprehensive analysis", "parallel")
                        
                        # Show all agents being prepared
                        for agent in agents:
                            await self._show_status(f"Preparing {agent['name']}", "reformulating")
                        
                        # Queue the complete workflow
                        await self._command_queue.put({
                            "workflow_type": "parallel_agents",
                            "agents": agents
                        })
                    
                    # Don't forward original prompt - workflow will handle everything
                    return
                
                # No workflow - forward the original prompt
                await self._show_status("Using current agent configuration", "info")
                await self._command_queue.put({
                    "command": user_input,
                    "purpose": "User prompt",
                    "reformulated_prompt": ""
                })
            else:
                # No response from Meta Agent - just forward the prompt
                await self._tui_debug("[Meta Agent] No commands needed - forwarding prompt directly")
                await self._command_queue.put({
                    "command": user_input,
                    "purpose": "User prompt (direct)"
                })
                    
        except Exception as e:
            error_msg = str(e)
            if "connection error" in error_msg.lower() or "api" in error_msg.lower():
                await self._show_status("API connection failed. Check your API keys.", "error")
            elif "model" in error_msg.lower():
                await self._show_status(f"Model error: {self.model} may not be available", "error")
            else:
                await self._show_status(f"Error: {error_msg[:100]}", "error")
            
            # Forward the prompt anyway to avoid blocking the user
            try:
                await self._show_status("Forwarding to current agent due to error", "info")
                await self._command_queue.put({
                    "command": user_input,
                    "purpose": "User prompt (error recovery)"
                })
            except:
                pass
    
    def _sync_litellm_completion(self, messages, user_input=None):
        """Synchronous LiteLLM completion for thread pool"""
        try:
            # Store debug info for TUI display
            self._last_litellm_debug = {
                "model": self.model,
                "message_count": len(messages),
                "timestamp": datetime.now().isoformat()
            }
            
            # Use non-async version
            # Some providers/models (e.g., gpt-5/o1) only allow temperature=1.
            # Respect litellm.drop_params if set; otherwise, adapt temperature based on model.
            temp = 0.7
            try:
                model_lower = str(self.model).lower()
                if any(flag in model_lower for flag in ["gpt-5", "o1", "o3"]):
                    temp = 1
            except Exception:
                pass

            response = litellm.completion(
                model=self.model,
                messages=messages,
                tools=[EXECUTE_WORKFLOW_TOOL],
                tool_choice="auto",
                temperature=temp,
                timeout=10  # 10 second timeout
            )
            
            assistant_message = response.choices[0].message
            
            # Store successful response info
            self._last_litellm_debug["success"] = True
            self._last_litellm_debug["has_tool_calls"] = hasattr(assistant_message, 'tool_calls') and bool(assistant_message.tool_calls)
            
            # Process tool calls
            if hasattr(assistant_message, 'tool_calls') and assistant_message.tool_calls:
                for tool_call in assistant_message.tool_calls:
                    if tool_call.function.name == "execute_workflow":
                        args = json.loads(tool_call.function.arguments)
                        workflow_type = args.get("workflow_type", "single_agent")
                        agents = args.get("agents", [])
                        description = args.get("description", "")
                        
                        self._last_litellm_debug["workflow_type"] = workflow_type
                        self._last_litellm_debug["agent_count"] = len(agents)
                        
                        return {
                            "workflow": {
                                "type": workflow_type,
                                "agents": agents,
                                "description": description
                            }
                        }
            
            # No tool calls - Meta Agent decided no action needed
            self._last_litellm_debug["no_action_needed"] = True
            return {"commands": []}
            
        except Exception as e:
            # Store error info for TUI display
            self._last_litellm_debug["error"] = str(e)
            self._last_litellm_debug["error_type"] = type(e).__name__
            
            # Check for common errors
            error_str = str(e).lower()
            if "connection error" in error_str or "api" in error_str:
                self._last_litellm_debug["likely_cause"] = "Missing or invalid API key"
            elif "model" in error_str:
                self._last_litellm_debug["likely_cause"] = "Invalid model name"
            elif "timeout" in error_str:
                self._last_litellm_debug["likely_cause"] = "Request timed out"
            
            return None
    
    async def execute_command(self, command: str, purpose: str = "") -> Optional[str]:
        """Queue command for execution without blocking"""
        if not self.enabled:
            return None
        
        # Ensure workers are started
        await self._ensure_workers_started()
            
        # Queue the command
        await self._command_queue.put({
            "command": command,
            "purpose": purpose
        })
        
        return "Command queued for execution"
    
    async def _execute_tui_command(self, command: str) -> str:
        """Internal method to execute TUI commands"""
        await self._tui_debug(f"[Meta Agent] _execute_tui_command called with: {command[:50]}...")
        
        if self.tui_app_ref:
            await self._tui_debug("[Meta Agent] TUI app reference exists")
            try:
                # Check if this is a TUI command or a user prompt
                if command.startswith("/") or command.startswith("$"):
                    # It's a command - execute normally
                    await self._tui_debug(f"[Meta Agent] Executing TUI command: {command}")
                    await self.tui_app_ref._process_command(command)
                else:
                    # It's a user prompt - send to active agent
                    await self._tui_debug(f"[Meta Agent] Forwarding user prompt to agent: {command[:50]}...")
                    
                    # Get the main terminal
                    main_terminal = self.tui_app_ref.terminal_grid.get_main_terminal()
                    if main_terminal:
                        # Display the prompt
                        main_terminal.write_command(command)
                        await self._tui_debug("[Meta Agent] Prompt displayed in terminal")
                    else:
                        await self._tui_debug("[Meta Agent] No main terminal found!")
                    
                    # Execute through session manager
                    if self.tui_app_ref.session_manager:
                        await self._tui_debug("[Meta Agent] Executing through session manager")
                        await self.tui_app_ref.session_manager.execute_command(command)
                    else:
                        await self._tui_debug("[Meta Agent] No session manager found!")
                
                return f"Executed: {command}"
            except Exception as e:
                await self._tui_debug(f"[Meta Agent] Execution error: {str(e)}")
                return f"Error: {str(e)}"
        else:
            await self._tui_debug("[Meta Agent] No TUI app reference!")
            return f"[Simulated] Executed: {command}"
    
    def update_agent_context(self, agent_name: str, context: Dict[str, Any]):
        """Update context for a specific agent"""
        if not self.enabled:
            return
            
        if agent_name not in self.agent_contexts:
            self.agent_contexts[agent_name] = {}
        
        self.agent_contexts[agent_name].update(context)
    
    def get_agent_context(self, agent_name: str) -> Dict[str, Any]:
        """Get context for a specific agent"""
        if not self.enabled:
            return {}
            
        return self.agent_contexts.get(agent_name, {})
    
    def should_intervene(self, current_state: Dict[str, Any]) -> bool:
        """Determine if the meta agent should intervene based on current state"""
        if not self.enabled:
            return False
            
        # Quick checks only - don't do heavy processing here
        command = current_state.get("command", "")
        
        # Check for help requests about workflows
        if command.startswith("/help") and any(word in command for word in ["workflow", "parallel"]):
            return True
            
        return False
    
    async def analyze_agent_output_async(self, agent_name: str, output: str) -> None:
        """Queue output for analysis without blocking"""
        if not self.enabled:
            return
        
        # Ensure workers are started
        await self._ensure_workers_started()
            
        # Queue for background analysis
        await self._output_analysis_queue.put({
            "agent_name": agent_name,
            "output": output
        })
    
    async def _tui_debug(self, message: str):
        """Write debug message to TUI output"""
        if self.tui_app_ref:
            try:
                main_terminal = self.tui_app_ref.terminal_grid.get_main_terminal()
                if main_terminal:
                    # Write debug message in dim style
                    main_terminal.write(f"[dim cyan]{message}[/dim cyan]")
                    # Log to activity
                    self._log_activity("debug", message)
            except:
                pass
    
    async def _show_status(self, message: str, style: str = "info"):
        """Show enhanced status message to user with Rich formatting"""
        if self.tui_app_ref:
            try:
                main_terminal = self.tui_app_ref.terminal_grid.get_main_terminal()
                if main_terminal:
                    # Different styles for different message types
                    if style == "analyzing":
                        formatted = f"\n[bold cyan]🔍 Meta Agent[/bold cyan] [dim white]→[/dim white] [cyan]{message}[/cyan]"
                    elif style == "routing":
                        formatted = f"\n[bold green]🎯 Routing[/bold green] [dim white]→[/dim white] [green]{message}[/green]"
                    elif style == "reformulating":
                        formatted = f"\n[bold yellow]✨ Reformulating[/bold yellow] [dim white]→[/dim white] [yellow]{message}[/yellow]"
                    elif style == "executing":
                        formatted = f"\n[bold magenta]⚡ Executing[/bold magenta] [dim white]→[/dim white] [magenta]{message}[/magenta]"
                    elif style == "parallel":
                        formatted = f"\n[bold blue]🔀 Parallel Mode[/bold blue] [dim white]→[/dim white] [blue]{message}[/blue]"
                    elif style == "complete":
                        formatted = f"\n[bold green]✅ Complete[/bold green] [dim white]→[/dim white] [dim green]{message}[/dim green]"
                    elif style == "error":
                        formatted = f"\n[bold red]❌ Error[/bold red] [dim white]→[/dim white] [red]{message}[/red]"
                    else:
                        formatted = f"\n[bold white]ℹ️  Meta Agent[/bold white] [dim white]→[/dim white] [white]{message}[/white]"
                    
                    main_terminal.write(formatted)
                    # Log to activity
                    self._log_activity(style, message)
            except:
                pass

    def _log_activity(self, kind: str, message: str) -> None:
        """Record a compact activity entry for sidebar visualization."""
        try:
            self._activity_log.append({
                "ts": datetime.now().strftime("%H:%M:%S"),
                "kind": kind,
                "message": message,
            })
            # Keep last 50 entries max
            if len(self._activity_log) > 50:
                self._activity_log = self._activity_log[-50:]
        except Exception:
            pass
    
    async def _show_reformulated_prompt(self, agent_name: str, prompt: str):
        """Show the reformulated prompt in a nice format"""
        if self.tui_app_ref:
            try:
                main_terminal = self.tui_app_ref.terminal_grid.get_main_terminal()
                if main_terminal:
                    # Create a nice box for the reformulated prompt
                    formatted = f"\n[bold yellow]📝 Optimized Prompt for {agent_name}:[/bold yellow]\n"
                    formatted += f"[dim white]┌─────────────────────────────────────────────────────────[/dim white]\n"
                    
                    # Wrap the prompt text nicely
                    import textwrap
                    wrapped = textwrap.wrap(prompt, width=55)
                    for line in wrapped[:3]:  # Show first 3 lines
                        formatted += f"[dim white]│[/dim white] [yellow]{line}[/yellow]\n"
                    
                    if len(wrapped) > 3:
                        formatted += f"[dim white]│[/dim white] [dim yellow]... ({len(wrapped) - 3} more lines)[/dim yellow]\n"
                    
                    formatted += f"[dim white]└─────────────────────────────────────────────────────────[/dim white]"
                    
                    main_terminal.write(formatted)
            except:
                pass
    
    def get_debug_info(self) -> Dict[str, Any]:
        """Get debug information for display"""
        return {
            "enabled": self.enabled,
            "model": self.model,
            "workers_started": self._workers_started,
            "processing": self._processing,
            "command_queue_size": self._command_queue.qsize() if hasattr(self, '_command_queue') else 0,
            "last_litellm_debug": self._last_litellm_debug,
            "last_selected_agent": self._last_selected_agent,
            "parallel_added": list(self._parallel_added_names),
            "activity_log": list(self._activity_log[-5:]),
        }
    
    def reset(self):
        """Reset the meta agent state"""
        if not self.enabled:
            return
            
        self.message_history = [{
            "role": "system",
            "content": META_AGENT_SYSTEM_PROMPT
        }]
        self.execution_history.clear()
        self.agent_contexts.clear()
        self.current_plan = None
        self._processing = False
        self._last_litellm_debug = {}

# Global instance
_meta_agent_controller = None

def get_meta_agent_controller() -> Optional[MetaAgentController]:
    """Get the global meta agent controller instance"""
    global _meta_agent_controller
    
    if _meta_agent_controller is None and META_AGENT_ENABLED:
        _meta_agent_controller = MetaAgentController()
    
    return _meta_agent_controller

# Integration hooks for TUI - Optimized for non-blocking
async def meta_agent_pre_command_hook(command: str, tui_app=None) -> Optional[str]:
    """Hook to be called before executing any TUI command - Non-blocking"""
    controller = get_meta_agent_controller()
    if not controller:
        return None
    
    # Set TUI app reference if provided
    if tui_app and not controller.tui_app_ref:
        controller.set_tui_app(tui_app)
        
    # Quick intervention check only
    if controller.should_intervene({"command": command}):
        # Don't block here - return quickly
        return None
    
    return None

async def meta_agent_post_output_hook(agent_name: str, output: str) -> None:
    """Hook to be called after an agent produces output - Non-blocking"""
    controller = get_meta_agent_controller()
    if not controller:
        return
        
    # Queue for background analysis - doesn't block
    await controller.analyze_agent_output_async(agent_name, output)

def meta_agent_workflow_hook(user_input: str) -> Optional[Dict[str, Any]]:
    """Hook to check if meta agent should create a workflow plan - Fast check"""
    controller = get_meta_agent_controller()
    if not controller:
        return None
        
    # Enhanced workflow triggers - quick string check
    workflow_triggers = [
        "multiple agents",
        "parallel",
        "coordinate",
        "workflow", 
        "orchestrate",
        "both",
        "simultaneously",
        "and then",
        "first", "then",
        "analyze with",
        "scan and report",
        "test with",
        "combine",
        "together"
    ]
    
    # Quick check if input contains workflow triggers
    input_lower = user_input.lower()
    
    # Use any() for early exit on first match
    for trigger in workflow_triggers:
        if trigger in input_lower:
            return {"should_process": True}
    
    return None

# Cleanup function
async def cleanup_meta_agent():
    """Clean up resources when shutting down"""
    controller = get_meta_agent_controller()
    if controller:
        # Signal workers to stop
        await controller._command_queue.put(None)
        await controller._output_analysis_queue.put(None)
