# CAI Terminal User Interface

Clean architecture implementation of the CAI Terminal User Interface using Textual.

## Overview

The CAI TUI provides a modern terminal interface for interacting with CAI agents. It features:

- **Clean Architecture**: Well-organized modular components following best design patterns
- **Universal Terminal Design**: All terminals look identical with numbered IDs (Terminal 1, 2, 3...)
- **Banner Animation**: CAI banner appears with cascade effect in all terminals
- **Parallel Agent Support**: Execute multiple agents simultaneously in split terminals
- **Session Management**: Proper isolation and management of agent conversations
- **CLI Logic Integration**: Full wrapper around cli.py functionality

## Architecture

The TUI is built with a modular component-based architecture:

### Core Components (`src/cai/tui/core/`)

#### 1. **SessionManager**
- Manages the overall TUI session
- Coordinates all terminal runners
- Handles parallel vs single mode switching
- Manages session statistics and cleanup

#### 2. **TerminalRunner**
- Manages agent execution within each terminal
- Creates isolated agent instances with fresh models
- Handles conversation history independently
- Manages agent switching and model updates

#### 3. **AgentExecutor**
- Handles parallel agent execution
- Executes agents in parallel mode
- Manages history isolation between agents
- Handles parallel result aggregation

### UI Components (`src/cai/tui/components/`)

#### 1. **UniversalTerminal**
- Single terminal widget for all purposes
- Shows sequential terminal numbers (Terminal 1, 2, 3...)
- Displays CAI banner with cascade animation
- No visual distinction between agent types

#### 2. **StableTerminalGrid**
- Grid layout manager
- Handles terminal splitting and layout
- Manages focus between terminals
- Supports various layout modes

#### 3. **Sidebar**
- Agent list for quick selection
- Toggle with Ctrl+S
- Shows available agents

#### 4. **BannerWidget**
- Displays the CAI ASCII art logo
- Cascade effect animation
- Reusable across different components

#### 5. **CommandHandler**
- CLI command processing
- Handles /commands
- Integrates with CAI REPL commands

#### 6. **AgentManager**
- Agent lifecycle management
- Agent initialization
- Agent switching

## Features

### Terminal Behavior

1. **Terminal 1** is always the main terminal
2. All terminals show identical UI with sequential numbering
3. CAI banner appears in all terminals with cascade effect
4. Each terminal maintains independent conversation history
5. In parallel mode, commands execute in all agent terminals simultaneously

### Model Isolation

Each terminal creates a fresh `OpenAIChatCompletionsModel` instance:

```python
client = AsyncOpenAI(api_key=api_key)
fresh_model = OpenAIChatCompletionsModel(
    openai_client=client,
    model=os.getenv("CAI_MODEL", "alias1")
)
```

This ensures complete isolation between parallel agents.

### Session Management

The `SessionManager` coordinates all terminals:

```python
session_manager = SessionManager()
runner = session_manager.add_terminal_runner(1, terminal_widget)
await session_manager.initialize_terminal(1)
await session_manager.execute_command(command)
```

### Parallel Execution

When in parallel mode, commands are distributed to all agent terminals:

```python
session_manager.set_parallel_mode(True)
# Commands now execute in all configured parallel agents
```

## Usage

```bash
# Start the TUI
cai --tui

# Key bindings
Ctrl+S  - Toggle sidebar
Ctrl+L  - Clear screen
Ctrl+P  - Send prompt to all agents (parallel mode)
Ctrl+C  - Exit

# Commands
/agent list                    - List available agents
/agent select <name>           - Select an agent
/parallel add <agent>          - Add agent to parallel mode
/parallel clear                - Clear all parallel agents
/help                          - Show help
```

## File Structure

```
src/cai/tui/
├── README.md                  # This file
├── cai_terminal.py           # Main application
├── core/                     # Core business logic
│   ├── __init__.py
│   ├── session_manager.py    # Session coordination
│   ├── terminal_runner.py    # Terminal execution
│   └── agent_executor.py     # Parallel agent execution
└── components/              # UI components
    ├── __init__.py
    ├── agent_manager.py      # Agent lifecycle management
    ├── autocomplete_input.py # Input with history/autocomplete
    ├── banner_widget.py      # CAI banner display
    ├── command_handler.py    # CLI command processing
    ├── sidebar.py            # Agent selection sidebar
    ├── stable_grid.py        # Terminal grid layout
    └── universal_terminal.py # Universal terminal widget
```

## CSS Architecture

The UI uses Textual's CSS system with:
- Dock layout for fixed positioning
- Flexbox for dynamic layouts
- Mode-based visibility toggling
- Consistent color scheme (#03fcb1 on black)

## Design Patterns

1. **Manager Pattern**: SessionManager coordinates all components
2. **Runner Pattern**: TerminalRunner encapsulates execution logic
3. **Factory Pattern**: Agent instances created fresh for each terminal
4. **Observer Pattern**: Terminals react to role and state changes
5. **Command Pattern**: Commands processed through handler chain

## State Management

- **Mode**: Reactive property switches between "single" and "parallel"
- **PARALLEL_CONFIGS**: Global configuration for parallel agents
- **Component isolation**: Each component manages its own state
- **Session isolation**: Each terminal maintains independent history

## Extension Points

To add new features:
1. Create a new component in `components/`
2. Import and compose in `cai_terminal.py`
3. Add CSS styling in the main CSS block
4. Handle events with `@on` decorators

## Future Enhancements

1. Terminal persistence and session saving
2. Advanced layouts (tabs, panes)
3. Plugin system for custom commands
4. Export conversation history
5. Real-time collaboration features