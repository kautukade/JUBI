"""``cai.repl.commands.settings`` — REPL ``/settings`` helpers and ``SettingsCommand``.

Implementation is mostly in ``_settings_monolith.py``; ``settings/general.py`` holds
env-file helpers extracted from that module.
"""

from cai.repl.commands.settings.general import (  # noqa: F401
    CLI_ONLY_VARIABLES,
    TUI_ONLY_VARIABLES,
    custom_style,
    delete_env_variable,
    filter_variables_for_mode,
    get_current_terminal_id,
    get_current_value,
    get_env_file_path,
    is_boolean_variable,
    is_tui_mode,
    read_env_file,
    update_env_file,
    write_env_file,
)

from cai.repl.commands._settings_monolith import *  # noqa: F401, F403
from cai.repl.commands._settings_monolith import (  # noqa: F811
    ADDITIONAL_VARS,
    SETTINGS_VARIABLES,
    SettingsCommand,
    TUISettingsState,
    add_new_api_key,
    delete_api_key_interactive,
    get_all_vars,
    get_current_language,
    get_tui_state,
    get_variables_by_category,
    prompt_for_variable,
    select_language,
    set_current_language,
    show_api_key_validation,
    show_faq_menu,
    show_system_status,
    tr,
)
