from __future__ import annotations

import base64
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
from typing import Iterable


_PROVIDER_ENV = {
    'openrouter': ('OPENROUTER_API_KEY',),
    'nvidia': ('NVIDIA_API_KEY', 'NVIDIA_NIM_API_KEY'),
    'huggingface': ('HF_TOKEN', 'HUGGINGFACEHUB_API_TOKEN', 'HUGGINGFACE_API_KEY'),
}


class _DATA_BLOB(ctypes.Structure):
    _fields_ = [('cbData', wintypes.DWORD), ('pbData', ctypes.POINTER(ctypes.c_byte))]


class CredentialStore:
    """Secret storage for optional Jubi cloud providers.

    On Windows, credentials saved through the dashboard are encrypted with the
    current user's DPAPI key and written outside the Git workspace under
    ``%LOCALAPPDATA%\\Jubi\\credentials.json``. Environment variables are also
    supported for managed/automated installations. Secrets are never returned
    by status APIs.

    Non-Windows runtimes may read provider credentials from environment
    variables, but intentionally cannot persist dashboard-entered secrets. This
    prevents CI/Linux development from silently falling back to plaintext
    storage while keeping the production Windows design dependency-free.
    """

    VERSION = 1

    def __init__(self, path: Path | None = None):
        if path is not None:
            self.path = Path(path)
        elif os.name == 'nt':
            base = Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData/Local')
            self.path = base / 'Jubi' / 'credentials.json'
        else:
            self.path = Path.home() / '.jubi' / 'credentials.json'

    @staticmethod
    def _provider(provider: str) -> str:
        name = str(provider or '').strip().lower()
        if name not in _PROVIDER_ENV:
            raise ValueError(f'unsupported provider: {provider!r}')
        return name

    @staticmethod
    def _env_names(provider: str) -> Iterable[str]:
        return _PROVIDER_ENV[provider]

    def _read_file(self) -> dict:
        try:
            raw = json.loads(self.path.read_text(encoding='utf-8'))
            if isinstance(raw, dict):
                return raw
        except FileNotFoundError:
            pass
        except Exception:
            # Corruption must not be interpreted as an empty valid secret store.
            raise RuntimeError(f'Jubi credential store is unreadable: {self.path}')
        return {'version': self.VERSION, 'entries': {}}

    def _write_file(self, payload: dict) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding='utf-8')
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        os.replace(tmp, self.path)
        try:
            os.chmod(self.path, 0o600)
        except OSError:
            pass

    @staticmethod
    def _blob(data: bytes):
        buf = ctypes.create_string_buffer(data)
        blob = _DATA_BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_byte)))
        return blob, buf

    @staticmethod
    def _protect(data: bytes, entropy: bytes) -> bytes:
        if os.name != 'nt':
            raise RuntimeError('Dashboard credential persistence requires Windows DPAPI')
        crypt32 = ctypes.windll.crypt32
        kernel32 = ctypes.windll.kernel32
        in_blob, in_buf = CredentialStore._blob(data)
        ent_blob, ent_buf = CredentialStore._blob(entropy)
        out_blob = _DATA_BLOB()
        ok = crypt32.CryptProtectData(
            ctypes.byref(in_blob),
            'Jubi provider credential',
            ctypes.byref(ent_blob),
            None,
            None,
            0x01,  # CRYPTPROTECT_UI_FORBIDDEN
            ctypes.byref(out_blob),
        )
        # Keep buffers referenced until CryptProtectData returns.
        _ = (in_buf, ent_buf)
        if not ok:
            raise ctypes.WinError()
        try:
            return ctypes.string_at(out_blob.pbData, out_blob.cbData)
        finally:
            kernel32.LocalFree(out_blob.pbData)

    @staticmethod
    def _unprotect(data: bytes, entropy: bytes) -> bytes:
        if os.name != 'nt':
            raise RuntimeError('Windows DPAPI credential cannot be decrypted on this platform')
        crypt32 = ctypes.windll.crypt32
        kernel32 = ctypes.windll.kernel32
        in_blob, in_buf = CredentialStore._blob(data)
        ent_blob, ent_buf = CredentialStore._blob(entropy)
        out_blob = _DATA_BLOB()
        desc = ctypes.c_wchar_p()
        ok = crypt32.CryptUnprotectData(
            ctypes.byref(in_blob),
            ctypes.byref(desc),
            ctypes.byref(ent_blob),
            None,
            None,
            0x01,
            ctypes.byref(out_blob),
        )
        _ = (in_buf, ent_buf)
        if not ok:
            raise ctypes.WinError()
        try:
            return ctypes.string_at(out_blob.pbData, out_blob.cbData)
        finally:
            if desc:
                kernel32.LocalFree(desc)
            kernel32.LocalFree(out_blob.pbData)

    @staticmethod
    def _entropy(provider: str) -> bytes:
        return f'Jubi Provider Credential v1::{provider}'.encode('utf-8')

    def get(self, provider: str) -> tuple[str | None, str]:
        provider = self._provider(provider)
        for env_name in self._env_names(provider):
            value = os.environ.get(env_name, '').strip()
            if value:
                return value, f'environment:{env_name}'

        if not self.path.exists():
            return None, 'none'
        data = self._read_file()
        entry = (data.get('entries') or {}).get(provider)
        if not entry:
            return None, 'none'
        if os.name != 'nt':
            return None, 'windows-dpapi-unavailable'
        try:
            encrypted = base64.b64decode(str(entry).encode('ascii'), validate=True)
            plain = self._unprotect(encrypted, self._entropy(provider)).decode('utf-8')
        except Exception as exc:
            raise RuntimeError(f'Unable to decrypt {provider} credential with Windows DPAPI') from exc
        return (plain.strip() or None), 'windows-dpapi'

    def set(self, provider: str, secret: str) -> dict:
        provider = self._provider(provider)
        value = str(secret or '').strip()
        if len(value) < 8:
            raise ValueError('API key/token is empty or unexpectedly short')
        if os.name != 'nt':
            raise RuntimeError(
                'Jubi only persists dashboard credentials on Windows using DPAPI. '
                'On this platform configure the provider with its environment variable instead.'
            )
        payload = self._read_file()
        payload['version'] = self.VERSION
        payload.setdefault('entries', {})[provider] = base64.b64encode(
            self._protect(value.encode('utf-8'), self._entropy(provider))
        ).decode('ascii')
        self._write_file(payload)
        return {'provider': provider, 'configured': True, 'source': 'windows-dpapi'}

    def delete(self, provider: str) -> dict:
        provider = self._provider(provider)
        if self.path.exists():
            payload = self._read_file()
            entries = payload.setdefault('entries', {})
            entries.pop(provider, None)
            self._write_file(payload)
        # An environment variable cannot safely be removed from the parent
        # process; report whether it still supplies the effective credential.
        value, source = self.get(provider)
        return {'provider': provider, 'configured': bool(value), 'source': source}

    def status(self, provider: str) -> dict:
        provider = self._provider(provider)
        value, source = self.get(provider)
        return {
            'provider': provider,
            'configured': bool(value),
            'source': source,
            'persistent_dashboard_storage': os.name == 'nt',
        }
