"""
CTR Canvas - Interactive CTR run viewer (Textual)

- Replaces the old Workflow canvas with a dedicated CTR visualizer.
- Loads prior CTR runs from tools/cut_the_rope/ctr_cai/output/run_*/
- Parses GraphStructure JSON and shows an interactive DAG layout.
- Overlays optimal defense probabilities on nodes when available.
- No backend/commands touched; purely front-end visualization.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
from copy import deepcopy
from typing import Any, Dict, List, Optional, Tuple

from textual import on
from textual.app import ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.geometry import Offset
from textual.message import Message
from textual.events import Click
from textual.widgets import Button, Label, Select, Static
from cai.tui.components.ctr_graph_viewport import GraphViewport, NodeSelected
from cai.ctr.paths import get_ctr_output_base_dir


class CTRNode(Container):
    """A positioned, clickable node representing a CTR graph node."""

    DEFAULT_CSS = """
    CTRNode { width: 36; min-height: 5; padding: 0; background: #0f1115; border: solid #1b1f2a; layer: above; }
    CTRNode:hover { background: #121725; border: solid #2a3243; }
    CTRNode.-selected { border: solid #2d7ff9; background: rgba(45,127,249,0.06); }
    
    .hdr { height: 3; text-align: left; content-align: center middle; dock: top; padding: 0 2; border-bottom: solid #1b1f2a; color: #e6edf3; text-style: bold; }
    .hdr.ok { background: #0f141b; border-left: solid #2caf85; }
    .hdr.vuln { background: #14120f; border-left: solid #f9707a; }
    .body { padding: 1 2; color: #a6b0c3; }
    .defprob { color: #ffd97b; text-style: bold; }
    """

    def __init__(self, node_id: str, title: str, vulnerable: bool, position: Tuple[int, int], defense_prob: Optional[float] = None, **kwargs) -> None:
        super().__init__(**kwargs)
        self.node_id = node_id
        self.title = title
        self.vulnerable = vulnerable
        self.position = position
        self.defense_prob = defense_prob
        self.styles.offset = position
        self._selected = False

    def compose(self) -> ComposeResult:
        hdr_class = "hdr vuln" if self.vulnerable else "hdr ok"
        yield Label(self.title, classes=hdr_class)
        prob = f" – D={self.defense_prob:.3f}" if isinstance(self.defense_prob, (int, float)) else ""
        yield Label(f"ID {self.node_id}{prob}", classes="body defprob" if prob else "body")

    def select(self, on: bool) -> None:
        self._selected = on
        if on:
            self.add_class("-selected")
        else:
            self.remove_class("-selected")

    def on_click(self, event: Click) -> None:  # bubble a selection message
        self.post_message(NodeChosen(self.node_id))
        event.stop()


class NodeChosen(Message):
    def __init__(self, node_id: str) -> None:
        super().__init__()
        self.node_id = node_id


class CTRCanvas(Container):
    """CTR interactive graph viewer bound to CTR run outputs."""

    DEFAULT_CSS = """
    CTRCanvas { width: 100%; height: 100%; layout: vertical; background: #0b0e14; }

    # Top toolbar
    #ctr-toolbar { height: 5; min-height: 5; max-height: 5; background: #0f1117; border-bottom: solid #1b2230; padding: 1 2 0 2; layout: horizontal; align: center middle; }
    #ctr-toolbar Label { color: #e6edf3; padding: 0 1 0 0; min-width: 8; content-align: center middle; }
    #ctr-toolbar Select { width: 1fr; min-width: 28; height: 3; min-height: 3; margin: 0 1 0 0; }
    #ctr-toolbar Button { height: 3; min-height: 3; min-width: 8; margin: 0 1 0 0; }
    #ctr-toolbar Button:last-of-type { margin-right: 0; }

    #workspace { width: 100%; height: 1fr; layout: horizontal; }
    #left { width: 1fr; height: 100%; layout: vertical; }
    #canvas { width: 100%; height: 1fr; background: #0b0e14; overflow: auto; scrollbar-size: 1 1; scrollbar-color: #529d86; scrollbar-background: #2e4f46; }
    #right { width: 38; height: 100%; border-left: solid #1b2230; background: #0f1117; }
    #status { height: 1; background: #0f1117; border-top: solid #1b2230; padding: 0 2; color: #7f8aa3; }

    .edge-marker { background: transparent; color: #55607a; }
    """

    PLACEHOLDER_ID = "__ctr_placeholder__"
    CURRENT_RUN_ID = "__ctr_current__"
    PLACEHOLDER_LABEL = "Graph placeholder (sample run)"
    CURRENT_RUN_LABEL = "Run from current session"
    PLACEHOLDER_GRAPH = {
        "nodes": [
            {
                "id": "1",
                "name": "Recon: Enumerate AD DNS",
                "info": "Attacker queries domain controllers and SRV records to map the forest.",
                "vulnerability": False,
                "message_id": 1,
            },
            {
                "id": "2",
                "name": "Gather Users & Groups",
                "info": "LDAP queries and BloodHound-style collection for ACL relationships.",
                "vulnerability": False,
                "message_id": 2,
            },
            {
                "id": "3",
                "name": "Password Spray",
                "info": "Low-and-slow spray across OWA/ADFS endpoints to locate weak creds.",
                "vulnerability": True,
                "message_id": 3,
            },
            {
                "id": "4",
                "name": "Initial Foothold (Workstation)",
                "info": "Compromise standard user workstation in marketing OU.",
                "vulnerability": True,
                "message_id": 4,
            },
            {
                "id": "5",
                "name": "Kerberoast Service Accounts",
                "info": "Extract SPNs, request tickets, crack weak service creds offline.",
                "vulnerability": True,
                "message_id": 5,
            },
            {
                "id": "6",
                "name": "Lateral Movement (PSExec)",
                "info": "Reuse cracked service creds to reach member servers.",
                "vulnerability": True,
                "message_id": 6,
            },
            {
                "id": "7",
                "name": "Dump LSASS / Hashes",
                "info": "Harvest additional credentials and NTLM hashes from memory.",
                "vulnerability": True,
                "message_id": 7,
            },
            {
                "id": "8",
                "name": "Privilege Escalation (DC Sync)",
                "info": "Abuse Replication permissions to pull KRBTGT hash.",
                "vulnerability": True,
                "message_id": 8,
            },
            {
                "id": "9",
                "name": "Golden Ticket Issued",
                "info": "Forge TGTs for Enterprise Admin persistence.",
                "vulnerability": True,
                "message_id": 9,
            },
            {
                "id": "10",
                "name": "Domain Dominance",
                "info": "Full control over Active Directory forest achieved.",
                "vulnerability": True,
                "message_id": 10,
            },
            {
                "id": "leaf_impact",
                "name": "Impact: Root Access",
                "info": "End-state objective representing complete domain compromise.",
                "vulnerability": True,
                "message_id": 11,
            },
            {
                "id": "11",
                "name": "Defender Alert: DC Sync Monitor",
                "info": "Simulated defensive node indicating detection opportunity.",
                "vulnerability": False,
                "message_id": 12,
            },
            {
                "id": "12",
                "name": "Segmented Admin Workstation",
                "info": "Hardens against lateral reuse of service credentials.",
                "vulnerability": False,
                "message_id": 13,
            },
        ],
        "edges": [
            {"source": "1", "target": "2"},
            {"source": "2", "target": "3"},
            {"source": "3", "target": "4"},
            {"source": "4", "target": "5"},
            {"source": "5", "target": "6"},
            {"source": "6", "target": "7"},
            {"source": "7", "target": "8"},
            {"source": "8", "target": "9"},
            {"source": "9", "target": "10"},
            {"source": "10", "target": "leaf_impact"},
            {"source": "3", "target": "5"},
            {"source": "5", "target": "7"},
            {"source": "7", "target": "9"},
            {"source": "11", "target": "8"},
            {"source": "12", "target": "6"},
        ],
    }
    PLACEHOLDER_BASELINE = {
        "optimal_defense": {
            "1": 0.12,
            "2": 0.18,
            "3": 0.27,
            "4": 0.35,
            "5": 0.62,
            "6": 0.74,
            "7": 0.81,
            "8": 0.9,
            "9": 0.95,
            "10": 0.97,
        }
    }

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._runs: List[Tuple[str, str]] = []  # [(path, label)]
        self._run_options: List[Tuple[str, str]] = []  # [(label, value_path)]
        self._node_widgets: Dict[str, CTRNode] = {}
        self._edges: List[Tuple[str, str]] = []
        self._defense: Dict[str, float] = {}
        self._node_meta: Dict[str, Dict] = {}
        self._edge_markers: List[Static] = []
        # Plotext rendering disabled in favor of dedicated viewport
        self._use_plotext: bool = False
        self._current_task: Optional[asyncio.Task[Any]] = None

    def compose(self) -> ComposeResult:
        with Horizontal(id="ctr-toolbar"):
            yield Label("Run:")
            yield Select([], id="run-select", prompt="Select run…")
            yield Button("Reload", id="reload-run")
            yield Button("Layout", id="relayout")
            yield Button("Zoom +", id="zoom_in")
            yield Button("Zoom -", id="zoom_out")
            yield Button("Fit", id="fit")
            yield Button("Clear", id="clear")

        with Horizontal(id="workspace"):
            with Vertical(id="left"):
                with Container(id="canvas"):
                    yield GraphViewport(id="viewport")
                yield Static("Ready", id="status")
            with Vertical(id="right"):
                yield Static("Details", id="details-title")
                yield Static("—", id="details-body")

    def on_mount(self) -> None:
        self._load_runs_into_select()
        # Auto-load preferred run if present, else latest
        sel = self.query_one("#run-select", Select)
        preferred_env = os.getenv("CAI_CTR_DEFAULT_RUN") or os.getenv("CAI_CTR_DEFAULT_OUTPUT_DIR")
        preferred_env = preferred_env.strip().replace("\n", "").replace("\r", "") if isinstance(preferred_env, str) else None
        candidates: List[str] = []
        actual_paths = [path for path, _label in self._runs]
        if preferred_env:
            env_path = os.path.abspath(preferred_env)
            if os.path.basename(env_path).startswith("run_"):
                candidates.append(env_path)
            elif os.path.isdir(env_path):
                try:
                    env_runs = sorted(
                        [
                            os.path.join(env_path, d)
                            for d in os.listdir(env_path)
                            if d.startswith("run_")
                        ]
                    )
                    if env_runs:
                        candidates.append(env_runs[-1])
                except Exception:
                    pass
        
        if actual_paths:
            # Prefer the latest actual run first, then the earliest as a fallback
            candidates.append(actual_paths[-1])
            candidates.append(actual_paths[0])

        placeholder_available = any(
            str(value) == self.PLACEHOLDER_ID for _label, value in self._run_options
        )
        if placeholder_available:
            candidates.append(self.PLACEHOLDER_ID)

        chosen = self._resolve_preferred_run(candidates)
        if chosen is None and placeholder_available:
            chosen = self.PLACEHOLDER_ID

        if chosen is not None:
            sel.value = chosen
            self._load_selected_run()
        else:
            # BUGFIX: Even if no preferred run, ensure we have a valid selection
            # Default to placeholder if available
            if self._run_options:
                sel.value = self._run_options[0][1]  # First option's value
                self._load_selected_run()
            else:
                self._set_status("No CTR runs available")

    # ---------- Run discovery / parsing ----------
    def _output_base(self) -> str:
        # Honor env override first
        env = os.getenv("CAI_CTR_DEFAULT_OUTPUT_DIR")
        if env:
            env = env.strip().replace("\n", "").replace("\r", "")
            if os.path.isdir(env):
                # If pointing to a specific run dir, use its parent as base
                if os.path.basename(env).startswith("run_"):
                    parent = os.path.dirname(env)
                    if os.path.isdir(parent):
                        return parent
                return env
        candidates = [
            get_ctr_output_base_dir(),  # Preferred base directory
            os.path.join("tools", "cut_the_rope", "ctr_cai", "output"),  # legacy
            os.path.join(os.getcwd(), "tools", "cut_the_rope", "ctr_cai", "output"),  # legacy 2
        ]
        for p in candidates:
            if os.path.isdir(p):
                return p
        return candidates[0]

    def _resolve_preferred_run(self, candidates: List[str]) -> Optional[str]:
        if not self._run_options:
            return None
        for candidate in candidates:
            if candidate is None:
                continue
            candidate_str = str(candidate)
            # Direct match against option values (covers placeholder id)
            for _label, value in self._run_options:
                if str(value) == candidate_str:
                    return value
            # Attempt absolute-path comparisons for filesystem targets
            try:
                candidate_abs = os.path.abspath(candidate_str)
            except Exception:
                continue
            for _label, value in self._run_options:
                try:
                    value_abs = os.path.abspath(str(value))
                except Exception:
                    continue
                if value_abs == candidate_abs:
                    return value
            basename = os.path.basename(candidate_abs)
            if basename.startswith("run_"):
                for _label, value in self._run_options:
                    try:
                        if os.path.basename(str(value)) == basename:
                            return value
                    except Exception:
                        continue
        return None

    def _load_runs_into_select(self) -> None:
        base = self._output_base()
        runs: List[str] = []
        try:
            for name in sorted(os.listdir(base)):
                if name.startswith("run_"):
                    runs.append(os.path.abspath(os.path.join(base, name)))
        except Exception:
            runs = []
        unique_runs: List[str] = []
        seen: set[str] = set()
        for run in runs:
            if run in seen:
                continue
            seen.add(run)
            unique_runs.append(run)

        select_widget = self.query_one("#run-select", Select)
        current_value = select_widget.value

        self._runs = []
        options: List[Tuple[str, str]] = [
            (self.PLACEHOLDER_LABEL, self.PLACEHOLDER_ID),
            (self.CURRENT_RUN_LABEL, self.CURRENT_RUN_ID),
        ]
        for run in unique_runs:
            label = self._label_for_run(run)
            self._runs.append((run, label))
            options.append((label, run))

        self._run_options = options
        select_widget.set_options(options)
        
        # BUGFIX: Ensure we don't leave the prompt as the selected value
        # Always select a valid option, never leave the prompt selected
        if current_value and any(str(val) == str(current_value) for _label, val in options):
            select_widget.value = current_value
        else:
            # Select the first valid option (placeholder or current run)
            if options:
                select_widget.value = options[0][1]  # First option's value
        status_msg = f"Found {len(self._runs)} CTR runs"
        if not self._runs:
            status_msg += " · using placeholder sample"
        self._set_status(status_msg)

    def _label_for_run(self, run_dir: str) -> str:
        info = os.path.join(run_dir, "graph_information.txt")
        label = os.path.basename(run_dir)
        try:
            with open(info, "r", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("Input Log:"):
                        log_path = line.split(":", 1)[1].strip()
                        label = f"{os.path.basename(run_dir)} · {os.path.basename(log_path)}"
                        break
        except Exception:
            pass
        return label

    def _parse_graphstructure_from_info(self, run_dir: str) -> Optional[Dict]:
        path = os.path.join(run_dir, "graph_information.txt")
        if not os.path.isfile(path):
            return None
        with open(path, "r", encoding="utf-8") as f:
            data = f.read()
        m = re.search(r"LLM Output:\n-+\n(\{[\s\S]*?\})\n-+", data)
        if not m:
            return None
        try:
            return json.loads(m.group(1))
        except Exception:
            return None

    def _parse_baseline_from_ctr(self, run_dir: str) -> Dict:
        path = os.path.join(run_dir, "ctr_baseline.txt")
        result: Dict = {}
        if not os.path.isfile(path):
            return result
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = f.read()
            m = re.search(r"BASELINE RESULT DICTIONARY:\n-+\n(\{[\s\S]*?\})", data)
            if m:
                base = json.loads(m.group(1))
                if isinstance(base.get("attacker_strategy"), str):
                    nums = re.findall(r"[0-9]*\.?[0-9]+", base["attacker_strategy"]) or []
                    base["attacker_strategy"] = [float(x) for x in nums]
                result = base
        except Exception:
            result = {}
        return result

    def _parse_rate_env(self, env_name: str, default: str) -> List[float]:
        raw = os.getenv(env_name, default)
        values: List[float] = []
        if raw is None:
            raw = default
        for part in str(raw).split(","):
            piece = part.strip()
            if not piece:
                continue
            try:
                values.append(float(piece))
            except ValueError:
                continue
        if values:
            return values
        try:
            fallback = float(default.split(",")[0].strip())
        except (ValueError, IndexError):
            fallback = 1.0
        return [fallback]

    def _get_active_runner(self) -> Tuple[Optional[Any], Optional[int]]:
        app = getattr(self, "app", None)
        if not app or not hasattr(app, "session_manager"):
            return None, None
        session_manager = getattr(app, "session_manager", None)
        runners = getattr(session_manager, "terminal_runners", {}) if session_manager else {}
        if not runners:
            return None, None
        terminal_number: Optional[int] = None
        try:
            grid = getattr(app, "terminal_grid", None)
            if grid and hasattr(grid, "get_focused_terminal"):
                focused = grid.get_focused_terminal()
                if focused and hasattr(focused, "terminal_number"):
                    terminal_number = getattr(focused, "terminal_number")
        except Exception:
            terminal_number = None
        if terminal_number is None or terminal_number not in runners:
            if 1 in runners:
                terminal_number = 1
            else:
                try:
                    terminal_number = sorted(runners.keys())[0]
                except Exception:
                    terminal_number = None
        if terminal_number is None:
            return None, None
        return runners.get(terminal_number), terminal_number

    def _collect_current_session_messages(self) -> Tuple[List[Dict[str, Any]], Optional[str]]:
        runner, _terminal_number = self._get_active_runner()
        if not runner:
            return [], None
        agent_name = None
        try:
            agent_name = getattr(getattr(runner, "config", None), "agent_name", None)
        except Exception:
            agent_name = None
        try:
            history = runner.get_history() or []
        except Exception:
            history = []
        # Ensure each entry is a plain dict copy to avoid mutations
        plain_history: List[Dict[str, Any]] = []
        for msg in history:
            if isinstance(msg, dict):
                plain_history.append(dict(msg))
        return plain_history, agent_name

    def _message_key(self, message: Dict[str, Any]) -> Tuple[Any, ...]:
        role = message.get("role")
        content = message.get("content")
        try:
            content_repr = json.dumps(content, sort_keys=True)
        except Exception:
            content_repr = str(content)
        tool_calls = message.get("tool_calls")
        try:
            tool_repr = json.dumps(tool_calls, sort_keys=True)
        except Exception:
            tool_repr = str(tool_calls)
        name = message.get("name") or message.get("tool_call_id")
        return role, content_repr, tool_repr, name

    def _merge_messages(
        self,
        primary: List[Dict[str, Any]],
        secondary: List[Dict[str, Any]],
    ) -> List[Dict[str, Any]]:
        merged: List[Dict[str, Any]] = []
        seen: set[Tuple[Any, ...]] = set()
        for pool in (primary, secondary):
            for msg in pool or []:
                if not isinstance(msg, dict):
                    continue
                key = self._message_key(msg)
                if key in seen:
                    continue
                seen.add(key)
                merged.append(dict(msg))
        return merged

    def _graph_structure_to_dict(self, graph: Any) -> Dict[str, List[Dict[str, Any]]]:
        nodes: List[Dict[str, Any]] = []
        edges: List[Dict[str, Any]] = []
        raw_nodes = getattr(graph, "nodes", None)
        if raw_nodes:
            for node in raw_nodes:
                if hasattr(node, "dict"):
                    node_data = node.dict()
                elif isinstance(node, dict):
                    node_data = dict(node)
                else:
                    continue
                node_id = str(node_data.get("id")) if node_data.get("id") is not None else ""
                if not node_id:
                    continue
                node_data["id"] = node_id
                node_data.setdefault("name", node_id)
                node_data.setdefault("info", "")
                node_data["vulnerability"] = bool(node_data.get("vulnerability"))
                node_data.setdefault("message_id", None)
                nodes.append(node_data)
        raw_edges = getattr(graph, "edges", None)
        if raw_edges:
            for edge in raw_edges:
                if hasattr(edge, "dict"):
                    edge_data = edge.dict()
                elif isinstance(edge, dict):
                    edge_data = dict(edge)
                else:
                    continue
                source = edge_data.get("source")
                target = edge_data.get("target")
                if source is None or target is None:
                    continue
                edge_data["source"] = str(source)
                edge_data["target"] = str(target)
                edges.append(edge_data)
        return {"nodes": nodes, "edges": edges}

    async def _build_graph_from_current_session(self) -> None:
        task = asyncio.current_task()
        try:
            self._set_status("Building CTR graph from current session…")
            try:
                from cai.sdk.agents.run_to_jsonl import (
                    get_session_recorder,
                    get_token_stats,
                    load_history_from_jsonl,
                )
                from cai.ctr.experiment import process_in_memory_session
            except Exception as exc:  # pragma: no cover - import failure path
                self._set_status(f"CTR tooling unavailable: {exc}")
                return

            jsonl_path: Optional[str] = None
            file_messages: List[Dict[str, Any]] = []
            try:
                recorder = get_session_recorder()
            except Exception:
                recorder = None
            if recorder and hasattr(recorder, "filename"):
                candidate = getattr(recorder, "filename")
                if isinstance(candidate, str) and os.path.isfile(candidate):
                    jsonl_path = candidate
                    try:
                        file_messages = load_history_from_jsonl(
                            candidate,
                            system_prompt=True,
                            truncate_tool_responses=False,
                        )
                    except Exception as exc:  # pragma: no cover - log parsing failure
                        self._set_status(f"Failed to read session log: {exc}")
                        file_messages = []

            memory_messages, agent_name = self._collect_current_session_messages()
            combined_messages = self._merge_messages(file_messages, memory_messages)
            if not combined_messages:
                self._set_status("Current session has no messages yet")
                return

            token_counts: Optional[Dict[str, Any]] = None
            if jsonl_path:
                try:
                    stats = get_token_stats(jsonl_path)
                except Exception:
                    stats = None
                if stats:
                    (
                        model_name,
                        prompt_tokens,
                        completion_tokens,
                        total_cost,
                        active_time,
                        idle_time,
                    ) = stats
                    token_counts = {
                        "model_name": model_name,
                        "input_tokens": prompt_tokens,
                        "output_tokens": completion_tokens,
                        "total_tokens": prompt_tokens + completion_tokens,
                        "total_cost": total_cost,
                        "active_time_seconds": active_time,
                        "idle_time_seconds": idle_time,
                    }

            distance_heuristic = os.getenv("CAI_CTR_DISTANCE_HEURISTIC") or None
            if isinstance(distance_heuristic, str):
                distance_heuristic = distance_heuristic.strip() or None
            is_ctf = os.getenv("CAI_CTR_IS_CTF", "false").lower() in {"true", "1", "yes"}
            attack_rates = self._parse_rate_env("CAI_CTR_ATTACK_RATES", "2")
            defense_rates = self._parse_rate_env("CAI_CTR_DEFENSE_RATES", "1")

            try:
                result = await process_in_memory_session(
                    messages=combined_messages,
                    token_counts=token_counts,
                    is_ctf=is_ctf,
                    attack_rate_list=attack_rates,
                    defense_rate_list=defense_rates,
                    distance_heuristic=distance_heuristic,
                )
            except asyncio.CancelledError:
                self._set_status("Cancelled current session graph build")
                raise
            except Exception as exc:
                self._set_status(f"CTR graph build failed: {exc}")
                return

            graph = result.get("graph_structure_llm")
            if not graph:
                self._set_status("CTR analysis returned no graph")
                return

            gs_dict = self._graph_structure_to_dict(graph)
            if not gs_dict.get("nodes"):
                self._set_status("CTR analysis returned an empty graph")
                return

            self._node_meta = gs_dict
            self._defense = {}
            self._node_widgets = {}
            self._edges = [
                (edge["source"], edge["target"])
                for edge in gs_dict.get("edges", [])
                if edge.get("source") and edge.get("target")
            ]

            try:
                vp = self.query_one("#viewport", GraphViewport)
                vp.set_graph(gs_dict["nodes"], gs_dict["edges"], self._defense)
                vp.layout_fill_view(margin=6)
                vp.render_now()
                
                # AUTO-FIT: Apply fit automatically when loading content for the first time
                self._apply_fit_to_viewport()
            except Exception:
                self._set_status("Loaded current session graph (viewport unavailable)")
                return

            vuln_count = sum(1 for n in gs_dict["nodes"] if n.get("vulnerability"))
            node_count = len(gs_dict["nodes"])
            edge_count = len(gs_dict["edges"])
            agent_label = agent_name or "current agent"
            detail_parts: List[str] = ["current session"]
            if jsonl_path:
                detail_parts.append(os.path.basename(jsonl_path))
            detail = " · ".join(detail_parts)
            self._set_status(
                f"Built {node_count} nodes/{edge_count} edges from {agent_label} ({detail}) · vulnerable={vuln_count}"
            )
        finally:
            if self._current_task is task:
                self._current_task = None

    # ---------- Layout / draw ----------
    def _build_layout(self, nodes: List[Dict], edges: List[Dict]) -> Dict[str, Tuple[int, int]]:
        node_ids = [str(n.get("id")) for n in nodes if n.get("id") is not None]
        if not node_ids:
            return {}
        succ: Dict[str, List[str]] = {nid: [] for nid in node_ids}
        indeg: Dict[str, int] = {nid: 0 for nid in node_ids}
        for e in edges:
            src = str(e.get("source")) if e.get("source") is not None else None
            dst = str(e.get("target")) if e.get("target") is not None else None
            if src in succ and dst in succ:
                succ[src].append(dst)
                indeg[dst] += 1
        roots = [nid for nid in node_ids if indeg.get(nid, 0) == 0] or node_ids[:1]
        depth: Dict[str, int] = {nid: 0 for nid in node_ids}
        visited = set()
        queue = list(roots)
        while queue:
            u = queue.pop(0)
            visited.add(u)
            for v in succ.get(u, []):
                depth[v] = max(depth.get(v, 0), depth[u] + 1)
                if v not in visited:
                    queue.append(v)

        layers: Dict[int, List[str]] = {}
        for nid, d in depth.items():
            layers.setdefault(d, []).append(nid)
        for d in layers:
            layers[d].sort(key=lambda x: (x.startswith("leaf_"), x))

        pos: Dict[str, Tuple[int, int]] = {}
        x_gap, y_gap = 24, 8
        for d in sorted(layers.keys()):
            for i, n in enumerate(layers[d]):
                pos[n] = (4 + d * x_gap, 2 + i * y_gap)
        return pos

    def _clear_canvas(self) -> None:
        # Delegate cleanup to viewport on-demand; keep simple here
        try:
            vp = self.query_one("#viewport", GraphViewport)
            # No explicit clear needed; viewport will overwrite output
            _ = vp
        except Exception:
            pass

    def _draw(self) -> None:
        # Delegate to viewport
        try:
            vp = self.query_one("#viewport", GraphViewport)
            vp.render_now()
        except Exception:
            pass

    def _apply_fit_to_viewport(self) -> None:
        """Apply fit operation to viewport (helper method for auto-fit and manual fit)"""
        try:
            vp = self.query_one("#viewport", GraphViewport)
            vp.scale = 1.0
            vp.offset_x = 2.0
            vp.offset_y = 2.0
            vp.fit_content(margin=6)
            vp.render_now()
        except Exception:
            pass

    @on(Button.Pressed, "#fit")
    def _on_fit(self) -> None:
        self._apply_fit_to_viewport()

    @on(Button.Pressed, "#zoom_in")
    def _on_zoom_in(self) -> None:
        try:
            vp = self.query_one("#viewport", GraphViewport)
            vp.zoom(1.2)
        except Exception:
            pass

    @on(Button.Pressed, "#zoom_out")
    def _on_zoom_out(self) -> None:
        try:
            vp = self.query_one("#viewport", GraphViewport)
            vp.zoom(1/1.2)
        except Exception:
            pass


    def _redraw_edges(self) -> None:
        try:
            vp = self.query_one("#viewport", GraphViewport)
            vp.render_now()
        except Exception:
            pass

    # ---------- Events ----------
    @on(Select.Changed, "#run-select")
    def _on_run_changed(self, event: Select.Changed) -> None:
        # BUGFIX: Prevent selection of the prompt/placeholder text
        # The prompt "Select run…" should not be selectable
        sel = self.query_one("#run-select", Select)
        
        # Check if the selected value is valid (not None, not empty, and exists in options)
        if not sel.value:
            # Reset to a valid default if available
            if self._run_options:
                # Try to find the first valid option (not a prompt)
                for label, value in self._run_options:
                    if value and value != "":  # Skip empty values that might be prompts
                        sel.value = value
                        break
            return
        
        # Verify the selected value exists in our options
        valid_values = [value for _label, value in self._run_options if value]
        if str(sel.value) not in [str(v) for v in valid_values]:
            # Invalid selection, reset to first valid option
            if valid_values:
                sel.value = valid_values[0]
            return
        
        self._load_selected_run()

    @on(Button.Pressed, "#reload-run")
    def _on_reload(self) -> None:
        self._load_runs_into_select()
        self._load_selected_run()

    @on(Button.Pressed, "#relayout")
    def _on_relayout(self) -> None:
        if not self._node_meta:
            return
        try:
            vp = self.query_one("#viewport", GraphViewport)
            # Fill viewport entirely for separations
            vp.layout_fill_view(margin=6)
            vp.render_now()
        except Exception:
            pass

    @on(Button.Pressed, "#clear")
    def _on_clear(self) -> None:
        self._clear_canvas()
        self._set_status("Cleared")

    @on(NodeSelected)
    def _on_node_clicked(self, message: NodeSelected) -> None:
        nid = message.node_id
        meta = None
        # Robust lookup by string id
        for n in (self._node_meta.get("nodes", []) or []):
            if str(n.get("id")) == str(nid):
                meta = n
                break
        if meta:
            vuln = "Yes" if meta.get("vulnerability") else "No"
            info = meta.get("info", "")
            dp = self._defense.get(nid)
            lines = [
                f"ID: {nid}",
                f"Name: {meta.get('name','')}",
                f"Vulnerable: {vuln}",
                f"Message ID: {meta.get('message_id','-')}",
            ]
            if isinstance(dp, (int, float)):
                lines.append(f"Defense prob: {dp:.3f}")
            if info:
                lines.append("")
                lines.append(info)
            try:
                self.query_one("#details-body", Static).update("\n".join(lines))
                # Ensure the right panel is visible by forcing a small refresh
                self.refresh(layout=True)
            except Exception:
                self._set_status(f"Selected {nid} (details panel not found)")
            # Also reflect on status bar
            self._set_status(f"Selected node {nid}")

    # ---------- Helpers ----------
    def _set_status(self, msg: str) -> None:
        self.query_one("#status", Static).update(msg)

    def _load_placeholder_graph(self, reason: Optional[str] = None) -> bool:
        gs = deepcopy(self.PLACEHOLDER_GRAPH)
        baseline = deepcopy(self.PLACEHOLDER_BASELINE)
        self._defense = {
            str(k): float(v) for k, v in baseline.get("optimal_defense", {}).items()
        }
        self._node_meta = gs
        try:
            vp = self.query_one("#viewport", GraphViewport)
        except Exception:
            return False
        vp.set_graph(gs["nodes"], gs["edges"], self._defense)
        vp.layout_fill_view(margin=6)
        vp.render_now()
        
        # AUTO-FIT: Apply fit automatically when loading content for the first time
        self._apply_fit_to_viewport()
        
        vuln_count = sum(1 for n in gs["nodes"] if n.get("vulnerability"))
        status_msg = reason or "Showing sample CTR graph"
        self._set_status(
            f"{status_msg} · nodes={len(gs['nodes'])} vulnerabilities={vuln_count}"
        )
        return True

    def _load_selected_run(self) -> None:
        sel = self.query_one("#run-select", Select)
        if not sel.value:
            self._set_status("No run selected")
            return
        
        run_dir = str(sel.value)
        
        # BUGFIX: Additional validation to prevent errors from invalid selections
        # Ensure the selected value is in our valid options
        valid_values = [value for _label, value in self._run_options if value]
        if run_dir not in [str(v) for v in valid_values]:
            self._set_status(f"Invalid run selection: {run_dir}")
            return
        if run_dir == self.PLACEHOLDER_ID:
            self._load_placeholder_graph()
            return
        if run_dir == self.CURRENT_RUN_ID:
            if self._current_task and not self._current_task.done():
                self._current_task.cancel()

                def _silence(task: asyncio.Task[Any]) -> None:
                    try:
                        task.exception()
                    except asyncio.CancelledError:
                        pass
                    except Exception:
                        pass

                self._current_task.add_done_callback(_silence)
            self._current_task = asyncio.create_task(self._build_graph_from_current_session())
            return
        gs = self._parse_graphstructure_from_info(run_dir)
        if not gs or not isinstance(gs, dict) or "nodes" not in gs or "edges" not in gs:
            # If the selected run doesn't provide a valid graph, switch to the
            # placeholder sample and bail out to avoid using a None `gs` below.
            reason = f"Run {os.path.basename(run_dir)} has no valid GraphStructure"
            loaded = self._load_placeholder_graph(reason)
            if not loaded:
                self._set_status("Run has no valid GraphStructure")
            return
        baseline = self._parse_baseline_from_ctr(run_dir)
        self._defense = {str(k): float(v) for k, v in baseline.get("optimal_defense", {}).items()}
        # Store current graph structure for detail panel lookups
        self._node_meta = gs
        vp = self.query_one("#viewport", GraphViewport)
        vp.set_graph(gs["nodes"], gs["edges"], self._defense)
        vp.layout_fill_view(margin=6)
        vp.render_now()
        
        # AUTO-FIT: Apply fit automatically when loading content for the first time
        self._apply_fit_to_viewport()
        
        vuln_count = sum(1 for n in gs["nodes"] if n.get("vulnerability"))
        self._set_status(f"Loaded {len(gs['nodes'])} nodes, {len(gs['edges'])} edges · vulnerable={vuln_count}")

    # ---------- Plotext rendering ----------
    def _render_plotext(self) -> None:
        try:
            import plotext as plt
        except Exception:
            # Fallback if not available
            return
        # Prepare figure
        plt.clf()
        # Collect positions; if none, compute a layout
        if not self._node_widgets and self._node_meta:
            pos = self._build_layout(self._node_meta.get("nodes", []), self._node_meta.get("edges", []))
            for node in self._node_meta.get("nodes", []):
                nid = node.get("id")
                title = node.get("name", nid)
                vuln = bool(node.get("vulnerability", False))
                dp = self._defense.get(str(nid))
                self._node_widgets[str(nid)] = CTRNode(str(nid), title, vuln, pos.get(str(nid), (0, 0)), defense_prob=dp)
        # Determine bounds
        xs = [n.position[0] for n in self._node_widgets.values()] or [0]
        ys = [n.position[1] for n in self._node_widgets.values()] or [0]
        min_x, max_x = min(xs) - 2, max(xs) + 2
        min_y, max_y = min(ys) - 2, max(ys) + 2
        # Plot edges as lines
        for (u, v) in self._edges:
            nu, nv = self._node_widgets.get(u), self._node_widgets.get(v)
            if not nu or not nv:
                continue
            plt.plot([nu.position[0], nv.position[0]], [nu.position[1], nv.position[1]])
            # Arrow head near target
            tx = nv.position[0] - 0.2 if nv.position[0] > nu.position[0] else nv.position[0] + 0.2
            ty = nv.position[1]
            try:
                plt.text(tx, ty, ">")
            except Exception:
                pass
        # Plot nodes
        for nid, node in self._node_widgets.items():
            color = "red" if node.vulnerable else "cyan"
            try:
                plt.scatter([node.position[0]], [node.position[1]], marker="•", color=color)
                # Label
                label = node.title if len(node.title) <= 20 else node.title[:17] + "…"
                plt.text(node.position[0] + 0.5, node.position[1], label)
            except Exception:
                pass
        # Style
        try:
            plt.xlim(min_x, max_x)
            plt.ylim(min_y, max_y)
            plt.axes(False)
            plt.ticks(None, None)
            plt.frame(False)
            plt.plotsize(120, 36)
        except Exception:
            pass
        # Render into Static
        out = ""
        try:
            out = plt.build()
        except Exception:
            try:
                # Fallback older API
                out = plt.get_plot()
            except Exception:
                out = "(plotext rendering failed)"
        self.query_one("#plotout", Static).update(out)


# Backwards-compat alias: if someone still imports GraphCanvas, provide CTRCanvas
GraphCanvas = CTRCanvas
