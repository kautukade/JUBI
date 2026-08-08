"""Message formatting for the ChatCompletions API.

Contains the _Converter class (items -> messages) and ToolConverter
(Tool -> ChatCompletionToolParam).  Extracted from
openai_chatcompletions.py for cohesion.
"""

from __future__ import annotations

import inspect
import json
import os
import time
import uuid
from collections.abc import Iterable
from typing import Any, cast

from openai.types.chat import (
    ChatCompletionAssistantMessageParam,
    ChatCompletionContentPartImageParam,
    ChatCompletionContentPartParam,
    ChatCompletionContentPartTextParam,
    ChatCompletionDeveloperMessageParam,
    ChatCompletionMessage,
    ChatCompletionMessageParam,
    ChatCompletionMessageToolCallParam,
    ChatCompletionSystemMessageParam,
    ChatCompletionToolMessageParam,
    ChatCompletionUserMessageParam,
)
from openai.types.chat.chat_completion_tool_param import ChatCompletionToolParam
from openai.types.responses import (
    ResponseFileSearchToolCallParam,
    ResponseFunctionToolCall,
    ResponseFunctionToolCallParam,
    ResponseInputContentParam,
    ResponseInputImageParam,
    ResponseInputTextParam,
    ResponseOutputMessage,
    ResponseOutputMessageParam,
    ResponseOutputRefusal,
    ResponseOutputText,
    EasyInputMessageParam,
)
from openai.types.responses.response_input_param import FunctionCallOutput, ItemReference, Message

from ...exceptions import AgentsException, UserError
from ...handoffs import Handoff
from ...items import TResponseInputItem, TResponseOutputItem
from ...logger import logger
from ...tool import FunctionTool, Tool
from ..fake_id import FAKE_RESPONSES_ID


