#!/usr/bin/env python3
"""
CTR Experiment Runner - Command line interface for CAI-CTR integration.

This thin wrapper delegates to `cai.ctr.experiment.main()` which exposes
an argparse-powered interface. It supports processing a single JSONL log
file or a directory of JSONL logs, plus optional flags for CTF mode and
attack/defense rate grids.

Examples
========
Run CTR over one log file:
    cai-ctr --input_log ~/.cai/logs/session_2025-09-04.jsonl

Run CTR over all logs in a folder:
    cai-ctr --input_log ~/.cai/logs/

Specify rates and output directory:
    cai-ctr --input_log ./logs --attack_rate 1,2,3 --defense_rate 0,1 \
            --output_dir /tmp/cai/ctr

Show help:
    cai-ctr -h
"""

import sys
import os
import asyncio

# Add src directory to Python path for editable installs and direct runs
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src'))

from cai.ctr.experiment import main as experiment_main


def main() -> None:
    """Synchronous entrypoint for the `cai-ctr` console script."""
    asyncio.run(experiment_main())


if __name__ == "__main__":
    main()
