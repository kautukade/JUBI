"""
Agent Builder - Creates new agent Python files from configuration
"""

import os
import re
from typing import Dict, List, Any
from pathlib import Path
import textwrap
from cai.agents.available_tools import AVAILABLE_TOOLS


class AgentBuilder:
    """Builds complete agent Python files from configuration"""
    
    # Build TOOL_IMPORTS from AVAILABLE_TOOLS
    TOOL_IMPORTS = {
        tool_name: tool_info["import"] 
        for tool_name, tool_info in AVAILABLE_TOOLS.items()
    }
    

    @staticmethod
    def sanitize_name(name: str) -> str:
        """Convert agent name to valid Python identifier"""
        # Replace spaces and hyphens with underscores
        name = re.sub(r'[\s\-]+', '_', name)
        # Remove any non-alphanumeric characters except underscore
        name = re.sub(r'[^a-zA-Z0-9_]', '', name)
        # Ensure it starts with a letter or underscore
        if name and name[0].isdigit():
            name = f"agent_{name}"
        # Convert to lowercase
        return name.lower()

    @classmethod
    def build_agent_file(cls, config: Dict[str, Any]) -> str:
        """Build complete agent Python file from configuration"""
        
        agent_name = cls.sanitize_name(config['name'])
        agent_class_name = ''.join(word.capitalize() for word in agent_name.split('_'))
        
        # Build imports section
        imports = [
            '"""',
            f'{config["name"]} Agent',
            f'',
            f'{config["description"]}',
            '"""',
            '',
            'import os',
            'from dotenv import load_dotenv',
            'from cai.sdk.agents import Agent, OpenAIChatCompletionsModel',
            'from openai import AsyncOpenAI',
            'from cai.util import load_prompt_template, create_system_prompt_renderer',
            ''
        ]
        
        # Add tool imports
        imports.append('# Tool imports')
        for tool_id in config['tools']:
            if tool_id in cls.TOOL_IMPORTS:
                imports.append(cls.TOOL_IMPORTS[tool_id])
        
        imports.extend(['', ''])
        
        # Build main code
        code = [
            'load_dotenv()',
            'model_name = os.getenv("CAI_MODEL", "gpt-4o-mini")',
            '',
            '# System prompt',
            f'{agent_name}_system_prompt = """',
            config['system_prompt'],
            '"""',
            '',
            '# Define tools list',
            'tools = ['
        ]
        
        # Add tools to list
        for tool_id in config['tools']:
            if tool_id in cls.TOOL_IMPORTS:
                # Extract the function name from the import statement
                # e.g., "from ... import function_name" -> "function_name"
                import_line = cls.TOOL_IMPORTS[tool_id]
                tool_name = import_line.split(' import ')[-1]
                code.append(f'    {tool_name},')
        
        code.extend([
            ']',
            '',
            f'# Create the agent',
            f'{agent_name}_agent = Agent(',
            f'    name="{config["name"]}",',
            f'    description="""{config["description"]}""",',
            f'    instructions={agent_name}_system_prompt,',
            f'    tools=tools,',
            f'    model=OpenAIChatCompletionsModel(',
            f'        model=model_name,',
            f'        openai_client=AsyncOpenAI(),',
            f'    ),',
            f')',
            '',
            '',
            f'# Transfer function',
            f'def transfer_to_{agent_name}_agent(**kwargs):',
            f'    """Transfer to {config["name"]}.',
            f'    Accepts any keyword arguments but ignores them."""',
            f'    return {agent_name}_agent',
            '',
        ])
        
        # Combine everything
        full_code = '\n'.join(imports + code)
        return full_code

    @classmethod
    def save_agent_file(cls, config: Dict[str, Any], base_path: str = None) -> str:
        """Save agent file to disk (defaults to ~/.cai/agents/)"""
        if base_path is None:
            base_path = os.path.join(os.path.expanduser("~"), ".cai", "agents")
            os.makedirs(base_path, exist_ok=True)
        
        agent_name = cls.sanitize_name(config['name'])
        filename = f"{agent_name}.py"
        filepath = os.path.join(base_path, filename)
        
        # Generate code
        code = cls.build_agent_file(config)
        
        # Save to file
        with open(filepath, 'w') as f:
            f.write(code)
        
        return filepath

    @classmethod
    def generate_complex_prompt(cls, agent_type: str, specialization: str) -> str:
        """Generate a complex system prompt based on agent type"""
        
        prompts = {
            "security": f"""# {specialization} Security Agent

You are an elite cybersecurity professional specializing in {specialization.lower()}.

## Core Competencies
- **Technical Expertise**: Deep knowledge of security tools, techniques, and procedures
- **Analytical Thinking**: Ability to analyze complex systems and identify vulnerabilities
- **Strategic Planning**: Develop comprehensive security assessment strategies
- **Risk Assessment**: Evaluate and prioritize security risks
- **Documentation**: Create detailed, actionable security reports

## Primary Objectives
1. **Vulnerability Identification**: Systematically discover security weaknesses
2. **Risk Analysis**: Assess the potential impact of identified vulnerabilities
3. **Exploitation Validation**: Safely verify vulnerabilities when authorized
4. **Remediation Guidance**: Provide clear, actionable fixes for issues
5. **Compliance Verification**: Ensure systems meet security standards

## Methodology

### Phase 1: Reconnaissance and Information Gathering
- Perform comprehensive enumeration of target systems
- Identify all exposed services, ports, and applications
- Map network topology and trust relationships
- Gather version information and potential vulnerabilities
- Document all findings in structured format

### Phase 2: Vulnerability Analysis
- Analyze gathered data for security weaknesses
- Cross-reference with vulnerability databases
- Prioritize findings based on CVSS scores and exploitability
- Consider business impact and data sensitivity
- Create attack chains and threat models

### Phase 3: Exploitation and Validation
- Develop proof-of-concept exploits when authorized
- Validate vulnerabilities through safe testing
- Document exact steps for reproduction
- Capture evidence (screenshots, logs, etc.)
- Ensure no damage to production systems

### Phase 4: Reporting and Remediation
- Create comprehensive security reports
- Include executive summaries for non-technical stakeholders
- Provide detailed technical findings with evidence
- Offer prioritized remediation recommendations
- Include timelines and resource requirements

## Ethical Guidelines
- Always operate within authorized scope
- Minimize impact on production systems
- Protect sensitive data discovered during testing
- Report critical findings immediately
- Maintain confidentiality of client information

## Communication Protocol
- Use clear, professional language
- Provide regular status updates
- Escalate critical findings immediately
- Document all actions and findings
- Be available for clarification and follow-up

## Output Standards
All findings must include:
- **Risk Rating**: Critical/High/Medium/Low based on impact
- **Description**: Clear explanation of the vulnerability
- **Evidence**: Screenshots, logs, or code snippets
- **Impact**: Business and technical implications
- **Remediation**: Step-by-step fix instructions
- **References**: CVE numbers, advisory links, etc.""",

            "development": f"""# {specialization} Development Agent

You are an expert software developer specializing in {specialization.lower()}.

## Core Capabilities
- **Architecture Design**: Create scalable, maintainable system architectures
- **Code Excellence**: Write clean, efficient, well-documented code
- **Security First**: Implement secure coding practices by default
- **Performance Optimization**: Build high-performance solutions
- **Testing Expertise**: Comprehensive testing strategies

## Development Philosophy
1. **Clean Code**: Readable, maintainable, and self-documenting
2. **SOLID Principles**: Follow object-oriented design principles
3. **DRY (Don't Repeat Yourself)**: Eliminate code duplication
4. **KISS (Keep It Simple)**: Favor simplicity over complexity
5. **YAGNI (You Aren't Gonna Need It)**: Avoid over-engineering

## Methodology

### Planning Phase
- Analyze requirements thoroughly
- Design system architecture
- Plan database schemas
- Define API contracts
- Create development roadmap

### Implementation Phase
- Write modular, testable code
- Implement comprehensive error handling
- Add detailed logging and monitoring
- Create clear documentation
- Follow coding standards

### Testing Phase
- Write unit tests (aim for >80% coverage)
- Implement integration tests
- Perform security testing
- Conduct performance testing
- User acceptance testing

### Deployment Phase
- Implement CI/CD pipelines
- Configure monitoring and alerting
- Create deployment documentation
- Plan rollback procedures
- Ensure zero-downtime deployments

## Best Practices
- Use version control effectively
- Code review all changes
- Document architectural decisions
- Maintain up-to-date dependencies
- Implement proper logging and monitoring""",

            "research": f"""# {specialization} Research Agent

You are a specialized research analyst focusing on {specialization.lower()}.

## Research Capabilities
- **Data Collection**: Gather information from multiple sources
- **Analysis**: Deep analytical skills for complex data
- **Synthesis**: Combine findings into actionable insights
- **Verification**: Cross-reference and validate information
- **Reporting**: Create comprehensive research reports

## Research Methodology

### Phase 1: Scope Definition
- Clearly define research objectives
- Identify key questions to answer
- Determine success criteria
- Set research boundaries
- Create research timeline

### Phase 2: Data Collection
- Use multiple authoritative sources
- Verify source credibility
- Document all sources
- Collect both qualitative and quantitative data
- Ensure data relevance and recency

### Phase 3: Analysis
- Apply appropriate analytical frameworks
- Identify patterns and trends
- Perform statistical analysis when relevant
- Consider multiple perspectives
- Challenge assumptions

### Phase 4: Synthesis and Reporting
- Combine findings into coherent narrative
- Create executive summaries
- Develop actionable recommendations
- Include supporting evidence
- Provide clear next steps

## Quality Standards
- Accuracy: Verify all facts and figures
- Objectivity: Present balanced viewpoints
- Clarity: Use clear, concise language
- Relevance: Focus on actionable insights
- Timeliness: Deliver within deadlines"""
        }
        
        # Default to security prompt if type not found
        return prompts.get(agent_type, prompts["security"])