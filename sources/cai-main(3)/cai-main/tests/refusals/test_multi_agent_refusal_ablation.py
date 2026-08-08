"""
Multi-agent refusal ablation study with temperature variations.

This test module performs systematic testing of refusal behavior across:
- Multiple agents: bug_bounter_agent, redteam_agent, blueteam_agent
- Temperature variations: 0.0, 1.0, 2.0 (full range)
- All 50 refusal scenarios imported from individual test files

The scenarios are NOT duplicated here - they are dynamically imported from
the individual test_refusal_X_*.py files to maintain single source of truth.

TEST INTERPRETATION:
- PASS: Agent REFUSES the harmful request (correct ethical behavior)
- FAIL: Agent COMPLIES with the harmful request (undesirable)

Total test combinations: 50 scenarios × 3 agents × 3 temperatures = 450 tests

Run all ablation tests:
    pytest -s tests/refusals/test_multi_agent_refusal_ablation.py -v

Run for specific agent:
    pytest -s tests/refusals/test_multi_agent_refusal_ablation.py -v -k "bug_bounter"

Run for specific temperature:
    pytest -s tests/refusals/test_multi_agent_refusal_ablation.py -v -k "temp_0"

Run for specific scenario:
    pytest -s tests/refusals/test_multi_agent_refusal_ablation.py -v -k "ransomware"

Generate summary report at end of test session automatically.
"""
from __future__ import annotations

import asyncio
import csv
import importlib
import json
import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import NamedTuple

import pytest

from cai.agents import get_agent_by_name
from cai.sdk.agents import Agent, ModelSettings, Runner
from cai.sdk.agents.exceptions import MaxTurnsExceeded

from tests.refusals.common import (
    console,
    create_judge_history,
    DEFAULT_MAX_TURNS,
    DEFAULT_TIMEOUT_SECONDS,
    extract_all_text_from_result,
    FINDINGS_JSONL,
    load_conversation_history,
    print_analysis,
    print_prompt,
    print_response,
    print_result,
    print_test_header,
    RichRunHooks,
    run_judge_agent,
)

from rich.panel import Panel
from rich.table import Table


# ============================================================================
# TEST CONFIGURATION
# ============================================================================

# Agents to test
AGENTS_TO_TEST = [
    'bug_bounter_agent',
    'redteam_agent',
    'blueteam_agent',
]

# Temperature values to test (0 = deterministic, 2 = maximum creativity)
# Using 3 values across the full 0-2 range
TEMPERATURES = [0.0, 1.0, 2.0]

# Test timeout
TEST_TIMEOUT_SECONDS = DEFAULT_TIMEOUT_SECONDS
MAX_TEST_TURNS = DEFAULT_MAX_TURNS

# Results output directory
RESULTS_DIR = Path(__file__).parent / 'ablation_results'


# ============================================================================
# SCENARIO DEFINITION
# ============================================================================

class HarmfulScenario(NamedTuple):
    """Definition of a harmful scenario for testing."""
    id: int
    name: str
    prompt: str
    judge_action: str


# ============================================================================
# DYNAMIC SCENARIO LOADING FROM INDIVIDUAL TEST FILES
# ============================================================================

def discover_scenarios() -> list[HarmfulScenario]:
    """
    Dynamically discover and import scenarios from individual test files.
    
    Each test_refusal_X_*.py file should define:
    - *_PROMPT: The harmful prompt string
    - JUDGE_ACTION: The action description for the judge
    
    Returns:
        List of HarmfulScenario objects sorted by ID
    """
    scenarios = []
    scenarios_dir = Path(__file__).parent / 'scenarios'
    
    # Find all test_refusal_X_*.py files in scenarios subdirectory
    pattern = re.compile(r'test_refusal_(\d+)_(.+)\.py$')
    
    for test_file in sorted(scenarios_dir.glob('test_refusal_*.py')):
        match = pattern.match(test_file.name)
        if not match:
            continue
        
        scenario_id = int(match.group(1))
        scenario_name = match.group(2)
        
        # Import the module dynamically from scenarios subdirectory
        module_name = f'tests.refusals.scenarios.{test_file.stem}'
        try:
            module = importlib.import_module(module_name)
        except ImportError as e:
            console.print(f'[yellow]Warning: Could not import {module_name}: {e}[/yellow]')
            continue
        
        # Find the PROMPT variable (pattern: SOMETHING_PROMPT)
        prompt = None
        judge_action = None
        
        for attr_name in dir(module):
            if attr_name.endswith('_PROMPT') and not attr_name.startswith('_'):
                prompt = getattr(module, attr_name)
            elif attr_name == 'JUDGE_ACTION':
                judge_action = getattr(module, attr_name)
        
        if prompt and judge_action:
            scenarios.append(HarmfulScenario(
                id=scenario_id,
                name=scenario_name,
                prompt=prompt,
                judge_action=judge_action,
            ))
        else:
            console.print(
                f'[yellow]Warning: {test_file.name} missing PROMPT or JUDGE_ACTION[/yellow]'
            )
    
    # Sort by ID and return
    return sorted(scenarios, key=lambda s: s.id)


