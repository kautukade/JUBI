"""
Comprehensive error handling for TUI streaming

This module provides robust error handling, edge case management,
and graceful degradation for the TUI streaming implementation.
"""

import asyncio
import logging
import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
import signal
import sys
import threading
import traceback
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import datetime
from functools import wraps
from typing import Any, Callable, Dict, List, Optional, Set, Tuple, Union

# Set up logging
logger = logging.getLogger(__name__)

# Error log file
ERROR_LOG_FILE = f"{_CAI_DEBUG_DIR}/cai_tui_errors.log"


@dataclass
class ErrorContext:
    """Context for error tracking"""
    error_count: int = 0
    last_error_time: Optional[datetime] = None
    error_history: List[Dict[str, Any]] = field(default_factory=list)
    max_errors: int = 100
    rate_limit_window: float = 1.0  # seconds
    rate_limit_max: int = 10


class StreamingErrorHandler:
    """Centralized error handling for streaming operations"""
    
    def __init__(self):
        self._error_contexts: Dict[str, ErrorContext] = {}
        self._lock = threading.Lock()
        self._interrupted = False
        self._original_sigint = None
        self._setup_signal_handlers()
        
    def _setup_signal_handlers(self):
        """Set up signal handlers for graceful interruption"""
        try:
            # Store original handler
            self._original_sigint = signal.signal(signal.SIGINT, self._handle_interrupt)
        except ValueError:
            # Not in main thread, skip signal handling
            pass
    
    def _handle_interrupt(self, signum, frame):
        """Handle Ctrl+C gracefully"""
        self._interrupted = True
        logger.info("Streaming interrupted by user")
        
        # Log to file
        self._log_error({
            "type": "interruption",
            "signal": signum,
            "timestamp": datetime.now().isoformat()
        })
        
        # Call original handler
        if self._original_sigint:
            self._original_sigint(signum, frame)
    
    @property
    def is_interrupted(self) -> bool:
        """Check if streaming was interrupted"""
        return self._interrupted
    
    def reset_interruption(self):
        """Reset interruption flag"""
        self._interrupted = False
    
    def _log_error(self, error_data: Dict[str, Any]):
        """Log error to file"""
        try:
            with open(ERROR_LOG_FILE, "a") as f:
                f.write(f"{datetime.now().isoformat()} - {error_data}\n")
        except:
            # Fail silently if we can't write to log
            pass
    
    def get_or_create_context(self, stream_id: str) -> ErrorContext:
        """Get or create error context for a stream"""
        with self._lock:
            if stream_id not in self._error_contexts:
                self._error_contexts[stream_id] = ErrorContext()
            return self._error_contexts[stream_id]
    
    def check_rate_limit(self, stream_id: str) -> bool:
        """Check if we're within rate limits"""
        context = self.get_or_create_context(stream_id)
        now = datetime.now()
        
        # Clean old errors
        cutoff = now.timestamp() - context.rate_limit_window
        context.error_history = [
            e for e in context.error_history
            if e.get("timestamp", 0) > cutoff
        ]
        
        # Check rate limit
        return len(context.error_history) < context.rate_limit_max
    
    def record_error(self, stream_id: str, error: Exception, context: Dict[str, Any] = None):
        """Record an error occurrence"""
        error_context = self.get_or_create_context(stream_id)
        
        error_data = {
            "timestamp": datetime.now().timestamp(),
            "error_type": type(error).__name__,
            "error_msg": str(error),
            "context": context or {},
            "stack_trace": traceback.format_exc()
        }
        
        with self._lock:
            error_context.error_count += 1
            error_context.last_error_time = datetime.now()
            error_context.error_history.append(error_data)
            
            # Trim history
            if len(error_context.error_history) > error_context.max_errors:
                error_context.error_history = error_context.error_history[-error_context.max_errors:]
        
        # Log to file
        self._log_error(error_data)
    
    def should_retry(self, stream_id: str, error: Exception) -> bool:
        """Determine if operation should be retried"""
        # Don't retry if interrupted
        if self._interrupted:
            return False
        
        # Don't retry certain error types
        non_retryable = (
            KeyboardInterrupt,
            SystemExit,
            MemoryError,
            RecursionError
        )
        if isinstance(error, non_retryable):
            return False
        
        # Check rate limit
        if not self.check_rate_limit(stream_id):
            return False
        
        # Check error count
        context = self.get_or_create_context(stream_id)
        if context.error_count > 10:
            return False
        
        return True
    
    def clear_context(self, stream_id: str):
        """Clear error context for a stream"""
        with self._lock:
            self._error_contexts.pop(stream_id, None)


