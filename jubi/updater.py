"""Verified public update client for the Jubi Windows installation.

The updater only trusts the configured JUBI GitHub repository/release, requires a
matching SHA-256 installer hash, and never executes arbitrary update commands
from the network. It is intentionally dependency-free so the private Jubi
Python runtime can run it without pip-installed packages.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP_PATH = ROOT / "config" / "bootstrap.json"
BUILD_INFO_PATH = ROOT / "config" / "build-info.json"
_ALLOWED_DOWNLOAD_HOST_SUFFIXES = (
    "github.com",
    "githubusercontent.com",
    "github-releases.githubusercontent.com",
)


def _load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def bootstrap_config() -> dict:
    data = _load_json(BOOTSTRAP_PATH)
    return data if isinstance(data, dict) else {}


def build_info() -> dict:
    data = _load_json(BUILD_INFO_PATH)
    return data if isinstance(data, dict) else {}


def state_root() -> Path:
    base = os.environ.get("LOCALAPPDATA")
    if base:
        root = Path(base) / "Jubi"
    else:
        root = Path.home() / ".jubi"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _request(url: str, *, timeout: int = 30) -> urllib.request.addinfourl:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https":
        raise RuntimeError("Jubi updater only permits HTTPS update endpoints.")
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "Jubi-Updater/1",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    response = urllib.request.urlopen(request, timeout=timeout)
    final_host = (urllib.parse.urlparse(response.geturl()).hostname or "").lower()
    if not any(final_host == suffix or final_host.endswith("." + suffix) for suffix in _ALLOWED_DOWNLOAD_HOST_SUFFIXES):
        response.close()
        raise RuntimeError(f"Update redirect left trusted GitHub hosts: {final_host}")
    return response


def _json_from_url(url: str, *, timeout: int = 30) -> dict:
    with _request(url, timeout=timeout) as response:
        payload = response.read(2 * 1024 * 1024 + 1)
    if len(payload) > 2 * 1024 * 1024:
        raise RuntimeError("Update metadata exceeded the allowed size.")
    data = json.loads(payload.decode("utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError("Update metadata is not a JSON object.")
    return data


def _asset(release: dict, name: str) -> dict:
    for asset in release.get("assets") or []:
        if isinstance(asset, dict) and asset.get("name") == name:
            return asset
    raise RuntimeError(f"Required update asset is missing: {name}")


def _current_commit() -> str:
    value = str(build_info().get("commit_sha") or "").strip().lower()
    return value if len(value) >= 7 else "unknown"


def _current_epoch() -> int:
    try:
        return int(build_info().get("build_epoch") or 0)
    except (TypeError, ValueError):
        return 0


def check_for_update() -> dict:
    cfg = bootstrap_config().get("auto_update") or {}
    if not cfg.get("enabled", True) or os.environ.get("JUBI_AUTO_UPDATE", "1").strip().lower() in {"0", "false", "off", "no"}:
        return {"available": False, "reason": "disabled"}

    release_api = str(cfg.get("release_api") or "").strip()
    manifest_name = str(cfg.get("manifest_asset") or "Jubi-Update-Manifest.json")
    installer_name = str(cfg.get("installer_asset") or "Jubi-Setup.exe")
    if not release_api.startswith("https://api.github.com/repos/kautukade/JUBI/"):
        raise RuntimeError("Update release API is outside the canonical JUBI repository.")

    release = _json_from_url(release_api)
    manifest_asset = _asset(release, manifest_name)
    installer_asset = _asset(release, installer_name)
    manifest_url = str(manifest_asset.get("browser_download_url") or "")
    installer_url = str(installer_asset.get("browser_download_url") or "")
    manifest = _json_from_url(manifest_url)

    remote_commit = str(manifest.get("commit_sha") or "").strip().lower()
    remote_hash = str(manifest.get("installer_sha256") or "").strip().lower()
    remote_epoch = int(manifest.get("build_epoch") or 0)
    if len(remote_commit) != 40 or any(c not in "0123456789abcdef" for c in remote_commit):
        raise RuntimeError("Update manifest commit SHA is invalid.")
    if len(remote_hash) != 64 or any(c not in "0123456789abcdef" for c in remote_hash):
        raise RuntimeError("Update manifest installer SHA-256 is invalid.")
    if manifest.get("repository") != "kautukade/JUBI":
        raise RuntimeError("Update manifest repository identity mismatch.")

    current = _current_commit()
    if current == remote_commit:
        return {"available": False, "reason": "current", "commit_sha": current}
    if not cfg.get("allow_downgrade", False) and _current_epoch() and remote_epoch and remote_epoch <= _current_epoch():
        return {"available": False, "reason": "not-newer", "commit_sha": current, "remote_commit": remote_commit}

    return {
        "available": True,
        "commit_sha": current,
        "remote_commit": remote_commit,
        "build_epoch": remote_epoch,
        "version": str(manifest.get("version") or ""),
        "installer_sha256": remote_hash,
        "installer_url": installer_url,
        "release_url": str(release.get("html_url") or ""),
    }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_update(info: dict) -> Path:
    if not info.get("available"):
        raise RuntimeError("No update is available.")
    remote_commit = str(info["remote_commit"])
    expected = str(info["installer_sha256"]).lower()
    target_dir = state_root() / "updates" / remote_commit
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / "Jubi-Setup.exe"
    partial = target.with_suffix(".download")

    with _request(str(info["installer_url"]), timeout=300) as response, partial.open("wb") as handle:
        total = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > 2 * 1024 * 1024 * 1024:
                raise RuntimeError("Update installer exceeded the 2 GiB safety limit.")
            handle.write(chunk)
    if partial.stat().st_size < 1024 * 1024:
        partial.unlink(missing_ok=True)
        raise RuntimeError("Downloaded update installer is unexpectedly small.")
    actual = _sha256(partial)
    if actual != expected:
        partial.unlink(missing_ok=True)
        raise RuntimeError(f"Update installer SHA-256 mismatch: expected {expected}, got {actual}")
    partial.replace(target)
    return target


def apply_update(info: dict) -> int:
    if os.name != "nt":
        raise RuntimeError("Jubi installer updates can only be applied on Windows.")
    installer = download_update(info)
    args = [
        str(installer),
        "/VERYSILENT",
        "/SUPPRESSMSGBOXES",
        "/NORESTART",
        "/CLOSEAPPLICATIONS",
        "/FORCECLOSEAPPLICATIONS",
        "/UPDATE",
    ]
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    completed = subprocess.run(args, check=False, creationflags=flags)
    return int(completed.returncode)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    command = argv[0].lower() if argv else "check"
    try:
        info = check_for_update()
        if command == "check":
            print(json.dumps(info, indent=2, sort_keys=True))
            return 0
        if command == "apply":
            if not info.get("available"):
                print("Jubi is already current.")
                return 0
            return apply_update(info)
        raise RuntimeError(f"Unknown updater command: {command}")
    except Exception as exc:  # updater must fail closed, not take the agent down
        print(f"Jubi updater error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
