"""Orchestration Agent - default entrypoint with optional dual-approach contest."""

from __future__ import annotations

from dotenv import load_dotenv
from openai import AsyncOpenAI

from cai.agents.guardrails import get_security_guardrails
from cai.config import get_config
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from cai.tools.misc.agent_discovery import (
    analyze_task_requirements,
    check_available_agents,
    get_agent_number,
)
from cai.tools.misc.approach_contest import (
    run_dual_approach_contest,
    run_parallel_specialists,
    run_specialist,
)
from cai.tools.web.search_web import make_web_search_with_explanation
from cai.util import create_system_prompt_renderer, load_prompt_template

load_dotenv()
_cfg = get_config()

_orchestration_system_prompt = load_prompt_template("prompts/system_orchestration_agent.md")

# fetch_url is intentionally NOT exposed here: the orchestrator's role is to delegate to specialists, not to perform reconnaissance itself. Letting it fetch URLs directly causes long "thinking" loops where it tries to solve the task before delegation. [evidence: debug session ab1027]

_tools = [
    check_available_agents,
    analyze_task_requirements,
    get_agent_number,
    run_dual_approach_contest,
    run_parallel_specialists,
    run_specialist,
]

if _cfg.perplexity_api_key:
    _tools.append(make_web_search_with_explanation)

_input_guardrails, _output_guardrails = get_security_guardrails()

orchestration_agent = Agent(
    name="Orchestration Agent",
    description=(
        "Default CAI orchestrator: breadth-first multi-agent delegation (parallel broad scouts, "
        "optional 2-branch contest), then narrow follow-up specialists until the user goal is met."
    ),
    instructions=create_system_prompt_renderer(
        _orchestration_system_prompt,
        cyber_micro_profile_key="selection",
    ),
    tools=_tools,
    handoffs=[],
    input_guardrails=_input_guardrails,
    output_guardrails=_output_guardrails,
    tool_use_behavior="run_llm_again",
    reset_tool_choice=True,
    model=OpenAIChatCompletionsModel(
        model=f"{_cfg.model}-thinking",
        openai_client=AsyncOpenAI(),
        agent_name="Orchestration Agent",
        agent_type="orchestration_agent",
    ),
)


def transfer_to_orchestration_agent(**kwargs):  # pylint: disable=W0613
    """Hand back to the orchestration agent."""
    return orchestration_agent
