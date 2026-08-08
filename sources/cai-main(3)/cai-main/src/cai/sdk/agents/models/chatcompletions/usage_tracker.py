"""Cost and token usage tracking.

Provides CustomResponseUsage and InputTokensDetails for compatibility
between different LLM provider token/cost naming conventions, plus
COST_TRACKER integration helpers.
"""

from __future__ import annotations

from typing import Optional

from openai._models import BaseModel
from openai.types.responses import ResponseUsage


class InputTokensDetails(BaseModel):
    prompt_tokens: int
    """The number of prompt tokens."""
    cached_tokens: int = 0
    """The number of cached tokens."""


class CustomResponseUsage(ResponseUsage):
    """
    Custom ResponseUsage class that provides compatibility between different field naming conventions.
    Works with both input_tokens/output_tokens and prompt_tokens/completion_tokens.
    Also supports cache metrics from Anthropic/Claude models.
    """

    # Add cache metrics as optional fields
    cache_creation_input_tokens: Optional[int] = None
    cache_read_input_tokens: Optional[int] = None

    @property
    def prompt_tokens(self) -> int:
        """Alias for input_tokens to maintain compatibility"""
        return self.input_tokens

    @property
    def completion_tokens(self) -> int:
        """Alias for output_tokens to maintain compatibility"""
        return self.output_tokens


# Rebuild Pydantic models to resolve forward references from __future__ annotations
CustomResponseUsage.model_rebuild()
