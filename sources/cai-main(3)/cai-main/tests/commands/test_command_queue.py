#!/usr/bin/env python3

"""
Test suite for the /queue command, focusing on the move subcommand
and the removal of the priority column from queue display.
"""

from datetime import datetime
from unittest.mock import patch

import pytest

from cai.repl.commands.queue import QueueCommand, FALLBACK_QUEUE


@pytest.fixture(autouse=True)
def clear_fallback_queue():
    """Ensure FALLBACK_QUEUE is empty before and after each test."""
    FALLBACK_QUEUE.clear()
    yield
    FALLBACK_QUEUE.clear()


@pytest.fixture
def queue_cmd():
    return QueueCommand()


def _populate_queue(n: int = 3) -> None:
    """Add *n* dummy items to FALLBACK_QUEUE."""
    for i in range(1, n + 1):
        FALLBACK_QUEUE.append({
            "prompt": f"prompt {i}",
            "timestamp": datetime.now(),
            "agent": None,
        })


class TestQueueMoveSubcommand:
    """Tests for /queue move <from> <to>."""

    def test_move_registered(self, queue_cmd):
        assert "move" in queue_cmd.subcommands

    @patch("cai.repl.commands.queue.console")
    def test_move_valid(self, mock_console, queue_cmd):
        _populate_queue(3)
        result = queue_cmd._handle_move_cmd(["3", "1"])
        assert result is True
        assert FALLBACK_QUEUE[0]["prompt"] == "prompt 3"
        assert FALLBACK_QUEUE[1]["prompt"] == "prompt 1"
        assert FALLBACK_QUEUE[2]["prompt"] == "prompt 2"

    @patch("cai.repl.commands.queue.console")
    def test_move_to_end(self, mock_console, queue_cmd):
        _populate_queue(3)
        result = queue_cmd._handle_move_cmd(["1", "3"])
        assert result is True
        assert FALLBACK_QUEUE[0]["prompt"] == "prompt 2"
        assert FALLBACK_QUEUE[2]["prompt"] == "prompt 1"

    @patch("cai.repl.commands.queue.console")
    def test_move_same_position(self, mock_console, queue_cmd):
        _populate_queue(3)
        result = queue_cmd._handle_move_cmd(["2", "2"])
        assert result is True
        assert FALLBACK_QUEUE[1]["prompt"] == "prompt 2"

    @patch("cai.repl.commands.queue.console")
    def test_move_no_args(self, mock_console, queue_cmd):
        result = queue_cmd._handle_move_cmd()
        assert result is False

    @patch("cai.repl.commands.queue.console")
    def test_move_one_arg(self, mock_console, queue_cmd):
        result = queue_cmd._handle_move_cmd(["1"])
        assert result is False

    @patch("cai.repl.commands.queue.console")
    def test_move_non_numeric(self, mock_console, queue_cmd):
        _populate_queue(2)
        result = queue_cmd._handle_move_cmd(["a", "b"])
        assert result is False

    @patch("cai.repl.commands.queue.console")
    def test_move_out_of_range(self, mock_console, queue_cmd):
        _populate_queue(2)
        result = queue_cmd._handle_move_cmd(["1", "5"])
        assert result is False

    @patch("cai.repl.commands.queue.console")
    def test_move_empty_queue(self, mock_console, queue_cmd):
        result = queue_cmd._handle_move_cmd(["1", "2"])
        assert result is False


class TestQueueNoPriorityColumn:
    """Verify the priority column has been removed from queue display."""

    @patch("cai.repl.commands.queue.console")
    def test_show_queue_no_priority_column(self, mock_console, queue_cmd):
        _populate_queue(2)
        result = queue_cmd._show_queue()
        assert result is True

        printed_args = [str(c) for c in mock_console.print.call_args_list]
        full_output = " ".join(printed_args)
        assert "Priority" not in full_output

    @patch("cai.repl.commands.queue.console")
    def test_get_next_no_priority(self, mock_console, queue_cmd):
        _populate_queue(1)
        result = queue_cmd._get_next()
        assert result is True

        call_args = mock_console.print.call_args[0][0]
        panel_content = str(call_args.renderable)
        assert "Priority" not in panel_content

    def test_add_to_queue_no_priority_key(self, queue_cmd):
        with patch("cai.repl.commands.queue.console"):
            queue_cmd._add_to_queue("test prompt")
        assert len(FALLBACK_QUEUE) == 1
        assert "priority" not in FALLBACK_QUEUE[0]


class TestQueueMoveDispatch:
    """Test that /queue move dispatches correctly through handle()."""

    @patch("cai.repl.commands.queue.console")
    def test_dispatch_move(self, mock_console, queue_cmd):
        _populate_queue(3)
        result = queue_cmd.handle(["move", "3", "1"])
        assert result is True
        assert FALLBACK_QUEUE[0]["prompt"] == "prompt 3"
