"""
This script loads a graph structure from a JSON file and computes edge probabilities
based on a provided input log. It then generates and saves a visual attack graph
using the computed probabilities and node vulnerability information.

Usage:
    python tools/cut_the_rope/benchmark/make_graph.py --json tools/cut_the_rope/benchmark/data/prueba/prueba_graph.json --input_log tools/cut_the_rope/benchmark/data/prueba/prueba.jsonl
    python tools/cut_the_rope/benchmark/make_graph.py --json /Users/lidia/Desktop/cai_final4/cai/tools/cut_the_rope/benchmark/data/mercadolibre/mercadolibre_graph.jsonl --input_log /Users/lidia/Desktop/cai_final4/cai/tools/cut_the_rope/benchmark/data/mercadolibre/mercadolibre.jsonl
Arguments:
    --json: Path to the JSON file containing the graph structure (nodes and edges).
    --input_log: Path to the log file containing the real message list for analysis.

The script performs the following steps:
    1. Parses the input log to filter relevant messages.
    2. Computes token and cost statistics from the log.
    3. Loads the graph structure from the specified JSON file.
    4. Computes edge probabilities using a heuristic based on tokens, cost, and message count.
    5. Extracts node vulnerability and name information.
    6. Creates an attack graph using the computed probabilities.
    7. Plots and saves the attack graph visualization.
    8. Runs CTR baseline analysis to compute security game equilibrium.

Example:
   python3 tools/cut_the_rope/benchmark/make_graph.py --json tools/cut_the_rope/benchmark/data/kolesagroup/kolesagroup_graph.json --input_log tools/cut_the_rope/benchmark/data/kolesagroup/kolesagroup.jsonl

"""

import json
import os
import sys
import argparse
import numpy as np
from typing import List, Dict, Any
from pydantic import BaseModel

from tools.cut_the_rope.ctr_cai.experiment_cai import NodeInfo, EdgeInfo, GraphStructure, run_ctr_baseline_analysis, preprocess_graph, postprocess_graph
from tools.cut_the_rope.ctr_cai.probability_computation import compute_edge_probabilities_offline, get_log_tokens
from tools.cut_the_rope.ctr_cai.experiment_cai import parse_input_log
from tools.cut_the_rope.ctr_cai.attack_graph_cai import create_graph_from_agent_output, plot_attack_graph
from cai.sdk.agents.run_to_jsonl import get_token_stats

def random_steps(route, attack_rate=None, defense_rate=None, graph=None):
    """Geometric distribution for randomly moving defender"""
    # What is the prob that defender checks before attacker can make the next move?
    p = defense_rate / (attack_rate + defense_rate)
    x = np.arange(len(route))
    pmf = p * np.power(1-p, x)
    pmf = pmf / pmf.sum()
    return pmf

def load_graph_from_json(json_path: str) -> GraphStructure:
    """
    Load a graph structure from a JSON file.

    Args:
        json_path (str): Path to the JSON file.

    Returns:
        GraphStructure: The loaded graph structure.
    """
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    nodes = [NodeInfo(**node) for node in data["nodes"]]
    edges = [EdgeInfo(**edge) for edge in data["edges"]]
    return GraphStructure(nodes=nodes, edges=edges)


