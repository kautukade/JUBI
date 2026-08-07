import { NextRequest, NextResponse } from "next/server";
import { getRequestUser } from "@/lib/server/auth";
import { callLocalAgent } from "@/lib/server/localAgent";
import { jsonError, readJson } from "@/lib/server/http";

export const runtime = "nodejs";
export const maxDuration = 300;
export async function POST(request: NextRequest) {
  const user = await getRequestUser(request);
  if (!user) return jsonError("Please sign in.", 401);
  if (user.role !== "admin") return jsonError("Admin access required.", 403);
  const body = await readJson<Record<string, unknown>>(request);
  try { return NextResponse.json(await callLocalAgent("/improve", { method: "POST", body: JSON.stringify(body || {}) })); }
  catch (error) { return jsonError(error instanceof Error ? error.message : "Improvement analysis failed", 500); }
}
