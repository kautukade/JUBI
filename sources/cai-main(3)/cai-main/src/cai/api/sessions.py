"""State management helpers for the CAI API backend."""

from __future__ import annotations

import asyncio
import copy
import os
import threading
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Dict, List, MutableMapping

from cai.agents import get_agent_by_name
from cai.config import DEFAULT_AGENT_TYPE
from cai.sdk.agents.agent import Agent
from cai.sdk.agents.items import ItemHelpers, TResponseInputItem
from cai.sdk.agents.result import RunResult
from cai.sdk.agents.run import DEFAULT_MAX_TURNS, Runner
from cai.util import update_agent_models_recursively


class SessionNotFoundError(KeyError):
    """Raised when a requested session cannot be found."""


AgentFactory = Callable[[str, str, str], Agent]
"""Factory type used to construct CAI agents for sessions."""


def _default_agent_factory(agent_name: str, model_name: str, agent_id: str) -> Agent:
    """Default factory that instantiates an agent and enforces the requested model."""
    agent = get_agent_by_name(agent_name, agent_id=agent_id)
    update_agent_models_recursively(agent, model_name)
    return agent


@dataclass
class SessionSummary:
    """Serializable summary for a session."""

    id: str
    agent: str
    model: str
    stateful: bool
    created_at: datetime
    updated_at: datetime
    history_length: int
    metadata: Dict[str, Any] = field(default_factory=dict)


class SessionState:
    """Represents a conversational session with its own agent instance and memory."""

    def __init__(
        self,
        *,
        agent_name: str,
        model_name: str,
        stateful: bool,
        metadata: Dict[str, Any] | None,
        agent_factory: AgentFactory,
        session_id: str | None = None,
    ) -> None:
        self.id = session_id or str(uuid.uuid4())
        self.agent_name = agent_name
        self.model_name = model_name
        self.stateful = stateful
        self.metadata = metadata or {}
        self._agent_factory = agent_factory
        self.agent: Agent = self._agent_factory(agent_name, model_name, self.id)
        self.history: List[TResponseInputItem] = []
        self.created_at = datetime.now(timezone.utc)
        self.updated_at = self.created_at
        self._lock = asyncio.Lock()
        self._current_task: asyncio.Task | None = None
        self.last_steps: List[Dict[str, Any]] = []

    def to_summary(self) -> SessionSummary:
        """Return a lightweight snapshot of the session state."""
        return SessionSummary(
            id=self.id,
            agent=self.agent_name,
            model=self.model_name,
            stateful=self.stateful,
            created_at=self.created_at,
            updated_at=self.updated_at,
            history_length=len(self.history),
            metadata=copy.deepcopy(self.metadata),
        )

    def to_detail(self) -> Dict[str, Any]:
        """Return a serializable dict with the entire history."""
        summary = self.to_summary().__dict__.copy()
        summary["history"] = copy.deepcopy(self.history)
        return summary

    async def run_inference(
        self,
        new_input: str | List[TResponseInputItem],
        *,
        context: Dict[str, Any] | None = None,
        max_turns: int | float | None = None,
    ) -> RunResult:
        """Execute the agent with the provided input and keep state up to date."""
        composed_input = self._compose_input(new_input)
        async with self._lock:
            # Create a task for the runner so it can be cancelled
            task = asyncio.create_task(
                Runner.run(
                    self.agent,
                    composed_input,
                    context=context,
                    max_turns=max_turns if max_turns is not None else DEFAULT_MAX_TURNS,
                )
            )
            self.set_running_task(task)
            try:
                result = await task
                self.updated_at = datetime.now(timezone.utc)
                # Always persist session-level history for UX/metadata, regardless of stateful mode.
                # This does NOT affect the next agent run input when stateful=False.
                try:
                    self.history = result.to_input_list()
                except Exception:
                    # Fallback to do nothing if conversion fails
                    pass
                return result
            except asyncio.CancelledError:
                # Task was cancelled - this is expected behavior
                self.updated_at = datetime.now(timezone.utc)
                raise
            finally:
                self.set_running_task(None)

    def set_running_task(self, task: asyncio.Task | None) -> None:
        self._current_task = task

    def interrupt(self) -> bool:
        """Attempt to cancel the currently running task, if any. Returns True if signal sent."""
        task = self._current_task
        if task and not task.done():
            task.cancel()
            return True
        return False

    async def interrupt_and_wait(self, timeout: float = 5.0) -> bool:
        """
        Cancel the running task and wait briefly for it to finish.
        Returns True if the task ended (cancelled or already done).
        """
        task = self._current_task
        if not task or task.done():
            return False
        task.cancel()
        try:
            await asyncio.wait_for(task, timeout=timeout)
        except asyncio.TimeoutError:
            # Deliver cancellation and swallow the exception so caller can proceed.
            await asyncio.gather(task, return_exceptions=True)
            self.set_running_task(None)
            return False
        except asyncio.CancelledError:
            self.set_running_task(None)
            return True
        self.set_running_task(None)
        return True

    def reload(self, preserve_history: bool = True) -> None:
        """Recreate the agent instance. Optionally preserve the message history."""
        old_history = self.history if preserve_history else []
        self.agent = self._agent_factory(self.agent_name, self.model_name, self.id)
        self.history = old_history
        self.updated_at = datetime.now(timezone.utc)
        # Sync preserved history into agent model so future runs/UX see it automatically
        try:
            if self.stateful and hasattr(self.agent, "model") and hasattr(self.agent.model, "message_history"):
                self.agent.model.message_history = list(self.history)
        except Exception:
            pass

    def reset(self) -> None:
        """Restart the session with a fresh agent and clean history."""
        self.agent = self._agent_factory(self.agent_name, self.model_name, self.id)
        self.history = []
        self.updated_at = datetime.now(timezone.utc)

    def update_model(self, new_model: str) -> None:
        """Switch the underlying model and refresh the agent instance."""
        self.model_name = new_model
        self.agent = self._agent_factory(self.agent_name, self.model_name, self.id)
        self.history = []
        self.updated_at = datetime.now(timezone.utc)

    def _compose_input(self, new_input: str | List[TResponseInputItem]) -> List[TResponseInputItem]:
        """Merge the stored history with the new input if stateful."""
        normalized_new = self._normalize_input(new_input)
        if self.stateful and self.history:
            combined = copy.deepcopy(self.history)
            combined.extend(normalized_new)
            return combined
        return normalized_new

    @staticmethod
    def _normalize_input(new_input: str | List[TResponseInputItem]) -> List[TResponseInputItem]:
        if isinstance(new_input, str):
            return [{"role": "user", "content": new_input}]
        return copy.deepcopy(new_input)


