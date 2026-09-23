"""Installer entry point; does not download models or start optional services."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sarus.core.hardware import profile_hardware
from sarus.core.models import OllamaRouter


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ollama-url', required=True)
    args = parser.parse_args()
    profile = profile_hardware(args.root, OllamaRouter(args.root / 'config/models.json', args.ollama_url))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix('.tmp')
    temporary.write_text(json.dumps(profile, indent=2), encoding='utf-8')
    temporary.replace(args.output)


if __name__ == '__main__':
    main()
