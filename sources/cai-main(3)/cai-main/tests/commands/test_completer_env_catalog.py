"""
Tests for REPL catalog completion: ``/env get|set`` and ``/help var`` share targets (name + #).
"""

from __future__ import annotations

from unittest.mock import MagicMock, patch

import pytest
from prompt_toolkit.document import Document


def _catalog_index_str(var_name: str) -> str:
    from cai.repl.commands.env_catalog import ENV_VARS

    for num, info in ENV_VARS.items():
        if info["name"] == var_name:
            return str(num)
    raise AssertionError(f"{var_name} not in ENV_VARS")


def _completer_for_tests():
    """``FuzzyCommandCompleter`` without background threads or model/agent HTTP fetches."""
    from cai.repl.commands import _ensure_command_loaded
    from cai.repl.commands.completer import FuzzyCommandCompleter

    # Register ``/h`` → ``/help`` (and ``/?``) so completion matches the live REPL.
    _ensure_command_loaded("help")
    _ensure_command_loaded("resume")

    with patch("cai.repl.commands.completer.threading.Thread", lambda *a, **kw: MagicMock()):
        c = FuzzyCommandCompleter()
    c.fetch_all_models = lambda: None
    c.fetch_all_agents = lambda: None
    return c


def _completion_texts(document_text: str) -> set[str]:
    c = _completer_for_tests()
    doc = Document(document_text, cursor_position=len(document_text))
    return {comp.text for comp in c.get_completions(doc, None)}


class TestGetEnvCatalogTargetSuggestions:
    """Unit tests for ``get_env_catalog_target_suggestions`` (shared by /env and /help var)."""

    def test_digit_prefix_returns_catalog_index_as_completion_text(self):
        from cai.repl.commands.completer import FuzzyCommandCompleter

        c = FuzzyCommandCompleter.__new__(FuzzyCommandCompleter)
        idx = _catalog_index_str("CAI_MODEL")
        out = [x.text for x in c.get_env_catalog_target_suggestions(idx)]
        assert idx in out

    def test_empty_current_word_yields_variable_names(self):
        from cai.repl.commands.completer import FuzzyCommandCompleter

        c = FuzzyCommandCompleter.__new__(FuzzyCommandCompleter)
        texts = {x.text for x in c.get_env_catalog_target_suggestions("")}
        assert "CAI_MODEL" in texts
        assert "CTF_NAME" in texts

    def test_name_substring_finds_catalog_var(self):
        from cai.repl.commands.completer import FuzzyCommandCompleter

        c = FuzzyCommandCompleter.__new__(FuzzyCommandCompleter)
        texts = {x.text for x in c.get_env_catalog_target_suggestions("PRICE_LIM")}
        assert "CAI_PRICE_LIMIT" in texts


class TestCompleterHelpVarMatchesEnvGetSet:
    """``/help var <TAB>`` must offer the same catalog targets as ``/env get|set``."""

    @pytest.mark.parametrize("cmd_prefix", ["/help var ", "/h var ", "/? var "])
    def test_trailing_space_third_token_same_as_env_get(self, cmd_prefix: str):
        help_texts = _completion_texts(cmd_prefix)
        env_texts = _completion_texts("/env get ")
        assert help_texts == env_texts
        assert "CAI_MODEL" in help_texts

    def test_numeric_prefix_same_completions_help_var_env_get_env_set(self):
        idx = _catalog_index_str("CAI_MODEL")
        # Prefix with first digit so multiple rows may match (e.g. 9, 90…); still identical pools
        digit = idx[0]
        h = _completion_texts(f"/help var {digit}")
        g = _completion_texts(f"/env get {digit}")
        s = _completion_texts(f"/env set {digit}")
        assert h == g == s
        if len(idx) == 1:
            assert idx in h

    def test_full_catalog_index_identical_help_var_and_env_get(self):
        idx = _catalog_index_str("CAI_PRICE_LIMIT")
        h = _completion_texts(f"/help var {idx}")
        g = _completion_texts(f"/env get {idx}")
        assert h == g
        assert idx in h


class TestResumeDirTokenCompletions:
    """Third-token completion for ``/resume <dir> <token>``."""

    def test_suggests_jsonl_basenames_under_dir(self, tmp_path):
        (tmp_path / "cai_abc_one.jsonl").write_text("[]", encoding="utf-8")
        (tmp_path / "other_log.jsonl").write_text("[]", encoding="utf-8")
        d = str(tmp_path)
        texts = _completion_texts(f"/resume {d} ")
        assert "cai_abc_one.jsonl" in texts
        assert "other_log.jsonl" in texts

    def test_alias_r_same_pool(self, tmp_path):
        (tmp_path / "sess.jsonl").write_text("[]", encoding="utf-8")
        d = str(tmp_path)
        assert _completion_texts(f"/r {d} ") == _completion_texts(f"/resume {d} ")

    def test_prefix_filters_by_filename(self, tmp_path):
        (tmp_path / "cai_only.jsonl").write_text("[]", encoding="utf-8")
        (tmp_path / "nomatch.jsonl").write_text("[]", encoding="utf-8")
        d = str(tmp_path)
        texts = _completion_texts(f"/resume {d} cai")
        assert "cai_only.jsonl" in texts
        assert "nomatch.jsonl" not in texts

    def test_last_skips_dir_token_suggestions(self, tmp_path):
        """``last`` is reserved as first arg, not a directory scan."""
        (tmp_path / "x.jsonl").write_text("[]", encoding="utf-8")
        texts = _completion_texts("/resume last ")
        assert "x.jsonl" not in texts
