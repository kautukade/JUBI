from __future__ import annotations

import asyncio
import contextvars
import dataclasses
import inspect
import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
from collections.abc import Awaitable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, cast

from openai.types.responses import (
    ResponseComputerToolCall,
    ResponseFileSearchToolCall,
    ResponseFunctionToolCall,
    ResponseFunctionWebSearch,
    ResponseOutputMessage,
)
from openai.types.responses.response_computer_tool_call import (
    ActionClick,
    ActionDoubleClick,
    ActionDrag,
    ActionKeypress,
    ActionMove,
    ActionScreenshot,
    ActionScroll,
    ActionType,
    ActionWait,
)
from openai.types.responses.response_input_param import ComputerCallOutput
from openai.types.responses.response_reasoning_item import ResponseReasoningItem

from .agent import Agent, ToolsToFinalOutputResult
from .agent_output import AgentOutputSchema
from .computer import AsyncComputer, Computer
from .exceptions import AgentsException, ModelBehaviorError, UserError
from .guardrail import InputGuardrail, InputGuardrailResult, OutputGuardrail, OutputGuardrailResult
from .handoffs import Handoff, HandoffInputData
from .items import (
    HandoffCallItem,
    HandoffOutputItem,
    ItemHelpers,
    MessageOutputItem,
    ModelResponse,
    ReasoningItem,
    RunItem,
    ToolCallItem,
    ToolCallOutputItem,
    TResponseInputItem,
)
from .lifecycle import RunHooks
from .logger import logger
from .model_settings import ModelSettings
from .models.interface import ModelTracing
from .run_context import RunContextWrapper, TContext
from .stream_events import RunItemStreamEvent, StreamEvent
from .tool import ComputerTool, FunctionTool, FunctionToolResult, Tool
from .tracing import (
    SpanError,
    Trace,
    function_span,
    get_current_trace,
    guardrail_span,
    handoff_span,
    trace,
)
from .orchestration_mas_hint import maybe_inject_orchestration_mas_hint_after_tools
from .util import _coro, _error_tracing

if TYPE_CHECKING:
    from .run import RunConfig


class QueueCompleteSentinel:
    pass


QUEUE_COMPLETE_SENTINEL = QueueCompleteSentinel()

_NOT_FINAL_OUTPUT = ToolsToFinalOutputResult(is_final_output=False, final_output=None)
_COMPACT_TASK_STACK: contextvars.ContextVar[tuple[str, ...]] = contextvars.ContextVar(
    "cai_compact_task_stack",
    default=(),
)
_COMPACT_HIDDEN_TOOL_NAMES = frozenset(
    {
        "analyze_task_requirements",
        "check_available_agents",
        # ``run_dual_approach_contest`` is the orchestrator's "explore-two-options"
        # primitive; the user only needs to see the follow-up ``run_specialist``
        # row that the orchestrator picks afterwards. Showing the contest row too
        # duplicates the orchestrator's name on the live block for no extra info.
        "run_dual_approach_contest",
    }
)


# ---------------------------------------------------------------------------
# Compact REPL: Task event emission helpers (q3=b)
# ---------------------------------------------------------------------------
# These thin wrappers translate per-tool-invocation lifecycle into Task* events
# on cai.output.OUTPUT, which the CompactCLIHandler consumes to render the
# single-line live block. They never raise — failures degrade silently so they
# can't break the agent runtime.


def _emit_compact_task_start(
    agent: "Agent[Any]",
    func_tool: Any,
    tool_call: Any,
) -> tuple[str, float]:
    """Emit :class:`TaskStartEvent` for a tool invocation.

    Returns ``(task_id, started_at)`` to be threaded into the matching
    complete/error emission, avoiding any global state.
    """
    import time
    import uuid

    tool_name = getattr(func_tool, "name", "") or ""
    task_id = uuid.uuid4().hex[:12]
    started_at = time.time()
    if tool_name in _COMPACT_HIDDEN_TOOL_NAMES:
        return "", started_at

    task_stack = _COMPACT_TASK_STACK.get()
    parent_task_id = task_stack[-1] if task_stack else ""
    depth = len(task_stack)
    try:
        from cai.output import OUTPUT, TaskStartEvent
        from cai.repl.ui.task_label import infer_task_label
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        agent_name = getattr(agent, "name", None) or "Agent"
        try:
            agent_id = getattr(getattr(agent, "model", None), "agent_id", None)
            agent_id = agent_id or AGENT_MANAGER.get_agent_id() or ""
        except Exception:
            agent_id = ""
        try:
            label = infer_task_label(tool_name, tool_call.arguments, agent_name)
        except Exception:
            label = tool_name
        OUTPUT.emit(
            TaskStartEvent(
                task_id=task_id,
                agent_name=agent_name,
                agent_id=agent_id,
                tool_name=tool_name,
                label=label,
                call_id=getattr(tool_call, "id", "") or "",
                parent_task_id=parent_task_id,
                depth=depth,
            )
        )
    except Exception:
        pass
    return task_id, started_at


def _push_compact_task(task_id: str) -> contextvars.Token[tuple[str, ...]]:
    """Mark subsequent nested tool calls as children of ``task_id`` in this async context."""
    return _COMPACT_TASK_STACK.set((*_COMPACT_TASK_STACK.get(), task_id))


