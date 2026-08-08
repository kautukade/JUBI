"""Bug Bounty Agent

Uses CAIConfig singleton for model/API-key configuration [S].
Conditional tools loaded based on CAIConfig API-key fields.
"""

from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from openai import AsyncOpenAI
from cai.config import get_config
from cai.util import load_prompt_template, create_system_prompt_renderer
from cai.tools.reconnaissance.generic_linux_command import (  # pylint: disable=import-error # noqa: E501
generic_linux_command,
)
from cai.tools.web.search_web import (  # pylint: disable=import-error # noqa: E501
    make_google_search,
)
from cai.agents._intel_tools import (  # pylint: disable=import-error  # noqa: E501
    WEB_INTEL_PROMPT_HARDENING,
    WEB_INTEL_TOOLS,
)

from cai.tools.reconnaissance.exec_code import (  # pylint: disable=import-error # noqa: E501
    execute_code,
)

from cai.tools.reconnaissance.shodan import (  # pylint: disable=import-error # noqa: E501
    shodan_search,
    shodan_host_info,
)
from cai.tools.reconnaissance.c99 import (  # pylint: disable=import-error # noqa: E501
    c99,
)
from cai.tools.reconnaissance.c99_subdomain import (  # pylint: disable=import-error # noqa: E501
    c99_subdomain_enum,
)

from cai.tools.plan import Todo_list  # pylint: disable=import-error # noqa: E501

from cai.agents.guardrails import get_security_guardrails

load_dotenv()
# Read config from CAIConfig singleton once [S]
_cfg = get_config()

# Prompts
bug_bounter_system_prompt = load_prompt_template("prompts/system_bug_bounter.md")

tools = [generic_linux_command, execute_code, shodan_search, shodan_host_info, *WEB_INTEL_TOOLS]

# Only expose plan tool when CAI_PLAN is enabled [S]
if _cfg.plan_enabled:
    tools.append(Todo_list)

# Add Google search if both API key and CX are available [S]
if _cfg.google_search_api_key and _cfg.google_search_cx:
    tools.append(make_google_search)

# Add C99 tools if C99 API key is available [S]
if _cfg.c99_api_key:
    tools.append(c99)
    tools.append(c99_subdomain_enum)

# Get security guardrails
input_guardrails, output_guardrails = get_security_guardrails()

bug_bounter_agent = Agent(
    name="Bug Bounter",
    instructions=create_system_prompt_renderer(
        bug_bounter_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="bugbounty",
    ),
    description="""Agent that specializes in bug bounty hunting and vulnerability discovery.
                   Expert in web security, API testing, and responsible disclosure.""",
    tools=tools,
    input_guardrails=input_guardrails,
    output_guardrails=output_guardrails,
    model=OpenAIChatCompletionsModel(
        model=_cfg.model,
        openai_client=AsyncOpenAI(),
    ),
)
