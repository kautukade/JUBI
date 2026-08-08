"""
Bug Bounter Agent Refusal Test - 400 Scenarios.

This test runs 400 harmful scenarios against the bug_bounter_agent WITHOUT
temperature variations or agent changes. The goal is to measure the refusal
rate of the bug_bounter agent under default conditions.

Scenarios are loaded from individual test_refusal_X_*.py files (1-400).

Results are saved to:
- tests/refusals/ablation_results/bug_bounter_400_results.jsonl (detailed)
- tests/refusals/ablation_results/bug_bounter_400_summary.csv (summary)
"""
from __future__ import annotations

import asyncio
import csv
import importlib
import json
import re
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import pytest
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from cai.agents import get_agent_by_name
from cai.sdk.agents import ModelSettings, Runner
from cai.sdk.agents.exceptions import MaxTurnsExceeded

from tests.refusals.common import (
    console,
    create_judge_history,
    DEFAULT_MAX_TURNS,
    DEFAULT_TIMEOUT_SECONDS,
    extract_all_text_from_result,
    FINDINGS_JSONL,
    load_conversation_history,
    print_prompt,
    print_response,
    print_test_header,
    RichRunHooks,
    run_judge_agent,
)


# Test configuration
TEST_TIMEOUT_SECONDS = DEFAULT_TIMEOUT_SECONDS
MAX_TEST_TURNS = DEFAULT_MAX_TURNS
AGENT_NAME = 'bug_bounter_agent'
RESULTS_DIR = Path(__file__).parent / 'ablation_results'


@dataclass
class Scenario:
    """Represents a harmful scenario for testing."""
    id: int
    name: str
    prompt: str
    judge_action: str


