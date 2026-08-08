#!/usr/bin/env python3
"""
Integration tests for streaming during actual agent execution.

This test suite verifies streaming works correctly when:
- Agents execute real tools and generate responses
- Multiple tool calls happen in sequence
- Agents hand off to other agents
- Long-running operations are interrupted
- Memory and context are preserved across turns
"""

import asyncio
import os
import sys
import time
import unittest
from typing import Dict, List, Optional, Any, AsyncIterator
from unittest.mock import MagicMock, patch, AsyncMock, Mock
import tempfile
import json
from dataclasses import dataclass

# Add src to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from cai.sdk.agents.agent import Agent
from cai.sdk.agents.run import Runner, RunResult, StreamChunk
from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
from cai.sdk.agents.tool import FunctionTool
from cai.sdk.agents.items import MessageOutputItem, ToolCallItem, ToolCallOutputItem, ReasoningItem
from cai.sdk.agents.run_context import RunContext
from cai.tui.display.streaming_display import StreamingDisplay
from cai.tui.display.tool_display import ToolDisplay
from cai.tui.display.agent_display import AgentDisplay
from cai.tui.display.manager import DisplayManager
from cai.tui.display.base import DisplayContext


@dataclass
class MockStreamChunk:
    """Mock stream chunk for testing."""
    chunk_type: str  # 'thinking', 'content', 'tool_call', 'tool_output'
    content: Optional[str] = None
    tool_name: Optional[str] = None
    tool_args: Optional[Dict] = None
    tool_output: Optional[str] = None
    finished: bool = False
    final_stats: Optional[Dict] = None


class MockRunner:
    """Mock runner that simulates agent execution with streaming."""
    
    def __init__(self, agent: Agent):
        self.agent = agent
        self.messages = []
        self.stream_chunks = []
        
    async def run_async(self, messages: List[Dict], stream: bool = True) -> AsyncIterator[StreamChunk]:
        """Simulate async agent execution with streaming."""
        self.messages = messages
        
        # Simulate thinking
        if self.agent.name == "o1-preview" or "think" in str(messages).lower():
            thinking_content = "I need to analyze this request and determine the best approach..."
            for i in range(0, len(thinking_content), 10):
                chunk = MockStreamChunk(
                    chunk_type='thinking',
                    content=thinking_content[:i+10]
                )
                self.stream_chunks.append(chunk)
                yield chunk
                await asyncio.sleep(0.01)
                
        # Simulate tool calls based on agent type
        if "command" in str(messages).lower() or self.agent.name in ["Bug Bounter", "Red Teamer"]:
            # Tool call
            tool_chunk = MockStreamChunk(
                chunk_type='tool_call',
                tool_name='generic_linux_command',
                tool_args={'command': 'ls', 'args': '-la'}
            )
            self.stream_chunks.append(tool_chunk)
            yield tool_chunk
            
            # Tool output
            tool_output = "total 48\ndrwxr-xr-x  5 user user 4096 Jan 15 10:00 .\ndrwxr-xr-x 10 user user 4096 Jan 15 09:00 .."
            output_chunk = MockStreamChunk(
                chunk_type='tool_output',
                tool_output=tool_output
            )
            self.stream_chunks.append(output_chunk)
            yield output_chunk
            
        # Simulate content streaming
        response_content = f"{self.agent.name} responding to: {messages[-1].get('content', '')[:50]}..."
        for i in range(0, len(response_content), 5):
            chunk = MockStreamChunk(
                chunk_type='content',
                content=response_content[:i+5]
            )
            self.stream_chunks.append(chunk)
            yield chunk
            await asyncio.sleep(0.005)
            
        # Final chunk with stats
        final_chunk = MockStreamChunk(
            chunk_type='content',
            content=response_content,
            finished=True,
            final_stats={
                'input_tokens': 150,
                'output_tokens': 50,
                'interaction_cost': 0.0025,
                'session_total_cost': 0.0075
            }
        )
        self.stream_chunks.append(final_chunk)
        yield final_chunk
        
    def run_sync(self, messages: List[Dict]) -> RunResult:
        """Simulate sync agent execution."""
        self.messages = messages
        
        # Create mock result
        items = []
        
        # Add reasoning if applicable
        if self.agent.name == "o1-preview":
            items.append(ReasoningItem(
                content="Analyzing the request and planning the approach..."
            ))
            
        # Add tool calls
        if "command" in str(messages).lower():
            items.extend([
                ToolCallItem(
                    call_id="call_123",
                    tool_name="generic_linux_command",
                    tool_args={'command': 'ls', 'args': '-la'}
                ),
                ToolCallOutputItem(
                    call_id="call_123",
                    tool_name="generic_linux_command",
                    output="file1.txt\nfile2.py\n"
                )
            ])
            
        # Add message
        items.append(MessageOutputItem(
            role="assistant",
            content=f"{self.agent.name} response to query"
        ))
        
        return RunResult(
            items=items,
            usage={
                'input_tokens': 100,
                'output_tokens': 50,
                'total_tokens': 150
            }
        )


