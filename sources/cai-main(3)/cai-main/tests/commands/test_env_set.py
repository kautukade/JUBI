"""
Tests for /env set / get / default (catalog by name or number).
"""
from __future__ import annotations

import os


def _price_limit_index() -> str:
    from cai.repl.commands.env_catalog import ENV_VARS

    for num, var_info in ENV_VARS.items():
        if var_info["name"] == "CAI_PRICE_LIMIT":
            return str(num)
    raise AssertionError("CAI_PRICE_LIMIT not in ENV_VARS")


class TestEnvSetSyntax:
    """/env set only via subcommand."""

    def test_env_set_by_name(self):
        from cai.repl.commands.env import EnvCommand

        cmd = EnvCommand()
        original = os.environ.get("CAI_PRICE_LIMIT")
        try:
            assert cmd.handle(["set", "CAI_PRICE_LIMIT", "750"]) is True
            assert os.environ.get("CAI_PRICE_LIMIT") == "750"
        finally:
            if original:
                os.environ["CAI_PRICE_LIMIT"] = original
            elif "CAI_PRICE_LIMIT" in os.environ:
                del os.environ["CAI_PRICE_LIMIT"]

    def test_env_set_by_number(self):
        from cai.repl.commands.env import EnvCommand

        cmd = EnvCommand()
        original = os.environ.get("CAI_PRICE_LIMIT")
        idx = _price_limit_index()
        try:
            assert cmd.handle(["set", idx, "999"]) is True
            assert os.environ.get("CAI_PRICE_LIMIT") == "999"
        finally:
            if original:
                os.environ["CAI_PRICE_LIMIT"] = original
            elif "CAI_PRICE_LIMIT" in os.environ:
                del os.environ["CAI_PRICE_LIMIT"]

    def test_env_set_value_with_spaces(self):
        from cai.repl.commands.env import EnvCommand

        cmd = EnvCommand()
        original = os.environ.get("CAI_PATTERN_DESCRIPTION")
        try:
            assert cmd.handle(["set", "CAI_PATTERN_DESCRIPTION", "hello", "world"]) is True
            assert os.environ.get("CAI_PATTERN_DESCRIPTION") == "hello world"
        finally:
            if original:
                os.environ["CAI_PATTERN_DESCRIPTION"] = original
            elif "CAI_PATTERN_DESCRIPTION" in os.environ:
                del os.environ["CAI_PATTERN_DESCRIPTION"]

    def test_env_rejects_unknown_at_root(self):
        from cai.repl.commands.env import EnvCommand

        cmd = EnvCommand()
        assert cmd.handle(["UNKNOWN_VAR=123"]) is False

    def test_env_rejects_unknown_set(self):
        from cai.repl.commands.env import EnvCommand

        assert EnvCommand().handle(["set", "NOT_A_VAR", "x"]) is False
