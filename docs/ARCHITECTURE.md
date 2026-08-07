# SARUS architecture

`Dashboard -> SARUS Core -> Orchestrator -> Agent Router / Model Router / Policy -> Source Adapters -> Evidence/Event Bus -> Verifier`

All ten repositories are retained beneath `sources/`. Public upstream projects are pinned to exact commits through Git submodules; custom SARA source is bundled directly. The adapter boundary prevents dependency collisions while preserving discoverability and allows each source to be upgraded independently.

## Execution modes
- Native: safe local SARUS-core capabilities.
- Adapter: call a repository through its supported entry point.
- Plugin: discover a skill/tool and execute through Hermes-compatible routing.
- Isolated: CAI active-security and Autoresearch mutation experiments.
- Approval: external send, destructive action, privileged change, credentials, production deployment.

## Trusted completion
An agent's natural-language claim is not completion evidence. SARUS stores events/action receipts and expects machine evidence for actions before final verification.
