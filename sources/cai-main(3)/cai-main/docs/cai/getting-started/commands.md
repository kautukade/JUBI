# CAI REPL Commands

This document provides documentation for all commands available in the CAI (Context-Aware Interface) REPL system.

## Base Command System (`base.py`)

---

## Core Commands

### **Agent Management (`agent.py`)**

### **AgentCommand**

- **Command**: `/agent`
- **Purpose**: Managing and switching between different AI agents
- **Features**:
  - List available agents
  - Switch between agents
  - Display agent information
  - Visualize agent interaction graphs
- **Defaults**: The CLI default is **`orchestration_agent`** (breadth-first entry with specialist tools: `run_specialist`, `run_dual_approach_contest`, `run_parallel_specialists`). Use **`selection_agent`** for a slimmer handoff-only router. Tune worker budgets and the optional multi-front hint with **`CAI_ORCHESTRATION_WORKER_MAX_TURNS`** and **`CAI_ORCHESTRATION_MAS_HINT`** (see [Environment variables](../../environment_variables.md)).

### **Configuration Management (`config.py`)**

### **ConfigCommand**

- **Command**: `/config` (alias `/cfg`)
- **Purpose**: Deprecated; prints a notice to use `/env` instead. Does not change variables.

### **Environment variables (`env.py`)**

### **EnvCommand**

- **Command**: `/env` (alias `/e`)
- **Purpose**: Inspect and change environment variables for the current REPL process
- **Features**:
  - Bare `/env`: table of `CAI_`* and `CTF_`* variables currently set (sensitive values masked)
  - `/env list`: numbered catalog with defaults and descriptions
  - `/env get <n|NAME>`: read one catalog entry
  - `/env set <n|NAME> <value...>`: set by catalog index or full variable name (value may contain spaces; no quotes)
  - `/env default`: restore every catalog variable to its registered default

### **Cost Tracking (`cost.py`)**

### **CostCommand**

- **Command**: `/cost` (aliases: `/costs`, `/usage`)
- **Purpose**: View usage costs and statistics (session via `COST_TRACKER`; persisted global totals in `~/.cai/usage.json` when usage tracking is enabled).
- **Subcommands**:
  - `/cost` or `/cost summary` — same: session + global summary, top models snippet, hints
  - `/cost models` — per-model costs
  - `/cost daily` — last 30 days plus weekly rollup
  - `/cost sessions` — recent sessions (default 10); `/cost sessions <n>` limits rows
  - `/cost reset` — clear persisted stats (confirm with `RESET`; backup created first)
- **Help**: `/h cost` — syntax aligned with the above

### **Exit (`exit.py`)**

### **ExitCommand**

- **Command**: `/exit` (aliases: `/q`, `/quit`)
- **Purpose**: Terminate the CAI REPL session with the same orderly shutdown as Ctrl+C at the prompt (including the session summary panel)
- **Features**:
  - Clean shutdown of the REPL
  - Save current session data
  - Cleanup background processes

### **Help System (`help.py`)**

### **HelpCommand**

- **Command**: `/help` or `/?` (aliases include `/h`). Note: **`/?`** is an alias for **`/help`** (leading slash). **`?`** alone (no slash) is a **different** command — see **Input shortcuts** below.
- **Purpose**: Display help information and command documentation
- **Features**:
  - **`/help commands`** (or **`/h commands`**, **`/? commands`**): one bordered help panel (same style as other `/h` topics) listing every registered slash command by category from the live registry
  - **`/help topics`**: same categories here in the REPL plus short copy on **`/help <topic>`** (detail panels; exceptions include **`/help var`**, **`/help commands`**, **`/help topics`**, **`/help aliases`**, **`/help config`**)
  - Show command usage
  - `**/help aliases`** (or `**/h aliases**`): list registered command shortcuts
  - Provide help for specific commands (e.g. `/help agent`, `/help env`; `/help model`)
  - **Environment variables:** bare `/help` shows command's guide plus **full environment reference tables** below
  - **Orchestration:** `/help var CAI_AGENT_TYPE`, `/help var CAI_ORCHESTRATION_WORKER_MAX_TURNS`, `/help var CAI_ORCHESTRATION_MAS_HINT` for the default entry agent and worker tuning
  - **Onboarding guide:** use **`/quickstart`** (aliases **`/qs`**, **`/quick`**); there is no `/help quick` or `/help quickstart` — if used, CAI prints a short hint to run **`/quickstart`**

### **Input shortcuts (`shortcuts.py`)**