class SessionManager:
    """In-memory registry for API sessions."""

    def __init__(self, agent_factory: AgentFactory | None = None) -> None:
        self._sessions: MutableMapping[str, SessionState] = {}
        self._agent_factory = agent_factory or _default_agent_factory
        self._lock = threading.Lock()
        self._default_agent = os.getenv("CAI_AGENT_TYPE", DEFAULT_AGENT_TYPE)
        self._default_model = os.getenv("CAI_MODEL", "alias1")

    def create_session(
        self,
        *,
        agent_name: str | None = None,
        model_name: str | None = None,
        stateful: bool = True,
        metadata: Dict[str, Any] | None = None,
    ) -> SessionState:
        session = SessionState(
            agent_name=agent_name or self._default_agent,
            model_name=model_name or self._default_model,
            stateful=stateful,
            metadata=metadata,
            agent_factory=self._agent_factory,
        )
        with self._lock:
            # Ensure per-session isolation: clear agent-side history and steps
            try:
                if hasattr(session.agent, "model") and hasattr(session.agent.model, "message_history"):
                    session.agent.model.message_history = []
            except Exception:
                pass
            session.last_steps = []
            self._sessions[session.id] = session
        return session

    def list_sessions(self) -> List[SessionSummary]:
        with self._lock:
            return [session.to_summary() for session in self._sessions.values()]

    def get_session(self, session_id: str) -> SessionState:
        with self._lock:
            try:
                return self._sessions[session_id]
            except KeyError as exc:
                raise SessionNotFoundError(session_id) from exc

    def delete_session(self, session_id: str) -> None:
        with self._lock:
            if session_id not in self._sessions:
                raise SessionNotFoundError(session_id)
            del self._sessions[session_id]

    def reset_session(self, session_id: str) -> SessionState:
        session = self.get_session(session_id)
        session.reset()
        return session

    def update_session_model(self, session_id: str, new_model: str) -> SessionState:
        session = self.get_session(session_id)
        session.update_model(new_model)
        return session


def summarize_run_result(result: RunResult) -> Dict[str, Any]:
    """Convert the run result into a JSON-serializable structure."""
    messages = []
    for item in result.new_items:
        entry: Dict[str, Any] = {
            "agent": getattr(item.agent, "name", None),
            "type": getattr(item, "type", item.__class__.__name__),
        }
        raw = item.raw_item
        if hasattr(raw, "model_dump"):
            entry["payload"] = raw.model_dump(exclude_unset=True)
        else:
            entry["payload"] = raw
        if hasattr(item, "output"):
            entry["output"] = getattr(item, "output")
        messages.append(entry)

    history = result.to_input_list()
    final_output = result.final_output
    if hasattr(final_output, "model_dump"):
        final_output = final_output.model_dump(exclude_unset=True)
    elif hasattr(final_output, "dict") and callable(final_output.dict):
        final_output = final_output.dict()  # type: ignore[call-arg]

    return {
        "messages": messages,
        "history": history,
        "final_output": final_output,
        "text_output": ItemHelpers.text_message_outputs(result.new_items),
        "input_guardrails": [getattr(g.output, "output_info", {}) for g in result.input_guardrail_results],
        "output_guardrails": [getattr(g.output, "output_info", {}) for g in result.output_guardrail_results],
    }
