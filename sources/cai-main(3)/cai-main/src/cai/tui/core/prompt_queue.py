"""
Prompt Queue - Manages queued prompts for sequential execution
"""

import asyncio
from typing import List, Optional, Dict, Any, Callable
from dataclasses import dataclass
from datetime import datetime
import logging


@dataclass
class QueuedPrompt:
    """A prompt waiting to be executed"""
    prompt: str
    terminal_number: Optional[int] = None  # None means all terminals
    timestamp: datetime = None
    priority: int = 0  # Higher priority executed first
    
    def __post_init__(self):
        if self.timestamp is None:
            self.timestamp = datetime.now()


class PromptQueue:
    """Manages a queue of prompts for execution"""
    
    def __init__(self):
        self._queue: List[QueuedPrompt] = []
        self._lock = asyncio.Lock()
        self._processing = False
        self._process_task: Optional[asyncio.Task] = None
        self._current_prompt: Optional[QueuedPrompt] = None
        self._process_callback: Optional[Callable] = None
        self.logger = logging.getLogger("PromptQueue")
        
    def set_process_callback(self, callback: Callable) -> None:
        """Set the callback function to process prompts"""
        self._process_callback = callback
        
    async def add_prompt(self, prompt: str, terminal_number: Optional[int] = None, priority: int = 0) -> None:
        """Add a prompt to the queue"""
        async with self._lock:
            queued_prompt = QueuedPrompt(
                prompt=prompt,
                terminal_number=terminal_number,
                priority=priority
            )
            
            # Insert based on priority (higher priority first)
            insert_pos = 0
            for i, existing in enumerate(self._queue):
                if existing.priority < priority:
                    insert_pos = i
                    break
                insert_pos = i + 1
                
            self._queue.insert(insert_pos, queued_prompt)
            self.logger.info(f"Added prompt to queue: '{prompt[:50]}...' (priority: {priority})")
            
        # Start processing if not already running
        if not self._processing:
            # single runner task
            self._process_task = asyncio.create_task(self._process_queue(), name="tui-prompt-queue")
            
    async def _process_queue(self) -> None:
        """Process prompts from the queue"""
        async with self._lock:
            if self._processing:
                return
            self._processing = True
            
        try:
            while True:
                # Get next prompt
                async with self._lock:
                    if not self._queue:
                        break
                    self._current_prompt = self._queue.pop(0)
                    
                # Process the prompt
                if self._process_callback:
                    try:
                        await self._process_callback(
                            self._current_prompt.prompt,
                            self._current_prompt.terminal_number
                        )
                    except Exception as e:
                        self.logger.error(f"Error processing prompt: {e}")
                        
                # Small delay between prompts (default unchanged)
                await asyncio.sleep(0.5)
                
        finally:
            async with self._lock:
                self._processing = False
                self._current_prompt = None
                self._process_task = None
                
    def get_queue_status(self) -> Dict[str, Any]:
        """Get current queue status"""
        return {
            "queue_length": len(self._queue),
            "processing": self._processing,
            "current_prompt": self._current_prompt.prompt if self._current_prompt else None,
            "prompts": [
                {
                    "prompt": p.prompt[:50] + "..." if len(p.prompt) > 50 else p.prompt,
                    "terminal": p.terminal_number,
                    "priority": p.priority,
                    "timestamp": p.timestamp.isoformat()
                }
                for p in self._queue[:5]  # Show first 5 prompts
            ]
        }
        
    def clear_queue(self) -> int:
        """Clear all queued prompts and return count cleared"""
        count = len(self._queue)
        self._queue.clear()
        self.logger.info(f"Cleared {count} prompts from queue")
        return count
        
    def remove_prompt(self, index: int) -> bool:
        """Remove a specific prompt by index"""
        if 0 <= index < len(self._queue):
            removed = self._queue.pop(index)
            self.logger.info(f"Removed prompt: '{removed.prompt[:50]}...'")
            return True
        return False
        
    def get_queue_size(self) -> int:
        """Get current queue size"""
        return len(self._queue)
        
    def is_processing(self) -> bool:
        """Check if queue is currently processing"""
        return self._processing


# Global prompt queue instance
PROMPT_QUEUE = PromptQueue()
