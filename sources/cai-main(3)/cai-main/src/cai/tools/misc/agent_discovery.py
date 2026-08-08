"""
Agent Discovery Tool for Selection Agent

This tool allows the selection agent to dynamically discover and analyze
all available agents in the CAI system to make informed recommendations.
"""

import importlib
import os
import pkgutil
from functools import lru_cache
from typing import Dict, List, Any
from cai.sdk.agents import Agent, function_tool


@lru_cache(maxsize=1)
def _check_available_agents() -> Dict[str, Any]:
    """
    Check all available agents in the CAI system and return their detailed information.

    Cached with ``lru_cache(maxsize=1)``: the agent catalogue is static for the
    lifetime of a session (no hot-reloading of agent modules), so paying the
    full ``pkgutil.iter_modules`` + ``importlib.import_module`` walk on every
    LLM tool call (orchestrator routing, ``_get_agent_number`` lookups, etc.)
    is wasteful. Callers must treat the returned dict as read-only.

    Returns:
        Dict containing comprehensive information about all available agents
    """
    agents_info = {}
    
    # Import the agents module
    import cai.agents
    
    # Scan the main agents directory
    for _, name, _ in pkgutil.iter_modules(cai.agents.__path__, cai.agents.__name__ + "."):
        try:
            module = importlib.import_module(name)
            
            # Look for Agent instances in the module
            for attr_name in dir(module):
                if attr_name.startswith("_"):
                    continue
                    
                attr = getattr(module, attr_name)
                if isinstance(attr, Agent):
                    agent_info = {
                        "name": attr.name,
                        "description": getattr(attr, "description", "No description available"),
                        "module": name,
                        "variable_name": attr_name,
                        "tools": [],
                        "capabilities": [],
                        "specialization": _extract_specialization(attr.name, getattr(attr, "description", "")),
                        "use_cases": _extract_use_cases(getattr(attr, "description", ""))
                    }
                    
                    # Extract tool information
                    if hasattr(attr, "tools") and attr.tools:
                        for tool in attr.tools:
                            if hasattr(tool, "name"):
                                agent_info["tools"].append({
                                    "name": tool.name,
                                    "description": getattr(tool, "description", "")
                                })
                    
                    agents_info[attr_name] = agent_info
                    
        except (ImportError, AttributeError) as e:
            continue
    
    # Also check patterns subdirectory
    patterns_path = os.path.join(os.path.dirname(cai.agents.__file__), "patterns")
    if os.path.exists(patterns_path):
        for _, name, _ in pkgutil.iter_modules([patterns_path], cai.agents.__name__ + ".patterns."):
            try:
                module = importlib.import_module(name)
                
                for attr_name in dir(module):
                    if attr_name.startswith("_"):
                        continue
                        
                    attr = getattr(module, attr_name)
                    if isinstance(attr, Agent):
                        agent_info = {
                            "name": attr.name,
                            "description": getattr(attr, "description", "No description available"),
                            "module": name,
                            "variable_name": attr_name,
                            "type": "pattern",
                            "tools": [],
                            "capabilities": [],
                            "specialization": _extract_specialization(attr.name, getattr(attr, "description", "")),
                            "use_cases": _extract_use_cases(getattr(attr, "description", ""))
                        }
                        
                        agents_info[attr_name] = agent_info
                        
            except (ImportError, AttributeError):
                continue
    
    # Create indexed list for easy reference
    agent_list = list(agents_info.keys())
    indexed_agents = {}
    for i, agent_key in enumerate(agent_list, 1):
        indexed_agents[i] = {
            "key": agent_key,
            "info": agents_info[agent_key]
        }
    
    return {
        "total_agents": len(agents_info),
        "agents": agents_info,
        "indexed_agents": indexed_agents,
        "agent_list": agent_list,
        "categories": _categorize_agents(agents_info)
    }


