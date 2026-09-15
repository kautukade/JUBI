"""Explicit real-model acceptance controller. Creates only a disposable project."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOAL = 'Inspect this small local project, identify a failing test, repair the code, run the test again, review the change and verify that the acceptance criteria pass.'
PROJECT_TESTS = '''import unittest
from pricing import subtotal
class PricingTests(unittest.TestCase):
    def test_multiple_items(self): self.assertEqual(subtotal(12, 4), 48)
    def test_zero_quantity(self): self.assertEqual(subtotal(5, 0), 0)
    def test_single_item(self): self.assertEqual(subtotal(7, 1), 7)
'''
HARNESS = '''import sys, unittest
from pathlib import Path
project = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(project))
if sys.argv[2] == 'test':
    suite = unittest.defaultTestLoader.discover(str(project), pattern='test_pricing.py')
else:
    from pricing import subtotal
    class IndependentAcceptance(unittest.TestCase):
        def test_unseen_cases(self):
            for price, quantity in [(0, 9), (3, 7), (19, 11), (-2, 3), (2.5, 4)]:
                with self.subTest(price=price, quantity=quantity):
                    self.assertEqual(subtotal(price, quantity), price * quantity)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(IndependentAcceptance)
result = unittest.TextTestRunner(verbosity=2).run(suite)
raise SystemExit(0 if result.wasSuccessful() else 1)
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--endpoint', default='http://127.0.0.1:11500')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    home = ROOT / 'data/development-acceptance' / uuid.uuid4().hex
    project = home / 'project'
    project.mkdir(parents=True)
    (project / 'README.md').write_text('subtotal(unit_price, quantity) must return unit_price multiplied by quantity. Inspect and fix pricing.py. Do not modify tests. The approved capability permits only this arithmetic function.\n')
    (project / 'pricing.py').write_text('def subtotal(unit_price, quantity):\n    return unit_price + quantity\n')
    (project / 'test_pricing.py').write_text(PROJECT_TESTS)
    (home / 'run_tests.py').write_text(HARNESS)
    request = {'root': str(ROOT), 'home': str(home), 'endpoint': args.endpoint, 'goal': GOAL}
    env = {k: v for k, v in os.environ.items() if k.upper() in
           {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATHEXT', 'PATH', 'TEMP', 'TMP'}}
    env.update(HERMES_HOME=str(home), HOME=str(home), USERPROFILE=str(home),
               LOCALAPPDATA=str(home), APPDATA=str(home), PYTHONPATH=str(ROOT),
               PYTHONUTF8='1', PYTHONIOENCODING='utf-8', PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    print(json.dumps({'workspace': str(project), 'cancellation_file': str(home / 'CANCEL')}), flush=True)
    process = subprocess.Popen([sys.executable, '-m', 'sarus.integrations.development_acceptance'],
                               cwd=home, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace',
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    job = None
    if os.name == 'nt':
        # Close the job on timeout/cancellation to terminate test subprocesses too.
        import win32api, win32job
        job = win32job.CreateJobObject(None, '')
        limits = win32job.QueryInformationJobObject(job, win32job.JobObjectExtendedLimitInformation)
        limits['BasicLimitInformation']['LimitFlags'] = win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        win32job.SetInformationJobObject(job, win32job.JobObjectExtendedLimitInformation, limits)
        win32job.AssignProcessToJobObject(job, process._handle)
    started, payload = time.monotonic(), json.dumps(request)
    try:
        while True:
            try:
                stdout, stderr = process.communicate(input=payload, timeout=1)
                break
            except subprocess.TimeoutExpired:
                payload = None
                if time.monotonic() - started >= 600 or (home / 'CANCEL').exists():
                    if job is not None:
                        win32api.CloseHandle(job)
                        job = None
                    process.kill()
                    stdout, stderr = process.communicate()
                    break
    finally:
        if job is not None:
            win32api.CloseHandle(job)
    (home / 'stdout.log').write_text(stdout, encoding='utf-8')
    (home / 'stderr.log').write_text(stderr, encoding='utf-8')
    evidence = home / 'evidence.json'
    result = json.loads(evidence.read_text(encoding='utf-8')) if evidence.exists() else {
        'status': 'FAILED', 'error': 'Acceptance process did not finish', 'diagnostic': stderr[-5000:]}
    if process.returncode and result.get('task_id'):
        os.environ['HERMES_HOME'] = str(home)
        sources = json.loads((ROOT / 'config/sources.json').read_text(encoding='utf-8'))
        sys.path.insert(0, str(ROOT / 'sources' / sources['hermes']))
        from hermes_cli import kanban_db
        board = kanban_db.connect(home / 'kanban.db')
        try:
            task = kanban_db.get_task(board, result['task_id'])
            kanban_db.block_task(board, task.id, reason='Controller cancellation or process failure', expected_run_id=task.current_run_id)
            result['kanban_status'] = kanban_db.get_task(board, task.id).status
        finally:
            board.close()
        result.update(status='FAILED', cancelled=(home / 'CANCEL').exists())
    result.update(process_exit_code=process.returncode, workspace=str(project),
                  duration_seconds=round(time.monotonic() - started, 2))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'evidence': str(args.output)}), flush=True)
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
