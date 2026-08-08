"""
This script creates a hardcoded graph structure with 3 vulnerable nodes and computes edge probabilities
using simulated data. It then generates and saves a visual attack graph for demonstration purposes.
It also runs the CTR baseline analysis to get the security game equilibrium results.

Usage:
    python tools/cut_the_rope/benchmark/graph_sample.py

The script performs the following steps:
    1. Creates a hardcoded graph structure with 3 vulnerable nodes.
    2. Simulates log data with token and cost statistics.
    3. Computes edge probabilities using a heuristic based on simulated tokens, cost, and message count.
    4. Extracts node vulnerability and name information.
    5. Creates an attack graph using the computed probabilities.
    6. Runs CTR baseline analysis to compute security game equilibrium.
    7. Plots and saves the attack graph visualization and CTR results.

Example:
   python3 tools/cut_the_rope/benchmark/graph_sample.py

"""

import json
import os
import argparse
import numpy as np
import sys
import io
from typing import List, Dict, Any
from pydantic import BaseModel

from tools.cut_the_rope.ctr_cai.experiment_cai import NodeInfo, EdgeInfo, GraphStructure
from tools.cut_the_rope.ctr_cai.probability_computation import compute_edge_probabilities_offline
from tools.cut_the_rope.ctr_cai.attack_graph_cai import create_graph_from_agent_output, plot_attack_graph

# Import CTR core functions for baseline analysis
sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from core.ctr_core import main as ctr_core_main
from core.ctr_core import find_and_add_entry_node, generate_game_elements
from tools.cut_the_rope.ctr_cai.ctr_baseline_visualization import visualize_baseline_results

def random_steps(route, attack_rate=None, defense_rate=None, graph=None):
    """
    Geometric distribution for randomly moving defender.
    
    Args:
        route: Attack path
        attack_rate: Attacker movement rate parameter
        defense_rate: Defender check rate parameter
        graph: Attack graph (not used in this implementation)
        
    Returns:
        Array of probabilities for each position in the path
    """
    # What is the prob that defender checks before attacker can make the next move?
    if attack_rate is None:
        attack_rate = 2  # Default attack rate
    if defense_rate is None:
        defense_rate = 2  # Default defense rate
        
    if (attack_rate + defense_rate) == 0:
        # Handle edge case
        p = 0.5
    else:
        p = defense_rate / (attack_rate + defense_rate)
    
    x = np.arange(len(route))
    pmf = p * np.power(1-p, x)
    
    if pmf.sum() > 0:
        pmf = pmf / pmf.sum()
    else:
        # Fallback if all probabilities are zero
        pmf = np.ones(len(route)) / len(route)
    
    return pmf

def create_hardcoded_graph() -> GraphStructure:
    """
    Load graph structure from the sample_graph_structure.json file.

    Returns:
        GraphStructure: The graph structure loaded from JSON file.
    """
    # Path to the sample graph structure JSON file
    json_file_path = "tools/cut_the_rope/benchmark/data/sample_graph_2_vulns/sample_graph_structure.json"
    
    try:
        with open(json_file_path, 'r') as f:
            graph_data = json.load(f)
        
        # Convert JSON data to NodeInfo and EdgeInfo objects
        nodes = []
        for node_data in graph_data["nodes"]:
            nodes.append(NodeInfo(
                id=node_data["id"],
                name=node_data["name"],
                info=node_data["info"],
                vulnerability=node_data["vulnerability"],
                message_id=node_data["message_id"]
            ))
        
        edges = []
        for edge_data in graph_data["edges"]:
            edges.append(EdgeInfo(
                source=edge_data["source"],
                target=edge_data["target"]
            ))
        
        print(f"Successfully loaded graph structure from: {json_file_path}")
        print(f"Loaded {len(nodes)} nodes and {len(edges)} edges")
        
        return GraphStructure(nodes=nodes, edges=edges)
        
    except FileNotFoundError:
        print(f"Error: Could not find {json_file_path}")
        print("Falling back to original hardcoded graph structure...")
        
        # Fallback to original hardcoded structure if file not found
        nodes = [
            NodeInfo(
                id="1",
                name="Entry",
                info="Initial access point",
                vulnerability=False,
                message_id=0
            ),
            NodeInfo(
                id="2", 
                name="Recon",
                info="Network reconnaissance",
                vulnerability=False,
                message_id=1
            ),
            NodeInfo(
                id="3",
                name="Web Service",
                info="Web application discovery",
                vulnerability=False,
                message_id=2
            ),
            NodeInfo(
                id="4",
                name="DB Service", 
                info="Database service discovery",
                vulnerability=False,
                message_id=3
            ),
            NodeInfo(
                id="5",
                name="Web Vuln",
                info="Web application vulnerability",
                vulnerability=True,
                message_id=4
            ),
            NodeInfo(
                id="6",
                name="DB Vuln",
                info="Database vulnerability", 
                vulnerability=True,
                message_id=5
            ),
            NodeInfo(
                id="7",
                name="Pivot Node",
                info="Strategic pivot point",
                vulnerability=False,
                message_id=6
            ),
            NodeInfo(
                id="8",
                name="Crown Jewels",
                info="Ultimate target system",
                vulnerability=True,
                message_id=7
            )
        ]
        
        edges = [
            EdgeInfo(source="1", target="2"),
            EdgeInfo(source="2", target="3"),
            EdgeInfo(source="2", target="4"),
            EdgeInfo(source="3", target="5"),
            EdgeInfo(source="4", target="6"),
            EdgeInfo(source="5", target="7"),
            EdgeInfo(source="7", target="8"),
            EdgeInfo(source="6", target="8"),
            EdgeInfo(source="6", target="7"),
        ]
        
        return GraphStructure(nodes=nodes, edges=edges)
    
    except Exception as e:
        print(f"Error loading graph structure: {str(e)}")
        raise