def _analyze_task_requirements(task_description: str) -> Dict[str, Any]:
    """
    Analyze a user's task description to extract key requirements and characteristics.
    
    Args:
        task_description: The user's description of what they want to accomplish
        
    Returns:
        Dict containing analysis of the task requirements
    """
    task_lower = task_description.lower()
    
    # Define task categories and keywords
    task_categories = {
        "penetration_testing": [
            "pentest", "penetration test", "security assessment", "vulnerability assessment",
            "exploit", "attack", "breach", "hack", "infiltration", "red team"
        ],
        "bug_bounty": [
            "bug bounty", "vulnerability discovery", "web security", "api testing",
            "responsible disclosure", "security bug", "vulnerability hunting"
        ],
        "blue_team": [
            "defense", "defensive", "blue team", "monitoring", "detection",
            "incident response", "security monitoring", "threat hunting", "soc"
        ],
        "forensics": [
            "forensics", "dfir", "incident response", "digital forensics",
            "investigation", "evidence", "malware analysis", "breach investigation"
        ],
        "reverse_engineering": [
            "reverse engineering", "binary analysis", "firmware analysis",
            "disassembly", "decompilation", "malware analysis", "code analysis"
        ],
        "network_security": [
            "network", "traffic analysis", "packet capture", "network monitoring",
            "protocol analysis", "wireshark", "tcpdump", "network forensics"
        ],
        "wireless_security": [
            "wifi", "wireless", "bluetooth", "radio", "rf", "802.11",
            "wireless security", "wifi hacking", "wireless penetration"
        ],
        "memory_analysis": [
            "memory analysis", "memory forensics", "process analysis",
            "runtime analysis", "memory dump", "heap analysis"
        ],
        "ctf": [
            "ctf", "capture the flag", "challenge", "flag", "competition",
            "security challenge", "hacking challenge"
        ],
        "reporting": [
            "report", "documentation", "summary", "findings", "analysis report",
            "security report", "executive summary"
        ]
    }
    
    # Analyze task characteristics
    detected_categories = []
    confidence_scores = {}
    
    for category, keywords in task_categories.items():
        matches = sum(1 for keyword in keywords if keyword in task_lower)
        if matches > 0:
            detected_categories.append(category)
            confidence_scores[category] = matches / len(keywords)
    
    # Determine complexity and scope
    complexity_indicators = {
        "simple": ["simple", "basic", "quick", "fast", "easy"],
        "medium": ["comprehensive", "detailed", "thorough", "complete"],
        "complex": ["advanced", "deep", "extensive", "sophisticated", "complex"]
    }
    
    complexity = "medium"  # default
    for level, indicators in complexity_indicators.items():
        if any(indicator in task_lower for indicator in indicators):
            complexity = level
            break
    
    # Determine if multiple agents might be needed
    multi_agent_indicators = [
        "comprehensive", "full", "complete", "end-to-end", "multiple",
        "both", "all", "various", "different perspectives"
    ]
    
    needs_multiple_agents = any(indicator in task_lower for indicator in multi_agent_indicators)
    
    return {
        "task_description": task_description,
        "detected_categories": detected_categories,
        "confidence_scores": confidence_scores,
        "complexity": complexity,
        "needs_multiple_agents": needs_multiple_agents,
        "primary_category": max(confidence_scores.items(), key=lambda x: x[1])[0] if confidence_scores else "general",
        "recommendations": _generate_initial_recommendations(detected_categories, complexity, needs_multiple_agents)
    }


def _extract_specialization(name: str, description: str) -> str:
    """Extract the main specialization from agent name and description"""
    specializations = {
        "red team": ["red team", "penetration", "exploit", "attack"],
        "blue team": ["blue team", "defense", "monitoring", "protection"],
        "bug bounty": ["bug bounty", "vulnerability discovery", "web security"],
        "forensics": ["forensics", "dfir", "investigation", "incident response"],
        "reverse engineering": ["reverse engineering", "binary analysis", "firmware"],
        "network security": ["network", "traffic", "protocol", "packet"],
        "wireless": ["wifi", "wireless", "radio", "rf"],
        "memory analysis": ["memory", "process", "runtime"],
        "reporting": ["report", "documentation", "summary"],
        "ctf": ["ctf", "challenge", "flag"],
        "general": ["general", "basic", "tool", "command"]
    }
    
    # Handle None values safely
    safe_name = name or ""
    safe_description = description or ""
    text = (safe_name + " " + safe_description).lower()
    
    for spec, keywords in specializations.items():
        if any(keyword in text for keyword in keywords):
            return spec
    
    return "general"


