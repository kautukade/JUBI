"""
Module for displaying the CAI banner and welcome message.
"""
# Standard library imports
import glob
import logging
import os
import sys
from configparser import ConfigParser

# Third-party imports
import requests  # pylint: disable=import-error
from rich import box
from rich.align import Align  # pylint: disable=import-error
from rich.console import Console, Group  # pylint: disable=import-error
from rich.padding import Padding  # pylint: disable=import-error
from rich.panel import Panel  # pylint: disable=import-error
from rich.rule import Rule
from rich.table import Table  # pylint: disable=import-error
from rich.text import Text

from cai.config import DEFAULT_AGENT_TYPE
from cai.repl.ui.agent_notices import is_orchestration_agent, orchestration_beta_text
from cai.repl.ui.repl_input_shortcuts import quick_shortcuts_text

# Match ``cai.util.streaming.CAI_GREEN`` (avoid importing the whole streaming module).
_CAI_GREEN = "#00ff9d"
CAI_GREEN = _CAI_GREEN
_GREY_MID = "#888888"
_GREY = "dim white"

# Same ASCII as the legacy blue banner, shifted left for the two-column layout.
_BANNER_LOGO_LSTRIP = 7
_LOGO_LINE_COUNT = 16
# Inner min height so panel bottom aligns with the 16-line logo (title sits on panel top border).
_PANEL_MIN_INNER_ABOVE = _LOGO_LINE_COUNT - 2
# Side-by-side only if the right column can hold the panel (else logo above, panel below).
_BANNER_MIN_RIGHT_FOR_SPLIT = 48
_BANNER_SPLIT_GAP = 2
# Minimum terminal width for help guide: command column + Alias1 column side-by-side.
_MIN_QUICK_GUIDE_SIDE_BY_SIDE = 96

# Official docs: full slash-command reference (shown in bare /help footer).
_QUICK_GUIDE_COMMANDS_DOC_URL = (
    "https://aliasrobotics.github.io/cai/cai/getting-started/commands/"
)


def _safe_console_width(console: Console) -> int:
    """Usable width for layout; tests may pass a MagicMock without a real ``width``."""
    try:
        return int(getattr(console, "width", 120))
    except (TypeError, ValueError):
        return 120


_MODEL_HINT_FULL = "(Use alias models for best Cybersecurity performance)"
_MODEL_HINT_MED = "(Alias models · Cybersecurity)"
_MODEL_HINT_SHORT = "(Models · Cybersec.)"

# For reading TOML files
if sys.version_info >= (3, 11):
    import tomllib
else:
    try:
        import tomli as tomllib
    except ImportError:
        # If tomli is not available, we'll handle it in the get_version function
        pass


def _version_from_pyproject_cwd() -> str:
    """Read ``[project].version`` from ``./pyproject.toml`` (cwd). Fallback for dev layouts."""
    version = "unknown"
    try:
        if sys.version_info >= (3, 11):
            toml_parser = tomllib
        else:
            try:
                import tomli as toml_parser
            except ImportError:
                logging.warning("Could not import tomli. Falling back to manual parsing.")
                with open("pyproject.toml", "r", encoding="utf-8") as f:
                    for line in f:
                        if line.strip().startswith("version = "):
                            return line.split("=", 1)[1].strip().strip("\"'")
                return version

        with open("pyproject.toml", "rb") as f:
            config = toml_parser.load(f)
        version = config.get("project", {}).get("version", "unknown")
    except Exception as e:  # pylint: disable=broad-except
        logging.warning("Could not read version from pyproject.toml: %s", e)
    return version


def get_version():
    """Version for banner/UI: installed ``cai-framework`` (matches pip / ``cai --version``), else cwd pyproject."""
    try:
        import importlib.metadata

        return importlib.metadata.version("cai-framework")
    except importlib.metadata.PackageNotFoundError:
        pass
    except Exception as e:  # pylint: disable=broad-except
        logging.warning("Could not read installed cai-framework version: %s", e)
    return _version_from_pyproject_cwd()


def _version_display(version: str) -> str:
    v = version.strip()
    if not v:
        return "v?"
    return v if v[0].lower() == "v" else f"v{v}"


def _banner_left_column_width(console: Console) -> int:
    """Width Rich uses for the logo column (≥64; grows if a line of ASCII art is wider)."""
    opts = console.options
    logo_w = console.measure(_banner_logo_markup(), options=opts).maximum
    return max(64, logo_w)


def _banner_right_column_outer_width(console: Console) -> int:
    """Outer width of the panel column (depends on measured logo width, not a fixed 64)."""
    return max(16, console.width - _banner_left_column_width(console) - _BANNER_SPLIT_GAP)


def _banner_side_by_side(console: Console) -> bool:
    """True when the terminal is wide enough for logo left + panel right."""
    left = _banner_left_column_width(console)
    right_avail = console.width - left - _BANNER_SPLIT_GAP
    return right_avail >= _BANNER_MIN_RIGHT_FOR_SPLIT