- **Command**: **`?`** on its own line (CLI headless REPL only; not interpreted as a command in the TUI)
- **Purpose**: Short table of prefix keys (`/`, `$`) and prompt-toolkit bindings (Tab, Enter, multiline keys, history, Ctrl+L, etc.)
- **Empty-line hint**: the headless REPL shows a grey italic placeholder (**`? for shortcuts · type your prompt`**) when the line is empty (defined in `prompt.py`).

### **History Management (`history.py`)**

### **HistoryCommand**

- **Command**: `/history`
- **Purpose**: Display conversation history with agent filtering
- **Features**:
  - Show conversation history
  - Filter by specific agents
  - Display message tree structure
- **Note**: `/history export` is removed; use `/save <file>` (see **Save Data**). If you run `/history export`, CAI prints a deprecation hint pointing to `/save`.

---

## Data Management Commands

### **Compact Conversation (`compact.py`)**

### **CompactCommand**

- **Command**: `/compact`
- **Purpose**: Compact current conversation and manage model/prompt settings
- **Features**:
  - Reduce conversation context size
  - Change model during compaction
  - Modify prompt settings
  - Maintain conversation flow while reducing tokens

### **Load Data (`load.py`)**

### **LoadCommand**

- **Command**: `/load`
- **Purpose**: Load JSONL data into the current session context
- **Features**:
  - Load conversation history from files
  - Import external data
  - Integrate with parallel configurations
  - Support for various data formats (including JSONL written by `/save` and session logs)
  - Expands `~/` in file paths when resolving JSONL locations

### **Save Data (`save.py`)**

### **SaveCommand**

- **Command**: `/save`
- **Purpose**: Write all agent conversation histories to **JSONL** (reload with `/load`) or **Markdown** (readable report)
- **Features**:
  - `**.jsonl`**: one JSON object per line (`agent`, `role`, `content`, plus tool fields); same shape as the former `/history export`, loaded by `/load`
  - `**.md` / `.markdown`**: structured Markdown export (per-agent sections, roles as headings); not consumed by `/load`
  - Works with isolated parallel histories when applicable
  - Expands `~/` paths and creates parent directories as needed before writing

### **Memory management (`/memory`)**

### **MemoryCommand**

- **Command**: `/memory`
- **Purpose**: Manage persistent memory storage in `.cai/memory`
- **Features**:
  - Store conversation context persistently
  - Apply memory to current context
  - Manage memory entries
  - Persistent storage across sessions

### **Flush History (`flush.py`)**

### **FlushCommand**

- **Command**: `/flush`
- **Purpose**: Clear conversation history
- **Features**:
  - Clear current conversation
  - Reset agent contexts
  - Clean up memory
  - Start fresh conversation

---

## Model Management Commands

### **Model Configuration (`model.py`)**

### **ModelCommand** (`model.py`)

- **Command**: `/model`
- **Purpose**: View and change the current LLM model; browse the full catalog
- **Syntax**:
  - `/model` — short table + current `CAI_MODEL`
  - `/model show` — full LiteLLM catalog (optional `supported`, search term, or both)
  - `/model <name>` / `/model <n>` — set model (same numbering as `/model show`)

---

## Advanced Features

### **Graph Visualization (`graph.py`)**

### **GraphCommand**

- **Command**: `/graph` (alias `/g`)
- **Purpose**: Visualize conversation flow (user, assistant, tools) as a compact graph or tables
- **Syntax**:
  - `/graph` or `/graph show` — multi-agent layout when `CAI_PARALLEL` > 1 or multiple parallel slots exist; otherwise the active agent
  - `/graph all` — every agent with history
  - `/graph P<n>` — agent in parallel slot n (e.g. `P1`)
  - `/graph <agent_name>` — named agent (multi-word names allowed)
- **Subcommands**:
  - `timeline` — Rich table of messages per agent (ordered by message index, not wall-clock)
  - `stats` — per-agent message and tool-call counts
  - `export <json|dot|mermaid> [filename]` — export all tracked histories to a file

### **CTR analysis (`ctr.py`)**

### **CTRCommand**

- **Command**: `/ctr`
- **Purpose**: Run Cut-The-Rope-style game-theoretic analysis on the current session (in-memory history, session log, or latest JSONL fallback) and manage saved runs under the CTR output base directory (`CAI_CTR_OUTPUT_DIR` or default temp layout; see `cai.ctr.paths`).
- **Subcommands**:
  - `/ctr` — full analysis pipeline (writes a new `run_*` tree)
  - `/ctr show` — print Nash equilibrium and strategies (Rich tables; same run resolution as below)
  - `/ctr graph` — open the best available attack-graph PNG when possible; optional node/edge summary from `graph_information.txt`
  - `/ctr list` — list `run_*` directories (top level or one nested level under the base), newest first; row numbers match `/ctr use <n>`
  - `/ctr use <n|run_name|path>` — select the active run by list index, folder name under the base, or absolute path to a run directory
  - `/ctr open` — open the containing folder in the system file manager
