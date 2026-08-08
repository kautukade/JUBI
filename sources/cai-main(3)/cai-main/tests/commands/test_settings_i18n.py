"""Tests for /settings i18n shell strings and category label mapping."""

from cai.repl.commands.settings_i18n import UI_STRINGS, get_string


def test_ui_strings_merge_fills_non_en_locales() -> None:
    """Non-English locales inherit new keys from English via setdefault."""
    assert "lang_selection_title" in UI_STRINGS["en"]
    assert UI_STRINGS["ru"]["lang_selection_title"] == UI_STRINGS["en"]["lang_selection_title"]
    assert UI_STRINGS["ja"]["change_language"] == UI_STRINGS["en"]["change_language"]


def test_spanish_category_and_shell_strings() -> None:
    assert get_string("cat_api_keys", "es") == "Claves API"
    assert get_string("change_language", "es") == "Cambiar idioma"
    assert "Ctrl+C" in get_string("settings_footer_hint", "es")


def test_category_internal_key_unchanged_spanish_label() -> None:
    """Spanish label for API Keys category (display), distinct from English key."""
    assert get_string("cat_api_keys", "es") != "API Keys"
    assert get_string("cat_api_keys", "en") == "API Keys"
