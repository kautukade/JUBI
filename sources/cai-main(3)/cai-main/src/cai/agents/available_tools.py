"""
Available Tools Registry - Maps tool names to their imports
"""

# This is a complete mapping of available tools based on the actual codebase
# Only include the tools that the user wants available
AVAILABLE_TOOLS = {
    # Core Tools - Only the ones specified by the user
"generic_linux_command": {
"import": "from cai.tools.reconnaissance.generic_linux_command import generic_linux_command",
        "category": "core",
        "description": "Execute any Linux command"
    },
    "execute_code": {
        "import": "from cai.tools.reconnaissance.exec_code import execute_code",
        "category": "core",
        "description": "Execute Python/other code"
    },
    "shodan_search": {
        "import": "from cai.tools.reconnaissance.shodan import shodan_search",
        "category": "core",
        "description": "Search Shodan for exposed services"
    },
    "shodan_host_info": {
        "import": "from cai.tools.reconnaissance.shodan import shodan_host_info",
        "category": "core",
        "description": "Get Shodan info for specific host"
    },
    "make_web_search_with_explanation": {
        "import": "from cai.tools.web.search_web import make_web_search_with_explanation",
        "category": "core",
        "description": "AI-powered web search with explanation"
    }
}

# Group tools by category for easy selection
TOOLS_BY_CATEGORY = {}
for tool_name, tool_info in AVAILABLE_TOOLS.items():
    category = tool_info["category"]
    if category not in TOOLS_BY_CATEGORY:
        TOOLS_BY_CATEGORY[category] = []
    TOOLS_BY_CATEGORY[category].append({
        "name": tool_name,
        "description": tool_info["description"]
    })