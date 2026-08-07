import { NextRequest, NextResponse } from "next/server";
import { getRequestUser, publicUser } from "@/lib/server/auth";
import { readDb } from "@/lib/server/db";
import { jsonError } from "@/lib/server/http";

export async function GET(request: NextRequest) {
  const user = await getRequestUser(request);
  if (!user || user.role !== "admin") return jsonError("Administrator access required.", 403);
  const db = await readDb();
  return NextResponse.json({
    stats: {
      users: db.users.length,
      activeUsers: db.users.filter((item) => item.active).length,
      messages: db.messages.length,
      pendingTasks: db.tasks.filter((item) => item.status === "pending").length,
      devices: db.devices.filter((item) => item.active).length,
      queuedCommands: db.commands.filter((item) => item.status === "queued").length,
    },
    users: db.users.map(publicUser),
    recentAudit: db.audit.slice(0, 50),
    configuration: {
      openaiConfigured: Boolean(process.env.OPENAI_API_KEY),
      ollamaConfigured: Boolean(process.env.OLLAMA_MODEL || process.env.OLLAMA_BASE_URL),
      ollamaModel: process.env.OLLAMA_MODEL || "llama3",
      dataDirectory: process.env.SARA_DATA_DIR ? "Custom persistent directory" : "Project data directory",
      productionSecretConfigured: Boolean(process.env.SARA_SESSION_SECRET),
    },
  });
}
