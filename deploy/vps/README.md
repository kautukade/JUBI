# Jubi on a Linux VPS

This profile runs the Jubi core as a non-root systemd service while preserving
the project's local-only inference boundary.

## Security model

Jubi continues to listen only on `127.0.0.1`. Do **not** open port 8877 in an
AWS Security Group or host firewall. Remote dashboard access should use an SSH
tunnel:

```bash
ssh -N -L 8877:127.0.0.1:8877 ubuntu@YOUR_VPS_IP
```

Then open `http://127.0.0.1:8877` on your own computer. This keeps Jubi's
session bootstrap and privileged APIs off the public Internet.

For a permanent public hostname, place a separately authenticated TLS reverse
proxy in front of the loopback service. The reverse proxy must authenticate
before forwarding **any** path, including `/api/session`. Do not simply change
`JUBI_HOST` to `0.0.0.0`.

## AWS baseline

For the first month of testing, use Ubuntu 24.04 LTS and keep only SSH (22)
reachable from your own IP. A CPU instance can run the dashboard/orchestration;
local Ollama model speed depends heavily on RAM/CPU. A GPU instance may run
Ollama on the same host, but Jubi still connects to it over loopback.

Recommended storage is at least 80 GiB when the large bundled source trees and
several Ollama models are retained. Monitor both EBS usage and model storage.

## Install

Clone the branch/release you intend to test, then:

```bash
sudo bash deploy/vps/install.sh --with-hermes --with-browser --start
```

For a lighter core-only deployment, omit the optional flags. The full agent profile installs Hermes and the read-only Playwright/Chromium browser, but model acquisition remains a separate explicit action.

Recommended compact model profile:

```bash
sudo /opt/jubi/deploy/vps/provision-models.sh --recommended
```

Or, after Ollama is already installed and reachable on loopback, use the full profile wrapper:

```bash
sudo bash deploy/vps/full-profile.sh
```

The full profile requires Hermes, Chromium, general/coding, vision and embedding roles before verification can pass.

The installer:
- creates a dedicated `jubi` service account;
- installs only OS runtime prerequisites on apt-based Linux;
- copies Jubi to `/opt/jubi`;
- creates a private Python virtual environment;
- creates persistent `data`, `workspace`, and `logs` directories;
- installs a hardened systemd service;
- keeps Jubi on loopback;
- does **not** download an AI model.

Ollama and each model remain explicit user choices. Once Ollama is running on
`127.0.0.1:11434`, verify:

```bash
sudo /opt/jubi/deploy/vps/verify.sh
```

## Operations

```bash
sudo systemctl status jubi
sudo systemctl restart jubi
sudo journalctl -u jubi -f
sudo /opt/jubi/deploy/vps/healthcheck.sh
sudo /opt/jubi/deploy/vps/verify.sh
sudo /opt/jubi/deploy/vps/backup.sh
```

Runtime configuration is in `/etc/jubi/jubi.env`. Keep
`JUBI_HOST=127.0.0.1`. After changing the environment file, restart the
service.

## What works on a VPS

The Linux profile is intended for the Jubi core: dashboard, persistent chat,
Brain routing, local Ollama, memory/RAG, vision, public research, AI Council,
executable VPS swarm, bounded autonomous coding with verification/review, Hermes
pilot processes, read-only JavaScript browser rendering, automations, receipts,
passive Linux neighbour-cache support, and workspace file/Git operations.

Windows-specific desktop features remain unavailable on a Linux VPS: Ring0,
Windows service/process controls, Windows application launch, DPAPI, native SARA
desktop control, camera/microphone desktop interaction, and the Windows EXE
installer.

A later split architecture can pair this VPS core with a separately authorized
Windows worker. That worker should remain an explicit, authenticated capability
boundary rather than turning the VPS into unrestricted remote desktop control.

## Backups

`backup.sh` uses SQLite's online backup API for `data/sarus.db` and packages
the rest of `data/` plus `workspace/`. It intentionally excludes
`/etc/jubi/jubi.env`, credentials, and Ollama model files.

Restore should be performed into a stopped service and reviewed manually during
this testing phase; automated destructive restore is intentionally not shipped.
