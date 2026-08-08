#!/usr/bin/env python3

"""
Test suite for the help command functionality.
Tests all handle methods and input possibilities for the help command.
"""

from io import StringIO
from unittest.mock import Mock, patch

import pytest
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from cai.repl.commands.base import Command
from cai.repl.commands.help import HelpCommand
from cai.repl.ui.banner import _QUICK_GUIDE_COMMANDS_DOC_URL, display_quick_guide


def test_help_topics_command_index_lists_bare_question_mark_for_shortcuts():
    """``/?`` is a ``/help`` alias; bare ``?`` is the shortcuts command — never show ``/?`` as the command name."""
    from cai.repl.commands.command_reference_index import help_topic_rows_by_category

    rows_by_cat = help_topic_rows_by_category()
    cmds = [cmd for _, rows in rows_by_cat for cmd, _ in rows]
    assert "?" in cmds
    assert "/?" not in cmds


class TestDisplayQuickGuide:
    """Regression tests for bare /help quick guide rendering (display_quick_guide)."""

    def test_no_cai_prompt_prefix_and_docs_url_in_output(self):
        buf = StringIO()
        # Tall + wide: body must not collapse to "..."; subtitle must fit GDPR + full docs URL.
        console = Console(file=buf, width=280, height=200, force_terminal=True)
        display_quick_guide(console)
        out = buf.getvalue()
        assert _QUICK_GUIDE_COMMANDS_DOC_URL in out
        assert "pseudonymized data" in out
        assert "GDPR" in out
        assert "CAI>/" not in out
        assert "CAI> " not in out
        assert "Essential commands" in out
        assert "Quick shortcuts" in out
        assert "Blue team review" in out


