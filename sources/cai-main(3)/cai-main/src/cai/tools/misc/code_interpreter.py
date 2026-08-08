"""
Module for executing Python code and capturing its output.
"""

import io
import json
import sys
from typing import Optional
from cai.sdk.agents import function_tool


@function_tool
def execute_python_code(code: str, context: Optional[str] = None) -> str:
    """
    Execute Python code and return the output.

    Args:
        code (str): Python code to execute
        context (str, optional): Additional context for execution as a JSON string
            (e.g. '{"x": 1, "y": 2}')

    Returns:
        str: Output from code execution
    """
    try:
        local_vars = {}
        if context:
            try:
                local_vars.update(json.loads(context))
            except (json.JSONDecodeError, TypeError):
                pass

        # Capture output using StringIO
        stdout = io.StringIO()
        sys.stdout = stdout

        # Execute code once with captured output
        # nosec B102 # pylint: disable=exec-used
        exec(code, globals(), local_vars)  # nosec 102

        # Restore stdout
        sys.stdout = sys.__stdout__
        output = stdout.getvalue()

        # Return captured output or last expression value
        return output if output else str(local_vars.get("__builtins__", {}).get("_", None))

    except Exception as e:  # pylint: disable=broad-except
        return f"Error executing code: {str(e)}"


# --- Auto-register with ToolRegistry ---
from cai.tool_registry import TOOL_REGISTRY  # noqa: E402
TOOL_REGISTRY.register("execute_python_code", execute_python_code, categories=["misc"])