class Converter:
    """Convert SDK item types to/from ChatCompletion message params."""

    def __init__(self):
        """Initialize converter with instance-based state."""
        self.recent_tool_calls = {}
        self.tool_outputs = {}

    # ------------------------------------------------------------------
    # Tool choice / response format helpers
    # ------------------------------------------------------------------

    def convert_tool_choice(self, tool_choice):
        if tool_choice is None:
            return "auto"
        elif tool_choice == "auto":
            return "auto"
        elif tool_choice == "required":
            return "required"
        elif tool_choice == "none":
            return "none"
        else:
            return {
                "type": "function",
                "function": {
                    "name": tool_choice,
                },
            }

    def convert_response_format(self, final_output_schema):
        if not final_output_schema or final_output_schema.is_plain_text():
            return None
        return {
            "type": "json_schema",
            "json_schema": {
                "name": "final_output",
                "strict": final_output_schema.strict_json_schema,
                "schema": final_output_schema.json_schema(),
            },
        }

    # ------------------------------------------------------------------
    # Malformed tool-call recovery
    # ------------------------------------------------------------------

    def _parse_malformed_tool_call(self, text: str) -> tuple[str, dict] | None:
        """Detect and parse malformed tool calls embedded in text content.

        Some LLMs return tool calls as XML-like text instead of proper function calls.
        Supports two emission formats:

        1) "<arg_key>k</arg_key><arg_value>v</arg_value>" (legacy/older Qwen-style):
               <tool_call>NAME ... <arg_key>k</arg_key><arg_value>v</arg_value> ...

        2) "<parameter=key>value</parameter>" (alias1 unrestricted / "abliteration"
           steering):
               <tool_call>
               &function=NAME>
               <parameter=key1>
               value1
               </parameter>
               <parameter=key2>
               value2
               </parameter>
               </function>
               </tool_call>

        Returns (tool_name, arguments_dict) if found, None otherwise.
        """
        if "<tool_call>" not in text:
            return None

        try:
            import re

            args: dict[str, Any] = {}

            # Restrict parsing to the first ``<tool_call>...</tool_call>`` block
            # so narrative text mentioning ``&function=…`` outside the block
            # cannot be misread as a tool invocation.
            block_match = re.search(r'<tool_call>(.*?)</tool_call>', text, re.DOTALL)
            block = block_match.group(1) if block_match else text

            # Tool name: try "&function=NAME" / "<function=NAME>" first (format 2),
            # then fall back to "<tool_call>NAME" (format 1).
            func_match = re.search(r'(?:&|<)function=(\w+)', block)
            if func_match:
                tool_name = func_match.group(1)
            else:
                tool_match = re.search(r'<tool_call>(\w+)', text)
                if not tool_match:
                    return None
                tool_name = tool_match.group(1)

            def _coerce(value: str) -> Any:
                v = value.strip()
                if v == "null":
                    return None
                if v.lower() == "false":
                    return False
                if v.lower() == "true":
                    return True
                if v.isdigit():
                    return int(v)
                return v

            # Format 2: <parameter=KEY>...</parameter> blocks (multiline tolerant).
            param_pattern = re.compile(
                r'<parameter=(\w+)>\s*(.*?)\s*</parameter>', re.DOTALL
            )
            for match in param_pattern.finditer(block):
                key, value = match.groups()
                args[key] = _coerce(value)

            # Format 1: <arg_key>K</arg_key><arg_value>V</arg_value> pairs.
            if not args:
                arg_pattern = r'<arg_key>(\w+)</arg_key><arg_value>([^<]*)</arg_value>'
                for match in re.finditer(arg_pattern, block):
                    key, value = match.groups()
                    args[key] = _coerce(value)

                if tool_name and "command" not in args:
                    cmd_match = re.search(r'<arg_value>([^<]+)</arg_value>', block)
                    if cmd_match:
                        args["command"] = cmd_match.group(1)

            if tool_name:
                return (tool_name, args)

            return None
        except Exception:
            return None

    # ------------------------------------------------------------------
    # Output items
    # ------------------------------------------------------------------

    def message_to_output_items(
        self, message: ChatCompletionMessage
    ) -> list[TResponseOutputItem]:
        items: list[TResponseOutputItem] = []

        message_item = ResponseOutputMessage(
            id=FAKE_RESPONSES_ID,
            content=[],
            role="assistant",
            type="message",
            status="completed",
        )

        parsed_tool_call = None
        if message.content and "<tool_call>" in message.content:
            parsed_tool_call = self._parse_malformed_tool_call(message.content)
            if parsed_tool_call:
                logger.warning(
                    f"Detected malformed tool call in text content, "
                    f"converting to proper function call: {parsed_tool_call[0]}"
                )

        if message.content:
            message_item.content.append(
                ResponseOutputText(text=message.content, type="output_text", annotations=[])
            )
        if hasattr(message, "refusal") and message.refusal:
            message_item.content.append(
                ResponseOutputRefusal(refusal=message.refusal, type="refusal")
            )
        if hasattr(message, "audio") and message.audio:
            raise AgentsException("Audio output not supported - Text responses only")

        if message_item.content:
            items.append(message_item)

        if hasattr(message, "tool_calls") and message.tool_calls:
            for tool_call in message.tool_calls:
                items.append(
                    ResponseFunctionToolCall(
                        id=FAKE_RESPONSES_ID,
                        call_id=tool_call.id[:40],
                        arguments=tool_call.function.arguments,
                        name=tool_call.function.name,
                        type="function_call",
                    )
                )
        elif parsed_tool_call:
            tool_name, tool_args = parsed_tool_call
            items.append(
                ResponseFunctionToolCall(
                    id=FAKE_RESPONSES_ID,
                    call_id=uuid.uuid4().hex[:16],
                    arguments=json.dumps(tool_args),
                    name=tool_name,
                    type="function_call",
                )
            )

        return items

    # ------------------------------------------------------------------
    # Item type detectors
    # ------------------------------------------------------------------

    def maybe_easy_input_message(self, item: Any) -> EasyInputMessageParam | None:
        if not isinstance(item, dict):
            return None
        if item.keys() != {"content", "role"}:
            return None
        role = item.get("role", None)
        if role not in ("user", "assistant", "system", "developer"):
            return None
        if "content" not in item:
            return None
        return cast(EasyInputMessageParam, item)

    def maybe_input_message(self, item: Any) -> Message | None:
        if (
            isinstance(item, dict)
            and item.get("type") == "message"
            and item.get("role") in ("user", "system", "developer")
        ):
            return cast(Message, item)
        return None

    def maybe_file_search_call(self, item: Any) -> ResponseFileSearchToolCallParam | None:
        if isinstance(item, dict) and item.get("type") == "file_search_call":
            return cast(ResponseFileSearchToolCallParam, item)
        return None

    def maybe_function_tool_call(self, item: Any) -> ResponseFunctionToolCallParam | None:
        if isinstance(item, dict) and item.get("type") == "function_call":
            return cast(ResponseFunctionToolCallParam, item)
        return None

    def maybe_function_tool_call_output(self, item: Any) -> FunctionCallOutput | None:
        if isinstance(item, dict) and item.get("type") == "function_call_output":
            return cast(FunctionCallOutput, item)
        return None

    def maybe_item_reference(self, item: Any) -> ItemReference | None:
        if isinstance(item, dict) and item.get("type") == "item_reference":
            return cast(ItemReference, item)
        return None

    def maybe_response_output_message(self, item: Any) -> ResponseOutputMessageParam | None:
        if (
            isinstance(item, dict)
            and item.get("type") == "message"
            and item.get("role") == "assistant"
        ):
            return cast(ResponseOutputMessageParam, item)
        return None

    # ------------------------------------------------------------------
    # Content extraction
    # ------------------------------------------------------------------

    def extract_text_content(self, content):
        all_content = self.extract_all_content(content)
        if isinstance(all_content, str):
            return all_content
        out = []
        for c in all_content:
            if c.get("type") == "text":
                out.append(cast(ChatCompletionContentPartTextParam, c))
        return out

    def extract_all_content(self, content):
        if isinstance(content, str):
            return content
        out: list[ChatCompletionContentPartParam] = []
        for c in content:
            if isinstance(c, dict) and c.get("type") == "input_text":
                casted_text_param = cast(ResponseInputTextParam, c)
                out.append(
                    ChatCompletionContentPartTextParam(
                        type="text",
                        text=casted_text_param["text"],
                    )
                )
            elif isinstance(c, dict) and c.get("type") == "input_image":
                casted_image_param = cast(ResponseInputImageParam, c)
                if "image_url" not in casted_image_param or not casted_image_param["image_url"]:
                    raise UserError("Image URLs required - Upload images to a URL first")
                out.append(
                    ChatCompletionContentPartImageParam(
                        type="image_url",
                        image_url={
                            "url": casted_image_param["image_url"],
                            "detail": casted_image_param["detail"],
                        },
                    )
                )
            elif isinstance(c, dict) and c.get("type") == "input_file":
                raise UserError("File uploads not supported - Use image URLs or text content")
            else:
                raise UserError("Unrecognized content type - Expected 'input_text' or 'input_image'")
        return out

    # ------------------------------------------------------------------
    # items_to_messages -- the main conversion pipeline
    # ------------------------------------------------------------------

    def items_to_messages(
        self,
        items: str | Iterable[TResponseInputItem],
        model_instance=None,
    ) -> list[ChatCompletionMessageParam]:
        """Convert a sequence of 'Item' objects into ChatCompletionMessageParam list."""

        if isinstance(items, str):
            return [ChatCompletionUserMessageParam(role="user", content=items)]

        result: list[ChatCompletionMessageParam] = []
        current_assistant_msg: ChatCompletionAssistantMessageParam | None = None

        def flush_assistant_message() -> None:
            nonlocal current_assistant_msg
            if current_assistant_msg is not None:
                if not current_assistant_msg.get("tool_calls"):
                    if current_assistant_msg.get("content") is None:
                        current_assistant_msg["content"] = (
                            "(No text content in this assistant message)"
                        )
                    current_assistant_msg.pop("tool_calls", None)
                result.append(current_assistant_msg)
                current_assistant_msg = None

        def ensure_assistant_message() -> ChatCompletionAssistantMessageParam:
            nonlocal current_assistant_msg
            if current_assistant_msg is None:
                current_assistant_msg = ChatCompletionAssistantMessageParam(role="assistant")
                current_assistant_msg["tool_calls"] = []
            return current_assistant_msg

        for item in items:
            # Handle 'tool' messages from history
            if (
                isinstance(item, dict)
                and item.get("role") == "tool"
                and "tool_call_id" in item
                and "content" in item
            ):
                flush_assistant_message()
                tool_message: ChatCompletionToolMessageParam = {
                    "role": "tool",
                    "tool_call_id": item["tool_call_id"],
                    "content": str(item["content"] or ""),
                }
                result.append(tool_message)
                continue

            # Assistant messages with tool_calls only (from memory)
            if (
                isinstance(item, dict)
                and item.get("role") == "assistant"
                and item.get("tool_calls")
            ):
                flush_assistant_message()
                tool_calls_param: list[ChatCompletionMessageToolCallParam] = []
                for tc in item["tool_calls"]:
                    function_details = tc.get("function", {})
                    name = (function_details.get("name") or "").strip()
                    if not name:
                        # If we don't have a valid function name, don't emit a synthetic
                        # placeholder tool call. A fake name like "unknown_function"
                        # can later be treated as a real tool invocation and crash.
                        continue
                    arguments = function_details.get("arguments")
                    if arguments is None or (isinstance(arguments, str) and arguments.strip() == ""):
                        arguments = "{}"
                    elif isinstance(arguments, dict):
                        arguments = json.dumps(arguments)
                    tool_calls_param.append(
                        ChatCompletionMessageToolCallParam(
                            id=tc.get("id", "")[:40],
                            type=tc.get("type", "function"),
                            function={
                                "name": name,
                                "arguments": arguments,
                            },
                        )
                    )
                if not tool_calls_param:
                    # Nothing to send (all tool calls were missing function names).
                    continue
                msg_asst: ChatCompletionAssistantMessageParam = {
                    "role": "assistant",
                    "content": item.get("content"),
                    "tool_calls": tool_calls_param,
                }
                result.append(msg_asst)
                continue

            # 1) Easy input message
            if easy_msg := self.maybe_easy_input_message(item):
                role = easy_msg["role"]
                content = easy_msg["content"]
                if role == "user":
                    flush_assistant_message()
                    result.append({"role": "user", "content": self.extract_all_content(content)})
                elif role == "system":
                    flush_assistant_message()
                    result.append({"role": "system", "content": self.extract_text_content(content)})
                elif role == "developer":
                    flush_assistant_message()
                    result.append({"role": "developer", "content": self.extract_text_content(content)})
                elif role == "assistant":
                    flush_assistant_message()
                    result.append({"role": "assistant", "content": self.extract_text_content(content)})
                else:
                    raise UserError(
                        f"Invalid role '{role}' - Use: user, assistant, system, or developer"
                    )

            # 2) Input message
            elif in_msg := self.maybe_input_message(item):
                role = in_msg["role"]
                content = in_msg["content"]
                flush_assistant_message()
                if role == "user":
                    result.append({"role": "user", "content": self.extract_all_content(content)})
                elif role == "system":
                    result.append({"role": "system", "content": self.extract_text_content(content)})
                elif role == "developer":
                    result.append({"role": "developer", "content": self.extract_text_content(content)})
                else:
                    raise UserError(
                        f"Invalid message role '{role}' - Must be: user, system, or developer"
                    )

            # 3) Response output message => assistant
            elif resp_msg := self.maybe_response_output_message(item):
                flush_assistant_message()
                new_asst = ChatCompletionAssistantMessageParam(role="assistant")
                contents = resp_msg["content"]
                text_segments = []
                for c in contents:
                    if c["type"] == "output_text":
                        text_segments.append(c["text"])
                    elif c["type"] == "refusal":
                        new_asst["refusal"] = c["refusal"]
                    elif c["type"] == "output_audio":
                        raise UserError(
                            "Audio content must use audio IDs - Direct audio data not supported"
                        )
                    else:
                        raise UserError(
                            "Unknown assistant message content - Check message format"
                        )
                if text_segments:
                    new_asst["content"] = "\n".join(text_segments)
                new_asst["tool_calls"] = []
                current_assistant_msg = new_asst

            # 4) Function/file-search calls => attach to assistant
            elif file_search := self.maybe_file_search_call(item):
                asst = ensure_assistant_message()
                tool_calls = list(asst.get("tool_calls", []))
                new_tool_call = ChatCompletionMessageToolCallParam(
                    id=file_search["id"][:40],
                    type="function",
                    function={
                        "name": "file_search_call",
                        "arguments": json.dumps(
                            {
                                "queries": file_search.get("queries", []),
                                "status": file_search.get("status"),
                            }
                        ),
                    },
                )
                tool_calls.append(new_tool_call)
                asst["tool_calls"] = tool_calls

            elif func_call := self.maybe_function_tool_call(item):
                asst = ensure_assistant_message()
                tool_calls = list(asst.get("tool_calls", []))

                current_time = time.time()
                # Periodic cleanup of old tool calls (older than 5 minutes)
                if len(self.recent_tool_calls) > 50:
                    stale_threshold = current_time - 300
                    stale_keys = [
                        k
                        for k, v in self.recent_tool_calls.items()
                        if v.get("start_time", 0) < stale_threshold
                    ]
                    for k in stale_keys:
                        del self.recent_tool_calls[k]

                self.recent_tool_calls[func_call["call_id"]] = {
                    "name": func_call["name"],
                    "arguments": func_call["arguments"],
                    "start_time": current_time,
                    "execution_info": {"start_time": current_time},
                }

                arguments = func_call.get("arguments")
                if arguments is None or (isinstance(arguments, str) and arguments.strip() == ""):
                    arguments = "{}"
                elif isinstance(arguments, dict):
                    arguments = json.dumps(arguments)

                new_tool_call = ChatCompletionMessageToolCallParam(
                    id=func_call["call_id"][:40],
                    type="function",
                    function={
                        "name": func_call["name"],
                        "arguments": arguments,
                    },
                )
                tool_calls.append(new_tool_call)
                asst["tool_calls"] = tool_calls

            # 5) Function call output => tool message
            elif func_output := self.maybe_function_tool_call_output(item):
                call_id = func_output["call_id"]
                output_content = func_output["output"]
                truncated_call_id = call_id[:40] if call_id else call_id

                # Update execution timing
                if call_id in self.recent_tool_calls:
                    tool_call_details = self.recent_tool_calls[call_id]
                    if "start_time" in tool_call_details:
                        end_time = time.time()
                        tool_execution_time = end_time - tool_call_details["start_time"]
                        if "execution_info" in tool_call_details:
                            tool_call_details["execution_info"]["end_time"] = end_time
                            tool_call_details["execution_info"]["tool_time"] = tool_execution_time
                            if not hasattr(self, "conversation_start_time"):
                                self.conversation_start_time = tool_call_details["start_time"]
                            total_time = end_time - getattr(
                                self, "conversation_start_time", tool_call_details["start_time"]
                            )
                            tool_call_details["execution_info"]["total_time"] = total_time

                self.tool_outputs[call_id] = output_content

                # Display tool output
                from cai.util import cli_print_tool_output

                tool_name = "Unknown Tool"
                tool_args = {}
                execution_info = {}

                if call_id in self.recent_tool_calls:
                    tool_call_details = self.recent_tool_calls[call_id]
                    tool_name = tool_call_details.get("name", "Unknown Tool")
                    tool_args = tool_call_details.get("arguments", {})
                    execution_info = tool_call_details.get("execution_info", {})

                # Get token counts from the OpenAIChatCompletionsModel if available
                model_inst = None
                for frame in inspect.stack():
                    if "self" in frame.frame.f_locals:
                        self_obj = frame.frame.f_locals["self"]
                        # Avoid circular import by checking class name
                        if type(self_obj).__name__ == "OpenAIChatCompletionsModel":
                            model_inst = self_obj
                            break

                token_info = {
                    "interaction_input_tokens": getattr(model_inst, "interaction_input_tokens", 0),
                    "interaction_output_tokens": getattr(model_inst, "interaction_output_tokens", 0),
                    "interaction_reasoning_tokens": getattr(model_inst, "interaction_reasoning_tokens", 0),
                    "total_input_tokens": getattr(model_inst, "total_input_tokens", 0),
                    "total_output_tokens": getattr(model_inst, "total_output_tokens", 0),
                    "total_reasoning_tokens": getattr(model_inst, "total_reasoning_tokens", 0),
                    "cache_read_tokens": getattr(model_inst, "cache_read_tokens", 0),
                    "cache_creation_tokens": getattr(model_inst, "cache_creation_tokens", 0),
                    "model": str(getattr(model_inst, "model", "")),
                    "agent_name": getattr(model_inst, "agent_name", "Agent"),
                }

                if model_inst and hasattr(model_inst, "model"):
                    from cai.util import COST_TRACKER

                    token_info["interaction_cost"] = getattr(COST_TRACKER, "last_interaction_cost", 0.0)
                    token_info["total_cost"] = getattr(COST_TRACKER, "last_total_cost", 0.0)

                from cai.util import is_tool_streaming_enabled

                is_streaming_enabled = is_tool_streaming_enabled()
                should_display = True

                if (
                    is_streaming_enabled
                    and call_id in self.recent_tool_calls
                ):
                    tool_call_info = self.recent_tool_calls[call_id]
                    if "start_time" in tool_call_info:
                        time_since_execution = time.time() - tool_call_info["start_time"]
                        if time_since_execution < 5.0 and "_command" in tool_name.lower():
                            try:
                                args_dict = (
                                    json.loads(tool_args)
                                    if isinstance(tool_args, str)
                                    else tool_args
                                )
                                is_session_cmd = False
                                if isinstance(args_dict, dict):
                                    if args_dict.get("session_id"):
                                        is_session_cmd = True
                                    cmd_str = str(args_dict.get("command", "")).lower()
                                    if cmd_str.startswith("session") or cmd_str.startswith("output ") or cmd_str.startswith("status "):
                                        is_session_cmd = True
                                    if " s" in cmd_str or " #" in cmd_str or cmd_str.startswith("s") or cmd_str.startswith("#"):
                                        import re

                                        if re.search(r'(^|\s)(s\d+|#\d+)', cmd_str):
                                            is_session_cmd = True
                                if not is_session_cmd:
                                    should_display = False
                            except Exception:
                                pass

                if should_display:
                    execution_info["is_final"] = True
                    execution_info["status"] = execution_info.get("status", "completed")
                    cli_print_tool_output(
                        tool_name=tool_name,
                        args=tool_args,
                        output=output_content,
                        call_id=call_id,
                        execution_info=execution_info,
                        token_info=token_info,
                    )

                flush_assistant_message()

                # ATOMIC ADDITION: Add pending tool call and response together
                if model_inst and hasattr(model_inst, "_pending_tool_calls"):
                    if call_id in model_inst._pending_tool_calls:
                        pending_msg = model_inst._pending_tool_calls[call_id]
                        model_inst.add_to_message_history(pending_msg)
                        tool_response_msg = {
                            "role": "tool",
                            "tool_call_id": truncated_call_id,
                            "content": func_output["output"],
                        }
                        model_inst.add_to_message_history(tool_response_msg)
                        del model_inst._pending_tool_calls[call_id]

                msg: ChatCompletionToolMessageParam = {
                    "role": "tool",
                    "tool_call_id": truncated_call_id,
                    "content": func_output["output"],
                }
                result.append(msg)

            # 6) Item reference
            elif self.maybe_item_reference(item):
                raise UserError("Item references not supported - Include content directly")

            # 7) Unrecognized
            else:
                raise UserError("❌ Invalid message format - Check documentation for supported types")

        flush_assistant_message()
        return result


class ToolConverter:
    """Convert Tool/Handoff objects to OpenAI ChatCompletionToolParam format."""

    @classmethod
    def to_openai(cls, tool: Tool) -> ChatCompletionToolParam:
        if isinstance(tool, FunctionTool):
            return {
                "type": "function",
                "function": {
                    "name": tool.name,
                    "description": tool.description or "",
                    "parameters": tool.params_json_schema,
                },
            }
        raise UserError(
            f"Hosted tools are not supported with the ChatCompletions API. "
            f"Got tool type: {type(tool)}, tool: {tool}"
        )

    @classmethod
    def convert_handoff_tool(cls, handoff: Handoff[Any]) -> ChatCompletionToolParam:
        return {
            "type": "function",
            "function": {
                "name": handoff.tool_name,
                "description": handoff.tool_description,
                "parameters": handoff.input_json_schema,
            },
        }
