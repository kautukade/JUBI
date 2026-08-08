"""Parallel command package.

Split from original parallel.py monolith (2,058 LOC).
All public names are re-exported here so that existing imports like
``from cai.repl.commands.parallel import PARALLEL_CONFIGS`` continue to work.
"""

from cai.repl.commands._parallel_monolith import *  # noqa: F401,F403

# Explicit re-exports for static analysis and external consumers
from cai.repl.commands._parallel_monolith import (  # noqa: F811
    PARALLEL_CONFIGS,
    PARALLEL_AGENT_INSTANCES,
    PARALLEL_COMMAND_INSTANCE,
    ParallelConfig,
    ParallelCommand,
    load_parallel_config_from_yaml,
)

__all__ = [
    "PARALLEL_CONFIGS",
    "PARALLEL_AGENT_INSTANCES",
    "PARALLEL_COMMAND_INSTANCE",
    "ParallelConfig",
    "ParallelCommand",
    "load_parallel_config_from_yaml",
]
