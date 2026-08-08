from dataclasses import dataclass
from typing import Optional


@dataclass
class Usage:
    requests: int = 0
    """Total requests made to the LLM API."""

    input_tokens: int = 0
    """Total input tokens sent, across all requests."""

    output_tokens: int = 0
    """Total output tokens received, across all requests."""

    total_tokens: int = 0
    """Total tokens sent and received, across all requests."""

    cache_creation_input_tokens: Optional[int] = None
    """Tokens written to cache (extra cost for cache writes)."""

    cache_read_input_tokens: Optional[int] = None
    """Tokens read from cache (savings from cache hits)."""

    def add(self, other: "Usage") -> None:
        self.requests += other.requests if other.requests else 0
        self.input_tokens += other.input_tokens if other.input_tokens else 0
        self.output_tokens += other.output_tokens if other.output_tokens else 0
        self.total_tokens += other.total_tokens if other.total_tokens else 0
        # Add cache metrics (handle None values)
        if other.cache_creation_input_tokens:
            self.cache_creation_input_tokens = (self.cache_creation_input_tokens or 0) + other.cache_creation_input_tokens
        if other.cache_read_input_tokens:
            self.cache_read_input_tokens = (self.cache_read_input_tokens or 0) + other.cache_read_input_tokens
