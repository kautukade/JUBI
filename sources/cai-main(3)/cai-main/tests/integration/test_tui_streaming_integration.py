#!/usr/bin/env python3
"""
Integration tests for CAI TUI streaming functionality.

This test suite ensures streaming works correctly with:
- Different agent types (bug_bounter, red_teamer, codeagent, etc.)
- Various tools (command execution, web search, code interpretation)
- Error handling and interruptions
- Different content types (thinking, text, code)
- Multi-turn conversations
- Parallel agent execution
"""

import asyncio
import os
import sys
import time
import unittest
from typing import Dict, List, Optional, Any
from unittest.mock import MagicMock, patch, AsyncMock
import tempfile
import json

# Add src to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from cai.tui.display.streaming_display import StreamingDisplay
from cai.tui.display.tool_display import ToolDisplay
from cai.tui.display.agent_display import AgentDisplay
from cai.tui.display.base import DisplayContext
from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
from cai.sdk.agents.agent import Agent
from cai.sdk.agents.run import RunResult
from cai.sdk.agents.items import MessageOutputItem, ToolCallItem, ReasoningItem
from cai.agents import get_agent_by_name, get_all_agents


class MockTerminalOutput:
    """Mock terminal output for testing."""
    
    def __init__(self):
        self.output_lines = []
        self.streaming_lines = {}
        self.panels_written = []
        
    def write(self, content):
        """Write content to output."""
        self.output_lines.append(content)
        self.panels_written.append(content)
        
    def start_streaming_line(self, line_id, header):
        """Start a streaming line."""
        self.streaming_lines[line_id] = {
            "header": header,
            "content": "",
            "finished": False,
            "updates": []
        }
        
    def update_streaming_line(self, line_id, content):
        """Update a streaming line."""
        if line_id in self.streaming_lines:
            self.streaming_lines[line_id]["content"] = content
            self.streaming_lines[line_id]["updates"].append(content)
            
    def finish_streaming_line(self, line_id, final_content, stats=None):
        """Finish a streaming line."""
        if line_id in self.streaming_lines:
            self.streaming_lines[line_id]["content"] = final_content
            self.streaming_lines[line_id]["finished"] = True
            self.streaming_lines[line_id]["stats"] = stats


