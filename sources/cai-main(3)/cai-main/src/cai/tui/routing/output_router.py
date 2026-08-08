"""
Output router for ensuring content goes to the correct terminal
Following Strategy pattern for different routing strategies
"""

import threading
import sys
from abc import ABC, abstractmethod
from contextvars import ContextVar
from typing import Any, Dict, Optional, Tuple

# Context variables for terminal routing
current_terminal_id: ContextVar[Optional[str]] = ContextVar('current_terminal_id', default=None)
current_terminal_number: ContextVar[Optional[int]] = ContextVar('current_terminal_number', default=None)


class IRoutingStrategy(ABC):
    """Interface for routing strategies"""

    @abstractmethod
    def get_terminal_id(self, context: Dict[str, Any]) -> Optional[str]:
        """Get terminal ID based on context"""
        pass

    @abstractmethod
    def set_terminal_context(self, terminal_id: str, terminal_number: int) -> Any:
        """Set terminal context and return token for cleanup"""
        pass


class ContextVarRoutingStrategy(IRoutingStrategy):
    """Routing based on context variables (best for async)"""

    def get_terminal_id(self, context: Dict[str, Any]) -> Optional[str]:
        """Get terminal ID from context variables"""
        # First check context variables
        terminal_id = current_terminal_id.get()
        if terminal_id:
            return terminal_id

        # Fallback to context dict
        return context.get('terminal_id')

    def set_terminal_context(self, terminal_id: str, terminal_number: int) -> Any:
        """Set context variables"""
        tokens = []
        tokens.append(current_terminal_id.set(terminal_id))
        tokens.append(current_terminal_number.set(terminal_number))
        return tokens


class ThreadLocalRoutingStrategy(IRoutingStrategy):
    """Routing based on thread locals (for sync code)"""

    def __init__(self):
        self._thread_local = threading.local()

    def get_terminal_id(self, context: Dict[str, Any]) -> Optional[str]:
        """Get terminal ID from thread local"""
        # Check thread local
        if hasattr(self._thread_local, 'terminal_id'):
            return self._thread_local.terminal_id

        # Fallback to context
        return context.get('terminal_id')

    def set_terminal_context(self, terminal_id: str, terminal_number: int) -> Any:
        """Set thread local context"""
        self._thread_local.terminal_id = terminal_id
        self._thread_local.terminal_number = terminal_number
        return None  # No cleanup needed for thread locals


class HybridRoutingStrategy(IRoutingStrategy):
    """Hybrid strategy that works with both async and sync code"""

    def __init__(self):
        self._context_strategy = ContextVarRoutingStrategy()
        self._thread_strategy = ThreadLocalRoutingStrategy()

    def get_terminal_id(self, context: Dict[str, Any]) -> Optional[str]:
        """Try both strategies"""
        # Try context vars first (async)
        terminal_id = self._context_strategy.get_terminal_id(context)
        if terminal_id:
            return terminal_id

        # Try thread local (sync)
        terminal_id = self._thread_strategy.get_terminal_id(context)
        if terminal_id:
            return terminal_id

        # Final fallback
        return context.get('terminal_id')

    def set_terminal_context(self, terminal_id: str, terminal_number: int) -> Any:
        """Set both contexts"""
        tokens = []

        # Set context vars
        ctx_tokens = self._context_strategy.set_terminal_context(terminal_id, terminal_number)
        if ctx_tokens:
            tokens.extend(ctx_tokens)

        # Set thread local
        self._thread_strategy.set_terminal_context(terminal_id, terminal_number)

        return tokens


class OutputRouter:
    """Central router for terminal output"""

    _instance = None
    _strategy: IRoutingStrategy = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._strategy = HybridRoutingStrategy()
        return cls._instance

    def set_strategy(self, strategy: IRoutingStrategy) -> None:
        """Set routing strategy"""
        self._strategy = strategy

    def get_terminal_id(self, context: Optional[Dict[str, Any]] = None) -> Optional[str]:
        """Get current terminal ID"""
        context = context or {}
        return self._strategy.get_terminal_id(context)

    def set_terminal_context(self, terminal_id: str, terminal_number: int = 1) -> Any:
        """Set terminal context for current execution"""
        return self._strategy.set_terminal_context(terminal_id, terminal_number)

    def route_to_terminal(self, terminal_id: str, terminal_number: int = 1):
        """Context manager for routing to specific terminal"""
        return TerminalRoutingContext(terminal_id, terminal_number, self._strategy)

    # Convenience helpers
    def get_current_context(self) -> Tuple[Optional[str], Optional[int]]:
        """Return the current (terminal_id, terminal_number) from context vars."""
        return current_terminal_id.get(), current_terminal_number.get()

    def clear_current_context(self) -> None:
        """Clear context vars for terminal id/number."""
        try:
            current_terminal_id.set(None)
            current_terminal_number.set(None)
        except Exception:
            pass

    def set_terminal_id_only(self, terminal_id: str) -> Any:
        """Set only terminal id in context vars (keeps number unchanged)."""
        return current_terminal_id.set(terminal_id)


