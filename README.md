# SARUS

SARUS is a local-first multi-agent operating platform that unifies the 10 supplied source repositories behind one dashboard, one Ollama model router, a shared execution/event pipeline, local memory, approvals and trusted execution receipts.

## Source repositories

SARA, Hermes Agent, ECC, Agency Agents, Awesome LLM Apps, Second Brain Skills, Superpowers, Fable OS, CAI and Autoresearch are preserved under `sources/` and indexed in the capability registry.

## Runtime

- Dashboard/API: `127.0.0.1:8877`
- Local inference: Ollama `127.0.0.1:11434`
- Windows action bridge: SARA v7 local API (normally `127.0.0.1:8765`)
- Database: local SQLite under `data/`
- Audit: hash-chained SARUS receipts
- Cloud models: excluded from automatic role routing

## Start after installation

Double-click `SARUS.exe` or the desktop shortcut.

## Developer checks

- `python -m unittest -v tests.integration_test`
- `python -m sarus.acceptance --full` on the target Windows laptop

See `docs/INSTALLATION.md`, `docs/TEST_REPORT.md`, and `docs/ALL_SOURCE_FEATURES.md`.
