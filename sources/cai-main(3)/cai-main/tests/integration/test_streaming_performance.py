#!/usr/bin/env python3
"""
Performance tests for TUI streaming.

This test suite ensures that streaming:
- Doesn't introduce significant latency
- Handles high-frequency updates efficiently
- Doesn't leak memory
- Scales with multiple concurrent streams
- Maintains responsiveness under load
"""

import asyncio
import gc
import os
import sys
import time
import unittest
import tracemalloc
from typing import Dict, List, Optional, Any
from unittest.mock import MagicMock, patch
import psutil
import threading

# Add src to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from cai.tui.display.streaming_display import StreamingDisplay
from cai.tui.display.tool_display import ToolDisplay
from cai.tui.display.agent_display import AgentDisplay
from cai.tui.display.manager import DisplayManager
from cai.tui.display.base import DisplayContext


class PerformanceMetrics:
    """Track performance metrics."""
    
    def __init__(self):
        self.latencies = []
        self.memory_snapshots = []
        self.cpu_usage = []
        self.update_counts = {}
        self.start_time = time.time()
        
    def record_latency(self, operation: str, duration: float):
        """Record operation latency."""
        self.latencies.append({
            'operation': operation,
            'duration': duration,
            'timestamp': time.time() - self.start_time
        })
        
    def record_memory(self):
        """Record current memory usage."""
        process = psutil.Process()
        self.memory_snapshots.append({
            'rss': process.memory_info().rss,
            'vms': process.memory_info().vms,
            'timestamp': time.time() - self.start_time
        })
        
    def record_cpu(self):
        """Record CPU usage."""
        process = psutil.Process()
        self.cpu_usage.append({
            'percent': process.cpu_percent(interval=0.1),
            'timestamp': time.time() - self.start_time
        })
        
    def increment_update(self, stream_id: str):
        """Increment update count for a stream."""
        self.update_counts[stream_id] = self.update_counts.get(stream_id, 0) + 1
        
    def get_summary(self) -> Dict[str, Any]:
        """Get performance summary."""
        if not self.latencies:
            return {'error': 'No metrics recorded'}
            
        latency_by_op = {}
        for metric in self.latencies:
            op = metric['operation']
            if op not in latency_by_op:
                latency_by_op[op] = []
            latency_by_op[op].append(metric['duration'])
            
        summary = {
            'total_duration': time.time() - self.start_time,
            'total_updates': sum(self.update_counts.values()),
            'latency_stats': {}
        }
        
        for op, durations in latency_by_op.items():
            summary['latency_stats'][op] = {
                'count': len(durations),
                'avg': sum(durations) / len(durations),
                'min': min(durations),
                'max': max(durations),
                'p95': sorted(durations)[int(len(durations) * 0.95)] if len(durations) > 1 else durations[0]
            }
            
        if self.memory_snapshots:
            initial_mem = self.memory_snapshots[0]['rss']
            final_mem = self.memory_snapshots[-1]['rss']
            summary['memory'] = {
                'initial_mb': initial_mem / 1024 / 1024,
                'final_mb': final_mem / 1024 / 1024,
                'growth_mb': (final_mem - initial_mem) / 1024 / 1024
            }
            
        if self.cpu_usage:
            cpu_values = [c['percent'] for c in self.cpu_usage]
            summary['cpu'] = {
                'avg_percent': sum(cpu_values) / len(cpu_values),
                'max_percent': max(cpu_values)
            }
            
        return summary