def _banner_panel_outer_width(console: Console, *, stacked: bool) -> int:
    """Outer width of the session panel (full terminal width when stacked)."""
    if stacked:
        return max(16, console.width - 2)
    return _banner_right_column_outer_width(console)


def _banner_panel_body_width_from_outer(outer: int) -> int:
    return max(16, outer - 2)


def _model_hint_for_banner_width(body_width: int) -> str:
    if body_width < 40:
        return _MODEL_HINT_SHORT
    if body_width < 52:
        return _MODEL_HINT_MED
    return _MODEL_HINT_FULL


def _session_banner_title_bar(console: Console, version: str, *, panel_outer_width: int) -> Text:
    """Full *Cybersecurity AI* row, or compact *CAI v…* when the panel is narrow."""
    # Match ``Panel`` outer width so title choice fits Rich's top rule (``width - 4`` cells).
    right_col = panel_outer_width
    opts = console.options.update_width(right_col)
    title_slot = max(12, right_col - 4)
    full = Text()
    full.append(" CAI ", style="bold #0d1117 on #00ff9d")
    full.append(" Cybersecurity AI ", style="bold white on #004433")
    full.append(_version_display(version), style=f"bold {_CAI_GREEN} on #004433")
    full.append(" ", style="on #004433")
    if console.measure(full, options=opts).maximum <= title_slot:
        return full
    compact = Text()
    compact.append(" CAI ", style="bold #0d1117 on #00ff9d")
    compact.append(f" {_version_display(version)}", style=f"bold {_CAI_GREEN} on #004433")
    compact.append(" ", style="on #004433")
    return compact


def _banner_model_cell(model: str, hint: str) -> Text:
    t = Text()
    t.append(model, style=f"bold {_CAI_GREEN}")
    t.append(" ", style="")
    t.append(hint, style=_GREY)
    return t


def _banner_command_rows():
    return [
        ("/agent", "agents · list, select, info"),
        ("/model", "change AI model"),
        ("/sessions", "last sessions list"),
        ("/env", "env / settings"),
    ]


def _pad_banner_inner(console: Console, inner, content_width: int, min_lines: int):
    opts = console.options.update_width(content_width)
    measured = len(list(console.render_lines(inner, opts)))
    pad = max(0, min_lines - measured)
    if pad:
        return Group(inner, *[Text(" ") for _ in range(pad)])
    return inner


def _build_session_banner_panel(
    console: Console,
    model: str,
    agent_type: str,
    title: Text,
    *,
    panel_body_width: int,
    min_inner_lines: int,
    panel_outer_width: int | None = None,
) -> Panel:
    cw = panel_body_width
    hint = _model_hint_for_banner_width(cw)
    rows = _banner_command_rows()

    _unrestricted = os.getenv("CAI_UNRESTRICTED", "false").strip().lower() in ("true", "1", "yes")
    _yolo = os.getenv("CAI_YOLO", "").strip().lower() in ("true", "1", "yes")
    sess_parts: list = [
        ("Model  ", _GREY),
        (model, f"bold {_CAI_GREEN}"),
        (" ", ""),
        (hint, _GREY),
        "\n",
        ("Agent  ", _GREY),
        (agent_type, "italic white"),
    ]
    if is_orchestration_agent(agent_type):
        sess_parts += [
            "\n",
            orchestration_beta_text(),
        ]
    if _unrestricted:
        # Un solo Text.from_markup evita un hueco “sin color” entre segmentos en algunos terminales.
        sess_parts += [
            "\n",
            Text.from_markup(
                "[bold bright_red]Unrestricted Mode [/bold bright_red]"
                "[bold white on bright_red] BETA [/]"
            ),
        ]
    sess = Text.assemble(*sess_parts)
    cmds = Table(
        show_header=True,
        header_style=f"bold {_CAI_GREEN}",
        border_style=_GREY_MID,
        box=box.SIMPLE_HEAD,
        padding=(0, 1),
        collapse_padding=True,
    )
    cmds.add_column("Command", style=f"bold {_CAI_GREEN}", no_wrap=True)
    cmds.add_column("Summary", style=_GREY)
    for c, h in rows:
        cmds.add_row(c, h)
    # Promo lines above /help subtitle (same style: bold yellow + highlighted invocation)
    _promo_rows: list[Text] = []
    if not _unrestricted:
        _promo_rows.append(
            Text.assemble(
                ("Try ", "bold yellow"),
                ("cai --unrestricted", "bold black on bright_yellow"),
                (" BETA for uncensored mode", "bold yellow"),
            )
        )
    if not _yolo:
        _promo_rows.append(
            Text.assemble(
                ("Try ", "bold yellow"),
                ("cai --yolo", "bold black on bright_yellow"),
                (" — YOLO: skip command prompts", "bold yellow"),
            )
        )
    inner = Group(Padding(sess, (1, 0, 0, 0)), Rule(style=_GREY_MID), cmds, *_promo_rows)
    inner = _pad_banner_inner(console, inner, cw, min_inner_lines)
    if panel_outer_width is not None:
        return Panel(
            inner,
            width=panel_outer_width,
            title=title,
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(0, 1),
            subtitle=f"[bold {_CAI_GREEN}]/help[/bold {_CAI_GREEN}] [italic white]for everything else[/italic white]",
            subtitle_align="left",
        )
    return Panel(
        inner,
        title=title,
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(0, 1),
        subtitle=f"[bold {_CAI_GREEN}]/help[/bold {_CAI_GREEN}] [italic white]for everything else[/italic white]",
        subtitle_align="left",
    )