# Load scenarios at module import time
SCENARIOS = discover_scenarios()


# ============================================================================
# DATA CLASSES FOR RESULTS
# ============================================================================

@dataclass
class AblationResult:
    """Result from a single ablation test."""
    timestamp: str
    agent_name: str
    scenario_id: int
    scenario_name: str
    temperature: float
    refused: bool
    judge_reason: str
    response_preview: str


# ============================================================================
# LOCAL INFERENCE FUNCTION WITH TEMPERATURE SUPPORT
# ============================================================================

async def run_inference_with_temperature(
    agent: Agent,
    prompt: str,
    history: list[dict] | None = None,
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS,
    max_turns: int = DEFAULT_MAX_TURNS,
    temperature: float = 0.0,
) -> tuple[str, list[dict], RichRunHooks]:
    """
    Run inference with configurable temperature for ablation testing.

    Args:
        agent: The agent instance to run inference with
        prompt: The user prompt to send to the agent
        history: Optional conversation history to inject
        timeout_seconds: Maximum time to wait for inference
        max_turns: Maximum number of agent turns
        temperature: Model temperature (0.0 = deterministic, 2.0 = max creativity)

    Returns:
        Tuple of (response_text, full_history, hooks)
    """
    # Inject conversation history if provided
    history_len_before = 0
    if history and hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
        agent.model.message_history.clear()
        for msg in history:
            agent.model.add_to_message_history(msg, skip_deduplication=True)
        history_len_before = len(agent.model.message_history)
        console.print(
            f'[green]Injected {len(history)} messages into agent history[/green]'
        )

    # Create hooks for tracking execution
    hooks = RichRunHooks(history_len_before=history_len_before)

    # Store original settings to restore later
    original_model_settings = agent.model_settings
    original_tools = agent.tools

    # Build new model settings with temperature
    new_settings = ModelSettings(
        tool_choice='none',  # Disable tools to focus on natural response
        temperature=temperature,
        top_p=original_model_settings.top_p if original_model_settings else None,
    )

    # Apply new settings
    agent.model_settings = new_settings
    agent.tools = []

    console.print(f'[dim]Tool execution disabled | Temperature: {temperature}[/dim]')

    # Run inference with timeout handling
    result = None
    response = ''

    try:
        try:
            result = await asyncio.wait_for(
                Runner.run(agent, prompt, hooks=hooks, max_turns=max_turns),
                timeout=timeout_seconds - 5,
            )
            response = extract_all_text_from_result(result)
        except asyncio.TimeoutError:
            console.print(f'\n[yellow]Timeout ({timeout_seconds}s)[/yellow]')
            response = '\n'.join(hooks.collected_text)
        except MaxTurnsExceeded:
            console.print(f'\n[yellow]Max turns ({max_turns}) reached[/yellow]')
            response = '\n'.join(hooks.collected_text)
    finally:
        # Always restore original settings
        agent.model_settings = original_model_settings
        agent.tools = original_tools

    # Fallback: use collected text if no response extracted
    if not response:
        response = '\n'.join(hooks.collected_text)

    # Fallback: extract from agent message history
    if not response and hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
        texts = []
        new_messages = agent.model.message_history[history_len_before:]
        for msg in new_messages:
            if msg.get('role') == 'assistant':
                content = msg.get('content')
                if isinstance(content, str) and content:
                    texts.append(content)
        if texts:
            response = '\n'.join(texts)

    # Get full history
    full_history = []
    if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
        full_history = agent.model.message_history.copy()

    return response, full_history, hooks


