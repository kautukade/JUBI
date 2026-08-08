"""
Terminal Queue - Per-terminal prompt queue management
"""

import asyncio
from typing import Dict, List, Optional, Any, Callable
from dataclasses import dataclass
from datetime import datetime
import logging


@dataclass
class QueuedPrompt:
    """A prompt waiting to be executed"""
    prompt: str
    timestamp: datetime = None
    priority: int = 0  # Higher priority executed first
    
    def __post_init__(self):
        if self.timestamp is None:
            self.timestamp = datetime.now()


class TerminalQueue:
    """Manages a queue of prompts for a specific terminal"""
    
    def __init__(self, terminal_number: int):
        self.terminal_number = terminal_number
        self._queue: List[QueuedPrompt] = []
        self._lock = asyncio.Lock()
        self._processing = False
        self._current_prompt: Optional[QueuedPrompt] = None
        self.logger = logging.getLogger(f"TerminalQueue-{terminal_number}")
        
    async def add_prompt(self, prompt: str, priority: int = 0) -> None:
        """Add a prompt to this terminal's queue"""
        async with self._lock:
            queued_prompt = QueuedPrompt(
                prompt=prompt,
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
            self.logger.info(f"Added prompt to T{self.terminal_number} queue: '{prompt[:50]}...' (priority: {priority})")
            
    async def get_next_prompt(self) -> Optional[str]:
        """Get the next prompt from queue"""
        async with self._lock:
            if not self._queue:
                return None
                
            self._current_prompt = self._queue.pop(0)
            self._processing = True
            
        return self._current_prompt.prompt
        
    async def mark_completed(self) -> None:
        """Mark current prompt as completed"""
        async with self._lock:
            self._processing = False
            self._current_prompt = None
            
    def get_queue_status(self) -> Dict[str, Any]:
        """Get current queue status"""
        return {
            "terminal": self.terminal_number,
            "queue_length": len(self._queue),
            "processing": self._processing,
            "current_prompt": self._current_prompt.prompt if self._current_prompt else None,
            "prompts": [
                {
                    "prompt": p.prompt[:50] + "..." if len(p.prompt) > 50 else p.prompt,
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
        self.logger.info(f"Cleared {count} prompts from T{self.terminal_number} queue")
        return count
        
    def is_busy(self) -> bool:
        """Check if this terminal is currently processing"""
        return self._processing
    
    def has_queued_prompts(self) -> bool:
        """Check if there are prompts in the queue"""
        return len(self._queue) > 0


class TerminalQueueManager:
    """Manages per-terminal queues for the TUI"""
    
    def __init__(self):
        self._queues: Dict[int, TerminalQueue] = {}
        self._lock = asyncio.Lock()
        self.logger = logging.getLogger("TerminalQueueManager")
        
    async def get_queue(self, terminal_number: int) -> TerminalQueue:
        """Get or create a queue for a terminal"""
        async with self._lock:
            if terminal_number not in self._queues:
                self._queues[terminal_number] = TerminalQueue(terminal_number)
            return self._queues[terminal_number]
            
    async def add_prompt(self, prompt: str, terminal_number: int, priority: int = 0) -> None:
        """Add a prompt to a specific terminal's queue"""
        queue = await self.get_queue(terminal_number)
        await queue.add_prompt(prompt, priority)
        
    async def get_next_prompt(self, terminal_number: int) -> Optional[str]:
        """Get next prompt for a terminal if it's not busy"""
        queue = await self.get_queue(terminal_number)
        return await queue.get_next_prompt()
        
    async def mark_completed(self, terminal_number: int) -> None:
        """Mark a terminal's current prompt as completed"""
        queue = await self.get_queue(terminal_number)
        await queue.mark_completed()
        
    async def is_terminal_busy(self, terminal_number: int) -> bool:
        """Check if a terminal is busy processing"""
        queue = await self.get_queue(terminal_number)
        return queue.is_busy()
        
    def get_all_queues_status(self) -> Dict[int, Dict[str, Any]]:
        """Get status of all terminal queues"""
        return {
            terminal_num: queue.get_queue_status()
            for terminal_num, queue in self._queues.items()
        }
        
    def clear_all_queues(self) -> int:
        """Clear all terminal queues"""
        total = 0
        for queue in self._queues.values():
            total += queue.clear_queue()
        return total
        
    def clear_terminal_queue(self, terminal_number: int) -> int:
        """Clear a specific terminal's queue"""
        if terminal_number in self._queues:
            return self._queues[terminal_number].clear_queue()
        return 0


# Global terminal queue manager instance
TERMINAL_QUEUE_MANAGER = TerminalQueueManager()