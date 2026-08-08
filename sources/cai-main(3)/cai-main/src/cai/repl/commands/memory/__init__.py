"""Memory command package — manages memory storage in ``.cai/memory``.

Implementation lives in ``_memory_monolith.py``; public names are re-exported
here so existing imports like ``from cai.repl.commands.memory import X`` work.
"""

# Re-export everything from the monolith for full backward compatibility.
from cai.repl.commands._memory_monolith import *  # noqa: F401, F403

# Explicit re-exports for externally referenced names
from cai.repl.commands._memory_monolith import (  # noqa: F811
    APPLIED_MEMORY_IDS,
    COMPACTED_SUMMARIES,
    MEMORY_COMMAND_INSTANCE,
    MEMORY_DIR,
    MEMORY_INDEX_FILE,
    MemoryCommand,
    get_applied_memory_id,
    get_applied_memory_ids,
    get_compacted_summary,
)
