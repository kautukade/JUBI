"""Jubi v0.1.0 compatibility entry package.

The production core still lives under the legacy ``sarus`` package during
Phase 0 so the tested installer/source/runtime integrations are not broken by a
mass rename. New user-facing entry points should import from ``jubi``.
"""

from sarus.core.app import Jubi

__all__ = ['Jubi']
__version__ = '0.1.0'
