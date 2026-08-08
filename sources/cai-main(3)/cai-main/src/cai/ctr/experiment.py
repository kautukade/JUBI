"""
CAI-CTR Integration Pipeline - Main Orchestration and Experimental Framework

This module serves as the primary entry point for the CAI-CTR integration, providing 
an automated pipeline that transforms conversation logs into strategic security analysis.
It orchestrates LLM-based attack graph extraction, probability computation, and 
game-theoretic security analysis.

CORE PIPELINE WORKFLOW:
======================
1. Log Processing: Parse JSONL conversation logs with token/cost tracking
2. LLM Graph Extraction: Use CAI agents to generate attack graph structures  
3. Probability Calculation: Compute edge exploitation probabilities based on
   conversation metrics (cost, tokens, message distance)
4. Graph Preprocessing: Clean and prepare graphs for CTR analysis
5. Security Game Analysis: Apply CTR core solver for Nash equilibrium solutions
6. Visualization & Reporting: Generate comprehensive analysis outputs

INTEGRATION CAPABILITIES:
========================
- Single Log Mode: Detailed analysis of individual penetration testing sessions
- Multi-Log Mode: Comparative analysis across multiple engagement logs
- Graph Correlation: Advanced multi-log graph merging and correlation analysis
- Experimental Framework: Configurable parameters for research and evaluation

MULTI-LOG GRAPH MODES:
=====================
1. Maxi Graph: Combines all individual graphs with unique node prefixes
   - Preserves all attack paths and nodes from each log
   - Enables analysis of complex multi-target scenarios
   - Suitable for comprehensive attack surface analysis

2. Simplified Graph: Represents each log as single node with vulnerability status
   - Each log becomes one node (vulnerable/non-vulnerable)
   - Normalized probabilities across vulnerable logs only
   - Suitable for high-level correlation and log comparison

PROBABILITY MODEL IMPLEMENTATION:
===============================
Offline Mode: P_offline,i = W_cost × S_cost_norm + W_msg × S_msg_norm + W_tokens × S_tokens_norm
- Default weights: W_cost=0.3, W_msg=0.3, W_tokens=0.4
- Supports both global normalization (across all logs) and individual normalization
- Handles cost estimation, token consumption, and temporal message distances

OUTPUT STRUCTURE PER RUN:
========================
- system_prompt.txt: LLM prompts used for graph extraction
- graph_information.txt: Detailed metadata, timing, and raw LLM outputs
- ctr_baseline.txt: Nash equilibrium analysis results
- attack_graph_*.png: Various graph visualizations (LLM, processed, cleaned)
- Multi-log runs generate additional correlation analysis in separate subdirectory

USAGE EXAMPLES:
==============
Single log processing:
    python3 tools/ctr_experiment.py --input_log path/to/pentest.jsonl

Multi-log batch processing:
    python3 tools/ctr_experiment.py --input_log path/to/logs_folder/

CTF analysis mode:
    python3 tools/ctr_experiment.py --input_log ctf_log.jsonl --is_ctf

Custom game parameters:
    python3 tools/ctr_experiment.py --input_log logs/ --attack_rate 1,2,3 --defense_rate 0,1

INTEGRATION POINTS:
==================
- CAI SDK: Agent framework, model abstraction, cost tracking
- CTR Core: Security game solver, Nash equilibrium computation
- Attack Graph Utils: NetworkX integration, visualization, preprocessing
- Probability Engine: Multi-factor edge weight calculation
"""

import os
import argparse
import json
import asyncio
import glob
from typing import List, Dict, Any, Optional
from pydantic import BaseModel
import networkx as nx

# Force a non-interactive matplotlib backend to avoid macOS NSWindow creation
# (which crashes from background threads in TUI). Must be set before importing pyplot.
import matplotlib  # noqa: E402
try:
    # Always prefer Agg when running inside CAI to render to files only.
    matplotlib.use("Agg", force=True)  # type: ignore[attr-defined]
except Exception:
    # Fallback: rely on MPLBACKEND env if set; otherwise matplotlib will choose.
    pass

import matplotlib.pyplot as plt
import numpy as np
from scipy.stats import poisson, norm
from copy import deepcopy
import logging
import dotenv
from datetime import datetime, timezone
import time
import sys
import contextlib
import tempfile
import pickle
import io
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel, Runner
from cai.sdk.agents.model_settings import ModelSettings
from openai import AsyncOpenAI
from cai.sdk.agents.run_to_jsonl import load_history_from_jsonl
# from cai.sdk.agents.run_to_jsonl import load_history_from_json_legacy as load_history_from_jsonl
from cai.sdk.agents.run_to_jsonl import get_token_stats
from cai.ctr.attack_graph import create_graph_from_agent_output, plot_attack_graph
from cai.ctr.probability_computation import compute_edge_probabilities_offline
from cai.ctr.visualization import visualize_baseline_results
from cai.ctr.paths import get_ctr_output_base_dir
import litellm


from cai.ctr.core import main as ctr_core_main
from cai.util import calculate_model_cost

# Load .env from current directory only, not from parent directories
dotenv_path = os.path.join(os.getcwd(), '.env')
dotenv.load_dotenv(dotenv_path=dotenv_path, verbose=False)

# Set default for OPENAI_API_KEY if not already set
if "OPENAI_API_KEY" not in os.environ:
    os.environ["OPENAI_API_KEY"] = ""

# # Disable auto-compaction to prevent context issues
# os.environ['CAI_AUTO_COMPACT'] = 'false'

# NOTE: Reasonable limit for the graph structure output
GRAPH_STRUCTURE_MAX_TOKENS = 8192

model_name = os.getenv('CAI_MODEL', "alias1")

def _create_run_dir(output_base_dir: Optional[str] = None) -> str:
    base = get_ctr_output_base_dir(output_base_dir)
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    run_dir = os.path.join(base, f"run_{timestamp}")
    os.makedirs(run_dir, exist_ok=True)
    return run_dir

# Definition for LLM structure output
class NodeInfo(BaseModel):
    id: str
    name: str
    info: str
    vulnerability: bool
    message_id: int

class EdgeInfo(BaseModel):
    source: str
    target: str

class GraphStructure(BaseModel):
    nodes: List[NodeInfo]  
    edges: List[EdgeInfo]  

def preprocess_graph(graph: GraphStructure) -> GraphStructure:
    """
    Preprocess the graph to ensure only one starting node (the node with the minimum id).
    Remove any edges that have this node as their target.
    """
    if not graph.nodes:
        return graph
    # Find the node with the minimum id (as int)
    min_id_node = min(graph.nodes, key=lambda n: int(n.id))
    allowed_start_id = min_id_node.id
    # Remove edges that have the starting node as their target
    filtered_edges = [edge for edge in graph.edges if edge.target != allowed_start_id]
    return GraphStructure(nodes=graph.nodes, edges=filtered_edges)

def postprocess_graph(
    graph: GraphStructure,
    edge_probabilities: dict = None,
    edge_probabilities_individual: dict = None
):
    """
    Post-process the graph and edge probabilities:
    - Recursively remove leaf nodes that are not vulnerable, except the initial entry node (min id).
    - For each vulnerable node, add an artificial leaf node with 100% probability.
    - Remove edge probabilities for removed edges.
    - Add 100% probability for edges to artificial leaf nodes.
    Returns:
        (GraphStructure, edge_probabilities, edge_probabilities_individual)
    """
    if not hasattr(graph, 'nodes') or not hasattr(graph, 'edges'):
        return graph, edge_probabilities, edge_probabilities_individual
    if not graph.nodes:
        return graph, edge_probabilities, edge_probabilities_individual

    # Find the initial entry node (min id as int)
    min_id_node = min(graph.nodes, key=lambda n: int(n.id))
    initial_entry_id = min_id_node.id

    # Helper to check if a node is a leaf
    def is_leaf(node_id, edges):
        return not any(edge.source == node_id for edge in edges)

    # Work on a copy of the node and edge lists
    new_nodes = list(graph.nodes)
    new_edges = list(graph.edges)

    # Make copies of edge probabilities if provided
    edge_probs = dict(edge_probabilities) if edge_probabilities is not None else None
    edge_probs_ind = dict(edge_probabilities_individual) if edge_probabilities_individual is not None else None

    removed = True
    while removed:
        removed = False
        # Do not remove the initial entry node, even if it is a non-vulnerable leaf
        leaf_nodes = [node for node in new_nodes if is_leaf(node.id, new_edges) and not node.vulnerability and node.id != initial_entry_id]
        if not leaf_nodes:
            break
        for leaf in leaf_nodes:
            # Remove the node
            new_nodes = [n for n in new_nodes if n.id != leaf.id]
            # Remove all edges to this node (should be only incoming)
            to_remove_edges = [e for e in new_edges if e.target == leaf.id]
            new_edges = [e for e in new_edges if e.target != leaf.id]
            # Remove edge probabilities for these edges (use string keys)
            if edge_probs is not None:
                for e in to_remove_edges:
                    edge_probs.pop(f"{e.source}->{e.target}", None)
            if edge_probs_ind is not None:
                for e in to_remove_edges:
                    edge_probs_ind.pop(f"{e.source}->{e.target}", None)
            removed = True

    # For vulnerable nodes: create an artificial leaf node joined to this one
    artificial_nodes = []
    artificial_edges = []
    for node in new_nodes:
        if node.vulnerability:
            artificial_id = f"leaf_{node.id}"
            artificial_node = NodeInfo(
                id=artificial_id,
                name=f"Artificial Leaf for {node.name}",
                info=f"Artificial leaf node for vulnerable node {node.id}",
                vulnerability=False,
                message_id=node.message_id
            )
            artificial_nodes.append(artificial_node)
            artificial_edge = EdgeInfo(source=node.id, target=artificial_id)
            artificial_edges.append(artificial_edge)
            # Add 100% probability for this edge (use string keys)
            if edge_probs is not None:
                edge_probs[f"{node.id}->{artificial_id}"] = 1.0
            if edge_probs_ind is not None:
                edge_probs_ind[f"{node.id}->{artificial_id}"] = 1.0
    new_nodes += artificial_nodes
    new_edges += artificial_edges

    # Remove edge probabilities for any edges that are not in the new edge list (use string keys)
    if edge_probs is not None:
        valid_edges = set(f"{e.source}->{e.target}" for e in new_edges)
        to_remove = [k for k in edge_probs if k not in valid_edges]
        for k in to_remove:
            edge_probs.pop(k, None)
    if edge_probs_ind is not None:
        valid_edges = set(f"{e.source}->{e.target}" for e in new_edges)
        to_remove = [k for k in edge_probs_ind if k not in valid_edges]
        for k in to_remove:
            edge_probs_ind.pop(k, None)

    return GraphStructure(nodes=new_nodes, edges=new_edges), edge_probs, edge_probs_ind

