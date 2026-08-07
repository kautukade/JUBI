# SARUS Windows Installation

## GitHub clone installation

1. Install Git for Windows, Python 3.11+, Ollama, and required local models.
2. Clone with submodules: `git clone --recurse-submodules https://github.com/kautukade/SARUS.git`.
3. Open the SARUS folder.
4. Run `INSTALL-SARUS.bat` once the installer batch is present.
5. Run `START_SARUS.bat` or `SARUS.exe` after installation.
6. Dashboard: `http://127.0.0.1:8877`.

## Local-only AI

Cloud-tagged Ollama models are not selected by the SARUS local router. Core roles use the installed local models defined in `config/models.json`.

## Security model

SARUS does not give an LLM unrestricted Ring-0 access. Windows actions use SARA/Windows broker paths, approvals and audit receipts. CAI active tooling, Autoresearch self-modification and Fable bare-metal/kernel experiments remain isolated/optional.
