"""
Internationalization (i18n) support for CAI settings command.
Contains all translatable strings and FAQ content for multiple languages.
"""

from typing import Dict, Any

# Supported languages with their display names
SUPPORTED_LANGUAGES = {
    'en': 'English',
    'es': 'Español',
    'ru': 'Русский',
    'zh': '中文',
    'hi': 'हिन्दी',
    'pt': 'Português',
    'fr': 'Français',
    'de': 'Deutsch',
    'ja': '日本語',
    'ko': '한국어',
}

# Default language
DEFAULT_LANGUAGE = 'en'

# =============================================================================
# UI Strings - All translatable interface text
# =============================================================================
UI_STRINGS: Dict[str, Dict[str, str]] = {
    'en': {
        # Main UI
        'title': 'CAI Interactive Settings',
        'subtitle': 'Configure all environment variables and troubleshoot issues',
        'select_language': 'Select your language',
        'select_category': 'Select a category to configure',
        'select_variable': 'Select a variable to configure',
        'exit': 'Exit',
        'back': 'Back',
        'save': 'Save',
        'cancel': 'Cancel',
        'current_value': 'Current value',
        'not_set': 'Not set',
        'success': 'Successfully updated',
        'error': 'Error',
        'configure_more': 'Configure another variable?',

        # Categories
        'cat_ctf': 'CTF Variables',
        'cat_core': 'Core CAI Settings',
        'cat_api_keys': 'API Keys',
        'cat_streaming': 'Streaming & Output',
        'cat_parallel': 'Parallelization',
        'cat_limits': 'Execution Limits',
        'cat_memory': 'Memory & Context',
        'cat_workspace': 'Workspace',
        'cat_support': 'Support Agent',
        'cat_ctr': 'CTR Settings',
        'cat_tracing': 'Tracing & Telemetry',
        'cat_security': 'Security',
        'cat_pricing': 'Pricing & Cost',
        'cat_reporting': 'Reporting',
        'cat_api_server': 'API Server',
        'cat_auth': 'Authentication',
        'cat_mcp': 'MCP Settings',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (Local Models)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUI Mode',
        'cat_advanced': 'Advanced',
        'cat_faq': 'FAQ & Troubleshooting',

        # FAQ Section
        'faq_title': 'Frequently Asked Questions & Troubleshooting',
        'faq_select': 'Select a topic for help',
        'faq_api_keys': 'My API keys are not working',
        'faq_ollama': 'Ollama / Local models not working',
        'faq_streaming': 'Streaming issues',
        'faq_parallel': 'Parallel execution problems',
        'faq_memory': 'Compacted memory / context not working',
        'faq_pricing': 'Cost/pricing issues',
        'faq_tui': 'TUI mode problems',
        'faq_connection': 'Connection/network issues',

        # Validation messages
        'validating_api_key': 'Validating API key...',
        'api_key_valid': 'API key is valid',
        'api_key_invalid': 'API key is invalid or expired',
        'api_key_error': 'Error validating API key',
        'checking_ollama': 'Checking Ollama connection...',
        'ollama_connected': 'Ollama is running and accessible',
        'ollama_not_running': 'Ollama is not running or not accessible',

        # Settings shell (REPL /settings) — frame UI, not env var descriptions
        'lang_selection_title': 'Language selection',
        'lang_selection_subtitle': 'Choose your preferred language for the settings interface.',
        'faq_panel_subtitle': 'Get help with common issues and configuration problems.',
        'faq_action_validate_all': 'Validate all API keys',
        'faq_action_system_status': 'Check system status',
        'press_enter_continue': 'Press Enter to continue...',
        'validating_api_keys_panel': 'Validating API keys...',
        'api_key_validation_table_title': 'API key validation results',
        'table_col_api_key': 'API key',
        'table_col_status': 'Status',
        'table_col_message': 'Message',
        'status_valid': 'Valid',
        'status_invalid': 'Invalid',
        'status_not_set': 'Not set',
        'status_error': 'Error',
        'checking_system_status': 'Checking system status...',
        'network_ok': 'Network connectivity OK',
        'network_issues': 'Network issues: {message}',
        'available_models_label': 'Available models',
        'and_n_more': '... and {n} more',
        'ollama_status_prefix': 'Ollama: {message}',
        'api_keys_summary': 'API keys: {valid_count}/{set_count} valid, {not_cfg} not configured',
        'faq_content_unavailable': 'FAQ content not available.',
        'no_faq_for_topic': "No FAQ content available for '{topic}'.",
        'check_status_line': 'Status: {value}',
        'fix_prefix': 'Fix: {text}',
        'live_status_check': 'Live status check',
        'suggestions_header': 'Suggestions',
        'setup_steps_header': 'Setup steps',
        'current_configuration_header': 'Current configuration',
        'related_commands_header': 'Related commands',
        'environment_variables_header': 'Environment variables',
        'var_current_line': 'Current: {value}',
        'common_issues_header': 'Common issues',
        'label_status': 'Status',
        'value_set_masked': 'Set ({masked})',
        'configuration_cancelled': 'Configuration cancelled.',
        'mode_cli': 'CLI mode',
        'mode_tui': 'TUI mode',
        'settings_footer_hint': 'Press Ctrl+C to cancel | Variables: {n_vars} | Categories: {n_cats}',
        'change_language': 'Change language',
        'select_variable_from_category': "Select a variable from '{category}':",
        'add_new_api_key_choice': '[+ Add new API key]',
        'back_to_categories': '[Back to categories]',
        'no_vars_in_category': "No configurable variables found in '{category}'.",
        'what_to_do_with_var': 'What would you like to do with {var}?',
        'action_edit_value': 'Edit value',
        'action_delete_key': 'Delete API key',
        'add_new_api_key_title': 'Add new API key',
        'add_new_api_key_intro': (
            'Format: PROVIDER_API_KEY\n'
            'Examples: ANTHROPIC_API_KEY, GOOGLE_API_KEY, MISTRAL_API_KEY\n\n'
            'The name must be in UPPERCASE and end with _API_KEY'
        ),
        'enter_api_key_name': 'Enter the API key name:',
        'err_api_key_name_empty': 'API key name cannot be empty',
        'err_api_key_name_format': (
            'Invalid format. Use UPPERCASE and end with _API_KEY (e.g., ANTHROPIC_API_KEY)'
        ),
        'err_api_key_exists': "API key '{name}' already exists. Use the edit option to modify it.",
        'enter_api_key_value': 'Enter the value for {name}:',
        'err_api_key_value_empty': 'API key value cannot be empty',
        'api_key_added_body': 'Value: {masked}\n\nChanges are saved to .env and active in current session.',
        'api_key_added_title': 'API key {name} added successfully.',
        'variable_updated_title': 'Variable {name} updated successfully.',
        'variable_updated_body': 'New value: {value}\n\nChanges are saved to .env and active in current session.',
        'configure_another_in_category': "Configure another variable in '{category}'?",
        'delete_api_key_title': 'Delete API key',
        'delete_api_key_irreversible': 'This action cannot be undone.',
        'delete_api_key_confirm': 'Are you sure you want to delete {name}?',
        'api_key_deleted_title': 'API key {name} deleted successfully.',
        'api_key_deleted_body': 'Changes are saved to .env and removed from current session.',
        'failed_delete_api_key': 'Failed to delete API key.',
        'faq_module_unavailable': 'FAQ module not available. Please install required dependencies.',
        'generic_error': 'Error: {message}',
        'hint_not_set_suffix': ' (not set)',
        'hint_current_masked': ' (current: {masked})',
        'hint_current_plain': ' (current: {value})',
        'choice_value_not_set': '(not set)',
        'choice_value_masked': '{masked}',
        'tui_settings_title': 'CAI settings',
        'tui_interactive_unavailable': 'Interactive mode is not available in TUI.',
        'tui_use_commands_below': 'Use the commands below to configure settings.',
        'tui_quick_commands': 'Quick configuration commands',
        'tui_current_configuration': 'Current configuration',
        'tui_api_keys_status': 'API keys status',
        'tui_tip_env': 'Tip: use /env set NAME value to change any catalog setting',
        'tui_label_model': 'Model',
        'tui_label_agent_type': 'Agent type',
        'tui_label_debug': 'Debug level',
        'tui_label_stream': 'LLM streaming',
        'tui_label_tool_stream': 'Tool streaming',
        'tui_label_compacted_memory': 'Compacted memory',
        'tui_label_tracing': 'Tracing',
        'tui_value_set': 'Set',
        'tui_value_not_set': 'Not set',
        'tui_cmd_list': 'List all configurable variables',
        'tui_cmd_get': 'Show current value of a catalog variable',
        'tui_cmd_set': 'Set a catalog variable',
        'tui_cmd_model': 'Change the current model',
        'tui_cmd_settings_status': 'Show system status',
        'tui_cmd_settings_validate': 'Validate API keys',
        'prompt_type_to_search_accept': '(Type to search, or press Enter to accept current value)',
        'prompt_type_to_search_empty_default': '(Type to search, or leave empty to use default)',
        'prompt_enter_temperature': 'Enter temperature (0.0-2.0):',
        'prompt_enter_top_p': 'Enter top_p (0.0-1.0):',
        'prompt_enter_max_turns': "Enter maximum turns (number or 'inf'):",
        'prompt_enter_price_limit': 'Enter price limit in dollars (e.g., 2.5):',
        'prompt_enter_timeout_seconds': 'Enter timeout in seconds:',
        'prompt_enter_interval_turns': 'Enter interval (number of turns):',
        'prompt_enter_port': 'Enter port number (1-65535):',
        'prompt_enter_workers': 'Enter number of workers:',
        'prompt_enter_ollama_base': 'Enter Ollama API base URL:',
        'choice_custom_value': 'Custom value...',
        'choice_custom_amount': 'Custom amount...',
        'choice_custom_port': 'Custom port...',
        'choice_custom_url': 'Custom URL...',
        'err_model_not_found': "Model '{name}' not found.",
        'err_agent_not_found': (
            "Agent '{name}' not found. Please select from available agents or type to search."
        ),
        'validation_module_unavailable': 'Validation module not available.',
        'status_module_unavailable': 'Status module not available.',
        'unknown_settings_subcommand': 'Unknown subcommand: {name}',
        'available_subcommands_header': 'Available subcommands:',
        'ollama_sugg_serve': "Run 'ollama serve' in a terminal",
        'ollama_sugg_firewall': 'Check if port 11434 is blocked by a firewall',
        'ollama_sugg_api_base': 'Verify OLLAMA_API_BASE points to your Ollama server',
    },

    'es': {
        # Main UI
        'title': 'Configuración Interactiva de CAI',
        'subtitle': 'Configura todas las variables de entorno y soluciona problemas',
        'select_language': 'Selecciona tu idioma',
        'select_category': 'Selecciona una categoría para configurar',
        'select_variable': 'Selecciona una variable para configurar',
        'exit': 'Salir',
        'back': 'Volver',
        'save': 'Guardar',
        'cancel': 'Cancelar',
        'current_value': 'Valor actual',
        'not_set': 'No configurado',
        'success': 'Actualizado correctamente',
        'error': 'Error',
        'configure_more': '¿Configurar otra variable?',

        # Categories
        'cat_ctf': 'Variables CTF',
        'cat_core': 'Configuración Principal',
        'cat_api_keys': 'Claves API',
        'cat_streaming': 'Streaming y Salida',
        'cat_parallel': 'Paralelización',
        'cat_limits': 'Límites de Ejecución',
        'cat_memory': 'Memoria y Contexto',
        'cat_workspace': 'Espacio de Trabajo',
        'cat_support': 'Agente de Soporte',
        'cat_ctr': 'Configuración CTR',
        'cat_tracing': 'Trazado y Telemetría',
        'cat_security': 'Seguridad',
        'cat_pricing': 'Precio y Coste',
        'cat_reporting': 'Reportes',
        'cat_api_server': 'Servidor API',
        'cat_auth': 'Autenticación',
        'cat_mcp': 'Configuración MCP',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (Modelos Locales)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'Modo TUI',
        'cat_advanced': 'Avanzado',
        'cat_faq': 'FAQ y Solución de Problemas',

        # FAQ Section
        'faq_title': 'Preguntas Frecuentes y Solución de Problemas',
        'faq_select': 'Selecciona un tema para obtener ayuda',
        'faq_api_keys': 'Mis claves API no funcionan',
        'faq_ollama': 'Ollama / Modelos locales no funcionan',
        'faq_streaming': 'Problemas de streaming',
        'faq_parallel': 'Problemas de ejecución paralela',
        'faq_memory': 'Memoria compactada / contexto no funcionan',
        'faq_pricing': 'Problemas de coste/precio',
        'faq_tui': 'Problemas del modo TUI',
        'faq_connection': 'Problemas de conexión/red',

        # Validation messages
        'validating_api_key': 'Validando clave API...',
        'api_key_valid': 'La clave API es válida',
        'api_key_invalid': 'La clave API es inválida o ha expirado',
        'api_key_error': 'Error al validar la clave API',
        'checking_ollama': 'Comprobando conexión con Ollama...',
        'ollama_connected': 'Ollama está funcionando y accesible',
        'ollama_not_running': 'Ollama no está funcionando o no es accesible',

        'lang_selection_title': 'Selección de idioma',
        'lang_selection_subtitle': 'Elige el idioma de la interfaz de ajustes.',
        'faq_panel_subtitle': 'Ayuda para problemas habituales y de configuración.',
        'faq_action_validate_all': 'Validar todas las claves API',
        'faq_action_system_status': 'Comprobar estado del sistema',
        'press_enter_continue': 'Pulsa Enter para continuar...',
        'validating_api_keys_panel': 'Validando claves API...',
        'api_key_validation_table_title': 'Resultados de validación de claves API',
        'table_col_api_key': 'Clave API',
        'table_col_status': 'Estado',
        'table_col_message': 'Mensaje',
        'status_valid': 'Válida',
        'status_invalid': 'No válida',
        'status_not_set': 'No configurada',
        'status_error': 'Error',
        'checking_system_status': 'Comprobando estado del sistema...',
        'network_ok': 'Conectividad de red correcta',
        'network_issues': 'Problemas de red: {message}',
        'available_models_label': 'Modelos disponibles',
        'and_n_more': '... y {n} más',
        'ollama_status_prefix': 'Ollama: {message}',
        'api_keys_summary': 'Claves API: {valid_count}/{set_count} válidas, {not_cfg} sin configurar',
        'faq_content_unavailable': 'Contenido de FAQ no disponible.',
        'no_faq_for_topic': "No hay FAQ disponible para '{topic}'.",
        'check_status_line': 'Estado: {value}',
        'fix_prefix': 'Solución: {text}',
        'live_status_check': 'Comprobación de estado en vivo',
        'suggestions_header': 'Sugerencias',
        'setup_steps_header': 'Pasos de configuración',
        'current_configuration_header': 'Configuración actual',
        'related_commands_header': 'Comandos relacionados',
        'environment_variables_header': 'Variables de entorno',
        'var_current_line': 'Actual: {value}',
        'common_issues_header': 'Problemas frecuentes',
        'label_status': 'Estado',
        'value_set_masked': 'Configurada ({masked})',
        'configuration_cancelled': 'Configuración cancelada.',
        'mode_cli': 'Modo CLI',
        'mode_tui': 'Modo TUI',
        'settings_footer_hint': (
            'Pulsa Ctrl+C para cancelar | Variables: {n_vars} | Categorías: {n_cats}'
        ),
        'change_language': 'Cambiar idioma',
        'select_variable_from_category': "Selecciona una variable de '{category}':",
        'add_new_api_key_choice': '[+ Añadir nueva clave API]',
        'back_to_categories': '[Volver a categorías]',
        'no_vars_in_category': "No hay variables configurables en '{category}'.",
        'what_to_do_with_var': '¿Qué quieres hacer con {var}?',
        'action_edit_value': 'Editar valor',
        'action_delete_key': 'Eliminar clave API',
        'add_new_api_key_title': 'Añadir nueva clave API',
        'add_new_api_key_intro': (
            'Formato: PROVIDER_API_KEY\n'
            'Ejemplos: ANTHROPIC_API_KEY, GOOGLE_API_KEY, MISTRAL_API_KEY\n\n'
            'El nombre debe estar en MAYÚSCULAS y terminar en _API_KEY'
        ),
        'enter_api_key_name': 'Introduce el nombre de la clave API:',
        'err_api_key_name_empty': 'El nombre de la clave API no puede estar vacío',
        'err_api_key_name_format': (
            'Formato no válido. Usa MAYÚSCULAS y termina en _API_KEY (p. ej., ANTHROPIC_API_KEY)'
        ),
        'err_api_key_exists': (
            "La clave API '{name}' ya existe. Usa la opción de edición para modificarla."
        ),
        'enter_api_key_value': 'Introduce el valor de {name}:',
        'err_api_key_value_empty': 'El valor de la clave API no puede estar vacío',
        'api_key_added_body': (
            'Valor: {masked}\n\nLos cambios se guardan en .env y están activos en esta sesión.'
        ),
        'api_key_added_title': 'Clave API {name} añadida correctamente.',
        'variable_updated_title': 'Variable {name} actualizada correctamente.',
        'variable_updated_body': (
            'Nuevo valor: {value}\n\nLos cambios se guardan en .env y están activos en esta sesión.'
        ),
        'configure_another_in_category': "¿Configurar otra variable en '{category}'?",
        'delete_api_key_title': 'Eliminar clave API',
        'delete_api_key_irreversible': 'Esta acción no se puede deshacer.',
        'delete_api_key_confirm': '¿Seguro que quieres eliminar {name}?',
        'api_key_deleted_title': 'Clave API {name} eliminada correctamente.',
        'api_key_deleted_body': 'Los cambios se guardan en .env y se eliminan de esta sesión.',
        'failed_delete_api_key': 'No se pudo eliminar la clave API.',
        'faq_module_unavailable': 'Módulo FAQ no disponible. Instala las dependencias necesarias.',
        'generic_error': 'Error: {message}',
        'hint_not_set_suffix': ' (no configurado)',
        'hint_current_masked': ' (actual: {masked})',
        'hint_current_plain': ' (actual: {value})',
        'choice_value_not_set': '(no configurado)',
        'choice_value_masked': '{masked}',
        'tui_settings_title': 'Ajustes de CAI',
        'tui_interactive_unavailable': 'El modo interactivo no está disponible en TUI.',
        'tui_use_commands_below': 'Usa los comandos siguientes para configurar.',
        'tui_quick_commands': 'Comandos rápidos de configuración',
        'tui_current_configuration': 'Configuración actual',
        'tui_api_keys_status': 'Estado de claves API',
        'tui_tip_env': 'Consejo: usa /env set NOMBRE valor para cambiar cualquier ajuste del catálogo',
        'tui_label_model': 'Modelo',
        'tui_label_agent_type': 'Tipo de agente',
        'tui_label_debug': 'Nivel de depuración',
        'tui_label_stream': 'Streaming del LLM',
        'tui_label_tool_stream': 'Streaming de herramientas',
        'tui_label_compacted_memory': 'Memoria compactada',
        'tui_label_tracing': 'Trazado',
        'tui_value_set': 'Configurada',
        'tui_value_not_set': 'No configurada',
        'tui_cmd_list': 'Listar todas las variables configurables',
        'tui_cmd_get': 'Mostrar el valor actual de una variable del catálogo',
        'tui_cmd_set': 'Establecer una variable del catálogo',
        'tui_cmd_model': 'Cambiar el modelo actual',
        'tui_cmd_settings_status': 'Mostrar estado del sistema',
        'tui_cmd_settings_validate': 'Validar claves API',
        'prompt_type_to_search_accept': '(Escribe para buscar o Enter para aceptar el valor actual)',
        'prompt_type_to_search_empty_default': '(Escribe para buscar o deja vacío para el predeterminado)',
        'prompt_enter_temperature': 'Introduce temperatura (0.0-2.0):',
        'prompt_enter_top_p': 'Introduce top_p (0.0-1.0):',
        'prompt_enter_max_turns': "Introduce máximo de turnos (número o 'inf'):",
        'prompt_enter_price_limit': 'Introduce límite de precio en dólares (p. ej., 2.5):',
        'prompt_enter_timeout_seconds': 'Introduce tiempo de espera en segundos:',
        'prompt_enter_interval_turns': 'Introduce intervalo (número de turnos):',
        'prompt_enter_port': 'Introduce puerto (1-65535):',
        'prompt_enter_workers': 'Introduce número de workers:',
        'prompt_enter_ollama_base': 'Introduce URL base de la API de Ollama:',
        'choice_custom_value': 'Valor personalizado...',
        'choice_custom_amount': 'Importe personalizado...',
        'choice_custom_port': 'Puerto personalizado...',
        'choice_custom_url': 'URL personalizada...',
        'err_model_not_found': "Modelo '{name}' no encontrado.",
        'err_agent_not_found': (
            "Agente '{name}' no encontrado. Elige entre los agentes disponibles o busca."
        ),
        'validation_module_unavailable': 'Módulo de validación no disponible.',
        'status_module_unavailable': 'Módulo de estado no disponible.',
        'unknown_settings_subcommand': 'Subcomando desconocido: {name}',
        'available_subcommands_header': 'Subcomandos disponibles:',
        'ollama_sugg_serve': "Ejecuta 'ollama serve' en una terminal",
        'ollama_sugg_firewall': 'Comprueba si el puerto 11434 está bloqueado por un firewall',
        'ollama_sugg_api_base': 'Verifica que OLLAMA_API_BASE apunte a tu servidor Ollama',
    },

    'ru': {
        'title': 'Интерактивные настройки CAI',
        'subtitle': 'Настройте все переменные среды и устраните проблемы',
        'select_language': 'Выберите язык',
        'select_category': 'Выберите категорию для настройки',
        'select_variable': 'Выберите переменную для настройки',
        'exit': 'Выход',
        'back': 'Назад',
        'save': 'Сохранить',
        'cancel': 'Отмена',
        'current_value': 'Текущее значение',
        'not_set': 'Не задано',
        'success': 'Успешно обновлено',
        'error': 'Ошибка',
        'configure_more': 'Настроить другую переменную?',

        'cat_ctf': 'Переменные CTF',
        'cat_core': 'Основные настройки CAI',
        'cat_api_keys': 'API ключи',
        'cat_streaming': 'Потоковая передача и вывод',
        'cat_parallel': 'Параллелизация',
        'cat_limits': 'Ограничения выполнения',
        'cat_memory': 'Память и контекст',
        'cat_workspace': 'Рабочее пространство',
        'cat_support': 'Агент поддержки',
        'cat_ctr': 'Параметры CTR',
        'cat_tracing': 'Трассировка и телеметрия',
        'cat_security': 'Безопасность',
        'cat_pricing': 'Цены и затраты',
        'cat_reporting': 'Отчетность',
        'cat_api_server': 'API сервер',
        'cat_auth': 'Аутентификация',
        'cat_mcp': 'Параметры MCP',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (локальные модели)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'Режим TUI',
        'cat_advanced': 'Дополнительно',
        'cat_faq': 'FAQ и устранение неполадок',

        'faq_title': 'Часто задаваемые вопросы и устранение неполадок',
        'faq_select': 'Выберите тему для получения справки',
        'faq_api_keys': 'Мои API ключи не работают',
        'faq_ollama': 'Ollama / локальные модели не работают',
        'faq_streaming': 'Проблемы с потоковой передачей',
        'faq_parallel': 'Проблемы параллельного выполнения',
        'faq_memory': 'Сжатая память / контекст не работают',
        'faq_pricing': 'Проблемы со стоимостью/ценой',
        'faq_tui': 'Проблемы режима TUI',
        'faq_connection': 'Проблемы подключения/сети',

        'validating_api_key': 'Проверка API ключа...',
        'api_key_valid': 'API ключ действителен',
        'api_key_invalid': 'API ключ недействителен или истек',
        'api_key_error': 'Ошибка при проверке API ключа',
        'checking_ollama': 'Проверка соединения Ollama...',
        'ollama_connected': 'Ollama запущен и доступен',
        'ollama_not_running': 'Ollama не запущен или недоступен',
    },

    'zh': {
        # Main UI
        'title': 'CAI 交互式设置',
        'subtitle': '配置所有环境变量并排除故障',
        'select_language': '选择您的语言',
        'select_category': '选择要配置的类别',
        'select_variable': '选择要配置的变量',
        'exit': '退出',
        'back': '返回',
        'save': '保存',
        'cancel': '取消',
        'current_value': '当前值',
        'not_set': '未设置',
        'success': '更新成功',
        'error': '错误',
        'configure_more': '配置另一个变量？',

        # Categories
        'cat_ctf': 'CTF 变量',
        'cat_core': '核心设置',
        'cat_api_keys': 'API 密钥',
        'cat_streaming': '流式输出和处理',
        'cat_parallel': '并行执行',
        'cat_limits': '执行限制',
        'cat_memory': '内存与上下文',
        'cat_workspace': '工作区',
        'cat_support': '支持代理',
        'cat_ctr': 'CTR 设置',
        'cat_tracing': '追踪与遥测',
        'cat_security': '安全性',
        'cat_pricing': '价格与成本',
        'cat_reporting': '报告',
        'cat_api_server': 'API 服务器',
        'cat_auth': '认证',
        'cat_mcp': 'MCP 设置',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama（本地模型）',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUI 模式',
        'cat_advanced': '高级',
        'cat_faq': '常见问题与故障排除',

        # FAQ Section
        'faq_title': '常见问题和故障排除',
        'faq_select': '选择一个主题以获取帮助',
        'faq_api_keys': '我的 API 密钥不工作',
        'faq_ollama': 'Ollama / 本地模型不工作',
        'faq_streaming': '流式传输问题',
        'faq_parallel': '并行执行问题',
        'faq_memory': '压缩记忆 / 上下文不工作',
        'faq_pricing': '成本/价格问题',
        'faq_tui': 'TUI 模式问题',
        'faq_connection': '连接/网络问题',

        # Validation messages
        'validating_api_key': '正在验证 API 密钥...',
        'api_key_valid': 'API 密钥有效',
        'api_key_invalid': 'API 密钥无效或已过期',
        'api_key_error': '验证 API 密钥出错',
        'checking_ollama': '正在检查 Ollama 连接...',
        'ollama_connected': 'Ollama 正在运行且可访问',
        'ollama_not_running': 'Ollama 未运行或无法访问',
    },

    'hi': {
        # Main UI
        'title': 'CAI इंटरैक्टिव सेटिंग्स',
        'subtitle': 'सभी पर्यावरण चर कॉन्फ़िगर करें और समस्याओं का निवारण करें',
        'select_language': 'अपनी भाषा चुनें',
        'select_category': 'कॉन्फ़िगर करने के लिए एक श्रेणी चुनें',
        'select_variable': 'कॉन्फ़िगर करने के लिए एक चर चुनें',
        'exit': 'बाहर निकलें',
        'back': 'वापस',
        'save': 'सहेजें',
        'cancel': 'रद्द करें',
        'current_value': 'वर्तमान मान',
        'not_set': 'सेट नहीं',
        'success': 'सफलतापूर्वक अपडेट किया गया',
        'error': 'त्रुटि',
        'configure_more': 'क्या किसी अन्य चर को कॉन्फ़िगर करें?',

        # Categories
        'cat_ctf': 'CTF चर',
        'cat_core': 'मुख्य सेटिंग्स',
        'cat_api_keys': 'API कुंजियाँ',
        'cat_streaming': 'स्ट्रीमिंग और आउटपुट',
        'cat_parallel': 'समानांतरकरण',
        'cat_limits': 'निष्पादन सीमाएं',
        'cat_memory': 'मेमोरी और संदर्भ',
        'cat_workspace': 'कार्यक्षेत्र',
        'cat_support': 'सहायता एजेंट',
        'cat_ctr': 'CTR सेटिंग्स',
        'cat_tracing': 'ट्रेसिंग और टेलीमेट्री',
        'cat_security': 'सुरक्षा',
        'cat_pricing': 'मूल्य निर्धारण और लागत',
        'cat_reporting': 'रिपोर्टिंग',
        'cat_api_server': 'API सर्वर',
        'cat_auth': 'प्रमाणीकरण',
        'cat_mcp': 'MCP सेटिंग्स',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (स्थानीय मॉडल)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUI मोड',
        'cat_advanced': 'उन्नत',
        'cat_faq': 'FAQ और समस्या निवारण',

        # FAQ Section
        'faq_title': 'अक्सर पूछे जाने वाले प्रश्न और समस्या निवारण',
        'faq_select': 'सहायता के लिए एक विषय चुनें',
        'faq_api_keys': 'मेरी API कुंजियाँ काम नहीं कर रहीं',
        'faq_ollama': 'Ollama / स्थानीय मॉडल काम नहीं कर रहे हैं',
        'faq_streaming': 'स्ट्रीमिंग समस्याएं',
        'faq_parallel': 'समानांतर निष्पादन समस्याएं',
        'faq_memory': 'संक्षिप्त मेमोरी / संदर्भ काम नहीं कर रहा',
        'faq_pricing': 'लागत/मूल्य निर्धारण समस्याएं',
        'faq_tui': 'TUI मोड समस्याएं',
        'faq_connection': 'कनेक्शन/नेटवर्क समस्याएं',

        # Validation messages
        'validating_api_key': 'API कुंजी की जांच की जा रही है...',
        'api_key_valid': 'API कुंजी मान्य है',
        'api_key_invalid': 'API कुंजी अमान्य है या समाप्त हो गई है',
        'api_key_error': 'API कुंजी की जांच में त्रुटि',
        'checking_ollama': 'Ollama कनेक्शन की जांच की जा रही है...',
        'ollama_connected': 'Ollama चल रहा है और सुलभ है',
        'ollama_not_running': 'Ollama चल नहीं रहा है या सुलभ नहीं है',
    },

    'ja': {
        # Main UI
        'title': 'CAI インタラクティブ設定',
        'subtitle': 'すべての環境変数を設定し、問題をトラブルシューティングします',
        'select_language': '言語を選択してください',
        'select_category': '設定するカテゴリを選択してください',
        'select_variable': '設定する変数を選択してください',
        'exit': '終了',
        'back': '戻る',
        'save': '保存',
        'cancel': 'キャンセル',
        'current_value': '現在の値',
        'not_set': '未設定',
        'success': '正常に更新されました',
        'error': 'エラー',
        'configure_more': '別の変数を設定しますか?',

        # Categories
        'cat_ctf': 'CTF変数',
        'cat_core': 'コア設定',
        'cat_api_keys': 'APIキー',
        'cat_streaming': 'ストリーミング & 出力',
        'cat_parallel': '並列化',
        'cat_limits': '実行制限',
        'cat_memory': 'メモリ & コンテキスト',
        'cat_workspace': 'ワークスペース',
        'cat_support': 'サポートエージェント',
        'cat_ctr': 'CTR設定',
        'cat_tracing': 'トレーシング & テレメトリ',
        'cat_security': 'セキュリティ',
        'cat_pricing': '価格 & コスト',
        'cat_reporting': 'レポーティング',
        'cat_api_server': 'APIサーバー',
        'cat_auth': '認証',
        'cat_mcp': 'MCP設定',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (ローカルモデル)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUIモード',
        'cat_advanced': '詳細',
        'cat_faq': 'FAQ & トラブルシューティング',

        # FAQ Section
        'faq_title': 'よくある質問 & トラブルシューティング',
        'faq_select': 'ヘルプのためのトピックを選択してください',
        'faq_api_keys': 'APIキーが機能しない',
        'faq_ollama': 'Ollama / ローカルモデルが機能しない',
        'faq_streaming': 'ストリーミングの問題',
        'faq_parallel': '並列実行の問題',
        'faq_memory': '圧縮メモリ / コンテキストが機能しない',
        'faq_pricing': 'コスト/価格の問題',
        'faq_tui': 'TUIモードの問題',
        'faq_connection': '接続/ネットワークの問題',

        # Validation messages
        'validating_api_key': 'APIキーを検証中...',
        'api_key_valid': 'APIキーは有効です',
        'api_key_invalid': 'APIキーは無効であるか、期限が切れています',
        'api_key_error': 'APIキーの検証中にエラーが発生しました',
        'checking_ollama': 'Ollama接続を確認中...',
        'ollama_connected': 'Ollamaが実行中でアクセス可能です',
        'ollama_not_running': 'Ollamaが実行中でないか、アクセスできません',
    },

    'de': {
        # Main UI
        'title': 'CAI Interaktive Einstellungen',
        'subtitle': 'Konfigurieren Sie alle Umgebungsvariablen und beheben Sie Probleme',
        'select_language': 'Wählen Sie Ihre Sprache',
        'select_category': 'Wählen Sie eine Kategorie zum Konfigurieren',
        'select_variable': 'Wählen Sie eine Variable zum Konfigurieren',
        'exit': 'Beenden',
        'back': 'Zurück',
        'save': 'Speichern',
        'cancel': 'Abbrechen',
        'current_value': 'Aktueller Wert',
        'not_set': 'Nicht gesetzt',
        'success': 'Erfolgreich aktualisiert',
        'error': 'Fehler',
        'configure_more': 'Eine weitere Variable konfigurieren?',

        # Categories
        'cat_ctf': 'CTF-Variablen',
        'cat_core': 'CAI-Kerneinstellungen',
        'cat_api_keys': 'API-Schlüssel',
        'cat_streaming': 'Streaming & Ausgabe',
        'cat_parallel': 'Parallelisierung',
        'cat_limits': 'Ausführungslimits',
        'cat_memory': 'Speicher & Kontext',
        'cat_workspace': 'Arbeitsbereich',
        'cat_support': 'Support-Agent',
        'cat_ctr': 'CTR-Einstellungen',
        'cat_tracing': 'Verfolgung & Telemetrie',
        'cat_security': 'Sicherheit',
        'cat_pricing': 'Preisgestaltung & Kosten',
        'cat_reporting': 'Berichterstattung',
        'cat_api_server': 'API-Server',
        'cat_auth': 'Authentifizierung',
        'cat_mcp': 'MCP-Einstellungen',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (Lokale Modelle)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUI-Modus',
        'cat_advanced': 'Erweitert',
        'cat_faq': 'FAQ & Fehlerbehebung',

        # FAQ Section
        'faq_title': 'Häufig gestellte Fragen & Fehlerbehebung',
        'faq_select': 'Wählen Sie ein Thema für Hilfe',
        'faq_api_keys': 'Meine API-Schlüssel funktionieren nicht',
        'faq_ollama': 'Ollama / Lokale Modelle funktionieren nicht',
        'faq_streaming': 'Streaming-Probleme',
        'faq_parallel': 'Probleme bei paralleler Ausführung',
        'faq_memory': 'Kompakter Speicher / Kontext funktioniert nicht',
        'faq_pricing': 'Kosten-/Preissprobleme',
        'faq_tui': 'TUI-Modus-Probleme',
        'faq_connection': 'Verbindungs-/Netzwerkprobleme',

        # Validation messages
        'validating_api_key': 'API-Schlüssel wird validiert...',
        'api_key_valid': 'API-Schlüssel ist gültig',
        'api_key_invalid': 'API-Schlüssel ist ungültig oder abgelaufen',
        'api_key_error': 'Fehler beim Validieren des API-Schlüssels',
        'checking_ollama': 'Ollama-Verbindung wird überprüft...',
        'ollama_connected': 'Ollama läuft und ist erreichbar',
        'ollama_not_running': 'Ollama läuft nicht oder ist nicht erreichbar',
    },

    'pt': {
        # Main UI
        'title': 'Configurações Interativas do CAI',
        'subtitle': 'Configure todas as variáveis de ambiente e resolva problemas',
        'select_language': 'Selecione seu idioma',
        'select_category': 'Selecione uma categoria para configurar',
        'select_variable': 'Selecione uma variável para configurar',
        'exit': 'Sair',
        'back': 'Voltar',
        'save': 'Salvar',
        'cancel': 'Cancelar',
        'current_value': 'Valor atual',
        'not_set': 'Não configurado',
        'success': 'Atualizado com sucesso',
        'error': 'Erro',
        'configure_more': 'Configurar outra variável?',

        # Categories
        'cat_ctf': 'Variáveis CTF',
        'cat_core': 'Configurações Principais',
        'cat_api_keys': 'Chaves de API',
        'cat_streaming': 'Streaming e Saída',
        'cat_parallel': 'Paralelização',
        'cat_limits': 'Limites de Execução',
        'cat_memory': 'Memória e Contexto',
        'cat_workspace': 'Espaço de Trabalho',
        'cat_support': 'Agente de Suporte',
        'cat_ctr': 'Configurações CTR',
        'cat_tracing': 'Rastreamento e Telemetria',
        'cat_security': 'Segurança',
        'cat_pricing': 'Preços e Custos',
        'cat_reporting': 'Relatórios',
        'cat_api_server': 'Servidor de API',
        'cat_auth': 'Autenticação',
        'cat_mcp': 'Configurações MCP',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (Modelos Locais)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'Modo TUI',
        'cat_advanced': 'Avançado',
        'cat_faq': 'FAQ e Resolução de Problemas',

        # FAQ Section
        'faq_title': 'Perguntas Frequentes e Resolução de Problemas',
        'faq_select': 'Selecione um tópico para obter ajuda',
        'faq_api_keys': 'Minhas chaves de API não estão funcionando',
        'faq_ollama': 'Ollama / Modelos locais não estão funcionando',
        'faq_streaming': 'Problemas de streaming',
        'faq_parallel': 'Problemas de execução paralela',
        'faq_memory': 'Memória compactada / contexto não funciona',
        'faq_pricing': 'Problemas de custo/preço',
        'faq_tui': 'Problemas do modo TUI',
        'faq_connection': 'Problemas de conexão/rede',

        # Validation messages
        'validating_api_key': 'Validando chave de API...',
        'api_key_valid': 'Chave de API é válida',
        'api_key_invalid': 'Chave de API é inválida ou expirou',
        'api_key_error': 'Erro ao validar chave de API',
        'checking_ollama': 'Verificando conexão com Ollama...',
        'ollama_connected': 'Ollama está funcionando e acessível',
        'ollama_not_running': 'Ollama não está funcionando ou não é acessível',
    },

    'fr': {
        # Main UI
        'title': 'Paramètres Interactifs de CAI',
        'subtitle': 'Configurez toutes les variables d\'environnement et résolvez les problèmes',
        'select_language': 'Sélectionnez votre langue',
        'select_category': 'Sélectionnez une catégorie à configurer',
        'select_variable': 'Sélectionnez une variable à configurer',
        'exit': 'Quitter',
        'back': 'Retour',
        'save': 'Enregistrer',
        'cancel': 'Annuler',
        'current_value': 'Valeur actuelle',
        'not_set': 'Non défini',
        'success': 'Mise à jour réussie',
        'error': 'Erreur',
        'configure_more': 'Configurer une autre variable ?',

        # Categories
        'cat_ctf': 'Variables CTF',
        'cat_core': 'Paramètres Principaux de CAI',
        'cat_api_keys': 'Clés API',
        'cat_streaming': 'Diffusion en Continu et Sortie',
        'cat_parallel': 'Parallélisation',
        'cat_limits': 'Limites d\'Exécution',
        'cat_memory': 'Mémoire et Contexte',
        'cat_workspace': 'Espace de Travail',
        'cat_support': 'Agent de Support',
        'cat_ctr': 'Paramètres CTR',
        'cat_tracing': 'Traçage et Télémétrie',
        'cat_security': 'Sécurité',
        'cat_pricing': 'Tarification et Coûts',
        'cat_reporting': 'Rapports',
        'cat_api_server': 'Serveur API',
        'cat_auth': 'Authentification',
        'cat_mcp': 'Paramètres MCP',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (Modèles Locaux)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'Mode TUI',
        'cat_advanced': 'Avancé',
        'cat_faq': 'FAQ et Dépannage',

        # FAQ Section
        'faq_title': 'Questions Fréquemment Posées et Dépannage',
        'faq_select': 'Sélectionnez un sujet pour obtenir de l\'aide',
        'faq_api_keys': 'Mes clés API ne fonctionnent pas',
        'faq_ollama': 'Ollama / Modèles locaux ne fonctionnent pas',
        'faq_streaming': 'Problèmes de diffusion en continu',
        'faq_parallel': 'Problèmes d\'exécution parallèle',
        'faq_memory': 'Mémoire compactée / contexte ne fonctionne pas',
        'faq_pricing': 'Problèmes de coûts/tarification',
        'faq_tui': 'Problèmes du mode TUI',
        'faq_connection': 'Problèmes de connexion/réseau',

        # Validation messages
        'validating_api_key': 'Validation de la clé API...',
        'api_key_valid': 'La clé API est valide',
        'api_key_invalid': 'La clé API est invalide ou expirée',
        'api_key_error': 'Erreur lors de la validation de la clé API',
        'checking_ollama': 'Vérification de la connexion Ollama...',
        'ollama_connected': 'Ollama est en cours d\'exécution et accessible',
        'ollama_not_running': 'Ollama n\'est pas en cours d\'exécution ou n\'est pas accessible',
    },

    'ko': {
        # Main UI
        'title': 'CAI 대화형 설정',
        'subtitle': '모든 환경 변수를 구성하고 문제를 해결합니다',
        'select_language': '언어를 선택하세요',
        'select_category': '구성할 카테고리를 선택하세요',
        'select_variable': '구성할 변수를 선택하세요',
        'exit': '종료',
        'back': '돌아가기',
        'save': '저장',
        'cancel': '취소',
        'current_value': '현재 값',
        'not_set': '설정되지 않음',
        'success': '성공적으로 업데이트됨',
        'error': '오류',
        'configure_more': '다른 변수를 구성하시겠습니까?',

        # Categories
        'cat_ctf': 'CTF 변수',
        'cat_core': 'CAI 핵심 설정',
        'cat_api_keys': 'API 키',
        'cat_streaming': '스트리밍 및 출력',
        'cat_parallel': '병렬 처리',
        'cat_limits': '실행 제한',
        'cat_memory': '메모리 및 컨텍스트',
        'cat_workspace': '작업 공간',
        'cat_support': '지원 에이전트',
        'cat_ctr': 'CTR 설정',
        'cat_tracing': '추적 및 원격 측정',
        'cat_security': '보안',
        'cat_pricing': '가격 및 비용',
        'cat_reporting': '보고',
        'cat_api_server': 'API 서버',
        'cat_auth': '인증',
        'cat_mcp': 'MCP 설정',
        'cat_openrouter': 'OpenRouter',
        'cat_ollama': 'Ollama (로컬 모델)',
        'cat_litellm': 'LiteLLM',
        'cat_openai': 'OpenAI',
        'cat_google': 'Google',
        'cat_tui': 'TUI 모드',
        'cat_advanced': '고급',
        'cat_faq': 'FAQ 및 문제 해결',

        # FAQ Section
        'faq_title': '자주 묻는 질문 및 문제 해결',
        'faq_select': '도움을 위해 주제를 선택하세요',
        'faq_api_keys': 'API 키가 작동하지 않습니다',
        'faq_ollama': 'Ollama / 로컬 모델이 작동하지 않습니다',
        'faq_streaming': '스트리밍 문제',
        'faq_parallel': '병렬 실행 문제',
        'faq_memory': '압축 메모리 / 컨텍스트가 작동하지 않습니다',
        'faq_pricing': '비용/가격 문제',
        'faq_tui': 'TUI 모드 문제',
        'faq_connection': '연결/네트워크 문제',

        # Validation messages
        'validating_api_key': 'API 키 검증 중...',
        'api_key_valid': 'API 키가 유효합니다',
        'api_key_invalid': 'API 키가 유효하지 않거나 만료되었습니다',
        'api_key_error': 'API 키 검증 중 오류가 발생했습니다',
        'checking_ollama': 'Ollama 연결 확인 중...',
        'ollama_connected': 'Ollama가 실행 중이고 접근 가능합니다',
        'ollama_not_running': 'Ollama가 실행 중이지 않거나 접근 불가능합니다',
    },
}