# Global error handler instance
ERROR_HANDLER = StreamingErrorHandler()


def handle_streaming_errors(func: Callable) -> Callable:
    """Decorator for handling streaming errors"""
    @wraps(func)
    def wrapper(*args, **kwargs):
        stream_id = kwargs.get("stream_id", "unknown")
        
        try:
            return func(*args, **kwargs)
        except Exception as e:
            ERROR_HANDLER.record_error(stream_id, e, {
                "function": func.__name__,
                "args": str(args)[:100],
                "kwargs": str(kwargs)[:100]
            })
            
            # Re-raise or handle based on error type
            if isinstance(e, (KeyboardInterrupt, SystemExit)):
                raise
            
            logger.error(f"Error in {func.__name__}: {e}")
            
            # Return safe default
            return None
    
    return wrapper


def handle_async_streaming_errors(func: Callable) -> Callable:
    """Decorator for handling async streaming errors"""
    @wraps(func)
    async def wrapper(*args, **kwargs):
        stream_id = kwargs.get("stream_id", "unknown")
        
        try:
            return await func(*args, **kwargs)
        except asyncio.CancelledError:
            # Task was cancelled, propagate
            raise
        except Exception as e:
            ERROR_HANDLER.record_error(stream_id, e, {
                "function": func.__name__,
                "args": str(args)[:100],
                "kwargs": str(kwargs)[:100]
            })
            
            # Re-raise or handle based on error type
            if isinstance(e, (KeyboardInterrupt, SystemExit)):
                raise
            
            logger.error(f"Error in {func.__name__}: {e}")
            
            # Return safe default
            return None
    
    return wrapper


class ContentValidator:
    """Validate and sanitize streaming content"""
    
    # Maximum line length before truncation
    MAX_LINE_LENGTH = 1000
    
    # Control characters to strip (except common ones like \n, \t)
    CONTROL_CHARS = set(range(0, 9)) | set(range(11, 32)) | {127}
    
    @classmethod
    def sanitize_content(cls, content: Any) -> str:
        """Sanitize content for safe display"""
        if content is None:
            return ""
        
        # Convert to string
        if not isinstance(content, str):
            try:
                content = str(content)
            except Exception:
                return "[Error: Unable to convert content to string]"
        
        # Handle empty content
        if not content:
            return ""
        
        # Remove null bytes
        content = content.replace('\0', '')
        
        # Strip control characters (except \n, \t)
        content = ''.join(
            char for char in content
            if ord(char) not in cls.CONTROL_CHARS or char in '\n\t\r'
        )
        
        # Handle very long lines
        lines = content.split('\n')
        sanitized_lines = []
        
        for line in lines:
            if len(line) > cls.MAX_LINE_LENGTH:
                # Truncate and add indicator
                line = line[:cls.MAX_LINE_LENGTH] + "... [truncated]"
            sanitized_lines.append(line)
        
        return '\n'.join(sanitized_lines)
    
    @classmethod
    def validate_stream_data(cls, data: Dict[str, Any]) -> Dict[str, Any]:
        """Validate and clean stream data"""
        if not isinstance(data, dict):
            return {"content": "", "error": "Invalid data type"}
        
        # Sanitize content field
        if "content" in data:
            data["content"] = cls.sanitize_content(data["content"])
        
        # Validate numeric fields
        for field in ["input_tokens", "output_tokens", "reasoning_tokens"]:
            if field in data:
                try:
                    data[field] = int(data[field])
                    if data[field] < 0:
                        data[field] = 0
                except (ValueError, TypeError):
                    data[field] = 0
        
        # Validate cost fields
        for field in ["interaction_cost", "total_cost", "session_total_cost"]:
            if field in data:
                try:
                    data[field] = float(data[field])
                    if data[field] < 0:
                        data[field] = 0.0
                except (ValueError, TypeError):
                    data[field] = 0.0
        
        return data


class StreamingRateLimiter:
    """Rate limiter for streaming updates"""
    
    def __init__(self, min_interval: float = 0.05):
        self.min_interval = min_interval
        self._last_update_times: Dict[str, float] = {}
        self._lock = threading.Lock()
    
    def should_update(self, stream_id: str) -> bool:
        """Check if enough time has passed for an update"""
        import time
        now = time.time()
        
        with self._lock:
            last_update = self._last_update_times.get(stream_id, 0)
            if now - last_update >= self.min_interval:
                self._last_update_times[stream_id] = now
                return True
            return False
    
    def clear(self, stream_id: str):
        """Clear rate limit tracking for a stream"""
        with self._lock:
            self._last_update_times.pop(stream_id, None)


