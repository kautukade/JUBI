"""Retester Agent for vulnerability verification and triage"""

from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from openai import AsyncOpenAI
from cai.util import load_prompt_template, create_system_prompt_renderer
from cai.config import get_config
from cai.tools.reconnaissance.generic_linux_command import (  # pylint: disable=import-error # noqa: E501
generic_linux_command,
)
from cai.tools.reconnaissance.exec_code import (  # pylint: disable=import-error # noqa: E501
    execute_code,
)
from cai.tools.web.search_web import (  # pylint: disable=import-error # noqa: E501
    make_google_search,
)


load_dotenv()
_cfg = get_config()

# Load the triage agent system prompt
retester_system_prompt = load_prompt_template("prompts/system_triage_agent.md")

tools = [generic_linux_command, execute_code]

if _cfg.google_search_api_key and _cfg.google_search_cx:
    tools.append(make_google_search)

retester_agent = Agent(
    name="Retester Agent",
    instructions=create_system_prompt_renderer(
        retester_system_prompt,
        cyber_micro_profile_key="triage",
    ),
    description="""Agent that specializes in vulnerability verification and
                   triage. Expert in determining exploitability and
                   eliminating false positives.""",
    tools=tools,
    model=OpenAIChatCompletionsModel(
        model=_cfg.model,
        openai_client=AsyncOpenAI(),
    ),
)