class TestHelpCommand:
    """Test class for HelpCommand functionality."""

    @pytest.fixture
    def help_command(self):
        """Create a HelpCommand instance for testing."""
        return HelpCommand()

    @pytest.fixture
    def mock_console(self):
        """Create a mock console for testing output."""
        with patch("cai.repl.commands.help.console") as mock_console:
            yield mock_console

    @pytest.fixture
    def mock_commands_registry(self):
        """Create a mock commands registry for testing."""
        mock_registry = {
            "/memory": Mock(name="/memory", description="Memory commands"),
            "/help": Mock(name="/help", description="Help commands"),
            "/agent": Mock(name="/agent", description="Agent commands"),
        }

        with patch("cai.repl.commands.help.COMMANDS", mock_registry):
            yield mock_registry

    @pytest.fixture
    def mock_aliases_registry(self):
        """Create a mock aliases registry for testing."""
        mock_aliases = {"/h": "/help", "/m": "/memory", "/a": "/agent"}

        with patch("cai.repl.commands.help.COMMAND_ALIASES", mock_aliases):
            yield mock_aliases

    def test_command_initialization(self, help_command):
        """Test that HelpCommand initializes correctly."""
        assert isinstance(help_command, Command)
        assert help_command.name == "/help"
        assert "Display help information about commands and features" in help_command.description
        assert "/h" in help_command.aliases

        # Check that all expected subcommands are registered
        expected_subcommands = [
            # Agent Management
            "agent",
            "parallel",
            "queue",
            # Memory & History
            "memory",
            "history",
            "compact",
            "flush",
            "load",
            "save",
            "merge",
            # Environment & Config
            "config",
            "env",
            "var",
            "workspace",
            "virtualization",
            # Tools & Integration
            "mcp",
            "shell",
            # Utilities
            "model",
            "graph",
            "aliases",
            # Session & Cost
            "cost",
            "context",
            "exit",
            "resume",
            "sessions",
            "replay",
            "continue",
            # Model Tuning
            "temperature",
            "topp",
            # Advanced
            "settings",
            "auth",
            "ctr",
            "api",
            "metadebug",
            # General
            "commands",
            "topics",
        ]
        for subcommand in expected_subcommands:
            assert subcommand in help_command.subcommands
        assert len(help_command.subcommands) == len(expected_subcommands)

    def test_handle_no_args(self, help_command, mock_console):
        """Bare /help: quick guide Panel, blank line, then environment reference Panel."""
        result = help_command.handle_no_args()

        assert result is True
        assert mock_console.print.call_count == 3
        first_arg = mock_console.print.call_args_list[0][0][0]
        assert isinstance(first_arg, Panel)
        third_arg = mock_console.print.call_args_list[2][0][0]
        assert isinstance(third_arg, Panel)

    def test_handle_var_usage_panel(self, help_command, mock_console):
        """/help var with no name shows usage Panel."""
        result = help_command.handle_var(None)
        assert result is True
        mock_console.print.assert_called_once()
        printed = mock_console.print.call_args[0][0]
        assert isinstance(printed, Panel)

    def test_handle_var_usage_panel_includes_help_var_syntax(self, help_command, mock_console):
        """Usage panel documents /help var NAME (visible in rendered body)."""
        help_command.handle_var([])
        panel = mock_console.print.call_args[0][0]
        body = str(panel.renderable)
        assert "/help" in body
        assert "var" in body.lower()
        assert "CAI_MODEL" in body or "Examples" in body

    def test_handle_var_known_config_variable(self, help_command, mock_console):
        """/help var CAI_MODEL renders a success Panel with catalog content."""
        result = help_command.handle_var(["CAI_MODEL"])
        assert result is True
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        assert isinstance(panel, Panel)
        body = str(panel.renderable)
        assert "CAI_MODEL" in body
        assert "/env" in body

    def test_handle_var_known_extra_only_variable(self, help_command, mock_console):
        """Former “additional” vars (e.g. CAI_YOLO) are in the catalog and get long-form help."""
        result = help_command.handle_var(["CAI_YOLO"])
        assert result is True
        panel = mock_console.print.call_args[0][0]
        assert isinstance(panel, Panel)
        body = str(panel.renderable)
        assert "CAI_YOLO" in body
        assert "/env list" in body

    def test_handle_var_unknown_variable_returns_false(self, help_command, mock_console):
        """Unknown name yields failure and explanatory body."""
        result = help_command.handle_var(["NOT_A_REAL_CAI_VAR_XYZ123"])
        assert result is False
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        assert isinstance(panel, Panel)
        combined = f"{panel.title!s}{panel.renderable!s}"
        assert "Unknown" in combined
        assert "environment variable" in str(panel.renderable).lower()

    def test_handle_var_multiple_names_mixed_ok(self, help_command, mock_console):
        """Several tokens: one known, one unknown → False and two Panels."""
        result = help_command.handle_var(["CAI_DEBUG", "NOT_REAL_VAR_ABC"])
        assert result is False
        assert mock_console.print.call_count == 2

    def test_handle_help_calls_print_environment_reference(self, help_command, mock_console):
        """Bare /help (handle_help) includes the environment reference."""
        with patch(
            "cai.repl.commands.environment_reference.print_environment_reference"
        ) as mock_ref:
            result = help_command.handle_help()
        assert result is True
        mock_ref.assert_called_once()
        assert mock_ref.call_args[0][0] is mock_console

    def test_handle_memory_subcommand(self, help_command, mock_console):
        """Test memory subcommand help renders a single Panel."""
        result = help_command.handle_memory()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Memory" in panel_content
        assert "/memory list" in panel_content
        assert "/memory save" in panel_content

    def test_handle_agents_subcommand(self, help_command, mock_console):
        """Test agents subcommand help."""
        result = help_command.handle_agent()

        assert result is True
        mock_console.print.assert_called_once()

        # Verify the content contains agent-related information
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Agent Commands" in panel_content or "agent" in panel_content.lower()
        assert "/agent list" in panel_content

    def test_handle_graph_subcommand(self, help_command, mock_console):
        """Test graph subcommand help."""
        result = help_command.handle_graph()

        assert result is True
        mock_console.print.assert_called_once()

        # Verify graph-related content
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Graph" in panel_content
        assert "/graph" in panel_content

    def test_handle_shell_subcommand(self, help_command, mock_console):
        """Test shell subcommand help."""
        result = help_command.handle_shell()

        assert result is True
        mock_console.print.assert_called_once()

        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Shell" in panel_content or "shell" in panel_content.lower()
        assert "/shell <command>" in panel_content or "shell" in panel_content.lower()

    def test_handle_env_subcommand(self, help_command, mock_console):
        """Test env subcommand help."""
        result = help_command.handle_env()

        assert result is True
        mock_console.print.assert_called_once()

        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Environment" in panel_content or "environment" in panel_content.lower()
        assert "CAI_MODEL" in panel_content

    def test_handle_aliases_subcommand(self, help_command, mock_console, mock_aliases_registry):
        """Test aliases subcommand help."""
        result = help_command.handle_aliases()

        assert result is True
        # Should print multiple times (header, table, tips)
        assert mock_console.print.call_count >= 2

    def test_handle_model_subcommand(self, help_command, mock_console):
        """Test model subcommand help renders a single Panel."""
        result = help_command.handle_model()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Model" in panel_content
        assert "/model" in panel_content

    def test_handle_config_subcommand(self, help_command, mock_console):
        """Test config subcommand help."""
        result = help_command.handle_config()

        assert result is True
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        text = str(getattr(panel, "renderable", panel))
        assert "deprecated" in text.lower()
        assert "/env" in text

    def test_handle_help_aliases(
        self, help_command, mock_console, mock_commands_registry, mock_aliases_registry
    ):
        """Test handle_help_aliases method directly."""
        result = help_command.handle_help_aliases()

        assert result is True
        # Should print header, table, and tips
        assert mock_console.print.call_count >= 3

    def test_handle_config_deprecated_notice(self, help_command, mock_console):
        """``/help config`` prints the same deprecation panel as ``/config`` (via ``help.console``)."""
        result = help_command.handle_config()
        assert result is True
        mock_console.print.assert_called_once()
        body = str(mock_console.print.call_args[0][0].renderable)
        assert "deprecated" in body.lower()
        assert "/env" in body

    def test_print_command_table(self, help_command, mock_console):
        """Test _print_command_table helper method."""
        test_commands = [
            ("/test", "/t", "Test command description"),
            ("/example", "/e", "Example command description"),
        ]

        help_command._print_command_table("Test Commands", test_commands)

        mock_console.print.assert_called_once()

    def test_create_styled_table_function(self):
        """Test create_styled_table helper function."""
        from cai.repl.commands.help import create_styled_table

        headers = [("Command", "yellow"), ("Description", "white")]
        table = create_styled_table("Test Table", headers)

        assert isinstance(table, Table)
        assert table.title == "Test Table"

    def test_create_notes_panel_function(self):
        """Test create_notes_panel helper function."""
        from cai.repl.commands.help import create_notes_panel

        notes = ["Note 1", "Note 2", "Note 3"]
        panel = create_notes_panel(notes, "Test Notes")

        assert isinstance(panel, Panel)

    def test_full_help_workflow(self, help_command, mock_console):
        """Test complete help workflow integration."""
        # Test main help
        result1 = help_command.handle_no_args()
        assert result1 is True

        # Test various subcommands
        result2 = help_command.handle_agent()
        assert result2 is True

        result3 = help_command.handle_shell()
        assert result3 is True

        result4 = help_command.handle_env()
        assert result4 is True

        # All should succeed
        assert all([result1, result2, result3, result4])

    def test_handle_aliases_with_empty_registry(self, help_command, mock_console):
        """Test aliases help with empty aliases registry."""
        with patch("cai.repl.commands.help.COMMAND_ALIASES", {}):
            with patch("cai.repl.commands.help.COMMANDS", {}):
                result = help_command.handle_help_aliases()

        assert result is True
        # Should still create the table structure even if empty
        assert mock_console.print.call_count >= 2

    # ------------------------------------------------------------------
    # Tests for subcommands that were previously uncovered (Phase 2)
    # ------------------------------------------------------------------

    def test_handle_parallel_subcommand(self, help_command, mock_console):
        """Test parallel subcommand help."""
        result = help_command.handle_parallel()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Parallel" in panel_content
        assert "/parallel add" in panel_content or "/parallel run" in panel_content

    def test_handle_queue_subcommand(self, help_command, mock_console):
        """Test queue subcommand help."""
        result = help_command.handle_queue()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Queue" in panel_content
        assert "/queue move" in panel_content

    def test_handle_history_subcommand(self, help_command, mock_console):
        """Test history subcommand help."""
        result = help_command.handle_history()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "History" in panel_content
        assert "/history" in panel_content

    def test_handle_compact_subcommand(self, help_command, mock_console):
        """Test compact subcommand help."""
        result = help_command.handle_compact()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Compact" in panel_content
        assert "/compact" in panel_content

    def test_handle_flush_subcommand(self, help_command, mock_console):
        """Test flush subcommand help."""
        result = help_command.handle_flush()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Flush" in panel_content or "Clear" in panel_content
        assert "/flush" in panel_content

    def test_handle_load_subcommand(self, help_command, mock_console):
        """Test load subcommand help."""
        result = help_command.handle_load()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Load" in panel_content
        assert "JSONL" in panel_content

    def test_handle_merge_help_subcommand(self, help_command, mock_console):
        """Test merge subcommand help."""
        result = help_command.handle_merge_help()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Merge" in panel_content
        assert "/merge" in panel_content

    def test_handle_workspace_subcommand(self, help_command, mock_console):
        """Test workspace subcommand help."""
        result = help_command.handle_workspace()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Workspace" in panel_content
        assert "/workspace" in panel_content

    def test_handle_virtualization_subcommand(self, help_command, mock_console):
        """Test virtualization subcommand help."""
        result = help_command.handle_virtualization()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Docker" in panel_content or "Virtualization" in panel_content
        assert "/virtualization" in panel_content or "/virt" in panel_content

    def test_handle_mcp_subcommand(self, help_command, mock_console):
        """Test MCP subcommand help."""
        result = help_command.handle_mcp()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "MCP" in panel_content or "Model Context Protocol" in panel_content
        assert "/mcp" in panel_content

    def test_handle_commands_subcommand(self, help_command, mock_console):
        """Test commands listing subcommand."""
        result = help_command.handle_commands()

        assert result is True
        assert mock_console.print.call_count == 1
        first_arg = mock_console.print.call_args_list[0][0][0]
        assert hasattr(first_arg, "renderable")
        panel_content = str(first_arg.renderable)
        assert "All available commands" in panel_content
        assert "/help topics" in panel_content

    def test_help_legacy_quick_tokens_hint_quickstart(self, help_command, mock_console):
        """/help quick and /help quickstart are not subcommands; hint points to /quickstart."""
        assert help_command.handle(["quick"]) is False
        assert help_command.handle(["quickstart"]) is False
        assert mock_console.print.call_count == 2
        for call in mock_console.print.call_args_list:
            text = call[0][0]
            assert "/quickstart" in text
            assert "/qs" in text or "quick" in text.lower()

    def test_handle_help_topics_subcommand(self, help_command):
        """/help topics: command index (single outer Panel), no environment reference."""
        buf = StringIO()
        console = Console(file=buf, width=160, height=120, force_terminal=True)
        stub_rows = [("Agent Management", [("/agent", "List and select agents")])]
        with patch("cai.repl.commands.help.console", console):
            with patch(
                "cai.repl.commands.command_reference_index.help_topic_rows_by_category",
                return_value=stub_rows,
            ):
                result = help_command.handle_help_topics()

        assert result is True
        out = buf.getvalue()
        assert "registered slash commands" in out
        assert "/help <topic>" in out
        assert "topics" in out.lower()

    # ------------------------------------------------------------------
    # Tests for Phase 3: newly added subcommands
    # ------------------------------------------------------------------

    def test_handle_cost_subcommand(self, help_command, mock_console):
        """Test cost subcommand help."""
        result = help_command.handle_cost()

        assert result is True
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        assert hasattr(panel, "renderable")
        body = str(panel.renderable)
        title = str(getattr(panel, "title", "") or "")
        assert "/cost" in body
        assert "Cost" in title or "cost" in body.lower()

    def test_handle_exit_subcommand(self, help_command, mock_console):
        """Test exit subcommand help."""
        result = help_command.handle_exit()

        assert result is True
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        assert hasattr(panel, "renderable")
        body = str(panel.renderable)
        title = str(getattr(panel, "title", "") or "")
        assert "/exit" in body
        assert "Exit" in title or "exit" in body.lower()

    def test_handle_resume_subcommand(self, help_command, mock_console):
        """Test resume subcommand help."""
        result = help_command.handle_resume()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Resume" in panel_content
        assert "/resume" in panel_content

    def test_handle_sessions_subcommand(self, help_command, mock_console):
        """Test sessions subcommand help."""
        result = help_command.handle_sessions()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Sessions" in panel_content or "sessions" in panel_content.lower()

    def test_handle_replay_subcommand(self, help_command, mock_console):
        """Test replay subcommand help."""
        result = help_command.handle_replay()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Replay" in panel_content
        assert "JSONL" in panel_content

    def test_handle_continue_subcommand(self, help_command, mock_console):
        """Test continue subcommand help."""
        result = help_command.handle_continue()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Continuation" in panel_content or "Continue" in panel_content

    def test_handle_temperature_subcommand(self, help_command, mock_console):
        """Test temperature subcommand help."""
        result = help_command.handle_temperature()

        assert result is True
        mock_console.print.assert_called_once()
        panel = mock_console.print.call_args[0][0]
        assert hasattr(panel, "renderable")
        panel_content = str(panel.renderable)
        assert "/temperature" in panel_content
        # Panel title is separate from ``renderable`` (body describes "Sampling temperature").
        assert panel.title is not None
        assert "Temperature" in str(panel.title)

    def test_handle_topp_subcommand(self, help_command, mock_console):
        """Test top-p subcommand help."""
        result = help_command.handle_topp()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Top-P" in panel_content or "top_p" in panel_content.lower()
        assert "/top_p" not in panel_content

    def test_handle_settings_subcommand(self, help_command, mock_console):
        """Test settings subcommand help."""
        result = help_command.handle_settings()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Settings" in panel_content or "Configuration" in panel_content

    def test_handle_auth_subcommand(self, help_command, mock_console):
        """Test auth subcommand help."""
        result = help_command.handle_auth()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Auth" in panel_content or "auth" in panel_content.lower()
        assert "add-user" in panel_content
        assert "add-ip" in panel_content

    def test_handle_ctr_subcommand(self, help_command, mock_console):
        """Test CTR subcommand help."""
        result = help_command.handle_ctr()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "CTR" in panel_content
        assert "/ctr" in panel_content

    def test_handle_api_subcommand(self, help_command, mock_console):
        """Test API subcommand help."""
        result = help_command.handle_api()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "API" in panel_content
        assert "/api" in panel_content

    def test_handle_metadebug_subcommand(self, help_command, mock_console):
        """Test metadebug subcommand help."""
        result = help_command.handle_metadebug()

        assert result is True
        mock_console.print.assert_called_once()
        call_args = mock_console.print.call_args[0][0]
        assert hasattr(call_args, "renderable")
        panel_content = str(call_args.renderable)
        assert "Meta" in panel_content or "Debug" in panel_content

    # ------------------------------------------------------------------
    # Dispatch through handle(): verify subcommand routing
    # ------------------------------------------------------------------

    def test_handle_dispatches_to_parallel(self, help_command, mock_console):
        """Test that /help parallel dispatches correctly."""
        result = help_command.handle(["parallel"])
        assert result is True
        assert mock_console.print.call_count >= 1

    def test_handle_dispatches_to_history(self, help_command, mock_console):
        """Test that /help history dispatches correctly."""
        result = help_command.handle(["history"])
        assert result is True
        assert mock_console.print.call_count >= 1

    def test_handle_dispatches_to_commands(self, help_command, mock_console):
        """Test that /help commands dispatches correctly."""
        result = help_command.handle(["commands"])
        assert result is True
        assert mock_console.print.call_count >= 1

    def test_handle_unknown_subcommand(self, help_command, mock_console):
        """Test that an unknown subcommand is handled gracefully."""
        result = help_command.handle(["nonexistent_subcommand"])
        assert result is True or result is False

    def test_subcommand_with_none_args(self, help_command):
        """Test that all subcommands handle None arguments correctly."""
        results = [
            help_command.handle_memory(None),
            help_command.handle_agent(None),
            help_command.handle_graph(None),
            help_command.handle_shell(None),
            help_command.handle_env(None),
            help_command.handle_aliases(None),
            help_command.handle_model(None),
            help_command.handle_config(None),
            help_command.handle_parallel(None),
            help_command.handle_queue(None),
            help_command.handle_history(None),
            help_command.handle_compact(None),
            help_command.handle_flush(None),
            help_command.handle_load(None),
            help_command.handle_merge_help(None),
            help_command.handle_workspace(None),
            help_command.handle_virtualization(None),
            help_command.handle_mcp(None),
            help_command.handle_commands(None),
            help_command.handle_help_topics(None),
            help_command.handle_cost(None),
            help_command.handle_exit(None),
            help_command.handle_resume(None),
            help_command.handle_sessions(None),
            help_command.handle_replay(None),
            help_command.handle_continue(None),
            help_command.handle_temperature(None),
            help_command.handle_topp(None),
            help_command.handle_settings(None),
            help_command.handle_auth(None),
            help_command.handle_ctr(None),
            help_command.handle_api(None),
            help_command.handle_metadebug(None),
        ]

        assert all(results)