def _emit_compact_task_complete(task_id: str, started_at: float, result: Any) -> None:
    """Emit :class:`TaskCompleteEvent` for ``task_id``."""
    import time

    try:
        from cai.output import OUTPUT, TaskCompleteEvent

        OUTPUT.emit(
            TaskCompleteEvent(
                task_id=task_id,
                output=truncate_output(result) if result is not None else "",
                duration_seconds=max(0.0, time.time() - started_at),
            )
        )
    except Exception:
        pass


def _emit_compact_task_error(task_id: str, started_at: float, error: BaseException) -> None:
    """Emit :class:`TaskErrorEvent` for ``task_id``."""
    import time

    try:
        from cai.output import OUTPUT, TaskErrorEvent

        OUTPUT.emit(
            TaskErrorEvent(
                task_id=task_id,
                output=str(error),
                error=str(error),
                error_type=type(error).__name__,
                duration_seconds=max(0.0, time.time() - started_at),
            )
        )
    except Exception:
        pass


def truncate_output(output: Any, max_length: int = 10000) -> str:
    """Truncate tool output if it exceeds max_length characters.

    Shows first 5000 and last 5000 characters with TRUNCATED in the middle.
    """
    output_str = str(output)
    if len(output_str) <= max_length:
        return output_str

    # Show first 5000 and last 5000 characters
    first_part = output_str[:5000]
    last_part = output_str[-5000:]
    return f"{first_part}\n\n... TRUNCATED ...\n\n{last_part}"


@dataclass
class AgentToolUseTracker:
    agent_to_tools: list[tuple[Agent, list[str]]] = field(default_factory=list)
    """Tuple of (agent, list of tools used). Can't use a dict because agents aren't hashable."""

    def add_tool_use(self, agent: Agent[Any], tool_names: list[str]) -> None:
        existing_data = next((item for item in self.agent_to_tools if item[0] == agent), None)
        if existing_data:
            existing_data[1].extend(tool_names)
        else:
            self.agent_to_tools.append((agent, tool_names))

    def has_used_tools(self, agent: Agent[Any]) -> bool:
        existing_data = next((item for item in self.agent_to_tools if item[0] == agent), None)
        return existing_data is not None and len(existing_data[1]) > 0


@dataclass
class ToolRunHandoff:
    handoff: Handoff
    tool_call: ResponseFunctionToolCall


@dataclass
class ToolRunFunction:
    tool_call: ResponseFunctionToolCall
    function_tool: FunctionTool


@dataclass
class ToolRunComputerAction:
    tool_call: ResponseComputerToolCall
    computer_tool: ComputerTool


@dataclass
class ProcessedResponse:
    new_items: list[RunItem]
    handoffs: list[ToolRunHandoff]
    functions: list[ToolRunFunction]
    computer_actions: list[ToolRunComputerAction]
    tools_used: list[str]  # Names of all tools used, including hosted tools

    def has_tools_to_run(self) -> bool:
        # Handoffs, functions and computer actions need local processing
        # Hosted tools have already run, so there's nothing to do.
        return any(
            [
                self.handoffs,
                self.functions,
                self.computer_actions,
            ]
        )


@dataclass
class NextStepHandoff:
    new_agent: Agent[Any]


@dataclass
class NextStepFinalOutput:
    output: Any


@dataclass
class NextStepRunAgain:
    pass


@dataclass
class SingleStepResult:
    original_input: str | list[TResponseInputItem]
    """The input items i.e. the items before run() was called. May be mutated by handoff input
    filters."""

    model_response: ModelResponse
    """The model response for the current step."""

    pre_step_items: list[RunItem]
    """Items generated before the current step."""

    new_step_items: list[RunItem]
    """Items generated during this current step."""

    next_step: NextStepHandoff | NextStepFinalOutput | NextStepRunAgain
    """The next step to take."""

    @property
    def generated_items(self) -> list[RunItem]:
        """Items generated during the agent run (i.e. everything generated after
        `original_input`)."""
        return self.pre_step_items + self.new_step_items


def get_model_tracing_impl(
    tracing_disabled: bool, trace_include_sensitive_data: bool
) -> ModelTracing:
    if tracing_disabled:
        return ModelTracing.DISABLED
    elif trace_include_sensitive_data:
        return ModelTracing.ENABLED
    else:
        return ModelTracing.ENABLED_WITHOUT_DATA


