"""
Interceptor for all command executions to capture live output
"""
import os
import sys
import subprocess
import threading
import asyncio
from typing import Any, Optional
import time


def get_terminal_and_capture():
    """Get current terminal ID and live capture handler"""
    if os.getenv("CAI_TUI_MODE") != "true":
        return None, None
        
    try:
        from cai.tui.core.terminal_tracking import get_current_terminal_id
        from cai.tui.display.live_output_capture import get_live_output_capture
        
        terminal_id = get_current_terminal_id()
        if not terminal_id:
            return None, None
            
        capture = get_live_output_capture()
        return terminal_id, capture
    except ImportError:
        return None, None


class ProcessOutputReader(threading.Thread):
    """Thread to read process output in real-time"""
    def __init__(self, stream, capture, capture_id, is_stderr=False):
        super().__init__(daemon=True)
        self.stream = stream
        self.capture = capture
        self.capture_id = capture_id
        self.is_stderr = is_stderr
        self.output_lines = []
        
    def run(self):
        """Read output line by line"""
        try:
            for line in iter(self.stream.readline, b''):
                if not line:
                    break
                    
                # Decode line
                try:
                    line_str = line.decode('utf-8', errors='replace')
                except:
                    line_str = str(line)
                    
                # Store line
                self.output_lines.append(line_str)
                
                # Send to capture
                if self.capture and self.capture_id:
                    self.capture.append_output(self.capture_id, line_str, is_error=self.is_stderr)
                    
        except Exception as e:
            print(f"Error reading output: {e}", file=sys.stderr)


def run_with_live_capture(command, tool_name=None, args=None, **kwargs):
    """Run a command with live output capture"""
    terminal_id, capture = get_terminal_and_capture()
    
    # If not in TUI mode or no capture, run normally
    if not capture or not terminal_id:
        result = subprocess.run(command, **kwargs)
        return result
        
    # Prepare tool info
    if not tool_name:
        if isinstance(command, list):
            tool_name = command[0] if command else "command"
        else:
            tool_name = command.split()[0] if command else "command"
            
    if not args:
        if isinstance(command, list):
            args = {"command": " ".join(command)}
        else:
            args = {"command": command}
            
    # Start capture
    capture_id = capture.start_capture(terminal_id, tool_name, args)
    
    # Run process with pipes
    process_kwargs = kwargs.copy()
    process_kwargs['stdout'] = subprocess.PIPE
    process_kwargs['stderr'] = subprocess.PIPE
    
    # Start process
    process = subprocess.Popen(command, **process_kwargs)
    
    # Start output readers
    stdout_reader = ProcessOutputReader(process.stdout, capture, capture_id, False)
    stderr_reader = ProcessOutputReader(process.stderr, capture, capture_id, True)
    
    stdout_reader.start()
    stderr_reader.start()
    
    # Wait for process to complete
    return_code = process.wait()
    
    # Wait for readers to finish
    stdout_reader.join(timeout=1.0)
    stderr_reader.join(timeout=1.0)
    
    # Get all output
    stdout_output = ''.join(stdout_reader.output_lines)
    stderr_output = ''.join(stderr_reader.output_lines)
    
    # Finish capture
    final_output = stdout_output + stderr_output
    capture.finish_capture(capture_id, final_output)
    
    # Create result object similar to subprocess.run
    class Result:
        def __init__(self, returncode, stdout, stderr):
            self.returncode = returncode
            self.stdout = stdout
            self.stderr = stderr
            
    return Result(return_code, stdout_output, stderr_output)


async def run_async_with_live_capture(command, tool_name=None, args=None, cwd=None, timeout=None):
    """Run async command with live output capture"""
    terminal_id, capture = get_terminal_and_capture()
    
    # Prepare tool info
    if not tool_name:
        if isinstance(command, str):
            tool_name = command.split()[0] if command else "command"
        else:
            tool_name = "command"
            
    if not args:
        args = {"command": command}
        
    # Start capture if available
    capture_id = None
    if capture and terminal_id:
        capture_id = capture.start_capture(terminal_id, tool_name, args)
    
    # Create subprocess
    if isinstance(command, str):
        process = await asyncio.create_subprocess_shell(
            command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=cwd
        )
    else:
        process = await asyncio.create_subprocess_exec(
            *command,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=cwd
        )
    
    # Read output in real-time
    output_lines = []
    
    async def read_stream(stream, is_stderr=False):
        while True:
            line = await stream.readline()
            if not line:
                break
                
            line_str = line.decode('utf-8', errors='replace')
            output_lines.append(line_str)
            
            # Send to capture
            if capture and capture_id:
                capture.append_output(capture_id, line_str)
    
    # Read both streams concurrently
    await asyncio.gather(
        read_stream(process.stdout),
        read_stream(process.stderr, True)
    )
    
    # Wait for process
    if timeout:
        try:
            return_code = await asyncio.wait_for(process.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            process.kill()
            await process.wait()
            raise subprocess.TimeoutExpired(command, timeout)
    else:
        return_code = await process.wait()
    
    # Get final output
    final_output = ''.join(output_lines)
    
    # Finish capture
    if capture and capture_id:
        capture.finish_capture(capture_id, final_output)
    
    return final_output, return_code