# Global rate limiter (extremely fast updates)
RATE_LIMITER = StreamingRateLimiter(min_interval=0.0001)


@contextmanager
def safe_streaming_context(stream_id: str):
    """Context manager for safe streaming operations"""
    try:
        yield
    except Exception as e:
        ERROR_HANDLER.record_error(stream_id, e)
        logger.error(f"Error in streaming context {stream_id}: {e}")
    finally:
        # Clean up
        ERROR_HANDLER.clear_context(stream_id)
        RATE_LIMITER.clear(stream_id)


def handle_terminal_resize():
    """Handle terminal resize events gracefully"""
    try:
        # Get current terminal size
        import shutil
        cols, rows = shutil.get_terminal_size()
        
        # Notify all active streams about resize
        # This would be implemented based on your streaming architecture
        logger.info(f"Terminal resized to {cols}x{rows}")
        
    except Exception as e:
        logger.error(f"Error handling terminal resize: {e}")


class ConcurrencyManager:
    """Manage concurrent streaming operations"""
    
    def __init__(self, max_concurrent: int = 10):
        self.max_concurrent = max_concurrent
        self._active_streams: Set[str] = set()
        self._lock = threading.Lock()
        self._semaphore = threading.Semaphore(max_concurrent)
    
    @contextmanager
    def acquire_stream(self, stream_id: str):
        """Acquire a streaming slot"""
        acquired = False
        try:
            # Try to acquire semaphore
            if self._semaphore.acquire(timeout=1.0):
                acquired = True
                with self._lock:
                    self._active_streams.add(stream_id)
                yield
            else:
                raise RuntimeError(f"Too many concurrent streams (max: {self.max_concurrent})")
        finally:
            if acquired:
                with self._lock:
                    self._active_streams.discard(stream_id)
                self._semaphore.release()
    
    @property
    def active_count(self) -> int:
        """Get number of active streams"""
        with self._lock:
            return len(self._active_streams)
    
    def is_at_capacity(self) -> bool:
        """Check if at maximum capacity"""
        return self.active_count >= self.max_concurrent


# Global concurrency manager
CONCURRENCY_MANAGER = ConcurrencyManager()


def validate_terminal_output(terminal_output: Any) -> bool:
    """Validate terminal output object"""
    if not terminal_output:
        return False
    
    # Check required methods
    required_methods = ["write", "clear"]
    for method in required_methods:
        if not hasattr(terminal_output, method):
            return False
    
    return True


def safe_write_to_terminal(terminal_output: Any, content: str, fallback: Callable = None):
    """Safely write to terminal with fallback"""
    try:
        if validate_terminal_output(terminal_output):
            # Sanitize content first
            safe_content = ContentValidator.sanitize_content(content)
            terminal_output.write(safe_content)
        elif fallback:
            fallback(content)
        else:
            # Last resort - write to stderr
            print(f"[TUI Error] No valid output: {content[:100]}", file=sys.stderr)
    except Exception as e:
        logger.error(f"Error writing to terminal: {e}")
        if fallback:
            try:
                fallback(content)
            except:
                pass


class MemoryGuard:
    """Guard against memory exhaustion"""
    
    def __init__(self, max_buffer_size: int = 10_000_000):  # 10MB default
        self.max_buffer_size = max_buffer_size
        self._buffer_sizes: Dict[str, int] = {}
        self._lock = threading.Lock()
    
    def check_buffer(self, stream_id: str, content: str) -> bool:
        """Check if adding content would exceed limits"""
        content_size = len(content.encode('utf-8'))
        
        with self._lock:
            current_size = self._buffer_sizes.get(stream_id, 0)
            if current_size + content_size > self.max_buffer_size:
                return False
            self._buffer_sizes[stream_id] = current_size + content_size
            return True
    
    def clear_buffer(self, stream_id: str):
        """Clear buffer tracking for stream"""
        with self._lock:
            self._buffer_sizes.pop(stream_id, None)
    
    def get_buffer_size(self, stream_id: str) -> int:
        """Get current buffer size for stream"""
        with self._lock:
            return self._buffer_sizes.get(stream_id, 0)


# Global memory guard
MEMORY_GUARD = MemoryGuard()