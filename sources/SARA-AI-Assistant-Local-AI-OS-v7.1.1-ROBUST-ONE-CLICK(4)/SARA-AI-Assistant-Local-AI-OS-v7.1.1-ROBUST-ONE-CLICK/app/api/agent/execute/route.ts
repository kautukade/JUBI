import { NextRequest, NextResponse } from "next/server";
import { getRequestUser } from "@/lib/server/auth";
import { callLocalAgent } from "@/lib/server/localAgent";
import { addAudit, mutateDb } from "@/lib/server/db";
import { jsonError, readJson } from "@/lib/server/http";

export const runtime = "nodejs";
export const maxDuration = 300;
export async function POST(request: NextRequest) {
  const user = await getRequestUser(request);
  if (!user) return jsonError("Please sign in.", 401);
  if (user.role !== "admin") return jsonError("Owner access required.", 403);
  const body = await readJson<Record<string, unknown>>(request);
  try {
    const result = await callLocalAgent<Record<string, unknown>>("/execute", { method: "POST", body: JSON.stringify(body || {}) });
    await mutateDb((db) => addAudit(db, "computer_agent_execute", `Plan ${String(body?.plan_id || "unknown")}: ${String(result.status || "unknown")}`, user.id));
    return NextResponse.json(result);
  } catch (error) { return jsonError(error instanceof Error ? error.message : "Execution failed", 500); }
}
