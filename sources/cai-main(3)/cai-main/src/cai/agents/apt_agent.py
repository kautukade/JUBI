"""Advanced Persistent Threat (APT) Simulation Agent

This agent emulates sophisticated nation-state adversary tactics, techniques, and
procedures (TTPs) following the MITRE ATT&CK framework. It conducts multi-stage,
stealthy operations emphasizing persistence, evasion, lateral movement, and
long-term objective achievement while maintaining operational security.
"""

import os
from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from openai import AsyncOpenAI
from cai.config import get_config

# Core tools for APT operations
from cai.tools.reconnaissance.generic_linux_command import (  # pylint: disable=import-error # noqa: E501
    generic_linux_command,
)

from cai.tools.reconnaissance.exec_code import (  # pylint: disable=import-error # noqa: E501
    execute_code,
)

from cai.agents._intel_tools import (  # pylint: disable=import-error  # noqa: E501
    WEB_INTEL_PROMPT_HARDENING,
    WEB_INTEL_TOOLS,
)

# Planning tool for multi-stage campaign tracking
from cai.tools.plan import Todo_list  # pylint: disable=import-error # noqa: E501

# Reasoning and strategic thinking tools
from cai.tools.misc.reasoning import (  # pylint: disable=import-error # noqa: E501
    think,
    thought,
    write_key_findings,
    read_key_findings,
)

# Utility imports
from cai.util import load_prompt_template, create_system_prompt_renderer
from cai.agents.guardrails import get_security_guardrails

# Load environment variables
load_dotenv()
_cfg = get_config()

# Model configuration - APT agent can use a different model if specified
# Per-agent override kept as os.getenv (by design)
model_name = os.getenv("CAI_APT_AGENT_MODEL", _cfg.model)

# Load system prompt (adapted from apt-agent-ultimate with TRACE methodology)
apt_agent_system_prompt = load_prompt_template("prompts/system_apt_agent.md")

# Core tools — API-key-gated tools (perplexity, c99) come from ToolRegistry via factory [E]
tools = [
    think,                      # Strategic thinking and complex reasoning
    thought,                    # Detailed multi-faceted analysis (breakdown, reflection, action, next_step, key_clues)
    generic_linux_command,      # Primary tool for command execution and shell sessions
    execute_code,               # Python code execution for custom tooling and automation
    write_key_findings,         # Persist critical intelligence discovered during campaign
    read_key_findings,          # Retrieve previously documented intelligence
    *WEB_INTEL_TOOLS,           # Static HTTP fetch + extract (HTML/PDF/JSON) with SSRF guard
]

# Planning tool is MANDATORY for APT operations (multi-stage campaigns)
# Only disable if explicitly set to false
if os.getenv("CAI_PLAN", "true").lower() == "true":
    tools.append(Todo_list)

# Get security guardrails to ensure responsible use
input_guardrails, output_guardrails = get_security_guardrails()

# Create the APT Agent instance
apt_agent = Agent(
    name="Advanced Persistent Threat Agent",
    description="""Elite Advanced Persistent Threat simulation agent that emulates
                   sophisticated nation-state adversary tactics. Expert in multi-stage
                   campaigns, stealth operations, persistence, lateral movement, and
                   long-term objective achievement following MITRE ATT&CK framework.
                   Specializes in low-and-slow operations with emphasis on OPSEC,
                   evasion, and maintaining access over extended periods.""",
    instructions=create_system_prompt_renderer(
        apt_agent_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="apt",
    ),
    tools=tools,
    input_guardrails=input_guardrails,
    output_guardrails=output_guardrails,
    model=OpenAIChatCompletionsModel(
        model=model_name,
        openai_client=AsyncOpenAI(),
    ),
)


# Transfer function for handoffs from other agents
def transfer_to_apt_agent(**kwargs):  # pylint: disable=W0613
    """Transfer control to the APT Agent.

    Use this when you need sophisticated, multi-stage attack simulation that
    requires stealth, persistence, and advanced evasion techniques typically
    associated with nation-state actors.

    Accepts any keyword arguments but ignores them.

    Returns:
        Agent: The APT Agent instance
    """
    return apt_agent