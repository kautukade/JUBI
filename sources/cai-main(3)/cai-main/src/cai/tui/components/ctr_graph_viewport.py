from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import math
from textual import on
from textual.app import ComposeResult
from textual.containers import Container
from textual.events import MouseDown, MouseMove, MouseUp, Resize, Click
from textual.message import Message
from textual.widgets import Static


@dataclass
class VPNode:
    id: str
    x: float
    y: float
    title: str
    vulnerable: bool = False
    defense_prob: Optional[float] = None
    w: int = 0
    h: int = 0


@dataclass
class VPEdge:
    u: str
    v: str
    prob: Optional[float] = None
    artificial: bool = False


class NodeSelected(Message, bubble=True):
    def __init__(self, node_id: str) -> None:
        super().__init__()
        self.node_id = node_id


class GraphViewport(Container):
    """ASCII graph viewport with drag and simple line drawing.

    - World coords (float) → screen grid (chars)
    - Draws nodes as labeled boxes and edges as line of dots + arrow head
    - Handles selection and drag with mouse
    - Designed as a drop-in minimal replacement when vector canvas isn't available
    """

    DEFAULT_CSS = """
    GraphViewport { width: 1fr; height: 1fr; background: #0b0e14; }
    #gv-out { width: 1fr; height: 1fr; overflow: hidden; }
    """

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self._v_nodes: Dict[str, VPNode] = {}
        self._edges: List[VPEdge] = []
        self._selected: Optional[str] = None
        self._dragging: Optional[str] = None
        self._drag_origin: Optional[Tuple[int, int]] = None
        self._world_origin: Optional[Tuple[float, float]] = None
        self._panning: bool = False
        self._pan_origin: Optional[Tuple[int, int]] = None
        self.scale: float = 1.0
        self.offset_x: float = 2.0
        self.offset_y: float = 2.0
        self.vert_gain: float = 2.5  # vertical stretch factor for fill layout
        self._out: Optional[Static] = None

    def compose(self) -> ComposeResult:
        yield Static("", id="gv-out")

    def on_mount(self) -> None:
        self._out = self.query_one("#gv-out", Static)
        self.render_now()

    # ---------- Data API ----------
    def set_graph(self, nodes: List[Dict], edges: List[Dict], defense: Dict[str, float]) -> None:
        self._v_nodes.clear()
        for n in nodes:
            nid = str(n.get("id"))
            title = n.get("name", nid)
            vuln = bool(n.get("vulnerability", False))
            dprob = defense.get(nid)
            self._v_nodes[nid] = VPNode(id=nid, x=0.0, y=0.0, title=title, vulnerable=vuln, defense_prob=dprob)
        self._edges = []
        for e in edges:
            u = str(e.get("source"))
            v = str(e.get("target"))
            if u in self._v_nodes and v in self._v_nodes:
                self._edges.append(VPEdge(u=u, v=v))

    def _compute_depth_layers(self) -> Dict[int, List[str]]:
        """Compute DAG-like depth layers without optional networkx dependency."""
        node_ids = list(self._v_nodes.keys())
        if not node_ids:
            return {}
        succ: Dict[str, List[str]] = {nid: [] for nid in node_ids}
        indeg: Dict[str, int] = {nid: 0 for nid in node_ids}
        for e in self._edges:
            if e.u in succ and e.v in succ:
                succ[e.u].append(e.v)
                indeg[e.v] += 1
        roots = [nid for nid in node_ids if indeg.get(nid, 0) == 0] or node_ids[:1]
        depth: Dict[str, int] = {nid: 0 for nid in node_ids}
        queue = list(roots)
        seen: set[str] = set()
        while queue:
            u = queue.pop(0)
            seen.add(u)
            for v in succ.get(u, []):
                depth[v] = max(depth.get(v, 0), depth[u] + 1)
                if v not in seen:
                    queue.append(v)
        layers: Dict[int, List[str]] = {}
        for nid, d in depth.items():
            layers.setdefault(d, []).append(nid)
        for d in layers:
            layers[d].sort(key=lambda x: (x.startswith("leaf_"), x))
        return layers

    def layout_topological(self) -> None:
        layers = self._compute_depth_layers()
        if not layers:
            return
        # Compute average node width to space columns
        # First set provisional sizes so we can estimate
        for nid in self._v_nodes:
            node = self._v_nodes[nid]
            label_len = len(f"{node.id}: {node.title}")
            node.w = max(14, min(36, label_len + 4))
            node.h = 3
        avg_w = max(14, int(sum(n.w for n in self._v_nodes.values()) / max(1, len(self._v_nodes))))
        # Spacing in world units (character units)
        x_gap, y_gap = float(avg_w + 12), 6.0
        # Target vertical rows to avoid a single line feeling, even in chains
        rows_target = max(3, min(8, int(len(self._v_nodes) ** 0.5) or 3))
        for d in sorted(layers.keys()):
            layer_nodes = layers[d]
            if len(layer_nodes) == 1:
                # Zig-zag across rows to use vertical space even for chains
                row = d % rows_target
                nid = layer_nodes[0]
                node = self._v_nodes[nid]
                node.x = float(d) * x_gap
                node.y = float(row) * y_gap
            else:
                for i, nid in enumerate(layer_nodes):
                    node = self._v_nodes[nid]
                    node.x = float(d) * x_gap
                    node.y = float(i) * y_gap
                # Node width accommodates "id: title"
                label_len = len(f"{node.id}: {node.title}")
                node.w = max(14, min(36, label_len + 4))
                node.h = 3
        # Auto-fit after layout
        self.fit_content(margin=6)

    def layout_fill_view(self, margin: int = 6) -> None:
        """Compute a layout that fills the current viewport horizontally and vertically.

        - Columns = DAG layers spaced evenly across width
        - Rows inside a column spread evenly from top to bottom
        - Sets scale=1 and offsets so that world coords are screen coords
        """
        layers = self._compute_depth_layers()
        if not layers:
            return

        # Measure viewport
        width = max(40, (self.size.width or 80))
        height = max(12, (self.size.height or 24))
        W = max(10, width - 2 * margin)
        H = max(6, height - 2 * margin)

        # Prepare node sizes
        for node in self._v_nodes.values():
            label_len = len(f"{node.id}: {node.title}")
            node.w = max(14, min(36, label_len + 4))
            node.h = 3

        L = len(layers)
        # Column centers across width
        if L == 1:
            col_xs = [margin + W // 2]
        else:
            step = W / (L - 1)
            col_xs = [int(margin + i * step) for i in range(L)]

        # Determine target rows for staggering when a layer has single node
        rows_target = max(5, min(14, int(max(5, len(layers)) * 0.8)))

        # Fill per-layer vertically
        for idx, d in enumerate(sorted(layers.keys())):
            ids = layers[d]
            k = len(ids)
            if k == 0:
                continue
            content_H = int(H * self.vert_gain)
            if k == 1:
                # Stagger single-node columns across multiple rows
                r = (idx % rows_target) + 1
                row_step = content_H / (rows_target + 1)
                y = margin + int(r * row_step)
                n = self._v_nodes[ids[0]]
                n.x = float(col_xs[idx])
                n.y = float(y)
            else:
                # Even spacing between margins (k+1 segments), stretched vertically
                row_step = content_H / (k + 1)
                for i, nid in enumerate(ids, start=1):
                    n = self._v_nodes[nid]
                    n.x = float(col_xs[idx])
                    n.y = float(int(margin + i * row_step))

        # Use world==screen coords for this layout
        self.scale = 1.0
        self.offset_x = 0.0
        self.offset_y = 0.0

    def set_vert_gain(self, gain: float) -> None:
        self.vert_gain = max(1.0, min(10.0, gain))
        self.layout_fill_view(margin=6)
        self.render_now()

    def fit_content(self, margin: int = 2) -> None:
        if not self._v_nodes:
            return
        xs = []
        ys = []
        for n in self._v_nodes.values():
            xs.extend([n.x - n.w / 2, n.x + n.w / 2])
            ys.extend([n.y - n.h / 2, n.y + n.h / 2])
        min_x, max_x = min(xs), max(xs)
        min_y, max_y = min(ys), max(ys)
        span_x = max(1.0, max_x - min_x)
        span_y = max(1.0, max_y - min_y)
        # Available size
        width = max(40, (self.size.width or 80)) - margin * 2
        height = max(12, (self.size.height or 24)) - margin * 2
        # Choose scale to use most of the space; favor horizontal spread
        sx = width / span_x
        sy = height / span_y
        self.scale = max(0.2, min(sx, sy))
        # Centering offsets
        world_cx = (min_x + max_x) / 2
        world_cy = (min_y + max_y) / 2
        screen_cx = (width // 2) + margin
        screen_cy = (height // 2) + margin
        self.offset_x = screen_cx - world_cx * self.scale
        self.offset_y = screen_cy - world_cy * self.scale

    def zoom(self, factor: float) -> None:
        """Zoom around viewport center by factor (>1 zoom in, <1 zoom out)."""
        try:
            width = max(40, (self.size.width or 80))
            height = max(12, (self.size.height or 24))
            cx = width / 2
            cy = height / 2
            # Current world center under screen center
            wx = (cx - self.offset_x) / max(1e-6, self.scale)
            wy = (cy - self.offset_y) / max(1e-6, self.scale)
            new_scale = min(6.0, max(0.2, self.scale * factor))
            self.offset_x = cx - wx * new_scale
            self.offset_y = cy - wy * new_scale
            self.scale = new_scale
            self.render_now()
        except Exception:
            pass

    def pan(self, dx: int, dy: int) -> None:
        self.offset_x += dx
        self.offset_y += dy
        self.render_now()

    # ---------- Rendering ----------
    def render_now(self) -> None:
        if not self._out:
            return
        width = max(40, (self.size.width or 80))
        height = max(12, (self.size.height or 24))
        grid = [[" " for _ in range(width)] for _ in range(height)]

        # Draw edges first (under nodes) and collect end markers
        end_markers: List[Tuple[int, int, str]] = []
        for e in self._edges:
            a = self._v_nodes.get(e.u)
            b = self._v_nodes.get(e.v)
            if not a or not b:
                continue
            ax, ay = self._world_to_screen(a.x, a.y)
            bx, by = self._world_to_screen(b.x, b.y)
            # shift to right side of box
            ax += a.w // 2
            bx -= b.w // 2
            self._draw_line(grid, ax, ay, bx, by, char="·")
            # Direction markers: '<' at source, '>' just before target border
            # Compute unit step towards target for proper placement
            sx = 1 if bx > ax else (-1 if bx < ax else 0)
            sy = 1 if by > ay else (-1 if by < ay else 0)
            hx = bx - (sx if sx != 0 else 0)
            hy = by - (sy if sy != 0 else 0)
            # Put '<' near the source as visual cue
            end_markers.append((ax, ay, "<"))
            # Put '>' at the last dot before the target box so it remains visible
            end_markers.append((hx, hy, ">"))

        # Draw nodes (on top of edges)
        for nid, n in self._v_nodes.items():
            sx, sy = self._world_to_screen(n.x, n.y)
            self._draw_box(grid, sx - n.w // 2, sy - n.h // 2, n.w, n.h, vuln=n.vulnerable, selected=(nid == self._selected))
            # label with id prefix
            label = f"{n.id}: {n.title}"
            if len(label) > n.w - 2:
                label = label[: n.w - 5] + "…"
            self._text(grid, sx - (len(label) // 2), sy, label)
            if n.defense_prob is not None:
                dline = f"D={n.defense_prob:.3f}"
                self._text(grid, sx - (len(dline) // 2), sy + 1, dline)

        # Overlay end markers after nodes so '>' remains visible
        for (mx, my, mch) in end_markers:
            self._put(grid, mx, my, mch)

        # Emit
        lines = ["".join(row) for row in grid]
        self._out.update("\n".join(lines))

    def _world_to_screen(self, x: float, y: float) -> Tuple[int, int]:
        sx = int(self.offset_x + x * self.scale)
        sy = int(self.offset_y + y * self.scale)
        return sx, sy

    def _put(self, grid: List[List[str]], x: int, y: int, ch: str) -> None:
        if 0 <= y < len(grid) and 0 <= x < len(grid[0]):
            grid[y][x] = ch

    def _draw_line(self, grid, x0, y0, x1, y1, char="·") -> None:
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self._put(grid, x0, y0, char)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def _draw_box(self, grid, x, y, w, h, vuln=False, selected=False) -> None:
        # Color scheme similar to CTR visuals
        border_color = "yellow" if selected else ("green" if vuln else "cyan")
        def c(ch: str) -> str:
            return f"[{border_color}]{ch}[/]"
        hor = c("─")
        ver = c("│")
        tl, tr, bl, br = c("┌"), c("┐"), c("└"), c("┘")
        if selected:
            hor = c("═"); ver = c("║"); tl = c("╔"); tr = c("╗"); bl = c("╚"); br = c("╝")
        # top/bottom
        for i in range(w):
            self._put(grid, x + i, y, hor)
            self._put(grid, x + i, y + h - 1, hor)
        # sides
        for j in range(h):
            self._put(grid, x, y + j, ver)
            self._put(grid, x + w - 1, y + j, ver)
        # corners
        self._put(grid, x, y, tl)
        self._put(grid, x + w - 1, y, tr)
        self._put(grid, x, y + h - 1, bl)
        self._put(grid, x + w - 1, y + h - 1, br)
        if vuln:
            # subtle tick on top border (colored)
            self._put(grid, x + 1, y, c("▲"))

    def _text(self, grid, x, y, text: str) -> None:
        for i, ch in enumerate(text):
            self._put(grid, x + i, y, ch)

    # ---------- Interaction ----------
    def _hit_node(self, sx: int, sy: int) -> Optional[str]:
        for nid, n in self._v_nodes.items():
            nx, ny = self._world_to_screen(n.x, n.y)
            x0, y0 = nx - n.w // 2, ny - n.h // 2
            if x0 <= sx <= x0 + n.w - 1 and y0 <= sy <= y0 + n.h - 1:
                return nid
        return None

    def on_mouse_down(self, event: MouseDown) -> None:
        sx, sy = event.x, event.y
        nid = self._hit_node(sx, sy)
        if nid:
            self._selected = nid
            self._dragging = nid
            self._drag_origin = (sx, sy)
            n = self._v_nodes[nid]
            self._world_origin = (n.x, n.y)
            self.post_message(NodeSelected(nid))
            # Refresh to paint selection border instantly
            self.render_now()
            self.capture_mouse()
            event.stop()
        else:
            # start panning background
            self._panning = True
            self._pan_origin = (sx, sy)
            self.capture_mouse()
            event.stop()

    def on_mouse_up(self, event: MouseUp) -> None:
        if self._dragging:
            self._dragging = None
            self._drag_origin = None
            self._world_origin = None
            self.release_mouse()
            event.stop()
        if self._panning:
            self._panning = False
            self._pan_origin = None
            self.release_mouse()
            event.stop()

    # Extra safety: emit selection on click (mouse up without drag)
    def on_click(self, event: Click) -> None:  # type: ignore[override]
        try:
            nid = self._hit_node(event.x, event.y)
            if nid:
                self._selected = nid
                self.post_message(NodeSelected(nid))
                self.render_now()
                event.stop()
        except Exception:
            pass

    # (DoubleClick event not available in this Textual version; single click shows details)

    def on_mouse_move(self, event: MouseMove) -> None:
        if self._dragging and self._drag_origin and self._world_origin:
            sx, sy = event.x, event.y
            dx = (sx - self._drag_origin[0]) / max(1.0, self.scale)
            dy = (sy - self._drag_origin[1]) / max(1.0, self.scale)
            n = self._v_nodes[self._dragging]
            n.x = self._world_origin[0] + dx
            n.y = self._world_origin[1] + dy
            # snap subtly
            n.x = round(n.x, 1)
            n.y = round(n.y, 1)
            self.render_now()
            event.stop()
        elif self._panning and self._pan_origin:
            sx, sy = event.x, event.y
            dx = sx - self._pan_origin[0]
            dy = sy - self._pan_origin[1]
            self._pan_origin = (sx, sy)
            self.pan(dx, dy)
            event.stop()

    # Note: deliberately avoid intercepting mouse wheel events here so that
    # Select dropdowns and other scrollable menus keep working above the canvas.

    def on_resize(self, event: Resize) -> None:
        self.render_now()