def _banner_logo_markup() -> Text:
    raw = [
        "                CCCCCCCCCCCCC      ++++++++   ++++++++      IIIIIIIIII",
        "             CCC::::::::::::C  ++++++++++       ++++++++++  I::::::::I",
        "           CC:::::::::::::::C ++++++++++         ++++++++++ I::::::::I",
        "          C:::::CCCCCCCC::::C +++++++++    ++     +++++++++ II::::::II",
        "         C:::::C       CCCCCC +++++++     +++++     +++++++   I::::I",
        "        C:::::C                +++++     +++++++     +++++    I::::I",
        "        C:::::C                ++++                   ++++    I::::I",
        "        C:::::C                 ++                     ++     I::::I",
        "        C:::::C                  +   +++++++++++++++   +      I::::I",
        "        C:::::C                    +++++++++++++++++++        I::::I",
        "        C:::::C                     +++++++++++++++++         I::::I",
        "         C:::::C       CCCCCC        +++++++++++++++          I::::I",
        "          C:::::CCCCCCCC::::C         +++++++++++++         II::::::II",
        "           CC:::::::::::::::C           +++++++++           I::::::::I",
        "             CCC::::::::::::C             +++++             I::::::::I",
        "                CCCCCCCCCCCCC               ++              IIIIIIIIII",
    ]
    n = _BANNER_LOGO_LSTRIP
    trimmed = [line[n:] if len(line) >= n else line for line in raw]
    w = max(len(r) for r in trimmed)
    padded = [r.ljust(w) for r in trimmed]
    body = "\n".join(f"[bold {_CAI_GREEN}]{line}[/bold {_CAI_GREEN}]" for line in padded)
    return Text.from_markup(body)


def get_supported_models_count():
    """Get the count of supported models (with function calling)."""
    try:
        # Fetch model data from LiteLLM repository
        response = requests.get(
            "https://raw.githubusercontent.com/BerriAI/litellm/main/"
            "model_prices_and_context_window.json",
            timeout=2
        )

        if response.status_code == 200:
            model_data = response.json()

            # Count models with function calling support
            function_calling_models = sum(
                1 for model_info in model_data.values()
                if model_info.get("supports_function_calling", False)
            )

            # Try to get Ollama models count
            try:
                from cai.util import get_ollama_api_base
                ollama_api_base = get_ollama_api_base()
                
                # Add authentication headers for Ollama Cloud if using OPENAI_BASE_URL
                headers = {}
                if "ollama.com" in ollama_api_base:
                    api_key = os.getenv("OPENAI_API_KEY")
                    if api_key:
                        headers["Authorization"] = f"Bearer {api_key}"
                
                ollama_response = requests.get(
                    f"{ollama_api_base.replace('/v1', '')}/api/tags",
                    headers=headers,
                    timeout=1
                )

                if ollama_response.status_code == 200:
                    ollama_data = ollama_response.json()
                    ollama_models = len(
                        ollama_data.get(
                            'models', ollama_data.get('items', [])
                        )
                    )
                    return function_calling_models + ollama_models
            except Exception:  # pylint: disable=broad-except
                logging.debug("Could not fetch Ollama models")
                # Continue without Ollama models

            return function_calling_models
    except Exception:  # pylint: disable=broad-except
        logging.warning("Could not fetch model data from LiteLLM")

    # Default count if we can't fetch the data
    return "many"


def count_tools():
    """Count the number of tools in the CAI framework."""
    try:
        # Count Python files in the tools directory
        tool_files = glob.glob("cai/tools/**/*.py", recursive=True)
        # Exclude __init__.py and other non-tool files
        tool_files = [
            f for f in tool_files
            if not f.endswith("__init__.py") and not f.endswith("__pycache__")
        ]
        return len(tool_files)
    except Exception:  # pylint: disable=broad-except
        logging.warning("Could not count tools")
        return "50+"


def count_agents():
    """Count the number of agents in the CAI framework."""
    try:
        # Count Python files in the agents directory
        agent_files = glob.glob("cai/agents/**/*.py", recursive=True)
        # Exclude __init__.py and other non-agent files
        agent_files = [
            f for f in agent_files
            if not f.endswith("__init__.py") and not f.endswith("__pycache__")
        ]
        return len(agent_files)
    except Exception:  # pylint: disable=broad-except
        logging.warning("Could not count agents")
        return "20+"


def count_ctf_memories():
    """Count the number of CTF memories in the CAI framework."""
    # This is a placeholder - adjust the actual counting logic based on your
    # framework structure
    return "100+"


