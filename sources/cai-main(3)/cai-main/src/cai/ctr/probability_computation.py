"""
Edge Probability Computation Engine - Multi-Factor Attack Path Analysis

This module implements the core probability computation engine for CAI-CTR integration,
calculating edge exploitation probabilities based on conversation analysis metrics.
It provides the mathematical foundation that transforms conversational penetration 
testing logs into quantitative attack path probabilities for game-theoretic analysis.

PROBABILITY MODEL:
==================
Offline Mode Formula:
P_offline,i = W_cost × S_cost_normalized + W_msg × S_msg_normalized + W_tokens × S_tokens_normalized

Where default weights are: W_cost=0.3, W_msg=0.3, W_tokens=0.4

SCORING COMPONENTS:
==================
1. Cost Score (S_cost): Economic efficiency of attack path
   - Based on actual token costs from LLM API usage
   - Reflects real-world resource consumption during penetration testing
   - Normalized: S_cost_normalized = 1.0 - (S_cost_i / total_cost)
   - Assumption: Lower cost paths are more likely to be exploited

2. Message Distance Score (S_msg): Temporal proximity in conversation
   - Distance between source and target nodes in conversation timeline
   - For vulnerable targets: direct message distance + 1
   - For non-vulnerable: distance to closest vulnerable node via target
   - Normalized: S_msg_normalized = 1.0 - ((S_msg_i - 1) / (total_messages - 1))
   - Assumption: Closer conversation elements indicate stronger relationships

3. Token Score (S_tokens): Information content along attack path
   - Sum of tokens consumed between source and target nodes
   - Estimated from conversation message token counts
   - Normalized: S_tokens_normalized = 1.0 - (token_estimate_i / total_tokens)
   - Assumption: Lower token consumption indicates more direct/efficient paths

NORMALIZATION MODES:
===================
1. Global Normalization: Across all logs in multi-log analysis
   - Uses total tokens/cost/messages from all processed logs
   - Enables comparison between different penetration testing sessions
   - Suitable for correlation analysis and dataset-wide insights

2. Individual Normalization: Per-log normalization
   - Uses only current log's tokens/cost/messages for normalization
   - Provides log-specific relative probabilities
   - Suitable for single-log analysis and log-internal strategy optimization

ADAPTIVE WEIGHT HANDLING:
========================
- Automatic cost weight redistribution when cost data unavailable
- Proportional reallocation to message and token weights
- Maintains mathematical consistency across different log types
- Handles missing cost information gracefully (free models, cached results)

VULNERABILITY TARGETING:
=======================
- Direct vulnerable target: Uses direct path metrics
- Non-vulnerable intermediate: Calculates path through to closest vulnerable node
- Supports multi-hop attack path analysis
- Handles complex attack graphs with multiple vulnerability points

INTEGRATION INTERFACES:
======================
- Token counting via litellm integration for consistency with CAI cost tracking
- Support for various LLM model token counting (Qwen, GPT, Claude)
- Compatible with CAI JSONL log format and message structure
- Integrates with graph preprocessing and CTR analysis pipeline
"""
import os
from dotenv import load_dotenv
import litellm
import json
import math
from rich.table import Table
from rich.console import Console

# Load .env from current directory only, not from parent directories
dotenv_path = os.path.join(os.getcwd(), '.env')
load_dotenv(dotenv_path=dotenv_path, verbose=False)

# Set default for OPENAI_API_KEY if not already set
if "OPENAI_API_KEY" not in os.environ:
    os.environ["OPENAI_API_KEY"] = ""

def get_log_tokens(log_file):
    """
    Calculate the total number of tokens in a JSONL using litellm.token_counter
    
    Args:
        log_file (str): Path to the JSONL log file.

    Returns:
        int: The total number of tokens (input + output) in the log file.
    """
    input_tokens = 0
    output_tokens = 0
    model = "qwen:Qwen/Qwen1.5-0.5B-Chat"  # Default model name for tokenizer
   
    with open(log_file, "r", encoding="utf-8") as f:
        for line in f:
            line = line.lstrip()
            # Lines containing model or id are logs API interactions
            if line.startswith('{"model":'):
                try:
                    all_input_text = line
                    input_tokens += litellm.token_counter(model=model, text=all_input_text)
                except Exception:
                    input_tokens += 0
            elif line.startswith('{"id":'):
                try:
                    output_text = line
                    output_tokens += litellm.token_counter(model=model, text=output_text)
                except Exception:
                    output_tokens += 0
    
    total_tokens = input_tokens + output_tokens
    
    return total_tokens


