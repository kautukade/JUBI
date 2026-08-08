"""Headless REPL: optional model-assisted hints after unexpected exceptions.

Environment:
  CAI_REPL_EXCEPTION_RECOVERY          — default on; set 0/false/off to disable.
  CAI_REPL_EXCEPTION_RECOVERY_ATTEMPTS  — max completion attempts (1–5, default 3).
  CAI_REPL_EXCEPTION_RECOVERY_AGENT     — after hints, offer to run the main agent to fix
                                         (default on); set 0/false/off to skip the prompt.
"""

from __future__ import annotations

import asyncio
import logging
import os
import traceback
from collections.abc import Callable
from typing import Any

logger = logging.getLogger(__name__)

_SYSTEM_PROMPT = """You are helping debug a CAI (cybersecurity agent CLI) runtime error.
The user hit an exception in the local REPL, not during a normal agent task.

Rules:
- Suggest concrete fixes (e.g. pip/uv install, env vars, config). Use markdown.
- Do NOT assume commands were run: nothing executes unless the user runs it or explicitly uses agent tools with their approval.
- If the fix needs elevated privileges, say so clearly.
- Be concise; prefer numbered steps.
- If the error is clearly transient (service overload), say to retry later instead of guessing.
"""


def is_recovery_enabled() -> bool:
    if os.getenv("CAI_TUI_MODE", "").strip().lower() in ("true", "1", "yes", "on"):
        return False
    v = os.getenv("CAI_REPL_EXCEPTION_RECOVERY", "1").strip().lower()
    return v not in ("0", "false", "no", "off")


def is_agent_recovery_offer_enabled() -> bool:
    v = os.getenv("CAI_REPL_EXCEPTION_RECOVERY_AGENT", "1").strip().lower()
    return v not in ("0", "false", "no", "off")


def recovery_max_attempts() -> int:
    try:
        return max(1, min(5, int(os.getenv("CAI_REPL_EXCEPTION_RECOVERY_ATTEMPTS", "3"))))
    except ValueError:
        return 3


def _iter_exception_chain(root: BaseException | None):
    if root is None:
        return
    seen: set[int] = set()
    todo: list[BaseException] = [root]
    while todo:
        exc = todo.pop()
        if id(exc) in seen:
            continue
        seen.add(id(exc))
        yield exc
        c = getattr(exc, "__cause__", None)
        if c is not None:
            todo.append(c)
        ctx = getattr(exc, "__context__", None)
        if ctx is not None and ctx is not exc:
            todo.append(ctx)


def should_skip_model_for_exception(e: BaseException) -> bool:
    """Do not call the model for overload / auth / guardrail / obvious infra errors."""
    from cai.errors import LLMContextOverflow, LLMProviderUnavailable, LLMRateLimited, LLMTimeout
    from cai.sdk.agents.exceptions import (
        InputGuardrailTripwireTriggered,
        MaxTurnsExceeded,
        OutputGuardrailTripwireTriggered,
        PriceLimitExceeded,
    )
    from litellm.exceptions import RateLimitError, Timeout as LitellmTimeout

    if isinstance(
        e,
        (
            LLMProviderUnavailable,
            LLMRateLimited,
            LLMTimeout,
            LLMContextOverflow,
            MaxTurnsExceeded,
            PriceLimitExceeded,
            OutputGuardrailTripwireTriggered,
            InputGuardrailTripwireTriggered,
            RateLimitError,
            LitellmTimeout,
            asyncio.TimeoutError,
            TimeoutError,
        ),
    ):
        return True

    try:
        import httpx

        for ex in _iter_exception_chain(e):
            if isinstance(ex, httpx.HTTPStatusError) and ex.response is not None:
                code = ex.response.status_code
                # 413: resending the same body in a model-recovery call would
                # 413 again — skip the recovery model call. Defensive fallback
                # in case a raw ``httpx.HTTPStatusError`` reaches here without
                # being mapped to ``LLMContextOverflow`` by the typed-error path
                # in ``httpx_client.py`` (the ``isinstance`` check above should
                # normally win for our own httpx wrapper).
                if code in (413, 429) or code >= 500:
                    return True
    except Exception:
        pass

    low = str(e).lower()
    for needle in (
        "503",
        "502",
        "504",
        "429",
        "rate limit",
        "too many requests",
        "gateway timeout",
        "service unavailable",
        "bad gateway",
    ):
        if needle in low:
            return True

    return False


def _resolve_model_name(agent: Any, cfg: Any) -> str:
    try:
        if agent is not None and hasattr(agent, "model") and hasattr(agent.model, "model"):
            m = getattr(agent.model, "model", None)
            if isinstance(m, str) and m.strip():
                return m.strip()
    except Exception:
        pass
    return str(getattr(cfg, "model", "") or "alias1")


def _litellm_kwargs_for_model(model_name: str, cfg: Any) -> dict[str, Any]:
    kwargs: dict[str, Any] = {}
    mn = model_name.lower()
    if mn == "alias2-mini" or ("alias" in mn and "alias0.5" not in mn):
        kwargs["api_base"] = "https://api.aliasrobotics.com:666/"
        kwargs["custom_llm_provider"] = "openai"
        key = (
            getattr(cfg, "alias_api_key", None)
            or os.getenv("ALIAS_API_KEY", "")
            or "sk-alias-1234567890"
        )
        kwargs["api_key"] = str(key).strip()
    return kwargs


