"""
Attack Graph Construction Utilities - CAI-CTR Graph Processing Engine

This module provides comprehensive utilities for constructing, processing, and visualizing
attack graphs within the CAI-CTR integration. It handles the transformation of LLM-generated
graph structures into NetworkX objects suitable for game-theoretic analysis, along with
advanced graph preprocessing, multi-log correlation, and visualization capabilities.

CORE FUNCTIONALITY:
==================
1. Graph Construction: Transform LLM outputs into NetworkX directed graphs
   - Convert Pydantic GraphStructure objects to NetworkX DiGraph objects
   - Preserve node attributes (vulnerability status, message IDs, descriptions)
   - Handle edge probability assignments and validation

2. Graph Preprocessing: Prepare graphs for CTR analysis requirements
   - Add artificial leaf nodes for vulnerable nodes (CTR compliance)
   - Remove non-vulnerable leaf nodes (game theory requirements)
   - Handle disconnected components and ensure graph validity
   - Clean visualization versions without artificial nodes

3. Multi-Log Graph Correlation: Advanced graph merging for comparative analysis
   - Maxi Graph Mode: Preserve all nodes/edges with unique prefixes per log
   - Simplified Graph Mode: Represent each log as single node with vulnerability status
   - Handle probability normalization across multiple logs
   - Support log-to-log correlation analysis

4. Advanced Visualization: Multi-format graph plotting with probability overlays
   - Color-coded nodes by vulnerability and role (entry, target, intermediate)
   - Edge thickness and labels based on exploitation probabilities
   - Multi-log visualization with log grouping and boundary indicators
   - Probability range visualization for simplified graphs
   - Export to high-resolution PNG with customizable layouts

GRAPH PREPROCESSING TRANSFORMATIONS:
===================================
LLM Raw Graph → Individual Graph → Individual Cleaned Graph
- Raw: Direct LLM output, may have cycles or structural issues
- Individual: Adds artificial leaf nodes, removes problematic leaf nodes
- Cleaned: Removes artificial nodes for visualization, maintains analysis validity

MULTI-LOG PROCESSING MODES:
===========================
1. Maxi Graph: Full preservation approach
   - Each log's nodes get unique prefixes (log1_node, log2_node)
   - All attack paths and vulnerabilities preserved
   - Central "Initial" node connects to each log's starting node
   - Suitable for comprehensive multi-target attack analysis

2. Simplified Graph: High-level correlation approach
   - Each log becomes single node with vulnerability boolean
   - Probability edges from central node to each log
   - Normalized probabilities only among vulnerable logs
   - Suitable for log comparison and correlation studies

VISUALIZATION CAPABILITIES:
==========================
- Node Color Coding: Gray (start), Blue (intermediate), Green (vulnerable)
- Edge Visualization: Thickness proportional to probability, labeled percentages
- Multi-log Layouts: Circular grouping with log boundaries and labels
- Artificial Edge Highlighting: Orange dashed lines for preprocessing additions
- High-resolution output: 300 DPI PNG for publication quality

INTEGRATION INTERFACES:
======================
- NetworkX compatibility for graph algorithms and analysis
- CTR Core integration for preprocessing requirements
- Probability computation engine integration for edge weighting
- Matplotlib/NetworkX visualization pipeline
- Multi-log experimental framework support
"""

import networkx as nx
# Ensure non-interactive backend for plotting (safe in background threads)
import matplotlib  # noqa: E402
try:
    matplotlib.use("Agg", force=True)  # type: ignore[attr-defined]
except Exception:
    pass
import matplotlib.pyplot as plt
import numpy as np
import re
from networkx.drawing.nx_pydot import graphviz_layout
from types import SimpleNamespace

from copy import deepcopy
from types import SimpleNamespace

# Global default weight value for edges (used if not specified)
DEFAULT_WEIGHT_VALUE = 0

def cai_set_default_weight(value):
    """
    Set the global default weight value for edges.

    Args:
        value (float): The value to set as the default edge weight.
    """
    global DEFAULT_WEIGHT_VALUE
    DEFAULT_WEIGHT_VALUE = value