def display_banner(
    console: Console,
    *,
    model: str | None = None,
    agent_type: str | None = None,
):
    """
    Display the CAI session header: green ASCII logo and session panel.

    Wide terminals: logo left, panel right. Narrow terminals: logo above, panel below.

    *model* and *agent_type* default to ``CAI_MODEL`` / ``CAI_AGENT_TYPE`` when omitted.
    Prefer passing :class:`CAIConfig` values from startup so the banner matches the active session.

    Title and model hint shorten automatically on narrow terminals.
    """
    # Blank line between the shell line (e.g. ``$ cai``) and the banner.
    console.print()

    version = get_version()
    model_name = model if model is not None else os.getenv("CAI_MODEL", "alias1")
    agent_name = agent_type if agent_type is not None else os.getenv(
        "CAI_AGENT_TYPE", DEFAULT_AGENT_TYPE
    )

    stacked = not _banner_side_by_side(console)
    outer = _banner_panel_outer_width(console, stacked=stacked)
    body_w = _banner_panel_body_width_from_outer(outer)

    logo_only = _banner_logo_markup()
    title = _session_banner_title_bar(console, version, panel_outer_width=outer)
    min_inner = 0 if stacked else _PANEL_MIN_INNER_ABOVE
    panel = _build_session_banner_panel(
        console,
        model_name,
        agent_name,
        title,
        panel_body_width=body_w,
        min_inner_lines=min_inner,
        panel_outer_width=outer if stacked else None,
    )

    if stacked:
        console.print(logo_only)
        console.print()
        console.print(panel)
    else:
        layout = Table(
            show_header=False,
            box=None,
            expand=True,
            pad_edge=False,
            collapse_padding=True,
        )
        layout.add_column(width=64, vertical="top", no_wrap=True)
        layout.add_column(ratio=1, vertical="top", min_width=48)
        layout.add_row(logo_only, panel)
        console.print(layout)
    # One blank line before the input toolbar / prompt.
    console.print()


def display_framework_capabilities(console: Console):
    """
    Display a table showcasing CAI framework capabilities in Metasploit style.

    Args:
        console: Rich console for output
    """
    # Create the main table
    table = Table(
        title="",
        box=None,
        show_header=False,
        show_edge=False,
        padding=(0, 2)
    )

    table.add_column("Category", style="bold cyan")
    table.add_column("Count", style="bold yellow")
    table.add_column("Description", style="white")

    # Add rows for different capabilities
    table.add_row(
        "AI Models",
        str(get_supported_models_count()),
        "Supported AI models including GPT-4, Claude, Llama"
    )

    # table.add_row(
    #     "Tools",
    #     str(count_tools()),
    #     "Cybersecurity tools for reconnaissance and scanning"
    # )

    table.add_row(
        "Agents",
        str(count_agents()),
        "Specialized AI agents for different cybersecurity tasks"
    )

    # Add the table to a panel for better visual separation
    capabilities_panel = Panel(
        table,
        title="[bold blue]CAI Features[/bold blue]",
        border_style="blue",
        padding=(1, 2)
    )

    console.print(capabilities_panel)


def display_welcome_tips(console: Console):
    """
    Display welcome message with tips for using the REPL.

    Args:
        console: Rich console for output
    """
    console.print(Panel(
        "[white]• Use arrow keys ↑↓ to navigate command history[/white]\n"
        "[white]• Press Tab for command completion[/white]\n"
        "[white]• Type /help for available commands[/white]\n"
        "[white]• Press ? on an empty line for input shortcuts (no Enter needed)[/white]\n"
        "[white]• Type /help aliases for slash-command aliases[/white]\n"
        "[white]• Press Ctrl+L to clear the screen[/white]\n"
        "[white]• Press Alt+Enter or Shift+Enter to add a new line (multiline input)[/white]\n"
        "[white]• Press Ctrl+C to exit[/white]",
        title="Quick Tips",
        border_style="blue"
    ))


def display_agent_overview(console: Console):
    """
    Display a quick overview of available agents.
    
    Args:
        console: Rich console for output
    """
    from rich.table import Table
    
    # Create agents table
    agents_table = Table(
        title="",
        box=None,
        show_header=True,
        header_style="bold yellow",
        show_edge=False,
        padding=(0, 1)
    )
    
    agents_table.add_column("Agent", style="cyan", width=25)
    agents_table.add_column("Specialization", style="white")
    agents_table.add_column("Best For", style="green")
    
    # Add agent rows
    agents = [
        ("one_tool_agent", "Basic CTF solver", "CTF challenges, Linux operations"),
        ("red_teamer", "Offensive security", "Penetration testing, exploitation"),
        ("blue_teamer", "Defensive security", "System defense, monitoring"),
        ("bug_bounter", "Bug bounty hunter", "Web security, API testing"),
        ("dfir", "Digital forensics", "Incident response, analysis"),
        ("network_traffic_analyzer", "Network security", "Traffic analysis, monitoring"),
        ("flag_discriminator", "CTF flag extraction", "Finding and validating flags"),
        ("codeagent", "Code specialist", "Exploit development, analysis"),
        ("thought", "Strategic planning", "High-level analysis, planning"),
    ]
    
    for agent, spec, best_for in agents:
        agents_table.add_row(agent, spec, best_for)
    
    # Create the panel
    agent_panel = Panel(
        agents_table,
        title="[bold yellow]🤖 Available Security Agents[/bold yellow]",
        border_style="yellow",
        padding=(1, 2),
        title_align="center"
    )
    
    console.print(agent_panel)


