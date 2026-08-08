"""
Live output capture for TUI action bar
Captures tool output in real-time regardless of streaming mode
"""
import asyncio
import threading
import time
import weakref
from typing import Dict, Optional, Any
import sys
import io
from contextlib import contextmanager

import os
_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")


class LiveOutputCapture:
    """Captures live output from tools and routes to action bar"""
    
    _instance = None
    _lock = threading.Lock()
    
    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self):
        if hasattr(self, '_initialized'):
            return
        self._initialized = True
        self._active_captures: Dict[str, Dict[str, Any]] = {}
        self._terminal_outputs: Dict[str, weakref.ref] = {}
        self._update_task = None
        self._running = False
        # Markers to flag stderr segments within streamed text
        self.STDERR_START_TOKEN = "[[STDERR]]"
        self.STDERR_END_TOKEN = "[[/STDERR]]"
        
    def register_terminal(self, terminal_id: str, terminal_output: Any) -> None:
        """Register a terminal for output capture"""
        # Debug logging
        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
            import datetime
            f.write(f"\n[{datetime.datetime.now().isoformat()}] register_terminal called\n")
            f.write(f"  terminal_id: {terminal_id}\n")
            f.write(f"  terminal_output type: {type(terminal_output).__name__}\n")
            f.write(f"  has action_bar: {hasattr(terminal_output, 'action_bar')}\n")
            f.write(f"  existing terminals: {list(self._terminal_outputs.keys())}\n")
        
        self._terminal_outputs[terminal_id] = weakref.ref(terminal_output)
        
    def start_capture(self, terminal_id: str, tool_name: str, args: Any) -> str:
        """Start capturing output for a tool"""
        import uuid
        capture_id = f"capture_{tool_name}_{str(uuid.uuid4())[:8]}"
        
        # Debug logging
        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
            import datetime
            f.write(f"\n[{datetime.datetime.now().isoformat()}] start_capture called\n")
            f.write(f"  terminal_id: {terminal_id}\n")
            f.write(f"  tool_name: {tool_name}\n")
            f.write(f"  args: {args}\n")
            f.write(f"  capture_id: {capture_id}\n")
        
        # Get terminal
        terminal_ref = self._terminal_outputs.get(terminal_id)
        if not terminal_ref:
            with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                f.write(f"  ERROR: No terminal ref found for {terminal_id}\n")
            return capture_id
            
        terminal_output = terminal_ref()
        if not terminal_output or not hasattr(terminal_output, 'action_bar'):
            return capture_id
            
        # Show tool call in action bar
        action_bar = terminal_output.action_bar
        
        # Format args with full command when available to avoid duplicates later
        # Use the same JSON structure as ToolStreamingHandler so dedup kicks in
        import json as _json
        args_payload = None
        if isinstance(args, dict):
            full_cmd = (
                args.get("full_command")
                or args.get("full")
                or (f"{args.get('command', '')} {args.get('args', '')}".strip())
            )
            if full_cmd:
                args_payload = _json.dumps({"full_command": full_cmd})
        elif isinstance(args, str) and args.strip():
            # Pass-through string; if it looks like JSON and contains fields, keep it
            try:
                parsed = _json.loads(args)
                if isinstance(parsed, dict):
                    full_cmd = (
                        parsed.get("full_command")
                        or parsed.get("full")
                        or (f"{parsed.get('command', '')} {parsed.get('args', '')}".strip())
                    )
                    if full_cmd:
                        args_payload = _json.dumps({"full_command": full_cmd})
            except Exception:
                # leave as raw string
                args_payload = args
        if args_payload is None:
            # Fallback minimal text
            args_payload = str(args)[:100]
        
        action_bar.show_tool_call(tool_name, args_payload)
        action_bar.start_streaming(tool_name, is_tool=True)
        
        # Debug
        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
            f.write(f"  action_bar.start_streaming called for {tool_name}\n")
        
        # Create capture info
        self._active_captures[capture_id] = {
            'terminal_id': terminal_id,
            'tool_name': tool_name,
            'args': args,
            'output_buffer': [],
            'last_update': 0,
            'start_time': time.time(),
            'is_active': True,
            'update_counter': 0
        }
        
        # Start update loop if not running
        if not self._running:
            self._start_update_loop()
            
        return capture_id
        
    def append_output(self, capture_id: str, output: str, is_error: bool = False) -> None:
        """Append output to capture buffer"""
        if capture_id not in self._active_captures:
            return
            
        capture = self._active_captures[capture_id]
        if not capture['is_active']:
            return
            
        # Add to buffer with size limit to prevent memory issues
        # Wrap stderr fragments with explicit markers for later styling
        if is_error:
            capture['had_error'] = True
            # Track last non-empty stderr line for status message
            try:
                last_line = None
                for part in str(output).splitlines():
                    if part.strip():
                        last_line = part.strip()
                if last_line:
                    capture['last_error_line'] = last_line[:200]
            except Exception:
                pass
            output = f"{self.STDERR_START_TOKEN}{output}{self.STDERR_END_TOKEN}"
        capture['output_buffer'].append(output)
        capture['has_new_content'] = True  # Mark that we have new content
        
        # Keep only last 100 items in buffer
        if len(capture['output_buffer']) > 100:
            # Join and trim to keep recent content
            combined = ''.join(capture['output_buffer'][-50:])
            capture['output_buffer'] = [combined]
        
        # Debug logging (only first append)
        if capture.get('debug_count', 0) < 5:
            with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                import datetime
                f.write(f"\n[{datetime.datetime.now().isoformat()}] append_output called\n")
                f.write(f"  capture_id: {capture_id}\n")
                f.write(f"  output length: {len(output)}\n")
                f.write(f"  first 50 chars: {repr(output[:50])}\n")
                f.write(f"  buffer size: {len(capture['output_buffer'])}\n")
            capture['debug_count'] = capture.get('debug_count', 0) + 1
        
    def _start_update_loop(self):
        """Start the update loop in a thread"""
        self._running = True
        thread = threading.Thread(target=self._update_loop, daemon=True)
        thread.start()
        
    def _update_loop(self):
        """Update action bars at reasonable intervals"""
        while self._running:
            current_time = time.time()
            
            for capture_id, capture in list(self._active_captures.items()):
                if not capture['is_active']:
                    continue
                    
                # Check more frequently to accumulate updates
                time_since_update = current_time - capture['last_update']

                # BUGFIX: Unified frequency to prevent scroll flicker
                min_interval = 0.15  # Single 150ms frequency for all updates
                if time_since_update < min_interval:
                    continue
                    
                capture['last_update'] = current_time
                
                # Get terminal
                terminal_ref = self._terminal_outputs.get(capture['terminal_id'])
                if not terminal_ref:
                    # Debug log missing terminal
                    if capture.get('debug_missing_terminal', 0) < 3:
                        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                            f.write(f"  WARNING: No terminal ref for {capture['terminal_id']}\n")
                            f.write(f"  Available terminals: {list(self._terminal_outputs.keys())}\n")
                        capture['debug_missing_terminal'] = capture.get('debug_missing_terminal', 0) + 1
                    continue
                    
                terminal_output = terminal_ref()
                if not terminal_output or not hasattr(terminal_output, 'action_bar'):
                    # Debug log missing action bar
                    if capture.get('debug_missing_action_bar', 0) < 3:
                        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                            f.write(f"  WARNING: No action_bar for terminal {capture['terminal_id']}\n")
                            f.write(f"  terminal_output exists: {terminal_output is not None}\n")
                            if terminal_output:
                                f.write(f"  terminal_output type: {type(terminal_output).__name__}\n")
                        capture['debug_missing_action_bar'] = capture.get('debug_missing_action_bar', 0) + 1
                    continue
                    
                # Update action bar with accumulated output
                if capture['output_buffer']:
                    # Only send the last portion to avoid performance issues
                    # Keep last 1000 characters for display
                    combined_output = ''.join(capture['output_buffer'])
                    if len(combined_output) > 1000:
                        combined_output = combined_output[-1000:]
                    terminal_output.action_bar.update_streaming_text(combined_output)
                    capture['update_counter'] += 1
                    capture['has_new_content'] = False  # Reset flag after update
                else:
                    # No new content; skip update to avoid flicker
                    pass
                    
            # BUGFIX: Sleep sincronizado con frecuencia unificada
            time.sleep(0.15)  # 150ms sleep - sincronizado con update_interval
            
    def finish_capture(self, capture_id: str, final_output: str = None) -> Any:
        """Finish capture and return terminal for final display"""
        if capture_id not in self._active_captures:
            return None
            
        capture = self._active_captures[capture_id]
        capture['is_active'] = False
        
        # Get terminal
        terminal_ref = self._terminal_outputs.get(capture['terminal_id'])
        if not terminal_ref:
            return None
            
        terminal_output = terminal_ref()
        if not terminal_output:
            return None
            
        # Final update with all output
        if hasattr(terminal_output, 'action_bar'):
            action_bar = terminal_output.action_bar
            
            # Send final output
            if final_output:
                # Ensure final stderr segments are marked
                if capture.get('had_error') and self.STDERR_START_TOKEN not in final_output:
                    # As a fallback (rare), wrap the entire final output to flag error
                    final_output = f"{self.STDERR_START_TOKEN}{final_output}{self.STDERR_END_TOKEN}"
                action_bar.update_streaming_text(final_output)
            elif capture['output_buffer']:
                combined_output = ''.join(capture['output_buffer'])
                action_bar.update_streaming_text(combined_output)
                
            # Stop streaming (will render status line)
            # If there were stderr fragments, flag the stream as error so a red status appears
            try:
                if capture.get('had_error') and hasattr(action_bar, 'mark_stream_error'):
                    # Prefer the last captured stderr line; fallback to parsing final_output
                    err_msg = capture.get('last_error_line')
                    if not err_msg and final_output:
                        text = str(final_output)
                        # Try to extract message between markers
                        start = text.rfind(self.STDERR_START_TOKEN)
                        if start != -1:
                            end = text.find(self.STDERR_END_TOKEN, start)
                            segment = text[start + len(self.STDERR_START_TOKEN): end if end != -1 else None]
                            for part in segment.splitlines()[::-1]:
                                if part.strip():
                                    err_msg = part.strip()[:200]
                                    break
                        if not err_msg and 'ERROR OUTPUT:' in text:
                            after = text.split('ERROR OUTPUT:', 1)[1]
                            for part in after.splitlines():
                                if part.strip():
                                    err_msg = part.strip()[:200]
                                    break
                    action_bar.mark_stream_error(err_msg or "Error")
            except Exception:
                pass
            action_bar.stop_streaming()
            
            # Don't write completion message - it causes duplicate output
            # action_bar.write(f"\n[bold green]✓ {capture['tool_name']} completed[/bold green]")
            
        # Clean up
        del self._active_captures[capture_id]
        
        # Stop update loop if no more captures
        if not self._active_captures:
            self._running = False
            
        return terminal_output