def add_edge_weights(G):
    """
    Add 'weight' attribute to each edge in the graph as -log(probability).
    Args:
        G (networkx.Graph or networkx.DiGraph): The graph whose edges will be updated.

    Returns:
        networkx.Graph or networkx.DiGraph: The updated graph with 'weight' attributes on edges.
    """
    for u, v in G.edges():
        p = G[u][v]['prob']
        w = -np.log(p)
        if abs(w) < 1e-14:
            w = 0.0
        G[u][v]['weight'] = w
    return G

def create_graph_from_agent_output(graph_structure, probabilities):
    """
    Graph Construction: Transform LLM output into NetworkX directed graph.
    
    Converts structured LLM agent output (Pydantic GraphStructure) into a NetworkX
    DiGraph object suitable for CTR analysis and visualization. This is the primary
    interface between the natural language processing pipeline and the mathematical
    graph analysis framework.
    
    CONVERSION PROCESS:
    1. Node Creation: Extract node information preserving all attributes
       - ID mapping for graph connectivity
       - Vulnerability status for target identification  
       - Message IDs for temporal analysis
       - Descriptive information for visualization labels
    
    2. Edge Creation: Build directed edges with probability attributes
       - Source-target connectivity from agent output
       - Probability assignment from computation engine
       - Artificial edge marking for preprocessing tracking
       - Validation of edge-node consistency
    
    3. Attribute Preservation: Maintain all metadata for downstream analysis
       - Node vulnerability status (critical for game formulation)
       - Message temporal ordering (for probability calculation)
       - Descriptive text (for human-readable outputs)
       - Probability scores (for strategic analysis)
    
    INTEGRATION WITH CTR PIPELINE:
    - Output compatible with CTR core preprocessing functions
    - Preserves probability assignments for payoff matrix construction
    - Maintains node attributes required for defender/attacker strategy computation
    - Supports visualization pipeline with color coding and labels
    
    Args:
        graph_structure: Pydantic GraphStructure object from LLM agent containing:
            - nodes: List[NodeInfo] with id, name, info, vulnerability, message_id
            - edges: List[EdgeInfo] with source, target node IDs
        probabilities (dict): Edge probability mapping {source->target: float}
                             Probabilities typically in [0.0, 1.0] representing
                             exploitation likelihood or strategic importance

    Returns:
        networkx.DiGraph: Directed graph ready for CTR analysis with:
            - Node attributes: name, info, vulnerability, message_id
            - Edge attributes: prob (probability), is_artificial (preprocessing flag)
            - Full connectivity as specified by input structure
            
    Raises:
        ValueError: If edge references non-existent nodes (structural inconsistency)
        KeyError: If required node attributes are missing from input structure
        TypeError: If input structure doesn't match expected schema
        
    Example:
        >>> nodes = [NodeInfo(id="1", name="Entry", vulnerability=False, message_id=0)]
        >>> edges = [EdgeInfo(source="1", target="2")]
        >>> structure = GraphStructure(nodes=nodes, edges=edges) 
        >>> probs = {"1->2": 0.75}
        >>> graph = create_graph_from_agent_output(structure, probs)
        >>> assert graph.has_edge("1", "2")
        >>> assert graph["1"]["2"]["prob"] == 0.75
    """
    # Create directed graph
    graph = nx.DiGraph()
    
    # Add nodes to the graph
    for node in graph_structure.nodes:
        graph.add_node(
            node.id, 
            name=node.name,
            info=node.info,
            vulnerability=node.vulnerability,
            message_id=node.message_id
        )
    
    # Add edges to the graph
    edges = [(edge.source, edge.target) for edge in graph_structure.edges]
    graph.add_edges_from(edges)
    
    # Store probabilities/scores as edge attributes
    for edge in graph_structure.edges:
        u, v = edge.source, edge.target
        if graph.has_edge(u, v):
            edge_key = f"{u}->{v}"
            prob = probabilities.get(edge_key, 0.0)  
            graph[u][v]['prob'] = float(prob)  
            # Mark artificial edges for visual distinction
            graph[u][v]['is_artificial'] = probabilities.get(f"artificial_{edge_key}", False)
        else:
            raise ValueError(f"Edge ({u}->{v}) not found in the graph. Check definitions.")
    
    return graph

