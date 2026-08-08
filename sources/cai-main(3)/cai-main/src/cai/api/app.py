"""ASGI application factory for the CAI API backend."""

from __future__ import annotations

import asyncio
import importlib.metadata
import os
import secrets
from typing import List

from fastapi import Depends, FastAPI, HTTPException, Request, Security, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
import logging
from starlette.middleware.base import BaseHTTPMiddleware
from fastapi.security import APIKeyHeader
import json as _json

from .commands import CommandExecutor
from .schemas import (
    AuthAddIpRequest,
    AuthAddIpResponse,
    AuthLoginRequest,
    AuthLoginResponse,
    AuthRegisterRequest,
    AgentsResponse,
    CancelTaskResponse,
    CommandMetadataModel,
    CommandRequest,
    CommandResponse,
    CommandsResponse,
    CreateSessionRequest,
    InterruptResponse,
    ModelInfoModel,
    ModelsResponse,
    HealthResponse,
    InferenceRequest,
    InferenceResponse,
    ReloadRequest,
    RunResultPayload,
    SessionDetailModel,
    SessionHistoryResponse,
    SessionSummaryModel,
    SessionsResponse,
    # UXSummaryRequest,  # no longer used
    # TitleRequest,      # removed with UX title endpoint
    # TitleResponse,     # removed with UX title endpoint
    # SummarizeRequest,  # removed with UX summarize endpoint
    # SummarizeResponse, # removed with UX summarize endpoint
    FinalMessageRequest,
    # Lite, sessionless UX endpoints
    UXSummarizeLiteRequest,
    UXSummarizeLiteResponse,
    UXTitleLiteRequest,
    UXTitleLiteResponse,
)
from .auth import AuthManager, InvalidCredentialsError, UserAlreadyExistsError
from .sessions import SessionManager, SessionNotFoundError, summarize_run_result
from .streaming import sse_stream_for_run, sse_stream_via_hooks, sse_stream_tokens_for_run

# Py3.11+: ExceptionGroup/BaseExceptionGroup is builtin; for older Python fall back
try:  # pragma: no cover - compatibility
    BaseExcGroup = BaseExceptionGroup  # type: ignore[name-defined]
except NameError:  # pragma: no cover - python <3.11
    try:
        from exceptiongroup import BaseExceptionGroup as BaseExcGroup  # type: ignore
    except Exception:  # pragma: no cover - backport not installed
        BaseExcGroup = tuple  # sentinel so isinstance(exc, BaseExcGroup) is False


def _get_version() -> str:
    try:
        return importlib.metadata.version("cai-framework")
    except importlib.metadata.PackageNotFoundError:  # pragma: no cover - fallback for editable installs
        return os.getenv("CAI_VERSION", "dev")


def _format_exc(exc: Exception, max_sub: int = 3) -> str:
    """Return a compact, human-friendly summary of an exception or ExceptionGroup."""
    try:
        if isinstance(exc, BaseExcGroup):  # py3.11+
            msgs = []
            for i, sub in enumerate(exc.exceptions[:max_sub]):
                msgs.append(_format_exc(sub, max_sub=max_sub))
            extra = ""
            if len(exc.exceptions) > max_sub:
                extra = f" (+{len(exc.exceptions) - max_sub} more)"
            return f"{exc.__class__.__name__}: [{'; '.join(msgs)}]{extra}"
        return f"{exc.__class__.__name__}: {exc}"
    except Exception:
        return str(exc)