class TestStreamingPerformance(unittest.TestCase):
    """Test streaming performance characteristics."""
    
    def setUp(self):
        """Set up test environment."""
        os.environ["CAI_TELEMETRY"] = "false"
        os.environ["CAI_TRACING"] = "false"
        os.environ["CAI_STREAM"] = "true"
        
        self.metrics = PerformanceMetrics()
        self.display_manager = DisplayManager()
        
        # Mock terminal with performance tracking
        self.mock_terminal = self._create_performance_terminal()
        
        # Patch get_terminal_output
        self.patcher = patch('cai.tui.core.terminal_console.get_terminal_output')
        self.mock_get_terminal = self.patcher.start()
        self.mock_get_terminal.return_value = self.mock_terminal
        
    def tearDown(self):
        """Clean up."""
        self.patcher.stop()
        
    def _create_performance_terminal(self):
        """Create mock terminal that tracks performance."""
        terminal = MagicMock()
        
        def write_with_metrics(content):
            start = time.time()
            # Simulate write operation
            time.sleep(0.0001)  # 0.1ms write time
            self.metrics.record_latency('terminal_write', time.time() - start)
            
        def start_streaming_with_metrics(line_id, header):
            start = time.time()
            self.metrics.record_latency('stream_start', time.time() - start)
            
        def update_streaming_with_metrics(line_id, content):
            start = time.time()
            self.metrics.increment_update(line_id)
            # Simulate update processing
            time.sleep(0.0001)
            self.metrics.record_latency('stream_update', time.time() - start)
            
        def finish_streaming_with_metrics(line_id, content, stats=None):
            start = time.time()
            self.metrics.record_latency('stream_finish', time.time() - start)
            
        terminal.write = MagicMock(side_effect=write_with_metrics)
        terminal.start_streaming_line = MagicMock(side_effect=start_streaming_with_metrics)
        terminal.update_streaming_line = MagicMock(side_effect=update_streaming_with_metrics)
        terminal.finish_streaming_line = MagicMock(side_effect=finish_streaming_with_metrics)
        
        return terminal
        
    def test_single_stream_latency(self):
        """Test latency of single stream operations."""
        display = StreamingDisplay()
        context = DisplayContext(
            terminal_id="perf-1",
            terminal_number=1,
            agent_name="Latency Test",
            interaction_counter=1
        )
        
        # Record initial memory
        self.metrics.record_memory()
        
        # Measure streaming operations
        stream_id = "latency-stream"
        
        # Start streaming
        start = time.time()
        display.start_streaming(context, stream_id, {
            "content_type": "text",
            "model": "gpt-4"
        })
        self.metrics.record_latency('display_start', time.time() - start)
        
        # Multiple updates
        content = "x" * 1000  # 1KB of content
        for i in range(100):  # 100 updates
            start = time.time()
            display.update_streaming(stream_id, {
                "content": content[:i*10]
            })
            self.metrics.record_latency('display_update', time.time() - start)
            
        # Finish streaming
        start = time.time()
        display.finish_streaming(stream_id, {
            "final_stats": {"output_tokens": 100}
        })
        self.metrics.record_latency('display_finish', time.time() - start)
        
        # Record final memory
        self.metrics.record_memory()
        
        # Analyze results
        summary = self.metrics.get_summary()
        
        # Assert performance requirements
        self.assertLess(summary['latency_stats']['display_update']['avg'], 0.005)  # < 5ms avg update
        self.assertLess(summary['latency_stats']['display_update']['p95'], 0.010)  # < 10ms p95
        self.assertLess(summary['memory']['growth_mb'], 10)  # < 10MB memory growth
        
    async def test_concurrent_streams_performance(self):
        """Test performance with multiple concurrent streams."""
        display = StreamingDisplay()
        
        # Record initial state
        self.metrics.record_memory()
        self.metrics.record_cpu()
        
        # Create multiple concurrent streams
        num_streams = 10
        contexts = []
        for i in range(num_streams):
            contexts.append(DisplayContext(
                terminal_id=f"concurrent-{i}",
                terminal_number=i+1,
                agent_name=f"Agent {i}",
                interaction_counter=1
            ))
            
        async def stream_agent(context, stream_num):
            """Simulate streaming for one agent."""
            stream_id = f"concurrent-stream-{stream_num}"
            
            # Start
            display.start_streaming(context, stream_id, {
                "content_type": "text"
            })
            
            # Stream updates
            for j in range(50):  # 50 updates per stream
                display.update_streaming(stream_id, {
                    "content": f"Update {j} from agent {stream_num}"
                })
                await asyncio.sleep(0.001)  # 1ms between updates
                
            # Finish
            display.finish_streaming(stream_id, {})
            
        # Run all streams concurrently
        start_time = time.time()
        tasks = []
        for i, context in enumerate(contexts):
            task = asyncio.create_task(stream_agent(context, i))
            tasks.append(task)
            
        await asyncio.gather(*tasks)
        total_time = time.time() - start_time
        
        # Record final state
        self.metrics.record_memory()
        self.metrics.record_cpu()
        
        # Analyze performance
        summary = self.metrics.get_summary()
        total_updates = num_streams * 50
        updates_per_second = total_updates / total_time
        
        # Performance assertions
        self.assertGreater(updates_per_second, 100)  # > 100 updates/sec
        self.assertLess(summary['memory']['growth_mb'], 50)  # < 50MB for 10 streams
        
    def test_rapid_update_handling(self):
        """Test handling of very rapid updates."""
        display = StreamingDisplay()
        context = DisplayContext(
            terminal_id="rapid-1",
            terminal_number=1,
            agent_name="Rapid Test",
            interaction_counter=1
        )
        
        stream_id = "rapid-stream"
        display.start_streaming(context, stream_id, {"content_type": "text"})
        
        # Blast updates as fast as possible
        start_time = time.time()
        update_count = 1000
        
        for i in range(update_count):
            display.update_streaming(stream_id, {
                "content": f"Rapid update {i}"
            })
            # No sleep - maximum speed
            
        display.finish_streaming(stream_id, {})
        
        elapsed = time.time() - start_time
        updates_per_second = update_count / elapsed
        
        # Should handle at least 1000 updates/sec
        self.assertGreater(updates_per_second, 1000)
        
        # Check that terminal wasn't overwhelmed
        actual_terminal_updates = self.metrics.update_counts.get(stream_id, 0)
        # Should batch updates (not every update hits terminal)
        self.assertLess(actual_terminal_updates, update_count)
        
    def test_memory_leak_prevention(self):
        """Test that streaming doesn't leak memory."""
        display = StreamingDisplay()
        
        # Enable memory tracking
        tracemalloc.start()
        
        # Take initial snapshot
        snapshot1 = tracemalloc.take_snapshot()
        
        # Create and destroy many streams
        for cycle in range(10):
            contexts = []
            for i in range(10):  # 10 streams per cycle
                context = DisplayContext(
                    terminal_id=f"leak-{cycle}-{i}",
                    terminal_number=1,
                    agent_name=f"Leak Test {cycle}-{i}",
                    interaction_counter=1
                )
                contexts.append(context)
                
                stream_id = f"leak-stream-{cycle}-{i}"
                
                # Full streaming lifecycle
                display.start_streaming(context, stream_id, {"content_type": "text"})
                
                # Multiple updates
                for j in range(100):
                    display.update_streaming(stream_id, {
                        "content": f"Memory test update {j}"
                    })
                    
                display.finish_streaming(stream_id, {})
                
            # Force garbage collection
            gc.collect()
            
        # Take final snapshot
        snapshot2 = tracemalloc.take_snapshot()
        
        # Compare snapshots
        top_stats = snapshot2.compare_to(snapshot1, 'lineno')
        
        # Calculate total growth
        total_growth = sum(stat.size_diff for stat in top_stats if stat.size_diff > 0)
        
        # Should not grow more than 10MB after 100 streams
        self.assertLess(total_growth / 1024 / 1024, 10)
        
        tracemalloc.stop()
        
    def test_display_responsiveness_under_load(self):
        """Test that display remains responsive under heavy load."""
        display = StreamingDisplay()
        
        # Create background load
        load_streams = []
        for i in range(5):
            context = DisplayContext(
                terminal_id=f"load-{i}",
                terminal_number=i+1,
                agent_name=f"Load Agent {i}",
                interaction_counter=1
            )
            stream_id = f"load-stream-{i}"
            display.start_streaming(context, stream_id, {"content_type": "text"})
            load_streams.append((stream_id, context))
            
        # Start background updates
        stop_load = threading.Event()
        
        def generate_load():
            """Generate continuous updates."""
            counter = 0
            while not stop_load.is_set():
                for stream_id, _ in load_streams:
                    display.update_streaming(stream_id, {
                        "content": f"Background update {counter}"
                    })
                counter += 1
                time.sleep(0.001)
                
        load_thread = threading.Thread(target=generate_load)
        load_thread.start()
        
        # Test responsiveness of new stream
        test_context = DisplayContext(
            terminal_id="responsive-test",
            terminal_number=10,
            agent_name="Responsiveness Test",
            interaction_counter=1
        )
        
        test_stream_id = "responsive-stream"
        
        # Measure latency of operations under load
        latencies = []
        
        # Start
        start = time.time()
        display.start_streaming(test_context, test_stream_id, {"content_type": "text"})
        latencies.append(('start', time.time() - start))
        
        # Updates
        for i in range(10):
            start = time.time()
            display.update_streaming(test_stream_id, {
                "content": f"Responsive test {i}"
            })
            latencies.append(('update', time.time() - start))
            time.sleep(0.01)
            
        # Finish
        start = time.time()
        display.finish_streaming(test_stream_id, {})
        latencies.append(('finish', time.time() - start))
        
        # Stop background load
        stop_load.set()
        load_thread.join()
        
        # Clean up load streams
        for stream_id, _ in load_streams:
            display.finish_streaming(stream_id, {})
            
        # Check responsiveness
        max_latency = max(lat[1] for lat in latencies)
        avg_latency = sum(lat[1] for lat in latencies) / len(latencies)
        
        # Even under load, operations should be fast
        self.assertLess(max_latency, 0.050)  # < 50ms max
        self.assertLess(avg_latency, 0.010)  # < 10ms average
        
    def test_large_content_streaming(self):
        """Test streaming performance with large content."""
        display = StreamingDisplay()
        context = DisplayContext(
            terminal_id="large-1",
            terminal_number=1,
            agent_name="Large Content",
            interaction_counter=1
        )
        
        # Generate large content (10KB)
        large_content = "x" * 10000
        
        stream_id = "large-stream"
        display.start_streaming(context, stream_id, {"content_type": "text"})
        
        # Stream in chunks
        chunk_size = 100
        start_time = time.time()
        
        for i in range(0, len(large_content), chunk_size):
            display.update_streaming(stream_id, {
                "content": large_content[:i+chunk_size]
            })
            
        display.finish_streaming(stream_id, {})
        
        elapsed = time.time() - start_time
        throughput_mbps = (len(large_content) / 1024 / 1024) / elapsed
        
        # Should handle at least 1MB/s throughput
        self.assertGreater(throughput_mbps, 1.0)


def run_async_test(coro):
    """Helper to run async test."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


class TestStreamingPerformanceSync(TestStreamingPerformance):
    """Synchronous wrapper for async tests."""
    
    def test_concurrent_streams_performance_sync(self):
        run_async_test(self.test_concurrent_streams_performance())


if __name__ == "__main__":
    print("🚀 Running TUI Streaming Performance Tests")
    print("=" * 60)
    unittest.main(verbosity=2)