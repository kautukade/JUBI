# Jubi architecture

`Dashboard -> Jubi Core -> Brain / Provider Manager -> Planner / Supervisor -> Tool Policy -> Source Adapters / Knowledge / Research -> Evidence / Events / Receipts -> Verifier`

Jubi preserves selected SARUS-era internals as a compatibility layer where blind renaming would risk installer, driver, persistence, or source-adapter behavior. User-facing product identity, dashboard, Python entry points, repository, installer artifact, and launcher are Jubi.

## Execution modes
- Local reasoning: installed Ollama models through Jubi Brain.
- Provider routing: optional OpenRouter, NVIDIA NIM, or Hugging Face under Provider Manager privacy/mode policy.
- Knowledge: local semantic embeddings, RAG, and experience memory.
- Supervisor: planner -> specialist reasoning -> reviewer workflows.
- Tool execution: typed, allowlisted Windows/workspace actions through policy and broker controls.
- Research: public web content is treated as untrusted evidence and cannot directly invoke privileged tools.
- Authorized LAN: passive discovery plus explicitly registered devices/services only.
- Approval: configured destructive, privileged, or otherwise high-impact actions require the appropriate approval/proof path.

## Trusted completion
An agent's natural-language claim is not completion evidence. Jubi records events, task state, routing history, approvals, receipts, and other machine evidence so actions can be verified independently of model prose.

## Compatibility boundary
The canonical repository is `https://github.com/kautukade/JUBI`. Internal `sarus/` source paths, `data/sarus.db`, selected installer helper names, and the `SarusRing0` ABI remain compatibility surfaces until they can be migrated with physical Windows validation.