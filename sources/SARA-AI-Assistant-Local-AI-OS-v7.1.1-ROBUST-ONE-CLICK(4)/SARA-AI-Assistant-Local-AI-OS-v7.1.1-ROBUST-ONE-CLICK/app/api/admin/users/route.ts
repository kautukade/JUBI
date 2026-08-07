import { NextRequest, NextResponse } from "next/server";
import { getRequestUser, publicUser } from "@/lib/server/auth";
import { addAudit, mutateDb } from "@/lib/server/db";
import { jsonError, readJson } from "@/lib/server/http";
import type { UserRole } from "@/lib/types";

export async function PATCH(request: NextRequest) {
  const admin = await getRequestUser(request);
  if (!admin || admin.role !== "admin") return jsonError("Administrator access required.", 403);
  const body = await readJson<{ id?: string; active?: boolean; role?: UserRole }>(request);
  if (!body?.id) return jsonError("User id is required.");
  if (body.id === admin.id && body.active === false) return jsonError("You cannot disable your own account.");
  if (body.id === admin.id && body.role === "user") return jsonError("You cannot remove your own admin role.");

  const result = await mutateDb((db) => {
    const user = db.users.find((item) => item.id === body.id);
    if (!user) return null;
    if (typeof body.active === "boolean") user.active = body.active;
    if (body.role && ["admin", "user"].includes(body.role)) user.role = body.role;
    addAudit(db, "user_updated", `${user.email}: role=${user.role}, active=${user.active}`, admin.id);
    return publicUser(user);
  });
  if (!result) return jsonError("User not found.", 404);
  return NextResponse.json({ user: result });
}