# New UI keys are defined in full for ``en`` (and ``es``); other locales inherit via ``setdefault``.
_EN_UI_STRINGS = UI_STRINGS['en']
for _lang_code, _strings in UI_STRINGS.items():
    if _lang_code == 'en':
        continue
    for _key, _val in _EN_UI_STRINGS.items():
        _strings.setdefault(_key, _val)

# =============================================================================
# FAQ Content - Troubleshooting guides per topic
# =============================================================================
FAQ_CONTENT: Dict[str, Dict[str, Dict[str, Any]]] = {
    'en': {
        'api_keys': {
            'title': 'API Key Troubleshooting',
            'description': 'Common issues with API keys and how to fix them',
            'related_commands': [
                {'command': '/env', 'description': 'Configure environment variables including API keys'},
                {'command': '/settings validate', 'description': 'Validate API keys'},
                {'command': '/model', 'description': 'Change the current model (requires valid API key)'},
            ],
            'checks': [
                {
                    'name': 'OpenAI API Key',
                    'env_var': 'OPENAI_API_KEY',
                    'validation_url': 'https://api.openai.com/v1/models',
                    'common_issues': [
                        'Key starts with sk- but is expired',
                        'Key has insufficient quota/credits',
                        'Key is from wrong organization',
                        'Rate limits exceeded',
                    ],
                    'solutions': [
                        'Check your OpenAI dashboard at https://platform.openai.com',
                        'Verify billing is set up correctly',
                        'Check API key permissions',
                        'Wait for rate limit reset or upgrade plan',
                    ],
                },
                {
                    'name': 'Anthropic API Key',
                    'env_var': 'ANTHROPIC_API_KEY',
                    'validation_url': 'https://api.anthropic.com/v1/messages',
                    'common_issues': [
                        'Key format is incorrect',
                        'Key has expired',
                        'Account is suspended',
                    ],
                    'solutions': [
                        'Get new key from https://console.anthropic.com',
                        'Check account status',
                        'Verify billing information',
                    ],
                },
                {
                    'name': 'Alias Robotics API Key',
                    'env_var': 'ALIAS_API_KEY',
                    'validation_url': None,  # Custom validation
                    'common_issues': [
                        'Key not provided',
                        'Key is invalid',
                    ],
                    'solutions': [
                        'Contact Alias Robotics for a valid key',
                        'Check key format',
                    ],
                },
                {
                    'name': 'OpenRouter API Key',
                    'env_var': 'OPENROUTER_API_KEY',
                    'validation_url': 'https://openrouter.ai/api/v1/models',
                    'common_issues': [
                        'Key is invalid',
                        'Insufficient credits',
                    ],
                    'solutions': [
                        'Get key from https://openrouter.ai/keys',
                        'Add credits to your account',
                    ],
                },
            ],
        },

        'ollama': {
            'title': 'Ollama / Local Models Troubleshooting',
            'description': 'How to set up and troubleshoot Ollama for local model inference',
            'related_commands': [
                {'command': '/model ollama/llama3.2', 'description': 'Switch to Ollama model'},
                {'command': '/settings ollama', 'description': 'Ollama setup guide'},
                {'command': '/env set OLLAMA', 'description': 'Configure Ollama environment variables'},
            ],
            'steps': [
                {
                    'step': 1,
                    'title': 'Verify Ollama is installed',
                    'command': 'ollama --version',
                    'expected': 'Should show version number',
                    'fix': 'Install from https://ollama.com/download',
                },
                {
                    'step': 2,
                    'title': 'Start Ollama server',
                    'command': 'ollama serve',
                    'expected': 'Server starts on port 11434',
                    'fix': 'Run ollama serve in a terminal',
                    'note': 'Do NOT use "ollama run" - that only runs models in CLI',
                },
                {
                    'step': 3,
                    'title': 'Verify server is running',
                    'command': 'curl http://127.0.0.1:11434',
                    'expected': 'Should return "Ollama is running"',
                    'fix': 'Check if port 11434 is blocked by firewall',
                },
                {
                    'step': 4,
                    'title': 'Pull a model',
                    'command': 'ollama pull llama3.2',
                    'expected': 'Model downloads successfully',
                    'fix': 'Check disk space and internet connection',
                },
                {
                    'step': 5,
                    'title': 'Configure CAI',
                    'env_vars': {
                        'OLLAMA': 'true',
                        'OLLAMA_API_BASE': 'http://127.0.0.1:11434/v1',
                        'CAI_MODEL': 'ollama/llama3.2',
                    },
                },
            ],
            'docker_notes': {
                'title': 'Docker-specific configuration',
                'description': 'When running CAI in Docker, localhost refers to the container, not your host',
                'solutions': {
                    'Windows/macOS': 'Use host.docker.internal:11434',
                    'Linux': 'Use 172.17.0.1:11434 or your host IP',
                },
            },
            'common_models': [
                'ollama/llama3.2',
                'ollama/llama3.2:1b',
                'ollama/mistral',
                'ollama/codellama',
                'ollama/qwen2.5',
                'ollama/deepseek-coder-v2',
            ],
        },

        'streaming': {
            'title': 'Streaming Configuration',
            'description': 'Understanding CAI streaming options',
            'related_commands': [
                {'command': '/env set CAI_STREAM', 'description': 'Toggle LLM inference streaming'},
                {'command': '/env set CAI_TOOL_STREAM', 'description': 'Toggle tool output streaming'},
            ],
            'variables': {
                'CAI_STREAM': {
                    'description': 'Controls LLM inference streaming (token-by-token output)',
                    'values': {'true': 'See tokens as generated', 'false': 'Wait for complete response'},
                    'default': 'false',
                },
                'CAI_TOOL_STREAM': {
                    'description': 'Controls tool output streaming (real-time command output)',
                    'values': {'true': 'See output as it happens', 'false': 'Wait for command completion'},
                    'default': 'true',
                },
            },
            'note': 'These are independent - you can enable one without the other',
        },

        'parallel': {
            'title': 'Parallel Execution Troubleshooting',
            'description': 'Issues with running multiple agents in parallel',
            'related_commands': [
                {'command': '/parallel', 'description': 'Start parallel agent execution'},
                {'command': '/env set CAI_PARALLEL', 'description': 'Set number of parallel agents'},
                {'command': '/env set CAI_PARALLEL_AGENTS', 'description': 'Specify agent names for parallel'},
            ],
            'variables': {
                'CAI_PARALLEL': 'Number of parallel instances (1-20)',
                'CAI_PARALLEL_AGENTS': 'Comma-separated agent names',
                'CAI_AUTO_RUN_PARALLEL': 'Auto-start parallel agents',
            },
            'common_issues': [
                {
                    'issue': 'Agents sharing message history',
                    'cause': 'Instance isolation not working',
                    'fix': 'Each parallel agent should have unique instance ID',
                },
                {
                    'issue': 'Rate limiting errors',
                    'cause': 'Too many parallel requests to API',
                    'fix': 'Reduce CAI_PARALLEL or add delays',
                },
            ],
        },

        'memory': {
            'title': 'Compacted memory troubleshooting',
            'description': 'Session summaries from /compact injected into agent prompts',
            'related_commands': [
                {'command': '/memory status', 'description': 'Check saved compact summaries'},
                {'command': '/compact', 'description': 'Compact conversation into a summary'},
                {'command': '/env set CAI_COMPACTED_MEMORY', 'description': 'Enable (true) or disable (false) prompt injection'},
            ],
            'requirements': [],
            'variables': {
                'CAI_COMPACTED_MEMORY': 'true/false — inject /compact summaries into system prompts',
            },
            'setup_steps': [
                'Run /compact (or TUI compaction) to create a summary',
                'Set CAI_COMPACTED_MEMORY=true so new agents load the summary into their prompt',
            ],
        },

        'tui': {
            'title': 'TUI Mode Troubleshooting',
            'description': 'Terminal UI mode issues',
            'related_commands': [
                {'command': 'cai --tui', 'description': 'Start CAI in TUI mode'},
                {'command': '/env set CAI_TUI_MODE', 'description': 'Enable/disable TUI mode'},
                {'command': '/help', 'description': 'Show all available commands'},
            ],
            'requirements': [
                'Terminal with Unicode support',
                'Minimum 80x24 terminal size',
            ],
            'variables': {
                'CAI_TUI_MODE': 'Enable TUI mode',
                'CAI_TUI_MAX_LINES': 'Maximum output lines',
                'CAI_TUI_MAX_RERENDERS_PER_SEC': 'Rendering performance',
            },
            'cli_only_vars': [
                'CAI_API_HOST', 'CAI_API_PORT', 'CAI_API_WORKERS',
                'These variables only apply to CLI mode',
            ],
            'tui_only_vars': [
                'CAI_TUI_MODE', 'CAI_TUI_STARTUP_YAML', 'CAI_TUI_SHARED_PROMPT',
                'CAI_TUI_MAX_LINES', 'CAI_TUI_MAX_RERENDERS_PER_SEC',
            ],
        },

        'connection': {
            'title': 'Connection/Network Issues',
            'description': 'Troubleshooting network connectivity',
            'related_commands': [
                {'command': '/settings status', 'description': 'System status'},
                {'command': '/settings validate', 'description': 'Validate API keys'},
                {'command': '/env set CAI_SKIP_NETWORK_CHECK', 'description': 'Skip network checks (not recommended)'},
            ],
            'checks': [
                {
                    'check': 'Internet connectivity',
                    'command': 'curl -I https://api.openai.com',
                    'fix': 'Check firewall/proxy settings',
                },
                {
                    'check': 'DNS resolution',
                    'command': 'nslookup api.openai.com',
                    'fix': 'Try using 8.8.8.8 as DNS',
                },
                {
                    'check': 'Proxy configuration',
                    'env_vars': ['HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY'],
                    'fix': 'Set appropriate proxy environment variables',
                },
            ],
            'skip_check': {
                'variable': 'CAI_SKIP_NETWORK_CHECK',
                'description': 'Skip network availability checks (not recommended)',
            },
        },
    },

    'es': {
        'api_keys': {
            'title': 'Solución de problemas de claves API',
            'description': 'Problemas comunes con claves API y cómo solucionarlos',
            'related_commands': [
                {'command': '/env', 'description': 'Variables de entorno y claves API'},
                {'command': '/settings validate', 'description': 'Validar claves API'},
                {'command': '/model', 'description': 'Cambiar el modelo actual (requiere clave válida)'},
            ],
            'checks': [
                {
                    'name': 'Clave API de OpenAI',
                    'env_var': 'OPENAI_API_KEY',
                    'common_issues': [
                        'La clave comienza con sk- pero ha expirado',
                        'La clave no tiene suficiente cuota/créditos',
                        'La clave es de otra organización',
                    ],
                    'solutions': [
                        'Revisa tu panel de OpenAI en https://platform.openai.com',
                        'Verifica que la facturación esté configurada correctamente',
                        'Revisa los permisos de la clave API',
                    ],
                },
                {
                    'name': 'Clave API de Alias Robotics',
                    'env_var': 'ALIAS_API_KEY',
                    'validation_url': None,
                    'common_issues': [
                        'Clave no proporcionada',
                        'Clave no válida',
                    ],
                    'solutions': [
                        'Contacta con Alias Robotics para obtener una clave válida',
                        'Comprueba el formato de la clave',
                    ],
                },
            ],
        },
        'ollama': {
            'title': 'Solución de problemas de Ollama / Modelos Locales',
            'description': 'Cómo configurar y solucionar problemas de Ollama para inferencia local',
            'related_commands': [
                {'command': '/model ollama/llama3.2', 'description': 'Cambiar a un modelo Ollama'},
                {'command': '/settings ollama', 'description': 'Guía de configuración de Ollama'},
                {'command': '/env set OLLAMA', 'description': 'Variables de entorno de Ollama'},
            ],
            'steps': [
                {
                    'step': 1,
                    'title': 'Verificar que Ollama está instalado',
                    'command': 'ollama --version',
                    'fix': 'Instalar desde https://ollama.com/download',
                },
                {
                    'step': 2,
                    'title': 'Iniciar servidor Ollama',
                    'command': 'ollama serve',
                    'note': 'NO uses "ollama run" - eso solo ejecuta modelos en CLI',
                },
            ],
        },
    },

    'de': {
        'api_keys': {
            'title': 'API-Schlüssel-Fehlerbehebung',
            'description': 'Häufige Probleme mit API-Schlüsseln und deren Lösungen',
            'related_commands': [
                {'command': '/env', 'description': 'Umgebungsvariablen und API-Schlüssel'},
                {'command': '/settings validate', 'description': 'API-Schlüssel prüfen'},
                {'command': '/model', 'description': 'Aktuelles Modell wechseln (gültiger API-Schlüssel nötig)'},
            ],
            'checks': [
                {
                    'name': 'OpenAI API-Schlüssel',
                    'env_var': 'OPENAI_API_KEY',
                    'validation_url': 'https://api.openai.com/v1/models',
                    'common_issues': [
                        'Schlüssel beginnt mit sk-, ist aber abgelaufen',
                        'Schlüssel hat unzureichendes Guthaben/Kredite',
                        'Schlüssel stammt aus falscher Organisation',
                        'Rate Limits überschritten',
                    ],
                    'solutions': [
                        'Überprüfen Sie Ihr OpenAI-Dashboard unter https://platform.openai.com',
                        'Überprüfen Sie, dass die Abrechnung korrekt eingerichtet ist',
                        'Überprüfen Sie die Berechtigungen des API-Schlüssels',
                        'Warten Sie auf Rate-Limit-Zurücksetzen oder aktualisieren Sie Ihren Plan',
                    ],
                },
                {
                    'name': 'Anthropic API-Schlüssel',
                    'env_var': 'ANTHROPIC_API_KEY',
                    'validation_url': 'https://api.anthropic.com/v1/messages',
                    'common_issues': [
                        'Schlüsselformat ist falsch',
                        'Schlüssel ist abgelaufen',
                        'Konto ist gesperrt',
                    ],
                    'solutions': [
                        'Neue Schlüssel von https://console.anthropic.com abrufen',
                        'Kontostatus überprüfen',
                        'Abrechnungsinformationen überprüfen',
                    ],
                },
                {
                    'name': 'Alias Robotics API-Schlüssel',
                    'env_var': 'ALIAS_API_KEY',
                    'validation_url': None,
                    'common_issues': [
                        'Schlüssel nicht bereitgestellt',
                        'Schlüssel ist ungültig',
                    ],
                    'solutions': [
                        'Kontaktieren Sie Alias Robotics für einen gültigen Schlüssel',
                        'Überprüfen Sie das Schlüsselformat',
                    ],
                },
                {
                    'name': 'OpenRouter API-Schlüssel',
                    'env_var': 'OPENROUTER_API_KEY',
                    'validation_url': 'https://openrouter.ai/api/v1/models',
                    'common_issues': [
                        'Schlüssel ist ungültig',
                        'Unzureichende Guthaben',
                    ],
                    'solutions': [
                        'Schlüssel von https://openrouter.ai/keys abrufen',
                        'Guthaben zu Ihrem Konto hinzufügen',
                    ],
                },
            ],
        },

        'ollama': {
            'title': 'Ollama / Lokale Modelle Fehlerbehebung',
            'description': 'Wie Sie Ollama für lokale Modellableitung einrichten und Fehlerbehebung durchführen',
            'related_commands': [
                {'command': '/model ollama/llama3.2', 'description': 'Zu Ollama-Modell wechseln'},
                {'command': '/settings ollama', 'description': 'Ollama-Einrichtungsanleitung'},
                {'command': '/env set OLLAMA', 'description': 'Ollama-Umgebungsvariablen setzen'},
            ],
            'steps': [
                {
                    'step': 1,
                    'title': 'Überprüfen Sie, dass Ollama installiert ist',
                    'command': 'ollama --version',
                    'expected': 'Sollte die Versionsnummer anzeigen',
                    'fix': 'Installieren Sie von https://ollama.com/download',
                },
                {
                    'step': 2,
                    'title': 'Ollama-Server starten',
                    'command': 'ollama serve',
                    'expected': 'Server startet auf Port 11434',
                    'fix': 'Führen Sie ollama serve in einem Terminal aus',
                    'note': 'Verwenden Sie NICHT "ollama run" - das führt nur Modelle in CLI aus',
                },
                {
                    'step': 3,
                    'title': 'Überprüfen Sie, dass der Server läuft',
                    'command': 'curl http://127.0.0.1:11434',
                    'expected': 'Sollte "Ollama is running" zurückgeben',
                    'fix': 'Überprüfen Sie, ob Port 11434 durch die Firewall blockiert wird',
                },
                {
                    'step': 4,
                    'title': 'Ein Modell abrufen',
                    'command': 'ollama pull llama3.2',
                    'expected': 'Modell wird erfolgreich heruntergeladen',
                    'fix': 'Überprüfen Sie den Festplattenspeicher und die Internetverbindung',
                },
                {
                    'step': 5,
                    'title': 'Konfigurieren Sie CAI',
                    'env_vars': {
                        'OLLAMA': 'true',
                        'OLLAMA_API_BASE': 'http://127.0.0.1:11434/v1',
                        'CAI_MODEL': 'ollama/llama3.2',
                    },
                },
            ],
            'docker_notes': {
                'title': 'Docker-spezifische Konfiguration',
                'description': 'Wenn Sie CAI in Docker ausführen, bezieht sich Localhost auf den Container, nicht auf Ihren Host',
                'solutions': {
                    'Windows/macOS': 'Verwenden Sie host.docker.internal:11434',
                    'Linux': 'Verwenden Sie 172.17.0.1:11434 oder Ihre Host-IP',
                },
            },
            'common_models': [
                'ollama/llama3.2',
                'ollama/llama3.2:1b',
                'ollama/mistral',
                'ollama/codellama',
                'ollama/qwen2.5',
                'ollama/deepseek-coder-v2',
            ],
        },

        'streaming': {
            'title': 'Streaming-Konfiguration',
            'description': 'Verständnis der CAI-Streaming-Optionen',
            'related_commands': [
                {'command': '/env set CAI_STREAM', 'description': 'LLM-Streaming umschalten'},
                {'command': '/env set CAI_TOOL_STREAM', 'description': 'Tool-Ausgabe-Streaming umschalten'},
            ],
            'variables': {
                'CAI_STREAM': {
                    'description': 'Steuert LLM-Inferenz-Streaming (Token-für-Token-Ausgabe)',
                    'values': {'true': 'Sehen Sie Token bei der Generierung', 'false': 'Warten Sie auf vollständige Antwort'},
                    'default': 'false',
                },
                'CAI_TOOL_STREAM': {
                    'description': 'Steuert Werkzeugausgabe-Streaming (Echtzeit-Befehlsausgabe)',
                    'values': {'true': 'Sehen Sie die Ausgabe in Echtzeit', 'false': 'Warten Sie auf Befehlsvollendung'},
                    'default': 'true',
                },
            },
            'note': 'Diese sind unabhängig - Sie können eine aktivieren, ohne die andere zu aktivieren',
        },

        'parallel': {
            'title': 'Fehlerbehebung bei paralleler Ausführung',
            'description': 'Probleme beim parallelen Ausführen mehrerer Agenten',
            'related_commands': [
                {'command': '/parallel', 'description': 'Parallele Agenten starten'},
                {'command': '/env set CAI_PARALLEL', 'description': 'Anzahl paralleler Agenten setzen'},
                {'command': '/env set CAI_PARALLEL_AGENTS', 'description': 'Agentennamen für Parallelität setzen'},
            ],
            'variables': {
                'CAI_PARALLEL': 'Anzahl der parallelen Instanzen (1-20)',
                'CAI_PARALLEL_AGENTS': 'Kommagetrennte Agentenamen',
                'CAI_AUTO_RUN_PARALLEL': 'Auto-Start parallele Agenten',
            },
            'common_issues': [
                {
                    'issue': 'Agenten teilen Nachrichtenverlauf',
                    'cause': 'Instanzisolation funktioniert nicht',
                    'fix': 'Jeder parallele Agent sollte eine eindeutige Instanz-ID haben',
                },
                {
                    'issue': 'Rate-Limiting-Fehler',
                    'cause': 'Zu viele parallele Anfragen an API',
                    'fix': 'Reduzieren Sie CAI_PARALLEL oder fügen Sie Verzögerungen hinzu',
                },
            ],
        },

        'memory': {
            'title': 'Fehlerbehebung: kompaktierte Sitzungsspeicher',
            'description': 'Zusammenfassungen aus /compact werden in Agenten-Prompts eingefügt',
            'related_commands': [
                {'command': '/memory status', 'description': 'Gespeicherte Kompakt-Zusammenfassungen prüfen'},
                {'command': '/compact', 'description': 'Konversation kompakt zusammenfassen'},
                {'command': '/env set CAI_COMPACTED_MEMORY', 'description': 'Prompt-Einblendung aktivieren (true) oder deaktivieren (false)'},
            ],
            'requirements': [],
            'variables': {
                'CAI_COMPACTED_MEMORY': 'true/false — /compact-Zusammenfassungen in System-Prompts einfügen',
            },
            'setup_steps': [
                '/compact ausführen (oder TUI-Kompaktierung), um eine Zusammenfassung zu erzeugen',
                'CAI_COMPACTED_MEMORY=true setzen, damit neue Agenten die Zusammenfassung laden',
            ],
        },

        'tui': {
            'title': 'Fehlerbehebung im TUI-Modus',
            'description': 'Probleme im Terminal-UI-Modus',
            'related_commands': [
                {'command': 'cai --tui', 'description': 'CAI im TUI-Modus starten'},
                {'command': '/env set CAI_TUI_MODE', 'description': 'TUI-Modus aktivieren oder deaktivieren'},
                {'command': '/help', 'description': 'Alle Befehle anzeigen'},
            ],
            'requirements': [
                'Terminal mit Unicode-Unterstützung',
                'Mindestgröße 80x24 Terminal',
            ],
            'variables': {
                'CAI_TUI_MODE': 'TUI-Modus aktivieren',
                'CAI_TUI_MAX_LINES': 'Maximale Ausgabezeilen',
                'CAI_TUI_MAX_RERENDERS_PER_SEC': 'Rendering-Leistung',
            },
            'cli_only_vars': [
                'CAI_API_HOST', 'CAI_API_PORT', 'CAI_API_WORKERS',
                'Diese Variablen gelten nur für CLI-Modus',
            ],
            'tui_only_vars': [
                'CAI_TUI_MODE', 'CAI_TUI_STARTUP_YAML', 'CAI_TUI_SHARED_PROMPT',
                'CAI_TUI_MAX_LINES', 'CAI_TUI_MAX_RERENDERS_PER_SEC',
            ],
        },

        'connection': {
            'title': 'Verbindungs-/Netzwerkprobleme',
            'description': 'Fehlerbehebung bei Netzwerkverbindungen',
            'related_commands': [
                {'command': '/settings status', 'description': 'Systemstatus'},
                {'command': '/settings validate', 'description': 'API-Schlüssel prüfen'},
                {'command': '/env set CAI_SKIP_NETWORK_CHECK', 'description': 'Netzwerkprüfungen überspringen (nicht empfohlen)'},
            ],
            'checks': [
                {
                    'check': 'Internetverbindung',
                    'command': 'curl -I https://api.openai.com',
                    'fix': 'Überprüfen Sie Firewall-/Proxy-Einstellungen',
                },
                {
                    'check': 'DNS-Auflösung',
                    'command': 'nslookup api.openai.com',
                    'fix': 'Versuchen Sie, 8.8.8.8 als DNS zu verwenden',
                },
                {
                    'check': 'Proxy-Konfiguration',
                    'env_vars': ['HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY'],
                    'fix': 'Setzen Sie die entsprechenden Proxy-Umgebungsvariablen',
                },
            ],
            'skip_check': {
                'variable': 'CAI_SKIP_NETWORK_CHECK',
                'description': 'Überspringen Sie Netzwerkverfügbarkeitsprüfungen (nicht empfohlen)',
            },
        },
    },

    'fr': {
        'api_keys': {
            'title': 'Dépannage des Clés API',
            'description': 'Problèmes courants avec les clés API et comment les résoudre',
            'related_commands': [
                {'command': '/env', 'description': 'Variables d\'environnement et clés API'},
                {'command': '/settings validate', 'description': 'Valider les clés API'},
                {'command': '/model', 'description': 'Changer le modèle actuel (clé API valide requise)'},
            ],
            'checks': [
                {
                    'name': 'Clé API OpenAI',
                    'env_var': 'OPENAI_API_KEY',
                    'validation_url': 'https://api.openai.com/v1/models',
                    'common_issues': [
                        'La clé commence par sk- mais est expirée',
                        'La clé n\'a pas assez de quota/crédits',
                        'La clé provient d\'une mauvaise organisation',
                        'Les limites de débit ont été dépassées',
                    ],
                    'solutions': [
                        'Vérifiez votre tableau de bord OpenAI sur https://platform.openai.com',
                        'Vérifiez que la facturation est correctement configurée',
                        'Vérifiez les permissions de la clé API',
                        'Attendez la réinitialisation du débit ou améliorez votre forfait',
                    ],
                },
                {
                    'name': 'Clé API Anthropic',
                    'env_var': 'ANTHROPIC_API_KEY',
                    'validation_url': 'https://api.anthropic.com/v1/messages',
                    'common_issues': [
                        'Le format de la clé est incorrect',
                        'La clé a expiré',
                        'Le compte est suspendu',
                    ],
                    'solutions': [
                        'Obtenez une nouvelle clé sur https://console.anthropic.com',
                        'Vérifiez l\'état du compte',
                        'Vérifiez les informations de facturation',
                    ],
                },
                {
                    'name': 'Clé API Alias Robotics',
                    'env_var': 'ALIAS_API_KEY',
                    'validation_url': None,
                    'common_issues': [
                        'Clé non fournie',
                        'La clé est invalide',
                    ],
                    'solutions': [
                        'Contactez Alias Robotics pour une clé valide',
                        'Vérifiez le format de la clé',
                    ],
                },
                {
                    'name': 'Clé API OpenRouter',
                    'env_var': 'OPENROUTER_API_KEY',
                    'validation_url': 'https://openrouter.ai/api/v1/models',
                    'common_issues': [
                        'La clé est invalide',
                        'Crédits insuffisants',
                    ],
                    'solutions': [
                        'Obtenez une clé sur https://openrouter.ai/keys',
                        'Ajoutez des crédits à votre compte',
                    ],
                },
            ],
        },

        'ollama': {
            'title': 'Dépannage d\'Ollama / Modèles Locaux',
            'description': 'Comment configurer et dépanner Ollama pour l\'inférence de modèles locaux',
            'related_commands': [
                {'command': '/model ollama/llama3.2', 'description': 'Passer à un modèle Ollama'},
                {'command': '/settings ollama', 'description': 'Guide de configuration Ollama'},
                {'command': '/env set OLLAMA', 'description': 'Variables d\'environnement Ollama'},
            ],
            'steps': [
                {
                    'step': 1,
                    'title': 'Vérifiez qu\'Ollama est installé',
                    'command': 'ollama --version',
                    'expected': 'Devrait afficher le numéro de version',
                    'fix': 'Installez depuis https://ollama.com/download',
                },
                {
                    'step': 2,
                    'title': 'Démarrez le serveur Ollama',
                    'command': 'ollama serve',
                    'expected': 'Le serveur démarre sur le port 11434',
                    'fix': 'Exécutez ollama serve dans un terminal',
                    'note': 'Ne PAS utiliser "ollama run" - cela ne fait que exécuter des modèles en CLI',
                },
                {
                    'step': 3,
                    'title': 'Vérifiez que le serveur est en cours d\'exécution',
                    'command': 'curl http://127.0.0.1:11434',
                    'expected': 'Devrait retourner "Ollama is running"',
                    'fix': 'Vérifiez si le port 11434 est bloqué par le pare-feu',
                },
                {
                    'step': 4,
                    'title': 'Téléchargez un modèle',
                    'command': 'ollama pull llama3.2',
                    'expected': 'Le modèle se télécharge avec succès',
                    'fix': 'Vérifiez l\'espace disque et la connexion Internet',
                },
                {
                    'step': 5,
                    'title': 'Configurez CAI',
                    'env_vars': {
                        'OLLAMA': 'true',
                        'OLLAMA_API_BASE': 'http://127.0.0.1:11434/v1',
                        'CAI_MODEL': 'ollama/llama3.2',
                    },
                },
            ],
            'docker_notes': {
                'title': 'Configuration spécifique à Docker',
                'description': 'Lors de l\'exécution de CAI dans Docker, localhost fait référence au conteneur, pas à votre hôte',
                'solutions': {
                    'Windows/macOS': 'Utilisez host.docker.internal:11434',
                    'Linux': 'Utilisez 172.17.0.1:11434 ou votre IP hôte',
                },
            },
            'common_models': [
                'ollama/llama3.2',
                'ollama/llama3.2:1b',
                'ollama/mistral',
                'ollama/codellama',
                'ollama/qwen2.5',
                'ollama/deepseek-coder-v2',
            ],
        },

        'streaming': {
            'title': 'Configuration de la Diffusion en Continu',
            'description': 'Comprendre les options de diffusion en continu de CAI',
            'related_commands': [
                {'command': '/env set CAI_STREAM', 'description': 'Activer ou désactiver le streaming LLM'},
                {'command': '/env set CAI_TOOL_STREAM', 'description': 'Activer ou désactiver le streaming des outils'},
            ],
            'variables': {
                'CAI_STREAM': {
                    'description': 'Contrôle la diffusion en continu de l\'inférence LLM (sortie token par token)',
                    'values': {'true': 'Voir les tokens au fur et à mesure qu\'ils sont générés', 'false': 'Attendre la réponse complète'},
                    'default': 'false',
                },
                'CAI_TOOL_STREAM': {
                    'description': 'Contrôle la diffusion en continu de la sortie d\'outil (sortie de commande en temps réel)',
                    'values': {'true': 'Voir la sortie au fur et à mesure qu\'elle se produit', 'false': 'Attendre la fin de la commande'},
                    'default': 'true',
                },
            },
            'note': 'Ces options sont indépendantes - vous pouvez activer l\'une sans l\'autre',
        },

        'parallel': {
            'title': 'Dépannage de l\'Exécution Parallèle',
            'description': 'Problèmes lors de l\'exécution de plusieurs agents en parallèle',
            'related_commands': [
                {'command': '/parallel', 'description': 'Lancer l\'exécution parallèle d\'agents'},
                {'command': '/env set CAI_PARALLEL', 'description': 'Définir le nombre d\'agents parallèles'},
                {'command': '/env set CAI_PARALLEL_AGENTS', 'description': 'Noms d\'agents pour le parallèle'},
            ],
            'variables': {
                'CAI_PARALLEL': 'Nombre d\'instances parallèles (1-20)',
                'CAI_PARALLEL_AGENTS': 'Noms d\'agents séparés par des virgules',
                'CAI_AUTO_RUN_PARALLEL': 'Démarrage automatique des agents parallèles',
            },
            'common_issues': [
                {
                    'issue': 'Agents partageant l\'historique des messages',
                    'cause': 'L\'isolation des instances ne fonctionne pas',
                    'fix': 'Chaque agent parallèle doit avoir un ID d\'instance unique',
                },
                {
                    'issue': 'Erreurs de limitation du débit',
                    'cause': 'Trop de demandes parallèles à l\'API',
                    'fix': 'Réduisez CAI_PARALLEL ou ajoutez des délais',
                },
            ],
        },

        'memory': {
            'title': 'Dépannage mémoire compactée',
            'description': 'Résumés de session issus de /compact injectés dans les prompts',
            'related_commands': [
                {'command': '/memory status', 'description': 'État des résumés compactés enregistrés'},
                {'command': '/compact', 'description': 'Résumer la conversation'},
                {'command': '/env set CAI_COMPACTED_MEMORY', 'description': 'Activer (true) ou désactiver (false) l\'injection dans le prompt'},
            ],
            'requirements': [],
            'variables': {
                'CAI_COMPACTED_MEMORY': 'true/false — injecter les résumés /compact dans le prompt système',
            },
            'setup_steps': [
                'Exécuter /compact (ou compaction TUI) pour créer un résumé',
                'Définir CAI_COMPACTED_MEMORY=true pour que les nouveaux agents chargent le résumé',
            ],
        },

        'tui': {
            'title': 'Dépannage du Mode TUI',
            'description': 'Problèmes du mode interface utilisateur terminal',
            'related_commands': [
                {'command': 'cai --tui', 'description': 'Démarrer CAI en mode TUI'},
                {'command': '/env set CAI_TUI_MODE', 'description': 'Activer ou désactiver le mode TUI'},
                {'command': '/help', 'description': 'Afficher toutes les commandes'},
            ],
            'requirements': [
                'Terminal avec support Unicode',
                'Taille de terminal minimale 80x24',
            ],
            'variables': {
                'CAI_TUI_MODE': 'Activer le mode TUI',
                'CAI_TUI_MAX_LINES': 'Lignes de sortie maximales',
                'CAI_TUI_MAX_RERENDERS_PER_SEC': 'Performance de rendu',
            },
            'cli_only_vars': [
                'CAI_API_HOST', 'CAI_API_PORT', 'CAI_API_WORKERS',
                'Ces variables s\'appliquent uniquement au mode CLI',
            ],
            'tui_only_vars': [
                'CAI_TUI_MODE', 'CAI_TUI_STARTUP_YAML', 'CAI_TUI_SHARED_PROMPT',
                'CAI_TUI_MAX_LINES', 'CAI_TUI_MAX_RERENDERS_PER_SEC',
            ],
        },

        'connection': {
            'title': 'Problèmes de Connexion/Réseau',
            'description': 'Dépannage de la connectivité réseau',
            'related_commands': [
                {'command': '/settings status', 'description': 'État du système'},
                {'command': '/settings validate', 'description': 'Valider les clés API'},
                {'command': '/env set CAI_SKIP_NETWORK_CHECK', 'description': 'Ignorer les vérifications réseau (non recommandé)'},
            ],
            'checks': [
                {
                    'check': 'Connectivité Internet',
                    'command': 'curl -I https://api.openai.com',
                    'fix': 'Vérifiez les paramètres de pare-feu/proxy',
                },
                {
                    'check': 'Résolution DNS',
                    'command': 'nslookup api.openai.com',
                    'fix': 'Essayez d\'utiliser 8.8.8.8 comme DNS',
                },
                {
                    'check': 'Configuration du proxy',
                    'env_vars': ['HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY'],
                    'fix': 'Définissez les variables d\'environnement proxy appropriées',
                },
            ],
            'skip_check': {
                'variable': 'CAI_SKIP_NETWORK_CHECK',
                'description': 'Ignorer les vérifications de disponibilité du réseau (non recommandé)',
            },
        },
    },

    'ko': {
        'api_keys': {
            'title': 'API 키 문제 해결',
            'description': 'API 키의 일반적인 문제 및 해결 방법',
            'related_commands': [
                {'command': '/env', 'description': '환경 변수 및 API 키'},
                {'command': '/settings validate', 'description': 'API 키 검증'},
                {'command': '/model', 'description': '현재 모델 변경(유효한 API 키 필요)'},
            ],
            'checks': [
                {
                    'name': 'OpenAI API 키',
                    'env_var': 'OPENAI_API_KEY',
                    'validation_url': 'https://api.openai.com/v1/models',
                    'common_issues': [
                        'sk-로 시작하지만 만료된 키',
                        '할당량/크레딧 부족',
                        '잘못된 조직의 키',
                        '속도 제한 초과',
                    ],
                    'solutions': [
                        'https://platform.openai.com 에서 OpenAI 대시보드 확인',
                        '청구가 올바르게 설정되었는지 확인',
                        'API 키 권한 확인',
                        '속도 제한 재설정 대기 또는 요금제 업그레이드',
                    ],
                },
                {
                    'name': 'Anthropic API 키',
                    'env_var': 'ANTHROPIC_API_KEY',
                    'validation_url': 'https://api.anthropic.com/v1/messages',
                    'common_issues': [
                        '키 형식이 잘못됨',
                        '키가 만료됨',
                        '계정이 일시 중지됨',
                    ],
                    'solutions': [
                        'https://console.anthropic.com 에서 새 키 받기',
                        '계정 상태 확인',
                        '청구 정보 확인',
                    ],
                },
                {
                    'name': 'Alias Robotics API 키',
                    'env_var': 'ALIAS_API_KEY',
                    'validation_url': None,
                    'common_issues': [
                        '제공된 키 없음',
                        '유효하지 않은 키',
                    ],
                    'solutions': [
                        'Alias Robotics에 문의하여 유효한 키 받기',
                        '키 형식 확인',
                    ],
                },
                {
                    'name': 'OpenRouter API 키',
                    'env_var': 'OPENROUTER_API_KEY',
                    'validation_url': 'https://openrouter.ai/api/v1/models',
                    'common_issues': [
                        '유효하지 않은 키',
                        '크레딧 부족',
                    ],
                    'solutions': [
                        'https://openrouter.ai/keys 에서 키 받기',
                        '계정에 크레딧 추가',
                    ],
                },
            ],
        },

        'ollama': {
            'title': 'Ollama / 로컬 모델 문제 해결',
            'description': '로컬 모델 추론을 위해 Ollama를 설정하고 문제를 해결하는 방법',
            'related_commands': [
                {'command': '/model ollama/llama3.2', 'description': 'Ollama 모델로 전환'},
                {'command': '/settings ollama', 'description': 'Ollama 설정 안내'},
                {'command': '/env set OLLAMA', 'description': 'Ollama 환경 변수'},
            ],
            'steps': [
                {
                    'step': 1,
                    'title': 'Ollama 설치 확인',
                    'command': 'ollama --version',
                    'expected': '버전 번호를 표시해야 함',
                    'fix': 'https://ollama.com/download 에서 설치',
                },
                {
                    'step': 2,
                    'title': 'Ollama 서버 시작',
                    'command': 'ollama serve',
                    'expected': '서버가 포트 11434에서 시작됨',
                    'fix': '터미널에서 ollama serve 실행',
                    'note': '"ollama run"을 사용하지 마세요 - CLI에서만 모델을 실행합니다',
                },
                {
                    'step': 3,
                    'title': '서버가 실행 중인지 확인',
                    'command': 'curl http://127.0.0.1:11434',
                    'expected': '"Ollama is running" 반환',
                    'fix': '포트 11434이 방화벽에 의해 차단되었는지 확인',
                },
                {
                    'step': 4,
                    'title': '모델 가져오기',
                    'command': 'ollama pull llama3.2',
                    'expected': '모델이 성공적으로 다운로드됨',
                    'fix': '디스크 공간 및 인터넷 연결 확인',
                },
                {
                    'step': 5,
                    'title': 'CAI 구성',
                    'env_vars': {
                        'OLLAMA': 'true',
                        'OLLAMA_API_BASE': 'http://127.0.0.1:11434/v1',
                        'CAI_MODEL': 'ollama/llama3.2',
                    },
                },
            ],
            'docker_notes': {
                'title': 'Docker 특정 구성',
                'description': 'Docker에서 CAI를 실행할 때 localhost는 호스트가 아니라 컨테이너를 의미합니다',
                'solutions': {
                    'Windows/macOS': 'host.docker.internal:11434 사용',
                    'Linux': '172.17.0.1:11434 또는 호스트 IP 사용',
                },
            },
            'common_models': [
                'ollama/llama3.2',
                'ollama/llama3.2:1b',
                'ollama/mistral',
                'ollama/codellama',
                'ollama/qwen2.5',
                'ollama/deepseek-coder-v2',
            ],
        },

        'streaming': {
            'title': '스트리밍 구성',
            'description': 'CAI 스트리밍 옵션 이해',
            'related_commands': [
                {'command': '/env set CAI_STREAM', 'description': 'LLM 스트리밍 켜기/끄기'},
                {'command': '/env set CAI_TOOL_STREAM', 'description': '도구 출력 스트리밍 켜기/끄기'},
            ],
            'variables': {
                'CAI_STREAM': {
                    'description': 'LLM 추론 스트리밍 제어 (토큰 단위 출력)',
                    'values': {'true': '생성된 토큰 확인', 'false': '완전한 응답 대기'},
                    'default': 'false',
                },
                'CAI_TOOL_STREAM': {
                    'description': '도구 출력 스트리밍 제어 (실시간 명령 출력)',
                    'values': {'true': '발생하는 대로 출력 확인', 'false': '명령 완료 대기'},
                    'default': 'true',
                },
            },
            'note': '이들은 독립적입니다 - 하나를 다른 하나 없이 활성화할 수 있습니다',
        },

        'parallel': {
            'title': '병렬 실행 문제 해결',
            'description': '여러 에이전트를 병렬로 실행할 때의 문제',
            'related_commands': [
                {'command': '/parallel', 'description': '병렬 에이전트 실행 시작'},
                {'command': '/env set CAI_PARALLEL', 'description': '병렬 에이전트 수 설정'},
                {'command': '/env set CAI_PARALLEL_AGENTS', 'description': '병렬용 에이전트 이름 지정'},
            ],
            'variables': {
                'CAI_PARALLEL': '병렬 인스턴스 수 (1-20)',
                'CAI_PARALLEL_AGENTS': '쉼표로 구분된 에이전트 이름',
                'CAI_AUTO_RUN_PARALLEL': '병렬 에이전트 자동 시작',
            },
            'common_issues': [
                {
                    'issue': '에이전트가 메시지 기록 공유',
                    'cause': '인스턴스 격리가 작동하지 않음',
                    'fix': '각 병렬 에이전트는 고유한 인스턴스 ID를 가져야 함',
                },
                {
                    'issue': '속도 제한 오류',
                    'cause': 'API에 대한 병렬 요청이 너무 많음',
                    'fix': 'CAI_PARALLEL 줄이기 또는 지연 추가',
                },
            ],
        },

        'memory': {
            'title': '압축 메모리 문제 해결',
            'description': '/compact 세션 요약이 에이전트 프롬프트에 삽입됩니다',
            'related_commands': [
                {'command': '/memory status', 'description': '저장된 압축 요약 상태 확인'},
                {'command': '/compact', 'description': '대화를 요약으로 압축'},
                {'command': '/env set CAI_COMPACTED_MEMORY', 'description': '프롬프트 삽입 켜기(true) 또는 끄기(false)'},
            ],
            'requirements': [],
            'variables': {
                'CAI_COMPACTED_MEMORY': 'true/false — /compact 요약을 시스템 프롬프트에 삽입',
            },
            'setup_steps': [
                '/compact(또는 TUI 압축)으로 요약 생성',
                'CAI_COMPACTED_MEMORY=true로 새 에이전트가 요약을 로드하도록 설정',
            ],
        },

        'tui': {
            'title': 'TUI 모드 문제 해결',
            'description': '터미널 UI 모드 문제',
            'related_commands': [
                {'command': 'cai --tui', 'description': 'TUI 모드로 CAI 시작'},
                {'command': '/env set CAI_TUI_MODE', 'description': 'TUI 모드 켜기/끄기'},
                {'command': '/help', 'description': '모든 명령 표시'},
            ],
            'requirements': [
                'Unicode를 지원하는 터미널',
                '최소 80x24 터미널 크기',
            ],
            'variables': {
                'CAI_TUI_MODE': 'TUI 모드 활성화',
                'CAI_TUI_MAX_LINES': '최대 출력 라인',
                'CAI_TUI_MAX_RERENDERS_PER_SEC': '렌더링 성능',
            },
            'cli_only_vars': [
                'CAI_API_HOST', 'CAI_API_PORT', 'CAI_API_WORKERS',
                '이 변수는 CLI 모드에만 적용됩니다',
            ],
            'tui_only_vars': [
                'CAI_TUI_MODE', 'CAI_TUI_STARTUP_YAML', 'CAI_TUI_SHARED_PROMPT',
                'CAI_TUI_MAX_LINES', 'CAI_TUI_MAX_RERENDERS_PER_SEC',
            ],
        },

        'connection': {
            'title': '연결/네트워크 문제',
            'description': '네트워크 연결 문제 해결',
            'related_commands': [
                {'command': '/settings status', 'description': '시스템 상태'},
                {'command': '/settings validate', 'description': 'API 키 검증'},
                {'command': '/env set CAI_SKIP_NETWORK_CHECK', 'description': '네트워크 검사 건너뛰기(비권장)'},
            ],
            'checks': [
                {
                    'check': '인터넷 연결',
                    'command': 'curl -I https://api.openai.com',
                    'fix': '방화벽/프록시 설정 확인',
                },
                {
                    'check': 'DNS 해석',
                    'command': 'nslookup api.openai.com',
                    'fix': '8.8.8.8을 DNS로 사용해 보기',
                },
                {
                    'check': '프록시 구성',
                    'env_vars': ['HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY'],
                    'fix': '적절한 프록시 환경 변수 설정',
                },
            ],
            'skip_check': {
                'variable': 'CAI_SKIP_NETWORK_CHECK',
                'description': '네트워크 가용성 검사 건너뛰기 (권장하지 않음)',
            },
        },
    },
}


