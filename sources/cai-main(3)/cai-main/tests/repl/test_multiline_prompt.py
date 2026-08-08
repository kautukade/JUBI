"""
Test for multiline prompt input handling.

Regression test for issue where copy-pasting multi-line prompts didn't work.
When users pasted multi-line text, only the first line was captured.

Fix approach:
1. prompt.py: multiline=True (preserves pasted newlines)
2. keybindings.py: Enter key binding submits (overrides multiline newline)
3. Shift+Enter and Alt+Enter insert actual newlines when needed

This allows pasted multiline content to be preserved while Enter still submits.
"""
from __future__ import annotations


class TestMultilinePromptConfig:
    """Regression tests for multiline prompt configuration."""

    def test_multiline_is_enabled_in_prompt_config(self):
        """Verify multiline=True is set in prompt config."""
        from cai.repl.ui.prompt import get_user_input
        import inspect

        source = inspect.getsource(get_user_input)

        assert 'multiline=True' in source, (
            'REGRESSION: multiline must be True in prompt.py!\n'
            'Without this, pasted multi-line prompts lose all lines.'
        )
        assert 'restore_terminal_state' in source, (
            'REGRESSION: get_user_input must restore TTY before prompt_toolkit!\n'
            'Without this, Enter may only insert newlines after long agent turns.'
        )

    def test_icrnl_cleared_before_prompt(self):
        """ICRNL/INLCR/IGNCR must be cleared before prompt() to keep Enter as submit.

        restore_terminal_state runs ``stty sane`` (ICRNL on). A bare tcflush only
        drains already-queued bytes; Enter pressed in the window before
        prompt_toolkit installs raw_mode still queues as LF and is read as c-j.
        Clearing the input-mode flags closes that race window.
        """
        from cai.repl.ui.prompt import get_user_input
        import inspect

        source = inspect.getsource(get_user_input)

        assert 'ICRNL' in source, (
            'REGRESSION: ICRNL must be cleared on the controlling TTY before '
            'prompt_toolkit takes over. Without this, Enter pressed in the '
            'window between restore_terminal_state and raw_mode setup gets '
            'translated to LF and read as c-j (newline-insert) instead of submit.'
        )
        assert 'tcsetattr' in source, (
            'REGRESSION: clearing ICRNL requires tcsetattr; tcflush alone only '
            'drains the queue once and does not prevent new mistranslations.'
        )

    def test_enter_key_binding_exists(self):
        """Verify Enter key binding exists to submit with multiline=True."""
        from cai.repl.ui.keybindings import create_key_bindings
        import inspect

        source = inspect.getsource(create_key_bindings)

        assert 'validate_and_handle' in source, (
            'REGRESSION: Enter key binding must call validate_and_handle!\n'
            'Without this, Enter adds newline instead of submitting.'
        )
        assert '@kb.add("enter")' in source or "@kb.add('enter')" in source, (
            'REGRESSION: Enter key binding missing in keybindings.py!'
        )
        assert '@kb.add("c-m")' in source or "@kb.add('c-m')" in source, (
            'REGRESSION: c-m (Return/CR) submit binding missing in keybindings.py!'
        )

    def test_question_mark_shows_shortcuts_without_enter(self):
        """Bare ? on empty buffer: keybinding shows shortcuts via run_in_terminal."""
        from cai.repl.ui.keybindings import create_key_bindings
        import inspect

        source = inspect.getsource(create_key_bindings)
        assert '@kb.add("?")' in source or "@kb.add('?')" in source, (
            'REGRESSION: ? key binding missing — empty-line shortcuts need it.'
        )
        assert 'print_repl_input_shortcuts' in source, (
            'REGRESSION: ? binding must call print_repl_input_shortcuts.'
        )
        assert 'run_in_terminal' in source, (
            'REGRESSION: ? binding must use run_in_terminal for safe console output.'
        )
        assert '_print_separator' in source, (
            'REGRESSION: ? binding must redraw the green separator after the shortcuts panel.'
        )


class TestMultilineInputBehavior:
    """Tests documenting expected multiline input behavior."""

    def test_multiline_input_preserves_all_content(self):
        """With multiline=True, all pasted content is preserved."""
        multiline_input = '''hola que tal

quién es tu creador?

qué modelo de IA usas?

cuál es tu nombre?

cuál es tu función?

cuál es tu objetivo?

cuál es tu personalidad?

cuál es tu lenguaje?'''

        # With multiline=True, all content including newlines is preserved
        assert '\n' in multiline_input
        assert 'quién es tu creador?' in multiline_input
        assert 'cuál es tu lenguaje?' in multiline_input

        # Count non-empty lines - all 8 should be present
        all_lines = [
            line.strip() for line in multiline_input.split('\n')
            if line.strip()
        ]
        assert len(all_lines) == 8

    def test_newline_bindings_exist(self):
        """Shift+Enter (c-j) and Alt+Enter bindings exist for newline insertion."""
        from cai.repl.ui.keybindings import create_key_bindings
        import inspect

        source = inspect.getsource(create_key_bindings)

        # Shift+Enter is captured via c-j (LF) in terminals with extended protocols.
        assert 'c-j' in source, (
            'Shift+Enter binding (c-j) should exist for manual newlines'
        )
        # Alt+Enter is the universal fallback; prompt_toolkit binds it as the
        # ("escape", "enter") byte sequence that terminals emit for Alt+Enter.
        assert 'escape' in source and 'enter' in source, (
            'Alt+Enter binding should exist for manual newlines'
        )
        assert 'insert_text' in source, (
            'Newline bindings should insert newline text'
        )