def create_simulated_log() -> List[Dict[str, Any]]:
    """
    Create log data that matches the message IDs from the loaded graph structure.
    Designed to create balanced edge probabilities for interesting CTR results.

    Returns:
        List[Dict[str, Any]]: Simulated filtered log data with balanced cost patterns.
    """
    return [
        # Message ID 0 - User prompt (low cost)
        {"message_id": 0, "content": "User ask to do a pentest for a domain", "role": "user", "content_tokens": 25},
        
        # Message ID 3 - Reconnaissance (medium cost)
        {"message_id": 3, "content": "Scanning internal network for live hosts and services", "role": "assistant", "content_tokens": 80},
        
        # Message ID 10 - Web Server (medium-high cost)
        {"message_id": 10, "content": "Identifying web technologies and potential entry points", "role": "assistant", "content_tokens": 120},
        
        # Message ID 20 - Database (medium cost)
        {"message_id": 20, "content": "Scanning for database services and metadata exposure", "role": "assistant", "content_tokens": 85},
        
        # Message ID 70 - Brute Force Failed (high cost - failure is expensive)
        {"message_id": 70, "content": "Brute force attempt failed due to patch or WAF detection", "role": "assistant", "content_tokens": 180},
        
        # Message ID 120 - File Upload Vulnerability (medium cost - moderate complexity)
        {"message_id": 120, "content": "File Upload Vulnerability (Remote Code Execution) discovered and exploited", "role": "assistant", "content_tokens": 140},
        
        # Message ID 160 - SQL Injection (medium cost - similar complexity)
        {"message_id": 160, "content": "Exploiting SQL injection to access database backend", "role": "assistant", "content_tokens": 130},
        
        # Message ID 200 - Lateral Movement (high cost - complex operation)
        {"message_id": 200, "content": "Using compromised systems to pivot deeper into the network", "role": "assistant", "content_tokens": 200},
        
        # Message ID 300 - Privilege Escalation (very high cost - final goal)
        {"message_id": 300, "content": "Escalating privileges to gain higher access", "role": "assistant", "content_tokens": 250},
    ]