def get_string(key: str, lang: str = DEFAULT_LANGUAGE) -> str:
    """Get a translated string for the given key and language.

    Falls back to English if the key is not found in the requested language.

    Args:
        key: The string key to look up
        lang: Language code (e.g., 'en', 'es', 'ru')

    Returns:
        The translated string, or the key if not found
    """
    # Try requested language first
    if lang in UI_STRINGS and key in UI_STRINGS[lang]:
        return UI_STRINGS[lang][key]

    # Fall back to English
    if key in UI_STRINGS.get('en', {}):
        return UI_STRINGS['en'][key]

    # Return key if not found
    return key


def get_faq(topic: str, lang: str = DEFAULT_LANGUAGE) -> Dict[str, Any]:
    """Get FAQ content for a topic in the specified language.

    Falls back to English if not available in requested language.

    Args:
        topic: FAQ topic key (e.g., 'api_keys', 'ollama')
        lang: Language code

    Returns:
        Dictionary with FAQ content
    """
    if lang in FAQ_CONTENT and topic in FAQ_CONTENT[lang]:
        return FAQ_CONTENT[lang][topic]

    if topic in FAQ_CONTENT.get('en', {}):
        return FAQ_CONTENT['en'][topic]

    return {}


def get_available_languages() -> Dict[str, str]:
    """Get dictionary of available languages.

    Returns:
        Dictionary mapping language codes to display names
    """
    return SUPPORTED_LANGUAGES.copy()