def _extract_use_cases(description: str) -> List[str]:
    """Extract potential use cases from agent description"""
    use_cases = []
    
    use_case_patterns = {
        "Penetration Testing": ["penetration", "pentest", "security assessment"],
        "Vulnerability Assessment": ["vulnerability", "security testing", "weakness"],
        "Network Analysis": ["network", "traffic", "protocol"],
        "Web Security": ["web", "api", "application"],
        "System Analysis": ["system", "host", "server"],
        "Malware Analysis": ["malware", "binary", "reverse"],
        "Incident Response": ["incident", "response", "investigation"],
        "Compliance": ["compliance", "audit", "standard"],
        "CTF Challenges": ["ctf", "challenge", "flag"],
        "Reporting": ["report", "documentation", "findings"]
    }
    
    # Handle None values safely
    safe_description = description or ""
    desc_lower = safe_description.lower()
    
    for use_case, keywords in use_case_patterns.items():
        if any(keyword in desc_lower for keyword in keywords):
            use_cases.append(use_case)
    
    return use_cases


def _categorize_agents(agents_info: Dict[str, Any]) -> Dict[str, List[str]]:
    """Categorize agents by their specialization"""
    categories = {}
    
    for agent_name, info in agents_info.items():
        spec = info.get("specialization", "general")
        if spec not in categories:
            categories[spec] = []
        categories[spec].append(agent_name)
    
    return categories


def _generate_initial_recommendations(categories: List[str], complexity: str, needs_multiple: bool) -> List[str]:
    """Generate initial recommendations based on task analysis"""
    recommendations = []
    
    if "penetration_testing" in categories:
        recommendations.append("Consider redteam_agent for comprehensive penetration testing")
    
    if "bug_bounty" in categories:
        recommendations.append("Consider bug_bounter_agent for vulnerability discovery")
    
    if "blue_team" in categories:
        recommendations.append("Consider blueteam_agent for defensive security analysis")
    
    if "forensics" in categories:
        recommendations.append("Consider dfir_agent for digital forensics and incident response")
    
    if "network_security" in categories:
        recommendations.append("Consider network_security_analyzer_agent for network analysis")
    
    if "wireless_security" in categories:
        recommendations.append("Consider wifi_security_agent for wireless security testing")
    
    if "reverse_engineering" in categories:
        recommendations.append("Consider reverse_engineering_agent for binary analysis")
    
    if "memory_analysis" in categories:
        recommendations.append("Consider memory_analysis_agent for runtime analysis")
    
    if "reporting" in categories:
        recommendations.append("Consider reporting_agent for generating reports")
    
    if needs_multiple:
        recommendations.append("Consider using multiple agents or a pattern for comprehensive coverage")
    
    if complexity == "complex":
        recommendations.append("Complex tasks may benefit from hierarchical or swarm patterns")
    
    return recommendations


def _get_agent_number(agent_name: str) -> Dict[str, Any]:
    """
    Get the numerical index of a specific agent for easy command reference.
    
    Args:
        agent_name: The name/key of the agent to find
        
    Returns:
        Dict containing agent number, command, and details
    """
    # Get all agents
    agents_data = _check_available_agents()
    indexed_agents = agents_data.get("indexed_agents", {})
    
    # Find the agent
    for number, agent_data in indexed_agents.items():
        if agent_data["key"].lower() == agent_name.lower():
            agent_info = agent_data["info"]
            return {
                "agent_number": number,
                "agent_key": agent_data["key"],
                "agent_name": agent_info.get("name", agent_data["key"]),
                "command": f"/agent {number}",
                "alt_command": f"/agent {agent_data['key']}",
                "found": True,
                "description": agent_info.get("description", "No description available")
            }
    
    return {
        "found": False,
        "message": f"Agent '{agent_name}' not found",
        "total_agents": agents_data.get("total_agents", 0)
    }


# Create the function tools for the agent.
# `name_override` is used so the names exposed to the LLM (and matched by
# ``_COMPACT_HIDDEN_TOOL_NAMES``) don't carry the leading underscore from
# the private helper functions above.
check_available_agents = function_tool(
    _check_available_agents,
    name_override="check_available_agents",
)
analyze_task_requirements = function_tool(
    _analyze_task_requirements,
    name_override="analyze_task_requirements",
)
get_agent_number = function_tool(
    _get_agent_number,
    name_override="get_agent_number",
)
