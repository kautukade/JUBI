"""Direct httpx client for alias models, bypassing LiteLLM overhead.

Provides both streaming and non-streaming completion calls using httpx
directly against an OpenAI-compatible endpoint.  This eliminates the
LiteLLM library from the hot path for supported models.

Includes built-in retry with exponential backoff for transient errors
(429 rate-limit, 502/503/504 server errors, connection failures).
Inspired by CSI proxy's zero-dependency retry pattern.

Extracted from openai_chatcompletions.py [F][N] to reduce monolith size.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import random
import time
from typing import TYPE_CHECKING, Any, AsyncIterator, Literal, cast

import httpx
from openai import NOT_GIVEN, NotGiven
from openai.types.responses import Response

from cai.errors import LLMContextOverflow, LLMTimeout, LLMRateLimited, LLMProviderUnavailable
from cai.util.llm_api_base import resolve_llm_openai_compatible_base
from cai.util.wait_hints import sleep_with_retry_backoff_hint
from ..fake_id import FAKE_RESPONSES_ID

if TYPE_CHECKING:
    from ...model_settings import ModelSettings
    from openai.types.chat import ChatCompletionToolChoiceOptionParam

# ---------------------------------------------------------------------------
# Retry configuration — mirrors CSI proxy pattern (simple, effective)
# ---------------------------------------------------------------------------
_MAX_RETRIES = 3
_BASE_DELAY = 1.0   # seconds (CSI uses 1s fixed; we add exponential growth)
_MAX_DELAY = 30.0   # cap in seconds
# HTTP status codes that trigger automatic retry
_RETRYABLE_STATUS = {429, 502, 503, 504, 529}

_LOG = logging.getLogger(__name__)


def verbose_http_retries() -> bool:
    """When false (default), HTTP retry sleeps happen without console spam."""
    v = os.getenv("CAI_VERBOSE_LLM_RETRY", os.getenv("CAI_VERBOSE_HTTP_RETRY", "")).strip().lower()
    return v in ("1", "true", "yes", "on")


def _log_failed_completion_response(resp: httpx.Response, url: str) -> None:
    """Log provider error body — 400s are often schema/message validation (silent otherwise)."""
    try:
        body = (resp.text or "")[:4000]
    except Exception:
        body = ""
    one_line = body.replace("\n", " ").strip()
    if len(one_line) > 900:
        one_line = one_line[:900] + "…"
    _LOG.warning("HTTP %s from %s — %s", resp.status_code, url, one_line or "(empty body)")
    if os.getenv("CAI_HTTP_ERROR_BODY", "").lower() in ("1", "true", "yes"):
        print(f"\n\033[33m── HTTP {resp.status_code} response body (CAI_HTTP_ERROR_BODY) ──\033[0m")
        print(body or "(empty)")
        print()


def _retry_delay(attempt: int) -> float:
    """Exponential backoff with jitter: min(cap, base * 2^attempt) + jitter.

    Attempt 0: ~1s,  Attempt 1: ~2s,  Attempt 2: ~4s  (capped at 30s)
    """
    delay = min(_MAX_DELAY, _BASE_DELAY * (2 ** attempt)) + random.uniform(0, 1)
    return delay


def _extract_retry_after(resp: httpx.Response) -> float | None:
    """Extract Retry-After header value if present (seconds)."""
    header = resp.headers.get("retry-after")
    if header:
        try:
            return float(header)
        except ValueError:
            pass
    return None


def _build_413_details(url: str, body: Any) -> dict:
    """Diagnostics dict attached to ``LLMContextOverflow`` for HTTP 413 responses.

    The ``origin: "http_413"`` marker lets the REPL panel renderer
    discriminate against the limiter-origin overflow (which uses
    ``origin: "client_rate_limiter"``) without inspecting other keys.
    """
    try:
        body_bytes: int | None = len(json.dumps(body).encode("utf-8"))
    except Exception:
        body_bytes = None
    return {
        "origin": "http_413",
        "status_code": 413,
        "url": url,
        "body_bytes": body_bytes,
        "body_message_count": (
            len(body.get("messages") or []) if isinstance(body, dict) else None
        ),
        "body_tools_count": (
            len(body.get("tools") or []) if isinstance(body, dict) else None
        ),
    }


async def direct_httpx_completion(
    *,
    kwargs: dict,
    model_settings: "ModelSettings",
    tool_choice: "ChatCompletionToolChoiceOptionParam | NotGiven",
    stream: bool,
    parallel_tool_calls: bool,
    model_name: str,
    user_agent: str,
) -> Any:
    """Call the model API directly with httpx, skipping LiteLLM overhead.

    Includes automatic retry for transient errors (429, 5xx, connection
    failures) with exponential backoff — no sleep(60) or history pollution.

    Args:
        kwargs: Raw completion kwargs (messages, model, temperature, etc.).
        model_settings: Current model settings.
        tool_choice: Tool choice configuration.
        stream: Whether to return a streaming response.
        parallel_tool_calls: Allow parallel tool calls.
        model_name: Model name for response metadata.
        user_agent: User-Agent header value.

    Returns:
        For non-streaming: a ``litellm.ModelResponse``.
        For streaming: a tuple ``(Response, async_generator)``.
    """
    # kwargs api_base > resolver (CSI_CUSTOM_ENDPOINT / ALIAS_API_URL for qualifying model ids)
    _mid = str(kwargs.get("model") or model_name or "")
    api_base = kwargs.pop(
        "api_base",
        resolve_llm_openai_compatible_base(_mid),
    )
    api_key = kwargs.pop(
        "api_key",
        os.getenv("ALIAS_API_KEY", os.getenv("OPENAI_API_KEY", "sk-placeholder")),
    ).strip()
    kwargs.pop("custom_llm_provider", None)
    kwargs.pop("extra_headers", None)

    url = f"{api_base.rstrip('/')}/chat/completions"
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
        "User-Agent": user_agent,
    }

    # Build clean request body
    body = {k: v for k, v in kwargs.items() if v is not NOT_GIVEN and v is not None}

    if stream:
        client = httpx.AsyncClient(timeout=180.0)

        async def _stream_gen() -> AsyncIterator:
            """Streaming generator with built-in retry for connection/HTTP errors.

            Retries happen BEFORE yielding any chunks — once streaming starts,
            the response is committed (same pattern as CSI proxy).
            """
            try:
                last_error: Exception | None = None
                for attempt in range(_MAX_RETRIES + 1):
                    try:
                        async with client.stream("POST", url, json=body, headers=headers) as resp:
                            # Check for retryable HTTP status BEFORE streaming
                            if resp.status_code in _RETRYABLE_STATUS:
                                await resp.aread()  # drain response body
                                if attempt < _MAX_RETRIES:
                                    retry_after = _extract_retry_after(resp)
                                    delay = retry_after if retry_after else _retry_delay(attempt)
                                    if verbose_http_retries():
                                        print(f"⏳ HTTP {resp.status_code} — stream retry "
                                              f"{attempt + 1}/{_MAX_RETRIES} in {delay:.1f}s")
                                    else:
                                        _LOG.debug(
                                            "stream HTTP %s — retry %s/%s in %.1fs",
                                            resp.status_code, attempt + 1, _MAX_RETRIES, delay,
                                        )
                                    await sleep_with_retry_backoff_hint(delay)
                                    continue
                                # Retries exhausted — raise typed error
                                if resp.status_code == 429:
                                    raise LLMRateLimited(
                                        f"Rate limited (429) after {_MAX_RETRIES} retries from {url}",
                                        retry_after=_extract_retry_after(resp),
                                    )
                                raise LLMProviderUnavailable(
                                    f"Server error ({resp.status_code}) after {_MAX_RETRIES} retries from {url}"
                                )

                            # HTTP 413: request body exceeds gateway/proxy POST
                            # size cap. Mirror the non-stream branch and raise
                            # LLMContextOverflow so the REPL can give actionable
                            # guidance instead of a raw httpx traceback.
                            if resp.status_code == 413:
                                try:
                                    await resp.aread()
                                except Exception:
                                    pass
                                _log_failed_completion_response(resp, url)
                                raise LLMContextOverflow(
                                    f"Request body too large (413) for {url}",
                                    details=_build_413_details(url, body),
                                )

                            if not resp.is_success:
                                try:
                                    await resp.aread()
                                except Exception:
                                    pass
                                _log_failed_completion_response(resp, url)
                            resp.raise_for_status()

                            # Stream is good — parse SSE chunks and yield
                            buffer = ""
                            async for chunk_text in resp.aiter_text():
                                buffer += chunk_text
                                while "\n" in buffer:
                                    line, buffer = buffer.split("\n", 1)
                                    line = line.strip()
                                    if not line.startswith("data: "):
                                        continue
                                    data_str = line[6:]
                                    if data_str == "[DONE]":
                                        return
                                    try:
                                        data = json.loads(data_str)
                                        from litellm import ModelResponse as _MR
                                        from litellm.types.utils import StreamingChoices, Delta

                                        delta_data = data.get("choices", [{}])[0].get("delta", {})
                                        delta = Delta(
                                            role=delta_data.get("role"),
                                            content=delta_data.get("content"),
                                            tool_calls=delta_data.get("tool_calls"),
                                        )
                                        choices = [StreamingChoices(
                                            index=0,
                                            delta=delta,
                                            finish_reason=data.get("choices", [{}])[0].get("finish_reason"),
                                        )]
                                        usage = None
                                        usage_data = data.get("usage")
                                        if usage_data:
                                            from litellm.types.utils import Usage
                                            usage = Usage(
                                                prompt_tokens=usage_data.get("prompt_tokens", 0),
                                                completion_tokens=usage_data.get("completion_tokens", 0),
                                                total_tokens=usage_data.get("total_tokens", 0),
                                            )
                                        yield _MR(
                                            id=data.get("id", "chatcmpl-direct"),
                                            created=data.get("created", int(time.time())),
                                            model=data.get("model", model_name),
                                            choices=choices,
                                            usage=usage,
                                            object="chat.completion.chunk",
                                        )
                                    except json.JSONDecodeError:
                                        continue
                            return  # stream completed successfully

                    except httpx.ConnectError as e:
                        # Connection failure — retry (like CSI: proxyReq.on('error'))
                        last_error = e
                        if attempt < _MAX_RETRIES:
                            delay = _retry_delay(attempt)
                            if verbose_http_retries():
                                print(f"⏳ Connection error — retry "
                                      f"{attempt + 1}/{_MAX_RETRIES} in {delay:.1f}s: {e}")
                            else:
                                _LOG.debug(
                                    "connection error — retry %s/%s in %.1fs: %s",
                                    attempt + 1, _MAX_RETRIES, delay, e,
                                )
                            await sleep_with_retry_backoff_hint(delay)
                            continue
                        raise LLMProviderUnavailable(
                            f"Connection failed after {_MAX_RETRIES} retries: {e}"
                        ) from e

                    except httpx.HTTPStatusError as e:
                        # Non-retryable HTTP error (e.g. 400, 401)
                        last_error = e
                        if e.response.status_code in _RETRYABLE_STATUS and attempt < _MAX_RETRIES:
                            delay = _retry_delay(attempt)
                            if verbose_http_retries():
                                print(f"⏳ HTTP {e.response.status_code} — retry "
                                      f"{attempt + 1}/{_MAX_RETRIES} in {delay:.1f}s")
                            else:
                                _LOG.debug(
                                    "HTTP %s — retry %s/%s in %.1fs",
                                    e.response.status_code, attempt + 1, _MAX_RETRIES, delay,
                                )
                            await sleep_with_retry_backoff_hint(delay)
                            continue
                        if e.response.status_code == 429:
                            raise LLMRateLimited(
                                f"Rate limited (429) after retries from {url}"
                            ) from e
                        if e.response.status_code in (408, 504):
                            raise LLMTimeout(
                                f"Timeout ({e.response.status_code}) from {url}"
                            ) from e
                        raise

                # Should not reach here, but safety net
                if last_error:
                    raise last_error

            finally:
                await client.aclose()

        response_obj = Response(
            id=FAKE_RESPONSES_ID,
            created_at=time.time(),
            model=model_name,
            object="response",
            output=[],
            tool_choice="auto"
            if tool_choice is None or tool_choice == NOT_GIVEN
            else cast(Literal["auto", "required", "none"], tool_choice),
            top_p=model_settings.top_p,
            temperature=model_settings.temperature,
            tools=[],
            parallel_tool_calls=parallel_tool_calls or False,
        )
        return response_obj, _stream_gen()

    else:
        # Non-streaming with built-in retry (like CSI's attempt() pattern)
        async with httpx.AsyncClient(timeout=180.0) as client:
            last_error: Exception | None = None
            for attempt in range(_MAX_RETRIES + 1):
                try:
                    resp = await client.post(url, json=body, headers=headers)

                    if resp.status_code in _RETRYABLE_STATUS:
                        if attempt < _MAX_RETRIES:
                            retry_after = _extract_retry_after(resp)
                            delay = retry_after if retry_after else _retry_delay(attempt)
                            if verbose_http_retries():
                                print(f"⏳ HTTP {resp.status_code} — retry "
                                      f"{attempt + 1}/{_MAX_RETRIES} in {delay:.1f}s")
                            else:
                                _LOG.debug(
                                    "HTTP %s — retry %s/%s in %.1fs",
                                    resp.status_code, attempt + 1, _MAX_RETRIES, delay,
                                )
                            await sleep_with_retry_backoff_hint(delay)
                            continue
                        # Retries exhausted — raise typed error
                        if resp.status_code == 429:
                            raise LLMRateLimited(
                                f"Rate limited (429) after {_MAX_RETRIES} retries from {url}",
                                retry_after=_extract_retry_after(resp),
                            )
                        if resp.status_code in (408, 504):
                            raise LLMTimeout(
                                f"Timeout ({resp.status_code}) after {_MAX_RETRIES} retries from {url}"
                            )
                        raise LLMProviderUnavailable(
                            f"Server error ({resp.status_code}) after {_MAX_RETRIES} retries from {url}"
                        )

                    if resp.status_code in (408,):
                        raise LLMTimeout(f"Timeout ({resp.status_code}) from {url}")

                    # HTTP 413: request body exceeds gateway/proxy POST size cap.
                    # Not in _RETRYABLE_STATUS because resending the same body
                    # would just 413 again; surface as LLMContextOverflow so the
                    # REPL can show actionable guidance ("/compact" / "/flush")
                    # instead of a raw httpx traceback.
                    if resp.status_code == 413:
                        _log_failed_completion_response(resp, url)
                        raise LLMContextOverflow(
                            f"Request body too large (413) for {url}",
                            details=_build_413_details(url, body),
                        )

                    if not resp.is_success:
                        _log_failed_completion_response(resp, url)
                    resp.raise_for_status()
                    data = resp.json()
                    from litellm import ModelResponse as _MR
                    return _MR(**data)

                except httpx.ConnectError as e:
                    last_error = e
                    if attempt < _MAX_RETRIES:
                        delay = _retry_delay(attempt)
                        if verbose_http_retries():
                            print(f"⏳ Connection error — retry "
                                  f"{attempt + 1}/{_MAX_RETRIES} in {delay:.1f}s: {e}")
                        else:
                            _LOG.debug(
                                "connection error — retry %s/%s in %.1fs: %s",
                                attempt + 1, _MAX_RETRIES, delay, e,
                            )
                        await sleep_with_retry_backoff_hint(delay)
                        continue
                    raise LLMProviderUnavailable(
                        f"Connection failed after {_MAX_RETRIES} retries: {e}"
                    ) from e

            # Should not reach here
            if last_error:
                raise last_error
            raise LLMProviderUnavailable(f"All retries exhausted for {url}")
