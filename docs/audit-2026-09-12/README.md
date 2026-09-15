# Jubi audit and integration review packet

**Decision pending: approve the source-to-pipeline map before implementation.** No product features were changed.

Read in this order:

1. [Source-to-pipeline map](SOURCE-TO-PIPELINE-MAP.md) — concise ownership and boundaries for every source; approval decision.
2. [Audit report and capability matrix](AUDIT-REPORT.md) — implemented/partial/broken/indexed capabilities, priority findings and test results.
3. [Unified pipeline design](PIPELINE-DESIGN.md) — Hermes ownership, local models, hardware admission, registry/state/memory, real workers, voice, installation and labs.
4. [Implementation plan](IMPLEMENTATION-PLAN.md) — concrete integration milestones and acceptance evidence; start M0/M1 only after map approval.

Detailed actual-code reviews:

- [Hermes, ECC, Superpowers and Agency](swarm-workflows.md)
- [SARA, Second Brain, Awesome, Fable and AutoResearch](capability-sources.md)
- [CAI, requested upstream benchmarks, Experiential and Hugging Face](cyber-optional-sources.md)

Evidence:

- [Recursive hashed file inventory](file-inventory.csv) and [summary/errors](inventory-summary.json)
- [First-party Python symbols/syntax](core-symbols.json) and [PowerShell syntax](powershell-syntax.json)
- [Existing test runs](test-results.json), [corrected fixture retries](test-retries.json), [raw logs](test-logs/) and [source workflow test outputs](swarm-test-results.json)
- [Four reproduced architecture gaps](probe-results.json) with [harmless audit reproductions](probes.py)
- [Host probe limitations](host-probe.json)

Audit scope is recursive inventory plus integration-critical code review and applicable safe tests, with explicit limits. No report claims that source presence, syntax checks, prompt generation or test doubles prove native end-to-end execution. The two CAI files that became unavailable are documented, not restored. The new worktree retains the same audited commit; historical test paths remain as observed.
