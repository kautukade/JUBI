import { NextRequest, NextResponse } from "next/server";
import { getRequestUser } from "@/lib/server/auth";
import { callLocalAgent } from "@/lib/server/localAgent";
import { jsonError, readJson } from "@/lib/server/http";

export const runtime = "nodejs";
export const maxDuration = 300;
export async function POST(request: NextRequest) {
  const user = await getRequestUser(request);
  if (!user) return jsonError("Please sign in.", 401);
  if (user.role !== "admin") return jsonError("Owner access required.", 403);
  try {
    const identity = await callLocalAgent<{ enabled?: boolean; verified?: boolean }>("/v7/identity/status");
    if (identity.enabled && !identity.verified) return jsonError("Owner face or Windows Hello verification required.", 423);
  } catch (error) {
    return jsonError(error instanceof Error ? error.message : "Owner identity service unavailable.", 503);
  }
  const body = await readJson<Record<string, unknown>>(request);
  if (!body?.command) return jsonError("Command is required.");
  try { return NextResponse.json(await callLocalAgent("/plan", { method: "POST", body: JSON.stringify(body) })); }
  catch (error) { return jsonError(error instanceof Error ? error.message : "Planning failed", 500); }
}
