"""Narrow, real execution capability for the disposable M1 arithmetic project.

This is not a general Python sandbox. Only a pure, bounded arithmetic function
may be edited; the test program and acceptance checks are immutable. Arbitrary
repositories remain disabled until an OS-contained executor is available.
"""
from __future__ import annotations

import ast
import difflib
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def validate_module(content: str):
    if len(content) > 4000:
        raise ValueError('Code exceeds the acceptance workspace limit')
    tree = ast.parse(content)
    if len(tree.body) != 1 or not isinstance(tree.body[0], ast.FunctionDef):
        raise PermissionError('Only the existing arithmetic function can be edited')
    function = tree.body[0]
    if (function.name != 'subtotal' or function.decorator_list or function.returns
            or getattr(function, 'type_params', []) or function.args.defaults or function.args.kw_defaults
            or function.args.vararg or function.args.kwarg or function.args.kwonlyargs
            or function.args.posonlyargs or [a.arg for a in function.args.args] != ['unit_price', 'quantity']
            or any(a.annotation for a in function.args.args)
            or len(function.body) != 1 or not isinstance(function.body[0], ast.Return)):
        raise PermissionError('Function signature and single-return structure are fixed')
    nodes = list(ast.walk(tree))
    allowed = {ast.Module, ast.FunctionDef, ast.arguments, ast.arg, ast.Return,
               ast.BinOp, ast.UnaryOp, ast.Add, ast.Sub, ast.Mult, ast.Div,
               ast.USub, ast.UAdd, ast.Name, ast.Load, ast.Constant}
    if len(nodes) > 64 or any(type(node) not in allowed for node in nodes):
        raise PermissionError('Only bounded arithmetic is allowed; imports, calls and attribute access are forbidden')
    for node in nodes:
        if isinstance(node, ast.Name) and node.id not in {'unit_price', 'quantity'}:
            raise PermissionError('Unknown arithmetic variable')
        if isinstance(node, ast.Constant) and (type(node.value) not in {int, float} or abs(node.value) > 1000000):
            raise PermissionError('Only small numeric constants are allowed')


class AcceptanceWorkspace:
    def __init__(self, project: Path, harness: Path, evidence: list, cancel_check=lambda: False):
        self.project = project.resolve()
        self.harness = harness.resolve()
        self.evidence = evidence
        self.cancel_check = cancel_check
        self.phase = 'coding'
        self.baseline = (project / 'pricing.py').read_text(encoding='utf-8')
        self.protected = {p: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (self.harness, project / 'test_pricing.py', project / 'README.md')}
        self.commands = tuple(tuple([sys.executable, '-I', '-B', str(self.harness), str(self.project), mode])
                              for mode in ('test', 'verify'))

    def _check(self):
        if self.cancel_check():
            raise RuntimeError('Acceptance task cancelled')
        for path, digest in self.protected.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise PermissionError('Protected acceptance artifact changed')

    def _file(self, path):
        if path not in {'pricing.py', 'test_pricing.py', 'README.md'}:
            raise PermissionError('Path is outside the approved acceptance capability')
        candidate = self.project / path
        if candidate.resolve().parent != self.project or candidate.is_symlink() or candidate.stat().st_nlink != 1:
            raise PermissionError('Workspace links are not permitted')
        return candidate

    def _observed_coding_tests(self):
        return [row['result']['exit_code'] for row in self.evidence
                if row.get('phase') == 'coding' and row.get('operation') == 'test'
                and row.get('status') == 'OBSERVED' and isinstance(row.get('result'), dict)
                and isinstance(row['result'].get('exit_code'), int)]

    def _observed_coding_write(self):
        return any(row.get('phase') == 'coding' and row.get('operation') == 'write'
                   and row.get('status') == 'OBSERVED' and isinstance(row.get('result'), dict)
                   for row in self.evidence)

    def execute(self, operation: str, path='', content=''):
        self._check()
        started = time.monotonic()
        row = {'operation': operation, 'phase': self.phase, 'path': path}
        try:
            allowed = {'coding': {'read', 'write', 'test', 'diff'},
                       'review': {'read', 'diff'}, 'verifying': {'verify', 'diff'}}
            if operation not in allowed.get(self.phase, set()):
                raise PermissionError('Operation is outside the active worker phase')
            if operation == 'read':
                result = {'content': self._file(path).read_text(encoding='utf-8')}
            elif operation == 'write':
                if self.phase != 'coding' or path != 'pricing.py':
                    raise PermissionError('Only the coding worker can edit pricing.py')
                tests = self._observed_coding_tests()
                if not tests or tests[-1] == 0:
                    raise PermissionError(
                        'Run the fixed test and observe it failing before editing pricing.py'
                    )
                validate_module(content)
                target = self._file(path)
                row['before_sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
                target.write_text(content, encoding='utf-8')
                row['after_sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
                result = {'written': path, 'sha256': row['after_sha256']}
            elif operation in {'test', 'verify'}:
                if operation == 'verify' and self.phase != 'verifying':
                    raise PermissionError('Acceptance verification belongs to the trusted verifier')
                validate_module(self._file('pricing.py').read_text(encoding='utf-8'))
                command = list(self.commands[operation == 'verify'])
                cp = subprocess.run(command, cwd=self.project, capture_output=True, text=True,
                                    timeout=8, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                result = {'command': command, 'cwd': str(self.project), 'exit_code': cp.returncode,
                          'stdout': cp.stdout[-6000:], 'stderr': cp.stderr[-6000:]}
            elif operation == 'diff':
                if self.phase == 'coding' and self._observed_coding_write():
                    tests = self._observed_coding_tests()
                    if not tests or tests[-1] != 0:
                        raise PermissionError(
                            'Run the fixed test and observe it passing after the edit before inspecting the diff'
                        )
                after = self._file('pricing.py').read_text(encoding='utf-8')
                result = {'diff': ''.join(difflib.unified_diff(self.baseline.splitlines(True), after.splitlines(True),
                                                               fromfile='a/pricing.py', tofile='b/pricing.py'))}
            else:
                raise PermissionError('Unknown workspace operation')
            row.update(result=result, status='OBSERVED')
            return result
        except Exception as exc:
            row.update(status='DENIED_OR_FAILED', error=str(exc))
            raise
        finally:
            row['duration_ms'] = round((time.monotonic() - started) * 1000, 2)
            self.evidence.append(row)