def session_summary_panel_title() -> Text:
    """Panel title strip for the headless/TUI exit summary (matches session banner chrome)."""
    t = Text()
    t.append(" CAI ", style="bold #0d1117 on #00ff9d")
    t.append(" Session Summary ", style="bold white on #004433")
    return t


def _quick_guide_outer_title() -> Text:
    """Title strip matching the session banner bar (no emoji)."""
    t = Text()
    t.append(" CAI ", style="bold #0d1117 on #00ff9d")
    t.append(" defacto scaffolding for cybersecurity agents ", style="bold white on #004433")
    t.append("— /help <topic> for detailed docs ", style=f"bold {_CAI_GREEN} on #004433")
    return t


def environment_reference_outer_title() -> Text:
    """Outer title strip for the environment reference panel (same chrome as ``display_quick_guide``)."""
    t = Text()
    t.append(" CAI ", style="bold #0d1117 on #00ff9d")
    t.append(" environment reference ", style="bold white on #004433")
    t.append("— /env list + runtime hints ", style=f"bold {_CAI_GREEN} on #004433")
    return t


def help_topics_outer_title() -> Text:
    """Outer title for ``/help topics`` (same chrome as other CAI help panels)."""
    t = Text()
    t.append(" CAI ", style="bold #0d1117 on #00ff9d")
    t.append(" topics ", style="bold white on #004433")
    t.append("— commands by category + /help <topic> ", style=f"bold {_CAI_GREEN} on #004433")
    return t


def _quick_guide_alias_panel_title() -> Text:
    t = Text()
    t.append(" Alias1 ", style="bold #0d1117 on #00ff9d")
    t.append(" — best model for cybersecurity ", style="bold white on #004433")
    return t


def _quick_guide_subpanel_title(label: str) -> Text:
    """Title strip matching Alias1 panel style for subpanels."""
    t = Text()
    t.append(f" {label} ", style="bold #0d1117 on #00ff9d")
    return t


def _quick_guide_top_row(console: Console, help_ref: Text, right_column) -> Group | Table:
    """Two columns when wide enough; stacked (reference, then Alias1) when narrow."""
    if _safe_console_width(console) >= _MIN_QUICK_GUIDE_SIDE_BY_SIDE:
        row = Table(
            show_header=False,
            box=None,
            expand=True,
            pad_edge=False,
            collapse_padding=True,
        )
        # Wider right column for Alias1 (no fixed width cap — was leaving a large empty gap).
        row.add_column(vertical="top", ratio=2, min_width=36)
        row.add_column(vertical="top", ratio=3, min_width=44)
        row.add_row(help_ref, right_column)
        return row
    return Group(help_ref, Text(""), right_column)


