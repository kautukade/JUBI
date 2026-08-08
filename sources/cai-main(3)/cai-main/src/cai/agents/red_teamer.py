"""Red Team Base Agent

Uses CAIConfig singleton for model/API-key configuration [S].
Conditional tools loaded based on CAIConfig API-key fields.
"""

from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from openai import AsyncOpenAI
from cai.config import get_config

from cai.tools.reconnaissance.generic_linux_command import (  # pylint: disable=import-error # noqa: E501
generic_linux_command,
)
from cai.tools.web.search_web import (  # pylint: disable=import-error # noqa: E501
    make_web_search_with_explanation,
)
from cai.agents._intel_tools import (  # pylint: disable=import-error  # noqa: E501
    WEB_INTEL_PROMPT_HARDENING,
    WEB_INTEL_TOOLS,
)

from cai.tools.reconnaissance.exec_code import (  # pylint: disable=import-error # noqa: E501
    execute_code,
)
from cai.tools.reconnaissance.c99 import (  # pylint: disable=import-error # noqa: E501
    c99,
)
from cai.tools.reconnaissance.c99_subdomain import (  # pylint: disable=import-error # noqa: E501
    c99_subdomain_enum,
)

from cai.tools.plan import Todo_list  # pylint: disable=import-error # noqa: E501
from cai.util import load_prompt_template, create_system_prompt_renderer
from cai.agents.guardrails import get_security_guardrails

load_dotenv()
# Read config from CAIConfig singleton once [S]
_cfg = get_config()
model_name = _cfg.model

# Prompts
redteam_agent_system_prompt = load_prompt_template("prompts/system_red_team_agent.md")
# Define tools list based on available API keys (via CAIConfig) [S]
tools = [
    generic_linux_command,
    execute_code,
    *WEB_INTEL_TOOLS,
]

# Only expose plan tool when CAI_PLAN is enabled [S]
if _cfg.plan_enabled:
    tools.append(Todo_list)

# Add search tool if Perplexity API key is available [S]
if _cfg.perplexity_api_key:
    tools.append(make_web_search_with_explanation)

# Add C99 tools if C99 API key is available [S]
if _cfg.c99_api_key:
    tools.append(c99)
    tools.append(c99_subdomain_enum)

# Get security guardrails
input_guardrails, output_guardrails = get_security_guardrails()

redteam_agent = Agent(
    name="Red Team Agent",
    description="""Agent that mimics a red teamer in a security assessment.
                   Expert in cybersecurity, recon, and exploitation.""",
    instructions=create_system_prompt_renderer(
        redteam_agent_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="redteam",
    ),
    tools=tools,
    input_guardrails=input_guardrails,
    output_guardrails=output_guardrails,
    model=OpenAIChatCompletionsModel(
        model=model_name,
        openai_client=AsyncOpenAI(),
    ),
)


# Transfer function
def transfer_to_redteam_agent(**kwargs):  # pylint: disable=W0613
    """Transfer to red team agent.
    Accepts any keyword arguments but ignores them."""
    return redteam_agent