class TestTUIStreamingIntegration(unittest.TestCase):
    """Integration tests for TUI streaming."""
    
    @classmethod
    def setUpClass(cls):
        """Set up test environment."""
        os.environ["CAI_TELEMETRY"] = "false"
        os.environ["CAI_TRACING"] = "false"
        os.environ["CAI_STREAM"] = "true"
        os.environ["OPENAI_API_KEY"] = os.environ.get("OPENAI_API_KEY", "test-key")
        
    def setUp(self):
        """Set up for each test."""
        self.terminal_outputs = {}
        self.streaming_display = StreamingDisplay()
        self.tool_display = ToolDisplay()
        self.agent_display = AgentDisplay()
        
        # Patch get_terminal_output
        self.patcher = patch('cai.tui.core.terminal_console.get_terminal_output')
        self.mock_get_terminal = self.patcher.start()
        self.mock_get_terminal.side_effect = self._get_mock_terminal
        
    def tearDown(self):
        """Clean up after each test."""
        self.patcher.stop()
        
    def _get_mock_terminal(self, terminal_id):
        """Get or create mock terminal output."""
        if terminal_id not in self.terminal_outputs:
            self.terminal_outputs[terminal_id] = MockTerminalOutput()
        return self.terminal_outputs[terminal_id]
        
    def _create_context(self, terminal_id="test-1", agent_name="Test Agent", interaction=1):
        """Create a display context."""
        return DisplayContext(
            terminal_id=terminal_id,
            terminal_number=1,
            agent_name=agent_name,
            agent_id=f"agent-{terminal_id}",
            interaction_counter=interaction
        )
        
    async def test_agent_text_streaming(self):
        """Test streaming text responses from agents."""
        context = self._create_context(agent_name="Bug Bounter")
        stream_id = "text-stream-1"
        
        # Start streaming
        self.streaming_display.start_streaming(context, stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        # Simulate streaming a response
        test_response = "I've analyzed the application and found several potential security issues:\n\n1. SQL Injection vulnerability in the login form\n2. Cross-Site Scripting (XSS) in user comments\n3. Insecure direct object references in API endpoints"
        
        # Stream in chunks
        chunk_size = 20
        for i in range(0, len(test_response), chunk_size):
            chunk = test_response[:i+chunk_size]
            self.streaming_display.update_streaming(stream_id, {"content": chunk})
            await asyncio.sleep(0.01)
            
        # Finish streaming
        self.streaming_display.finish_streaming(stream_id, {
            "final_stats": {
                "input_tokens": 250,
                "output_tokens": 85,
                "interaction_cost": 0.0035,
                "session_total_cost": 0.0125,
                "context_usage_pct": 2.5
            }
        })
        
        # Verify output
        terminal = self.terminal_outputs[context.terminal_id]
        self.assertTrue(len(terminal.streaming_lines) > 0)
        
        # Check streaming line was created and finished
        stream_line = list(terminal.streaming_lines.values())[0]
        self.assertTrue(stream_line["finished"])
        self.assertIn("Bug Bounter", stream_line["header"])
        self.assertEqual(stream_line["content"], test_response.replace('\n', ' ').replace('\t', ' '))
        
    async def test_agent_thinking_streaming(self):
        """Test streaming thinking/reasoning content."""
        context = self._create_context(agent_name="Red Teamer")
        stream_id = "thinking-stream-1"
        
        # Start thinking stream
        self.streaming_display.start_streaming(context, stream_id, {
            "content_type": "thinking",
            "model": "o1-preview"
        })
        
        # Stream thinking content
        thinking = "The user wants me to test the application's authentication system. I should start by examining the login endpoints and checking for common vulnerabilities like weak password policies, brute force protection, and session management issues."
        
        for i in range(0, len(thinking), 15):
            chunk = thinking[:i+15]
            self.streaming_display.update_streaming(stream_id, {"content": chunk})
            await asyncio.sleep(0.005)
            
        # Finish thinking
        self.streaming_display.finish_streaming(stream_id, {})
        
        # Verify thinking was displayed
        terminal = self.terminal_outputs[context.terminal_id]
        # Thinking displays as panels, not streaming lines
        self.assertTrue(len(terminal.panels_written) > 0)
        
        # Check for thinking panel
        thinking_panel_found = False
        for panel in terminal.panels_written:
            if isinstance(panel, str) and "thinking" in str(panel).lower():
                thinking_panel_found = True
                break
        self.assertTrue(thinking_panel_found)
        
    async def test_tool_execution_display(self):
        """Test tool execution display with streaming."""
        context = self._create_context(agent_name="Network Analyzer")
        
        # Test command execution tool
        command_data = {
            "tool_name": "generic_linux_command",
            "args": {"command": "nmap", "args": "-sn 192.168.1.0/24"},
            "call_id": "call_nmap_123"
        }
        
        # Start tool streaming
        stream_id = "tool-stream-1"
        self.tool_display.start_streaming(context, stream_id, command_data)
        
        # Simulate streaming output
        output_lines = [
            "Starting Nmap 7.92 ( https://nmap.org )",
            "Nmap scan report for 192.168.1.1",
            "Host is up (0.0023s latency).",
            "Nmap scan report for 192.168.1.100",
            "Host is up (0.0045s latency).",
            "Nmap done: 256 IP addresses (2 hosts up) scanned in 3.42 seconds"
        ]
        
        accumulated_output = ""
        for line in output_lines:
            accumulated_output += line + "\n"
            self.tool_display.update_streaming(stream_id, {"output": accumulated_output})
            await asyncio.sleep(0.02)
            
        # Finish tool execution
        self.tool_display.finish_streaming(stream_id, {
            "output": accumulated_output,
            "execution_info": {
                "status": "completed",
                "tool_time": 3.42,
                "exit_code": 0
            }
        })
        
        # Verify tool panel was created
        terminal = self.terminal_outputs[context.terminal_id]
        self.assertTrue(len(terminal.panels_written) > 0)
        
        # Check for tool panel
        tool_panel_found = False
        for panel in terminal.panels_written:
            if "generic_linux_command" in str(panel) or "nmap" in str(panel):
                tool_panel_found = True
                break
        self.assertTrue(tool_panel_found)
        
    async def test_error_handling_during_streaming(self):
        """Test error handling during streaming."""
        context = self._create_context(agent_name="Error Test Agent")
        
        # Test tool error
        error_data = {
            "tool_name": "web_search",
            "args": {"query": "test query"},
            "call_id": "call_error_456"
        }
        
        stream_id = "error-stream-1"
        self.tool_display.start_streaming(context, stream_id, error_data)
        
        # Simulate error
        error_output = "Error: Failed to connect to search API - Connection timeout"
        self.tool_display.update_streaming(stream_id, {"output": error_output})
        
        # Finish with error
        self.tool_display.finish_streaming(stream_id, {
            "output": error_output,
            "execution_info": {
                "status": "error",
                "error": "Connection timeout",
                "tool_time": 30.0
            }
        })
        
        # Verify error was displayed
        terminal = self.terminal_outputs[context.terminal_id]
        error_found = False
        for panel in terminal.panels_written:
            if "error" in str(panel).lower():
                error_found = True
                break
        self.assertTrue(error_found)
        
    async def test_multi_turn_conversation_streaming(self):
        """Test streaming in multi-turn conversations."""
        context = self._create_context(agent_name="Code Agent")
        
        # Turn 1: User asks for code review
        turn1_stream_id = "turn1-stream"
        self.streaming_display.start_streaming(context, turn1_stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        response1 = "I'll review your code for security vulnerabilities. Let me analyze the file."
        self.streaming_display.update_streaming(turn1_stream_id, {"content": response1})
        self.streaming_display.finish_streaming(turn1_stream_id, {
            "final_stats": {"output_tokens": 15}
        })
        
        # Tool execution
        tool_stream_id = "code-analysis-stream"
        self.tool_display.start_streaming(context, tool_stream_id, {
            "tool_name": "read_file",
            "args": {"path": "app.py"},
            "call_id": "call_read_789"
        })
        
        code_content = """def login(username, password):
    query = f"SELECT * FROM users WHERE username='{username}' AND password='{password}'"
    result = db.execute(query)
    return result"""
        
        self.tool_display.finish_streaming(tool_stream_id, {
            "output": code_content,
            "execution_info": {"status": "completed"}
        })
        
        # Turn 2: Agent provides analysis
        context.interaction_counter = 2
        turn2_stream_id = "turn2-stream"
        self.streaming_display.start_streaming(context, turn2_stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        analysis = "I found a critical SQL injection vulnerability in your login function. The query uses string formatting with user input directly, allowing attackers to inject malicious SQL."
        
        # Stream the analysis
        for i in range(0, len(analysis), 10):
            chunk = analysis[:i+10]
            self.streaming_display.update_streaming(turn2_stream_id, {"content": chunk})
            await asyncio.sleep(0.01)
            
        self.streaming_display.finish_streaming(turn2_stream_id, {
            "final_stats": {
                "input_tokens": 320,
                "output_tokens": 45,
                "interaction_cost": 0.0042
            }
        })
        
        # Verify multi-turn output
        terminal = self.terminal_outputs[context.terminal_id]
        self.assertTrue(len(terminal.streaming_lines) >= 2)  # At least 2 streaming responses
        self.assertTrue(len(terminal.panels_written) >= 1)  # At least 1 tool panel
        
    async def test_parallel_agents_streaming(self):
        """Test streaming with multiple parallel agents."""
        # Create contexts for parallel agents
        contexts = []
        for i in range(3):
            contexts.append(self._create_context(
                terminal_id=f"parallel-{i}",
                agent_name=f"Agent {i+1}",
                interaction=1
            ))
            
        # Define agent tasks
        async def stream_agent_response(context, agent_num):
            stream_id = f"parallel-stream-{agent_num}"
            
            # Start streaming
            self.streaming_display.start_streaming(context, stream_id, {
                "content_type": "text",
                "model": "gpt-4"
            })
            
            # Different responses for each agent
            responses = [
                "Scanning network for open ports and services...",
                "Analyzing application for XSS vulnerabilities...",
                "Testing authentication bypass techniques..."
            ]
            
            response = responses[agent_num % len(responses)]
            
            # Stream response
            for i in range(0, len(response), 5):
                chunk = response[:i+5]
                self.streaming_display.update_streaming(stream_id, {"content": chunk})
                await asyncio.sleep(0.01)
                
            # Finish streaming
            self.streaming_display.finish_streaming(stream_id, {
                "final_stats": {
                    "output_tokens": 20 + agent_num * 5,
                    "interaction_cost": 0.001 * (agent_num + 1)
                }
            })
            
        # Run parallel streaming
        tasks = []
        for i, context in enumerate(contexts):
            task = asyncio.create_task(stream_agent_response(context, i))
            tasks.append(task)
            
        await asyncio.gather(*tasks)
        
        # Verify all agents streamed successfully
        for i, context in enumerate(contexts):
            terminal = self.terminal_outputs[context.terminal_id]
            self.assertTrue(len(terminal.streaming_lines) > 0)
            
            # Check streaming completed
            for line_data in terminal.streaming_lines.values():
                self.assertTrue(line_data["finished"])
                
    async def test_streaming_with_special_characters(self):
        """Test streaming with special characters and formatting."""
        context = self._create_context(agent_name="Format Test Agent")
        
        # Test with various special content
        test_cases = [
            {
                "name": "markdown",
                "content": "# Security Report\n\n**Critical Issues:**\n- SQL Injection\n- XSS vulnerability\n\n```python\ncode_example()\n```"
            },
            {
                "name": "unicode",
                "content": "Testing unicode: 你好世界 🔒 Security ⚠️ Warning"
            },
            {
                "name": "ansi_escape",
                "content": "Status: \033[32mPASSED\033[0m | Risk: \033[31mHIGH\033[0m"
            }
        ]
        
        for i, test_case in enumerate(test_cases):
            stream_id = f"special-stream-{i}"
            
            # Start streaming
            self.streaming_display.start_streaming(context, stream_id, {
                "content_type": "text",
                "model": "gpt-4"
            })
            
            # Stream content
            content = test_case["content"]
            self.streaming_display.update_streaming(stream_id, {"content": content})
            
            # Finish streaming
            self.streaming_display.finish_streaming(stream_id, {})
            
            # Verify content was processed
            terminal = self.terminal_outputs[context.terminal_id]
            
            # Content should be cleaned for single-line display
            stream_line = list(terminal.streaming_lines.values())[-1]
            self.assertFalse('\n' in stream_line["content"])  # Newlines replaced
            self.assertFalse('\t' in stream_line["content"])  # Tabs replaced
            
    async def test_streaming_interruption(self):
        """Test handling streaming interruption (simulated Ctrl+C)."""
        context = self._create_context(agent_name="Interrupt Test Agent")
        
        # Start streaming
        stream_id = "interrupt-stream"
        self.streaming_display.start_streaming(context, stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        # Stream partial content
        partial_content = "Analyzing security vulnerabilities in the application..."
        self.streaming_display.update_streaming(stream_id, {"content": partial_content})
        
        # Simulate interruption - just finish early
        self.streaming_display.finish_streaming(stream_id, {
            "final_stats": {
                "output_tokens": 8,
                "interrupted": True
            }
        })
        
        # Verify partial content was displayed
        terminal = self.terminal_outputs[context.terminal_id]
        stream_line = list(terminal.streaming_lines.values())[0]
        self.assertTrue(stream_line["finished"])
        self.assertEqual(stream_line["content"], partial_content)
        
    async def test_streaming_performance(self):
        """Test streaming performance with rapid updates."""
        context = self._create_context(agent_name="Performance Test Agent")
        stream_id = "perf-stream"
        
        # Start streaming
        self.streaming_display.start_streaming(context, stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        # Rapid updates
        start_time = time.time()
        update_count = 100
        content = ""
        
        for i in range(update_count):
            content += f"Update {i} "
            self.streaming_display.update_streaming(stream_id, {"content": content})
            await asyncio.sleep(0.001)  # 1ms between updates
            
        # Finish streaming
        self.streaming_display.finish_streaming(stream_id, {})
        
        elapsed = time.time() - start_time
        updates_per_second = update_count / elapsed
        
        # Verify performance
        self.assertGreater(updates_per_second, 50)  # Should handle at least 50 updates/sec
        
        # Verify all updates were processed
        terminal = self.terminal_outputs[context.terminal_id]
        stream_line = list(terminal.streaming_lines.values())[0]
        self.assertTrue(stream_line["finished"])
        self.assertIn(f"Update {update_count-1}", stream_line["content"])
        
    async def test_agent_specific_features(self):
        """Test streaming with agent-specific features."""
        # Test Bug Bounter with vulnerability findings
        bug_context = self._create_context(agent_name="Bug Bounter")
        bug_stream_id = "bug-stream"
        
        self.streaming_display.start_streaming(bug_context, bug_stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        
        vuln_report = "Found SQL Injection vulnerability with CVSS score 9.8 (Critical)"
        self.streaming_display.update_streaming(bug_stream_id, {"content": vuln_report})
        self.streaming_display.finish_streaming(bug_stream_id, {})
        
        # Test Red Teamer with exploit code
        red_context = self._create_context(agent_name="Red Teamer")
        
        # Display code execution
        code_data = {
            "tool_name": "execute_code",
            "args": {
                "code": "import requests\n\n# Exploit code here\nresponse = requests.get('http://target.com')",
                "language": "python"
            },
            "call_id": "call_exploit_123"
        }
        
        code_stream_id = "code-stream"
        self.tool_display.start_streaming(red_context, code_stream_id, code_data)
        self.tool_display.finish_streaming(code_stream_id, {
            "output": "Exploit executed successfully",
            "execution_info": {"status": "completed"}
        })
        
        # Verify agent-specific output
        bug_terminal = self.terminal_outputs[bug_context.terminal_id]
        red_terminal = self.terminal_outputs[red_context.terminal_id]
        
        # Bug Bounter should have vulnerability info
        bug_line = list(bug_terminal.streaming_lines.values())[0]
        self.assertIn("SQL Injection", bug_line["content"])
        self.assertIn("CVSS", bug_line["content"])
        
        # Red Teamer should have code panel
        code_panel_found = False
        for panel in red_terminal.panels_written:
            if "execute_code" in str(panel) or "python" in str(panel):
                code_panel_found = True
                break
        self.assertTrue(code_panel_found)


def run_async_test(coro):
    """Helper to run async test."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


class TestTUIStreamingIntegrationSync(TestTUIStreamingIntegration):
    """Synchronous wrapper for async tests."""
    
    def test_agent_text_streaming_sync(self):
        run_async_test(self.test_agent_text_streaming())
        
    def test_agent_thinking_streaming_sync(self):
        run_async_test(self.test_agent_thinking_streaming())
        
    def test_tool_execution_display_sync(self):
        run_async_test(self.test_tool_execution_display())
        
    def test_error_handling_during_streaming_sync(self):
        run_async_test(self.test_error_handling_during_streaming())
        
    def test_multi_turn_conversation_streaming_sync(self):
        run_async_test(self.test_multi_turn_conversation_streaming())
        
    def test_parallel_agents_streaming_sync(self):
        run_async_test(self.test_parallel_agents_streaming())
        
    def test_streaming_with_special_characters_sync(self):
        run_async_test(self.test_streaming_with_special_characters())
        
    def test_streaming_interruption_sync(self):
        run_async_test(self.test_streaming_interruption())
        
    def test_streaming_performance_sync(self):
        run_async_test(self.test_streaming_performance())
        
    def test_agent_specific_features_sync(self):
        run_async_test(self.test_agent_specific_features())


if __name__ == "__main__":
    unittest.main(verbosity=2)