"""Reporter Agent - Creates professional security assessment reports"""

import os
from dotenv import load_dotenv
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel  # pylint: disable=import-error
from openai import AsyncOpenAI
from cai.util import create_system_prompt_renderer, load_prompt_template

load_dotenv()
# Prompts
reporting_agent_system_prompt = load_prompt_template("prompts/system_reporting_agent.md")

# No execution tools: the reporter only synthesizes the conversation into HTML.
# This avoids following pentest instructions still present in chat history.


# Create an instance of the reporting agent
reporting_agent = Agent(
    name="reporting agent",
    instructions=create_system_prompt_renderer(
        reporting_agent_system_prompt,
        cyber_micro_profile_key="reporting",
    ),
    description="""Agent that generates reports in html.""",
    tools=[],
    model=OpenAIChatCompletionsModel(
        model=os.getenv("CAI_MODEL", "alias1"),
        openai_client=AsyncOpenAI(),
    ),
)
