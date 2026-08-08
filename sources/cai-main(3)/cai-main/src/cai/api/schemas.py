"""Pydantic schemas for the CAI API."""

from __future__ import annotations

from datetime import datetime
from typing import Any, Dict, List

from pydantic import BaseModel, Field


class HealthResponse(BaseModel):
    status: str
    version: str


class CommandMetadataModel(BaseModel):
    name: str
    description: str = ""
    aliases: List[str] = Field(default_factory=list)
    subcommands: List[str] = Field(default_factory=list)


class CommandRequest(BaseModel):
    args: List[str] | None = None
    auto_correct: bool = True


class CommandResponse(BaseModel):
    handled: bool
    suggested_command: str | None = None
    stdout: str
    stderr: str
    exit_code: int | None = None


class SessionSummaryModel(BaseModel):
    id: str
    agent: str
    model: str
    stateful: bool
    created_at: datetime
    updated_at: datetime
    history_length: int
    metadata: Dict[str, Any] = Field(default_factory=dict)


class SessionDetailModel(SessionSummaryModel):
    history: List[Dict[str, Any]] = Field(default_factory=list)


class CreateSessionRequest(BaseModel):
    agent: str | None = None
    model: str | None = None
    stateful: bool = True
    metadata: Dict[str, Any] | None = None


class RunResultPayload(BaseModel):
    messages: List[Dict[str, Any]]
    history: List[Dict[str, Any]]
    final_output: Any
    text_output: str | None = None
    input_guardrails: List[Dict[str, Any]] = Field(default_factory=list)
    output_guardrails: List[Dict[str, Any]] = Field(default_factory=list)


class InferenceRequest(BaseModel):
    input: str | List[Dict[str, Any]]
    context: Dict[str, Any] | None = None
    max_turns: int | float | None = None
    # Optional: launch one or more MCP SSE servers for this request (ephemeral)
    class MCPSseServer(BaseModel):
        url: str
        name: str | None = None
        headers: Dict[str, str] | None = None
        timeout: float | None = None
        sse_read_timeout: float | None = None

    mcp_sse: List[MCPSseServer] | None = None


class InferenceResponse(BaseModel):
    session: SessionSummaryModel
    result: RunResultPayload


class SessionsResponse(BaseModel):
    sessions: List[SessionSummaryModel]


class CommandsResponse(BaseModel):
    commands: List[CommandMetadataModel]


class SessionHistoryResponse(BaseModel):
    session: SessionDetailModel


class AgentToolModel(BaseModel):
    name: str
    description: str | None = None


class AgentMetadataModel(BaseModel):
    name: str
    description: str | None = None
    type: str = "agent"  # agent | pattern
    pattern_type: str | None = None
    tools: list[AgentToolModel] = Field(default_factory=list)


class AgentsResponse(BaseModel):
    agents: list[AgentMetadataModel]


class ModelPricingModel(BaseModel):
    input_cost_per_token: float | None = None
    output_cost_per_token: float | None = None
    max_tokens: int | None = None
    max_input_tokens: int | None = None
    max_output_tokens: int | None = None
    supports_function_calling: bool | None = None
    supports_vision: bool | None = None
    supports_response_schema: bool | None = None
    supports_tool_choice: bool | None = None


class ModelInfoModel(BaseModel):
    name: str
    provider: str | None = None
    category: str | None = None
    description: str | None = None
    input_cost: float | None = None  # per million tokens, if available
    output_cost: float | None = None  # per million tokens, if available
    pricing: ModelPricingModel | None = None


class ModelsResponse(BaseModel):
    models: list[ModelInfoModel]


class ReloadRequest(BaseModel):
    preserve_history: bool = True


class InterruptResponse(BaseModel):
    interrupted: bool


class FinalMessageRequest(BaseModel):
    prompt: str
    steps: List[Dict[str, Any]] = Field(default_factory=list)
    messages: List[Dict[str, Any]] = Field(default_factory=list)
    max_turns: int | float | None = None


# Lite UX endpoints (no session coupling)
class UXSummarizeLiteRequest(BaseModel):
    messages: List[Dict[str, Any]] = Field(default_factory=list)
    steps: List[Dict[str, Any]] = Field(default_factory=list)
    max_len: int = 100


class UXSummarizeLiteResponse(BaseModel):
    summary_text: str


class UXTitleLiteRequest(BaseModel):
    messages: List[Dict[str, Any]] = Field(default_factory=list)
    title_hint: str | None = None


class UXTitleLiteResponse(BaseModel):
    title: str


# Session cancellation endpoint
class CancelTaskResponse(BaseModel):
    """Response for task cancellation request."""
    cancelled: bool
    message: str


# Authentication endpoints
class AuthAddIpRequest(BaseModel):
    """Request body for the IP-based device pairing flow."""

    ip: str | None = None


class AuthAddIpResponse(BaseModel):
    """Response for the IP-based device pairing flow."""

    username: str
    password: str
    session_token: str


class AuthRegisterRequest(BaseModel):
    """Request to register a new user with explicit credentials."""

    username: str = Field(min_length=1)
    password: str = Field(min_length=8)


class AuthLoginRequest(BaseModel):
    """Request to log in with username/password and obtain a session token."""

    username: str
    password: str
    ip: str | None = None


class AuthLoginResponse(BaseModel):
    """Response containing a session token for subsequent API calls."""

    session_token: str