def load_prompt_template(message_count=None, max_number_of_nodes=10, min_number_of_nodes=4, is_ctf=False):
    """
    Load a prompt template from the filesystem and replace placeholders with actual values.
    
    Args:
        message_count: Number of messages to use for placeholder replacement
        max_number_of_nodes: Maximum number of nodes to include in the graph.
        min_number_of_nodes: Minimum number of nodes to include in the graph.
        is_ctf: Whether this is a CTF challenge
    Returns:
        The template content as a string with placeholders replaced
    """
    try:
        if model_name.startswith("qwen"):
            template_path = "system_prompts/qwen.md"
        else:
            template_path = "system_prompts/claude.md"

        current_dir = os.path.dirname(os.path.abspath(__file__))
        full_path = os.path.join(current_dir, template_path)
        with open(full_path, 'r', encoding='utf-8') as f:
            template = f.read()
        if message_count is not None:
            template = template.replace("{total_number_of_messages}", str(message_count))
            template = template.replace("{min_number_of_nodes}", str(min_number_of_nodes))
            template = template.replace("{max_number_of_nodes}", str(max_number_of_nodes))
        
        ctf_content = "This is a log for a CTF, flags and files where you find the flags are also considered as vulnerable nodes" if is_ctf else ""
        template = template.replace("{ctf_content}", ctf_content)
            
        return template
    except Exception as e:
        raise ValueError(f"Failed to load template '{template_path}': {str(e)}")

def parse_input_log(input_log: str, max_tool_response_chars: int = 200) -> List[Dict[str, Any]]:
    """
    Parses a JSONL log file and maps the total token count of each JSONL line 
    to the corresponding message.

    Args:
        input_log (str): Path to the JSONL log file.

    Returns:
        List[Dict[str, Any]]: A list of dictionaries, each representing a filtered message with its
            corresponding JSONL line token count and content.
    """
    model = "qwen:Qwen/Qwen1.5-0.5B-Chat"
    
    # Read all raw lines and count their tokens
    raw_line_tokens = []
    with open(input_log, "r", encoding="utf-8") as f:
        for line_idx, line in enumerate(f, start=1):
            line_stripped = line.strip()
            if line_stripped.startswith('{"model"') or line_stripped.startswith('{"id"'):
                try:
                    # Count tokens for the entire JSONL line
                    line_tokens = litellm.token_counter(model=model, text=line_stripped)
                    raw_line_tokens.append(line_tokens)
                except Exception as e:
                    print(f"Error counting tokens for line {line_idx}: {e}")
                    raw_line_tokens.append(0)

    total_raw_tokens = sum(raw_line_tokens)
    
    # Extract all messages with tool response truncation for token efficiency
    messages = load_history_from_jsonl(input_log, truncate_tool_responses=True,
                                        max_tool_response_chars=max_tool_response_chars)
    filtered_log = []
    api_call_idx = 0
    
    for idx, msg in enumerate(messages):
        role = msg.get("role")
        if role not in ("user", "assistant", "tool"):
            continue 
        content = str(msg.get("content", "")).strip() 
        
        # Handle assistant messages with tool calls
        if role == "assistant":
            if msg.get("tool_calls"):
                for tool_call in msg["tool_calls"]:
                    if "function" in tool_call:
                        content = str(tool_call["function"])
            elif not content:
                continue
        if not content:
            continue
        
        # Get the token count from the corresponding JSONL line
        msg_tokens = raw_line_tokens[api_call_idx] if api_call_idx < len(raw_line_tokens) else 0
        api_call_idx += 1

        filtered_log.append({
            "message_id": idx,
            # "content_tokens": msg_tokens,  # @vmayoral: original @Li implementation
            "content_tokens": litellm.token_counter(model=model, text=content),
            "role": role,
            "content": content
        })

    #total_content_tokens = sum(msg["content_tokens"] for msg in filtered_log)
    #print(f"Total content tokens (mapped from lines): {total_content_tokens}")
    #print(f"Total raw tokens: {total_raw_tokens}")  
    #print(f"Difference: {total_raw_tokens - total_content_tokens}")

    return filtered_log


