# SARUS

SARUS is a local Windows multi-agent AI workspace that unifies the project core with pinned external source integrations and a custom SARA Windows assistant source.

## Install

1. Download or clone this repository.
2. If cloning with Git, use submodules: `git clone --recurse-submodules https://github.com/kautukade/SARUS.git`
3. On Windows, run `INSTALL-SARUS.bat`.
4. After installation, use `START_SARUS.bat` or the generated SARUS launcher.

## Source layout

- `sarus/` — SARUS runtime/core
- `sources/` — SARA direct source plus pinned external source integrations
- `installer/` — Windows installer engine
- `config/` — model/source/policy configuration
- `tests/` — integration tests
- `docs/` — architecture and test documentation

Public diagnostic exclusions are limited to SARA support-report ZIPs and a local installer log that can contain machine-specific data.