class TerminalRoutingContext:
    """Context manager for terminal routing"""

    def __init__(self, terminal_id: str, terminal_number: int, strategy: IRoutingStrategy):
        self.terminal_id = terminal_id
        self.terminal_number = terminal_number
        self.strategy = strategy
        self.tokens = None

    def __enter__(self):
        """Set terminal context"""
        self.tokens = self.strategy.set_terminal_context(self.terminal_id, self.terminal_number)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Clean up context"""
        # Context vars clean up automatically when tokens go out of scope
        pass


# Global router instance
output_router = OutputRouter()

# Global terminal outputs registry
_terminal_outputs = {}
_registry_lock = threading.RLock()


def register_terminal_output(terminal_id: str, output_widget: Any) -> None:
    """Register a terminal's output widget"""
    with _registry_lock:
        _terminal_outputs[terminal_id] = output_widget


def get_terminal_output(terminal_id: str) -> Any:
    """Get a terminal's output widget"""
    with _registry_lock:
        return _terminal_outputs.get(terminal_id)


class EnhancedTerminalRoutingContext:
    """Enhanced routing context that redirects stdout/stderr"""
    
    def __init__(self, terminal_id: str, output_widget: Any, terminal_number: int = 1):
        self.terminal_id = terminal_id
        self.output_widget = output_widget
        self.terminal_number = terminal_number
        self.old_stdout = None
        self.old_stderr = None
        self.old_print = None
        
    def __enter__(self):
        """Set up routing"""
        import sys
        
        # Save originals
        self.old_stdout = sys.stdout
        self.old_stderr = sys.stderr
        self.old_print = __builtins__.get('print', print)
        
        # Create wrapper for stdout/stderr
        class TerminalWriter:
            def __init__(self, widget, is_stderr=False):
                self.widget = widget
                self.is_stderr = is_stderr
                
            def write(self, text):
                if text and self.widget:
                    try:
                        if self.is_stderr:
                            self.widget.write(f"[red]{text}[/red]")
                        else:
                            self.widget.write(text)
                    except Exception:
                        # Fallback
                        pass
                return len(text) if text else 0
                
            def flush(self):
                pass
                
            def isatty(self):
                return False
                
        # Replace stdout/stderr
        sys.stdout = TerminalWriter(self.output_widget)
        sys.stderr = TerminalWriter(self.output_widget, is_stderr=True)
        
        # Replace print
        def terminal_print(*args, **kwargs):
            text = ' '.join(str(arg) for arg in args)
            if text and self.output_widget:
                self.output_widget.write(text)
                
        __builtins__['print'] = terminal_print
        
        # Also set context for other routing
        output_router.set_terminal_context(self.terminal_id, self.terminal_number)
        
        return self
        
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Restore routing"""
        import sys
        
        # Restore originals
        if self.old_stdout:
            sys.stdout = self.old_stdout
        if self.old_stderr:
            sys.stderr = self.old_stderr
        if self.old_print:
            __builtins__['print'] = self.old_print


# Helper functions
def get_current_terminal_id() -> Optional[str]:
    """Get current terminal ID"""
    return output_router.get_terminal_id()

def get_current_terminal_context() -> Tuple[Optional[str], Optional[int]]:
    """Get current (terminal_id, terminal_number) from context."""
    return output_router.get_current_context()


def set_terminal_context(terminal_id: str, terminal_number: int = 1) -> Any:
    """Set terminal context"""
    return output_router.set_terminal_context(terminal_id, terminal_number)

def clear_current_terminal_context() -> None:
    """Clear terminal context from context vars."""
    output_router.clear_current_context()

def set_terminal_id_only(terminal_id: str) -> Any:
    """Set only terminal id in context vars (keeps number)."""
    return output_router.set_terminal_id_only(terminal_id)


def route_to_terminal(terminal_id: str, terminal_output=None, terminal_number: int = 1):
    """Context manager for routing output to specific terminal
    
    Args:
        terminal_id: Terminal ID to route to
        terminal_output: Optional output widget (for direct routing)
        terminal_number: Terminal number (default 1)
    """
    if terminal_output:
        # Use enhanced routing with output widget
        return EnhancedTerminalRoutingContext(terminal_id, terminal_output, terminal_number)
    else:
        # Use standard routing
        return output_router.route_to_terminal(terminal_id, terminal_number)