def main():
    """
    Main function to parse arguments, compute edge probabilities, and plot the attack graph.
    """
    parser = argparse.ArgumentParser(description="Load and print a graph from a JSON file")
    parser.add_argument("--json", type=str, required=True, help="Path to the JSON file with graph information")
    parser.add_argument("--input_log", type=str, required=True, help="Path to the real log file (for reach the mesage list)")
    args = parser.parse_args()

    filtered_log = parse_input_log(args.input_log) 
      
    (model_name_log, total_prompt_tokens, total_completion_tokens,
        total_cost_real, last_active_time, last_idle_time) = get_token_stats(args.input_log) 
    total_tokens_real = total_prompt_tokens + total_completion_tokens

    # Obtain heuristic tokens/cost    
    total_tokens_heuristic = get_log_tokens(args.input_log)
    if total_tokens_real > 0:   
        euro_per_token_heuristic = total_cost_real / total_tokens_heuristic 
    else:
        euro_per_token_heuristic = 0.0
    
    total_number_messages = len(filtered_log)
    
    graph = load_graph_from_json(args.json)
    
    # Keep the original graph structure for LLM plotting
    graph_structure_llm = graph
    
    graph = preprocess_graph(graph)

    edge_probabilities = compute_edge_probabilities_offline(
        filtered_log=filtered_log,
        graph_structure=graph,
        total_tokens=total_tokens_heuristic,
        total_cost=total_tokens_heuristic * euro_per_token_heuristic,
        total_number_messages=total_number_messages,
        w_cost=0.3,
        w_msg=0.3,
        w_tokens=0.4,
    )
    edge_probabilities_individual=edge_probabilities
    graph, edge_probabilities, edge_probabilities_individual = postprocess_graph(graph, edge_probabilities, edge_probabilities_individual)

    node_vulnerabilities = {node.id: node.vulnerability for node in graph.nodes}
    node_info_dict = {node.id: node.name for node in graph.nodes}
    attack_graph = create_graph_from_agent_output(graph, edge_probabilities)

    type_graph_name = os.path.splitext(os.path.basename(args.input_log))[0]
    save_path = "tools/cut_the_rope/benchmark/data/"+type_graph_name
    
    # Plot original LLM graph structure (before preprocessing)
    node_vulnerabilities_llm = {node.id: node.vulnerability for node in graph_structure_llm.nodes}
    node_info_dict_llm = {node.id: node.name for node in graph_structure_llm.nodes}
    attack_graph_llm = create_graph_from_agent_output(graph_structure_llm, edge_probabilities)
    plot_attack_graph(attack_graph_llm, save_path=save_path, node_info_dict=node_info_dict_llm, node_vulnerabilities=node_vulnerabilities_llm, type_graph=f"{type_graph_name}_llm")
    
    # Plot regular graph with leaf_ nodes (for computation verification)
    plot_attack_graph(attack_graph, save_path=save_path, node_info_dict=node_info_dict, node_vulnerabilities=node_vulnerabilities, type_graph=type_graph_name)
    
    # Import and plot cleaned graph without leaf_ nodes (for visualization)
    from tools.cut_the_rope.ctr_cai.attack_graph_cai import create_cleaned_graph_for_visualization
    cleaned_graph_structure, cleaned_edge_probabilities = create_cleaned_graph_for_visualization(graph, edge_probabilities)
    cleaned_node_vulnerabilities = {node.id: node.vulnerability for node in cleaned_graph_structure.nodes}
    cleaned_node_info_dict = {node.id: node.name for node in cleaned_graph_structure.nodes}
    cleaned_attack_graph = create_graph_from_agent_output(cleaned_graph_structure, cleaned_edge_probabilities)
    plot_attack_graph(cleaned_attack_graph, save_path=save_path, node_info_dict=cleaned_node_info_dict, node_vulnerabilities=cleaned_node_vulnerabilities, type_graph=f"{type_graph_name}_cleaned")

    # Run CTR baseline analysis
    print(f"\n{'='*60}")
    print("RUNNING CTR BASELINE ANALYSIS")
    print(f"{'='*60}")
    
    baseline_result, ctr_time = run_ctr_baseline_analysis(
        graph_structure=graph,
        edge_probabilities=edge_probabilities,
        output_dir=save_path,
        attack_rate_list=[2],
        defense_rate_list=[2]
    )
    
    print(f"CTR baseline analysis completed in {ctr_time:.4f} seconds")
    print(f"Results saved to: {save_path}/ctr_baseline.txt")

if __name__ == "__main__":
    main()