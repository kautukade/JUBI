# Environment Variables Reference

This comprehensive guide documents all environment variables available in CAI, including their purposes, default values, and usage examples.

---

## 🔎 Discovering variables in the REPL

In current CAI releases, you can explore environment variables **from inside the interactive CLI** without leaving the session:


| What you need                                                                        | Command                                                                                                 |
| ------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------- |
| **Numbered list with live values** (what is set *now*)                               | `/env` or `/env list` for extended list of variables                                                    |
| **Full reference tables** (defaults, allowed values, when they apply, extras)        | `/help` — scroll past the quick guide to the tables (`/help topics` lists commands by category only, no env tables) |
| **Long-form help for one variable** (examples, `/env list` index when listed, notes) | `/help var VARIABLE_NAME` (e.g. `/help var CAI_MODEL`)                                                  |


Aliases such as `/h` for `/help` work the same way. This page remains the **canonical web reference**; the REPL output tracks the version you have installed.

---

## 📋 Complete Reference Table


| Variable                   | Description                                                                                                                                                                                                                                                                                                                                                                                      | Default                                        |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------- |
| CTF_NAME                   | Name of the CTF challenge to run (e.g. "picoctf_static_flag")                                                                                                                                                                                                                                                                                                                                    | -                                              |
| CTF_CHALLENGE              | Specific challenge name within the CTF to test                                                                                                                                                                                                                                                                                                                                                   | -                                              |
| CTF_SUBNET                 | Network subnet for the CTF container                                                                                                                                                                                                                                                                                                                                                             | 192.168.3.0/24                                 |
| CTF_IP                     | IP address for the CTF container                                                                                                                                                                                                                                                                                                                                                                 | 192.168.3.100                                  |
| CTF_INSIDE                 | Whether to conquer the CTF from within container                                                                                                                                                                                                                                                                                                                                                 | true                                           |
| CAI_MODEL                  | Model to use for agents                                                                                                                                                                                                                                                                                                                                                                          | alias1                                         |
| CAI_DEBUG                  | Set debug output level (0: Only tool outputs, 1: Verbose debug output, 2: CLI debug output)                                                                                                                                                                                                                                                                                                      | 1                                              |
| CAI_BRIEF                  | Enable/disable brief output mode                                                                                                                                                                                                                                                                                                                                                                 | false                                          |
| CAI_MAX_TURNS              | Maximum number of turns for agent interactions                                                                                                                                                                                                                                                                                                                                                   | inf                                            |
| CAI_ORCHESTRATION_WORKER_MAX_TURNS | Max ``Runner`` turns for each specialist worker spawned by ``orchestration_agent`` tools (``run_specialist``, ``run_dual_approach_contest``, ``run_parallel_specialists``). Integer 1–32                                                                                                                                                                                                                                                                                                                                                   | 6                                              |
| CAI_ORCHESTRATION_MAS_HINT         | When ``true``, ``orchestration_agent`` may receive one synthetic ``user``-role nudge per ``Runner`` run if the user message looks multi-front but only ``run_specialist`` was invoked (suggests ``run_parallel_specialists`` / contest). Set ``false`` to disable                                                                                                                                                                                                                                                                                                                                         | true                                           |
| CAI_MAX_INTERACTIONS       | Maximum number of interactions (tool calls, agent actions, etc.) allowed in a session. If exceeded, only CLI commands are allowed until increased. If force_until_flag=true, the session will exit                                                                                                                                                                                               | inf                                            |
| CAI_PRICE_LIMIT            | Price limit for the conversation in dollars. If exceeded, only CLI commands are allowed until increased. If force_until_flag=true, the session will exit                                                                                                                                                                                                                                         | 1                                              |
| CAI_TRACING                | Enable/disable OpenTelemetry tracing. When enabled, traces execution flow and agent interactions for debugging and analysis                                                                                                                                                                                                                                                                      | true                                           |
| CAI_AGENT_TYPE             | Registered agent key. Defaults to `orchestration_agent` for default routing plus optional dual-approach contest; use `selection_agent` for the slimmer handoff-only router, or pin a specialist such as `redteam_agent`. Use "/agent" command in CLI to list all available agents                                                                                                                   | orchestration_agent                           |
| CAI_STATE                  | Enable/disable stateful mode. When enabled, the agent will use a state agent to keep track of the state of the network and the flags found                                                                                                                                                                                                                                                       | false                                          |
| CAI_COMPACTED_MEMORY       | When true, inject `/compact` conversation summaries into agent system prompts                                                                                                                                                                                                                                                                                                                      | false                                          |
| CAI_ENV_CONTEXT            | Add environment context, dirs and current env available                                                                                                                                                                                                                                                                                                                                          | true                                           |
| CAI_SUPPORT_MODEL          | Model to use for the support agent                                                                                                                                                                                                                                                                                                                                                               | o3-mini                                        |
| CAI_SUPPORT_INTERVAL       | Number of turns between support agent executions                                                                                                                                                                                                                                                                                                                                                 | 5                                              |
| CAI_STREAM                 | Enable/disable streaming output for LLM inference (token-by-token display). Does NOT affect tool output streaming                                                                                                                                                                                                                                                                                | false                                          |
| CAI_TOOL_STREAM            | Enable/disable streaming output for tool executions (real-time command output). Independent of CAI_STREAM                                                                                                                                                                                                                                                                                        | true                                           |
| CAI_DEBUG_TOOLS_VIZ        | Enable debug output for tool visualization and panel rendering. Shows detailed info about tool call display, deduplication, and streaming state                                                                                                                                                                                                                                                  | false                                          |
| CAI_SHOW_CACHE             | Show cache information and message history list. Displays prompt caching stats and the full message list sent to the model                                                                                                                                                                                                                                                                       | false                                          |
| CAI_TELEMETRY              | Enable/disable telemetry                                                                                                                                                                                                                                                                                                                                                                         | true                                           |
| CAI_PARALLEL               | Number of parallel agent instances to run. When set to values greater than 1, executes multiple instances of the same agent in parallel and displays all results                                                                                                                                                                                                                                 | 1                                              |
| CAI_GUARDRAILS             | Enable/disable security guardrails for agents. When set to "true", applies security guardrails to prevent potentially dangerous outputs and inputs                                                                                                                                                                                                                                               | false                                          |
| CAI_GCTR_NITERATIONS       | Number of tool interactions before triggering GCTR (Generative Cut-The-Rope) analysis in bug_bounter_gctr agent. Only applies when using gctr-enabled agents                                                                                                                                                                                                                                     | 5                                              |
| CAI_ACTIVE_CONTAINER       | Docker container ID where commands should be executed. When set, shell commands and tools execute inside the specified container instead of the host. Automatically set when CTF challenges start (if CTF_INSIDE=true) or when attaching a container via `/virtualization` / `/virt` in the REPL                                                                                                                  | -                                              |
| C99_API_KEY                | API key for C99.nl subdomain discovery service. Required for using the C99 reconnaissance tool for DNS enumeration and subdomain discovery. Obtain from [C99.nl](https://c99.nl)                                                                                                                                                                                                                 | -                                              |
| CAI_TOOL_TIMEOUT           | Override the default timeout for tool command executions in seconds. When set, this value overrides all default timeouts for shell commands and tool executions                                                                                                                                                                                                                                  | varies (10s for interactive, 100s for regular) |
| CAI_IDLE_TIMEOUT           | Maximum seconds a command can produce no output before being terminated. Useful for long-running commands like nmap scans that may have gaps between output lines                                                                                                                                                                                                                                | 100                                            |
| CAI_CTX_TRUNC              | Enable context truncation for large tool outputs. When set to "true", automatically truncates large outputs (>50k chars) to prevent context overflow. JS/HTML/CSS/JSON files get aggressive truncation with preview only. Message history also applies position-based truncation when context exceeds 100k tokens or 60% usage                                                                   | false                                          |
| CAI_DISPLAY_MAX_OUTPUT     | Show full tool output without truncation. When set to "true", displays complete tool output regardless of length. By default (false), outputs longer than 10,000 characters are truncated showing the first 5,000 and last 5,000 characters with "... TRUNCATED ..." in between. Useful for debugging format string exploits, large command outputs, or when you need to see the complete result | false                                          |


---

## 🎯 Quick Reference by Use Case

### 🚀 Getting Started (Essential)

For first-time users, these are the essential variables to configure:

```bash
# Required: Model selection
CAI_MODEL="alias1"                    # or gpt-4o, claude-sonnet-4.5, ollama/qwen2.5:72b

# Recommended: Agent type (default CLI entry is orchestration_agent)
CAI_AGENT_TYPE="orchestration_agent" # breadth-first + specialist tools; selection_agent = handoffs only
# CAI_ORCHESTRATION_WORKER_MAX_TURNS=6   # per-worker turn cap when using orchestration_agent tools
# CAI_ORCHESTRATION_MAS_HINT=true        # optional multi-front nudge for orchestration_agent
# CAI_AGENT_TYPE="redteam_agent"        # pin a specialist when you know the toolkit

# Optional but useful: Cost control
CAI_PRICE_LIMIT="1"                   # Maximum spend in dollars
```

**Related Documentation:**

- [Installation Guide](cai/getting-started/installation.md)
- [Configuration Guide](cai/getting-started/configuration.md)

---

### 🏴 CTF Challenges

For running Capture The Flag challenges in containerized environments:

```bash
# Challenge selection
CTF_NAME="picoctf_static_flag"        # Name of the CTF challenge
CTF_CHALLENGE="web_exploitation_1"    # Specific sub-challenge

# Network configuration
CTF_SUBNET="192.168.3.0/24"          # Container subnet
CTF_IP="192.168.3.100"               # Container IP address

# Execution mode
CTF_INSIDE="true"                     # Run agent inside container
```

**Best Practices:**

- Set `CTF_INSIDE=true` to run the agent inside the challenge container
- Use `CAI_ACTIVE_CONTAINER` to manually specify which container to execute commands in
- Combine with `CAI_STATE=true` to track discovered flags

**Related Documentation:**

- [CTF Benchmarks](benchmarking/jeopardy_ctfs.md)

---

### 🔍 Reconnaissance & OSINT

For reconnaissance tasks using external tools:

```bash
# C99.nl subdomain discovery
C99_API_KEY="your-c99-api-key"        # Enable C99 reconnaissance tool

# Agent configuration for recon
CAI_AGENT_TYPE="redteam_agent"        # Or create custom recon agent
```

**Reconnaissance Tools:**

- **C99 Tool**: Subdomain discovery and DNS enumeration via C99.nl API
- Configure `C99_API_KEY` to enable the C99 reconnaissance tool
- See [Tools Documentation](tools.md) for usage examples

**Related Documentation:**

- [Tools Documentation](tools.md#c99-tool)

---

### 🧠 Compacted memory and state

For carrying forward summarized context after `/compact`:

```bash
# State tracking
CAI_STATE="true"                      # Enable network state tracking

# Inject /compact summaries into new agent prompts
CAI_COMPACTED_MEMORY="true"
```

`CAI_MEMORY` and related Qdrant-style variables are deprecated and ignored by core CAI; use `CAI_COMPACTED_MEMORY` only.

**Related documentation:**

- [Advanced Features](tui/advanced_features.md)

---

### 🛡️ Security & Safety

For enabling security guardrails and controlling agent behavior:

```bash
# Security guardrails
CAI_GUARDRAILS="true"                 # Prevent dangerous commands
CAI_PRICE_LIMIT="1"                   # Maximum cost in dollars
CAI_MAX_INTERACTIONS="inf"            # Maximum allowed interactions

# Debugging & monitoring
CAI_DEBUG="1"                         # 0: minimal, 1: verbose, 2: CLI debug
CAI_TRACING="true"                    # Enable OpenTelemetry tracing
```

**Security Layers:**

- **Guardrails**: Prompt injection detection and command validation
- **Cost Limits**: Prevent runaway API usage
- **Interaction Limits**: Control agent autonomy

**Related Documentation:**

- [Guardrails Documentation](guardrails.md)
- [TUI Advanced Features](tui/advanced_features.md)

---

### ⚡ Performance Optimization

For optimizing output, execution speed, and resource usage:

```bash
# Output control
CAI_BRIEF="true"                      # Concise output mode
CAI_STREAM="false"                    # Disable LLM inference streaming (default: false)
CAI_TOOL_STREAM="true"                # Enable tool output streaming (default: true)

# Context optimization
CAI_ENV_CONTEXT="true"                # Include environment in context
CAI_MAX_TURNS="50"                    # Limit conversation turns
CAI_CTX_TRUNC="true"                  # Truncate large outputs to save context
CAI_DISPLAY_MAX_OUTPUT="false"        # Show full output (set true to disable truncation)

# Tool execution timeout
CAI_TOOL_TIMEOUT="60"                 # Override default command timeouts (in seconds)
CAI_IDLE_TIMEOUT="100"                # Max seconds without output before terminating (default: 100)

# Telemetry
CAI_TELEMETRY="true"                  # Enable usage analytics
```

**Streaming Configuration:**

- `CAI_STREAM`: Controls LLM inference streaming (token-by-token display). Default: `false`
- `CAI_TOOL_STREAM`: Controls tool output streaming (real-time command output). Default: `true`
- These are **independent** - you can have tool streaming enabled while LLM streaming is disabled

**Performance Tips:**

- Enable `CAI_BRIEF` for concise outputs in automated workflows
- Set `CAI_MAX_TURNS` to prevent infinite loops
- Use `CAI_STREAM=false` (default) for faster LLM responses without token-by-token display
- Use `CAI_TOOL_STREAM=true` (default) to see command output in real-time
- Set `CAI_TOOL_TIMEOUT` to control command execution timeouts (default: 10s for interactive, 100s for regular commands)
- Set `CAI_IDLE_TIMEOUT` to control how long a command can run without producing output before being terminated (default: 100s). Increase for slow network scans like nmap
- Enable `CAI_CTX_TRUNC=true` when working with large files (JS/HTML/CSS) to prevent context overflow
- Set `CAI_DISPLAY_MAX_OUTPUT=true` to see full tool output without truncation (useful for debugging format strings, large outputs)

---

### 🔧 Advanced Agent Configuration

For specialized agents and complex workflows:

```bash
# Support agent (meta-reasoning)
CAI_SUPPORT_MODEL="o3-mini"          # Model for support agent
CAI_SUPPORT_INTERVAL="5"             # Turns between support executions

# Parallel execution
CAI_PARALLEL="3"                      # Run 3 agent instances simultaneously

# Specialized agents
CAI_GCTR_NITERATIONS="5"             # For bug_bounty_gctr agent
```

**Specialized Agent Variables:**

- `CAI_GCTR_NITERATIONS`: Controls Cut-The-Rope analysis frequency in GCTR agents
- `CAI_SUPPORT_MODEL`: Meta-agent for strategic planning
- `CAI_PARALLEL`: Swarm-style parallel agent execution

**Related Documentation:**

- [Agents Documentation](agents.md)
- [Teams & Parallel Execution](tui/teams_and_parallel_execution.md)

---

### 🐳 Container & Virtualization

For executing commands inside Docker containers:

```bash
# Container targeting
CAI_ACTIVE_CONTAINER="a1b2c3d4e5f6"  # Docker container ID

# Automatic with CTF
CTF_INSIDE="true"                     # Auto-set CAI_ACTIVE_CONTAINER on CTF start
```

**Container Execution:**

- When `CAI_ACTIVE_CONTAINER` is set, all shell commands execute inside that container
- Automatically configured when starting CTF challenges with `CTF_INSIDE=true`
- Switch containers using `/virtualization` or `/virt` in the REPL

**Related Documentation:**

- [Commands Reference](cai/getting-started/commands.md)

---

### 🖥️ TUI-Specific Configuration

For Terminal User Interface features and workflows:

```bash
# TUI display
CAI_STREAM="true"                     # Enable LLM inference streaming in TUI panels
CAI_TOOL_STREAM="true"                # Enable tool output streaming (default)
CAI_BRIEF="false"                     # Full output for interactive sessions

# TUI workflows
CAI_PARALLEL="1"                      # Usually 1 for TUI, use Teams feature instead
CAI_GUARDRAILS="false"                # Consider enabling for team workflows
```

**TUI Recommendations:**

- Set `CAI_STREAM=true` for better interactive LLM response experience
- Keep `CAI_TOOL_STREAM=true` (default) to see command output in real-time
- Use built-in Teams feature instead of `CAI_PARALLEL`
- Enable `CAI_GUARDRAILS` when coordinating multiple agents

**Related Documentation:**

- [TUI Documentation](tui/tui_index.md)
- [TUI Getting Started](tui/getting_started.md)

---

### 🐛 Debugging & Development

For debugging CAI internals and development:

```bash
# Debug levels
CAI_DEBUG="1"                         # 0: minimal, 1: verbose, 2: CLI debug

# Tool visualization debugging
CAI_DEBUG_TOOLS_VIZ="true"            # Debug tool panel rendering and deduplication

# Cache and message debugging
CAI_SHOW_CACHE="true"                 # Show cache stats and full message history list

# Pricing debugging
CAI_DEBUG_PRICING="1"                 # Log pricing calculations to debug_pricing.txt
```

**Debug Variables Explained:**

- `CAI_DEBUG`: General debug output level (0-2)
- `CAI_DEBUG_TOOLS_VIZ`: Shows detailed info about tool call display, panel rendering, streaming state, and deduplication logic
- `CAI_SHOW_CACHE`: Displays prompt caching statistics and the complete message list sent to the model. Useful for debugging context issues
- `CAI_DEBUG_PRICING`: Writes detailed pricing calculations to `debug_pricing.txt` for cost analysis

**When to Use:**

- Use `CAI_DEBUG_TOOLS_VIZ=true` when tool outputs are not displaying correctly or duplicating
- Use `CAI_SHOW_CACHE=true` when debugging context window issues or cache behavior
- Use `CAI_DEBUG_PRICING=1` when investigating cost discrepancies

---

## 💡 Common Configuration Examples

### Example 1: Local Development with Ollama

```bash
CAI_MODEL="ollama/qwen2.5:72b"
CAI_AGENT_TYPE="redteam_agent"
CAI_PRICE_LIMIT="0"
CAI_DEBUG="1"
CAI_GUARDRAILS="false"
```

### Example 2: Production CTF Solving

```bash
CTF_NAME="hackthebox_challenge"
CTF_INSIDE="true"
CAI_MODEL="alias1"
CAI_STATE="true"
CAI_COMPACTED_MEMORY="true"
CAI_GUARDRAILS="true"
CAI_PRICE_LIMIT="5"
```

### Example 3: Pentesting with Cost Control

```bash
CAI_MODEL="gpt-4o"
CAI_AGENT_TYPE="redteam_agent"
CAI_PRICE_LIMIT="2"
CAI_MAX_INTERACTIONS="100"
CAI_GUARDRAILS="true"
CAI_BRIEF="false"
```

### Example 4: Parallel Testing (Non-TUI)

```bash
CAI_MODEL="alias0-fast"
CAI_PARALLEL="5"
CAI_BRIEF="true"
CAI_MAX_TURNS="20"
CAI_STREAM="false"                    # LLM inference streaming off
CAI_TOOL_STREAM="false"               # Tool streaming off for parallel (auto-disabled)
```

---

## 📚 Related Documentation

- [Configuration Guide](cai/getting-started/configuration.md) - Basic setup and API keys
- [Commands Reference](cai/getting-started/commands.md) - Available CLI commands
- [TUI Documentation](tui/tui_index.md) - Terminal User Interface features
- [Agents Documentation](agents.md) - Available agent types
- [Guardrails](guardrails.md) - Security and safety features

---

## ⚠️ Important Notes

### API Keys

CAI does NOT provide API keys for any model by default. Configure your own keys in the `.env` file:

```bash
OPENAI_API_KEY="sk-..."              # Required (can use "sk-123" as placeholder)
ANTHROPIC_API_KEY="sk-ant-..."       # For Claude models
ALIAS_API_KEY="sk-..."               # For alias1 (CAI PRO)
OLLAMA_API_BASE="http://localhost:11434/v1"  # For local models
C99_API_KEY="your-api-key"           # For C99.nl subdomain discovery tool
```

See the [Configuration Guide](cai/getting-started/configuration.md) for more details.

### Setting Variables

There are three ways to configure environment variables:

**1. `.env` file (Recommended)**

```bash
# Add to .env file
CAI_MODEL="alias1"
CAI_PRICE_LIMIT="1"
```

**2. Command-line**

```bash
CAI_MODEL="gpt-4o" CAI_PRICE_LIMIT="2" cai
```

**3. Runtime configuration**

Use slash commands during a session: `/env list`, `/env set …`, and the in-session help above (`/help`, `/help var …`). See [Commands Reference](cai/getting-started/commands.md).