# Global instance
_capture = LiveOutputCapture()


def get_live_output_capture() -> LiveOutputCapture:
    """Get the global live output capture instance"""
    return _capture


@contextmanager
def capture_tool_output(terminal_id: str, tool_name: str, args: Any):
    """Context manager to capture tool output"""
    capture = get_live_output_capture()
    capture_id = capture.start_capture(terminal_id, tool_name, args)
    
    # Store original stdout/stderr
    original_stdout = sys.stdout
    original_stderr = sys.stderr
    
    # Create capturing streams
    stdout_capture = io.StringIO()
    stderr_capture = io.StringIO()
    
    class TeeOutput:
        """Tee output to both original and capture"""
        def __init__(self, original, capture_stream, capture_obj, capture_id, is_error: bool = False):
            self.original = original
            self.capture_stream = capture_stream
            self.capture_obj = capture_obj
            self.capture_id = capture_id
            self.is_error = is_error
            self.buffer = []
            
        def write(self, data):
            # Write to original
            if self.original:
                self.original.write(data)
                self.original.flush()
            
            # Capture
            self.capture_stream.write(data)
            self.buffer.append(data)
            
            # Send to live capture
            self.capture_obj.append_output(self.capture_id, data, is_error=self.is_error)
            
        def flush(self):
            if self.original:
                self.original.flush()
            self.capture_stream.flush()
    
    # Replace stdout/stderr with tee
    sys.stdout = TeeOutput(original_stdout, stdout_capture, capture, capture_id, is_error=False)
    sys.stderr = TeeOutput(original_stderr, stderr_capture, capture, capture_id, is_error=True)
    
    try:
        yield capture_id
    finally:
        # Restore original stdout/stderr
        sys.stdout = original_stdout
        sys.stderr = original_stderr
        
        # Get final output
        final_output = stdout_capture.getvalue() + stderr_capture.getvalue()
        
        # Finish capture
        terminal_output = capture.finish_capture(capture_id, final_output)
        
        # Return terminal for final display
        return terminal_output
