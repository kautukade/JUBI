"""
CTR Results Presentation Engine - Rich Console Visualization for Security Game Analysis

This module provides comprehensive visualization and presentation capabilities for CTR
(Cut The Rope) security game analysis results. It transforms raw Nash equilibrium 
solutions into human-readable, formatted console output using rich text formatting,
tables, and panels for professional security analysis reporting.

CORE FUNCTIONALITY:
==================
1. Strategy Visualization: Present optimal mixed strategies for both players
   - Defender Strategy: Probability distribution over check locations
   - Attacker Strategy: Probability distribution over attack paths  
   - Sorted display (highest to lowest probability) for strategic insights
   - Color-coded formatting for visual clarity and emphasis

2. Attack Path Translation: Convert internal CTR representations to readable format
   - Translate artificial nodes (leaf_X) back to original vulnerable node names
   - Convert merged target names from c(leaf_6,leaf_8) to c(6,8) format
   - Display full attack sequences with readable node transitions
   - Handle complex multi-path scenarios with clear path identification

3. Equilibrium Analysis Display: Present game-theoretic solution insights
   - Nash equilibrium success probabilities for both players
   - Game value interpretation (should be equal for both players in equilibrium)
   - Strategic implications and security recommendations
   - Professional formatting for security assessment reports

4. Multi-Format Output Support: Flexible output destinations
   - Rich console output for interactive analysis sessions
   - File export for report generation and documentation
   - Batch processing support for multi-log comparative analysis
   - Integration with experimental framework outputs

VISUALIZATION FEATURES:
======================
- Rich Tables: Professional grid-based strategy presentations
- Color Coding: Strategic highlighting and emphasis
- Sortable Display: Priority-based ordering (highest probability first)
- Path Sequences: Full attack path visualization with node transitions
- Panels: Organized sections for different analysis components
- Export Compatibility: Text-based output suitable for reports

CTR INTEGRATION INTERFACES:
===========================
- Baseline Results Processing: Direct integration with CTR core solver output
- Attack Path Mapping: Compatible with CTR attack path enumeration (as2)
- Node Name Translation: Handles CTR preprocessing artifacts (artificial nodes)
- Multi-Log Support: Processes results from comparative multi-log analysis

OUTPUT COMPONENTS:
==================
1. Optimal Defense Strategy Table:
   - Node IDs with probability assignments
   - Sorted by strategic priority (highest probability first)
   - Clear probability formatting (6 decimal precision)

2. Attacker Strategy Table:
   - Attack path IDs with full path sequences
   - Probability assignments for each path
   - Readable node name translations
   - Strategic path ranking

3. Game Equilibrium Panel:
   - Defender success probability (security effectiveness)
   - Attacker success probability (threat capability)
   - Strategic interpretation and implications

USAGE IN CTR PIPELINE:
======================
This module serves as the final presentation layer in the CTR analysis pipeline:
Log Analysis → Graph Extraction → Probability Computation → Game Solving → Visualization

The output provides actionable security insights:
- Which nodes should be prioritized for defense (high probability in defense strategy)
- Which attack paths pose the greatest threat (high probability in attack strategy)
- Overall security posture assessment (equilibrium success probabilities)
- Strategic recommendations for security improvements
"""

import re
from pathlib import Path

import numpy as np
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from cai.repl.ui.banner import _CAI_GREEN

def convert_target_name_to_original(target_name):
    """
    Convert CTR target names from c(leaf_6,leaf_8) format to original vulnerable node names.
    
    Args:
        target_name: Target name in CTR format (e.g., "c(leaf_6,leaf_8)")
        
    Returns:
        Converted name showing original vulnerable nodes (e.g., "vulnerable nodes: 6, 8")
    """
    # Check if it's a merged target format: c(leaf_X,leaf_Y,...)
    match = re.match(r'c\((.*)\)', str(target_name))
    if match:
        leaf_nodes = match.group(1).split(',')
        original_ids = []
        for leaf in leaf_nodes:
            leaf = leaf.strip()
            if leaf.startswith('leaf_'):
                original_id = leaf.replace('leaf_', '')
                original_ids.append(original_id)
            else:
                original_ids.append(leaf)
        
        if len(original_ids) == 1:
            return f"c({original_ids[0]})"
        else:
            return f"c({', '.join(original_ids)})"
    
    # If it doesn't match the pattern, return as-is
    return str(target_name)