class RunImpl:
    @classmethod
    async def execute_tools_and_side_effects(
        cls,
        *,
        agent: Agent[TContext],
        # The original input to the Runner
        original_input: str | list[TResponseInputItem],
        # Everything generated by Runner since the original input, but before the current step
        pre_step_items: list[RunItem],
        new_response: ModelResponse,
        processed_response: ProcessedResponse,
        output_schema: AgentOutputSchema | None,
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        run_config: RunConfig,
    ) -> SingleStepResult:
        # Make a copy of the generated items
        pre_step_items = list(pre_step_items)

        new_step_items: list[RunItem] = []
        new_step_items.extend(processed_response.new_items)

        # First, lets run the tool calls - function tools and computer actions
        # Create tasks separately so we can handle partial results
        function_task = asyncio.create_task(
            cls.execute_function_tool_calls(
                agent=agent,
                tool_runs=processed_response.functions,
                hooks=hooks,
                context_wrapper=context_wrapper,
                config=run_config,
            )
        )
        computer_task = asyncio.create_task(
            cls.execute_computer_actions(
                agent=agent,
                actions=processed_response.computer_actions,
                hooks=hooks,
                context_wrapper=context_wrapper,
                config=run_config,
            )
        )

        function_results = []
        computer_results = []
        interrupt_exception = None

        try:
            function_results, computer_results = await asyncio.gather(function_task, computer_task)
        except (KeyboardInterrupt, asyncio.CancelledError) as e:
            interrupt_exception = e

            # Try to get partial results from the tasks
            if function_task.done() and not function_task.cancelled():
                try:
                    function_results = function_task.result()
                except Exception:
                    # If the task failed, create synthetic results
                    function_results = []
                    for tool_run in processed_response.functions:
                        result = FunctionToolResult(
                            tool=tool_run.function_tool,
                            output="Tool execution interrupted",
                            run_item=ToolCallOutputItem(
                                output="Tool execution interrupted",
                                raw_item=ItemHelpers.tool_call_output_item(
                                    tool_run.tool_call, "Tool execution interrupted"
                                ),
                                agent=agent,
                            ),
                        )
                        function_results.append(result)
            else:
                # Task was cancelled or not done, create synthetic results
                function_results = []
                for tool_run in processed_response.functions:
                    result = FunctionToolResult(
                        tool=tool_run.function_tool,
                        output="Tool execution interrupted",
                        run_item=ToolCallOutputItem(
                            output="Tool execution interrupted",
                            raw_item=ItemHelpers.tool_call_output_item(
                                tool_run.tool_call, "Tool execution interrupted"
                            ),
                            agent=agent,
                        ),
                    )
                    function_results.append(result)

            if computer_task.done() and not computer_task.cancelled():
                try:
                    computer_results = computer_task.result()
                except Exception:
                    computer_results = []
            else:
                computer_results = []

        new_step_items.extend([result.run_item for result in function_results])
        new_step_items.extend(computer_results)

        # Re-raise the interruption after ensuring results are added
        if interrupt_exception:
            raise interrupt_exception

        # Second, check if there are any handoffs
        if run_handoffs := processed_response.handoffs:
            return await cls.execute_handoffs(
                agent=agent,
                original_input=original_input,
                pre_step_items=pre_step_items,
                new_step_items=new_step_items,
                new_response=new_response,
                run_handoffs=run_handoffs,
                hooks=hooks,
                context_wrapper=context_wrapper,
                run_config=run_config,
            )

        maybe_inject_orchestration_mas_hint_after_tools(
            agent=agent,
            original_input=original_input,
            function_results=function_results,
            run_config=run_config,
        )

        # Third, we'll check if the tool use should result in a final output
        check_tool_use = await cls._check_for_final_output_from_tools(
            agent=agent,
            tool_results=function_results,
            context_wrapper=context_wrapper,
            config=run_config,
        )

        if check_tool_use.is_final_output:
            # If the output type is str, then let's just stringify it
            if not agent.output_type or agent.output_type is str:
                check_tool_use.final_output = str(check_tool_use.final_output)

            if check_tool_use.final_output is None:
                logger.error(
                    "Model returned a final output of None. Not raising an error because we assume"
                    "you know what you're doing."
                )

            return await cls.execute_final_output(
                agent=agent,
                original_input=original_input,
                new_response=new_response,
                pre_step_items=pre_step_items,
                new_step_items=new_step_items,
                final_output=check_tool_use.final_output,
                hooks=hooks,
                context_wrapper=context_wrapper,
            )

        # Now we can check if the model also produced a final output
        message_items = [item for item in new_step_items if isinstance(item, MessageOutputItem)]

        # We'll use the last content output as the final output
        potential_final_output_text = (
            ItemHelpers.extract_last_text(message_items[-1].raw_item) if message_items else None
        )

        # There are two possibilities that lead to a final output:
        # 1. Structured output schema => always leads to a final output
        # 2. Plain text output schema => only leads to a final output if there are no tool calls
        if output_schema and not output_schema.is_plain_text() and potential_final_output_text:
            final_output = output_schema.validate_json(potential_final_output_text)
            return await cls.execute_final_output(
                agent=agent,
                original_input=original_input,
                new_response=new_response,
                pre_step_items=pre_step_items,
                new_step_items=new_step_items,
                final_output=final_output,
                hooks=hooks,
                context_wrapper=context_wrapper,
            )
        elif (
            not output_schema or output_schema.is_plain_text()
        ) and not processed_response.has_tools_to_run():
            # If there are tool outputs in the step (e.g., a synthetic output for a missing tool),
            # do not finalize; run the model again so it can react to the tool output guidance.
            if any(isinstance(item, ToolCallOutputItem) for item in new_step_items):
                return SingleStepResult(
                    original_input=original_input,
                    model_response=new_response,
                    pre_step_items=pre_step_items,
                    new_step_items=new_step_items,
                    next_step=NextStepRunAgain(),
                )
            return await cls.execute_final_output(
                agent=agent,
                original_input=original_input,
                new_response=new_response,
                pre_step_items=pre_step_items,
                new_step_items=new_step_items,
                final_output=potential_final_output_text or "",
                hooks=hooks,
                context_wrapper=context_wrapper,
            )
        else:
            # If there's no final output, we can just run again
            return SingleStepResult(
                original_input=original_input,
                model_response=new_response,
                pre_step_items=pre_step_items,
                new_step_items=new_step_items,
                next_step=NextStepRunAgain(),
            )

    @classmethod
    def maybe_reset_tool_choice(
        cls, agent: Agent[Any], tool_use_tracker: AgentToolUseTracker, model_settings: ModelSettings
    ) -> ModelSettings:
        """Resets tool choice to None if the agent has used tools and the agent's reset_tool_choice
        flag is True."""

        if agent.reset_tool_choice is True and tool_use_tracker.has_used_tools(agent):
            return dataclasses.replace(model_settings, tool_choice=None)

        return model_settings

    @classmethod
    def process_model_response(
        cls,
        *,
        agent: Agent[Any],
        all_tools: list[Tool],
        response: ModelResponse,
        output_schema: AgentOutputSchema | None,
        handoffs: list[Handoff],
    ) -> ProcessedResponse:
        items: list[RunItem] = []

        run_handoffs = []
        functions = []
        computer_actions = []
        tools_used: list[str] = []
        handoff_map = {handoff.tool_name: handoff for handoff in handoffs}
        function_map = {tool.name: tool for tool in all_tools if isinstance(tool, FunctionTool)}
        computer_tool = next((tool for tool in all_tools if isinstance(tool, ComputerTool)), None)

        for output in response.output:
            if isinstance(output, ResponseOutputMessage):
                items.append(MessageOutputItem(raw_item=output, agent=agent))
            elif isinstance(output, ResponseFileSearchToolCall):
                items.append(ToolCallItem(raw_item=output, agent=agent))
                tools_used.append("file_search")
            elif isinstance(output, ResponseFunctionWebSearch):
                items.append(ToolCallItem(raw_item=output, agent=agent))
                tools_used.append("web_search")
            elif isinstance(output, ResponseReasoningItem):
                items.append(ReasoningItem(raw_item=output, agent=agent))
            elif isinstance(output, ResponseComputerToolCall):
                items.append(ToolCallItem(raw_item=output, agent=agent))
                tools_used.append("computer_use")
                if not computer_tool:
                    _error_tracing.attach_error_to_current_span(
                        SpanError(
                            message="Computer tool not found",
                            data={},
                        )
                    )
                    raise ModelBehaviorError(
                        "Model produced computer action without a computer tool."
                    )
                computer_actions.append(
                    ToolRunComputerAction(tool_call=output, computer_tool=computer_tool)
                )
            elif not isinstance(output, ResponseFunctionToolCall):
                logger.warning(f"Unexpected output type, ignoring: {type(output)}")
                continue

            # At this point we know it's a function tool call
            if not isinstance(output, ResponseFunctionToolCall):
                continue

            tools_used.append(output.name)

            # Handoffs
            if output.name in handoff_map:
                items.append(HandoffCallItem(raw_item=output, agent=agent))
                handoff = ToolRunHandoff(
                    tool_call=output,
                    handoff=handoff_map[output.name],
                )
                run_handoffs.append(handoff)
            # Regular function tool call
            else:
                if output.name not in function_map:
                    # Gracefully handle missing tools by emitting a tool call and
                    # a synthetic tool output instead of raising. This allows the
                    # agent loop to continue and the model to react.
                    _error_tracing.attach_error_to_current_span(
                        SpanError(
                            message="Tool not found",
                            data={"tool_name": output.name},
                        )
                    )

                    # Record the attempted tool call so it appears in history/inputs
                    items.append(ToolCallItem(raw_item=output, agent=agent))

                    # Emit a synthetic tool output containing a prompt for the LLM
                    # with guidance to pick another available tool.
                    available_tools = ", ".join(sorted(function_map.keys())) or "(none)"
                    error_msg = (
                        "You attempted to call an unavailable tool '"
                        f"{output.name}' for agent '{agent.name}'.\n"
                        f"Available tools: {available_tools}.\n"
                        "Choose the best alternative tool and issue a new function_call with"
                        " appropriate arguments. If no tool fits, ask one brief clarifying"
                        " question instead of calling a tool."
                    )
                    items.append(
                        ToolCallOutputItem(
                            raw_item=ItemHelpers.tool_call_output_item(output, error_msg),
                            output=error_msg,
                            agent=agent,
                        )
                    )
                    # Don't schedule execution for a non-existent tool
                    continue
                items.append(ToolCallItem(raw_item=output, agent=agent))
                functions.append(
                    ToolRunFunction(
                        tool_call=output,
                        function_tool=function_map[output.name],
                    )
                )

        return ProcessedResponse(
            new_items=items,
            handoffs=run_handoffs,
            functions=functions,
            computer_actions=computer_actions,
            tools_used=tools_used,
        )

    @classmethod
    async def execute_function_tool_calls(
        cls,
        *,
        agent: Agent[TContext],
        tool_runs: list[ToolRunFunction],
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        config: RunConfig,
    ) -> list[FunctionToolResult]:
        # DEBUG: Log tool execution details
        import os
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                    import datetime
                    f.write(f"\n[{datetime.datetime.now()}] execute_function_tool_calls called\n")
                    f.write(f"Agent: {agent.name if agent else 'None'}\n")
                    f.write(f"Number of tool_runs: {len(tool_runs) if tool_runs else 0}\n")
                    if tool_runs:
                        for i, run in enumerate(tool_runs):
                            tool_name = (
                                run.function_tool.name
                                if run and run.function_tool
                                else "None"
                            )
                            f.write(f"  Tool {i}: {tool_name}\n")
            except Exception as e:
                pass  # Don't let debug logging break execution
        async def run_single_tool(
            func_tool: FunctionTool, tool_call: ResponseFunctionToolCall
        ) -> Any:
            # DEBUG: Log individual tool execution
            if os.getenv("CAI_TUI_MODE") == "true":
                try:
                    with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                        tool_name = func_tool.name if func_tool else "None"
                        f.write(f"\n[DEBUG] run_single_tool: {tool_name}\n")
                        f.write(f"  Tool call ID: {tool_call.id if tool_call else 'None'}\n")
                        f.write(f"  Tool call type: {type(tool_call)}\n")
                except Exception:
                    pass

            # Compact REPL: emit TaskStartEvent so the live block can render
            # a single-line row for this tool. We compute the deterministic
            # label once here and reuse it on completion to avoid drift.
            _compact_task_id, _compact_started_at = _emit_compact_task_start(
                agent, func_tool, tool_call
            )
            _compact_stack_token = (
                _push_compact_task(_compact_task_id) if _compact_task_id else None
            )

            try:
                with function_span(func_tool.name) as span_fn:
                    if config.trace_include_sensitive_data:
                        span_fn.span_data.input = tool_call.arguments

                    try:
                        _, _, result = await asyncio.gather(
                            hooks.on_tool_start(context_wrapper, agent, func_tool),
                            (
                                agent.hooks.on_tool_start(context_wrapper, agent, func_tool)
                                if agent.hooks
                                else _coro.noop_coroutine()
                            ),
                            func_tool.on_invoke_tool(context_wrapper, tool_call.arguments),
                        )

                        await asyncio.gather(
                            hooks.on_tool_end(context_wrapper, agent, func_tool, result),
                            (
                                agent.hooks.on_tool_end(context_wrapper, agent, func_tool, result)
                                if agent.hooks
                                else _coro.noop_coroutine()
                            ),
                        )

                        if _compact_task_id:
                            _emit_compact_task_complete(
                                _compact_task_id,
                                _compact_started_at,
                                result,
                            )
                    except asyncio.CancelledError:
                        # Let cancellation propagate without wrapping
                        raise
                    except RuntimeError as e:
                        if "cannot reuse already awaited coroutine" in str(e):
                            # This is expected when cancelling - just return cancelled message
                            if _compact_task_id:
                                _emit_compact_task_error(
                                    _compact_task_id,
                                    _compact_started_at,
                                    e,
                                )
                            return "Tool execution cancelled"
                        # Log other runtime errors
                        if os.getenv("CAI_TUI_MODE") == "true":
                            with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                                import traceback
                                f.write(f"\n[ERROR] RuntimeError in tool execution:\n")
                                f.write(f"Tool: {func_tool.name if func_tool else 'None'}\n")
                                f.write(f"Error: {str(e)}\n")
                                traceback.print_exc(file=f)

                        _error_tracing.attach_error_to_current_span(
                            SpanError(
                                message="Error running tool",
                                data={"tool_name": func_tool.name, "error": str(e)},
                            )
                        )
                        if _compact_task_id:
                            _emit_compact_task_error(_compact_task_id, _compact_started_at, e)
                        raise UserError(f"Error running tool {func_tool.name}: {e}") from e
                    except Exception as e:
                        # Check for coroutine reuse in any exception
                        if "cannot reuse already awaited coroutine" in str(e):
                            if _compact_task_id:
                                _emit_compact_task_error(
                                    _compact_task_id,
                                    _compact_started_at,
                                    e,
                                )
                            return "Tool execution cancelled"

                        # DEBUG: Log the specific error
                        if os.getenv("CAI_TUI_MODE") == "true":
                            with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                                import traceback
                                f.write(f"\n[ERROR] Exception in tool execution:\n")
                                f.write(f"Tool: {func_tool.name if func_tool else 'None'}\n")
                                f.write(f"Error type: {type(e).__name__}\n")
                                f.write(f"Error: {str(e)}\n")
                                if "NoneType" in str(e):
                                    f.write(f"*** NoneType error detected! ***\n")
                                f.write("Full traceback:\n")
                                traceback.print_exc(file=f)

                        _error_tracing.attach_error_to_current_span(
                            SpanError(
                                message="Error running tool",
                                data={"tool_name": func_tool.name, "error": str(e)},
                            )
                        )
                        if _compact_task_id:
                            _emit_compact_task_error(_compact_task_id, _compact_started_at, e)
                        if isinstance(e, AgentsException):
                            raise e
                        raise UserError(f"Error running tool {func_tool.name}: {e}") from e

                    if config.trace_include_sensitive_data:
                        span_fn.span_data.output = result
            finally:
                if _compact_stack_token is not None:
                    _COMPACT_TASK_STACK.reset(_compact_stack_token)
            return result

        tasks = []
        for tool_run in tool_runs:
            # DEBUG: Log tool run details
            if os.getenv("CAI_TUI_MODE") == "true":
                try:
                    with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                        f.write(f"\n[DEBUG] Creating task for tool_run\n")
                        f.write(f"  tool_run type: {type(tool_run)}\n")
                        f.write(f"  has function_tool: {hasattr(tool_run, 'function_tool')}\n")
                        f.write(f"  has tool_call: {hasattr(tool_run, 'tool_call')}\n")
                        if hasattr(tool_run, 'function_tool') and tool_run.function_tool:
                            f.write(f"  function_tool name: {tool_run.function_tool.name}\n")
                        if hasattr(tool_run, 'tool_call') and tool_run.tool_call:
                            f.write(f"  tool_call id: {tool_run.tool_call.id}\n")
                except Exception as e:
                    pass

            function_tool = tool_run.function_tool
            tasks.append(asyncio.create_task(run_single_tool(function_tool, tool_run.tool_call)))

        try:
            if tool_runs:
                from cai.util.wait_hints import tool_batch_wait_hints

                names = [tr.function_tool.name for tr in tool_runs]
                args_list = [tr.tool_call.arguments for tr in tool_runs]
                async with tool_batch_wait_hints(names, args_list):
                    results = await asyncio.gather(*tasks)
            else:
                results = await asyncio.gather(*tasks)
        except (KeyboardInterrupt, asyncio.CancelledError) as e:
            # When interrupted, cancel any still-running task explicitly so
            # the subprocesses they own (e.g. nmap, sqlmap) get killed in
            # their own ``except CancelledError`` blocks instead of leaking
            # into the background.
            for task in tasks:
                if not task.done():
                    task.cancel()
            # Drain so cancellation actually reaches each tool, then collect
            # whatever finished.
            try:
                await asyncio.gather(*tasks, return_exceptions=True)
            except Exception:
                pass

            results = []
            for i, task in enumerate(tasks):
                if task.done() and not task.cancelled():
                    try:
                        results.append(task.result())
                    except Exception:
                        results.append("Tool execution interrupted")
                else:
                    results.append("Tool execution interrupted")

            # Re-raise the exception after collecting results
            raise e

        return [
            FunctionToolResult(
                tool=tool_run.function_tool,
                output=result,
                run_item=ToolCallOutputItem(
                    output=result,
                    raw_item=ItemHelpers.tool_call_output_item(
                        tool_run.tool_call, truncate_output(result)
                    ),
                    agent=agent,
                ),
            )
            for tool_run, result in zip(tool_runs, results)
        ]

    @classmethod
    async def execute_computer_actions(
        cls,
        *,
        agent: Agent[TContext],
        actions: list[ToolRunComputerAction],
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        config: RunConfig,
    ) -> list[RunItem]:
        """Run computer-use steps one after another.

        Parallel execution with ``asyncio.gather`` is intentionally not used here:
        each action (click, type, scroll, …) mutates shared UI state and the next
        screenshot must reflect the previous step. Parallelizing would require
        explicit dependency graphs or isolated environments (future work / high risk).
        """
        results: list[RunItem] = []
        # Need to run these serially, because each action can affect the computer state
        for action in actions:
            results.append(
                await ComputerAction.execute(
                    agent=agent,
                    action=action,
                    hooks=hooks,
                    context_wrapper=context_wrapper,
                    config=config,
                )
            )

        return results

    @classmethod
    async def execute_handoffs(
        cls,
        *,
        agent: Agent[TContext],
        original_input: str | list[TResponseInputItem],
        pre_step_items: list[RunItem],
        new_step_items: list[RunItem],
        new_response: ModelResponse,
        run_handoffs: list[ToolRunHandoff],
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        run_config: RunConfig,
    ) -> SingleStepResult:
        # If there is more than one handoff, add tool responses that reject those handoffs
        multiple_handoffs = len(run_handoffs) > 1
        if multiple_handoffs:
            output_message = "Multiple handoffs detected, ignoring this one."
            new_step_items.extend(
                [
                    ToolCallOutputItem(
                        output=output_message,
                        raw_item=ItemHelpers.tool_call_output_item(
                            handoff.tool_call, output_message
                        ),
                        agent=agent,
                    )
                    for handoff in run_handoffs[1:]
                ]
            )

        actual_handoff = run_handoffs[0]
        with handoff_span(from_agent=agent.name) as span_handoff:
            handoff = actual_handoff.handoff
            new_agent: Agent[Any] = await handoff.on_invoke_handoff(
                context_wrapper, actual_handoff.tool_call.arguments
            )
            span_handoff.span_data.to_agent = new_agent.name

            # Propagate display context to the new agent for TUI support
            try:
                from cai.tui.display.handoff_context import propagate_display_context_to_agent

                propagate_display_context_to_agent(new_agent, parent_agent=agent)
            except ImportError:
                # TUI module not available, skip
                pass
            if multiple_handoffs:
                requested_agents = [handoff.handoff.agent_name for handoff in run_handoffs]
                span_handoff.set_error(
                    SpanError(
                        message="Multiple handoffs requested",
                        data={
                            "requested_agents": requested_agents,
                        },
                    )
                )

            # Append a tool output item for the handoff
            new_step_items.append(
                HandoffOutputItem(
                    agent=agent,
                    raw_item=ItemHelpers.tool_call_output_item(
                        actual_handoff.tool_call,
                        handoff.get_transfer_message(new_agent),
                    ),
                    source_agent=agent,
                    target_agent=new_agent,
                )
            )

            # Execute handoff hooks
            await asyncio.gather(
                hooks.on_handoff(
                    context=context_wrapper,
                    from_agent=agent,
                    to_agent=new_agent,
                ),
                (
                    agent.hooks.on_handoff(
                        context_wrapper,
                        agent=new_agent,
                        source=agent,
                    )
                    if agent.hooks
                    else _coro.noop_coroutine()
                ),
            )

            # If there's an input filter, filter the input for the next agent
            input_filter = handoff.input_filter or (
                run_config.handoff_input_filter if run_config else None
            )
            if input_filter:
                logger.debug("Filtering inputs for handoff")
                handoff_input_data = HandoffInputData(
                    input_history=tuple(original_input)
                    if isinstance(original_input, list)
                    else original_input,
                    pre_handoff_items=tuple(pre_step_items),
                    new_items=tuple(new_step_items),
                )
                if not callable(input_filter):
                    _error_tracing.attach_error_to_span(
                        span_handoff,
                        SpanError(
                            message="Invalid input filter",
                            data={"details": "not callable()"},
                        ),
                    )
                    raise UserError(f"Invalid input filter: {input_filter}")
                filtered = input_filter(handoff_input_data)
                if not isinstance(filtered, HandoffInputData):
                    _error_tracing.attach_error_to_span(
                        span_handoff,
                        SpanError(
                            message="Invalid input filter result",
                            data={"details": "not a HandoffInputData"},
                        ),
                    )
                    raise UserError(f"Invalid input filter result: {filtered}")

                original_input = (
                    filtered.input_history
                    if isinstance(filtered.input_history, str)
                    else list(filtered.input_history)
                )
                pre_step_items = list(filtered.pre_handoff_items)
                new_step_items = list(filtered.new_items)

        return SingleStepResult(
            original_input=original_input,
            model_response=new_response,
            pre_step_items=pre_step_items,
            new_step_items=new_step_items,
            next_step=NextStepHandoff(new_agent),
        )

    @classmethod
    async def execute_final_output(
        cls,
        *,
        agent: Agent[TContext],
        original_input: str | list[TResponseInputItem],
        new_response: ModelResponse,
        pre_step_items: list[RunItem],
        new_step_items: list[RunItem],
        final_output: Any,
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
    ) -> SingleStepResult:
        # Run the on_end hooks
        await cls.run_final_output_hooks(agent, hooks, context_wrapper, final_output)

        return SingleStepResult(
            original_input=original_input,
            model_response=new_response,
            pre_step_items=pre_step_items,
            new_step_items=new_step_items,
            next_step=NextStepFinalOutput(final_output),
        )

    @classmethod
    async def run_final_output_hooks(
        cls,
        agent: Agent[TContext],
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        final_output: Any,
    ):
        await asyncio.gather(
            hooks.on_agent_end(context_wrapper, agent, final_output),
            agent.hooks.on_end(context_wrapper, agent, final_output)
            if agent.hooks
            else _coro.noop_coroutine(),
        )

    @classmethod
    async def run_single_input_guardrail(
        cls,
        agent: Agent[Any],
        guardrail: InputGuardrail[TContext],
        input: str | list[TResponseInputItem],
        context: RunContextWrapper[TContext],
    ) -> InputGuardrailResult:
        with guardrail_span(guardrail.get_name()) as span_guardrail:
            result = await guardrail.run(agent, input, context)
            span_guardrail.span_data.triggered = result.output.tripwire_triggered
            return result

    @classmethod
    async def run_single_output_guardrail(
        cls,
        guardrail: OutputGuardrail[TContext],
        agent: Agent[Any],
        agent_output: Any,
        context: RunContextWrapper[TContext],
    ) -> OutputGuardrailResult:
        with guardrail_span(guardrail.get_name()) as span_guardrail:
            result = await guardrail.run(agent=agent, agent_output=agent_output, context=context)
            span_guardrail.span_data.triggered = result.output.tripwire_triggered
            return result

    @classmethod
    def stream_step_result_to_queue(
        cls,
        step_result: SingleStepResult,
        queue: asyncio.Queue[StreamEvent | QueueCompleteSentinel],
    ):
        for item in step_result.new_step_items:
            if isinstance(item, MessageOutputItem):
                event = RunItemStreamEvent(item=item, name="message_output_created")
            elif isinstance(item, HandoffCallItem):
                event = RunItemStreamEvent(item=item, name="handoff_requested")
            elif isinstance(item, HandoffOutputItem):
                event = RunItemStreamEvent(item=item, name="handoff_occured")
            elif isinstance(item, ToolCallItem):
                event = RunItemStreamEvent(item=item, name="tool_called")
            elif isinstance(item, ToolCallOutputItem):
                event = RunItemStreamEvent(item=item, name="tool_output")
            elif isinstance(item, ReasoningItem):
                event = RunItemStreamEvent(item=item, name="reasoning_item_created")
            else:
                logger.warning(f"Unexpected item type: {type(item)}")
                event = None

            if event:
                queue.put_nowait(event)

    @classmethod
    async def _check_for_final_output_from_tools(
        cls,
        *,
        agent: Agent[TContext],
        tool_results: list[FunctionToolResult],
        context_wrapper: RunContextWrapper[TContext],
        config: RunConfig,
    ) -> ToolsToFinalOutputResult:
        """Returns (i, final_output)."""
        if not tool_results:
            return _NOT_FINAL_OUTPUT

        if agent.tool_use_behavior == "run_llm_again":
            return _NOT_FINAL_OUTPUT
        elif agent.tool_use_behavior == "stop_on_first_tool":
            return ToolsToFinalOutputResult(
                is_final_output=True, final_output=tool_results[0].output
            )
        elif isinstance(agent.tool_use_behavior, dict):
            names = agent.tool_use_behavior.get("stop_at_tool_names", [])
            for tool_result in tool_results:
                if tool_result.tool.name in names:
                    return ToolsToFinalOutputResult(
                        is_final_output=True, final_output=tool_result.output
                    )
            return ToolsToFinalOutputResult(is_final_output=False, final_output=None)
        elif callable(agent.tool_use_behavior):
            if inspect.iscoroutinefunction(agent.tool_use_behavior):
                return await cast(
                    Awaitable[ToolsToFinalOutputResult],
                    agent.tool_use_behavior(context_wrapper, tool_results),
                )
            else:
                return cast(
                    ToolsToFinalOutputResult, agent.tool_use_behavior(context_wrapper, tool_results)
                )

        logger.error(f"Invalid tool_use_behavior: {agent.tool_use_behavior}")
        raise UserError(f"Invalid tool_use_behavior: {agent.tool_use_behavior}")