def run_ctr_baseline_analysis(graph_structure, edge_probabilities, save_path, attack_rate_list=None, defense_rate_list=None):
    """
    Run CTR baseline analysis on the hardcoded graph structure.
    
    Args:
        graph_structure: The hardcoded graph structure
        edge_probabilities: Edge probabilities dictionary
        save_path: Path to save CTR results
        attack_rate_list: List of attack rates (default: [2])
        defense_rate_list: List of defense rates (default: [2])
        
    Returns:
        baseline_result: CTR analysis results dictionary
    """
    if attack_rate_list is None:
        attack_rate_list = [2]
    if defense_rate_list is None:
        defense_rate_list = [2]
    
    # Create attack graph
    full_attack_graph = create_graph_from_agent_output(graph_structure, edge_probabilities)
    
    # Extract attack paths (as2) - replicating the preprocessing steps from ctr_core
    attacker_graph = full_attack_graph.copy()
    atk_virtual_entry_node, attacker_graph, atk_original_roots = find_and_add_entry_node(attacker_graph)
    # Do NOT merge targets - we want to preserve individual vulnerable nodes
    _, V, _, as2, target_list, node_order, adv_list, theta, m = generate_game_elements(
        attacker_graph, atk_virtual_entry_node, atk_original_roots)
    
    # Check if generate_game_elements returned empty values (indicating no targets)
    if not target_list or not V or not as2:
        print("Warning: No target nodes or attack paths found after graph preprocessing. Skipping CTR analysis.")
        return {
            'optimal_defense': {},
            'attacker_strategy': [],
            'defender_success': 0.0,
            'attacker_success': 0.0,
            'error': 'No valid targets found after preprocessing'
        }
    
    # Capture CTR core output
    captured_output = ""
    baseline_result = None
    
    try:
        # Redirect stdout to capture CTR output
        stdout = sys.stdout
        output = io.StringIO()
        sys.stdout = output
        
        # Run CTR baseline analysis
        baseline_result = ctr_core_main(
            full_attack_graph=full_attack_graph,
            defender_subgraphs_list=None,
            attack_rate_list=attack_rate_list,
            defense_rate_list=defense_rate_list,
            random_steps_fn=random_steps,
            run_baseline_only=True
        )
        
        captured_output = output.getvalue()
    except Exception as e:
        print(f"Error during CTR analysis: {str(e)}")
        baseline_result = {
            'optimal_defense': {},
            'attacker_strategy': [],
            'defender_success': 0.0,
            'attacker_success': 0.0,
            'error': f'CTR analysis failed: {str(e)}'
        }
    finally:
        # Restore stdout
        sys.stdout = stdout
    
    # Handle case where baseline_result is None
    if baseline_result is None:
        print("Warning: CTR baseline analysis returned None. Using default values.")
        baseline_result = {
            'optimal_defense': {},
            'attacker_strategy': [],
            'defender_success': 0.0,
            'attacker_success': 0.0,
            'error': 'CTR analysis returned None'
        }
    
    # Save CTR baseline results
    ctr_baseline_file_path = os.path.join(save_path, 'ctr_baseline.txt')
    with open(ctr_baseline_file_path, 'w') as f:
        f.write("CTR Baseline Analysis Results\n")
        f.write("=" * 50 + "\n\n")
        f.write("CAPTURED OUTPUT:\n")
        f.write("-" * 20 + "\n")
        f.write(captured_output)
        f.write("\n\n")
        f.write("BASELINE RESULT DICTIONARY:\n")
        f.write("-" * 30 + "\n")
        f.write(json.dumps(baseline_result, indent=2, default=str))
        f.write("\n\n")
    
    # Create formatted baseline tables with path information
    try:
        if baseline_result and 'error' not in baseline_result:
            with open(ctr_baseline_file_path, 'a') as f:
                f.write("FORMATTED TABLES:\n")
                f.write("-" * 20 + "\n")
            visualize_baseline_results(baseline_result, ctr_baseline_file_path, paths=as2, print_to_console=False)
    except Exception as e:
        print(f"Note: Could not create baseline visualization: {str(e)}")
    
    print(f"CTR baseline results saved to: {ctr_baseline_file_path}")
    return baseline_result