- **Help**: `/h ctr` — syntax aligned with the above

### **Parallel Execution (`parallel.py`)**

### **ParallelCommand**

- **Command**: `/parallel`
- **Purpose**: Configure and run parallel agent workflows with isolated contexts
- **Features**:
  - Add/remove/list parallel agents
  - Queue prompts per agent or broadcast to all agents
  - Execute queued prompts with `/parallel run`
  - Merge results back into the main context
  - Exit parallel mode with or without merge

### **Queue Management (`queue.py`)**

### **QueueCommand**

- **Command**: `/queue`
- **Purpose**: Manage sequential prompt queue independently from parallel mode
- **Features**:
  - Add prompts to queue
  - List queued prompts
  - Run queued prompts sequentially
  - Clear queue safely

### **Merge Histories (`merge.py`)**

### **MergeCommand**

- **Command**: `/merge` (alias `/mrg`)
- **Purpose**: Merge parallel agent contexts into main context and exit parallel mode
- **Features**:
  - Combine histories from multiple agents
  - Integrate parallel conversation results into the current main thread
  - Automatically leave parallel mode after successful merge
  - Tab completion for agent arguments matches `/flush agent` (non-empty histories) and omits agents already listed in the command

---

## Integration Commands

### **MCP Integration (`mcp.py`)**

### **MCPCommand**

- **Command**: `/mcp` (alias `/m`)
- **Purpose**: Manage MCP (Model Context Protocol) servers and their tools
- **Subcommands** (see also `/mcp help`, `/help mcp`, and `/h mcp`):
  - `load <url> <name>` — SSE server; `load sse <url> <name>` — legacy SSE form; `load stdio <name> <command> [args…]` — stdio server
  - `list` — active servers (bare `/mcp` is equivalent)
  - `add <server_name> <agent_name_or_number>` — **server first**, then agent (name or index)
  - `remove`, `tools`, `status`, `associations`, `test`, `help`
- **Features**:
  - Load SSE MCP servers
  - Load STDIO MCP servers
  - List active MCP connections
  - Add MCP tools to agents
  - Manage MCP server lifecycle

---

## System Management Commands

### **Shell Access (`shell.py`)**

### **ShellCommand**

- **Command**: `/shell` (aliases: `/s`, `$` as the first token on the line)
- **Purpose**: Execute shell commands from within the REPL
- **Features**:
  - Run system commands
  - Access workspace directory
  - Container workspace support
  - Signal handling for processes
- **Note**: To send a signal to a host OS process by PID (similar to the removed dedicated `/kill` command), use the shell’s `kill`, for example `**/shell kill <PID>`** (or `kill -TERM`, `kill -9`, etc., as supported by your shell).

### **Virtualization (`virtualization` package / `_virtualization_monolith.py`)**

### **VirtualizationCommand**

- **Command**: `/virtualization` or `/virt`
- **Purpose**: Manage Docker-based virtualization environments
- **Subcommands**: `info` (same as no args), `list`, `set <container_id>`, `clear`, `pull <image>`, `run <image_or_id>` — `run` starts a new container from an image unless the token is a **unique** existing container-ID prefix (then it activates); `set <id>` or bare `/virt <id>` also attach.
- **Features**:
  - Set up Docker containers
  - Manage container lifecycle
  - Workspace virtualization
  - Environment isolation

### **Workspace Management (`workspace.py`)**

### **WorkspaceCommand**

- **Command**: `/workspace` or `/ws`
- **Purpose**: Manage named workspace (`CAI_WORKSPACE`) and paths on host or in an active Docker container
- **Subcommands**: `set <name>`, `get` (same as no args; there is no `show`), `ls [path]`, `exec <cmd>`, `copy` (requires `CAI_ACTIVE_CONTAINER` and the `container:` prefix on exactly one side)
- **Features**:
  - Host workspace dirs under `CAI_WORKSPACE_DIR` (default `~/.cai/workspace`)
  - When `CAI_ACTIVE_CONTAINER` is set: `ls` and `exec` run in the container workspace path; `copy` uses `docker cp`
- **REPL help**: `/h workspace` matches these subcommands.
- **Do not use**: a `show` subcommand (none exists) or `list` as a subcommand name — use `**ls`**.