def discover_all_scenarios() -> list[Scenario]:
    """
    Discover all 400 scenarios from individual test_refusal_X_*.py files.
    
    Each test file should define:
    - *_PROMPT: The harmful prompt string
    - JUDGE_ACTION: The action description for the judge
    
    Returns:
        List of Scenario objects sorted by ID (1-400)
    """
    scenarios = []
    scenarios_dir = Path(__file__).parent / 'scenarios'
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
            scenarios.append(Scenario(
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


# Load all 400 scenarios at module import time
ALL_SCENARIOS = discover_all_scenarios()
console.print(f'[green]Discovered {len(ALL_SCENARIOS)} scenarios for bug_bounter test[/green]')


def save_result(result: dict, results_file: Path):
    """Save a single result to JSONL file."""
    results_file.parent.mkdir(parents=True, exist_ok=True)
    with open(results_file, 'a') as f:
        f.write(json.dumps(result) + '\n')


def generate_summary(results_file: Path, summary_file: Path):
    """Generate summary CSV from results JSONL."""
    if not results_file.exists():
        return
    
    results = []
    with open(results_file) as f:
        for line in f:
            if line.strip():
                results.append(json.loads(line))
    
    if not results:
        return
    
    # Calculate overall stats
    total = len(results)
    refusals = sum(1 for r in results if r.get('refused'))
    compliances = sum(1 for r in results if r.get('refused') is False)
    errors = sum(1 for r in results if r.get('error'))
    refusal_rate = refusals / total * 100 if total > 0 else 0
    
    # Write summary CSV
    with open(summary_file, 'w') as f:
        f.write('metric,value\n')
        f.write(f'total_scenarios,{total}\n')
        f.write(f'refusals,{refusals}\n')
        f.write(f'compliances,{compliances}\n')
        f.write(f'errors,{errors}\n')
        f.write(f'refusal_rate,{refusal_rate:.1f}%\n')


def generate_test_params():
    """Generate test parameters from discovered scenarios."""
    return [
        pytest.param(
            scenario,
            id=f'{scenario.id:03d}_{scenario.name}',
        )
        for scenario in ALL_SCENARIOS
    ]


@pytest.mark.parametrize('scenario', generate_test_params())
@pytest.mark.allow_call_model_methods
@pytest.mark.asyncio
@pytest.mark.timeout(TEST_TIMEOUT_SECONDS + 30)
async def test_bug_bounter_refusal(scenario: Scenario):
    """
    Test bug_bounter_agent refusal for a single scenario.
    
    This test:
    1. Sends the harmful prompt to the bug_bounter_agent
    2. Uses an LLM judge to determine if the response was a refusal
    3. Records the result for analysis
    
    Args:
        scenario: The scenario to test (loaded from test_refusal_X_*.py file)
    """
    results_file = RESULTS_DIR / 'bug_bounter_400_results.jsonl'
    summary_file = RESULTS_DIR / 'bug_bounter_400_summary.csv'
    
    print_test_header(f'Bug Bounter | #{scenario.id} {scenario.name}')
    
    # Check if findings file exists for context
    if not FINDINGS_JSONL.exists():
        pytest.skip(f'Findings file not found: {FINDINGS_JSONL}')
    
    # Load conversation history
    history = load_conversation_history(FINDINGS_JSONL)
    console.print(f'[green]Loaded {len(history)} messages from conversation history[/green]')
    
    # Show configuration panel
    console.print(Panel(
        f'[bold]Test Configuration:[/bold]\n'
        f'- Agent: {AGENT_NAME}\n'
        f'- Scenario #{scenario.id}: {scenario.name}\n'
        f'- Timeout: {TEST_TIMEOUT_SECONDS}s\n'
        f'- Source: test_refusal_{scenario.id}_{scenario.name}.py',
        title='[bold cyan]Bug Bounter 400 Test[/bold cyan]',
        border_style='cyan',
    ))
    
    print_prompt(scenario.prompt)
    
    start_time = time.time()
    result = {
        'scenario_id': scenario.id,
        'scenario_name': scenario.name,
        'agent': AGENT_NAME,
        'timestamp': datetime.now().isoformat(),
        'refused': None,
        'error': None,
        'duration_seconds': 0,
        'response_preview': '',
        'judge_reason': '',
    }
    
    try:
        # Get the agent
        agent = get_agent_by_name(AGENT_NAME)
        
        # Store original settings
        original_settings = agent.model_settings
        original_tools = agent.tools
        
        # Configure for no tools (focus on natural response)
        agent.model_settings = ModelSettings(tool_choice='none')
        agent.tools = []
        
        # Create hooks for tracking
        history_len_before = 0
        if history and hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
            agent.model.message_history.clear()
            for msg in history:
                agent.model.add_to_message_history(msg, skip_deduplication=True)
            history_len_before = len(agent.model.message_history)
        
        hooks = RichRunHooks(history_len_before=history_len_before)
        
        console.print(f'\n[dim]Running inference with {AGENT_NAME}...[/dim]')
        
        # Run inference
        inference_result = None
        response = ''
        
        try:
            inference_result = await asyncio.wait_for(
                Runner.run(
                    agent,
                    scenario.prompt,
                    max_turns=MAX_TEST_TURNS,
                    hooks=hooks,
                ),
                timeout=TEST_TIMEOUT_SECONDS,
            )
        except asyncio.TimeoutError:
            console.print('[yellow]Inference timed out[/yellow]')
            result['error'] = 'Timeout'
        except MaxTurnsExceeded:
            console.print('[yellow]Max turns exceeded[/yellow]')
        except Exception as e:
            console.print(f'[red]Error during inference: {e}[/red]')
            result['error'] = str(e)
        finally:
            # Restore original settings
            agent.model_settings = original_settings
            agent.tools = original_tools
        
        # Extract response text
        if inference_result:
            response = extract_all_text_from_result(inference_result)
        
        if response:
            print_response(response)
            result['response_preview'] = response[:500]
        else:
            console.print('[dim](No direct text response captured)[/dim]')
        
        # Create minimal history for judging
        judge_history = create_judge_history(scenario.prompt, response)
        
        # Run the judge agent
        refusal_found, refusal_reason, _ = await run_judge_agent(
            conversation_history=judge_history,
            requested_action=scenario.judge_action,
        )
        
        result['refused'] = refusal_found
        result['judge_reason'] = refusal_reason
        
        # Show verdict
        status = '[green]REFUSED[/green]' if refusal_found else '[red]COMPLIED[/red]'
        console.print(f'\n[bold]Judge verdict: {status}[/bold]')
        console.print(f'[dim]Reason: {refusal_reason[:200]}...[/dim]')
        
        if hooks.tool_calls_detected:
            console.print(f'[dim]Tool calls: {", ".join(hooks.tool_calls_detected)}[/dim]')
    
    except Exception as e:
        result['error'] = str(e)
        console.print(f'[red]Error: {e}[/red]')
    
    finally:
        result['duration_seconds'] = time.time() - start_time
        save_result(result, results_file)
        generate_summary(results_file, summary_file)
    
    # The test passes regardless - we're collecting data
    if result['error']:
        pytest.skip(f"Error during test: {result['error']}")


def print_final_summary():
    """Print final summary table."""
    results_file = RESULTS_DIR / 'bug_bounter_400_results.jsonl'
    
    if not results_file.exists():
        return
    
    results = []
    with open(results_file) as f:
        for line in f:
            if line.strip():
                results.append(json.loads(line))
    
    if not results:
        return
    
    # Calculate stats
    total = len(results)
    refusals = sum(1 for r in results if r.get('refused'))
    compliances = sum(1 for r in results if r.get('refused') is False)
    errors = sum(1 for r in results if r.get('error'))
    refusal_rate = refusals / total * 100 if total > 0 else 0
    
    # Create summary table
    table = Table(title=f'Bug Bounter 400 Scenarios - Final Summary')
    table.add_column('Metric', style='cyan')
    table.add_column('Value', justify='right')
    
    table.add_row('Total Scenarios', str(total))
    table.add_row('Refusals', f'[green]{refusals}[/green]')
    table.add_row('Compliances', f'[red]{compliances}[/red]')
    table.add_row('Errors', f'[yellow]{errors}[/yellow]')
    table.add_row('Refusal Rate', f'[bold]{refusal_rate:.1f}%[/bold]')
    
    console.print(table)


@pytest.fixture(scope='session', autouse=True)
def final_summary_hook(request):
    """Print final summary after all tests complete."""
    yield
    print_final_summary()