def get_edge_probability(graph, u, v, default=0.0): # REMINDER: We have used the term "probability" for the scores of the edges
    """
    Safely retrieve the probability of an edge in the graph.

    Args:
        graph (networkx.Graph or networkx.DiGraph): The graph containing the edge.
        u: Source node identifier.
        v: Target node identifier.
        default (float, optional): Value to return if the edge or probability is missing. Defaults to 0.0.

    Returns:
        float: The probability value for the edge, or the default if not found.
    """
    try:
        return float(graph[u][v].get('prob', default))
    except (KeyError, ValueError, TypeError):
        return default

def connect_disconnected_starting_nodes(graph_structure, filtered_log, total_tokens, total_cost, total_number_messages):
    """
    Connect disconnected starting nodes to the real starting node (lowest message_id).
    
    Args:
        graph_structure: Graph structure with nodes and edges
        filtered_log: Log messages for probability computation
        total_tokens: Total tokens for probability computation  
        total_cost: Total cost for probability computation
        total_number_messages: Total message count for probability computation
        
    Returns:
        modified_graph_structure: Graph structure with artificial edges added
    """
    # The only allowed starting node is the one with id '1'.
    # Join all other starting nodes to node '1' with artificial edges.
    # If there are any edges with target '1', remove them (disconnect them).

  

    # Remove edges that have target '1'
    filtered_edges = [edge for edge in graph_structure.edges if edge.target != '1']

    # Find nodes with no incoming edges (starting nodes)
    incoming_edges = {edge.target for edge in filtered_edges}
    starting_nodes = [node for node in graph_structure.nodes if node.id not in incoming_edges]

    # Only node '1' is allowed to be a starting node
    # The start id should be the minimum id among all node ids (as string)
    allowed_start_id = min((node.id for node in graph_structure.nodes), key=lambda x: int(x))
    disconnected_nodes = [node for node in starting_nodes if node.id != allowed_start_id]

    # Add artificial edges from node '1' to all other starting nodes
    new_edges = []
    for disc_node in disconnected_nodes:
        new_edges.append(SimpleNamespace(source=allowed_start_id, target=disc_node.id))

    # Create new graph structure
    new_graph_structure = deepcopy(graph_structure)
    new_graph_structure.edges = filtered_edges + new_edges

    return new_graph_structure

def create_cleaned_graph_for_visualization(graph_structure, edge_probabilities):
    """
    Create a cleaned version of the graph for visualization by removing artificial leaf_ nodes
    and connecting edges directly to the original vulnerable nodes.
    
    Args:
        graph_structure: Original graph structure with leaf_ nodes
        edge_probabilities: Edge probabilities dictionary
        
    Returns:
        tuple: (cleaned_graph_structure, cleaned_edge_probabilities)
    """
    # Check if there are any leaf_ nodes at all
    has_leaf_nodes = any(node.id.startswith('leaf_') for node in graph_structure.nodes)
    
    if not has_leaf_nodes:
        # No leaf nodes to clean, return the original graph
        return graph_structure, edge_probabilities
    
    # Import at module level to avoid circular imports
    try:
        from .experiment import NodeInfo, EdgeInfo, GraphStructure
    except ImportError:
        from experiment import NodeInfo, EdgeInfo, GraphStructure
    
    # Create cleaned nodes list (exclude leaf_ nodes) - create new objects for Pydantic
    cleaned_nodes = []
    for node in graph_structure.nodes:
        if not node.id.startswith('leaf_'):
            cleaned_nodes.append(NodeInfo(
                id=node.id,
                name=node.name,
                info=node.info,
                vulnerability=node.vulnerability,
                message_id=node.message_id
            ))
    
    # Create cleaned edges list
    cleaned_edges = []
    cleaned_probabilities = {}
    
    for edge in graph_structure.edges:
        source, target = edge.source, edge.target
        edge_key = f"{source}->{target}"
        prob = edge_probabilities.get(edge_key, 0.0)
        
        # Skip edges to leaf_ nodes - they're artificial
        if target.startswith('leaf_'):
            continue
            
        # Keep all other edges as they are - create new EdgeInfo object
        cleaned_edges.append(EdgeInfo(source=source, target=target))
        cleaned_probabilities[edge_key] = prob
    
    return GraphStructure(nodes=cleaned_nodes, edges=cleaned_edges), cleaned_probabilities

