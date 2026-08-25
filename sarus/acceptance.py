from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from .core.app import Jubi


def _load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding='utf-8'))


def _find_ollama() -> str | None:
    found = shutil.which('ollama') or shutil.which('ollama.exe')
    if found:
        return found
    if os.name == 'nt':
        candidates = [
            Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs' / 'Ollama' / 'ollama.exe',
            Path(os.environ.get('ProgramFiles', '')) / 'Ollama' / 'ollama.exe',
        ]
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
    return None


def _wait_for_ollama(app: Jubi, seconds: int = 30) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        if app.models.list_models().get('online'):
            return True
        time.sleep(1)
    return False


def _start_ollama_for_install(app: Jubi) -> bool:
    if app.models.list_models().get('online'):
        return True
    exe = _find_ollama()
    if not exe:
        return False
    kwargs: dict = {'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    if os.name == 'nt':
        kwargs['creationflags'] = (
            getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
            | getattr(subprocess, 'DETACHED_PROCESS', 0)
            | getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        )
    subprocess.Popen([exe, 'serve'], **kwargs)
    return _wait_for_ollama(app)


def _provision_required_models(app: Jubi, required: list[str]) -> dict:
    status = app.models.list_models()
    if not status.get('online'):
        return {'ok': False, 'error': status.get('error', 'Ollama is offline'), 'pulled': []}
    installed = set(status.get('models', []))
    pulled: list[str] = []
    for model in required:
        if model in installed:
            continue
        response = app.models._json('/api/pull', {'model': model, 'stream': False}, timeout=7200)
        if response.get('status') != 'success':
            return {'ok': False, 'error': f'pull failed for {model}: {response}', 'pulled': pulled}
        pulled.append(model)
    final = app.models.list_models()
    missing = [model for model in required if model not in set(final.get('models', []))]
    return {'ok': not missing, 'pulled': pulled, 'missing': missing}


def _install_mode() -> bool:
    current = os.environ.get('JUBI_INSTALL_MODE', '').lower()
    legacy = os.environ.get('SARUS_INSTALL_MODE', '').lower()
    return current == 'exe' or legacy == 'exe'


def run_acceptance(
    root: Path,
    *,
    full: bool = False,
    provision_models: bool = False,
    require_ring0: bool = False,
) -> dict:
    manifest = _load_json(root / 'BUILD_MANIFEST.json')
    production = _load_json(root / 'config' / 'production.json')
    app = Jubi(root)
    checks: list[dict] = []

    def check(name, fn, required=True):
        try:
            detail = fn()
            if isinstance(detail, dict) and 'ok' in detail:
                ok = bool(detail['ok'])
            else:
                ok = detail is not False
            checks.append({'name': name, 'ok': bool(ok), 'detail': detail, 'required': required})
        except Exception as exc:
            checks.append({'name': name, 'ok': False, 'detail': str(exc), 'required': required})

    check('manifest version matches production profile', lambda: manifest['version'] == production['version'])
    check('Jubi product metadata synchronized', lambda: manifest.get('name') == 'Jubi' and production.get('name') == 'Jubi')
    check(
        '10 source adapters',
        lambda: len(app.adapters.connect()) == manifest['source_repositories']
        and all(x.connected for x in app.adapters.connect()),
    )
    check(
        'capability registry matches manifest',
        lambda: sum(x['files'] for x in app.registry.summary().values()) == manifest['indexed_original_files'],
    )
    check('receipt chain', lambda: app.receipts.verify_chain()['ok'])

    def memory_roundtrip():
        token = 'accept-' + str(time.time())
        saved = app.memory.add(token, 'acceptance', 'test')
        # Reopen a new store to prove the write is committed, not merely visible
        # through one connection/object.
        from .core.memory import MemoryStore
        reopened = MemoryStore(app.db_path)
        return {'ok': any(x['id'] == saved['id'] for x in reopened.search(token, 'test', 5))}

    check('memory write/reopen/search', memory_roundtrip)
    check('policy approval gate', lambda: app.policy.evaluate('privileged_system_action', 5, 'core')['decision'] == 'approval')
    check('CAI isolation', lambda: app.policy.evaluate('active_test', 2, 'cai')['decision'] == 'isolated')

    fable = app.fable.status()
    check('Fable native integration', lambda: bool(fable.get('integrated')))
    check('Fable source complete', lambda: bool(fable.get('source', {}).get('source_complete')))
    check('Fable proof boundary', lambda: bool(fable.get('trace', {}).get('model_prose_is_not_proof')))

    required_models = list(production.get('required_models', []))
    install_mode = _install_mode()
    should_provision = provision_models or install_mode
    if should_provision and not app.models.list_models().get('online'):
        check('Ollama auto-start for installer', lambda: _start_ollama_for_install(app), required=True)
    if should_provision:
        check('Required Ollama model provisioning', lambda: _provision_required_models(app, required_models), required=True)

    doctor = app.doctor.run()
    check('Ollama online', lambda: doctor['models'].get('online', False), required=True)
    installed = set(doctor['models'].get('models', []))
    for model in required_models:
        check('model ' + model, lambda model=model: model in installed, required=True)

    if full and doctor['models'].get('online') and required_models:
        chat_model = app.models.choose('general')
        check(
            'Ollama generation',
            lambda: bool(chat_model and app.models.generate_text('Reply exactly JUBI_OK', 'general', model=chat_model)[:100]),
            required=True,
        )

    if os.name == 'nt':
        check('Windows process broker', lambda: app.windows.action('list_processes').get('ok'), required=True)
        check(
            'SARA v7 native API bridge',
            lambda: app.native.status()['sara']['ready'],
            required=bool(production.get('require_sara_on_windows', True)),
        )
        check('ECC native runtime', lambda: app.native.status()['ecc']['ready'], required=False)
        check('Hermes native CLI', lambda: app.native.status()['hermes']['ready'], required=False)
        ring0 = app.windows.ring0.status()
        checks.append(
            {
                'name': 'Controlled legacy Ring0 compatibility bridge',
                'ok': bool(ring0.get('ok')),
                'detail': ring0,
                'required': require_ring0,
            }
        )
    else:
        checks.append(
            {
                'name': 'Windows target acceptance',
                'ok': False,
                'detail': 'run acceptance on the target Windows laptop for physical certification',
                'required': False,
            }
        )

    ok = all(c['ok'] for c in checks if c['required'])
    out = {
        'name': 'Jubi Production Acceptance',
        'version': production['version'],
        'foundation': production.get('foundation'),
        'ok': ok,
        'target': 'windows' if os.name == 'nt' else os.name,
        'install_mode': install_mode,
        'checks': checks,
        'doctor': doctor,
    }
    app.shutdown()
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--provision-models', action='store_true')
    parser.add_argument('--require-ring0', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = run_acceptance(
        root,
        full=args.full,
        provision_models=args.provision_models,
        require_ring0=args.require_ring0,
    )
    print(json.dumps(out, ensure_ascii=False, indent=2))
    raise SystemExit(0 if out['ok'] else 2)


if __name__ == '__main__':
    main()
