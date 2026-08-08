#!/usr/bin/env python3
"""
Backward compatibility tests for TUI streaming.

This test suite ensures that the streaming implementation:
- Doesn't break non-streaming mode
- Works with legacy display methods
- Maintains compatibility with existing CLI commands
- Preserves all existing display features
"""

import os
import sys
import unittest
from typing import Dict, List, Optional, Any
from unittest.mock import MagicMock, patch, Mock
import json

# Add src to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from cai.tui.display.streaming_display import StreamingDisplay
from cai.tui.display.tool_display import ToolDisplay
from cai.tui.display.agent_display import AgentDisplay
from cai.tui.display.manager import DisplayManager
from cai.tui.display.base import DisplayContext, OutputType
from cai.tui.display.panel_formatter import PanelFormatter
from rich.panel import Panel
from rich.console import Console


class TestStreamingBackwardCompatibility(unittest.TestCase):
    """Test backward compatibility of streaming implementation."""
    
    def setUp(self):
        """Set up test environment."""
        # Test both streaming and non-streaming modes
        self.original_stream_env = os.environ.get("CAI_STREAM", "false")
        
        # Create display components
        self.streaming_display = StreamingDisplay()
        self.tool_display = ToolDisplay()
        self.agent_display = AgentDisplay()
        self.display_manager = DisplayManager()
        
        # Mock terminal output
        self.mock_terminal = MagicMock()
        self.mock_terminal.write = MagicMock()
        self.mock_terminal.print = MagicMock()
        
        # Patch get_terminal_output
        self.patcher = patch('cai.tui.core.terminal_console.get_terminal_output')
        self.mock_get_terminal = self.patcher.start()
        self.mock_get_terminal.return_value = self.mock_terminal
        
    def tearDown(self):
        """Clean up."""
        self.patcher.stop()
        os.environ["CAI_STREAM"] = self.original_stream_env
        
    def test_non_streaming_mode_compatibility(self):
        """Test that non-streaming mode still works."""
        os.environ["CAI_STREAM"] = "false"
        
        context = DisplayContext(
            terminal_id="test-1",
            terminal_number=1,
            agent_name="Test Agent",
            interaction_counter=1
        )
        
        # Test direct display methods (non-streaming)
        # Agent message
        self.agent_display.display(context, {
            "messages": [{
                "role": "assistant",
                "content": "This is a non-streaming response"
            }],
            "model": "gpt-4"
        })
        
        # Tool output
        self.tool_display.display(context, {
            "tool_name": "generic_linux_command",
            "args": {"command": "echo", "args": "test"},
            "output": "test",
            "execution_info": {"status": "completed"}
        })
        
        # Verify output was written
        self.assertTrue(self.mock_terminal.write.called)
        self.assertEqual(self.mock_terminal.write.call_count, 2)
        
    def test_legacy_display_methods(self):
        """Test that legacy display methods still work."""
        context = DisplayContext(
            terminal_id="test-2",
            terminal_number=1,
            agent_name="Legacy Test",
            interaction_counter=1
        )
        
        # Test direct panel creation (legacy method)
        panel = PanelFormatter.create_agent_panel(
            agent_name="Legacy Agent",
            message="Testing legacy display",
            metadata={"model": "gpt-3.5-turbo"},
            streaming=False
        )
        
        # Should be able to write panel directly
        self.mock_terminal.write(panel)
        
        # Verify panel was created and written
        self.assertIsInstance(panel, Panel)
        self.assertTrue(self.mock_terminal.write.called)
        
    def test_display_manager_routing(self):
        """Test that display manager correctly routes events."""
        context = DisplayContext(
            terminal_id="test-3",
            terminal_number=1,
            agent_name="Router Test",
            interaction_counter=1
        )
        
        # Test various event types
        events = [
            ('agent_message', {
                "messages": [{"role": "assistant", "content": "Test message"}]
            }),
            ('tool_output', {
                "tool_name": "test_tool",
                "output": "Tool output"
            }),
            ('stream_start', {
                "stream_id": "test-stream",
                "content_type": "text"
            }),
            ('stream_update', {
                "stream_id": "test-stream",
                "content": "Streaming content"
            }),
            ('stream_finish', {
                "stream_id": "test-stream"
            })
        ]
        
        for event_type, data in events:
            # Should not raise any exceptions
            self.display_manager.dispatch(event_type, context, data)
            
    def test_panel_formatter_compatibility(self):
        """Test that panel formatter methods remain compatible."""
        # Test all panel creation methods
        
        # Agent panel
        agent_panel = PanelFormatter.create_agent_panel(
            agent_name="Test Agent",
            message="Test message",
            metadata={"timestamp": "10:00:00"},
            streaming=True
        )
        self.assertIsInstance(agent_panel, Panel)
        
        # Tool panel
        tool_panel = PanelFormatter.create_tool_panel(
            tool_name="generic_linux_command",
            args={"command": "ls"},
            output="file1.txt\nfile2.txt",
            execution_info={"tool_time": 0.5},
            token_info={},
            streaming=False
        )
        self.assertIsInstance(tool_panel, Panel)
        
        # Thinking panel
        thinking_panel = PanelFormatter.create_thinking_panel(
            agent_name="Thinking Agent",
            thinking_content="Processing request...",
            model_name="o1-preview",
            finished=True
        )
        self.assertIsInstance(thinking_panel, Panel)
        
        # Error panel
        error_panel = PanelFormatter.create_error_panel(
            error_type="ValueError",
            error_message="Invalid input",
            details="Expected string, got int"
        )
        self.assertIsInstance(error_panel, Panel)
        
    def test_mixed_streaming_and_non_streaming(self):
        """Test mixing streaming and non-streaming display."""
        context = DisplayContext(
            terminal_id="test-4",
            terminal_number=1,
            agent_name="Mixed Mode",
            interaction_counter=1
        )
        
        # Non-streaming tool output
        self.tool_display.display(context, {
            "tool_name": "read_file",
            "args": {"path": "test.txt"},
            "output": "File contents",
            "execution_info": {"status": "completed"}
        })
        
        # Streaming agent response
        stream_id = "mixed-stream"
        self.streaming_display.start_streaming(context, stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        self.streaming_display.update_streaming(stream_id, {
            "content": "This is a streaming response"
        })
        
        self.streaming_display.finish_streaming(stream_id, {
            "final_stats": {"output_tokens": 5}
        })
        
        # Non-streaming agent message
        self.agent_display.display(context, {
            "messages": [{"role": "assistant", "content": "Final message"}]
        })
        
        # All displays should work without conflicts
        self.assertGreaterEqual(self.mock_terminal.write.call_count, 2)
        
    def test_context_history_preservation(self):
        """Test that context history is preserved with streaming."""
        context = DisplayContext(
            terminal_id="test-5",
            terminal_number=1,
            agent_name="History Test",
            interaction_counter=1
        )
        
        # Add outputs of different types
        context.add_output({
            "type": OutputType.AGENT_MESSAGE,
            "content": "First message"
        })
        
        context.add_output({
            "type": OutputType.TOOL_OUTPUT,
            "tool_name": "test_tool",
            "output": "Tool result"
        })
        
        context.add_output({
            "type": OutputType.STREAMING,
            "content": "Streamed content"
        })
        
        # Verify history is maintained
        self.assertEqual(len(context.outputs), 3)
        self.assertEqual(context.outputs[0]["type"], OutputType.AGENT_MESSAGE)
        self.assertEqual(context.outputs[1]["type"], OutputType.TOOL_OUTPUT)
        self.assertEqual(context.outputs[2]["type"], OutputType.STREAMING)
        
    def test_terminal_output_methods(self):
        """Test both write and print methods on terminal output."""
        context = DisplayContext(
            terminal_id="test-6",
            terminal_number=1,
            agent_name="Terminal Test",
            interaction_counter=1
        )
        
        # Test with terminal that has write method
        terminal_with_write = MagicMock()
        terminal_with_write.write = MagicMock()
        
        with patch('cai.tui.core.terminal_console.get_terminal_output', return_value=terminal_with_write):
            self.agent_display.display(context, {
                "messages": [{"role": "assistant", "content": "Test write method"}]
            })
            
        self.assertTrue(terminal_with_write.write.called)
        
        # Test with terminal that has print method
        terminal_with_print = MagicMock()
        terminal_with_print.print = MagicMock()
        del terminal_with_print.write  # Ensure no write method
        
        with patch('cai.tui.core.terminal_console.get_terminal_output', return_value=terminal_with_print):
            # Create new display instance to test print path
            agent_display = AgentDisplay()
            agent_display.display(context, {
                "messages": [{"role": "assistant", "content": "Test print method"}]
            })
            
        # Should fall back to console methods
        
    def test_streaming_api_presence(self):
        """Test that streaming API doesn't break terminals without it."""
        context = DisplayContext(
            terminal_id="test-7",
            terminal_number=1,
            agent_name="API Test",
            interaction_counter=1
        )
        
        # Terminal without streaming API
        basic_terminal = MagicMock()
        basic_terminal.write = MagicMock()
        # No streaming methods
        
        with patch('cai.tui.core.terminal_console.get_terminal_output', return_value=basic_terminal):
            # Should gracefully handle missing streaming API
            stream_id = "no-api-stream"
            self.streaming_display.start_streaming(context, stream_id, {
                "content_type": "text"
            })
            
            self.streaming_display.update_streaming(stream_id, {
                "content": "Content without streaming API"
            })
            
            self.streaming_display.finish_streaming(stream_id, {})
            
        # Should fall back to regular write
        self.assertTrue(basic_terminal.write.called)
        
    def test_deduplication_still_works(self):
        """Test that deduplication logic is preserved."""
        context = DisplayContext(
            terminal_id="test-8",
            terminal_number=1,
            agent_name="Dedup Test",
            interaction_counter=1
        )
        
        # Display same tool output multiple times
        tool_data = {
            "tool_name": "echo",
            "args": {"text": "duplicate"},
            "output": "duplicate",
            "call_id": "same_call_id"
        }
        
        # First display
        self.tool_display.display(context, tool_data)
        first_call_count = self.mock_terminal.write.call_count
        
        # Immediate duplicate should be skipped
        self.tool_display.display(context, tool_data)
        self.assertEqual(self.mock_terminal.write.call_count, first_call_count)
        
        # After time passes, should display again
        import time
        time.sleep(0.6)  # Wait more than dedup window
        self.tool_display.display(context, tool_data)
        self.assertGreater(self.mock_terminal.write.call_count, first_call_count)
        
    def test_error_handling_compatibility(self):
        """Test error handling remains compatible."""
        context = DisplayContext(
            terminal_id="test-9",
            terminal_number=1,
            agent_name="Error Test",
            interaction_counter=1
        )
        
        # Test with various error conditions
        # Missing required data
        self.agent_display.display(context, {})  # No messages
        
        # Invalid data types
        self.tool_display.display(context, {
            "tool_name": None,
            "output": 123  # Should be string
        })
        
        # Streaming with missing stream_id
        self.streaming_display.update_streaming("non_existent_stream", {
            "content": "Update to non-existent stream"
        })
        
        # None of these should raise exceptions
        # System should handle gracefully
        
    def test_token_stats_compatibility(self):
        """Test token statistics display compatibility."""
        context = DisplayContext(
            terminal_id="test-10",
            terminal_number=1,
            agent_name="Stats Test",
            interaction_counter=1
        )
        
        # Test with full token info
        full_token_info = {
            "interaction_input_tokens": 100,
            "interaction_output_tokens": 50,
            "interaction_cost": 0.0025,
            "session_total_cost": 0.0175,
            "context_usage_pct": 15.5
        }
        
        self.agent_display.display(context, {
            "messages": [{"role": "assistant", "content": "Response with full stats"}],
            "token_info": full_token_info
        })
        
        # Test with partial token info
        partial_token_info = {
            "interaction_input_tokens": 75,
            "interaction_output_tokens": 25
            # Missing costs and context usage
        }
        
        self.agent_display.display(context, {
            "messages": [{"role": "assistant", "content": "Response with partial stats"}],
            "token_info": partial_token_info
        })
        
        # Test with no token info
        self.agent_display.display(context, {
            "messages": [{"role": "assistant", "content": "Response without stats"}]
        })
        
        # All should display without errors
        self.assertGreaterEqual(self.mock_terminal.write.call_count, 3)


if __name__ == "__main__":
    unittest.main(verbosity=2)