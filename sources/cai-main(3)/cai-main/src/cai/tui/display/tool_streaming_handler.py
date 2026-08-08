"""
Direct tool output streaming handler for TUI action bar
This bypasses Rich streaming contexts and directly updates the action bar
"""
import asyncio
import time
from typing import Dict, Optional, Any
import json
from threading import Lock
import weakref


class ToolStreamingHandler:
    """Handles direct streaming of tool output to action bars"""
    
    _instance = None
    _lock = Lock()
    
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
        self._active_streams: Dict[str, Dict[str, Any]] = {}
        self._terminal_outputs: Dict[str, weakref.ref] = {}  # terminal_id -> weak ref to terminal
        self._update_tasks: Dict[str, asyncio.Task] = {}
        
    def register_terminal(self, terminal_id: str, terminal_output: Any) -> None:
        """Register a terminal output for streaming"""
        self._terminal_outputs[terminal_id] = weakref.ref(terminal_output)
        
    def start_streaming(self, terminal_id: str, stream_id: str, tool_name: str, args: Any) -> None:
        """Start a new streaming session"""
        with self._lock:
            # Get terminal output
            terminal_ref = self._terminal_outputs.get(terminal_id)
            if not terminal_ref:
                return
                
            terminal_output = terminal_ref()
            if not terminal_output or not hasattr(terminal_output, 'action_bar'):
                return
            
            # Create stream info
            self._active_streams[stream_id] = {
                'terminal_id': terminal_id,
                'tool_name': tool_name,
                'args': args,
                'buffer': '',
                'last_update': 0,
                'start_time': time.time(),
                'is_active': True
            }
            
            # Show tool call in action bar - minimal & clean
            action_bar = terminal_output.action_bar
            try:
                # For generic_linux_command: prefer full command line
                if tool_name == "generic_linux_command":
                    full_cmd = None
                    if isinstance(args, dict):
                        full_cmd = (
                            args.get("full_command")
                            or args.get("full")
                            or (f"{args.get('command', '')} {args.get('args', '')}".strip())
                        )
                    elif isinstance(args, str):
                        try:
                            parsed = json.loads(args)
                            if isinstance(parsed, dict):
                                full_cmd = (
                                    parsed.get("full_command")
                                    or parsed.get("full")
                                    or (f"{parsed.get('command', '')} {parsed.get('args', '')}".strip())
                                )
                        except Exception:
                            # Could be a raw command string already
                            full_cmd = args
                    if full_cmd:
                        action_bar.show_tool_call(
                            tool_name, json.dumps({"full_command": full_cmd})
                        )
                    else:
                        action_bar.show_tool_call(tool_name, args if isinstance(args, str) else str(args))
                else:
                    # Minimal tool call. For execute_code we want only the code panel here;
                    # the animated indicator will be appended after streaming starts so it
                    # always stays below the output.
                    action_bar.show_tool_call(tool_name, args if isinstance(args, str) else str(args))
            except Exception:
                pass
            # Start streaming as tool stream to suppress CAI> during output
            action_bar.start_streaming(tool_name, is_tool=True)
            
            # Do NOT push an initial empty update; it was causing blank duplicate lines.

            # Append the animated indicator at the very bottom (after code panel)
            # and also mark the StreamingStatusBar as code mode for UX context.
            try:
                # Special pretty one-line status for the top status bar when executing code
                if tool_name == "execute_code" and hasattr(terminal_output, "status_bar"):
                    try:
                        sb = getattr(terminal_output, "status_bar", None)
                        if sb and hasattr(sb, "start_execute_code"):
                            # Extract language/filename if possible
                            lang = "python"
                            fname = "exploit"
                            try:
                                if isinstance(args, dict):
                                    lang = str(args.get("language", lang))
                                    fname = str(args.get("filename", fname))
                            except Exception:
                                pass
                            sb.start_execute_code(language=lang, filename=fname)
                    except Exception:
                        pass
                # BUGFIX: Completely disable tool indicators to prevent scroll issues
                # The command is already visible in the main terminal output
                pass
            except Exception:
                # BUGFIX: Disable fallback tool indicators to prevent scroll issues
                pass
            
    def update_stream(self, stream_id: str, output: str) -> None:
        """Update streaming output"""
        with self._lock:
            stream = self._active_streams.get(stream_id)
            if not stream or not stream['is_active']:
                return
            
            # Update buffer with full output; the action bar displays full text every update
            stream['buffer'] = output
            current_time = time.time()
            
            # BUGFIX: Sync with other streaming components (150ms) to prevent scroll flicker
            if current_time - stream['last_update'] < 0.15:
                return
            
            stream['last_update'] = current_time
            
            # Get terminal
            terminal_ref = self._terminal_outputs.get(stream['terminal_id'])
            if not terminal_ref:
                # Debug logging
                import os
                if os.getenv("CAI_DEBUG") == "2":
                    import sys
                    print(f"[STREAM DEBUG] No terminal ref for {stream['terminal_id']}", file=sys.stderr)
                    print(f"[STREAM DEBUG] Available terminals: {list(self._terminal_outputs.keys())}", file=sys.stderr)
                return
            
            terminal_output = terminal_ref()
            if not terminal_output or not hasattr(terminal_output, 'action_bar'):
                # Debug logging
                import os
                if os.getenv("CAI_DEBUG") == "2":
                    import sys
                    print(f"[STREAM DEBUG] Terminal has no action_bar: {terminal_output}", file=sys.stderr)
                return
            
            # Update action bar directly; ensure only ONE streaming visualization is active
            # by replacing the current streaming line content rather than adding another panel.
            action_bar = terminal_output.action_bar
            # BUGFIX: Disable streaming updates to prevent scroll issues
            # The output is already visible in the main terminal
            pass
            
            # Debug logging
            import os
            if os.getenv("CAI_DEBUG") == "2":
                import sys
                print(f"[STREAM DEBUG] Updated action bar with {len(output)} chars", file=sys.stderr)
            
    def finish_stream(self, stream_id: str, final_output: str, status: str = "completed") -> Any:
        """Finish streaming and return terminal output for final display"""
        with self._lock:
            stream = self._active_streams.get(stream_id)
            if not stream:
                return None
                
            stream['is_active'] = False
            
            # Get terminal
            terminal_ref = self._terminal_outputs.get(stream['terminal_id'])
            if not terminal_ref:
                return None
                
            terminal_output = terminal_ref()
            if not terminal_output:
                return None
                
            # Stop streaming in action bar after flushing last accumulated buffer
            if hasattr(terminal_output, 'action_bar'):
                action_bar = terminal_output.action_bar
                # BUGFIX: Disable final streaming update to prevent scroll issues
                # The output is already visible in the main terminal
                pass
                # BUGFIX: Disable tool execution indicator to prevent scroll issues
                pass
                # Mark error state so the built-in completion line reflects it
                try:
                    if status in ("error", "timeout") and hasattr(action_bar, 'mark_stream_error'):
                        # Try to extract a concise error message from final_output
                        err_msg = None
                        text = str(buffer_text or "")
                        if text:
                            # 1) If contains 'ERROR OUTPUT:', take the first non-empty line after it
                            low = text.lower()
                            if 'error output' in low:
                                after = text.split('\n', 1)[1] if low.startswith('error output') else text.split('ERROR OUTPUT:', 1)[1]
                                for part in after.splitlines():
                                    if part.strip() and not part.strip().lower().startswith('command exited with code'):
                                        err_msg = part.strip()[:200]
                                        break
                            # 2) Fallback: pick the last non-empty line that is not the exit code line
                            if not err_msg:
                                for part in text.splitlines()[::-1]:
                                    if part.strip() and not part.strip().lower().startswith('command exited with code'):
                                        err_msg = part.strip()[:200]
                                        break
                        if status == "timeout" and not err_msg:
                            err_msg = "Timeout"
                        action_bar.mark_stream_error(err_msg or "Error")
                except Exception:
                    pass
                # Stop streaming – ActualActionBar.complete_streaming() will add the single status line
                action_bar.stop_streaming()
            
            # Clean up
            del self._active_streams[stream_id]
            
            # Return terminal output for final panel display
            return terminal_output
            
    def cleanup_terminal(self, terminal_id: str) -> None:
        """Clean up resources for a terminal"""
        with self._lock:
            # Remove terminal reference
            if terminal_id in self._terminal_outputs:
                del self._terminal_outputs[terminal_id]
                
            # Cancel any active streams for this terminal
            streams_to_remove = []
            for stream_id, stream in self._active_streams.items():
                if stream['terminal_id'] == terminal_id:
                    streams_to_remove.append(stream_id)
                    
            for stream_id in streams_to_remove:
                del self._active_streams[stream_id]


# Global instance
_handler = ToolStreamingHandler()


def get_tool_streaming_handler() -> ToolStreamingHandler:
    """Get the global tool streaming handler"""
    return _handler