def plot_attack_graph(attack_graph, save_path, node_info_dict, node_vulnerabilities, type_graph="", log_group_labels=None, probability_ranges=None):
    """
    Plot the attack graph with edge colors and labels based on probability of exploitation.
    Args:
        attack_graph (networkx.DiGraph): The attack graph to plot.
        save_path (str): Directory path where the plot image will be saved.
        node_info_dict (dict): Mapping from node id to node name/info for labeling.
        node_vulnerabilities (dict): Mapping from node id to boolean indicating vulnerability.
        type_graph (str, optional): Type of graph layout. Defaults to "". E.g. "maxi" or "simplified".
        log_group_labels (dict, optional): Mapping from log ID to log name for labeling. Defaults to None.

    Returns:
        None. The plot is saved as 'attack_graph.png' in the specified directory.
    """
    import matplotlib.patches as mpatches

    img_path = f'{save_path}/attack_graph_{type_graph}.png'

    # Use all nodes for plotting
    nodes_to_plot = list(attack_graph.nodes)
    subgraph = attack_graph.subgraph(nodes_to_plot)

    # Set up figure size and layout for clarity
    if type_graph == "maxi":
        plt.figure(figsize=(20, 16))
        log_groups = {}
        initial_nodes = []

        # Group nodes by log ID
        for node in subgraph.nodes:
            match = re.match(r"(log\d+)_", str(node))
            if match:
                log_id = match.group(1)
                log_groups.setdefault(log_id, []).append(node)
            else:
                initial_nodes.append(node)

        # Calculate positions for each log group in a circular arrangement
        num_logs = len(log_groups)
        radius = 7  # Increased radius to spread groups further apart
        pos = {}

        # Position initial nodes in center
        if initial_nodes:
            for i, node in enumerate(initial_nodes):
                pos[node] = np.array([0, 0])

        # Position each log group in its own sector around the circle
        group_centers = {}
        for i, (log_id, nodes) in enumerate(sorted(log_groups.items())):
            # Calculate angle for this log group
            theta = (2 * np.pi * i) / num_logs

            # Calculate center point for this log group
            group_center_x = radius * np.cos(theta)
            group_center_y = radius * np.sin(theta)
            group_centers[log_id] = (group_center_x, group_center_y)

            # Position nodes in a wider mini-circle within their sector
            num_nodes = len(nodes)
            inner_radius = 3.5  # Increased inner radius to spread nodes within groups

            for j, node in enumerate(nodes):
                # Calculate angle for this node within its group
                inner_theta = (2 * np.pi * j) / num_nodes

                # Position relative to group center
                node_x = group_center_x + inner_radius * np.cos(inner_theta)
                node_y = group_center_y + inner_radius * np.sin(inner_theta)

                pos[node] = np.array([node_x, node_y])

        # --- Draw log group rectangles and labels ---
        ax = plt.gca()
        for log_id, nodes in log_groups.items():
            # Get positions of all nodes in this group
            group_positions = np.array([pos[n] for n in nodes])
            if group_positions.shape[0] == 0:
                continue
            min_x, min_y = group_positions.min(axis=0)
            max_x, max_y = group_positions.max(axis=0)
            # Adjust padding to prevent overlap while keeping groups compact
            pad_x = 0.9
            pad_y = 0.9
            rect_x = min_x - pad_x
            rect_y = min_y - pad_y
            rect_w = (max_x - min_x) + 2 * pad_x
            rect_h = (max_y - min_y) + 2 * pad_y
            # Draw rectangle
            rect = mpatches.FancyBboxPatch(
                (rect_x, rect_y), rect_w, rect_h,
                boxstyle="round,pad=0.11",  # Slightly more round padding
                linewidth=2, edgecolor="#B2B2B2", facecolor=(0.95, 0.95, 0.98, 0.13), zorder=0
            )
            ax.add_patch(rect)
            # Draw log group label just above the rectangle, with a small vertical offset
            label_x = (min_x + max_x) / 2
            label_y = max_y + 0.28  # Slightly more offset above the top

            # Get the name of the first node in this log group (e.g., log1_1), using node_info_dict
            if log_group_labels and log_id in log_group_labels:
                label_name = log_group_labels[log_id]
            elif nodes:
                first_node = sorted(nodes, key=lambda n: str(n))[0]
                label_name = node_info_dict.get(first_node, str(first_node))
            else:
                label_name = log_id  # fallback

            ax.text(
                label_x, label_y, label_name,
                fontsize=13, fontweight='bold', color="#444488",
                ha='center', va='bottom',
                bbox=dict(boxstyle='round,pad=0.13', fc='white', ec='none', alpha=0.85)
            )
    else:
        plt.figure(figsize=(12, 8))
        # Improved layout parameters for better edge visibility
        pos = nx.spring_layout(subgraph, k=3.0, iterations=100, seed=42)

    # Identify edges with a 'prob' attribute
    edges_with_prob = [(u, v) for u, v in subgraph.edges() if 'prob' in subgraph[u][v]]
    
    # Separate artificial and regular edges
    artificial_edges = [(u, v) for u, v in edges_with_prob if subgraph[u][v].get('is_artificial', False)]
    regular_edges = [(u, v) for u, v in edges_with_prob if not subgraph[u][v].get('is_artificial', False)]
    
    arrow_color = '#173C47'
    artificial_arrow_color = '#FF8C00'  # Orange for artificial edges

    # Draw edges
    if edges_with_prob:
        edge_labels = {}
        
        # Draw regular edges
        if regular_edges:
            edge_widths = []
            for u, v in regular_edges:
                prob = subgraph[u][v]['prob']
                
                # Check if probability ranges are available and this is simplified graph
                edge_key = f"{u}->{v}"
                if probability_ranges and edge_key in probability_ranges and type_graph == "simplified":
                    first_prob, last_prob = probability_ranges[edge_key]
                    if first_prob == last_prob:
                        edge_labels[(u, v)] = f"{first_prob:.2%}"
                    else:
                        edge_labels[(u, v)] = f"{first_prob:.2%} - {last_prob:.2%}"
                else:
                    edge_labels[(u, v)] = f"{prob:.2%}"
                    
                width = 1 + 7 * prob  # Thicker for higher probability
                edge_widths.append(width)
            
            nx.draw_networkx_edges(
                subgraph, pos,
                edgelist=regular_edges,
                edge_color=arrow_color,
                arrows=True,
                arrowsize=35,
                width=[w*0.7 for w in edge_widths],
                connectionstyle='arc3,rad=0.1',
                min_target_margin=15
            )
        
        # Draw artificial edges in orange
        if artificial_edges:
            edge_widths_artificial = []
            for u, v in artificial_edges:
                prob = subgraph[u][v]['prob']
                edge_labels[(u, v)] = f"{prob:.2%}"  # Remove (artificial) text - orange color is sufficient
                width = 1 + 7 * prob  # Thicker for higher probability
                edge_widths_artificial.append(width)
            
            nx.draw_networkx_edges(
                subgraph, pos,
                edgelist=artificial_edges,
                edge_color=artificial_arrow_color,
                arrows=True,
                arrowsize=35,
                width=[w*0.7 for w in edge_widths_artificial],
                connectionstyle='arc3,rad=0.2',  # Different curvature to prevent overlap
                min_target_margin=15,
                style='dashed'  # Dashed style for artificial edges
            )
        
        ################# START OF MITIGATING OVERLAP of elements IN GRAPHS #################
        # Draw edge labels with varied positions to avoid confusion
        # Group edges by source node to handle multiple outgoing edges better
        edges_by_source = {}
        for u, v in edges_with_prob:
            if u not in edges_by_source:
                edges_by_source[u] = []
            edges_by_source[u].append((u, v))
        
        for source_node, source_edges in edges_by_source.items():
            if len(source_edges) == 1:
                # Single edge from this source - use default position
                u, v = source_edges[0]
                nx.draw_networkx_edge_labels(
                    subgraph, pos,
                    edge_labels={(u, v): edge_labels[(u, v)]},
                    font_size=9,
                    label_pos=0.5,  # Center position on edge
                    rotate=False,
                    bbox=dict(boxstyle='round,pad=0.2', fc='white', ec='gray', alpha=0.9, linewidth=1),
                    horizontalalignment='center',
                    verticalalignment='center'
                )
            else:
                # Multiple edges from same source - spread them out more distinctly
                for i, (u, v) in enumerate(source_edges):
                    # For multiple edges from same source, use more spread out positions
                    if len(source_edges) == 2:
                        label_positions = [0.3, 0.7]  # More spread out
                    elif len(source_edges) == 3:
                        label_positions = [0.25, 0.5, 0.75]  # Better spacing
                    else:
                        # For 4+ edges, space them more evenly
                        label_positions = [0.2, 0.4, 0.6, 0.8, 0.15, 0.85]
                    
                    label_pos = label_positions[i % len(label_positions)]
                    
                    nx.draw_networkx_edge_labels(
                        subgraph, pos,
                        edge_labels={(u, v): edge_labels[(u, v)]},
                        font_size=9,
                        label_pos=label_pos,
                        rotate=False,
                        bbox=dict(boxstyle='round,pad=0.2', fc='white', ec='gray', alpha=0.9, linewidth=1),
                        horizontalalignment='center',
                        verticalalignment='center'
                    )
        ######### END OF MITIGATING OVERLAP of elements IN GRAPHS #########
        
    # If no edges with prob, draw all edges as normal
    if not edges_with_prob:
        nx.draw_networkx_edges(
            subgraph, pos,
            edge_color=arrow_color,
            arrows=True,
            arrowsize=35,
            width=2,
            connectionstyle='arc3,rad=0.1',
            min_target_margin=15
        )

    # Assign node colors based on vulnerability and special node types
    node_colors = []

    # Define the "initial" node
    # Determine possible initial nodes: "Initial", "0", or "1"
    if "Initial" in map(str, subgraph.nodes()):
        initial_node = "Initial"
    elif "0" in map(str, subgraph.nodes()):
        initial_node = "0"
    else:
        initial_node = "1"

    for node in subgraph.nodes():
        node_str = str(node)
        if node_str == initial_node:
            node_colors.append('#E3E5E6')  # Gray for starting node
        elif node_vulnerabilities.get(node_str, False):
            node_colors.append('#00BCA2') # Green for vulnerable
        else:
            node_colors.append('#B2D8D8')  # Light blue for non-vulnerable

    # Draw nodes
    nx.draw_networkx_nodes(
        subgraph, pos,
        node_color=node_colors,
        node_size=1000,
        edgecolors=node_colors,
        linewidths=2
    )

    # Draw node labels: main label (name/info), and a transparent ID overlay
    for node in subgraph.nodes():
        x, y = pos[node]
        node_str = str(node)
        # Main label: name/info (without ID)
        main_label = f"{node_info_dict.get(node, str(node))}"
        plt.text(
            x, y + 0.12,  # Moved further up to avoid edge labels
            main_label,
            fontsize=9,
            fontweight='bold',
            ha='center',
            va='bottom',
            bbox=dict(boxstyle='round,pad=0.2', fc='white', ec='none', alpha=0.85)
        )
        # Node ID: always shown, less prominent, more transparent
        plt.text(
            x, y - 0.18,  # Moved further down to avoid edge labels
            f"ID:{node}",
            fontsize=9,
            fontweight='normal',
            ha='center',
            va='top',
            color=(0.2, 0.2, 0.2, 0.4),
            bbox=dict(boxstyle='round,pad=0.1', fc=(1,1,1,0.0), ec='none', alpha=0.0)
        )

    # Add title and legend
    plt.title("Attack Graph", pad=20, fontsize=14, fontweight='bold')
    legend_elements = [
        plt.Line2D([0], [0], marker='o', color='w', label='Starting node', markerfacecolor='#E3E5E6', markersize=10),
        plt.Line2D([0], [0], marker='o', color='w', label='Non vulnerable node', markerfacecolor='#B2D8D8', markersize=10),
        plt.Line2D([0], [0], marker='o', color='w', label='Vulnerable node', markerfacecolor='#00BCA2', markersize=10),
    ]
    # Add legend for attack path (orange line) and artificial (dashed) if present
    if artificial_edges:
        legend_elements.append(
            plt.Line2D([0], [0], color='orange', lw=2, linestyle='--', label='Multiple starting nodes')
        )
    plt.legend(handles=legend_elements, loc='center left', bbox_to_anchor=(1, 0.5))
    plt.axis('off')
    plt.tight_layout()
    plt.savefig(img_path, dpi=300, bbox_inches='tight')
    plt.close()
