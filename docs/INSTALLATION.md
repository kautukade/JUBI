# Jubi Windows Installation

## GitHub clone installation

1. Install Git for Windows, Python 3.11+, Ollama, and the required local models.
2. Clone the canonical repository:

   `git clone --recurse-submodules https://github.com/kautukade/JUBI.git`

3. Open the `JUBI` folder.
4. Preferred source launch: run `START_JUBI.bat` or `python -m jubi.server`.
5. Preferred acceptance test: run `RUN_JUBI_ACCEPTANCE.bat` or `python -m jubi.acceptance --full`.
6. Dashboard: `http://127.0.0.1:8877`.
7. For packaged deployment, use the canonical `Jubi-Setup.exe` artifact and installed `Jubi.exe` launcher.

## Local-first AI

Jubi uses installed local Ollama models for the production baseline and can optionally use OpenRouter, NVIDIA NIM, or Hugging Face through the Provider Manager. `Local Only` remains the safest fully local mode.

## Security model

Jubi does not give an LLM unrestricted Ring-0 or arbitrary shell access. Windows actions go through typed broker actions, approvals, scoped resources, and signed/audited receipts. The controlled Ring0 compatibility bridge remains limited to the narrow supported status/ping interface.

## Compatibility note

Some internal filenames and paths still contain `SARUS` because they are compatibility-sensitive migration surfaces. New user-facing launch, repository, dashboard, and installer instructions should use the Jubi names above.