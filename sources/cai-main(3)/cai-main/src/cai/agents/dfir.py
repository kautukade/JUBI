"""DFIR Base Agent
Digital Forensics and Incident Response (DFIR) Agent module for conducting security investigations
and analyzing digital evidence. This agent specializes in:

- System and network forensics: Analyzing system artifacts, network traffic, and logs
- Malware analysis: Static and dynamic analysis of suspicious code and binaries
- Memory forensics: Examining RAM dumps for evidence of compromise
- Disk forensics: Recovering and analyzing data from storage devices
- Timeline reconstruction: Building chronological sequences of security events
- Evidence preservation: Maintaining chain of custody and forensic integrity
- Incident response: Coordinating investigation and remediation activities
- Threat hunting: Proactively searching for indicators of compromise
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

from cai.tools.reconnaissance.exec_code import (  # pylint: disable=import-error # noqa: E501
    execute_code,
)
from cai.tools.web.search_web import (  # pylint: disable=import-error # noqa: E501
    make_web_search_with_explanation,
)
from cai.agents._intel_tools import (  # pylint: disable=import-error  # noqa: E501
    WEB_INTEL_PROMPT_HARDENING,
    WEB_INTEL_TOOLS,
)
from cai.tools.reconnaissance.shodan import shodan_search
from cai.tools.web.google_search import google_search
from cai.tools.misc.reasoning import think  # pylint: disable=import-error

load_dotenv()
_cfg = get_config()

# Prompts
dfir_agent_system_prompt = load_prompt_template("prompts/system_dfir_agent.md")
# Define tool list based on available API keys (via CAIConfig) [S]
tools = [
    generic_linux_command,
    run_ssh_command_with_credentials,
    execute_code,
    think,
    *WEB_INTEL_TOOLS,
]

if _cfg.perplexity_api_key:
    tools.append(make_web_search_with_explanation)

# Add Shodan and Google search capabilities conditionally [S]
if _cfg.shodan_api_key:
    tools.append(shodan_search)

if _cfg.google_search_api_key and _cfg.google_search_cx:
    tools.append(google_search)


dfir_agent = Agent(
    name="DFIR Agent",
    instructions=create_system_prompt_renderer(
        dfir_agent_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="dfir",
    ),
    description="""Agent that specializes in Digital Forensics and Incident Response.
                   Expert in investigation and analysis of digital evidence.""",
    model=OpenAIChatCompletionsModel(
        model=_cfg.model,
        openai_client=AsyncOpenAI(),
    ),
    tools=tools,
)
