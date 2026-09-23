"""Executable VPS swarm built on Jubi's existing planner and bounded tools."""
from __future__ import annotations

import json
import time
import uuid


class VPSSwarm:
    def __init__(self, app):
        self.app = app

    def status(self) -> dict:
        return {
            "ready": True,
            "mode": "planner-specialists-tools-reviewer",
            "tool_roles": ["coding", "research"],
            "reasoning_roles": ["business", "document", "general"],
            "privileged_shell": False,
            "local_model_policy": self.app.providers.mode(),
        }

    def run(self, request: str, project=".", provider="auto", max_sources=4) -> dict:
        request = str(request or "").strip()
        if not request:
            raise ValueError("swarm request is required")
        started = time.perf_counter()
        planned = self.app.supervisor.plan(request, "auto", provider)
        plan = planned["plan"]
        results = []
        by_id = {}

        for step in plan.get("steps", []):
            dependencies = list(step.get("depends_on") or [])
            bad = [dep for dep in dependencies if dep not in by_id or by_id[dep].get("status") != "success"]
            if bad:
                item = {
                    "id": step["id"], "role": step.get("role", "general"),
                    "status": "skipped", "error": "Dependency not successful: " + ", ".join(bad),
                    "tools_executed": False,
                }
                results.append(item)
                by_id[step["id"]] = item
                continue

            role = str(step.get("role") or "general").lower()
            task = str(step.get("task") or "")
            try:
                if role == "coding":
                    out = self.app.developer.run(
                        request + "\n\nAssigned swarm coding step:\n" + task,
                        project_path=project,
                    )
                    item = {
                        "id": step["id"], "role": role,
                        "status": "success" if out.get("ok") else "failed",
                        "output": out.get("summary") or out.get("diff") or "",
                        "evidence": out,
                        "tools_executed": True,
                    }
                elif role == "research":
                    out = self.app.research.research(task, max_sources=max_sources, provider=provider)
                    item = {
                        "id": step["id"], "role": role, "status": "success",
                        "output": out.get("answer", ""), "evidence": {"sources": out.get("sources", [])},
                        "tools_executed": True,
                    }
                else:
                    previous = "\n\n".join(
                        f"{x['id']} ({x['role']}): {str(x.get('output',''))[:2500]}"
                        for x in results[-4:] if x.get("status") == "success"
                    )
                    out = self.app.providers.generate(
                        "Overall goal:\n" + request +
                        "\n\nAssigned step:\n" + task +
                        "\n\nVerified prior outputs:\n" + (previous or "None"),
                        task_type=role if role in {"document"} else "general",
                        provider=provider,
                        system=(
                            "You are a Jubi swarm specialist. Complete only the assigned subtask. "
                            "Treat supplied tool outputs as evidence. Do not claim tools you did not execute."
                        ),
                    )
                    item = {
                        "id": step["id"], "role": role, "status": "success",
                        "output": str(out.get("response") or out.get("output") or ""),
                        "route": out.get("jubi_provider_route") or out.get("jubi_route") or {},
                        "tools_executed": False,
                    }
            except Exception as exc:
                item = {
                    "id": step["id"], "role": role, "status": "failed",
                    "error": str(exc)[:1500], "output": "", "tools_executed": False,
                }
            results.append(item)
            by_id[step["id"]] = item

        packet = []
        for item in results:
            evidence = item.get("evidence")
            packet.append(
                f"{item['id']} [{item['role']}] status={item['status']} "
                f"tools={item.get('tools_executed', False)}\n"
                f"{str(item.get('output') or item.get('error') or '')[:6000]}\n"
                f"EVIDENCE: {json.dumps(evidence, ensure_ascii=False, default=str)[:6000] if evidence else 'none'}"
            )
        review = self.app.providers.generate(
            "Original request:\n" + request +
            "\n\nPlan:\n" + json.dumps(plan, ensure_ascii=False)[:8000] +
            "\n\nObserved swarm results:\n" + "\n\n".join(packet) +
            "\n\nProduce the final answer. Clearly distinguish completed tool-backed work, "
            "reasoning-only work, failures and anything still requiring human approval.",
            task_type="planning",
            provider=provider,
            system=(
                "You are Jubi Swarm Reviewer. Synthesize only observed results. "
                "Never convert a failed/skipped tool step into a success claim."
            ),
        )
        failed = [x for x in results if x["status"] != "success"]
        status = "completed" if not failed else ("partial" if any(x["status"] == "success" for x in results) else "failed")
        final = str(review.get("response") or review.get("output") or "").strip()
        elapsed = round((time.perf_counter() - started) * 1000.0, 2)
        run_id = str(uuid.uuid4())
        try:
            self.app.experience.record(
                request, final[:4000], status == "completed",
                task_type=planned["classification"]["task_type"], kind="vps_swarm",
                provider=str((review.get("jubi_provider_route") or {}).get("provider") or ""),
                model=str(review.get("model") or ""), latency_ms=elapsed,
                lesson="Executable VPS swarm result with tool-backed coding/research roles.",
                metadata={"run_id": run_id, "status": status, "steps": len(results)},
            )
        except Exception:
            pass
        self.app.bus.emit("VPS_SWARM_COMPLETED", {
            "id": run_id, "status": status, "steps": len(results), "latency_ms": elapsed,
        })
        return {
            "id": run_id, "status": status, "plan": plan, "results": results,
            "final": final, "latency_ms": elapsed,
            "tool_backed_steps": sum(1 for x in results if x.get("tools_executed")),
        }