def convert_path_sequence_to_original(path_sequence):
    """
    Convert path sequences containing CTR target names to show original vulnerable nodes.
    
    Args:
        path_sequence: Path string (e.g., "1 → 4 → 5 → 6 → c(leaf_6,leaf_8)" or "1 → 2 → leaf_7")
        
    Returns:
        Converted path string with readable target names
    """
    if not isinstance(path_sequence, str):
        return str(path_sequence)
    
    # Replace any c(leaf_X,leaf_Y) patterns in the path
    def replace_target(match):
        return convert_target_name_to_original(match.group(0))
    
    # Pattern to match c(...) with leaf_ nodes
    pattern_c = r'c\([^)]*leaf_[^)]*\)'
    converted_path = re.sub(pattern_c, replace_target, path_sequence)
    
    # Also replace standalone leaf_X patterns
    def replace_standalone_leaf(match):
        leaf_node = match.group(0)
        original_id = leaf_node.replace('leaf_', '')
        return f"vulnerable node: {original_id}"
    
    pattern_leaf = r'\bleaf_\d+\b'
    converted_path = re.sub(pattern_leaf, replace_standalone_leaf, converted_path)
    
    return converted_path

def visualize_baseline_results(baseline_results: dict, output_path: str = None, paths: list = None, print_to_console: bool = True) -> None:
    """
    Security Game Results Visualization: Transform Nash equilibrium into professional presentation.
    
    Creates comprehensive, formatted visualization of CTR security game analysis results
    using rich console formatting. Transforms raw mathematical Nash equilibrium solutions
    into actionable security insights with professional presentation suitable for
    security assessment reports and strategic decision-making.
    
    VISUALIZATION COMPONENTS:
    1. Optimal Defense Strategy Table:
       - Lists all defender check locations with probability assignments
       - Sorted by strategic priority (highest probability = highest priority)
       - Provides clear guidance on resource allocation for security monitoring
       
    2. Attacker Strategy Analysis Table:
       - Shows all attack paths with probability assignments and full sequences
       - Translates internal node representations to readable path descriptions
       - Identifies highest-risk attack vectors requiring prioritized mitigation
       
    3. Game Equilibrium Summary Panel:
       - Presents defender success probability (security effectiveness metric)
       - Shows attacker success probability (threat capability assessment)
       - Provides overall security posture evaluation
    
    STRATEGIC INTERPRETATION:
    - High defender probabilities: Critical nodes requiring focused security investment
    - High attacker probabilities: Priority threat vectors for mitigation planning
    - Equilibrium values: Overall security effectiveness and threat landscape assessment
    - Path sequences: Detailed attack progression for incident response planning
    
    OUTPUT FORMATTING:
    - Professional rich table formatting with clear headers and alignment
    - Color-coded elements for visual clarity and emphasis
    - Sortable displays prioritizing strategic importance
    - Export-friendly text format for report integration
    
    Args:
        baseline_results (dict): Nash equilibrium solution from CTR core analysis containing:
            - 'optimal_defense': {node_id: probability} defender mixed strategy
            - 'attacker_strategy': [probability] list for attacker mixed strategy
            - 'defender_success': Equilibrium defender success probability
            - 'attacker_success': Equilibrium attacker success probability
        output_path (str, optional): File path for text export of formatted results
                                   If provided, appends formatted output to specified file
        paths (list, optional): Attack path sequences (as2 from CTR core) for path visualization
                               List of node sequences showing full attack progressions
        print_to_console (bool): Console output control flag
                                True: Display formatted results in console
                                False: Only export to file (for batch processing)
                                
    Returns:
        None: Function performs side effects (console output and/or file export)
        
    Example Usage:
        >>> results = {'optimal_defense': {'node_5': 0.8, 'node_3': 0.2}, 
        ...           'attacker_strategy': [0.6, 0.4],
        ...           'defender_success': 0.75, 'attacker_success': 0.75}
        >>> paths = [['1', '2', '5'], ['1', '3', '4']]
        >>> visualize_baseline_results(results, paths=paths)
        # Outputs formatted tables showing defense priorities and attack threats
        
    Note:
        This function serves as the primary interface for presenting CTR analysis results
        to security professionals, providing actionable insights for strategic security
        decision-making and resource allocation planning.
    """
    console = Console()

    # Create defense table
    defense_table = Table(
        title="Optimal Defense Strategy",
        show_header=True,
        header_style=f"bold {_CAI_GREEN}",
    )
    defense_table.add_column("Node ID", justify="center", style="white")
    defense_table.add_column("Probability", justify="right", style="dim")
    
    # Sort defense probabilities from high to low, then by node ID for equal probabilities
    defense_items = sorted(
        baseline_results['optimal_defense'].items(),
        key=lambda x: (-float(x[1]), str(x[0])) 
    )
    
    for node_id, prob in defense_items:
        defense_table.add_row(str(node_id), f"{float(prob):.6f}")
    
    # Create attack table
    attack_table = Table(
        title="Attacker Strategy",
        show_header=True,
        header_style=f"bold {_CAI_GREEN}",
    )
    attack_table.add_column("Path ID", justify="center", style="white")
    if paths:
        attack_table.add_column("Path Sequence", justify="left", style="dim")
    attack_table.add_column("Probability", justify="right", style="dim")
    
    # Convert attacker strategy to list if it's a string representation
    attack_strategy_value = baseline_results.get('attacker_strategy', [])
    if isinstance(attack_strategy_value, str):
        import ast
        try:
            attack_probs = ast.literal_eval(attack_strategy_value)
        except (SyntaxError, ValueError):
            # Handle numpy-style arrays or other non-Python formats, e.g. "[0.5 0.5]"
            import re
            attack_probs = [float(num) for num in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", attack_strategy_value)]
    else:
        attack_probs = attack_strategy_value

    # Normalize attacker probabilities into a simple iterable of floats
    if isinstance(attack_probs, (int, float)):
        attack_probs = [float(attack_probs)]
    elif hasattr(attack_probs, 'tolist'):
        attack_probs = attack_probs.tolist()

    # Ensure we have a basic list for downstream processing
    attack_probs = list(attack_probs or [])

    # Create list of tuples with (path_id, probability) for sorting
    attack_items = list(enumerate(attack_probs, 1))
    
    # Sort by probability (descending) and then by path ID (ascending) for equal probabilities
    attack_items.sort(key=lambda x: (-float(x[1]), x[0]))
    
    # Add attack probabilities in sorted order
    for original_path_id, prob in attack_items:
        if paths and len(paths) >= original_path_id:
            # Get the actual path sequence using the original path ID (1-indexed, so subtract 1 for 0-based paths list)
            path_sequence = " → ".join(str(node) for node in paths[original_path_id - 1])
            # Convert path sequence to show original vulnerable nodes
            path_sequence = convert_path_sequence_to_original(path_sequence)
            attack_table.add_row(str(original_path_id), path_sequence, f"{float(prob):.6f}")
        else:
            if paths:
                attack_table.add_row(str(original_path_id), "Path not found", f"{float(prob):.6f}")
            else:
                attack_table.add_row(str(original_path_id), f"{float(prob):.6f}")
    
    # Create equilibrium panel
    equilibrium_text = (
        f"Defender can keep attacker success below: {float(baseline_results['defender_success']):.6f}\n"
        f"Attacker can guarantee success probability of: {float(baseline_results['attacker_success']):.6f}"
    )
    equilibrium_panel = Panel(
        equilibrium_text,
        title="Game Equilibrium",
        title_align="left",
        border_style=_CAI_GREEN,
    )
    
    if output_path:
        # Create a string representation of the output
        str_console = Console(record=True)
        str_console.print(defense_table)
        str_console.print("\n")
        str_console.print(attack_table)
        str_console.print("\n")
        str_console.print(equilibrium_panel)
        
        # Save to file
        with open(output_path, 'a') as f:
            f.write(str_console.export_text())
    
    # Only print to console if explicitly requested
    if print_to_console:
        console.print(defense_table)
        console.print("\n")
        console.print(attack_table)
        console.print("\n")
        console.print(equilibrium_panel)
