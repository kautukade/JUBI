# Jubi Windows Installation

## Recommended: one-click installer

For normal Windows users, use `Jubi-Setup.exe`. Manual Python/Ollama/model setup is not the intended path.

When the installer is double-clicked it requests the normal Windows UAC permission once and then performs the supported setup automatically:

1. checks 64-bit Windows and the Jubi installer payload;
2. detects Python 3.11 and installs it automatically when missing;
3. detects Ollama and installs/starts it automatically when missing;
4. attempts to provision optional Git and Node.js tooling through Windows Package Manager when they are missing;
5. pulls the required local Ollama models from `config/production.json` when they are not already installed;
6. creates/repairs Jubi's private Python runtime;
7. runs Jubi acceptance/certification checks;
8. creates the Start-menu/Desktop `Jubi` shortcut that launches `Jubi.exe`;
9. registers `Jubi Background Agent` in Windows Task Scheduler for the signed-in user, with highest privileges and automatic restart on failure;
10. starts the localhost Jubi server and keeps it supervised in the background.

Installer and background logs are written under the Jubi install/log directory and `%LOCALAPPDATA%\Jubi\logs` so known prerequisite/runtime failures can be diagnosed and retried automatically.

## Automatic updates

Every successful push to the repository `main` branch builds a fresh `Jubi-Setup.exe`. The build publishes the installer plus `Jubi-Update-Manifest.json` and `SHA256.txt` to the `continuous` GitHub release.

The installed background supervisor checks that release periodically. It will only apply an update when:

- the release is from the configured `kautukade/JUBI` repository;
- the update manifest identifies the same repository;
- the remote build is newer than the installed build identity;
- the downloaded `Jubi-Setup.exe` SHA-256 exactly matches the manifest.

The update installer then runs silently in `/UPDATE` mode, repairs requirements if needed, replaces the application files, re-registers the background task and reloads Jubi. Reinstalling manually after every repository update is not required.

Set the user environment variable `JUBI_AUTO_UPDATE=0` to disable automatic application of continuous updates.

## Background behavior

`Jubi Background Agent` starts after the configured Windows user signs in. It keeps the localhost server alive, restarts it after unexpected failures, performs bounded fast self-heal checks, and checks for verified updates. This design keeps per-user DPAPI provider credentials in the same Windows user context instead of moving them into a SYSTEM service account.

The dashboard remains localhost-only at:

`http://127.0.0.1:8877`

## Source/developer installation

Developers may still clone the canonical repository:

`git clone --recurse-submodules https://github.com/kautukade/JUBI.git`

Then run `START_JUBI.bat` or `python -m jubi.server`. Source-mode execution is a developer workflow; the packaged one-click installer is the normal end-user path.

## Local-first AI

Jubi uses local Ollama models for the production baseline and can optionally use OpenRouter, NVIDIA NIM, or Hugging Face through the Provider Manager. `Local Only` remains the fully local mode.

## Security model

Jubi does not give an LLM unrestricted Ring-0 or arbitrary shell access. Windows actions go through typed broker actions, approvals, scoped resources, and signed/audited receipts. Automatic updates are restricted to the canonical public JUBI release and verified by SHA-256 before execution.

## Compatibility note

Some internal filenames and paths still contain `SARUS` because they are compatibility-sensitive migration surfaces. New user-facing launch, repository, dashboard, installer and update instructions use Jubi names.