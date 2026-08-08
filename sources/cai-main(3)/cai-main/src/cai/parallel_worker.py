"""External terminal worker for CLI parallel execution.

Runs one agent+prompt pair and writes a JSON result payload for the
main CLI process to aggregate.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from dataclasses import dataclass

from cai.agents import get_agent_by_name
from cai.sdk.agents import Runner, set_tracing_disabled
from cai.util import update_agent_models_recursively
from cai.util.pricing import COST_TRACKER
from cai.util.tokens import get_model_input_tokens


@dataclass
class WorkerArgs:
    agent: str
    agent_id: str
    model: str
    prompt: str
    result_file: str


def _parse_args() -> WorkerArgs:
    parser = argparse.ArgumentParser(description="CAI external parallel worker")
    parser.add_argument("--agent", required=True)
    parser.add_argument("--agent-id", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--prompt", required=True)
    parser.add_argument("--result-file", required=True)
    ns = parser.parse_args()
    return WorkerArgs(
        agent=ns.agent,
        agent_id=ns.agent_id,
        model=ns.model,
        prompt=ns.prompt,
        result_file=ns.result_file,
    )


async def _run(args: WorkerArgs) -> dict:
    os.environ["CAI_STREAM"] = "true"
    os.environ["CAI_TOOL_STREAM"] = "true"
    # Avoid non-fatal tracing noise in detached worker terminals (e.g. OPENAI_API_KEY placeholder).
    os.environ["CAI_TRACING"] = "false"
    set_tracing_disabled(True)
    boot = os.environ.get("CAI_PARALLEL_MCP_BOOTSTRAP", "").strip()
    if boot and os.path.isfile(boot):
        try:
            from cai.repl.commands.mcp import apply_parallel_mcp_bootstrap_file

            await apply_parallel_mcp_bootstrap_file(boot)
        except Exception as e:  # pylint: disable=broad-except
            print(f"[CAI worker] MCP bootstrap skipped: {e}", file=sys.stderr)
    agent = get_agent_by_name(
        args.agent,
        custom_name=f"{args.agent} [{args.agent_id}]",
        model_override=args.model,
        agent_id=args.agent_id,
    )
    update_agent_models_recursively(agent, args.model)
    result = await Runner.run(agent, args.prompt)
    final_output = getattr(result, "final_output", "")
    history = []
    try:
        if hasattr(agent, "model") and hasattr(agent.model, "message_history"):
            history = [
                dict(msg) if isinstance(msg, dict) else msg
                for msg in (agent.model.message_history or [])
            ]
    except Exception:
        history = []
    usage = getattr(result, "usage", None)
    in_tokens = int(getattr(usage, "input_tokens", 0) or 0)
    out_tokens = int(getattr(usage, "output_tokens", 0) or 0)
    # Fallback for providers/paths that don't populate result.usage consistently.
    if in_tokens <= 0:
        in_tokens = int(getattr(COST_TRACKER, "interaction_input_tokens", 0) or 0)
    if out_tokens <= 0:
        out_tokens = int(getattr(COST_TRACKER, "interaction_output_tokens", 0) or 0)
    model_max = int(get_model_input_tokens(args.model) or 0)
    context_pct = (in_tokens / model_max * 100.0) if model_max > 0 else 0.0
    return {
        "status": "ok",
        "agent": args.agent,
        "agent_id": args.agent_id,
        "model": args.model,
        "final_output": str(final_output) if final_output is not None else "",
        "history": history,
        "usage": {
            "input_tokens": in_tokens,
            "output_tokens": out_tokens,
            "max_input_tokens": model_max,
            "context_pct": context_pct,
        },
        "cost": {
            "session_total_cost": float(getattr(COST_TRACKER, "session_total_cost", 0.0) or 0.0),
            "last_interaction_cost": float(getattr(COST_TRACKER, "last_interaction_cost", 0.0) or 0.0),
        },
    }


def main() -> int:
    args = _parse_args()
    payload: dict
    try:
        payload = asyncio.run(_run(args))
    except asyncio.CancelledError:
        payload = {
            "status": "cancelled",
            "agent": args.agent,
            "agent_id": args.agent_id,
            "model": args.model,
            "error": "Worker execution cancelled",
        }
    except Exception as e:  # pylint: disable=broad-except
        payload = {
            "status": "error",
            "agent": args.agent,
            "agent_id": args.agent_id,
            "model": args.model,
            "error": str(e),
        }

    try:
        with open(args.result_file, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False)
    except Exception as write_error:  # pylint: disable=broad-except
        print(f"[CAI worker] failed to write result file: {write_error}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

