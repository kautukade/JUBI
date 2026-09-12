# JUBI External Integrations

This document defines the JUBI-native integration plan for the next external capability sources.

## Targets

- https://github.com/sunblaze-ucb/exploitgym
- https://github.com/experientiallabs/experiential
- https://github.com/sunblaze-ucb/cybergym
- https://github.com/sunblaze-ucb/cybergym-e2e
- https://huggingface.co/

## Integration principles

1. External repositories are integrated through adapters/capability contracts rather than merged blindly into the JUBI core.
2. Every integration must declare dependencies, platform requirements, health checks, permissions, data paths and execution boundaries.
3. JUBI keeps its existing default-deny approval and execution policies.
4. Cybersecurity benchmark integrations run only in explicitly configured isolated local lab environments. They are not exposed as unrestricted real-world exploitation tools and must never target arbitrary external systems.
5. Hugging Face access is treated as a model/dataset/provider integration and follows JUBI privacy, credential and provider policies.

## Experiential

Target role: advanced model gateway/router for JUBI agents.

Planned integration:
- OpenAI-compatible local gateway adapter.
- Optional hosted gateway adapter.
- Provider/model aliases exposed to JUBI Provider Manager.
- Per-agent identities and budgets.
- Latency/cost/quality telemetry import into JUBI Brain routing history.
- Optional router optimization from JUBI traces after explicit enablement.
- Loopback-only local gateway by default.

## Hugging Face

Target role: model, dataset and inference ecosystem.

Planned integration:
- Hugging Face token storage through JUBI credential storage.
- Model Hub search/metadata/install workflow.
- Dataset Hub integration for approved datasets.
- Inference Providers adapter retained/expanded under Provider Manager.
- Local model download/import workflows where compatible with the selected runtime.
- Cache, disk-space and download-progress visibility.
- License/model-card metadata surfaced before installation.
- Optional benchmark dataset acquisition for JUBI labs.

## CyberGym

Target role: isolated cybersecurity evaluation lab.

Planned integration:
- Dedicated Cyber Lab dashboard.
- Docker/runtime readiness checks.
- Local-only benchmark server configuration.
- Dataset presence/size checks without automatic multi-terabyte downloads.
- Firewall/isolation state surfaced in JUBI.
- Task registry and benchmark-run metadata.
- Results/receipts imported into JUBI Experience and Evaluation history.
- No public binding and no arbitrary external targets.

## CyberGym-E2E

Target role: isolated end-to-end secure-code evaluation.

Planned integration:
- Patch-only evaluation as the default JUBI mode.
- Optional benchmark end-to-end mode only inside the isolated lab.
- Test/patch validation results surfaced to Development and Security dashboards.
- JUBI coding agents can be benchmarked against known vulnerable benchmark projects without exposing benchmark execution to normal Computer Operator workflows.

## ExploitGym

Target role: isolated advanced agent-security benchmark.

Planned integration:
- Separate lab adapter and capability namespace.
- Docker/firewall/controller readiness checks.
- Benchmark metadata and task selection.
- Agent performance evaluation and result ingestion.
- No direct bridge from benchmark exploit artifacts to normal JUBI LAN/Computer Operator execution.

## JUBI capability namespaces

Proposed namespaces:

- `gateway.experiential.*`
- `hub.huggingface.*`
- `lab.cybergym.*`
- `lab.cybergym_e2e.*`
- `lab.exploitgym.*`

## Architecture

```text
JUBI Brain / Supervisor
        |
        +-- Provider Manager
        |      +-- Ollama
        |      +-- OpenRouter
        |      +-- NVIDIA
        |      +-- Hugging Face
        |      +-- Experiential Gateway
        |
        +-- Capability Registry
        |      +-- existing sources/* adapters
        |      +-- Hugging Face Hub
        |
        +-- Isolated Labs
               +-- CyberGym
               +-- CyberGym-E2E
               +-- ExploitGym
```

## Acceptance requirements

An integration is not considered complete merely because its source exists under `sources/` or is listed in a catalog. Completion requires:

- adapter is registered;
- health/readiness is truthful;
- UI/API exposes the integration;
- required permissions are enforced;
- execution is isolated where required;
- failures are visible;
- unit/integration tests cover the adapter;
- Windows/WSL/Docker target-machine requirements are documented and validated where applicable.
