# CAI TUI Streaming Integration Tests

This directory contains comprehensive integration tests for the CAI TUI streaming functionality. These tests ensure that the streaming implementation works correctly across all parts of the system.

## Test Files

### 1. `test_tui_streaming_integration.py`
Tests the core streaming functionality with different CAI components:
- **Agent Text Streaming**: Verifies streaming of agent responses
- **Thinking Display**: Tests streaming of reasoning/thinking content
- **Tool Execution**: Validates tool output streaming
- **Error Handling**: Ensures errors are displayed correctly during streaming
- **Multi-turn Conversations**: Tests streaming across multiple interaction turns
- **Parallel Agents**: Verifies concurrent streaming from multiple agents
- **Special Characters**: Tests handling of Unicode, ANSI codes, and formatting
- **Interruption Handling**: Simulates Ctrl+C during streaming
- **Performance**: Basic performance validation
- **Agent-specific Features**: Tests streaming with different agent types

### 2. `test_streaming_agent_execution.py`
Tests streaming during actual agent execution flows:
- **Single Agent Execution**: Full agent run with streaming
- **Multi-tool Execution**: Multiple tool calls in sequence
- **Interruption During Execution**: Handling KeyboardInterrupt
- **Parallel Agent Execution**: Multiple agents running concurrently
- **Context Preservation**: Maintaining state across turns
- **Mock Agent Framework**: Simulates real agent behavior

### 3. `test_streaming_backward_compatibility.py`
Ensures the streaming implementation doesn't break existing functionality:
- **Non-streaming Mode**: Verifies CAI_STREAM=false still works
- **Legacy Display Methods**: Direct panel creation compatibility
- **Display Manager Routing**: Event routing validation
- **Panel Formatter**: All panel types remain functional
- **Mixed Mode**: Streaming and non-streaming together
- **Context History**: Output history preservation
- **Terminal Methods**: Both write() and print() support
- **Graceful Fallbacks**: Missing streaming API handling
- **Deduplication**: Existing deduplication logic preserved
- **Error Handling**: Graceful error handling maintained
- **Token Statistics**: All stats display correctly

### 4. `test_streaming_performance.py`
Performance testing to ensure streaming doesn't introduce regressions:
- **Latency Testing**: Measures operation latencies
- **Concurrent Streams**: Performance with multiple streams
- **Rapid Updates**: Handling high-frequency updates
- **Memory Leak Prevention**: Validates no memory leaks
- **Responsiveness Under Load**: UI remains responsive
- **Large Content**: Streaming large amounts of data
- **Throughput Testing**: Measures data throughput

## Running the Tests

### Run All Integration Tests
```bash
# From project root
python -m pytest tests/integration/test_*streaming*.py -v
```

### Run Individual Test Files
```bash
# Core functionality tests
python tests/integration/test_tui_streaming_integration.py

# Agent execution tests
python tests/integration/test_streaming_agent_execution.py

# Backward compatibility tests
python tests/integration/test_streaming_backward_compatibility.py

# Performance tests
python tests/integration/test_streaming_performance.py
```

### Run Specific Test Methods
```bash
# Run a specific test
python -m pytest tests/integration/test_tui_streaming_integration.py::TestTUIStreamingIntegrationSync::test_agent_text_streaming_sync -v

# Run tests matching a pattern
python -m pytest tests/integration -k "parallel" -v
```

## Test Coverage

The integration tests cover:

1. **Display Components**
   - StreamingDisplay
   - ToolDisplay
   - AgentDisplay
   - DisplayManager
   - PanelFormatter

2. **Agent Types**
   - Bug Bounter (security testing)
   - Red Teamer (penetration testing)
   - Code Agent (code analysis)
   - Network Analyzer
   - Generic agents

3. **Tool Types**
   - generic_linux_command
   - web_search
   - read_file
   - execute_code
   - Custom tools

4. **Content Types**
   - Text streaming
   - Thinking/reasoning
   - Tool output
   - Error messages
   - Code display

5. **Edge Cases**
   - Empty content
   - Very long content
   - Special characters
   - Unicode text
   - ANSI escape codes
   - Markdown formatting
   - Rapid updates
   - Interruptions

## Performance Benchmarks

Expected performance characteristics:
- **Update Latency**: < 5ms average, < 10ms p95
- **Concurrent Streams**: > 100 updates/second with 10 streams
- **Rapid Updates**: > 1000 updates/second single stream
- **Memory Growth**: < 10MB for 100 streams
- **Responsiveness**: < 50ms max latency under load
- **Throughput**: > 1MB/s for large content

## Debugging Failed Tests

If tests fail, check:

1. **Environment Variables**
   ```bash
   export CAI_TELEMETRY=false
   export CAI_TRACING=false
   export CAI_STREAM=true  # or false for non-streaming tests
   ```

2. **Mock Terminal Output**
   - Tests use mock terminals that simulate the real UniversalTerminal
   - Check that streaming API methods are called correctly

3. **Async Execution**
   - Some tests use asyncio for concurrent testing
   - Ensure event loop is properly configured

4. **Performance Requirements**
   - Performance tests may fail on slower systems
   - Adjust thresholds if needed for CI/CD environments

## Adding New Tests

When adding new integration tests:

1. **Use Consistent Patterns**
   - Create DisplayContext for each test
   - Mock terminal output appropriately
   - Test both streaming start/update/finish lifecycle

2. **Test Real Scenarios**
   - Base tests on actual CAI usage patterns
   - Include multi-turn conversations
   - Test error conditions

3. **Verify Output**
   - Check that content is displayed correctly
   - Verify streaming lines are created and finished
   - Ensure panels are formatted properly

4. **Clean Up Resources**
   - Stop any background threads
   - Clear mock state between tests
   - Restore environment variables

## CI/CD Integration

These tests should be run in CI/CD pipelines:

```yaml
# Example GitHub Actions workflow
- name: Run Streaming Integration Tests
  run: |
    python -m pytest tests/integration/test_*streaming*.py \
      --junit-xml=test-results/streaming-integration.xml \
      --cov=src/cai/tui/display \
      --cov-report=xml
```

## Related Documentation

- [Streaming Architecture](../../SINGLE_LINE_STREAMING_IMPLEMENTATION.md)
- [TUI Architecture](../../TUI_ARCHITECTURE_ANALYSIS.md)
- [Streaming Fix Summary](../../FINAL_STREAMING_FIX_SUMMARY.md)