class TraceCtxManager:
    """Creates a trace only if there is no current trace, and manages the trace lifecycle."""

    def __init__(
        self,
        workflow_name: str,
        trace_id: str | None,
        group_id: str | None,
        metadata: dict[str, Any] | None,
        disabled: bool,
    ):
        self.trace: Trace | None = None
        self.workflow_name = workflow_name
        self.trace_id = trace_id
        self.group_id = group_id
        self.metadata = metadata
        self.disabled = disabled

    def __enter__(self) -> TraceCtxManager:
        current_trace = get_current_trace()
        if not current_trace:
            self.trace = trace(
                workflow_name=self.workflow_name,
                trace_id=self.trace_id,
                group_id=self.group_id,
                metadata=self.metadata,
                disabled=self.disabled,
            )
            self.trace.start(mark_as_current=True)

        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.trace:
            self.trace.finish(reset_current=exc_type is not GeneratorExit)


class ComputerAction:
    @classmethod
    async def execute(
        cls,
        *,
        agent: Agent[TContext],
        action: ToolRunComputerAction,
        hooks: RunHooks[TContext],
        context_wrapper: RunContextWrapper[TContext],
        config: RunConfig,
    ) -> RunItem:
        output_func = (
            cls._get_screenshot_async(action.computer_tool.computer, action.tool_call)
            if isinstance(action.computer_tool.computer, AsyncComputer)
            else cls._get_screenshot_sync(action.computer_tool.computer, action.tool_call)
        )

        _, _, output = await asyncio.gather(
            hooks.on_tool_start(context_wrapper, agent, action.computer_tool),
            (
                agent.hooks.on_tool_start(context_wrapper, agent, action.computer_tool)
                if agent.hooks
                else _coro.noop_coroutine()
            ),
            output_func,
        )

        await asyncio.gather(
            hooks.on_tool_end(context_wrapper, agent, action.computer_tool, output),
            (
                agent.hooks.on_tool_end(context_wrapper, agent, action.computer_tool, output)
                if agent.hooks
                else _coro.noop_coroutine()
            ),
        )

        # TODO: don't send a screenshot every single time, use references
        image_url = f"data:image/png;base64,{output}"
        return ToolCallOutputItem(
            agent=agent,
            output=image_url,
            raw_item=ComputerCallOutput(
                call_id=action.tool_call.call_id,
                output={
                    "type": "computer_screenshot",
                    "image_url": image_url,
                },
                type="computer_call_output",
            ),
        )

    @classmethod
    async def _get_screenshot_sync(
        cls,
        computer: Computer,
        tool_call: ResponseComputerToolCall,
    ) -> str:
        action = tool_call.action
        if isinstance(action, ActionClick):
            computer.click(action.x, action.y, action.button)
        elif isinstance(action, ActionDoubleClick):
            computer.double_click(action.x, action.y)
        elif isinstance(action, ActionDrag):
            computer.drag([(p.x, p.y) for p in action.path])
        elif isinstance(action, ActionKeypress):
            computer.keypress(action.keys)
        elif isinstance(action, ActionMove):
            computer.move(action.x, action.y)
        elif isinstance(action, ActionScreenshot):
            computer.screenshot()
        elif isinstance(action, ActionScroll):
            computer.scroll(action.x, action.y, action.scroll_x, action.scroll_y)
        elif isinstance(action, ActionType):
            computer.type(action.text)
        elif isinstance(action, ActionWait):
            computer.wait()

        return computer.screenshot()

    @classmethod
    async def _get_screenshot_async(
        cls,
        computer: AsyncComputer,
        tool_call: ResponseComputerToolCall,
    ) -> str:
        action = tool_call.action
        if isinstance(action, ActionClick):
            await computer.click(action.x, action.y, action.button)
        elif isinstance(action, ActionDoubleClick):
            await computer.double_click(action.x, action.y)
        elif isinstance(action, ActionDrag):
            await computer.drag([(p.x, p.y) for p in action.path])
        elif isinstance(action, ActionKeypress):
            await computer.keypress(action.keys)
        elif isinstance(action, ActionMove):
            await computer.move(action.x, action.y)
        elif isinstance(action, ActionScreenshot):
            await computer.screenshot()
        elif isinstance(action, ActionScroll):
            await computer.scroll(action.x, action.y, action.scroll_x, action.scroll_y)
        elif isinstance(action, ActionType):
            await computer.type(action.text)
        elif isinstance(action, ActionWait):
            await computer.wait()

        return await computer.screenshot()