def create_cai_api_app(
    *,
    session_manager: SessionManager | None = None,
    command_executor: CommandExecutor | None = None,
) -> FastAPI:
    os.environ.setdefault("CAI_API_MODE", "true")

    app = FastAPI(
        title="CAI API",
        version=_get_version(),
        description="Backend HTTP state for the Cybersecurity AI Framework",
        docs_url="/api/docs",
        openapi_url="/api/openapi.json",
        redoc_url="/api/redoc",
    )

    cors_origins = os.getenv("CAI_API_CORS", "*")
    if cors_origins:
        origins: List[str]
        if cors_origins.strip() == "*":
            origins = ["*"]
        else:
            origins = [origin.strip() for origin in cors_origins.split(",") if origin.strip()]
        app.add_middleware(
            CORSMiddleware,
            allow_origins=origins,
            allow_methods=["*"],
            allow_headers=["*"],
            allow_credentials=True,
        )

    app.state.session_manager = session_manager or SessionManager()
    app.state.command_executor = command_executor or CommandExecutor()
    app.state.auth_manager = AuthManager()

    api_key_header_name = os.getenv("CAI_API_KEY_HEADER", "X-CAI-API-Key")
    api_key_scheme = APIKeyHeader(name=api_key_header_name, auto_error=False)

    def _require_api_key(
        request: Request,
        api_key: str | None = Security(api_key_scheme),
    ) -> None:
        """Validate either the static API key or a session token.

        For backwards compatibility, authentication is considered disabled
        unless a root API key is configured via ``ALIAS_API_KEY`` or
        ``CAI_API_KEY``. When a root key is present, we accept:

        - The exact root key value, or
        - Any valid ``session_token`` previously issued by :class:`AuthManager`.
        """
        # Prefer the client's ALIAS_API_KEY for authentication; fallback to CAI_API_KEY only if needed.
        root_key = os.getenv("ALIAS_API_KEY") or os.getenv("CAI_API_KEY")
        log_auth = os.getenv("CAI_API_LOG_AUTH", "false").lower() in ("1", "true", "yes")

        # If no root key is configured, keep the original "dev mode"
        # behavior where the API does not enforce authentication.
        if not root_key:
            return

        if not api_key:
            if log_auth:
                logging.getLogger("uvicorn.error").info("Auth failed: missing header %s", api_key_header_name)
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid or missing API key")

        # Path 1: static root API key.
        if secrets.compare_digest(api_key, root_key):
            return

        # Path 2: per-device session token managed by AuthManager.
        auth_manager: AuthManager | None = getattr(request.app.state, "auth_manager", None)
        if auth_manager is not None and auth_manager.validate_session_token(api_key):
            return

        if log_auth:
            logging.getLogger("uvicorn.error").info("Auth failed: invalid token length=%d", len(api_key))
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid or missing API key",
        )

    # Optional request logging middleware (method, path, sanitized headers, truncated body)
    if os.getenv("CAI_API_LOG_REQUESTS", "false").lower() in ("1", "true", "yes"):
        class RequestLogMiddleware(BaseHTTPMiddleware):
            async def dispatch(self, request, call_next):  # type: ignore[override]
                logger = logging.getLogger("uvicorn.error")
                try:
                    body_bytes = await request.body()
                    # Attempt to re-inject body for downstream handlers
                    try:
                        request._body = body_bytes  # type: ignore[attr-defined]
                    except Exception:
                        pass
                except Exception:
                    body_bytes = b""

                # Sanitize headers
                headers = dict(request.headers)
                for h in (api_key_header_name.lower(), "authorization"):
                    if h in headers:
                        val = headers[h]
                        headers[h] = f"***{len(val)}***"

                preview = body_bytes.decode("utf-8", errors="replace")
                if len(preview) > 2000:
                    preview = preview[:2000] + "...<truncated>"

                logger.info("REQ %s %s headers=%s body=%s",
                            request.method, request.url.path, headers, preview)

                response = await call_next(request)
                logger.info("RES %s %s status=%s content-type=%s",
                            request.method, request.url.path, getattr(response, "status_code", "?"), response.headers.get("content-type"))
                return response

        app.add_middleware(RequestLogMiddleware)

    def _session_manager_dependency(request: Request) -> SessionManager:
        return request.app.state.session_manager

    def _command_executor_dependency(request: Request) -> CommandExecutor:
        return request.app.state.command_executor

    def _auth_manager_dependency(request: Request) -> AuthManager:
        return request.app.state.auth_manager

    @app.get("/api/v1/health", response_model=HealthResponse, tags=["meta"])
    def healthcheck() -> HealthResponse:
        return HealthResponse(status="ok", version=_get_version())
    
    @app.get("/api/tags", tags=["meta"])
    def get_tags() -> Dict[str, List[str]]:
        """Return available API tags for discovery."""
        return {
            "tags": ["meta", "auth", "catalog", "sessions", "inference", "commands", "ux"]
        }

    # Compatibility alias: some clients probe /api/v1/tags (matching Ollama-style
    # discovery). Serve the same payload to avoid 404s during environment checks.
    @app.get("/api/v1/tags", include_in_schema=False, tags=["meta"])
    def get_tags_v1() -> Dict[str, List[str]]:
        return get_tags()

    # ------------------------------------------------------------------
    # Authentication endpoints
    # ------------------------------------------------------------------

    @app.post(
        "/api/v1/auth/add-ip",
        response_model=AuthAddIpResponse,
        tags=["auth"],
    )
    def auth_add_ip(
        request: Request,
        payload: AuthAddIpRequest,
        auth_manager: AuthManager = Depends(_auth_manager_dependency),
        _: None = Depends(_require_api_key),
    ) -> AuthAddIpResponse:
        """Flow 1: create random credentials and a session token for a device IP.

        Admin-only: requires the root API key. The endpoint provisions a new
        device, so an unauthenticated caller could mint themselves a session
        token and bypass auth entirely.
        """
        ip = payload.ip
        if not ip and request.client:
            ip = request.client.host
        if not ip:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="IP address is required",
            )

        user, plain_password, session = auth_manager.create_random_user_and_session_for_ip(ip)
        return AuthAddIpResponse(username=user.username, password=plain_password, session_token=session.token)

    @app.post(
        "/api/v1/auth/register",
        status_code=status.HTTP_201_CREATED,
        tags=["auth"],
    )
    def auth_register(
        payload: AuthRegisterRequest,
        auth_manager: AuthManager = Depends(_auth_manager_dependency),
        _: None = Depends(_require_api_key),
    ) -> dict:
        """Create a new user with explicit username/password.

        Admin-only: requires the root API key. Without this gate, an
        unauthenticated caller could plant credentials and then log in to
        obtain a valid session token.
        """
        try:
            user = auth_manager.create_user(payload.username, payload.password)
        except UserAlreadyExistsError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail=str(exc),
            ) from exc
        except ValueError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail=str(exc),
            ) from exc
        return {"username": user.username}

    @app.post(
        "/api/v1/auth/login",
        response_model=AuthLoginResponse,
        tags=["auth"],
    )
    def auth_login(
        request: Request,
        payload: AuthLoginRequest,
        auth_manager: AuthManager = Depends(_auth_manager_dependency),
    ) -> AuthLoginResponse:
        """Flow 2: login with username/password and issue a session token."""
        device_ip = payload.ip
        if not device_ip and request.client:
            device_ip = request.client.host

        try:
            session = auth_manager.login(payload.username, payload.password, device_ip=device_ip)
        except InvalidCredentialsError as exc:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Invalid username or password",
            ) from exc

        return AuthLoginResponse(session_token=session.token)

    # Removed helper functions used by deprecated UX endpoints (summarize/title)
    # - _history_for_ux
    # - _extract_text
    # - _title_from_history

    @app.get(
        "/api/v1/agents",
        response_model=AgentsResponse,
        tags=["catalog"],
        dependencies=[Depends(_require_api_key)],
    )
    def list_agents() -> AgentsResponse:
        from cai.agents import get_available_agents

        agents = []
        for name, agent in get_available_agents().items():
            a_type = "pattern" if hasattr(agent, "pattern") else "agent"
            pattern_type = getattr(agent, "pattern_type", None) or getattr(agent, "pattern", None)
            tools = []
            try:
                if hasattr(agent, "tools") and agent.tools:
                    for t in agent.tools:
                        tool_name = getattr(t, "name", None)
                        tool_desc = getattr(t, "description", None)
                        if tool_name:
                            tools.append({"name": tool_name, "description": tool_desc})
            except Exception:
                pass

            agents.append(
                {
                    "name": getattr(agent, "name", name),
                    "description": getattr(agent, "description", None),
                    "type": a_type,
                    "pattern_type": str(pattern_type) if pattern_type else None,
                    "tools": tools,
                }
            )
        from .schemas import AgentMetadataModel
        return AgentsResponse(agents=[AgentMetadataModel.model_validate(a) for a in agents])

    @app.get(
        "/api/v1/models",
        response_model=ModelsResponse,
        tags=["catalog"],
        dependencies=[Depends(_require_api_key)],
    )
    def list_models() -> ModelsResponse:
        # Merge predefined models (with provider/category/description) and pricing.json capabilities when available
        from cai.repl.commands.model import get_all_predefined_models
        from cai.util import get_pricings_dir
        import json as _json
        import os as _os
        from pathlib import Path as _Path

        predefined = get_all_predefined_models()  # [{name,provider,category,description,input_cost,output_cost}]
        models_by_name: dict[str, dict] = {m["name"]: dict(m) for m in predefined}

        # Load pricing.json if present and attach as pricing
        try:
            pricing_path = get_pricings_dir() / "pricing.json"
            if pricing_path.exists():
                with open(pricing_path, "r", encoding="utf-8") as fh:
                    pricing = _json.load(fh)  # name -> pricing dict
                for mname, pdata in pricing.items():
                    entry = models_by_name.setdefault(mname, {"name": mname})
                    # Map pricing fields to a nested pricing object; keep original costs too
                    entry["pricing"] = {
                        "input_cost_per_token": pdata.get("input_cost_per_token"),
                        "output_cost_per_token": pdata.get("output_cost_per_token"),
                        "max_tokens": pdata.get("max_tokens"),
                        "max_input_tokens": pdata.get("max_input_tokens"),
                        "max_output_tokens": pdata.get("max_output_tokens"),
                        "supports_function_calling": pdata.get("supports_function_calling"),
                        "supports_vision": pdata.get("supports_vision"),
                        "supports_response_schema": pdata.get("supports_response_schema"),
                        "supports_tool_choice": pdata.get("supports_tool_choice"),
                    }
        except Exception:
            pass

        # Normalize to Pydantic model
        result_models = []
        for m in models_by_name.values():
            result_models.append(
                {
                    "name": m.get("name"),
                    "provider": m.get("provider"),
                    "category": m.get("category"),
                    "description": m.get("description"),
                    "input_cost": m.get("input_cost"),
                    "output_cost": m.get("output_cost"),
                    "pricing": m.get("pricing"),
                }
            )
        result_models.sort(key=lambda x: (x["provider"] or "zzz", x["name"] or ""))
        return ModelsResponse(models=result_models)

    @app.get(
        "/api/v1/commands",
        response_model=CommandsResponse,
        tags=["commands"],
        dependencies=[Depends(_require_api_key)],
    )
    def list_commands(executor: CommandExecutor = Depends(_command_executor_dependency)) -> CommandsResponse:
        commands = [CommandMetadataModel.model_validate(cmd.__dict__) for cmd in executor.describe_commands()]
        return CommandsResponse(commands=commands)

    @app.post(
        "/api/v1/commands/{command_name}",
        response_model=CommandResponse,
        tags=["commands"],
        dependencies=[Depends(_require_api_key)],
    )
    def run_command(
        command_name: str,
        payload: CommandRequest,
        executor: CommandExecutor = Depends(_command_executor_dependency),
    ) -> CommandResponse:
        result = executor.run(command_name, payload.args, payload.auto_correct)
        return CommandResponse(
            handled=result.handled,
            suggested_command=result.suggested_command,
            stdout=result.stdout,
            stderr=result.stderr,
            exit_code=result.exit_code,
        )

    @app.post(
        "/api/v1/sessions",
        response_model=SessionDetailModel,
        status_code=status.HTTP_201_CREATED,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def create_session(
        payload: CreateSessionRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> SessionDetailModel:
        session = manager.create_session(
            agent_name=payload.agent,
            model_name=payload.model,
            stateful=payload.stateful,
            metadata=payload.metadata,
        )
        return SessionDetailModel.model_validate(session.to_detail())

    @app.get(
        "/api/v1/sessions",
        response_model=SessionsResponse,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def list_sessions(manager: SessionManager = Depends(_session_manager_dependency)) -> SessionsResponse:
        summaries = [SessionSummaryModel.model_validate(summary.__dict__) for summary in manager.list_sessions()]
        return SessionsResponse(sessions=summaries)

    @app.get(
        "/api/v1/sessions/{session_id}",
        response_model=SessionHistoryResponse,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def get_session(
        session_id: str,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> SessionHistoryResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        return SessionHistoryResponse(session=SessionDetailModel.model_validate(session.to_detail()))

    @app.delete(
        "/api/v1/sessions/{session_id}",
        status_code=status.HTTP_204_NO_CONTENT,
        response_model=None,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def delete_session(session_id: str, manager: SessionManager = Depends(_session_manager_dependency)) -> None:
        try:
            manager.delete_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc

    @app.post(
        "/api/v1/sessions/{session_id}/reset",
        response_model=SessionDetailModel,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def reset_session(
        session_id: str,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> SessionDetailModel:
        try:
            session = manager.reset_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        return SessionDetailModel.model_validate(session.to_detail())

    @app.post(
        "/api/v1/sessions/{session_id}/cancel",
        response_model=CancelTaskResponse,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    async def cancel_task(
        session_id: str,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> CancelTaskResponse:
        """Cancel/interrupt the currently running task in a session (best-effort)."""
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        
        # First signal cancellation
        signalled = session.interrupt()
        # Then wait briefly for the task to exit
        waited = await session.interrupt_and_wait()

        if signalled or waited:
            return CancelTaskResponse(
                cancelled=True,
                message=f"Task in session {session_id} has been cancelled"
            )
        else:
            return CancelTaskResponse(
                cancelled=False,
                message=f"No running task found in session {session_id}"
            )

    @app.post(
        "/api/v1/sessions/{session_id}/messages",
        response_model=InferenceResponse,
        tags=["inference"],
        dependencies=[Depends(_require_api_key)],
    )
    async def send_message(
        session_id: str,
        payload: InferenceRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> InferenceResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        try:
            result = await session.run_inference(payload.input, context=payload.context, max_turns=payload.max_turns)
        except asyncio.CancelledError:
            # Task was cancelled - return a specific error
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail="Task was cancelled by user request"
            )
        except Exception as exc:  # pragma: no cover - propagate unexpected execution errors
            raise HTTPException(
                status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
                detail=f"Agent execution failed: {exc}",
            ) from exc

        summary = SessionSummaryModel.model_validate(session.to_summary().__dict__)
        run_payload = RunResultPayload.model_validate(summarize_run_result(result))
        return InferenceResponse(session=summary, result=run_payload)

    @app.get(
        "/api/v1/sessions/{session_id}/history",
        response_model=SessionHistoryResponse,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def get_history(
        session_id: str,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> SessionHistoryResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        return SessionHistoryResponse(session=SessionDetailModel.model_validate(session.to_detail()))

    @app.post(
        "/api/v1/sessions/{session_id}/interrupt",
        response_model=InterruptResponse,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def interrupt_session(session_id: str, manager: SessionManager = Depends(_session_manager_dependency)) -> InterruptResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        interrupted = session.interrupt()
        return InterruptResponse(interrupted=interrupted)

    @app.post(
        "/api/v1/sessions/{session_id}/reload",
        response_model=SessionDetailModel,
        tags=["sessions"],
        dependencies=[Depends(_require_api_key)],
    )
    def reload_session(
        session_id: str,
        payload: ReloadRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> SessionDetailModel:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc
        session.interrupt()
        session.reload(preserve_history=payload.preserve_history)
        return SessionDetailModel.model_validate(session.to_detail())

    # Removed deprecated _summarize_activity helper used only by UX summarize endpoint

    # Removed UX summarize endpoint

    # Removed UX title endpoint

    @app.post(
        "/api/v1/sessions/{session_id}/messages/stream",
        tags=["inference"],
        response_class=StreamingResponse,
        dependencies=[Depends(_require_api_key)],
    )
    async def send_message_stream(
        session_id: str,
        payload: InferenceRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> StreamingResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc

        # Compose input with history if stateful; reuse SessionState internals
        composed = session._compose_input(payload.input)  # noqa: SLF001 (intentional reuse)

        # API requirement: underlying OpenAI chat completions must NOT stream.
        # Implement streaming via RunHooks (non-streaming model calls) instead of Runner.run_streamed.
        async def _gen():
            async for chunk in sse_stream_via_hooks(
                session.agent,
                composed,
                context=payload.context,
                max_turns=payload.max_turns,
                session=session,
            ):
                yield chunk
            # After finishing, persist history/state so subsequent calls see full context.
            # We can't rely on agent.model.message_history (not all models expose it),
            # so we run a non-streaming pass to reconstruct the conversation input list.
            try:
                from cai.sdk.agents.run import Runner, DEFAULT_MAX_TURNS as _DEF_TURNS

                recon_result = await Runner.run(
                    session.agent,
                    composed,
                    context=payload.context,
                    max_turns=int(payload.max_turns) if isinstance(payload.max_turns, (int, float)) else _DEF_TURNS,
                )
                session.history = recon_result.to_input_list()
            except Exception:
                # Best-effort only; if it fails we keep the previous history.
                pass

        headers = {
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        }
        return StreamingResponse(_gen(), media_type="text/event-stream", headers=headers)

    @app.post(
        "/api/v1/sessions/{session_id}/messages/stream_tokens",
        tags=["inference"],
        response_class=StreamingResponse,
        dependencies=[Depends(_require_api_key)],
    )
    async def send_message_stream_tokens(
        session_id: str,
        payload: InferenceRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> StreamingResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc

        composed = session._compose_input(payload.input)  # noqa: SLF001

        # Use Runner.run_streamed to enable provider token/delta events; this endpoint is explicit token-level streaming
        from cai.sdk.agents.run import Runner, DEFAULT_MAX_TURNS

        # Optionally spin up MCP SSE servers provided in the request (ephemeral for this call)
        attached_servers = []
        prev_mcp_list = None
        if getattr(payload, "mcp_sse", None):
            try:
                from cai.sdk.agents.mcp.server import MCPServerSse, MCPServerSseParams
                default_timeout = float(os.getenv("CAI_MCP_SSE_TIMEOUT", "5"))
                default_read_timeout = float(os.getenv("CAI_MCP_SSE_READ_TIMEOUT", str(60 * 5)))
                # Connect and attach before kicking off the run
                for cfg in payload.mcp_sse or []:
                    try:
                        params: MCPServerSseParams = {"url": cfg.url}
                        if cfg.headers:
                            params["headers"] = dict(cfg.headers)
                        if cfg.timeout is not None:
                            params["timeout"] = float(cfg.timeout)
                        else:
                            params["timeout"] = default_timeout
                        if cfg.sse_read_timeout is not None:
                            params["sse_read_timeout"] = float(cfg.sse_read_timeout)
                        else:
                            params["sse_read_timeout"] = default_read_timeout
                        server = MCPServerSse(params, name=cfg.name or f"sse:{cfg.url}")
                        await server.connect()
                        attached_servers.append(server)
                    except Exception as exc:
                        logging.getLogger("uvicorn.error").warning(
                            "Failed to init MCP SSE server %s: %s", getattr(cfg, "url", "?"), _format_exc(exc)
                        )
                # Attach to agent
                if attached_servers:
                    try:
                        prev_mcp_list = list(getattr(session.agent, "mcp_servers", []))
                        session.agent.mcp_servers = prev_mcp_list + attached_servers
                    except Exception:
                        prev_mcp_list = None
            except Exception as exc:
                logging.getLogger("uvicorn.error").warning("MCP SSE setup failed: %s", _format_exc(exc))

        run = Runner.run_streamed(
            session.agent,
            composed,
            context=payload.context,
            max_turns=payload.max_turns or DEFAULT_MAX_TURNS,
        )

        # Register running task for interruption (wrap stream into a task)
        async def _gen():
            try:
                async for chunk in sse_stream_tokens_for_run(run, session=session):
                    yield chunk
            finally:
                try:
                    session.set_running_task(None)
                except Exception:
                    pass
                # Persist history after completion for stateful sessions
                try:
                    if session.stateful:
                        session.history = run.to_input_list()
                except Exception:
                    pass
                # Detach and cleanup ephemeral MCP servers
                if attached_servers:
                    try:
                        if prev_mcp_list is not None:
                            session.agent.mcp_servers = prev_mcp_list
                        else:
                            # Best effort remove appended servers
                            existing = list(getattr(session.agent, "mcp_servers", []))
                            session.agent.mcp_servers = [s for s in existing if s not in attached_servers]
                    except Exception:
                        pass
                    # Cleanup
                    for srv in attached_servers:
                        try:
                            await srv.cleanup()
                        except asyncio.CancelledError:
                            # Expected when the connection is closed
                            pass
                        except asyncio.TimeoutError:
                            # Expected for SSE connections
                            pass
                        except Exception:
                            pass

        # Track the underlying asyncio task of the streaming impl
        try:
            # run._run_impl_task is created inside Runner.run_streamed
            session.set_running_task(run._run_impl_task)  # type: ignore[attr-defined]
        except Exception:
            pass

        headers = {
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        }
        return StreamingResponse(_gen(), media_type="text/event-stream", headers=headers)

    @app.post(
        "/api/v1/sessions/{session_id}/ux/final_message/stream_tokens",
        tags=["ux"],
        response_class=StreamingResponse,
        dependencies=[Depends(_require_api_key)],
    )
    async def ux_final_message_stream_tokens(
        session_id: str,
        payload: FinalMessageRequest,
        manager: SessionManager = Depends(_session_manager_dependency),
    ) -> StreamingResponse:
        try:
            session = manager.get_session(session_id)
        except SessionNotFoundError as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Session not found") from exc

        # Build context for UX agent from provided steps or session steps, and optional history
        steps = payload.steps if payload.steps else (session.last_steps or [])
        context_blob = {"session_id": session.id, "steps": steps}
        if payload.messages:
            context_blob["history"] = payload.messages

        # Prepare input for UX agent: first CONTEXT, then the user prompt
        input_items = [
            {"role": "user", "content": f"CONTEXT: {context_blob}"},
            {"role": "user", "content": payload.prompt},
        ]

        from cai.agents import get_agent_by_name
        from cai.sdk.agents.run import Runner, DEFAULT_MAX_TURNS
        # UX agent was removed; if requested, respond with a clear error
        try:
            ux_agent = get_agent_by_name("user_experience_agent")
        except Exception as exc:  # pragma: no cover - disabled feature path
            raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="UX agent is disabled") from exc
        run = Runner.run_streamed(
            ux_agent,
            input_items,
            max_turns=payload.max_turns or DEFAULT_MAX_TURNS,
        )

        async def _gen():
            try:
                async for chunk in sse_stream_tokens_for_run(run, session=session):
                    yield chunk
            finally:
                try:
                    session.set_running_task(None)
                except Exception:
                    pass

        try:
            session.set_running_task(run._run_impl_task)  # type: ignore[attr-defined]
        except Exception:
            pass

        headers = {
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        }
        return StreamingResponse(_gen(), media_type="text/event-stream", headers=headers)

    # -----------------------------
    # Lite UX endpoints (no sessions)
    # -----------------------------

    def _run_litellm_title_summary(
        *,
        messages: list[dict] | None = None,
        steps: list[dict] | None = None,
        title_hint: str | None = None,
        max_len: int = 100,
    ) -> tuple[str, str]:
        """Call LiteLLM with alias1 and a single tool call to produce title and summary.

        Returns (title, summary).
        """
        try:
            import os as _os
            import litellm as _litellm
        except Exception as exc:  # pragma: no cover - import failure
            raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail=f"LiteLLM not available: {exc}") from exc

        sys_prompt = (
            "You are a deterministic UX helper. Always respond by calling the tool 'produce_title_and_summary' "
            "exactly once with JSON arguments. The title must be concise (<=60 chars). The summary must be a single "
            f"line (<= {max_len} chars). No extra text."
        )

        # Build a compact context for the model
        user_blob = {
            "title_hint": (title_hint or "").strip() or None,
            "messages": messages or [],
            "steps": steps or [],
        }

        tool = {
            "type": "function",
            "function": {
                "name": "produce_title_and_summary",
                "description": "Return a compact chat title and a one-line summary of intent/activity.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "title": {"type": "string", "description": "Concise title <=60 chars"},
                        "summary": {"type": "string", "description": f"One-line summary <= {max_len} chars"},
                    },
                    "required": ["title", "summary"],
                },
            },
        }

        # Messages for LiteLLM (OpenAI-compatible)
        llm_messages = [
            {"role": "system", "content": sys_prompt},
            {"role": "user", "content": f"CONTEXT:\n{_json.dumps(user_blob, ensure_ascii=False)}"},
        ]

        # Configure alias1 provider mapping (replicate CAI behavior)
        # Note: Using lower temperature (0.2) for focused/deterministic API responses
        # top_p uses default (1.0) from CAI_TOP_P env var
        kwargs = {
            "model": "alias1",
            "messages": llm_messages,
            "tools": [tool],
            "tool_choice": "required",
            "temperature": 0.2,  # Override default (0.7) - lower for API consistency
            "top_p": float(_os.getenv("CAI_TOP_P", "1.0")),  # Use env default
            "api_base": "https://api.aliasrobotics.com:666/",
            "custom_llm_provider": "openai",
            "api_key": _os.getenv("ALIAS_API_KEY", "sk-alias-1234567890").strip(),
        }

        try:
            resp = _litellm.completion(**kwargs)
        except Exception as exc:  # pragma: no cover - provider error path
            raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=f"Model call failed: {exc}") from exc

        # Extract the single tool call
        try:
            # Handle dict-like and object-like responses
            choices = None
            if isinstance(resp, dict):
                choices = resp.get("choices")
            else:
                choices = getattr(resp, "choices", None)
            if not choices:
                raise ValueError("No choices in completion result")
            choice = choices[0]
            msg = choice.get("message") if isinstance(choice, dict) else getattr(choice, "message", {})
            tool_calls = (msg.get("tool_calls") if isinstance(msg, dict) else getattr(msg, "tool_calls", None)) or []
            if not tool_calls:
                # Fallback: try function_call (older schema)
                fc = msg.get("function_call") if isinstance(msg, dict) else getattr(msg, "function_call", None)
                if fc:
                    args = (fc.get("arguments") if isinstance(fc, dict) else getattr(fc, "arguments", None)) or "{}"
                    data = _json.loads(args)
                    title = str(data.get("title") or "").strip()
                    summary = str(data.get("summary") or "").strip()
                    return title[:60], (summary[:max_len] + ("…" if len(summary) > max_len else ""))
                raise ValueError("No tool_calls in completion result")
            call = tool_calls[0]
            fn = call.get("function") if isinstance(call, dict) else getattr(call, "function", {})
            if not isinstance(fn, dict):
                fn = {"arguments": getattr(fn, "arguments", None)}
            args_raw = fn.get("arguments") or "{}"
            data = _json.loads(args_raw)
            title = str(data.get("title") or "").strip()
            summary = str(data.get("summary") or "").strip()
            # Enforce length constraints server-side too
            title = title[:60]
            summary = summary[:max_len] + ("…" if len(summary) > max_len else "")
            return title, summary
        except Exception as exc:  # pragma: no cover - parse failure
            raise HTTPException(status_code=status.HTTP_502_BAD_GATEWAY, detail=f"Invalid tool call: {exc}") from exc

    @app.post(
        "/api/v1/ux/summarize",
        response_model=UXSummarizeLiteResponse,
        tags=["ux"],
        dependencies=[Depends(_require_api_key)],
    )
    def ux_summarize_lite(payload: UXSummarizeLiteRequest) -> UXSummarizeLiteResponse:
        title, summary = _run_litellm_title_summary(
            messages=payload.messages,
            steps=payload.steps,
            title_hint=None,
            max_len=payload.max_len,
        )
        return UXSummarizeLiteResponse(summary_text=summary)

    @app.post(
        "/api/v1/ux/title",
        response_model=UXTitleLiteResponse,
        tags=["ux"],
        dependencies=[Depends(_require_api_key)],
    )
    def ux_title_lite(payload: UXTitleLiteRequest) -> UXTitleLiteResponse:
        title, summary = _run_litellm_title_summary(
            messages=payload.messages,
            steps=[],
            title_hint=payload.title_hint,
            max_len=100,
        )
        return UXTitleLiteResponse(title=title)

    return app