def compute_edge_probabilities_offline(
    filtered_log,
    graph_structure,
    total_tokens,
    total_cost,
    total_number_messages,
    w_cost=0.3,
    w_msg=0.1,
    w_tokens=0.6,
    probability_display_mode: str = "both",
    probability_output_mode: str = "logistic",
    # probability_output_mode: str = "weighted",
):
    """
    Multi-Factor Edge Probability Computation: Calculate attack path exploitation probabilities.
    
    Implements the core probability computation algorithm that transforms conversation
    analysis metrics into quantitative edge probabilities for game-theoretic security
    analysis. Uses a weighted combination of cost efficiency, temporal proximity, and
    information content to model attack path likelihood.
    
    ALGORITHM OVERVIEW:
    1. Weight Validation & Adaptation: Ensure weights sum to 1.0, redistribute if cost unavailable
    2. Vulnerability Mapping: Identify and sort vulnerable nodes by conversation timeline  
    3. Edge Processing: For each edge, compute multi-factor probability score
    4. Path Analysis: Handle direct vulnerable targets vs. multi-hop paths through intermediates
    5. Normalization: Apply global or individual normalization based on input totals
    
    SCORING METHODOLOGY:
    - Cost Component: Reflects economic efficiency - lower cost = higher probability
    - Message Component: Reflects temporal proximity - closer messages = higher probability  
    - Token Component: Reflects information content - lower tokens = more direct = higher probability
    - Final Score: Weighted linear combination ensuring probabilistic interpretation
    
    VULNERABILITY HANDLING:
    - Direct Vulnerable Target: Path ends at vulnerable node, uses direct metrics
    - Intermediate Target: Path continues to closest vulnerable node, uses extended metrics
    - Multi-Vulnerable Scenarios: Finds optimal vulnerable target based on total path distance
    
    NORMALIZATION MODES:
    - Global: total_* parameters represent multi-log aggregates for cross-log comparison
    - Individual: total_* parameters represent single log for internal relative analysis
    
    Args:
        filtered_log (list): Chronologically ordered conversation messages, each containing:
            - message_id (int): Sequential message identifier (0-indexed)
            - content_tokens (int): Token count for this message
            - role (str): Message role (user/assistant/tool)
            - content (str): Message content text
        graph_structure: Pydantic GraphStructure object containing:
            - nodes: List[NodeInfo] with id, message_id, vulnerability attributes
            - edges: List[EdgeInfo] with source, target node identifiers
        total_tokens (int): Normalization denominator for token-based scoring
        total_cost (float): Normalization denominator for cost-based scoring (euros/USD)
        total_number_messages (int): Normalization denominator for message-based scoring
        w_cost (float): Cost component weight [0.0-1.0], default 0.3
        w_msg (float): Message distance component weight [0.0-1.0], default 0.1  
        w_tokens (float): Token content component weight [0.0-1.0], default 0.6
        probability_display_mode (str): Controls optional Rich preview table.
            - "weighted": show weighted scores only
            - "logistic": show logistic scores only
            - "both" (default): show both weighted and logistic columns
            - "none": suppress the preview entirely
        probability_output_mode (str): Selects which score set the caller receives.
            - "weighted" (default): return original weighted scores
            - "logistic": return sigmoid-transformed scores

    Returns:
        dict: Edge probability mapping {source_id->target_id: probability_score}
              - Keys: String format "source_node_id->target_node_id"
              - Values: Float probabilities in [0.0, 1.0] representing exploitation likelihood
              
    Raises:
        ValueError: If weights don't sum to 1.0 (mathematical consistency requirement)
        KeyError: If graph nodes reference non-existent message IDs
        IndexError: If message_id references exceed filtered_log bounds
        
    Example:
        >>> log = [{"message_id": 0, "content_tokens": 100, "role": "user", "content": "scan"}]
        >>> nodes = [NodeInfo(id="1", message_id=0, vulnerability=True)]
        >>> edges = [EdgeInfo(source="0", target="1")]
        >>> graph = GraphStructure(nodes=nodes, edges=edges)
        >>> probs = compute_edge_probabilities_offline(log, graph, 100, 0.01, 1)
        >>> assert "0->1" in probs
        >>> assert 0.0 <= probs["0->1"] <= 1.0
    
    Note:
        This function is the mathematical core of the CAI-CTR probability model,
        bridging natural language conversation analysis with quantitative security
        game theory. The output directly feeds into CTR payoff matrix construction.
    """
    
    if w_cost + w_msg + w_tokens != 1.0:
        raise ValueError("Weights for each component (cost, message, tokens) must sum to 1")

    # Compute cost per token
    euro_cost_per_token = total_cost / total_tokens if total_tokens > 0 and total_cost > 0 else None

    # If no cost, adjust and redistribute w_cost to w_msg and w_tokens
    if not euro_cost_per_token:
        total_remaining = w_msg + w_tokens
        if total_remaining > 0:
            msg_proportion = w_msg / total_remaining
            tokens_proportion = w_tokens / total_remaining
            w_msg = msg_proportion
            w_tokens = tokens_proportion
        w_cost = 0.0

    # Map nodes by ID and find vulnerable nodes sorted by message_id
    node_map = {node.id: node for node in graph_structure.nodes}
    vulnerable_nodes = sorted(
        [node for node in graph_structure.nodes if node.vulnerability],
        key=lambda x: x.message_id
    )

    # Iterate over edges to compute edge_probabilities
    edge_probabilities = {}
    for edge in graph_structure.edges:
        # Check node existence
        if edge.source not in node_map:
            print(f"Warning: Edge source node '{edge.source}' not found in graph. Skipping edge {edge.source}->{edge.target}")
            continue
        if edge.target not in node_map:
            print(f"Warning: Edge target node '{edge.target}' not found in graph. Skipping edge {edge.source}->{edge.target}")
            continue
        source_node = node_map[edge.source]
        target_node = node_map[edge.target]

        # Obtain message distance score  
        msg_diff = abs(target_node.message_id - source_node.message_id)

        # Find the closest vulnerable node by absolute distance
        if target_node.vulnerability:
            Smsg_i = (msg_diff + 1) if msg_diff > 0 else 1
            path_start_msg_id = source_node.message_id
            path_end_msg_id = target_node.message_id
        else:
            # If target is not vulnerable, find closest vulnerable node
            closest_distance = float('inf')
            closest_v_node = None
            for v_node in vulnerable_nodes:
                # Calculate total path distance: source->target->vulnerable
                path_distance = abs(target_node.message_id - source_node.message_id) + \
                                abs(v_node.message_id - target_node.message_id)
                if path_distance < closest_distance:
                    closest_distance = path_distance
                    closest_v_node = v_node
            if closest_distance == float('inf'):
                Smsg_i = 0
                path_start_msg_id = source_node.message_id
                path_end_msg_id = target_node.message_id
            else:
                # For non-vulnerable target, we'll use the full path through to the closest vulnerable node
                Smsg_i = (closest_distance + 1)  
                path_start_msg_id = source_node.message_id
                path_end_msg_id = closest_v_node.message_id

        # Estimate tokens along the path
        token_estimate_i = 0
        Scost_i = 0

        min_msg_id = min(path_start_msg_id, path_end_msg_id)
        max_msg_id = max(path_start_msg_id, path_end_msg_id)
        if path_start_msg_id < path_end_msg_id:
            msg_range = range(min_msg_id + 1, max_msg_id + 1)
        else:
            msg_range = range(max_msg_id, min_msg_id - 1, -1)
        for msg_id in msg_range:
            try:
                token_estimate_i += filtered_log[msg_id - 1]["content_tokens"]
            except IndexError:
                continue
       
        # Scores
        if euro_cost_per_token:
            Scost_i = token_estimate_i * euro_cost_per_token
            Scost_normalized= 1.0 - (Scost_i / total_cost) if (total_cost > 0 and Scost_i > 0) else 1.0
        else:
            Scost_normalized = 0
        Stokens_normalized = 1.0 - (token_estimate_i / total_tokens) if (total_tokens > 0 and token_estimate_i > 0) else 1.0
        Smsg_normalized = 1.0 - ((Smsg_i -1)/ (total_number_messages -1)) if (total_number_messages > 1 and Smsg_i > 0) else 1.0
        
        # -- Final edge score (Ct) --
        Ct = 0
        if Scost_normalized > 0: 
            Ct += w_cost * Scost_normalized
        if Smsg_normalized > 0:  
            Ct += w_msg * Smsg_normalized
        if Stokens_normalized > 0: 
            Ct += w_tokens * Stokens_normalized
 
        edge_key = f"{edge.source}->{edge.target}"
        edge_probabilities[edge_key] = Ct

    # Derive logistic-normalized probabilities without altering primary computation
    def _logistic_transform(value: float, midpoint: float = 0.5, steepness: float = 10.0) -> float:
        """Squash [0, 1] inputs into (0, 1) using a tuned sigmoid."""
        # Clamp to reasonable domain to avoid overflow in exp
        adjusted = max(min(value, 1.0), 0.0)
        exponent = -steepness * (adjusted - midpoint)
        try:
            return 1.0 / (1.0 + math.exp(exponent))
        except OverflowError:
            return 0.0 if exponent > 0 else 1.0

    mode = (probability_display_mode or "both").lower()
    show_weighted = mode in {"weighted", "both"}
    show_logistic = mode in {"logistic", "both"}
    show_preview = mode not in {"none", "off"}

    logistic_edge_probabilities = {}
    if show_logistic or probability_output_mode.lower() == "logistic":
        logistic_edge_probabilities = {
            edge: _logistic_transform(prob)
            for edge, prob in edge_probabilities.items()
        }

    # Present an optional comparison table using Rich (limited to top entries)
    if edge_probabilities and show_preview and (show_weighted or show_logistic):
        console = Console()
        table = Table(title="CTR Edge Probability Comparison", show_lines=False)
        table.add_column("Edge", style="cyan", no_wrap=True)
        if show_weighted:
            table.add_column("Weighted Score", justify="right")
        if show_logistic:
            table.add_column("Logistic Score", justify="right")

        # Show up to 8 highest-scoring edges for quick comparison
        for edge_name, original_prob in sorted(edge_probabilities.items(), key=lambda item: item[1], reverse=True)[:8]:
            row = [edge_name]
            if show_weighted:
                row.append(f"{original_prob:.4f}")
            if show_logistic:
                logistic_prob = logistic_edge_probabilities.get(edge_name, 0.0)
                row.append(f"{logistic_prob:.4f}")
            table.add_row(*row)

        console.print(table)

    if probability_output_mode.lower() == "logistic":
        return logistic_edge_probabilities

    return edge_probabilities
