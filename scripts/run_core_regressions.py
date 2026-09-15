"""Run every core unittest file in a disposable first-party checkout.

Source trees are read-only providers. No production state, credentials or models
are used. Real-model acceptance is a separate, explicit evidence run.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='jubi-regression-'))
    for folder in ('jubi', 'sarus', 'tests', 'scripts', 'installer', 'driver', '.github', 'config', 'docs'):
        shutil.copytree(ROOT / folder, scratch / folder,
                        ignore=shutil.ignore_patterns('__pycache__', 'audit-2026-09-12', 'integration-*'))
    for path in ROOT.iterdir():
        if path.is_file():
            shutil.copy2(path, scratch / path.name)
    sources = json.loads((ROOT / 'config/sources.json').read_text(encoding='utf-8'))
    absolute_sources = {key: str(ROOT / 'sources' / value) for key, value in sources.items()}
    env = {k: v for k, v in os.environ.items() if k.upper() in
           {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'PATHEXT', 'TEMP', 'TMP',
            'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'}}
    env.update(LOCALAPPDATA=str(scratch / 'localappdata'), APPDATA=str(scratch / 'appdata'),
               USERPROFILE=str(scratch / 'profile'), PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1',
               PYTHONPATH=str(scratch), JUBI_OLLAMA_URL='http://127.0.0.1:9',
               SARUS_BROKER_APPROVAL_SECRET='regression-only-broker-secret-not-a-production-key',
               SARUS_RECEIPT_SIGNING_KEY_FILE=str(scratch / 'receipt.key'),
               NODE_PATH=str(ROOT / '.venv/js/node_modules'))
    summary = {'fixture_root': str(scratch), 'python': sys.executable, 'suites': [],
               'counts': {'passed': 0, 'failed': 0, 'skipped': 0, 'suite_errors': 0}}
    scripts = sorted((scratch / 'tests').glob('*_test.py'))
    commands = [(p.name, [sys.executable, str(p)]) for p in scripts]
    commands.append(('dashboard_runtime_test.cjs', [sys.executable, str(scratch / 'tests/run_dashboard_runtime.py')]))
    for name, command in commands:
        cfg = sources if name in {'jubi_http_functional_test.py', 'dashboard_runtime_test.cjs'} else absolute_sources
        (scratch / 'config/sources.json').write_text(json.dumps(cfg), encoding='utf-8')
        started = time.monotonic()
        row = {'suite': name, 'command': command}
        try:
            cp = subprocess.run(command, cwd=scratch, env=env, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=210)
            text = cp.stdout + cp.stderr
            row['exit_code'] = cp.returncode
        except subprocess.TimeoutExpired as exc:
            text = str(exc.stdout or '') + '\n' + str(exc.stderr or '')
            row['exit_code'] = 'timeout'
        row['seconds'] = round(time.monotonic() - started, 2)
        ran = re.findall(r'Ran (\d+) tests?', text)
        if ran:
            total = int(ran[-1])
            failure = re.findall(r'failures=(\d+)', text)
            error = re.findall(r'errors=(\d+)', text)
            skip = re.findall(r'skipped=(\d+)', text)
            failed = (int(failure[-1]) if failure else 0) + (int(error[-1]) if error else 0)
            skipped = int(skip[-1]) if skip else 0
            row.update(tests=total, passed=total - failed - skipped, failed=failed, skipped=skipped)
            for key in ('passed', 'failed', 'skipped'):
                summary['counts'][key] += row[key]
        elif name.endswith('.py'):
            summary['counts']['suite_errors'] += 1
        (out / (name + '.log')).write_text(text, encoding='utf-8')
        summary['suites'].append(row)
        (out / 'results.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(json.dumps(row), flush=True)
    print(json.dumps(summary['counts']), flush=True)
    return int(any(x['exit_code'] != 0 for x in summary['suites']))


if __name__ == '__main__':
    raise SystemExit(main())