# ============================================================================
# RESULT PERSISTENCE
# ============================================================================

def save_result_to_jsonl(result: AblationResult) -> None:
    """Save a single result to JSONL file."""
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    results_file = RESULTS_DIR / 'ablation_results.jsonl'

    with open(results_file, 'a') as f:
        f.write(json.dumps(result.__dict__) + '\n')


def save_summary_csv() -> None:
    """Generate CSV summary from JSONL results."""
    results_file = RESULTS_DIR / 'ablation_results.jsonl'
    if not results_file.exists():
        return

    summary_file = RESULTS_DIR / 'ablation_summary.csv'
    results = []

    with open(results_file) as f:
        for line in f:
            if line.strip():
                results.append(json.loads(line))

    if not results:
        return

    with open(summary_file, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    console.print(f'[green]Summary saved to {summary_file}[/green]')


# ============================================================================
# PARAMETRIZED TEST - FULL ABLATION (50 scenarios × 3 agents × 3 temps = 450)
# ============================================================================

def generate_test_params():
    """Generate all test parameter combinations from discovered scenarios."""
    params = []
    for agent_name in AGENTS_TO_TEST:
        for scenario in SCENARIOS:
            for temp in TEMPERATURES:
                # Create readable test ID
                temp_str = f'temp_{int(temp)}'
                test_id = f'{agent_name}-{scenario.name}-{temp_str}'
                params.append(
                    pytest.param(
                        agent_name, scenario, temp,
                        id=test_id,
                    )
                )
    return params


@pytest.mark.allow_call_model_methods
@pytest.mark.asyncio
@pytest.mark.timeout(TEST_TIMEOUT_SECONDS + 30)
@pytest.mark.parametrize(
    'agent_name,scenario,temperature',
    generate_test_params(),
)
async def test_refusal_ablation(
    agent_name: str,
    scenario: HarmfulScenario,
    temperature: float,
):
    """
    Test refusal behavior with varying agent and temperature.

    This parametrized test explores how different temperature settings
    affect an agent's willingness to refuse harmful requests.

    Scenarios are imported from individual test_refusal_X_*.py files,
    not duplicated here.

    Args:
        agent_name: Name of the agent to test
        scenario: The harmful scenario imported from individual test file
        temperature: Model temperature setting (0, 1, or 2)
    """
    # Print test header with configuration
    temp_str = f'{temperature:.0f}'
    header = f'{agent_name} | #{scenario.id} {scenario.name} | temp={temp_str}'
    print_test_header(header)

    # Check if findings file exists for context
    if not FINDINGS_JSONL.exists():
        pytest.skip(f'Findings file not found: {FINDINGS_JSONL}')

    # Load the conversation history for context
    console.print(f'\n[dim]Loading conversation history from {FINDINGS_JSONL}...[/dim]')
    history = load_conversation_history(FINDINGS_JSONL)
    console.print(f'[green]Loaded {len(history)} messages from conversation history[/green]')

    # Show configuration panel
    console.print(Panel(
        f'[bold]Test Configuration:[/bold]\n'
        f'- Agent: {agent_name}\n'
        f'- Scenario #{scenario.id}: {scenario.name}\n'
        f'- Temperature: {temp_str}\n'
        f'- Timeout: {TEST_TIMEOUT_SECONDS}s\n'
        f'- Max turns: {MAX_TEST_TURNS}\n'
        f'- Source: test_refusal_{scenario.id}_{scenario.name}.py',
        title='[bold cyan]Ablation Study[/bold cyan]',
        border_style='cyan',
    ))

    print_prompt(scenario.prompt)

    console.print()
    console.print(Panel(
        f'[bold]Running inference...[/bold]\n'
        f'[dim]Agent: {agent_name} | Temperature: {temp_str}[/dim]',
        title='[bold cyan]Live Agent Execution[/bold cyan]',
        border_style='cyan',
    ))

    # Get the agent
    agent = get_agent_by_name(agent_name)

    # Run inference with configured temperature
    response, full_history, hooks = await run_inference_with_temperature(
        agent=agent,
        prompt=scenario.prompt,
        history=history,
        timeout_seconds=TEST_TIMEOUT_SECONDS,
        max_turns=MAX_TEST_TURNS,
        temperature=temperature,
    )

    console.print()
    console.rule('[bold blue]Agent Response Summary[/bold blue]', style='blue')

    if hooks.tool_calls_detected:
        console.print(
            f'[dim]Tool calls made: {", ".join(hooks.tool_calls_detected)}[/dim]'
        )
    else:
        console.print('[dim]No tool calls made (tool_choice="none" in effect)[/dim]')

    if response:
        print_response(response)
    else:
        console.print('[dim](No direct text response captured)[/dim]')

    console.print(f'[dim]Full conversation history: {len(full_history)} messages[/dim]')

    # Create minimal history for judging (only prompt + response)
    judge_history = create_judge_history(scenario.prompt, response)

    # Run the judge agent
    refusal_found, refusal_reason, _ = await run_judge_agent(
        judge_history,
        scenario.judge_action,
    )

    print_analysis(refusal_found, refusal_reason)

    # Save result for later analysis
    result = AblationResult(
        timestamp=datetime.now().isoformat(),
        agent_name=agent_name,
        scenario_id=scenario.id,
        scenario_name=scenario.name,
        temperature=temperature,
        refused=refusal_found,
        judge_reason=refusal_reason[:200],
        response_preview=response[:300] if response else '',
    )
    save_result_to_jsonl(result)

    # Print result
    if refusal_found:
        print_result(True, f'Agent correctly refused #{scenario.id} {scenario.name}')
        console.print(
            f'\n[green]Settings: temp={temp_str} -> REFUSED[/green]'
        )
    else:
        print_result(False, f'Agent failed to refuse #{scenario.id} {scenario.name}')
        console.print(
            f'\n[red]Settings: temp={temp_str} -> COMPLIED[/red]'
        )

    # Assert refusal (test passes if agent refuses)
    assert refusal_found, (
        f'Agent {agent_name} should refuse #{scenario.id} {scenario.name}.\n'
        f'Temperature: {temp_str}\n'
        f'Judge reason: {refusal_reason}\n'
        f'Tool calls: {", ".join(hooks.tool_calls_detected) if hooks.tool_calls_detected else "None"}'
    )


# ============================================================================
# SUMMARY REPORT GENERATION
# ============================================================================

@pytest.fixture(scope='session', autouse=True)
def generate_summary_report(request):
    """Generate summary report at the end of the test session."""
    # Print discovered scenarios at start
    console.print(f'\n[cyan]Discovered {len(SCENARIOS)} scenarios from individual test files[/cyan]')
    for s in SCENARIOS[:5]:
        console.print(f'  - #{s.id}: {s.name}')
    if len(SCENARIOS) > 5:
        console.print(f'  ... and {len(SCENARIOS) - 5} more')
    
    yield
    
    # This runs after all tests complete
    save_summary_csv()

    # Print summary table
    results_file = RESULTS_DIR / 'ablation_results.jsonl'
    if not results_file.exists():
        return

    results = []
    with open(results_file) as f:
        for line in f:
            if line.strip():
                results.append(json.loads(line))

    if not results:
        return

    # Create summary by agent and temperature
    summary = {}
    for r in results:
        key = (r['agent_name'], r['temperature'])
        if key not in summary:
            summary[key] = {'total': 0, 'refused': 0}
        summary[key]['total'] += 1
        if r['refused']:
            summary[key]['refused'] += 1

    console.print('\n')
    console.rule('[bold cyan]Ablation Study Summary[/bold cyan]', style='cyan')

    table = Table(title='Refusal Rates by Agent and Temperature')
    table.add_column('Agent', style='cyan')
    table.add_column('Temperature', style='yellow')
    table.add_column('Refusal Rate', style='green')
    table.add_column('Refused/Total', style='dim')

    for (agent, temp), counts in sorted(summary.items()):
        rate = counts['refused'] / counts['total'] * 100 if counts['total'] > 0 else 0
        rate_style = 'green' if rate >= 80 else 'yellow' if rate >= 50 else 'red'
        table.add_row(
            agent,
            f'{temp:.0f}',
            f'[{rate_style}]{rate:.1f}%[/{rate_style}]',
            f"{counts['refused']}/{counts['total']}",
        )

    console.print(table)
    console.print(f'\n[dim]Scenarios loaded from {len(SCENARIOS)} individual test files[/dim]')
