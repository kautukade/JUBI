"""Reverse Engineering and Binary Analysis Agent"""

import os
from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel  # pylint: disable=import-error
from openai import AsyncOpenAI
from cai.util import create_system_prompt_renderer, load_prompt_template
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

load_dotenv()
# Prompts
reverse_engineering_agent_system_prompt = load_prompt_template(
    "prompts/reverse_engineering_agent.md"
)

# Define functions list
functions = [
    generic_linux_command,
    run_ssh_command_with_credentials,
    execute_code,
    *WEB_INTEL_TOOLS,
]

# Add make_web_search_with_explanation function if PERPLEXITY_API_KEY environment variable is set
if os.getenv("PERPLEXITY_API_KEY"):
    functions.append(make_web_search_with_explanation)

# Create the agent
reverse_engineering_agent = Agent(
    name="Reverse Engineering Specialist",
    instructions=create_system_prompt_renderer(
        reverse_engineering_agent_system_prompt + WEB_INTEL_PROMPT_HARDENING,
        cyber_micro_profile_key="reverse",
    ),
    description="""Agent for binary analysis and reverse engineering.
                   Specializes in firmware analysis, binary disassembly,
                   decompilation, and vulnerability discovery using tools
                   like Ghidra, Binwalk, and various binary analysis utilities.""",
    tools=functions,
    model=OpenAIChatCompletionsModel(
        model=os.getenv("CAI_MODEL", "alias1"),
        openai_client=AsyncOpenAI(),
    ),
)