| Invocation                       | Behaviour                                                                                                                                            |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `/workspace` or `/workspace get` | Prints workspace name, environment (host vs container), resolved paths, and short hints for subcommands.                                             |
| `/workspace set <name>`          | Sets `CAI_WORKSPACE` (label: letters, digits, `_`, `-` only). Creates the host folder and, if a **running** container is active, the path inside it. |
| `/workspace ls` [path]           | Lists files (container workspace when `CAI_ACTIVE_CONTAINER` is usable; else host). Optional path is relative to the workspace root.                 |
| `/workspace exec <cmd…>`         | Shell in workspace cwd (container when active, else host).                                                                                           |
| `/workspace copy <src> <dst>`    | `docker cp`; **requires** `CAI_ACTIVE_CONTAINER`; `**container:`** on exactly one path.                                                              |


To set `CAI_ACTIVE_CONTAINER`, attach a container with `**/virtualization**` or `**/virt**` (see `**/h virtualization**`).

### **Quickstart (`quickstart.py`)**

### **QuickstartCommand**

- **Command**: `/quickstart` (aliases **`/qs`**, **`/quick`**)
- **Purpose**: Display setup information for new users
- **Features**:
  - Essential setup guidance
  - Configuration instructions
  - Getting started tutorial
  - Auto-runs on first launch

---

## Utility Commands

### **Command Completion (`completer.py`)**

### **FuzzyCommandCompleter**

- **Purpose**: Intelligent command completion with fuzzy matching
- **Features**:
  - Command auto-completion
  - Fuzzy matching for typos
  - Subcommand suggestions
  - Argument completion
  - Command shadowing detection

---

## Usage Examples

### Basic Workflow

```bash
# Start CAI REPL
cai

# View available agents
/agent list

# Switch to a specific agent
/agent switch <agent_name>

# View conversation history
/history

# Change model
/model gpt-4

# Clear conversation
/flush

# Exit
/exit
```

### Advanced Features

```bash
# Set up parallel execution
/parallel add red_teamer
/parallel add network_traffic_analyzer

# Add prompts (per agent or all)
/parallel prompt all "Scan 192.168.1.0/24"

# Execute in parallel
/parallel run

# Merge all parallel contexts into main context and exit parallel mode
/merge

# Optional: exit without merging contexts
/parallel clear
```

### Integration Examples

```bash
# Burp Suite MCP (PortSwigger): stdio proxy to the BApp SSE endpoint — replace /path/to with the extracted JAR path
/mcp load stdio burp java -jar /path/to/mcp-proxy-all.jar --sse-url http://127.0.0.1:9876

# Add MCP tools to agent (server name first)
/mcp add burp <agent_name_or_number>

# Set up virtualized environment and a named workspace
/virtualization pull kalilinux/kali-rolling
/virtualization run kalilinux/kali-rolling
/virtualization list
/workspace set myproject
```

---

## Command Registration

All commands are automatically registered when their respective modules are imported through the `__init__.py` file. The command system uses a registry pattern to track all available commands and their aliases.

---

## File Structure

```
src/cai/repl/commands/
├── __init__.py          # Module exports and imports
├── base.py              # Base command class
├── agent.py             # Agent management
├── compact.py           # Conversation compaction
├── completer.py         # Command completion
├── config.py            # Configuration management
├── cost.py              # Cost tracking
├── env.py               # Environment variables
├── exit.py              # REPL exit
├── flush.py             # History clearing
├── graph.py             # Graph visualization
├── help.py              # Help system
├── history.py           # History management
├── load.py              # Data loading
├── mcp.py               # MCP integration
├── memory/              # /memory command (compacted summaries, .cai/memory)
├── merge.py             # History merging
├── model.py             # Model management
├── parallel.py          # Parallel execution
├── quickstart.py        # User onboarding
├── run.py               # Parallel execution trigger
├── shell.py             # Shell access
├── virtualization/      # Container management (re-exports monolith)
└── workspace.py         # Workspace management
```

---

## Extending the Command System

To add new commands:

1. Create a new Python file in `src/cai/repl/commands/`
2. Import the base `Command` class from `base.py`
3. Extend the `Command` class with your implementation
4. Use the `register_command` decorator or function
5. Add the import to `__init__.py`

Example:

```python
from cai.repl.commands.base import Command, register_command

class MyCommand(Command):
    def __init__(self):
        super().__init__(
            name="/mycommand",
            description="My custom command",
            aliases=["/my", "/mc"]
        )
    
    def execute(self, args):
        # Command implementation
        pass

register_command(MyCommand())
```