def _temperature_for_model(model_name: str) -> float:
    ml = model_name.lower()
    if any(x in ml for x in ("gpt-5", "o1", "o3")):
        return 1.0
    return 0.2


def format_exception_brief(e: BaseException, *, limit_tb_lines: int = 40) -> str:
    lines = traceback.format_exception(type(e), e, e.__traceback__)
    text = "".join(lines)
    if text.count("\n") > limit_tb_lines:
        head = "\n".join(text.splitlines()[:limit_tb_lines])
        return head + f"\n… ({len(text) - len(head)} more chars truncated)"
    return text


async def _acompletion_once(model_name: str, user_content: str, cfg: Any) -> str | None:
    import litellm

    kwargs: dict[str, Any] = {
        "model": model_name,
        "messages": [
            {"role": "system", "content": _SYSTEM_PROMPT},
            {"role": "user", "content": user_content},
        ],
        "temperature": _temperature_for_model(model_name),
        "max_tokens": 1500,
        "stream": False,
    }
    kwargs.update(_litellm_kwargs_for_model(model_name, cfg))
    timeout_s = float(os.getenv("CAI_REPL_EXCEPTION_RECOVERY_TIMEOUT", "50"))

    resp = await asyncio.wait_for(litellm.acompletion(**kwargs), timeout=timeout_s)
    if not resp or not getattr(resp, "choices", None):
        return None
    msg = resp.choices[0].message
    content = getattr(msg, "content", None)
    if isinstance(content, str) and content.strip():
        return content.strip()
    return None


def build_recovery_agent_user_message(hint: str, brief: str) -> str:
    """Single user turn for the main agent after explicit user authorization."""
    return (
        "The CAI REPL hit an error. I authorized you to try to fix the local environment.\n\n"
        "## Exception (traceback excerpt)\n\n"
        f"```\n{brief}\n```\n\n"
        "## Diagnostic suggestions (from a separate model call)\n\n"
        f"{hint}\n\n"
        "## Your task\n\n"
        "- Only address what is needed to clear this error (e.g. install a missing Python package, "
        "adjust config, fix imports). Do not pivot to unrelated security work.\n"
        "- Use your tools as appropriate. I understand I may need to approve sudo/password prompts.\n"
        "- Summarize briefly what you did or what I must still do manually.\n"
    )


def try_recover_with_model(
    e: BaseException,
    agent: Any,
    console: Any,
    cfg: Any,
    *,
    recovery_agent_runner: Callable[[str, str], None] | None = None,
) -> None:
    """Print intro + exception, then ask the model (up to N attempts)."""
    from cai.util.streaming import CAI_GREEN, _CAI_MD_THEME
    from rich.markdown import Markdown
    from rich.panel import Panel

    model_name = _resolve_model_name(agent, cfg)
    brief = format_exception_brief(e)
    user_blob = (
        "The following exception was raised in the CAI headless REPL main loop.\n\n"
        f"```\n{brief}\n```\n\n"
        "What should the user do next? Remember: suggest only; do not claim anything was executed."
    )

    # Accent matches sudo flat line “Authentication Required” (Rich ``bold yellow`` → amber/orange on many terminals).
    _warn_open = "[bold yellow]"
    _warn_close = "[/bold yellow]"
    console.print()
    console.print(
        Panel.fit(
            f"{_warn_open}An unexpected error occurred in the REPL.{_warn_close}\n"
            "[dim]Showing the error below, then asking the configured model for recovery "
            "suggestions. Nothing runs automatically—review any commands before executing them.[/dim]",
            border_style="yellow",
        )
    )
    console.print()
    console.print(
        Panel(
            brief,
            title="[dim]Exception[/dim]",
            border_style="dim",
            style="dim",
            expand=False,
        )
    )

    attempts = recovery_max_attempts()
    last_err: str | None = None
    for i in range(attempts):
        try:
            hint = asyncio.run(_acompletion_once(model_name, user_blob, cfg))
            if hint:
                console.print()
                console.push_theme(_CAI_MD_THEME)
                try:
                    console.print(
                        Panel(
                            Markdown(hint),
                            title=f"[bold {CAI_GREEN}]Model suggestions[/bold {CAI_GREEN}]",
                            border_style=CAI_GREEN,
                            expand=True,
                        )
                    )
                finally:
                    console.pop_theme()
                if (
                    recovery_agent_runner is not None
                    and is_agent_recovery_offer_enabled()
                ):
                    try:
                        from rich.prompt import Confirm

                        if Confirm.ask(
                            f"\n{_warn_open}Let the CAI agent try to fix this now?{_warn_close} "
                            "[dim](You may be asked to approve shell commands or sudo/password.)[/dim]",
                            default=False,
                            console=console,
                        ):
                            recovery_agent_runner(hint, brief)
                    except Exception:
                        logger.warning("recovery agent authorization prompt failed", exc_info=True)
                return
            last_err = "empty response"
        except asyncio.TimeoutError:
            last_err = "timeout"
        except Exception as ex:
            last_err = str(ex)
            logger.warning("exception_recovery attempt %s/%s failed: %s", i + 1, attempts, ex)

    console.print(
        f"\n[dim]The model could not produce recovery suggestions after {attempts} attempt(s)"
        + (f" (last issue: {last_err})." if last_err else ".")
        + "[/dim]"
    )
    console.print(
        "[dim]You can retry the command, fix the environment manually, or set "
        "CAI_DEBUG=2 for full logs.[/dim]\n"
    )
