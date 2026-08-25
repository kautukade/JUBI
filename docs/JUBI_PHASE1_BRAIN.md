# Jubi Phase 1 — Advanced Local Brain

This phase adds a local-first adaptive routing layer without introducing cloud providers or expanding privileged execution.

## Implemented

- Automatic intent classification for general, coding, vision, research, planning, document and system-oriented requests.
- Complexity and privacy classification metadata.
- Dynamic ranking of models that are actually reported by Ollama.
- Automatic exclusion of embedding models from chat generation.
- Automatic exclusion of `cloud-through-ollama` models from smart routing in Phase 1.
- Explicit user model override remains supported.
- Bounded fallback across compatible installed local models when a model call fails.
- SQLite-backed model/task performance history with success/failure and latency measurements.
- Persistent routing decision metadata using prompt hashes instead of storing full prompt text in the Brain history table.
- `/api/brain`, `/api/brain/decisions`, `/api/brain/performance` and `/api/brain/route` endpoints.
- `/api/chat` now routes through the Advanced Local Brain.
- Dedicated professional `Brain & Router` dashboard page.
- Smart Auto chat mode and visible route/model information.

## Safety / privacy boundary

Phase 1 remains localhost-only. Automatic routing does not invoke NVIDIA, OpenRouter, Hugging Face, arbitrary shell execution, LAN discovery or new Ring-0 capabilities. Cloud-through-Ollama models are not automatically selected.

## Next phase

The planned next major capability is semantic memory/RAG using the installed local embedding model, while keeping current SQLite memory compatibility.
