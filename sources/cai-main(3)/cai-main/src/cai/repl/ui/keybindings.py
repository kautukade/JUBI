"""
Module for CAI REPL key bindings.
"""

# pylint: disable=import-error
from prompt_toolkit.application import run_in_terminal
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.auto_suggest import AutoSuggestFromHistory
from cai.repl.commands import FuzzyCommandCompleter


def create_key_bindings(current_text):
    """
    Create key bindings for the REPL.

    Args:
        current_text: Reference to the current text for command shadowing

    Returns:
        KeyBindings object with configured bindings
    """
    kb = KeyBindings()

    @kb.add("c-d", eager=True)
    def _discard_eof_when_empty(event):
        """Buffer empty: swallow key (no submit, no exit). Non-empty: delete char forward."""
        buf = event.current_buffer
        if buf.text:
            buf.delete()
        # else: intentional no-op — same effect as an unbound key (e.g. Ctrl+F)

    @kb.add("?")
    def _repl_input_shortcuts_on_question(event):
        """Empty buffer: show Input shortcuts immediately (no Enter). Non-empty: insert '?'."""
        buf = event.current_buffer
        if buf.text:
            buf.insert_text("?")
            return

        def _show() -> None:
            try:
                from rich.console import Console

                from cai.repl.ui.repl_input_shortcuts import print_repl_input_shortcuts
                from cai.repl.ui.prompt import _print_separator

                print_repl_input_shortcuts(Console())
                # Same green line as get_user_input() — that line is only printed once
                # before prompt(); run_in_terminal breaks the visual frame, so redraw it here.
                _print_separator()
            except Exception:
                # Never break the REPL for a help panel
                pass

        run_in_terminal(_show)
        # Do not insert '?' — avoids a duplicate panel if the user presses Enter afterwards.

    @kb.add("c-l")
    def _(event):
        """Clear the screen."""
        event.app.renderer.clear()

    @kb.add("c-o")
    def _expand_task(event):
        """Open the compact-mode task expand popup (lists tasks of the current turn)."""

        def _show() -> None:
            try:
                from cai.repl.ui.expand_popup import open_expand_popup

                open_expand_popup()
            except Exception:
                # Never crash the REPL because of the expand popup
                pass

        run_in_terminal(_show)

    @kb.add("tab")
    def handle_tab(event):
        """Handle tab key to show completions menu or complete command."""
        buffer = event.current_buffer
        text = buffer.text

        # Update current text for shadow
        current_text[0] = text

        # First check if we have a history suggestion
        history_suggestion = None
        if text:
            # Get suggestion from history
            auto_suggest = AutoSuggestFromHistory()
            suggestion = auto_suggest.get_suggestion(buffer, buffer.document)
            if suggestion and suggestion.text:
                history_suggestion = text + suggestion.text

        # If we have a history suggestion, use it
        if history_suggestion:
            buffer.text = history_suggestion
            buffer.cursor_position = len(history_suggestion)
        else:
            # If no history suggestion, check for command shadow from the active completer
            shadow = None
            completer = getattr(buffer, "completer", None)
            if completer is None and hasattr(event.app, "completer"):
                completer = event.app.completer

            if completer is None:
                # Fallback to a fresh instance (kept for backward compatibility)
                completer = FuzzyCommandCompleter()

            if hasattr(completer, "get_command_shadow"):
                try:
                    shadow = completer.get_command_shadow(text)
                except Exception:  # pylint: disable=broad-except
                    shadow = None

            if shadow and shadow.startswith(text):
                # Complete with the shadow
                buffer.text = shadow
                buffer.cursor_position = len(shadow)
            # If no shadow or shadow is the same as current text
            elif buffer.complete_state:
                # If completion menu is already showing, select the next item
                buffer.complete_next()
            else:
                # Otherwise, start completion
                buffer.start_completion(select_first=True)

    @kb.add("enter")
    @kb.add("c-m")
    def handle_enter(event):
        """
        Submit input on Enter / Return (CR).

        With multiline=True, Enter would normally insert a newline.
        This binding overrides that to submit the input, allowing
        pasted multiline content to be preserved while Enter still submits.
        ``c-m`` is bound explicitly because some TTY states after Rich Live /
        compact UI only deliver carriage return under that name, not ``enter``.
        Use Shift+Enter or Alt+Enter to insert actual newlines.
        """
        event.current_buffer.validate_and_handle()

    # Note: prompt_toolkit represents Alt+Enter as the ("escape", "enter")
    # key sequence because that's the byte stream terminals emit for Alt+Enter.
    @kb.add("c-j")
    @kb.add("escape", "enter")
    def handle_newline(event):
        """
        Insert a newline in the input buffer.

        - Shift+Enter: works in terminals that send LF (c-j) for Shift+Enter
          while Enter sends CR (c-m). Supported by iTerm2, Kitty, WezTerm,
          Alacritty (with CSI-u) and most modern terminals when configured.
        - Alt+Enter: universal fallback for terminals that don't distinguish
          Shift+Enter from Enter.
        """
        event.current_buffer.insert_text("\n")

    return kb