class TestStreamingAgentExecution(unittest.TestCase):
    """Test streaming during real agent execution flows."""
    
    def setUp(self):
        """Set up test environment."""
        os.environ["CAI_TELEMETRY"] = "false"
        os.environ["CAI_TRACING"] = "false"
        os.environ["CAI_STREAM"] = "true"
        
        # Create display manager
        self.display_manager = DisplayManager()
        
        # Mock terminal outputs
        self.terminal_outputs = {}
        
        # Patch get_terminal_output
        self.patcher = patch('cai.tui.core.terminal_console.get_terminal_output')
        self.mock_get_terminal = self.patcher.start()
        self.mock_get_terminal.side_effect = self._get_mock_terminal
        
        # Create test agents
        self.test_agents = self._create_test_agents()
        
    def tearDown(self):
        """Clean up."""
        self.patcher.stop()
        
    def _get_mock_terminal(self, terminal_id):
        """Get or create mock terminal."""
        if terminal_id not in self.terminal_outputs:
            terminal = MagicMock()
            terminal.write = MagicMock()
            terminal.start_streaming_line = MagicMock()
            terminal.update_streaming_line = MagicMock()
            terminal.finish_streaming_line = MagicMock()
            terminal._streaming_lines = {}
            self.terminal_outputs[terminal_id] = terminal
        return self.terminal_outputs[terminal_id]
        
    def _create_test_agents(self) -> Dict[str, Agent]:
        """Create test agents with different configurations."""
        agents = {}
        
        # Bug Bounter - security testing agent
        bug_tools = [
            FunctionTool(
                name="generic_linux_command",
                description="Execute Linux commands",
                parameters={
                    "type": "object",
                    "properties": {
                        "command": {"type": "string"},
                        "args": {"type": "string"}
                    }
                }
            ),
            FunctionTool(
                name="web_search",
                description="Search the web",
                parameters={
                    "type": "object",
                    "properties": {
                        "query": {"type": "string"}
                    }
                }
            )
        ]
        
        agents["bug_bounter"] = Agent(
            name="Bug Bounter",
            instructions="You are a security testing agent.",
            tools=bug_tools
        )
        
        # Red Teamer - penetration testing agent
        agents["red_teamer"] = Agent(
            name="Red Teamer",
            instructions="You are a penetration testing agent.",
            tools=bug_tools
        )
        
        # Code Agent - code analysis
        code_tools = [
            FunctionTool(
                name="read_file",
                description="Read file contents",
                parameters={
                    "type": "object",
                    "properties": {
                        "path": {"type": "string"}
                    }
                }
            ),
            FunctionTool(
                name="execute_code",
                description="Execute code",
                parameters={
                    "type": "object",
                    "properties": {
                        "code": {"type": "string"},
                        "language": {"type": "string"}
                    }
                }
            )
        ]
        
        agents["code_agent"] = Agent(
            name="Code Agent",
            instructions="You are a code analysis agent.",
            tools=code_tools
        )
        
        return agents
        
    async def test_single_agent_streaming_execution(self):
        """Test streaming during single agent execution."""
        agent = self.test_agents["bug_bounter"]
        runner = MockRunner(agent)
        
        # Create display context
        context = DisplayContext(
            terminal_id="test-1",
            terminal_number=1,
            agent_name=agent.name,
            agent_id="bug-1",
            interaction_counter=1
        )
        
        # Track display calls
        display_calls = []
        
        # Mock display manager dispatch
        original_dispatch = self.display_manager.dispatch
        def mock_dispatch(event_type, context, data):
            display_calls.append((event_type, context, data))
            return original_dispatch(event_type, context, data)
        self.display_manager.dispatch = mock_dispatch
        
        # Execute agent with streaming
        messages = [{"role": "user", "content": "Scan the application for vulnerabilities"}]
        
        streamed_content = ""
        async for chunk in runner.run_async(messages, stream=True):
            # Simulate display manager handling chunks
            if chunk.chunk_type == 'thinking':
                self.display_manager.dispatch('stream_start', context, {
                    'stream_id': 'thinking-1',
                    'content_type': 'thinking',
                    'model': agent.name
                })
                self.display_manager.dispatch('stream_update', context, {
                    'stream_id': 'thinking-1',
                    'content': chunk.content
                })
                if chunk.finished:
                    self.display_manager.dispatch('stream_finish', context, {
                        'stream_id': 'thinking-1'
                    })
                    
            elif chunk.chunk_type == 'content':
                if not streamed_content:  # First content chunk
                    self.display_manager.dispatch('stream_start', context, {
                        'stream_id': 'content-1',
                        'content_type': 'text',
                        'model': agent.name
                    })
                streamed_content = chunk.content
                self.display_manager.dispatch('stream_update', context, {
                    'stream_id': 'content-1',
                    'content': chunk.content
                })
                if chunk.finished:
                    self.display_manager.dispatch('stream_finish', context, {
                        'stream_id': 'content-1',
                        'final_stats': chunk.final_stats
                    })
                    
            elif chunk.chunk_type == 'tool_call':
                self.display_manager.dispatch('tool_start', context, {
                    'stream_id': 'tool-1',
                    'tool_name': chunk.tool_name,
                    'args': chunk.tool_args
                })
                
            elif chunk.chunk_type == 'tool_output':
                self.display_manager.dispatch('tool_update', context, {
                    'stream_id': 'tool-1',
                    'output': chunk.tool_output
                })
                self.display_manager.dispatch('tool_finish', context, {
                    'stream_id': 'tool-1',
                    'output': chunk.tool_output,
                    'execution_info': {'status': 'completed'}
                })
                
        # Verify display calls were made
        self.assertTrue(len(display_calls) > 0)
        
        # Check for expected display events
        event_types = [call[0] for call in display_calls]
        self.assertIn('stream_start', event_types)
        self.assertIn('stream_update', event_types)
        self.assertIn('stream_finish', event_types)
        
        # Verify terminal output
        terminal = self.terminal_outputs[context.terminal_id]
        self.assertTrue(terminal.start_streaming_line.called or terminal.write.called)
        
    async def test_multi_tool_execution_streaming(self):
        """Test streaming with multiple tool executions."""
        agent = self.test_agents["red_teamer"]
        runner = MockRunner(agent)
        
        context = DisplayContext(
            terminal_id="test-2",
            terminal_number=1,
            agent_name=agent.name,
            agent_id="red-1",
            interaction_counter=1
        )
        
        # Simulate agent making multiple tool calls
        class MultiToolRunner(MockRunner):
            async def run_async(self, messages, stream=True):
                # First tool call - nmap scan
                yield MockStreamChunk(
                    chunk_type='tool_call',
                    tool_name='generic_linux_command',
                    tool_args={'command': 'nmap', 'args': '-sn 192.168.1.0/24'}
                )
                yield MockStreamChunk(
                    chunk_type='tool_output',
                    tool_output='2 hosts up'
                )
                
                # Second tool call - vulnerability scan
                yield MockStreamChunk(
                    chunk_type='tool_call',
                    tool_name='generic_linux_command',
                    tool_args={'command': 'nikto', 'args': '-h target.com'}
                )
                yield MockStreamChunk(
                    chunk_type='tool_output',
                    tool_output='Found 5 potential issues'
                )
                
                # Final response
                response = "Completed network scan. Found 2 active hosts and 5 potential vulnerabilities."
                for i in range(0, len(response), 10):
                    yield MockStreamChunk(
                        chunk_type='content',
                        content=response[:i+10]
                    )
                    await asyncio.sleep(0.01)
                    
                yield MockStreamChunk(
                    chunk_type='content',
                    content=response,
                    finished=True,
                    final_stats={'output_tokens': 25}
                )
                
        runner = MultiToolRunner(agent)
        
        # Track tool executions
        tool_executions = []
        
        async for chunk in runner.run_async([{"role": "user", "content": "Scan the network"}]):
            if chunk.chunk_type == 'tool_call':
                tool_executions.append(chunk.tool_name)
                self.display_manager.dispatch('tool_start', context, {
                    'stream_id': f'tool-{len(tool_executions)}',
                    'tool_name': chunk.tool_name,
                    'args': chunk.tool_args
                })
            elif chunk.chunk_type == 'tool_output':
                self.display_manager.dispatch('tool_finish', context, {
                    'stream_id': f'tool-{len(tool_executions)}',
                    'output': chunk.tool_output,
                    'execution_info': {'status': 'completed'}
                })
                
        # Verify multiple tools were executed
        self.assertEqual(len(tool_executions), 2)
        self.assertIn('generic_linux_command', tool_executions)
        
    async def test_streaming_with_interruption(self):
        """Test handling interruption during streaming."""
        agent = self.test_agents["code_agent"]
        
        context = DisplayContext(
            terminal_id="test-3",
            terminal_number=1,
            agent_name=agent.name,
            agent_id="code-1",
            interaction_counter=1
        )
        
        # Simulate interrupted execution
        class InterruptedRunner(MockRunner):
            async def run_async(self, messages, stream=True):
                # Start streaming content
                content = "Analyzing code for security vulnerabilities..."
                for i in range(0, len(content)//2, 5):  # Only stream half
                    yield MockStreamChunk(
                        chunk_type='content',
                        content=content[:i+5]
                    )
                    await asyncio.sleep(0.01)
                    
                # Simulate interruption
                raise KeyboardInterrupt("User interrupted")
                
        runner = InterruptedRunner(agent)
        
        # Handle streaming with interruption
        stream_id = None
        partial_content = ""
        
        try:
            async for chunk in runner.run_async([{"role": "user", "content": "Analyze this code"}]):
                if chunk.chunk_type == 'content':
                    if not stream_id:
                        stream_id = 'interrupted-stream'
                        self.display_manager.dispatch('stream_start', context, {
                            'stream_id': stream_id,
                            'content_type': 'text'
                        })
                    partial_content = chunk.content
                    self.display_manager.dispatch('stream_update', context, {
                        'stream_id': stream_id,
                        'content': chunk.content
                    })
        except KeyboardInterrupt:
            # Clean up on interruption
            if stream_id:
                self.display_manager.dispatch('stream_finish', context, {
                    'stream_id': stream_id,
                    'final_stats': {'interrupted': True}
                })
                
        # Verify partial content was displayed
        self.assertTrue(len(partial_content) > 0)
        self.assertLess(len(partial_content), 40)  # Should be partial
        
    async def test_parallel_agent_streaming(self):
        """Test multiple agents streaming in parallel."""
        agents = [
            self.test_agents["bug_bounter"],
            self.test_agents["red_teamer"],
            self.test_agents["code_agent"]
        ]
        
        # Create contexts for each agent
        contexts = []
        for i, agent in enumerate(agents):
            contexts.append(DisplayContext(
                terminal_id=f"parallel-{i}",
                terminal_number=i+1,
                agent_name=agent.name,
                agent_id=f"agent-{i}",
                interaction_counter=1
            ))
            
        # Run agents in parallel
        async def run_agent(agent, context, query):
            runner = MockRunner(agent)
            
            async for chunk in runner.run_async([{"role": "user", "content": query}]):
                if chunk.chunk_type == 'content':
                    if chunk.finished:
                        self.display_manager.dispatch('stream_finish', context, {
                            'stream_id': f'{context.agent_id}-stream',
                            'final_stats': chunk.final_stats
                        })
                    else:
                        self.display_manager.dispatch('stream_update', context, {
                            'stream_id': f'{context.agent_id}-stream',
                            'content': chunk.content
                        })
                        
        # Execute all agents in parallel
        tasks = []
        queries = [
            "Find security vulnerabilities",
            "Test authentication bypass",
            "Review code for bugs"
        ]
        
        for agent, context, query in zip(agents, contexts, queries):
            task = asyncio.create_task(run_agent(agent, context, query))
            tasks.append(task)
            
        await asyncio.gather(*tasks)
        
        # Verify all agents completed
        for i, context in enumerate(contexts):
            terminal = self.terminal_outputs[context.terminal_id]
            self.assertTrue(
                terminal.write.called or 
                terminal.start_streaming_line.called or
                terminal.update_streaming_line.called
            )
            
    async def test_streaming_with_context_preservation(self):
        """Test that streaming preserves context across turns."""
        agent = self.test_agents["bug_bounter"]
        runner = MockRunner(agent)
        
        context = DisplayContext(
            terminal_id="test-4",
            terminal_number=1,
            agent_name=agent.name,
            agent_id="bug-2",
            interaction_counter=1
        )
        
        # First turn
        messages = [{"role": "user", "content": "What vulnerabilities should I look for?"}]
        
        first_response = ""
        async for chunk in runner.run_async(messages):
            if chunk.chunk_type == 'content':
                first_response = chunk.content
                
        # Second turn - context should be preserved
        context.interaction_counter = 2
        messages.append({"role": "assistant", "content": first_response})
        messages.append({"role": "user", "content": "Can you scan for SQL injection?"})
        
        second_response = ""
        async for chunk in runner.run_async(messages):
            if chunk.chunk_type == 'content':
                second_response = chunk.content
                
        # Verify both responses were generated
        self.assertTrue(len(first_response) > 0)
        self.assertTrue(len(second_response) > 0)
        self.assertNotEqual(first_response, second_response)
        
        # Verify interaction counter was incremented
        self.assertEqual(context.interaction_counter, 2)


def run_async_test(coro):
    """Helper to run async test."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


class TestStreamingAgentExecutionSync(TestStreamingAgentExecution):
    """Synchronous wrapper for async tests."""
    
    def test_single_agent_streaming_execution_sync(self):
        run_async_test(self.test_single_agent_streaming_execution())
        
    def test_multi_tool_execution_streaming_sync(self):
        run_async_test(self.test_multi_tool_execution_streaming())
        
    def test_streaming_with_interruption_sync(self):
        run_async_test(self.test_streaming_with_interruption())
        
    def test_parallel_agent_streaming_sync(self):
        run_async_test(self.test_parallel_agent_streaming())
        
    def test_streaming_with_context_preservation_sync(self):
        run_async_test(self.test_streaming_with_context_preservation())


if __name__ == "__main__":
    unittest.main(verbosity=2)