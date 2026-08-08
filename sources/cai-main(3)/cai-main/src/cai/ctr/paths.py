"""
CTR path utilities.

Provides a single source of truth for where CTR run artifacts are written and read
from. Uses an environment override when provided and falls back to the system temp
directory for portability across platforms.
"""

from __future__ import annotations

import os
import tempfile
from typing import Optional


def get_ctr_output_base_dir(override: Optional[str] = None) -> str:
    """Resolve the base directory for CTR outputs.

    Order of precedence:
    - Explicit override provided to the function
    - Environment variable `CAI_CTR_OUTPUT_DIR`
    - System temporary directory at `<tempdir>/cai/ctr`
    """
    base = (
        override
        or os.getenv("CAI_CTR_OUTPUT_DIR")
        or os.path.join(tempfile.gettempdir(), "cai", "ctr")
    )
    try:
        os.makedirs(base, exist_ok=True)
    except Exception:
        # As a last resort, fall back to tempdir without nested folders
        base = os.path.join(tempfile.gettempdir(), "ctr")
        os.makedirs(base, exist_ok=True)
    return base

