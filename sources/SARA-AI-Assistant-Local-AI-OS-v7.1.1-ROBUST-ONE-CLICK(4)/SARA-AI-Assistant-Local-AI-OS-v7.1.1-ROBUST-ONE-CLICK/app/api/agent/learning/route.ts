import { NextRequest, NextResponse } from "next/server";
import { getRequestUser } from "@/lib/server/auth";
import { callLocalAgent } from "@/lib/server/localAgent";
import { jsonError } from "@/lib/server/http";

export const runtime = "nodejs";
export async function GET(request: NextRequest) {
  if (!await getRequestUser(request)) return jsonError("Please sign in.", 401);
  try { return NextResponse.json(await callLocalAgent("/learning")); }
  catch (error) { return jsonError(error instanceof Error ? error.message : "Learning data unavailable", 503); }
}
