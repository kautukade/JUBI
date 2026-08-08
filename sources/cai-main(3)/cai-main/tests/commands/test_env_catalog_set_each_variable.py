#!/usr/bin/env python3
"""
Stress-test ``handle_env_catalog_set`` for every catalog entry with a valid sample value.

Ensures validation + set path does not raise for any variable (no REPL crash on /env set).
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

from cai.repl.commands.env_catalog import ENV_VARS, handle_env_catalog_set
from cai.repl.commands.env_catalog_validate import sample_valid_test_value
from cai.repl.commands.env_info_catalog import is_restart_required

_CATALOG_ROWS = sorted(ENV_VARS.items(), key=lambda x: x[0])


@pytest.mark.parametrize(
    "num,var_info",
    _CATALOG_ROWS,
    ids=[f"{n}_{d['name']}" for n, d in _CATALOG_ROWS],
)
def test_env_set_single_catalog_entry_no_exception(num, var_info):
    """Every catalog entry is dispatched without raising.

    Restart-required vars (locked at startup) are expected to be rejected with
    ``False`` and a user-facing error; runtime-friendly vars must succeed.
    """
    name = str(var_info["name"])
    value = sample_valid_test_value(name, var_info)
    old = os.environ.get(name)
    try:
        ok = handle_env_catalog_set([str(num), value])
        if is_restart_required(name):
            assert ok is False, (
                f"/env set should refuse locked-at-startup #{num} {name}"
            )
        else:
            assert ok is True, (
                f"/env set failed for #{num} {name} value={value!r}"
            )
    finally:
        if old is not None:
            os.environ[name] = old
        else:
            os.environ.pop(name, None)
