"""Hardware-friendly Hermes context admission for local laptop models.

This module intentionally does not own model routing or provider policy. Jubi's
Brain chooses candidates and Local Only validates transport/model provenance.
These helpers only decide whether a validated local model can run a deliberately
small Hermes task without forcing the upstream runtime's largest context size.
"""
from __future__ import annotations

MIN_COMPACT_CONTEXT = 8192
TARGET_COMPACT_CONTEXT = 8192
ANALYSIS_MIN_CONTEXT = 4096
ANALYSIS_TARGET_CONTEXT = 4096


def model_context_capacity(metadata: dict) -> int:
    """Return the largest advertised model context length, or zero."""
    info = metadata.get('model_info') or {}
    values = [int(value) for key, value in info.items()
              if str(key).endswith('.context_length') and isinstance(value, int)]
    return max(values or [0])


def has_native_tools(metadata: dict) -> bool:
    """Use Ollama's explicit capability metadata rather than model-name guesses."""
    capabilities = metadata.get('capabilities') or []
    return 'tools' in capabilities


def compact_profile(metadata: dict, *, require_tools: bool = True,
                    min_context: int = MIN_COMPACT_CONTEXT,
                    target_context: int = TARGET_COMPACT_CONTEXT) -> dict:
    """Return truthful admission metadata for a bounded Hermes task.

    The runtime context is capped deliberately. A 32K model should not allocate
    or advertise 32K/64K just because that capacity exists when the task only
    needs a small tool schema and progressive file reads.
    """
    capacity = model_context_capacity(metadata)
    tools = has_native_tools(metadata)
    if require_tools and not tools:
        return {
            'eligible': False,
            'capacity': capacity,
            'runtime_context': 0,
            'native_tools': False,
            'reason': 'Model does not advertise native tool calling',
        }
    if capacity < int(min_context):
        return {
            'eligible': False,
            'capacity': capacity,
            'runtime_context': 0,
            'native_tools': tools,
            'reason': f'Model context {capacity} is below compact minimum {int(min_context)}',
        }
    runtime_context = min(capacity, max(int(min_context), int(target_context)))
    return {
        'eligible': True,
        'capacity': capacity,
        'runtime_context': runtime_context,
        'native_tools': tools,
        'reason': 'Eligible for bounded Hermes compact mode',
    }


def compact_excerpt(text: str, max_chars: int = 5000) -> str:
    """Bound optional workflow guidance without pretending it is full context."""
    value = str(text or '')
    if len(value) <= max_chars:
        return value
    head = max_chars * 3 // 4
    tail = max_chars - head
    return value[:head] + '\n\n[... compacted by Jubi ...]\n\n' + value[-tail:]