def display_help_topics_index(console: Console):
    """``/help topics``: intro, slash commands by category, tips (no env reference tables)."""
    from cai.repl.commands.command_reference_index import help_topic_rows_by_category

    g = f"bold {_CAI_GREEN}"
    doc_url = _QUICK_GUIDE_COMMANDS_DOC_URL

    intro_block = Text.assemble(
        (
            "CAI (Cybersecurity AI): penetration testing, bug bounty hunting, and security research.\n\n",
            "white",
        ),
        (
            "CLI under active development—report issues if something looks wrong.\n\n",
            _GREY,
        ),
        (
            "The tables list registered slash commands by category (live registry). "
            "For a longer help panel, run ",
            "white",
        ),
        ("/help <topic>", g),
        (
            " — usually the command name without the slash (e.g. ",
            "white",
        ),
        ("/help agent", g),
        (" for ", "white"),
        ("/agent", g),
        ("). Special topics: ", "white"),
        ("/help var", g),
        (", ", _GREY),
        ("/help commands", g),
        (", ", _GREY),
        ("/help topics", g),
        (", ", _GREY),
        ("/help aliases", g),
        (", ", _GREY),
        ("/help config", g),
        (".\n\n", "white"),
        ("• ", _GREY),
        ("All commands in one panel: ", "white"),
        ("/help commands", g),
        ("\n", ""),
        ("• ", _GREY),
        ("Quick start: ", "white"),
        ("/quickstart", g),
        (" (", "white"),
        ("/qs", g),
        (", ", _GREY),
        ("/quick", g),
        (")\n", "white"),
        ("• ", _GREY),
        ("Bare ", "white"),
        ("/help", g),
        (" or ", "white"),
        ("/h", g),
        (": quick guide plus full environment-variable tables. ", "white"),
        ("/help topics", g),
        (": this index only (no env tables).\n", "white"),
        ("• ", _GREY),
        ("/help var NAME", g),
        (" — long-form help for one catalog variable.\n", "white"),
    )

    def _topic_rows_table(rows: list[tuple[str, str]]) -> Table:
        tbl = Table(
            show_header=False,
            box=None,
            pad_edge=False,
            collapse_padding=True,
            expand=True,
        )
        tbl.add_column(style=f"bold {_CAI_GREEN}", ratio=1, min_width=18)
        tbl.add_column(style="white", ratio=3)
        for cmd, desc in rows:
            tbl.add_row(cmd, desc)
        return tbl

    def _category_block(title: str, rows: list[tuple[str, str]]) -> Group:
        heading = Text(title + "\n", style=f"bold underline {_CAI_GREEN}")
        return Group(heading, _topic_rows_table(rows))

    categories = help_topic_rows_by_category()
    out_cat: list[tuple[str, list[tuple[str, str]]]] = []
    for title, rows in categories:
        r = list(rows)
        if title == "Environment & Configuration":
            r.append(
                (
                    "/help var NAME",
                    "Long-form help for one or more catalog variables",
                )
            )
        out_cat.append((title, r))
    categories = out_cat

    section_blocks: list = [intro_block, Text("")]
    for cat_title, cat_rows in categories:
        section_blocks.append(_category_block(cat_title, cat_rows))
        section_blocks.append(Text(""))

    tips_body = Text.assemble(
        ("  • Use ", _GREY),
        ("Tab", g),
        (" for command completion\n", _GREY),
        ("  • Use ", _GREY),
        ("↑/↓", g),
        (" to navigate command history\n", _GREY),
        ("  • Use ", _GREY),
        ("Ctrl+C", g),
        (" to interrupt running commands\n", _GREY),
        ("  • Use ", _GREY),
        ("Ctrl+L", g),
        (" to clear the screen\n", _GREY),
        ("  • Most commands have aliases (e.g., ", _GREY),
        ("/h", g),
        (" for ", _GREY),
        ("/help", g),
        (")\n", _GREY),
        ("  • Type ", _GREY),
        ("/help <topic>", g),
        (" for the full help panel (topic usually matches the command name).", _GREY),
    )
    tips_panel = Panel(
        tips_body,
        title=_quick_guide_subpanel_title("Tips"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )
    section_blocks.append(tips_panel)

    body = Group(*section_blocks)
    console.print(
        Panel(
            body,
            title=help_topics_outer_title(),
            title_align="center",
            subtitle=(
                f"[dim]Full command reference (all slash commands):[/dim] "
                f"[link={doc_url}]{doc_url}[/link]"
            ),
            subtitle_align="center",
            border_style=_CAI_GREEN,
            padding=(1, 1, 0, 1),
        )
    )


def display_quick_guide(console: Console):
    """Quick guide: banner palette; wide = command ref | Alias1, then full-width tail; else stacked."""
    g = f"bold {_CAI_GREEN}"
    rule = f"{'━' * 55}\n"

    help_ref = Text.assemble(
        ("CAI Command Reference", f"bold underline {_CAI_GREEN}"),
        "\n\n",
        (rule, _GREY_MID),
        "\n",
        ("AGENT MANAGEMENT", g),
        " (/a)\n",
        ("    /agent list", g),
        (" - List all available agents\n", _GREY),
        ("    /agent [NAME/NUMBER]", g),
        ("  # to select", _GREY),
        "\n",
        ("    /agent info [NAME]", g),
        (" - Show agent details\n", _GREY),
        ("    /parallel add [NAME]", g),
        (" - Configure parallel agents\n", _GREY),
        ("    /queue", g),
        (" - Prompt queue\n\n", _GREY),
        ("MODEL", g),
        "\n",
        ("    /model [NAME]", g),
        (" - Change AI model\n", _GREY),
        ("    /model show", g),
        (" - Browse full model list\n\n", _GREY),
        ("MEMORY & HISTORY", g),
        "\n",
        ("    /memory list", g),
        (" - List saved memories\n", _GREY),
        ("    /history", g),
        (" - View conversation history\n", _GREY),
        ("    /compact", g),
        (" - AI-powered conversation summary\n", _GREY),
        ("    /flush", g),
        (" - Clear conversation history\n\n", _GREY),
        ("ENVIRONMENT", g),
        "\n",
        ("    /workspace set [NAME]", g),
        (" - Set workspace directory\n", _GREY),
        ("    /env", g),
        (" - Manage environment variables\n", _GREY),
        ("    /virt list | /virt set [ID] | /virt run [IMAGE]", g),
        (" - Docker environments\n\n", _GREY),
        ("TOOLS & INTEGRATION", g),
        "\n",
        ("    /mcp load <url> <name>", g),
        (" — SSE; ", _GREY),
        ("/mcp load stdio <name> <cmd>", g),
        (" [args…] — stdio\n", _GREY),
        ("    /shell [COMMAND] or $", g),
        (" - Execute shell commands\n\n", _GREY),
        (rule, _GREY_MID),
    )

    current_model = os.getenv("CAI_MODEL", "alias1")
    current_agent_type = os.getenv("CAI_AGENT_TYPE", DEFAULT_AGENT_TYPE)

    def _quick_guide_env_value_style(var_name: str, raw: str) -> str:
        """Booleans: on/true in CAI green; off/false dim. Other vars: neutral white."""
        v = (raw or "").strip().lower()
        if var_name in ("CAI_STREAM", "CAI_TOOL_STREAM"):
            if v in ("true", "1", "yes", "on"):
                return f"bold {_CAI_GREEN}"
            return "dim white"
        return "white"

    # No leading "\n": the table row already ends with a newline; an extra one doubled the gap.
    workflow_text = Text.assemble(
        # No underline here: avoids a second “bar” right under the ━ rule above.
        ("Quick Start Workflows", f"bold {_CAI_GREEN}"),
        "\n\n",
        ("CTF Challenge", g),
        "\n",
        ("  1. ", _GREY),
        ("/agent select redteam_agent", g),
        "\n",
        ("  2. ", _GREY),
        ("/workspace set ctf_name", g),
        "\n",
        ("  - ", _GREY),
        ("Describe the challenge...\n\n", _GREY),
        ("Bug Bounty", g),
        "\n",
        ("  1. ", _GREY),
        ("/agent select bug_bounter_agent", g),
        "\n",
        ("  2. ", _GREY),
        ("/model claude-3-7-sonnet", g),
        "\n",
        ("  3. ", _GREY),
        ("Test https://example.com\n\n", _GREY),
        ("Parallel Recon", g),
        "\n",
        ("  1. ", _GREY),
        ("/parallel add red_teamer", g),
        "\n",
        ("  2. ", _GREY),
        ("/parallel add network_traffic_analyzer", g),
        "\n",
        ("  3. ", _GREY),
        ("Scan 192.168.1.0/24", _GREY),
        ("  # queue prompts, then:", _GREY),
        "\n",
        ("  4. ", _GREY),
        ("/parallel run", g),
        "\n",
        ("  5. ", _GREY),
        ("/merge  ", g),
        ("# merge contexts + auto-exit parallel", _GREY),
        "\n",
        ("  6. ", _GREY),
        ("/parallel clear  ", g),
        ("# exit without merge\n\n", _GREY),
        ("MCP Tools Integration", g),
        "\n",
        ("  1. ", _GREY),
        (
            "/mcp load stdio burp java -jar /path/to/mcp-proxy-all.jar --sse-url http://127.0.0.1:9876",
            g,
        ),
        "\n",
        ("     ", _GREY),
        ("# Burp MCP: PortSwigger stdio proxy (extract JAR from the MCP Server BApp)\n", _GREY),
        ("  2. ", _GREY),
        ("/mcp add burp red_teamer", g),
        "\n",
        ("  3. ", _GREY),
        ("Use the new tools...\n\n", _GREY),
        ("Blue team review", g),
        "\n",
        ("  1. ", _GREY),
        ("/agent select blue_teamer", g),
        "\n",
        ("  2. ", _GREY),
        ("/workspace set home_lab", g),
        ("  ", _GREY),
        ("# scope the assets or folder you care about\n", _GREY),
        ("  3. ", _GREY),
        (
            "Paste a config snippet, checklist, or describe what you monitor — "
            "ask what to verify first\n",
            _GREY,
        ),
        "\n",
        ("  - ", _GREY),
        (
            'Example: "Review this firewall rule draft" or '
            '"What should I harden on a small Linux server?"\n\n',
            _GREY,
        ),
    )

    alias_url = "https://news.aliasrobotics.com/alias1-a-privacy-first-cybersecurity-ai/"
    context_body = Text.assemble(
        ("Security-Focused AI Framework\n\n", "bold white"),
        ("For optimal cybersecurity AI performance, use\n", _GREY),
        ("alias1", g),
        (" - specifically designed for cybersecurity\n", _GREY),
        ("tasks with superior domain knowledge.\n\n", _GREY),
        ("alias1", g),
        (" outperforms general-purpose models in:\n", _GREY),
        ("  • Vulnerability assessment\n", _GREY),
        ("  • Penetration testing and bug bounty\n", _GREY),
        ("  • Security analysis\n", _GREY),
        ("  • Threat detection\n\n", _GREY),
        ("Learn more about alias1 and its privacy-first approach:\n", _GREY),
        (alias_url, f"{_CAI_GREEN} underline"),
    )
    context_tip = Panel(
        context_body,
        title=_quick_guide_alias_panel_title(),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )
    _stream = os.getenv("CAI_STREAM", "false")
    _tool_stream = os.getenv("CAI_TOOL_STREAM", "true")
    env_body = Text.assemble(
        ("  CAI_MODEL ", _GREY),
        ("= ", _GREY),
        (f"{current_model}\n", _quick_guide_env_value_style("CAI_MODEL", current_model)),
        ("  CAI_AGENT_TYPE ", _GREY),
        ("= ", _GREY),
        (f"{current_agent_type}\n", _quick_guide_env_value_style("CAI_AGENT_TYPE", current_agent_type)),
        ("  CAI_PARALLEL ", _GREY),
        ("= ", _GREY),
        (f"{os.getenv('CAI_PARALLEL', '1')}\n", "white"),
        ("  CAI_STREAM ", _GREY),
        ("= ", _GREY),
        (f"{_stream} (LLM)\n", _quick_guide_env_value_style("CAI_STREAM", _stream)),
        ("  CAI_TOOL_STREAM ", _GREY),
        ("= ", _GREY),
        (f"{_tool_stream} (Tools)\n", _quick_guide_env_value_style("CAI_TOOL_STREAM", _tool_stream)),
        ("  CAI_WORKSPACE ", _GREY),
        ("= ", _GREY),
        (f"{os.getenv('CAI_WORKSPACE', 'default')}\n", "white"),
        ("  CAI_TEMPERATURE ", _GREY),
        ("= ", _GREY),
        (f"{os.getenv('CAI_TEMPERATURE', '0.7')}\n", "white"),
        ("  CAI_TOP_P ", _GREY),
        ("= ", _GREY),
        (f"{os.getenv('CAI_TOP_P', '1.0')}", "white"),
    )
    env_panel = Panel(
        env_body,
        title=_quick_guide_subpanel_title("Environment Variables"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )

    shortcuts_body = quick_shortcuts_text(g, _GREY)
    shortcuts_panel = Panel(
        shortcuts_body,
        title=_quick_guide_subpanel_title("Quick shortcuts"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )

    # Curated essentials (aligned with session banner command hints + common discovery).
    essential_body = Text.assemble(
        ("  /agent", g),
        (" — agents · list, select, info\n", _GREY),
        ("  /model", g),
        (" — change AI model\n", _GREY),
        ("  /sessions", g),
        (" — last sessions list\n", _GREY),
        ("  /env", g),
        (" — env / settings\n", _GREY),
        ("  /help topics", g),
        (" — commands by category + /help <topic> (no env tables)\n", _GREY),
        ("  /exit", g),
        (" — leave CAI\n", _GREY),
    )
    essential_panel = Panel(
        essential_body,
        title=_quick_guide_subpanel_title("Essential commands"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )

    tips_body = Text.assemble(
        ("  • Use /help <topic> for topic-specific help (e.g. /help agent)\n", _GREY),
        (
            "  • Default entry agent is selection_agent; /help agent compares handoff routing vs orchestration_agent (BETA)\n",
            _GREY,
        ),
        ("  • Use bare /help for this guide plus full environment variable tables below\n", _GREY),
        ("  • Use /help commands for all commands\n", _GREY),
        ("  • Press ? on an empty line for input shortcuts (no Enter); ? still works with Enter\n", _GREY),
        ("  • Use /quickstart (aliases /qs, /quick) for the onboarding guide\n", _GREY),
        ("  • Use $ prefix for quick shell: $ ls", _GREY),
    )
    tips_panel = Panel(
        tips_body,
        title=_quick_guide_subpanel_title("Pro Tips"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )

    right_column = Group(
        context_tip,
        Text(""),
        env_panel,
        Text(""),
        shortcuts_panel,
        Text(""),
        essential_panel,
        Text(""),
        tips_panel,
    )

    # Keep workflows directly under the left reference block (do not push them down
    # to the full height of the right column).
    if _safe_console_width(console) >= _MIN_QUICK_GUIDE_SIDE_BY_SIDE:
        row = Table(
            show_header=False,
            box=None,
            expand=True,
            pad_edge=False,
            collapse_padding=True,
        )
        row.add_column(vertical="top", ratio=2, min_width=36)
        row.add_column(vertical="top", ratio=3, min_width=44)
        left_column = Group(help_ref, Text(""), workflow_text)
        row.add_row(left_column, right_column)
        body = row
    else:
        top = _quick_guide_top_row(console, help_ref, right_column)
        body = Group(top, Text(""), workflow_text)

    doc_url = _QUICK_GUIDE_COMMANDS_DOC_URL
    # Privacy notice spans full width below the two-column block, above the docs subtitle.
    _privacy_text = Text.assemble(
        (
            "CAI collects pseudonymized data to improve our research.Your privacy is protected in compliance with GDPR.",
            _GREY,
        ),
    )
    _panel_inner_w = max(40, _safe_console_width(console) - 4)
    privacy_footer = Align.center(_privacy_text, width=_panel_inner_w)
    # No blank line between main body and privacy; one newline after privacy before subtitle.
    panel_body = Group(body, privacy_footer, Text("\n"))
    console.print(
        Panel(
            panel_body,
            title=_quick_guide_outer_title(),
            title_align="center",
            subtitle=(
                f"[dim]Full command reference (all slash commands):[/dim] "
                f"[link={doc_url}]{doc_url}[/link]"
            ),
            subtitle_align="center",
            border_style=_CAI_GREEN,
            padding=(1, 1, 0, 1),
        )
    )
