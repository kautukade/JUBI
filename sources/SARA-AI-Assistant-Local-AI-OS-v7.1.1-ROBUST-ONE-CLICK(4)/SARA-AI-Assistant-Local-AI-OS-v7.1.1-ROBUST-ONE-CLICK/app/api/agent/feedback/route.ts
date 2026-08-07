import { NextRequest, NextResponse } from "next/server";
import { getRequestUser } from "@/lib/server/auth";
import { callLocalAgent } from "@/lib/server/localAgent";
import { jsonError, readJson } from "@/lib/server/http";

export const runtime = "nodejs";
export async function POST(request: NextRequest) {
  if (!await getRequestUser(request)) return jsonError("Please sign in.", 401);
  const body = await readJson<Record<string, unknown>>(request);
  try { return NextResponse.json(await callLocalAgent("/feedback", { method: "POST", body: JSON.stringify(body || {}) })); }
  catch (error) { return jsonError(error instanceof Error ? error.message : "Feedback failed", 500); }
}
