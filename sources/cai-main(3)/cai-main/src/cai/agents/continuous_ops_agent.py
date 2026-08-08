"""Continuous Ops agent — CLI wizard for 24/7-style periodic cybersecurity workloads."""

from __future__ import annotations

from dotenv import load_dotenv
from openai import AsyncOpenAI

from cai.agents.guardrails import get_security_guardrails
from cai.config import get_config
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from cai.tools.misc.reasoning import think
from cai.tools.reconnaissance.generic_linux_command import (  # pylint: disable=import-error # noqa: E501
    generic_linux_command,
)
from cai.tools.web.search_web import (  # pylint: disable=import-error # noqa: E501
    make_web_search_with_explanation,
)
from cai.util import create_system_prompt_renderer, load_prompt_template

load_dotenv()
_cfg = get_config()

continuous_ops_agent_system_prompt = load_prompt_template("prompts/system_continuous_ops_agent.md")

tools = [generic_linux_command, think]

if _cfg.perplexity_api_key:
    tools.append(make_web_search_with_explanation)

input_guardrails, output_guardrails = get_security_guardrails()

continuous_ops_agent = Agent(
    name="Continuous Ops Agent",
    description="""Plan and launch periodic (24/7-style) cybersecurity monitoring tasks: CLI wizard
validates API-safe tick intervals, tmux detach, privilege policy, then runs a Selection-Agent worker loop.""",
    instructions=create_system_prompt_renderer(
        continuous_ops_agent_system_prompt,
        cyber_micro_profile_key="continuous_ops",
    ),
    tools=tools,
    input_guardrails=input_guardrails,
    output_guardrails=output_guardrails,
    model=OpenAIChatCompletionsModel(
        model=_cfg.model,
        openai_client=AsyncOpenAI(),
        agent_name="Continuous Ops Agent",
        agent_type="continuous_ops_agent",
    ),
)


def transfer_to_continuous_ops_agent(**kwargs):  # pylint: disable=W0613
    """Hand off to the Continuous Ops agent."""
    return continuous_ops_agent
