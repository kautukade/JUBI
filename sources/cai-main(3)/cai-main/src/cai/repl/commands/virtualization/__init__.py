"""Virtualization command package.

Re-exports the REPL Docker helpers from the monolith so imports like
``from cai.repl.commands.virtualization import DockerManager`` stay stable.
"""

from cai.repl.commands._virtualization_monolith import (
    DEFAULT_IMAGES,
    DockerManager,
    VirtualizationCommand,
    normalize_image_name,
)

__all__ = [
    "DEFAULT_IMAGES",
    "DockerManager",
    "VirtualizationCommand",
    "normalize_image_name",
]