def parse_messages_history(messages: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """
    Build a filtered log structure from in-memory agent message history.

    Args:
        messages: List of CAI message dicts (user/assistant/tool etc.)

    Returns:
        List of dicts with keys: message_id, content_tokens, role, content
    """
    model = "qwen:Qwen/Qwen1.5-0.5B-Chat"
    filtered_log: List[Dict[str, Any]] = []

    import litellm
    msg_index = 0
    for msg in messages or []:
        role = msg.get("role")
        if role not in ("user", "assistant", "tool"):
            continue
        content = msg.get("content", "")
        # If assistant contained tool calls, echo minimal function info like parse_input_log
        if role == "assistant" and msg.get("tool_calls"):
            try:
                tc = msg["tool_calls"]
                # Represent tool calls minimally
                content = str(tc[0].get("function", tc[0])) if tc else ""
            except Exception:
                content = str(content)
        content_str = str(content).strip()
        if not content_str:
            continue
        try:
            tokens = litellm.token_counter(model=model, text=content_str)
        except Exception:
            tokens = 0
        msg_index += 1
        filtered_log.append({
            "message_id": msg_index,
            "content_tokens": tokens,
            "role": role,
            "content": content_str,
        })

    return filtered_log


def save_run_information(model_name_log, input_log, inference_time, inference_cost, ctr_time, plotting_time, total_time, graph_structure_output, run_dir, message, probabilities, system_prompt=None):
    """
    Save detailed run information including timing measurements and costs to output files.
    
    Args:
        model_name_log: Model name from the log
        input_log: Path to input log file
        inference_time: Time for LLM inference in seconds
        inference_cost: Cost for LLM inference in dollars
        ctr_time: Time for CTR baseline analysis in seconds
        plotting_time: Time for graph plotting in seconds
        total_time: Total execution time in seconds
        graph_structure_output: Generated graph structure
        run_dir: Directory to save files
        message: Processed log messages
        probabilities: Edge probabilities
        system_prompt: The actual system prompt used (with variables filled in)
    """
    # Save the system prompt to system_prompt.txt
    if system_prompt:
        system_prompt_path = os.path.join(run_dir, 'system_prompt.txt')
        with open(system_prompt_path, 'w', encoding='utf-8') as f:
            f.write(system_prompt)
    
    # Save graph information with timing
    graph_info_path = os.path.join(run_dir, 'graph_information.txt')
    with open(graph_info_path, 'w') as f:
        f.write(f"Model Name (log): {model_name_log}\n")
        f.write(f"Model Name (inference): {model_name}\n")
        f.write(f"Input Log: {input_log}\n")
        f.write(f"Inference time: {inference_time:.4f} seconds\n")
        f.write(f"Inference cost: ${inference_cost:.6f}\n")
        f.write(f"CTR Baseline execution time: {ctr_time:.4f} seconds\n")
        f.write(f"Plotting time: {plotting_time:.4f} seconds\n")
        f.write(f"Total Execution time: {total_time:.4f} seconds\n")
        
        # Check if this is a multi-log summary with detailed timing breakdown
        timing_breakdown = None
        if isinstance(message, list):
            for msg_item in message:
                if isinstance(msg_item, dict) and "timing_breakdown" in msg_item:
                    timing_breakdown = msg_item["timing_breakdown"]
                    break
        
        if timing_breakdown:
            f.write(f"\n")
            f.write(f"=" * 60 + "\n")
            f.write(f"DETAILED TIMING BREAKDOWN\n")
            f.write(f"=" * 60 + "\n")
            f.write(f"\n")
            
            # Individual logs summary
            individual = timing_breakdown["individual_logs_summary"]
            f.write(f"INDIVIDUAL LOGS TOTAL:\n")
            f.write(f"  Inference time: {individual['total_inference_time']:.4f} seconds\n")
            f.write(f"  Inference cost: ${individual['total_inference_cost']:.6f}\n")
            f.write(f"  CTR time: {individual['total_ctr_time']:.4f} seconds\n")
            f.write(f"  Plotting time: {individual['total_plotting_time']:.4f} seconds\n")
            f.write(f"  Execution time: {individual['total_execution_time']:.4f} seconds\n")
            f.write(f"\n")
            
            # Multi-log processing
            multi_log = timing_breakdown["multi_log_processing"]
            f.write(f"MULTI-LOG PROCESSING:\n")
            f.write(f"  Multi-log CTR time: {multi_log['multi_log_ctr_time']:.4f} seconds\n")
            f.write(f"  Multi-log plotting time: {multi_log['multi_log_plotting_time']:.4f} seconds\n")
            f.write(f"  Multi-log total time: {multi_log['multi_log_total_time']:.4f} seconds\n")
            f.write(f"\n")
            
            # Grand totals
            grand = timing_breakdown["grand_totals"]
            f.write(f"GRAND TOTALS (Individual + Multi-log):\n")
            f.write(f"  Total inference time: {grand['total_inference_time']:.4f} seconds\n")
            f.write(f"  Total inference cost: ${grand['total_inference_cost']:.6f}\n")
            f.write(f"  Total CTR time: {grand['total_ctr_time']:.4f} seconds\n")
            f.write(f"  Total plotting time: {grand['total_plotting_time']:.4f} seconds\n")
            f.write(f"  Total execution time: {grand['total_execution_time']:.4f} seconds\n")
            f.write(f"=" * 60 + "\n")
        
        f.write(f"\n")
        f.write(f"LLM Output:\n")
        f.write(f"--------------------\n")
        f.write(json.dumps(graph_structure_output.model_dump(), indent=2))
        f.write(f"\n------------------------------\n")
        f.write(f"Edge Exploitation Probabilities:\n")
        
        # Sort edge probabilities by probability value in descending order
        edge_prob_items = [(edge, prob) for edge, prob in probabilities.items() 
                          if not edge.startswith('artificial_') and isinstance(prob, (int, float))]
        edge_prob_items.sort(key=lambda x: x[1], reverse=True)
        
        for edge, prob in edge_prob_items:
            # Convert edge format from "source->target" to a more readable format
            if '->' in edge:
                source, target = edge.split('->')
                # Find node names for better readability
                source_name = next((node.name for node in graph_structure_output.nodes if node.id == source), source)
                target_name = next((node.name for node in graph_structure_output.nodes if node.id == target), target)
                f.write(f"Edge ({source_name} -> {target_name}): {prob:.2%}\n")
        
        f.write(f"\n------------------------------\n")
        f.write(f"Message ID log information: {input_log}\n")
        f.write(json.dumps(message, indent=2))
        f.write(f"\n")

def random_steps(route, attack_rate=None, defense_rate=None, graph=None):
    """Geometric distribution for randomly moving defender"""
    # What is the prob that defender checks before attacker can make the next move?
    p = defense_rate / (attack_rate + defense_rate)
    x = np.arange(len(route))
    pmf = p * np.power(1-p, x)
    pmf = pmf / pmf.sum()
    return pmf

def run_ctr_baseline_analysis(graph_structure, edge_probabilities, output_dir, attack_rate_list, defense_rate_list):
    """
    Run CTR baseline analysis and save results to output directory.
    
    Args:
        graph_structure: Graph structure from LLM
        edge_probabilities: Edge probabilities dictionary
        output_dir: Directory to save results
        attack_rate_list: List of attack rates
        defense_rate_list: List of defense rates
    
    Returns:
        tuple: (baseline_result, ctr_time) - CTR analysis results dictionary and time taken
    """
    from cai.ctr.core import find_and_add_entry_node, generate_game_elements
    
    ctr_start_time = time.time()
    
    # Create attack graph
    full_attack_graph = create_graph_from_agent_output(graph_structure, edge_probabilities)
    
    # Extract attack paths (as2) - replicating the preprocessing steps from ctr_core
    attacker_graph = full_attack_graph.copy()
    atk_virtual_entry_node, attacker_graph, atk_original_roots = find_and_add_entry_node(attacker_graph)
    
    # IMPORTANT: Merge targets just like the main CTR function does
    from cai.ctr.core import merge_targets_with_multi_edges
    attacker_graph = merge_targets_with_multi_edges(attacker_graph)
    
    _, V, _, as2, target_list, node_order, adv_list, theta, m = generate_game_elements(
        attacker_graph, atk_virtual_entry_node, atk_original_roots)
    
    # Check if generate_game_elements returned empty values (indicating no targets)
    if not target_list or not V or not as2:
        print("Warning: No target nodes or attack paths found after graph preprocessing. Skipping CTR analysis.")
        ctr_end_time = time.time()
        ctr_time = ctr_end_time - ctr_start_time
        print(f"  CTR analysis skipped due to preprocessing issues (time: {ctr_time:.4f}s)")
        return {
            'optimal_defense': {},
            'attacker_strategy': [],
            'defender_success': 0.0,
            'attacker_success': 0.0,
            'error': 'No valid targets found after preprocessing'
        }, ctr_time
    
    # Capture CTR core output
    captured_output = ""
    try:
        stdout = sys.stdout
        output = io.StringIO()
        sys.stdout = output
        
        ctr_analysis_start = time.time()
        baseline_result = ctr_core_main(
            full_attack_graph=full_attack_graph,
            defender_subgraphs_list=None,
            attack_rate_list=attack_rate_list,
            defense_rate_list=defense_rate_list,
            random_steps_fn=random_steps,
            run_baseline_only=True
        )
        ctr_analysis_end = time.time()
        
        captured_output = output.getvalue()
    finally:
        sys.stdout = stdout
    
    # Handle case where baseline_result is None
    if baseline_result is None:
        print("Warning: CTR baseline analysis returned None. Using default values.")
        baseline_result = {
            'optimal_defense': {},
            'attacker_strategy': [],
            'defender_success': 0.0,
            'attacker_success': 0.0,
            'error': 'CTR analysis failed to produce results'
        }
    
    ctr_end_time = time.time()
    ctr_time = ctr_end_time - ctr_start_time
    ctr_analysis_time = ctr_analysis_end - ctr_analysis_start
    
    print(f"  CTR analysis timing: total={ctr_time:.4f}s, core_analysis={ctr_analysis_time:.4f}s")
    
    # Save CTR baseline results
    ctr_baseline_file_path = os.path.join(output_dir, 'ctr_baseline.txt')
    with open(ctr_baseline_file_path, 'w') as f:
        f.write("CTR Baseline Analysis Results\n")
        f.write("=" * 50 + "\n\n")
        f.write(f"CTR Analysis Timing:\n")
        f.write(f"  Total CTR time: {ctr_time:.4f} seconds\n")
        f.write(f"  Core analysis time: {ctr_analysis_time:.4f} seconds\n")
        f.write(f"  Target nodes found: {len(target_list)}\n")
        f.write(f"  Attack paths found: {len(as2)}\n")
        f.write(f"\n")
        f.write("CAPTURED OUTPUT:\n")
        f.write("-" * 20 + "\n")
        f.write(captured_output)
        f.write("\n\n")
        f.write("BASELINE RESULT DICTIONARY:\n")
        f.write("-" * 30 + "\n")
        f.write(json.dumps(baseline_result, indent=2, default=str))
        f.write("\n")
    
    # Create formatted baseline tables with path information
    # Also persist machine-readable artifacts for downstream consumers (/ctr show)
    try:
        # 1) Baseline result JSON
        nash_json_path = os.path.join(output_dir, 'nash_equilibrium.json')
        with open(nash_json_path, 'w') as jf:
            json.dump(baseline_result, jf, indent=2, default=str)

        # 2) Attack path sequences (as2)
        # Store as a list-of-lists of node ids (strings/ints from CTR core)
        paths_json_path = os.path.join(output_dir, 'attack_paths.json')
        try:
            # ensure JSON-serializable (convert numpy types if any)
            serializable_paths = []
            for p in as2:
                serializable_paths.append([str(x) for x in p])
            with open(paths_json_path, 'w') as pf:
                json.dump({"paths": serializable_paths}, pf, indent=2)
        except Exception as e:
            print(f"Note: Could not persist attack_paths.json: {e}")

        # Console + file pretty tables
        if baseline_result and 'error' not in baseline_result:
            # Print to console with converted path names and also save to file
            visualize_baseline_results(baseline_result, ctr_baseline_file_path, paths=as2, print_to_console=True)
    except Exception as e:
        print(f"Note: Could not create baseline visualization: {str(e)}")
    
    return baseline_result, ctr_time

def plot_attack_graph_with_timing(attack_graph, save_path, node_info_dict, node_vulnerabilities, type_graph="", log_group_labels=None, probability_ranges=None):
    """
    Wrapper function to measure plotting time for attack graph visualization.
    
    Args:
        Same as plot_attack_graph function
    
    Returns:
        float: Time taken for plotting in seconds
    """
    plot_start_time = time.time()
    plot_attack_graph(attack_graph, save_path, node_info_dict, node_vulnerabilities, type_graph, log_group_labels, probability_ranges)
    plot_end_time = time.time()
    return plot_end_time - plot_start_time

async def extract_attack_graph(input_log: Optional[str],
                                is_ctf: bool,
                                attack_rate_list: List[float],
                                defense_rate_list: List[float],
                                messages: Optional[List[Dict[str, Any]]] = None,
                                total_tokens_override: Optional[int] = None):
    """
    LLM Graph Extraction: Process conversation log into attack graph structure.
    
    Orchestrates the extraction of attack graph structures from penetration testing
    or security assessment conversation logs using CAI's agent framework. This function
    handles the core LLM interaction that transforms unstructured conversation data
    into structured attack graph representations.
    
    PROCESSING WORKFLOW:
    1. Log Parsing: Extract messages with token counts and role information
    2. Adaptive Scaling: Adjust expected graph complexity based on conversation length
    3. Template Loading: Select appropriate system prompts (CTF vs. standard pentest)
    4. Agent Configuration: Set up CAI agent with model-specific optimizations
    5. LLM Inference: Generate structured graph output with cost/timing tracking
    6. Validation: Ensure output conforms to expected schema
    
    GRAPH COMPLEXITY SCALING:
    - <70 messages: 12-16% of messages become nodes (detailed analysis)
    - 70-200 messages: 6-12% of messages become nodes (balanced analysis)
    - >200 messages: 3.5-5% of messages become nodes (high-level analysis)
    - Min: 4 nodes, Max: 25 nodes (prevents over/under-complexity)
    
    LLM MODEL ADAPTATIONS:
    - Qwen models: Enhanced output format examples and strict JSON requirements
    - Claude/GPT models: Standard structured generation approach
    - Automatic prompt template selection based on configured model
    
    Args:
        input_log (str): Path to JSONL conversation log file to process
        is_ctf (bool): CTF mode flag - affects prompt to focus on flag/file vulnerabilities
        attack_rate_list (List[float]): Reserved for future CTR analysis (not used in extraction)
        defense_rate_list (List[float]): Reserved for future CTR analysis (not used in extraction)

    Returns:
        tuple: (graph_structure, filtered_messages, inference_time_sec, inference_cost_usd, prompt_used)
            - graph_structure: Pydantic GraphStructure object with nodes/edges
            - filtered_messages: List of processed conversation messages
            - inference_time_sec: LLM inference duration for cost analysis
            - inference_cost_usd: Dollar cost of LLM inference via CAI cost tracking
            - prompt_used: Complete system prompt used for reproducibility
            
    Raises:
        FileNotFoundError: If input_log path does not exist
        ValidationError: If LLM output doesn't match expected GraphStructure schema
        RuntimeError: If LLM inference fails or CAI agent encounters errors
        
    Note:
        This function integrates with CAI's cost tracking system. CTR inference costs
        are accumulated on top of existing agent costs in the session total.
    """
    from cai.util import COST_TRACKER

    # Don't reset the cost tracker - let CTR costs accumulate on top of agent costs
    # This ensures the session total includes both agent and CTR inference costs

    # Start timing for inference
    start_time = time.time()

    # Record cost before CTR-specific inference (for calculating CTR cost only)
    cost_before = COST_TRACKER.session_total_cost
    
    # Parse messages: either from file (JSONL) or in-memory history
    if messages is not None:
        filtered_log = parse_messages_history(messages)
    else:
        filtered_log = parse_input_log(input_log)
    total_tokens = sum(msg.get("content_tokens", 0) for msg in filtered_log)

    if total_tokens_override and total_tokens_override > 0:
        if total_tokens > 0:
            scale = total_tokens_override / float(total_tokens)
            adjusted_tokens = 0
            for idx, msg in enumerate(filtered_log):
                scaled_value = int(round(msg.get("content_tokens", 0) * scale))
                filtered_log[idx]["content_tokens"] = scaled_value
                adjusted_tokens += scaled_value
            # Fix rounding drift on the last message if needed
            drift = total_tokens_override - adjusted_tokens
            if filtered_log and drift != 0:
                filtered_log[-1]["content_tokens"] = max(
                    0,
                    filtered_log[-1]["content_tokens"] + drift,
                )
        else:
            # Evenly distribute tokens when we lack per-message counts
            count = len(filtered_log)
            if count > 0:
                base = total_tokens_override // count
                remainder = total_tokens_override % count
                for idx, msg in enumerate(filtered_log):
                    extra = 1 if idx < remainder else 0
                    filtered_log[idx]["content_tokens"] = base + extra
        total_tokens = total_tokens_override
        print(f"Total log tokens count {total_tokens} (override applied)")
    else:
        print(f"Total log tokens count {total_tokens}")

    # Prepare the log for LLM input: only keep message_id, role, and content
    filtered_log_postprocessed_llm = [
    {"message_id": entry["message_id"], "role": entry["role"], "content": entry["content"]}
    for entry in filtered_log
    ]

    # Convert the filtered log to JSON string for LLM input
    json_input = json.dumps(filtered_log_postprocessed_llm)
    message_count = len(filtered_log)

    # GRAPH SIZE SCALING: Dynamically adjust node count based on conversation length
    # This heuristic ensures graph complexity scales appropriately with log size:
    # - Long conversations (200+ messages): Lower density to avoid overwhelming graphs
    # - Medium conversations (70-199 messages): Moderate density for balanced detail
    # - Short conversations (<70 messages): Higher density to capture key interactions
    # Final bounds: minimum 4 nodes, maximum 25 nodes for practical visualization
    min_number_of_nodes = 4
    max_number_of_nodes = 10
    if message_count >= 200: 
        min_number_of_nodes = message_count * 0.035
        max_number_of_nodes = message_count * 0.05
    elif 70 <= message_count < 200:  
        min_number_of_nodes = message_count * 0.06
        max_number_of_nodes = message_count * 0.12
    else:  
        min_number_of_nodes = message_count * 0.12
        max_number_of_nodes = message_count * 0.16

    min_number_of_nodes = max(4, int(min_number_of_nodes))
    max_number_of_nodes = min(25, int(max_number_of_nodes))

    # Load the prompt template for the LLM, with placeholders filled in
    instructions = load_prompt_template(
        message_count=message_count,
        max_number_of_nodes=max_number_of_nodes,
        min_number_of_nodes=min_number_of_nodes,
        is_ctf=is_ctf
    )

    # Create the LLM agent for graph structure extraction
    # Set a reasonable max_tokens to prevent LiteLLM's default (36% of context)
    # This leaves room for large inputs while ensuring we don't exceed context limits
    jsonl_to_graph_agent = Agent(
        name="JSON Graph Structure Generator",
        description="Converts input conversation into JSON graph structure with numeric node IDs and edges.",
        instructions=instructions,
        output_type=GraphStructure,
        model_settings=ModelSettings(
            max_tokens=GRAPH_STRUCTURE_MAX_TOKENS  # Reasonable limit for graph structure output
        ),
        model=OpenAIChatCompletionsModel(
            model=model_name,
            openai_client=AsyncOpenAI(),
        )
    )

    # Capture session cost before inference
    try:
        from cai.util import COST_TRACKER
        pre_inference_cost = COST_TRACKER.session_total_cost
    except ImportError:
        pre_inference_cost = 0.0

    # Run the agent to generate the graph structure
    if model_name.startswith("qwen"):
        # For Qwen models, append a strict output format example to the input
        graph_structure = await Runner.run(
            starting_agent=jsonl_to_graph_agent,
            input=json_input + """**OUTPUT FORMAT EXAMPLE**
Your response must be a valid JSON object with this exact structure (IMPORTANT: DO NOT include any markdown formatting, triple backticks, or language tags. Output ONLY the raw JSON object, with no extra text or formatting):
Fill in every field!!
Add as many nodes and edges as they represent the pentesting exercise! 
{
  "nodes": [
    {
      "id": string,              // Unique string ID (e.g., "1", "2", ...)
      "name": string,            // Descriptive name
      "info": string,            // Brief detail
      "vulnerability": boolean,  // True if it's a final vulnerability
      "message_id": integer      // First message ID where it appears
    },
    {...}
  ],
  "edges": [
    {
      "source": string,          // ID of source node
      "target": string           // ID of target node
    },
    {...}
  ]
}""",
        )
    else:
        # For other models, just use the JSON input
        graph_structure = await Runner.run(
            starting_agent=jsonl_to_graph_agent,
            input=json_input
        )

    end_time = time.time()
    inference_time = end_time - start_time

    # Capture session cost after inference and calculate the difference
    try:
        from cai.util import COST_TRACKER
        post_inference_cost = COST_TRACKER.session_total_cost
        inference_cost = post_inference_cost - pre_inference_cost

        # CTR inference cost is now added to the session total
        # (agent costs + CTR costs are accumulated together)
    except ImportError:
        inference_cost = 0.0

    # Return the graph structure, filtered log, inference time, inference cost, and system prompt
    return graph_structure.final_output, filtered_log, inference_time, inference_cost, instructions

def find_first_vulnerable_node(graph_structure):
    """Helper to find the first vulnerable node by message_id."""
    return next((node for node in sorted(graph_structure.nodes, key=lambda x: x.message_id) 
                if node.vulnerability), None)

def find_last_vulnerable_node(graph_structure):
    """Helper to find the last vulnerable node by message_id."""
    vulnerable_nodes = [node for node in graph_structure.nodes if node.vulnerability]
    if not vulnerable_nodes:
        return None
    return max(vulnerable_nodes, key=lambda x: x.message_id)

def find_highest_probability_edge_from_starting_node(graph_structure, edge_probabilities):
    """
    Find the highest probability edge from the starting node.
    This handles cases where starting node has multiple outgoing edges.
    
    Args:
        graph_structure: The graph structure with nodes and edges
        edge_probabilities: Dictionary of edge probabilities
    
    Returns:
        float: The highest probability among all edges from starting node
    """
    starting_node = min(graph_structure.nodes, key=lambda x: x.message_id)
    # Find all outgoing edges from starting node
    outgoing_edges = [edge for edge in graph_structure.edges if edge.source == starting_node.id]
    if not outgoing_edges:
        return 0.0 
    # Find the highest probability among all outgoing edges
    max_probability = 0.0
    for edge in outgoing_edges:
        edge_key = f"{edge.source}->{edge.target}"
        probability = edge_probabilities.get(edge_key, 0.0)
        max_probability = max(max_probability, probability)
    return max_probability if max_probability > 0.0 else 0.01

def create_log_node_mapping(graph_structure, log_idx):
    """
    Create a mapping from original node IDs to new node IDs for a specific log.
    Args:
        graph_structure: The graph structure containing nodes to be remapped.
        log_idx (int): The index of the log (used to prefix node IDs and names).

    Returns:
        tuple: (log_node_mapping, new_nodes)
            - log_node_mapping (dict): Maps original node IDs to new node IDs.
            - new_nodes (list): List of NodeInfo objects with updated IDs and names.
    """
    log_node_mapping = {}
    new_nodes = []
    
    for node in graph_structure.nodes:
        original_id = node.id
        new_id = f"log{log_idx}_{original_id}"
        log_node_mapping[original_id] = new_id
        
        new_nodes.append(NodeInfo(
            id=new_id,
            name=f"L{log_idx}:{node.name}",
            info=node.info,
            vulnerability=node.vulnerability,
            message_id=node.message_id
        ))
    
    return log_node_mapping, new_nodes

def compute_initial_to_log_probability(starting_node, first_vulnerable_node, filtered_log, total_tokens_all_logs, total_cost_all_logs, total_number_messages):
    """
    Calculates the probability from the initial node to the first vulnerable node in a log.

    Args:
        starting_node: The node representing the start of the log.
        first_vulnerable_node: The first node in the log identified as vulnerable.
        filtered_log: The filtered log messages for the current log.
        total_tokens_all_logs: The total number of tokens across all logs.
        total_cost_all_logs: The total cost across all logs.
        total_number_messages: The total number of messages across all logs.

    Returns:
        float: The computed probability from the starting node to the first vulnerable node.
               Returns 0.0 if there is no vulnerable node.

    """
    if not first_vulnerable_node:
        return 0.0  # Zero probability for logs without vulnerabilities
    
    # Create a minimal fake graph structure for the probability computation
    fake_nodes = [starting_node, first_vulnerable_node]
    fake_edges = [EdgeInfo(source=starting_node.id, target=first_vulnerable_node.id)]
    from types import SimpleNamespace
    fake_graph = SimpleNamespace(nodes=fake_nodes, edges=fake_edges)
    
    # Use existing probability computation function
    probabilities = compute_edge_probabilities_offline(
        filtered_log=filtered_log,
        graph_structure=fake_graph,
        total_tokens=total_tokens_all_logs,
        total_cost=total_cost_all_logs,
        total_number_messages=total_number_messages
    )
    
    edge_key = f"{starting_node.id}->{first_vulnerable_node.id}"
    return probabilities.get(edge_key, 0.0)

def create_multi_log_graphs(results, total_tokens_all_logs, total_cost_all_logs, total_number_messages, main_run_dir, attack_rates, defense_rates):
    """
    This function generates two types of multi-log graphs:
        1. Maxi Graph: Combines all logs into a single large graph, preserving all nodes and edges.
        2. Simplified Graph: Each log is represented as a single node, with edges from a central 'Initial' node.
    It also creates a CTR (Cut-the-Rope) baseline graph for further analysis.

    Args:
        results (list): List of dictionaries, each containing log analysis results, including graph structures and probabilities.
        total_tokens_all_logs (int): Total number of tokens across all logs.
        total_cost_all_logs (float): Total cost across all logs.
        total_number_messages (int): Total number of messages across all logs.
        main_run_dir (str): Directory where output files and graphs will be saved.
        attack_rates (list): List of attack rates for CTR baseline analysis.
        defense_rates (list): List of defense rates for CTR baseline analysis.

    Returns:
        tuple: (graph_dict, multi_log_ctr_time, multi_log_plotting_time) where:
            - graph_dict: Dictionary with keys "maxi" and "simplified", each mapping to a tuple of (GraphStructure, edge_probabilities)
            - multi_log_ctr_time: Time spent on CTR analysis for multi-log graphs
            - multi_log_plotting_time: Time spent on plotting multi-log graphs
    """
    graph_run_dir = os.path.join(main_run_dir, 'multi_log_graphs')
    os.makedirs(graph_run_dir, exist_ok=True)
    
    multi_log_ctr_time = 0.0
    multi_log_plotting_time = 0.0
    
    # Calculate accumulated timing from all individual logs
    total_individual_inference_time = sum(result.get("inference_execution_time", 0.0) for result in results)
    total_individual_inference_cost = sum(result.get("inference_cost", 0.0) for result in results)
    total_individual_ctr_time = sum(result.get("ctr_time", 0.0) for result in results)
    total_individual_plotting_time = sum(result.get("plotting_time", 0.0) for result in results)
    total_individual_execution_time = sum(result.get("total_execution_time", 0.0) for result in results)
    
    # Get model name from the first result (should be consistent across all logs)
    model_name = results[0]["model_name_log"] if results else "unknown"

    # --------- LOG MAPPING ---------
    # Build a mapping from log index to file info and vulnerability status
    log_mapping = {}
    for idx, result in enumerate(results, 1):
        log_file = result["log_file"]
        graph_structure = result["graph_structure"]
        filtered_log = result["filtered_log"]
        log_mapping[f"Log {idx}"] = {
            "file_path": log_file,
            "file_name": os.path.basename(log_file),
            "node_count": len(graph_structure.nodes),
            "edge_count": len(graph_structure.edges),
            "message_count": len(filtered_log),
            "has_vulnerabilities": any(node.vulnerability for node in graph_structure.nodes)
        }

    # Write the log mapping to a file for user reference
    mapping_file_path = os.path.join(graph_run_dir, 'log_mapping.txt')
    with open(mapping_file_path, 'w') as f:
        f.write(f"{'='*60}\n")
        f.write("LOG MAPPING FOR MULTI-LOG GRAPHS (MAXI & SIMPLIFIED)\n")
        f.write(f"{'='*60}\n\n")
        f.write("This file shows which log number corresponds to which log file.\n")
        f.write("Use this to understand the graph structure and node naming.\n\n")
        f.write("LOG MAPPING:\n") 
        f.write("-" * 15 + "\n")
        for log_id, info in log_mapping.items():
            f.write(f"{log_id}:\n")
            f.write(f"  File Name: {info['file_name']}\n")
            f.write(f"  Full Path: {info['file_path']}\n")
            f.write(f"  Has Vulnerabilities: {info['has_vulnerabilities']}\n")
            f.write(f"  Original Nodes: {info['node_count']}\n")
            f.write(f"  Original Edges: {info['edge_count']}\n")
            f.write(f"  Messages: {info['message_count']}\n")
            f.write("-" * 30 + "\n")
        f.write("\n")
        f.write(f"Total tokens across all logs: {total_tokens_all_logs:,.1f}\n")
        f.write(f"Total cost across all logs: ${total_cost_all_logs:.6f}\n")
        f.write("\n")

    # --------- MAXI GRAPH ---------
    # Build a unified graph containing all nodes and edges from all logs
    all_nodes_maxi = []
    all_edges_maxi = []
    all_probabilities_maxi = {}
    initial_node_id = results[0]["log_file"].split("/")[-2]
    all_nodes_maxi.append(NodeInfo(
        id="Initial",
        name=initial_node_id,
        info="Central starting point for multi-log graph",
        vulnerability=False,
        message_id=0
    ))
    for idx, result in enumerate(results, 1):
        graph_structure = result["graph_structure_llm"] 
        filtered_log = result["filtered_log"]
        # Map original node IDs to new unique IDs for this log
        log_node_mapping, new_nodes = create_log_node_mapping(graph_structure, idx)
        all_nodes_maxi.extend(new_nodes)
        for edge in graph_structure.edges:
            # Only add edges if both source and target nodes exist in the mapping
            if edge.source not in log_node_mapping:
                print(f"Warning: Edge source node '{edge.source}' not found in log {idx} node mapping. Skipping edge {edge.source}->{edge.target}")
                continue
            if edge.target not in log_node_mapping:
                print(f"Warning: Edge target node '{edge.target}' not found in log {idx} node mapping. Skipping edge {edge.source}->{edge.target}")
                continue
            source_new_id = log_node_mapping[edge.source]
            target_new_id = log_node_mapping[edge.target]
            edge_key = f"{edge.source}->{edge.target}"
            # Use global probabilities for multi-log
            original_prob = result["edge_probabilities"].get(edge_key, 0.0)
            all_edges_maxi.append(EdgeInfo(source=source_new_id, target=target_new_id))
            all_probabilities_maxi[f"{source_new_id}->{target_new_id}"] = original_prob
        # Find the starting node (lowest message_id) for this log
        starting_node = min(graph_structure.nodes, key=lambda x: x.message_id)
        starting_node_new_id = log_node_mapping[starting_node.id]
        # Find the highest probability edge from the starting node
        initial_prob = find_highest_probability_edge_from_starting_node(graph_structure, result["edge_probabilities"])
        # Add an edge from the central Initial node to the log's starting node
        all_edges_maxi.append(EdgeInfo(source="Initial", target=starting_node_new_id))
        all_probabilities_maxi[f"Initial->{starting_node_new_id}"] = initial_prob

    # Save and plot the maxi graph
    graph_structure_maxi = GraphStructure(nodes=all_nodes_maxi, edges=all_edges_maxi)
    
    # Measure plotting time for maxi graph
    attack_graph_maxi = create_graph_from_agent_output(graph_structure_maxi, all_probabilities_maxi)
    node_vulnerabilities_maxi = {node.id: node.vulnerability for node in all_nodes_maxi}
    node_info_dict_maxi = {node.id: node.name for node in all_nodes_maxi}
    log_group_labels = {}
    for idx, result in enumerate(results, 1):
        log_group_labels[f"log{idx}"] = os.path.basename(result["log_file"])
    maxi_plotting_time = plot_attack_graph_with_timing(attack_graph_maxi, graph_run_dir, node_info_dict_maxi, node_vulnerabilities_maxi, type_graph="maxi", log_group_labels=log_group_labels)
    multi_log_plotting_time += maxi_plotting_time
    
    # Run CTR baseline analysis and save results - measure timing
    maxi_baseline_result, maxi_ctr_time = run_ctr_baseline_analysis(graph_structure_maxi, all_probabilities_maxi, graph_run_dir, attack_rates, defense_rates)
    multi_log_ctr_time += maxi_ctr_time

    # --------- SIMPLIFIED GRAPH ---------
    all_nodes_simplified = []
    all_edges_simplified = []
    all_probabilities_simplified = {}
    all_nodes_simplified.append(NodeInfo(
        id="Initial",
        name="Initial",
        info="Central starting point for simplified graph",
        vulnerability=False,
        message_id=0
    ))
    log_vuln_probs = [] 
    log_vuln_probs_ranges = []  # Store probability ranges (first, last)
    for idx, result in enumerate(results, 1):
        graph_structure = result["graph_structure"]
        filtered_log = result["filtered_log"]
        has_vulnerable_nodes = log_mapping[f"Log {idx}"]["has_vulnerabilities"]
        log_node_id = f"log_{idx}"
        log_file_name = result["log_file"].split("/")[-1]
        log_node_name = f"{log_file_name}"
        all_nodes_simplified.append(NodeInfo(
            id=log_node_id,
            name=log_node_name,
            info=f"Log {idx}: {os.path.basename(result['log_file'])}",
            vulnerability=has_vulnerable_nodes,
            message_id=idx
        ))
        starting_node = min(graph_structure.nodes, key=lambda x: x.message_id)
        first_vulnerable_node = find_first_vulnerable_node(graph_structure)
        last_vulnerable_node = find_last_vulnerable_node(graph_structure)
        
        # If log has no vulnerable nodes, set probabilities to 0.0
        if not has_vulnerable_nodes:
            first_prob = 0.0
            last_prob = 0.0
        else:
            # Compute probability to first vulnerable node
            first_prob = compute_initial_to_log_probability(
                starting_node, first_vulnerable_node, filtered_log,
                total_tokens_all_logs, total_cost_all_logs, total_number_messages   
            )
            # Compute probability to last vulnerable node  
            last_prob = compute_initial_to_log_probability(
                starting_node, last_vulnerable_node, filtered_log,
                total_tokens_all_logs, total_cost_all_logs, total_number_messages
            )
        log_vuln_probs.append((log_node_id, first_prob))
        log_vuln_probs_ranges.append((log_node_id, first_prob, last_prob))

    # Separate vulnerable and non-vulnerable logs for normalization
    vulnerable_indices = []
    non_vulnerable_indices = []
    first_probs = []
    last_probs = []
    for i, (log_node_id, first_prob, last_prob) in enumerate(log_vuln_probs_ranges):
        first_probs.append(first_prob)
        last_probs.append(last_prob)
        # Check if this log has vulnerabilities by looking at the corresponding result
        result = results[i]
        has_vulnerabilities = log_mapping[f"Log {i+1}"]["has_vulnerabilities"]
        if has_vulnerabilities:
            vulnerable_indices.append(i)
        else:
            non_vulnerable_indices.append(i)
    
    # Only normalize probabilities among vulnerable logs
    vulnerable_first_probs = [first_probs[i] for i in vulnerable_indices]
    vulnerable_last_probs = [last_probs[i] for i in vulnerable_indices]
    
    # Normalize first probabilities (only among vulnerable logs)
    total_vulnerable_first_prob = sum(vulnerable_first_probs)
    if total_vulnerable_first_prob > 0:
        normalized_vulnerable_first_probs = [prob / total_vulnerable_first_prob for prob in vulnerable_first_probs]
    else:
        n_vulnerable = len(vulnerable_first_probs)
        normalized_vulnerable_first_probs = [1.0 / n_vulnerable] * n_vulnerable if n_vulnerable > 0 else []
    
    # Normalize last probabilities (only among vulnerable logs)
    total_vulnerable_last_prob = sum(vulnerable_last_probs)
    if total_vulnerable_last_prob > 0:
        normalized_vulnerable_last_probs = [prob / total_vulnerable_last_prob for prob in vulnerable_last_probs]
    else:
        n_vulnerable = len(vulnerable_last_probs)
        normalized_vulnerable_last_probs = [1.0 / n_vulnerable] * n_vulnerable if n_vulnerable > 0 else []
    
    # Build final normalized probabilities lists (keeping 0.0 for non-vulnerable logs)
    normalized_first_probs = [0.0] * len(first_probs)
    normalized_last_probs = [0.0] * len(last_probs)
    # Fill in normalized probabilities for vulnerable logs
    for i, vuln_idx in enumerate(vulnerable_indices):
        normalized_first_probs[vuln_idx] = normalized_vulnerable_first_probs[i]
        normalized_last_probs[vuln_idx] = normalized_vulnerable_last_probs[i]
    # Non-vulnerable logs keep 0.0 probability (already set above)
    
    # Store probability ranges for edge labels (using normalized values)
    probability_ranges = {}
    for i, (log_node_id, _, _) in enumerate(log_vuln_probs_ranges):
        edge_key = f"Initial->{log_node_id}"
        probability_ranges[edge_key] = (normalized_first_probs[i], normalized_last_probs[i])
    
    # Use normalized first probabilities for edge weights
    for i, (log_node_id, _, _) in enumerate(log_vuln_probs_ranges):
        all_edges_simplified.append(EdgeInfo(source="Initial", target=log_node_id))
        all_probabilities_simplified[f"Initial->{log_node_id}"] = normalized_first_probs[i]

    # Save and plot the simplified graph
    graph_structure_simplified = GraphStructure(nodes=all_nodes_simplified, edges=all_edges_simplified)
    
    # Measure plotting time for simplified graph
    attack_graph_simplified = create_graph_from_agent_output(graph_structure_simplified, all_probabilities_simplified)
    node_vulnerabilities_simplified = {node.id: node.vulnerability for node in all_nodes_simplified}
    node_info_dict_simplified = {node.id: node.name for node in all_nodes_simplified}
    log_group_labels = {}
    for idx, result in enumerate(results, 1):
        log_group_labels[f"log{idx}"] = os.path.basename(result["log_file"])
    simplified_plotting_time = plot_attack_graph_with_timing(attack_graph_simplified, graph_run_dir, node_info_dict_simplified, node_vulnerabilities_simplified, type_graph="simplified", log_group_labels=log_group_labels, probability_ranges=probability_ranges)
    multi_log_plotting_time += simplified_plotting_time

    # Calculate total accumulated times for multi-log operations
    total_accumulated_inference_time = total_individual_inference_time  # All individual inference times (no multi-log inference)
    total_accumulated_inference_cost = total_individual_inference_cost # All individual inference costs
    total_accumulated_ctr_time = total_individual_ctr_time + multi_log_ctr_time  # Individual CTR + multi-log CTR
    total_accumulated_plotting_time = total_individual_plotting_time + multi_log_plotting_time  # Individual plotting + multi-log plotting
    total_accumulated_execution_time = total_individual_execution_time + multi_log_ctr_time + multi_log_plotting_time

    # Create detailed timing breakdown message for multi-log summary
    timing_breakdown = {
        "individual_logs_summary": {
            "total_inference_time": total_individual_inference_time,
            "total_inference_cost": total_individual_inference_cost,
            "total_ctr_time": total_individual_ctr_time,
            "total_plotting_time": total_individual_plotting_time,
            "total_execution_time": total_individual_execution_time
        },
        "multi_log_processing": {
            "multi_log_ctr_time": multi_log_ctr_time,
            "multi_log_plotting_time": multi_log_plotting_time,
            "multi_log_total_time": multi_log_ctr_time + multi_log_plotting_time
        },
        "grand_totals": {
            "total_inference_time": total_accumulated_inference_time,
            "total_inference_cost": total_accumulated_inference_cost,
            "total_ctr_time": total_accumulated_ctr_time,
            "total_plotting_time": total_accumulated_plotting_time,
            "total_execution_time": total_accumulated_execution_time
        }
    }

    # Save run information with accumulated timing from all logs PLUS multi-log processing
    # For multi-log summary (this should go to the multi_log_graphs subdirectory)
    save_run_information(
        model_name_log=model_name,
        input_log=f"multi_log_summary_{len(results)}_logs",  # Clear indication this is multi-log
        inference_time=total_accumulated_inference_time,  # Sum of all individual inference times
        inference_cost=total_accumulated_inference_cost, # Sum of all individual inference costs
        ctr_time=total_accumulated_ctr_time,  # Sum of all individual CTR times + multi-log CTR time
        plotting_time=total_accumulated_plotting_time,  # Sum of all individual plotting times + multi-log plotting time
        total_time=total_accumulated_execution_time,  # Total of everything
        graph_structure_output=graph_structure_simplified,
        run_dir=graph_run_dir,  # This correctly points to the multi_log_graphs directory
        message=[
            {"info": f"Multi-log summary: {len(results)} logs processed", "logs": [os.path.basename(r['log_file']) for r in results]},
            {"timing_breakdown": timing_breakdown}
        ],
        probabilities=all_probabilities_simplified,
        system_prompt=None  # No single system prompt for multi-log summary
    )

    return {
        "maxi": (graph_structure_maxi, all_probabilities_maxi),
        "simplified": (graph_structure_simplified, all_probabilities_simplified)
    }, multi_log_ctr_time, multi_log_plotting_time


async def process_in_memory_session(
    messages: List[Dict[str, Any]],
    token_counts: Optional[Dict[str, Any]],
    is_ctf: bool,
    attack_rate_list: List[float],
    defense_rate_list: List[float],
    distance_heuristic: Optional[str] = None) -> Dict[str, Any]:
    """
    Process in-memory conversation session for CTR analysis.

    Specialized handler for active CAI sessions that extracts attack graphs
    from in-memory message history without requiring JSONL files. This enables
    real-time CTR analysis during live penetration testing sessions.

    Args:
        messages: List of conversation messages from active CAI session
        token_counts: Token usage statistics from the session
        is_ctf: Whether this is a CTF analysis mode
        attack_rate_list: Attack rates for game analysis
        defense_rate_list: Defense rates for game analysis
        distance_heuristic: Optional distance metric for analysis:
            - 'token_weighted': Weight by token count (default)
            - 'cost_weighted': Weight by cost metrics
            - 'message_uniform': Uniform weight per message
            - 'hybrid': Balanced weighting across metrics

    Returns:
        Dict containing analysis results with keys:
            - log_file: "__in_memory__" marker
            - model_name_log: Model used in session
            - graph_structure_llm: Raw LLM-generated graph
            - filtered_log: Processed messages with token counts
            - total_tokens_real: Actual token count
            - total_cost_real: Actual session cost
            - inference_execution_time: LLM inference duration
            - inference_cost: Cost of graph extraction
            - system_prompt: Prompt used for extraction
    """
    # Note: Printing is handled by the caller in run() function
    model_name_log = os.getenv('CAI_MODEL', model_name)
    total_tokens_real = 0
    total_cost_real = 0.0
    real_values_source = "heuristic"

    # Extract token counts from provided statistics
    used_token_counts = False
    if token_counts:
        input_tokens = int(token_counts.get('input_tokens') or 0)
        output_tokens = int(token_counts.get('output_tokens') or 0)
        total_from_counts = token_counts.get('total_tokens')
        if total_from_counts is None:
            total_from_counts = input_tokens + output_tokens
        total_from_counts = int(total_from_counts or 0)

        if total_from_counts > 0:
            total_tokens_real = total_from_counts
            # If total provided but individual buckets missing, best effort split
            if input_tokens == 0 and output_tokens == 0:
                input_tokens = total_tokens_real
            elif input_tokens + output_tokens == 0:
                input_tokens = total_tokens_real
                output_tokens = 0
            elif input_tokens + output_tokens != total_tokens_real:
                # Prefer preserving provided input tokens; adjust output to match total
                output_tokens = max(total_tokens_real - input_tokens, 0)

            try:
                total_cost_real = calculate_model_cost(str(model_name_log), input_tokens, output_tokens)
            except Exception:
                total_cost_real = 0.0
            used_token_counts = True
            real_values_source = "provided_token_counts"

    if not used_token_counts:
        # Fallback: estimate tokens and cost heuristically
        heuristic_model = "qwen:Qwen/Qwen1.5-0.5B-Chat"
        try:
            total_tokens_real = 0
            for msg in messages:
                text = str(msg.get("content", ""))
                total_tokens_real += litellm.token_counter(model=heuristic_model, text=text)
        except Exception:
            total_tokens_real = 0
        try:
            from cai.util import COST_TRACKER
            total_cost_real = float(COST_TRACKER.session_total_cost)
        except Exception:
            total_cost_real = 0.0
        real_values_source = "heuristic_messages"

    print(f"Total tokens (real) {total_tokens_real} [source={real_values_source}]")
    print(f"Total cost (real) {total_cost_real}")

    # Apply distance heuristic for token/cost calculations
    if distance_heuristic == 'cost_weighted':
        # Prioritize cost in the analysis
        total_tokens_heuristic = int(total_tokens_real * 0.7 + (total_cost_real * 10000) * 0.3)
    elif distance_heuristic == 'message_uniform':
        # Uniform weight per message
        total_tokens_heuristic = len(messages) * 100  # Fixed tokens per message
    elif distance_heuristic == 'hybrid':
        # Balance tokens, cost, and message count
        msg_weight = len(messages) * 50
        total_tokens_heuristic = int(total_tokens_real * 0.5 + (total_cost_real * 10000) * 0.3 + msg_weight * 0.2)
    else:
        # Default: token_weighted or None
        total_tokens_heuristic = total_tokens_real

    euro_per_token_heuristic = total_cost_real / total_tokens_heuristic if total_tokens_heuristic > 0 else 0.0

    print(f"Total tokens (heuristic) {total_tokens_heuristic} [heuristic={distance_heuristic or 'token_weighted'}]")
    print(f"Euro per token (heuristic) {euro_per_token_heuristic}")

    # Process messages through LLM for graph extraction
    graph_structure, filtered_log, inference_time, inference_cost, instructions = await extract_attack_graph(
        input_log=None,
        is_ctf=is_ctf,
        attack_rate_list=attack_rate_list,
        defense_rate_list=defense_rate_list,
        messages=messages,
        total_tokens_override=total_tokens_real if total_tokens_real > 0 else None,
    )

    # Apply preprocessing to graph structure (same as file-based processing)
    graph_structure_preprocessed = preprocess_graph(graph_structure)

    return {
        "log_file": "__in_memory__",
        "model_name_log": model_name_log,
        "graph_structure_llm": graph_structure_preprocessed,  # Use preprocessed graph
        "filtered_log": filtered_log,
        "total_tokens_heuristic": total_tokens_heuristic,
        "total_tokens_real": total_tokens_real,
        "total_cost_real": total_cost_real,
        "euro_per_token_heuristic": euro_per_token_heuristic,
        "inference_execution_time": inference_time,
        "inference_cost": inference_cost,
        "system_prompt": instructions
    }


def _debug_display_messages(messages: List[Dict[str, Any]], token_counts: Optional[Dict[str, Any]] = None):
    """Display messages using history command's format for debugging."""
    from rich.console import Console
    from rich.table import Table
    from cai.repl.commands.history import HistoryCommand

    console = Console()
    history_cmd = HistoryCommand()

    # Create a table for the history (same as history command)
    table = Table(
        title=f"In-Memory Conversation History ({len(messages)} messages)",
        show_header=True,
        header_style="bold yellow",
    )
    table.add_column("#", style="dim")
    table.add_column("Role", style="cyan")
    table.add_column("Content", style="green")

    # Add messages to the table using history command's formatter
    for idx, msg in enumerate(messages, 1):
        role = msg.get("role", "unknown")
        content = msg.get("content", "")
        tool_calls = msg.get("tool_calls", None)

        # Use history command's formatter for consistent display
        formatted_content = history_cmd._format_message_content_full(content, tool_calls)

        # Color the role based on type
        role_style = {
            "user": "cyan",
            "assistant": "yellow",
            "system": "blue",
            "tool": "magenta",
        }.get(role, "white")

        # Add a newline between each role for better readability
        if idx > 1:
            table.add_row("", "", "")

        table.add_row(str(idx), f"[{role_style}]{role}[/{role_style}]", formatted_content)

    console.print(table)

    # Print token counts if available
    if token_counts:
        console.print(f"\n[bold]Token Usage:[/bold]")
        console.print(f"  Input Tokens: {token_counts.get('input_tokens', 'N/A')}")
        console.print(f"  Output Tokens: {token_counts.get('output_tokens', 'N/A')}")
        console.print(f"  Total Tokens: {token_counts.get('total_tokens', 'N/A')}\n")


async def run(
    input_log: Optional[str] = None,
    is_ctf: bool = False,
    attack_rates: Optional[List[float]] = None,
    defense_rates: Optional[List[float]] = None,
    output_base_dir: Optional[str] = None,
    messages: Optional[List[Dict[str, Any]]] = None,
    token_counts: Optional[Dict[str, Any]] = None):
    """Main entry point for CTR experiment execution.

    CTR Experiment Runner
    This function is called from ``cai.repl.commands.ctr`` via ``asyncio.run()``.
    It orchestrates the entire CTR analysis pipeline:
    1. Accepts conversation data from CAI (either in-memory or from JSONL files), gen graph and stats
    2. Processes each log through the LLM-based graph extraction
    3. Runs the CTR core solver for game-theoretic analysis
    4. Generates visualizations and saves results

    Args:
        input_log: Path to JSONL file or directory containing logs
        is_ctf: Whether this is a CTF (Capture The Flag) analysis
        attack_rates: List of attack rates for game analysis (default [2])
        defense_rates: List of defense rates for game analysis (default [2])
        output_base_dir: Custom output directory (defaults to ~/.cai_cache/ctr/)
        messages: In-memory conversation history from active CAI session
    """
    # TIMING: Start total execution timer for performance tracking
    total_start_time = time.time()

    # GAME PARAMETERS: Set attack/defense rates for CTR game-theoretic analysis
    # These rates control the Nash equilibrium computation in the security game
    #
    # NOTE: We set the defense rate to half of the attack rate
    ATTACK_RATE = attack_rates if attack_rates is not None else [2]
    DEFENSE_RATE = defense_rates if defense_rates is not None else [1]

    if not input_log and not messages:
        raise ValueError("Error: either input_log or messages must be provided")

    input_log_path = input_log if input_log else "__in_memory__"
    # Create a fresh run directory per invocation
    main_run_dir = _create_run_dir(output_base_dir)
    jsonl_files = []

    if messages is not None:
        # Treat as single in-memory session
        jsonl_files = ["__in_memory__"]

        # # # NOTE: For debugging purposes - display messages in same format as /history
        # _debug_display_messages(messages, token_counts)
    else:
        if os.path.isfile(input_log_path) and input_log_path.endswith('.jsonl'):
            jsonl_files = [input_log_path]
        elif os.path.isdir(input_log_path):
            jsonl_files = sorted([f for f in glob.glob(os.path.join(input_log_path, "*.jsonl"))])
        else:
            raise ValueError(f"Error: {input_log_path} is not a .jsonl file or a valid directory")

    results = []

    total_tokens_all_logs = 0
    total_cost_all_logs = 0.0
    total_words_all_logs = 0
    total_number_messages = 0
    total_tokens_heuristic_all_logs =0

    # Track cumulative timing across all logs
    total_inference_time = 0.0
    total_inference_cost = 0.0
    total_ctr_time = 0.0
    total_plotting_time = 0.0

    ##############################################################################
    # 1. Process messages (or log files) and return attack graph with token stats
    ##############################################################################
    for log_file in jsonl_files:        
        print(f"---------------")
        print(f"PROCESSING LOG: {log_file}")
        # Check if we're processing in-memory messages
        using_in_memory = messages is not None and log_file == "__in_memory__"

        if using_in_memory:
            # Use the new function for in-memory processing
            result_dict = await process_in_memory_session(
                messages=messages,
                token_counts=token_counts,
                is_ctf=is_ctf,
                attack_rate_list=ATTACK_RATE,
                defense_rate_list=DEFENSE_RATE,
            )
            result_dict["log_file"] = log_file

            # Extract all required values for accumulation and downstream processing
            model_name_log = result_dict["model_name_log"]  # Extract model name for logging
            total_tokens_real = result_dict["total_tokens_real"]
            total_cost_real = result_dict["total_cost_real"]
            total_tokens_heuristic = result_dict["total_tokens_heuristic"]
            inference_time = result_dict["inference_execution_time"]
            inference_cost = result_dict["inference_cost"]
            filtered_log = result_dict["filtered_log"]  # Extract filtered_log for message count
            graph_structure_llm = result_dict["graph_structure_llm"]  # Extract for downstream plotting

            total_inference_time += inference_time
            total_inference_cost += inference_cost
        else:
            # Note, use the current model name instead
            # TODO: Process model_name from file-based log if needed
            #
            model_name = os.getenv('CAI_MODEL')
            model_name_log = model_name  # assign to model_name_log for logging
            
            # Get real tokens/cost from file
            (
                model_name,
                total_prompt_tokens,
                total_completion_tokens,
                total_cost_real,
                last_active_time,
                last_idle_time,
            ) = get_token_stats(log_file)
            total_tokens_real = total_prompt_tokens + total_completion_tokens
            real_values_source = "jsonl_stats"

            print(f"Total tokens (real) {total_tokens_real} [source={real_values_source}]")
            print(f"Total cost (real) {total_cost_real}")

            # # @vmayoral: this was terribly wrong, and implemented originally by @Li
            # # NOTE how the heuristic proposed using the TOTAL log tokens, not the messages tokens
            # #
            # from cai.ctr.probability_computation import get_log_tokens
            # total_tokens_heuristic = get_log_tokens(log_file)            

            # Get heuristic tokens from actual message content
            messages_from_log = parse_input_log(log_file)
            # Sum up the content_tokens from each message (already counted by parse_input_log)
            total_tokens_heuristic = sum(msg.get("content_tokens", 0) for msg in messages_from_log)

            # @vmayoral: not documented previously
            # 
            # Calculate cost-per-token rate using real costs but heuristic token counts
            # This creates a hybrid rate: actual cost divided by estimated tokens
            #
            # Inferred Rationale: We have accurate cost data from JSONL but use accessible heuristic 
            # token counting for consistency with other operations that lack real token data
            euro_per_token_heuristic = total_cost_real / total_tokens_heuristic if total_tokens_heuristic > 0 else 0.0

            print(f"Total tokens (heuristic) {total_tokens_heuristic}")
            print(f"Euro per token (heuristic) {euro_per_token_heuristic}")

            # LLM attack graph
            graph_structure, filtered_log, inference_time, inference_cost, instructions = await extract_attack_graph(
                input_log=log_file,
                is_ctf=is_ctf,
                attack_rate_list=ATTACK_RATE,
                defense_rate_list=DEFENSE_RATE,
                messages=None,
                total_tokens_override=None,
            )
            graph_structure_llm = graph_structure
            graph_structure = preprocess_graph(graph_structure)

            total_inference_time += inference_time
            total_inference_cost += inference_cost

            result_dict = {
                "log_file": log_file,
                "model_name_log": model_name,
                "graph_structure_llm": graph_structure,
                "filtered_log": filtered_log,
                "total_tokens_heuristic": total_tokens_heuristic,
                "total_tokens_real": total_tokens_real,
                "total_cost_real": total_cost_real,
                "euro_per_token_heuristic": euro_per_token_heuristic,
                "inference_execution_time": inference_time,
                "inference_cost": inference_cost,
                "system_prompt": instructions
            }

        results.append(result_dict)

        # Accumulate totals
        total_tokens_heuristic_all_logs += total_tokens_heuristic
        total_tokens_all_logs += total_tokens_real 
        total_cost_all_logs += total_cost_real 
        total_number_messages += len(filtered_log)
    
    ##############################################################################
    # 2. Compute edge probabilities for each log, plot them
    ##############################################################################
    save_dir = os.path.dirname(jsonl_files[0]) if jsonl_files else "."
    for idx, result in enumerate(results, 1):
        filtered_log = result["filtered_log"]
        graph_structure = result["graph_structure_llm"]
        estimated_tokens = result["total_tokens_heuristic"]
        estimated_cost = result["euro_per_token_heuristic"] * estimated_tokens 

        # Compute edge probabilities using global totals (for normalization across all logs)
        edge_probabilities = compute_edge_probabilities_offline(
            filtered_log=filtered_log,
            graph_structure=graph_structure,
            total_tokens= total_tokens_heuristic_all_logs,
            total_cost=total_cost_all_logs,
            total_number_messages= total_number_messages,
            w_cost=0.3,
            w_msg=0.3,
            w_tokens=0.4,
        )

        # Compute edge probabilities using only this log's stats (per-log normalization)
        edge_probabilities_individual = compute_edge_probabilities_offline(
            filtered_log=filtered_log,
            graph_structure=graph_structure,
            total_tokens=estimated_tokens,
            total_cost=estimated_cost,
            total_number_messages=len(filtered_log),
            w_cost=0.3,
            w_msg=0.3,
            w_tokens=0.4,
        )
        
        # Save original edge probabilities before postprocessing (for LLM graph plotting)
        edge_probabilities_llm = edge_probabilities.copy()
        edge_probabilities_individual_llm = edge_probabilities_individual.copy()
        
        graph_structure, edge_probabilities, edge_probabilities_individual = postprocess_graph(graph_structure, edge_probabilities, edge_probabilities_individual)

        result["graph_structure"] = graph_structure 
        result["edge_probabilities"] = edge_probabilities
        result["edge_probabilities_individual"] = edge_probabilities_individual

        # Create subfolder for each JSONL file if processing multiple files
        if len(jsonl_files) > 1:
            jsonl_filename = os.path.splitext(os.path.basename(result["log_file"]))[0]
            current_run_dir = os.path.join(main_run_dir, jsonl_filename)
            os.makedirs(current_run_dir, exist_ok=True)
        else:
            current_run_dir = main_run_dir

        # Create attack graph for global (if it is multilog) and individual probabilities
        node_vulnerabilities = {node.id: node.vulnerability for node in graph_structure.nodes}
        node_info_dict = {node.id: node.name for node in graph_structure.nodes}
        
        # Import the cleaned graph function
        from cai.ctr.attack_graph import create_cleaned_graph_for_visualization
        
        # Measure plotting time
        current_plotting_time = 0.0
        
        # Plot the original LLM graph structure (before preprocessing)
        node_vulnerabilities_llm = {node.id: node.vulnerability for node in graph_structure_llm.nodes}
        node_info_dict_llm = {node.id: node.name for node in graph_structure_llm.nodes}
        
        if len(results) == 1:
            # For single log, plot LLM graph with individual probabilities
            attack_graph_llm = create_graph_from_agent_output(graph_structure_llm, edge_probabilities_individual_llm)
            current_plotting_time += plot_attack_graph_with_timing(attack_graph_llm, current_run_dir, node_info_dict_llm, node_vulnerabilities_llm, type_graph="llm")
            
            # Plot regular graph with leaf_ nodes (for computation)
            attack_graph = create_graph_from_agent_output(graph_structure, edge_probabilities_individual)
            current_plotting_time += plot_attack_graph_with_timing(attack_graph, current_run_dir, node_info_dict, node_vulnerabilities, type_graph="individual")
            
            # Plot cleaned graph without leaf_ nodes (for visualization)
            cleaned_graph_structure, cleaned_edge_probabilities = create_cleaned_graph_for_visualization(graph_structure, edge_probabilities_individual)
            cleaned_node_vulnerabilities = {node.id: node.vulnerability for node in cleaned_graph_structure.nodes}
            cleaned_node_info_dict = {node.id: node.name for node in cleaned_graph_structure.nodes}
            cleaned_attack_graph = create_graph_from_agent_output(cleaned_graph_structure, cleaned_edge_probabilities)
            current_plotting_time += plot_attack_graph_with_timing(cleaned_attack_graph, current_run_dir, cleaned_node_info_dict, cleaned_node_vulnerabilities, type_graph="individual_cleaned")
        else:
            # For multi-log, plot LLM graph with both global and individual probabilities
            for probs, probs_llm, graph_name in [(edge_probabilities, edge_probabilities_llm, "global"), (edge_probabilities_individual, edge_probabilities_individual_llm, "individual")]:
                # Plot LLM graph
                attack_graph_llm = create_graph_from_agent_output(graph_structure_llm, probs_llm)
                current_plotting_time += plot_attack_graph_with_timing(attack_graph_llm, current_run_dir, node_info_dict_llm, node_vulnerabilities_llm, type_graph=f"llm_{graph_name}")
                
                # Plot regular graph with leaf_ nodes (for computation)  
                attack_graph = create_graph_from_agent_output(graph_structure, probs)
                current_plotting_time += plot_attack_graph_with_timing(attack_graph, current_run_dir, node_info_dict, node_vulnerabilities, type_graph=graph_name)
                
                # Plot cleaned graph without leaf_ nodes (for visualization)
                cleaned_graph_structure, cleaned_edge_probabilities = create_cleaned_graph_for_visualization(graph_structure, probs)
                cleaned_node_vulnerabilities = {node.id: node.vulnerability for node in cleaned_graph_structure.nodes}
                cleaned_node_info_dict = {node.id: node.name for node in cleaned_graph_structure.nodes}
                cleaned_attack_graph = create_graph_from_agent_output(cleaned_graph_structure, cleaned_edge_probabilities)
                current_plotting_time += plot_attack_graph_with_timing(cleaned_attack_graph, current_run_dir, cleaned_node_info_dict, cleaned_node_vulnerabilities, type_graph=f"{graph_name}_cleaned")
        
        total_plotting_time += current_plotting_time
        
        # Run CTR baseline analysis
        baseline_result, ctr_time = run_ctr_baseline_analysis(graph_structure, edge_probabilities_individual, current_run_dir, ATTACK_RATE, DEFENSE_RATE)
        result["baseline_result"] = baseline_result
        result["ctr_time"] = ctr_time # Store CTR time
        result["plotting_time"] = current_plotting_time # Store plotting time for multi-log accumulation
        total_ctr_time += ctr_time

        # Calculate total time up to this point for this specific log
        current_total_time = time.time() - total_start_time

        # Store the individual log's total execution time for multi-log accumulation
        result["total_execution_time"] = current_total_time

        # Update save_run_information with real timing values
        save_run_information(
            model_name_log=model_name_log,
            input_log=result["log_file"], 
            inference_time=result["inference_execution_time"],
            inference_cost=result["inference_cost"],
            ctr_time=ctr_time,
            plotting_time=current_plotting_time,
            total_time=current_total_time,
            graph_structure_output=graph_structure_llm,
            run_dir=current_run_dir,
            message=filtered_log,
            probabilities=edge_probabilities,
            system_prompt=result["system_prompt"]
        )

    # If there are multiple logs, unify the graphs and plot the merged graph
    if len(results) > 1:
        multi_log_graphs, multi_log_ctr_time, multi_log_plotting_time = create_multi_log_graphs(results, total_tokens_all_logs, total_cost_all_logs, total_number_messages, main_run_dir, ATTACK_RATE, DEFENSE_RATE)
        total_ctr_time += multi_log_ctr_time
        total_plotting_time += multi_log_plotting_time
    
    # Calculate final total execution time
    total_end_time = time.time()
    final_total_time = total_end_time - total_start_time
    
    print(f"\n" + "="*50)
    print(f"TIMING SUMMARY")
    print(f"="*50)
    print(f"Total Inference time: {total_inference_time:.4f} seconds")
    print(f"Total Inference Cost: ${total_inference_cost:.6f}")
    print(f"Total CTR Baseline execution time: {total_ctr_time:.4f} seconds")
    print(f"Total Plotting time: {total_plotting_time:.4f} seconds")
    print(f"Total Execution time: {final_total_time:.4f} seconds")
    print(f"="*50)
    
    print(f" Results saved in: {main_run_dir}")
    return {"run_dir": main_run_dir}

async def main():
    parser = argparse.ArgumentParser(description='Convert JSONL conversation logs to graph structures')
    parser.add_argument('--input_log', required=False, help='Path to input JSONL log file or a folder with JSONL log')
    parser.add_argument('--is_ctf', action='store_true', help='Whether this is a CTF challenge')
    parser.add_argument('--attack_rate', type=str, help='Comma-separated list of attack rates (e.g., "1,2,3")')
    parser.add_argument('--defense_rate', type=str, help='Comma-separated list of defense rates (e.g., "0,1,2")')
    parser.add_argument('--output_dir', type=str, help='Base directory for CTR outputs (overrides env)')
    args = parser.parse_args()

    if args.input_log is None:
        parser.error("Error: --input_log [file.jsonl] must be specified")
        return

    attack_rates = [float(rate) for rate in args.attack_rate.split(',')] if args.attack_rate else None
    defense_rates = [float(rate) for rate in args.defense_rate.split(',')] if args.defense_rate else None

    await run(
        input_log=args.input_log,
        is_ctf=bool(args.is_ctf),
        attack_rates=attack_rates,
        defense_rates=defense_rates,
        output_base_dir=args.output_dir,
    )

if __name__ == "__main__":
    asyncio.run(main())  
