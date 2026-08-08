"""Blue Team Base Agent

Uses CAIConfig singleton for model/API-key configuration [S].
Conditional tools loaded based on CAIConfig API-key fields.
SSH_PASS, SSH_HOST, SSH_USER remain as direct env vars (secrets).
"""

from openai import AsyncOpenAI
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel  # pylint: disable=import-error
from cai.util import load_prompt_template, create_system_prompt_renderer
from cai.config import get_config
from dotenv import load_dotenv
from cai.tools.command_and_control.sshpass import (  # pylint: disable=import-error # noqa: E501
    run_ssh_command_with_credentials,
)

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

from cai.tools.plan import Todo_list  # pylint: disable=import-error # noqa: E501

load_dotenv()
# Read config from CAIConfig singleton once [S]
_cfg = get_config()

# Prompts
blueteam_agent_system_prompt = load_prompt_template("prompts/system_blue_team_agent.md")
# Define tools list based on available API keys (via CAIConfig) [S]
tools = [
    generic_linux_command,
    run_ssh_command_with_credentials,
    execute_code,
    *WEB_INTEL_TOOLS,
]

# Only expose plan tool when CAI_PLAN is enabled [S]
if _cfg.plan_enabled:
    tools.append(Todo_list)

# Add search tool if Perplexity API key is available [S]
if _cfg.perplexity_api_key:
    tools.append(make_web_search_with_explanation)

blueteam_agent = Agent(
    name="Blue Team Agent",
    instructions=create_system_prompt_renderer(
        blueteam_agent_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="blueteam",
    ),
    description="""Agent that specializes in system defense and security monitoring.
                   Expert in cybersecurity protection and incident response.""",
    model=OpenAIChatCompletionsModel(
        model=_cfg.model,
        openai_client=AsyncOpenAI(),
    ),
    tools=tools,
)