def main():
    """
    Main function to create hardcoded graph, compute edge probabilities, and plot the attack graph.
    """
    # Create hardcoded graph structure
    graph = create_hardcoded_graph()
    
    # Create simulated log data
    filtered_log = create_simulated_log()
    
    # Calculate total tokens from the simulated log
    total_tokens_heuristic = sum(msg["content_tokens"] for msg in filtered_log)
    total_cost_real = 0.80  # Lower cost to create more moderate probabilities
    total_number_messages = len(filtered_log)
    
    # Calculate euro per token with some variation
    if total_tokens_heuristic > 0:   
        euro_per_token_heuristic = total_cost_real / total_tokens_heuristic 
    else:
        euro_per_token_heuristic = 0.0
    
    # Compute edge probabilities with balanced weights to avoid extreme values
    edge_probabilities = compute_edge_probabilities_offline(
        filtered_log=filtered_log,
        graph_structure=graph,
        total_tokens=total_tokens_heuristic,
        total_cost=total_tokens_heuristic * euro_per_token_heuristic,
        total_number_messages=total_number_messages,
        w_cost=0.4,   # Balanced cost weight
        w_msg=0.3,    # Higher message weight for distance consideration  
        w_tokens=0.3, # Balanced token weight
    )
    
    # Fix the starting edge probability (from node 1 to node 2) if it's 0%
    starting_edge_key = "1->2"
    if edge_probabilities.get(starting_edge_key, 0.0) == 0.0:
        # Set a reasonable minimum probability for the starting edge
        edge_probabilities[starting_edge_key] = 0.1  # 10% minimum for starting edge
    
    # Extract node information for visualization
    node_vulnerabilities = {node.id: node.vulnerability for node in graph.nodes}
    node_info_dict = {node.id: node.name for node in graph.nodes}
    
    # Create attack graph
    attack_graph = create_graph_from_agent_output(graph, edge_probabilities)
    
    # Set output path and graph name - fix the name
    type_graph_name = "clean_sample_graph"
    save_path = "tools/cut_the_rope/benchmark/data/" + type_graph_name
    
    # Create directory if it doesn't exist
    import os
    os.makedirs(save_path, exist_ok=True)
    
    # Plot and save the attack graph
    plot_attack_graph(
        attack_graph, 
        save_path=save_path, 
        node_info_dict=node_info_dict, 
        node_vulnerabilities=node_vulnerabilities,
        type_graph=type_graph_name
    )
    
    # Print summary information
    print(f"Graph created with {len(graph.nodes)} nodes and {len(graph.edges)} edges")
    print(f"Vulnerable nodes: {sum(1 for node in graph.nodes if node.vulnerability)}")
    print(f"Total simulated tokens: {total_tokens_heuristic}")
    print(f"Total simulated cost: €{total_cost_real:.4f}")
    print(f"Edge probabilities computed and graph saved to: {save_path}")
    
    # Print edge probabilities for reference
    print("\nEdge Probabilities:")
    for edge in graph.edges:
        edge_key = f"{edge.source}->{edge.target}"
        prob = edge_probabilities.get(edge_key, 0.0)
        source_name = next((node.name for node in graph.nodes if node.id == edge.source), "Unknown")
        target_name = next((node.name for node in graph.nodes if node.id == edge.target), "Unknown")
        print(f"  {source_name} -> {target_name}: {prob:.2%}")
    
    # Save the graph structure as JSON for reference
    graph_json = {
        "nodes": [node.model_dump() for node in graph.nodes],
        "edges": [edge.model_dump() for edge in graph.edges]
    }
    
    json_path = save_path + "/clean_sample_graph_structure.json"
    with open(json_path, "w") as f:
        json.dump(graph_json, f, indent=2)
    
    print(f"\nGraph structure also saved as JSON to: {json_path}")

    # Run CTR baseline analysis
    print(f"\n{'='*60}")
    print("RUNNING CTR BASELINE ANALYSIS")
    print(f"{'='*60}")
    
    # Try different attack/defense rate combinations for more interesting results
    baseline_result = run_ctr_baseline_analysis(
        graph, edge_probabilities, save_path, 
        attack_rate_list=[1.5], defense_rate_list=[1.5]  # More balanced rates
    )
    
    # Print CTR summary
    print(f"\n{'='*60}")
    print("CTR BASELINE ANALYSIS SUMMARY")
    print(f"{'='*60}")
    
    if baseline_result and 'error' not in baseline_result:
        print(f"Defender Success Probability: {baseline_result.get('defender_success', 0.0):.6f}")
        print(f"Attacker Success Probability: {baseline_result.get('attacker_success', 0.0):.6f}")
        
        optimal_defense = baseline_result.get('optimal_defense', {})
        if optimal_defense:
            print(f"\nOptimal Defense Strategy:")
            for location, probability in optimal_defense.items():
                location_name = next((node.name for node in graph.nodes if node.id == str(location)), f"Node {location}")
                print(f"  {location_name}: {probability:.4f}")
        
        attacker_strategy = baseline_result.get('attacker_strategy', [])
        if len(attacker_strategy) > 0:
            print(f"\nAttacker Strategy (probabilities for each path):")
            for i, prob in enumerate(attacker_strategy):
                print(f"  Path {i+1}: {prob:.4f}")
    else:
        print("CTR analysis encountered an error or returned no results.")
        if 'error' in baseline_result:
            print(f"Error: {baseline_result['error']}")
    
    print(f"\nAll results saved to: {save_path}")

if __name__ == "__main__":
    main() 