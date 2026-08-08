# CAI TUI Command Analysis: Agent, Model, and Parallel Commands

## Overview

This document analyzes how CAI commands like `/agent`, `/model`, and `/parallel` work in the TUI context, focusing on the distinction between global state and terminal-specific state.

## Architecture

### Key Components

1. **CommandHandler** (`src/cai/tui/components/command_handler.py`)
   - Handles CLI command execution within the TUI
   - Intercepts console output and redirects to appropriate terminal widgets
   - Has special handling for `/agent` and `/model` commands

2. **SessionManager** (`src/cai/tui/core/session_manager.py`)
   - Manages overall TUI session state
   - Coordinates multiple terminal runners
   - Handles parallel mode coordination
   - Methods: `update_model()`, `switch_agent()`

3. **TerminalRunner** (`src/cai/tui/core/terminal_runner.py`)
   - Manages agent execution within a single terminal
   - Each terminal has its own agent instance and message history
   - Methods: `switch_agent()`, `update_model()`

4. **REPL Commands** (`src/cai/repl/commands/`)
   - `/agent` - Agent selection and management
   - `/model` - Model selection
   - `/parallel` - Parallel agent configuration

## Command Behavior Analysis

### 1. `/agent` Command

**Current Implementation:**
- Modifies global environment variable `CAI_AGENT_TYPE`
- Updates `AGENT_MANAGER` global state
- Handles parallel pattern loading into `PARALLEL_CONFIGS`

**TUI Context Issues:**
- The command modifies global state that affects all terminals
- No per-terminal agent selection mechanism
- CommandHandler has special handling but only updates local state

**Per-Terminal Requirements:**
- Need to call `session_manager.switch_agent(agent_name, terminal_number)`
- Should not modify global `CAI_AGENT_TYPE` when in TUI mode
- Each terminal should maintain its own agent configuration

### 2. `/model` Command

**Current Implementation:**
- Modifies global environment variable `CAI_MODEL`
- Affects all future agent interactions globally

**TUI Context Behavior:**
- CommandHandler detects `/model` commands
- Calls `session_manager.update_model(model_name)`
- SessionManager updates all terminal runners with the new model
- This is actually appropriate for model changes (typically want all terminals to use same model)

**Working Correctly:**
- Model updates propagate to all terminals as expected
- Each terminal runner updates its agent's model recursively

### 3. `/parallel` Command

**Current Implementation:**
- Manages `PARALLEL_CONFIGS` global list
- Sets `CAI_PARALLEL` and `CAI_PARALLEL_AGENTS` environment variables
- Configures agents for parallel execution

**TUI Context Behavior:**
- SessionManager detects parallel mode via `is_parallel_mode` flag
- Distributes parallel agents across terminals (Terminal 1 = P1, Terminal 2 = P2, etc.)
- Uses `PARALLEL_CONFIGS` to determine agent assignments

**Issues:**
- Parallel mode is global, not per-terminal
- Cannot have different parallel configurations in different terminals
- `/agent` command that loads parallel patterns affects all terminals

## State Management

### Global State (Shared Across All Terminals)
- Environment variables: `CAI_AGENT_TYPE`, `CAI_MODEL`, `CAI_PARALLEL`
- `PARALLEL_CONFIGS` list
- `AGENT_MANAGER` registry
- `PARALLEL_ISOLATION` histories

### Per-Terminal State
- `TerminalRunner.agent` - Agent instance
- `TerminalRunner.message_history` - Conversation history
- `TerminalRunner.config` - Terminal configuration including agent_name and model
- `TerminalConfig.parallel_config` - Parallel agent assignment

## Recommended Fixes

### 1. Make `/agent` Terminal-Aware in TUI Mode

```python
# In command_handler.py, enhance the special handling:
if cmd_name.lower() in ["/agent", "/a"] and args:
    if args[0] == "select" and len(args) > 1:
        agent_name = args[1]
        terminal_number = self.terminal_number  # Need to add this
        
        # Don't modify global state in TUI mode
        if hasattr(self, 'session_manager') and self.session_manager:
            asyncio.create_task(
                self.session_manager.switch_agent(agent_name, terminal_number)
            )
```

### 2. Add Terminal-Specific Agent Command

Create a new command like `/terminal-agent` or modify `/agent` to accept terminal number:
- `/agent select <name> --terminal 2` - Select agent for specific terminal
- `/agent select <name>` - In TUI, affects only current terminal

### 3. Enhance Parallel Mode for TUI

- Allow parallel configurations to be terminal-set specific
- Each terminal could have its own parallel group
- Terminal 1-3 could run one parallel set, Terminal 4-6 another

### 4. Add TUI Context Detection

Commands should detect TUI mode and behave differently:
```python
def is_tui_mode():
    return os.getenv("CAI_TUI_MODE") == "true"

# In agent command:
if not is_tui_mode():
    # Current behavior - modify global state
    os.environ["CAI_AGENT_TYPE"] = agent_key
else:
    # TUI behavior - notify session manager
    # Don't modify global state
```

## Current Workarounds

1. **For Agent Selection in Specific Terminal:**
   - Use the terminal's direct execution instead of commands
   - Manually reinitialize terminal runners after global changes

2. **For Model Changes:**
   - The current implementation works well for global model updates
   - Use `/model` command normally

3. **For Parallel Mode:**
   - Configure parallel agents before starting terminals
   - Use environment variables to pre-configure
   - Parallel mode affects all terminals as designed

## Conclusion

The TUI command system currently relies heavily on global state inherited from the CLI design. To support true multi-terminal workflows, commands need to be enhanced with terminal-awareness and the ability to modify per-terminal state rather than global state. The `/model` command works well as-is since model changes are typically desired globally, but `/agent` and `/parallel` need terminal-specific implementations.