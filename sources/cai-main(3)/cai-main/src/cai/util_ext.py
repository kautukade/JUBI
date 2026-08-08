"""Extended utilities for CAI"""
import base64
import hashlib
import json
import os
import platform
import random
import string
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Optional, Dict, Any

# Embedded server public key
_K = """-----BEGIN PUBLIC KEY-----
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEA98imbEha/70cxkfXIbyJ
dbpM6y7X+MWMVcdSTwAeb+jzLRfKzMZVXeEaYzkzH+STlDiqmb+XufX+guhmpyKz
RbV3rcJeM2s4QJ3OcsbOnxVG9Eyo6LrHu/aU71LOW8pPD5eIh0/BRCDojG56pZ8N
CFF0Rsfve6SH6waOibUoovMYu2ZfzzGb/oeyPsL8yb4fIqnOOH85FbAm8aCrpGNZ
6A8U67s6TQAnqLFn6x2h901K2GhNxweRQqJ5n2qwMCPLmEHyKZiLo8GbK3lrcnbj
j6qNKkgoL+b9rcYJ1toLP8btTHIyQX2N+gWVgxzMNPtDfCKIJm4Jag0fIEiVsNF2
QZ7NaMjRqtRs8WIzmUWBMVoyWqWCty6sNrum/uYZNCcNma855IeFEOrmYNzNEWxP
5xgJT/Hvs9/bTOUWqrB3L/8EGeV3hrhUZ1km25mogAnSOcFWviqObREc/1yTfsB4
Ln0Hv4tvILd74aBqG1zRF0Wjd25O7vl8oY3RBp7iCtrq+CdOI1jrTx4yo9JM/DKu
MfTbFI1YPjuJahQjbbKHvCKPKq6pxCQBZ91SsBHdM2tHLRXZ1olO0y0trety+fjx
T9W1tmmRU9QyFvS9LsKdjfBOzyprC5MCz740pctKkSspYn88R19N8Cu3MihaTUw6
/JHvKMwL2+DgFmF7Dqjc+jsCAwEAAQ==
-----END PUBLIC KEY-----"""

_V_URL = "https://api.aliasrobotics.com:665/v1/validate"
_I_URL = "https://api.aliasrobotics.com:666/key/info"

try:
    from Crypto.Hash import SHA256
    from Crypto.PublicKey import RSA
    from Crypto.Signature import pkcs1_15
    _crypto_available = True
except ImportError:
    try:
        from Cryptodome.Hash import SHA256
        from Cryptodome.PublicKey import RSA
        from Cryptodome.Signature import pkcs1_15
        _crypto_available = True
    except ImportError:
        _crypto_available = False


def _h() -> str:
    """Generate system fingerprint"""
    p: list[str] = []
    try:
        n = uuid.getnode()
        if n:
            p.append(f"mac:{n:012x}")
    except Exception:
        pass
    for path in (Path("/etc/machine-id"), Path("/var/lib/dbus/machine-id")):
        try:
            if path.exists():
                v = path.read_text().strip()
                if v:
                    p.append(f"machine:{v}")
                    break
        except Exception:
            pass
    for g, l in ((platform.node, "node"), (platform.system, "system"), (platform.machine, "arch")):
        try:
            v = g()
            if v:
                p.append(f"{l}:{v}")
        except Exception:
            pass
    if not p:
        r = ''.join(random.choices(string.ascii_letters + string.digits, k=16))
        p.append("fallback:" + r)
    d = hashlib.sha256('|'.join(sorted(p)).encode()).hexdigest()
    return d[:32]


def _n(l: int = 32) -> str:
    """Generate nonce"""
    a = string.ascii_letters + string.digits
    return ''.join(random.choice(a) for _ in range(l))


def _s(sig_b64: str, msg: str) -> bool:
    """Verify signature"""
    if not _crypto_available:
        return False
    try:
        k = RSA.import_key(_K)
        sig = base64.b64decode(sig_b64)
        h = SHA256.new(msg.encode('utf-8'))
        pkcs1_15.new(k).verify(h, sig)
        return True
    except Exception:
        return False


def _c(k: str) -> bool:
    """Check key via info endpoint (httpx; same status semantics as legacy curl check)."""
    try:
        import httpx

        timeout = httpx.Timeout(3.0, connect=3.0)
        with httpx.Client(timeout=timeout) as client:
            r = client.get(
                _I_URL,
                headers={"Authorization": f"Bearer {k}"},
            )
        code = r.status_code
        # 403 = virtual key (valid for LLM routes), 500/502/429 = proxy/rate-limit
        return code in (200, 403, 500, 502, 429)
    except Exception:
        return False


def _v(k: str) -> bool:
    """Validate key"""
    if not k or not k.strip():
        return False
    # Only check if key is valid via /key/info endpoint (HTTP 200 = valid)
    # No machine-specific validation
    return _c(k)


def _license_off() -> bool:
    """Return True when ``CAI_LICENSE_OFF`` is set to a truthy value.

    When enabled, CAI runs in open-source mode: the startup license check is
    bypassed and update operations target the public PyPI ``cai-framework``
    package instead of the private Alias package index.
    """
    return os.getenv("CAI_LICENSE_OFF", "").strip().lower() in ("1", "true", "yes")


def _chk() -> bool:
    """Check license validity.

    Set ``CAI_LICENSE_OFF=1`` in the environment to bypass the license check
    entirely (e.g. for open-source builds or local development).
    """
    if _license_off():
        return True
    k = os.getenv("ALIAS_API_KEY", "").strip()
    if not k:
        return False  # No key set, deny operation
    return _v(k)


def check_system_dependencies() -> tuple[bool, list[str]]:
    """Check for required system dependencies.
    
    Returns:
        Tuple of (all_ok, missing_dependencies)
    """
    import shutil
    required = ["curl"]
    missing = [cmd for cmd in required if shutil.which(cmd) is None]
    return (len(missing) == 0, missing)


def display_missing_dependencies_error(missing: list[str]) -> None:
    """Display friendly error message for missing dependencies."""
    from rich.console import Console
    from rich.panel import Panel
    
    console = Console(stderr=True)
    deps_list = "\n".join(f"  • {dep}" for dep in missing)
    
    install_hint = ""
    if "curl" in missing:
        install_hint = (
            "\n[yellow]Installation hints:[/yellow]\n"
            "  • Debian/Ubuntu: [cyan]sudo apt-get install curl[/cyan]\n"
            "  • macOS:         [cyan]brew install curl[/cyan]"
        )
    
    console.print(
        Panel(
            f"[bold red]Missing Required System Dependencies[/bold red]\n\n"
            f"The following system commands are required:\n\n"
            f"{deps_list}\n"
            f"{install_hint}",
            title="[red]Dependency Error[/red]",
            border_style="red"
        )
    )


def pip_index_timeout_seconds() -> int:
    """Timeout for ``pip index`` in :func:`check_for_updates` (``CAI_UPDATE_PIP_TIMEOUT``, default 10)."""
    try:
        v = int(os.getenv("CAI_UPDATE_PIP_TIMEOUT", "10"))
    except ValueError:
        return 10
    return max(3, min(v, 120))


def user_env_requests_auto_framework_update() -> bool:
    """Return True only if the user explicitly enabled auto-install via environment.

    ``CAI_AUTO_UPDATE`` must be **present** in :data:`os.environ` (e.g. from ``export`` or
    ``.env`` before process start). If the key is missing, startup always prompts.
    When present, the value must be truthy (``1``, ``true``, ``yes``, ``on``); any other
    value (including empty) is treated as off so accidental ``CAI_AUTO_UPDATE=`` does not
    auto-upgrade.
    """
    if "CAI_AUTO_UPDATE" not in os.environ:
        return False
    return os.getenv("CAI_AUTO_UPDATE", "").strip().lower() in ("1", "true", "yes", "on")


def check_for_updates() -> Optional[Dict[str, Any]]:
    """Check if there's an update available for cai-framework.
    
    Returns:
        Dict with update info if available, None if no update or on error
        {
            "current_version": "x.x.x",
            "latest_version": "y.y.y",
            "update_available": True
        }
    """
    try:
        import importlib.metadata
        import re
        
        # Get current installed version
        try:
            current_version = importlib.metadata.version("cai-framework")
        except importlib.metadata.PackageNotFoundError:
            # Development installation
            return None
            
        # In OSS mode, check against public PyPI (no ALIAS_API_KEY required).
        # Otherwise, use the private Alias index gated by the API key.
        oss_mode = _license_off()
        if oss_mode:
            pip_args = [
                sys.executable, "-m", "pip", "index", "versions",
                "--no-color",
                "cai-framework",
            ]
        else:
            k = os.getenv("ALIAS_API_KEY", "").strip()
            if not k:
                return None
            index_url = f"https://packages.aliasrobotics.com:664/{k}/"
            pip_args = [
                sys.executable, "-m", "pip", "index", "versions",
                "--index-url", index_url,
                "--no-color",
                "cai-framework",
            ]

        # Use pip index to check for latest version without downloading
        result = subprocess.run(
            pip_args,
            capture_output=True,
            text=True,
            timeout=pip_index_timeout_seconds(),
        )
        
        if result.returncode != 0:
            return None
            
        # Parse output to find available versions
        output = result.stdout
        # Look for version numbers in the output
        version_pattern = r'(\d+\.\d+\.\d+(?:\.\w+)?)'
        versions = re.findall(version_pattern, output)
        
        if not versions:
            return None
            
        # Sort versions and get the latest
        from packaging import version as pkg_version
        sorted_versions = sorted(versions, key=pkg_version.parse, reverse=True)
        latest_version = sorted_versions[0] if sorted_versions else None
        
        if not latest_version:
            return None
            
        # Compare versions
        update_available = pkg_version.parse(latest_version) > pkg_version.parse(current_version)
        return {
            "current_version": current_version,
            "latest_version": latest_version,
            "update_available": update_available,
        }

    except Exception:
        # Silently fail - don't interrupt normal operation
        pass

    return None


def prompt_for_update(update_info: Dict[str, Any]) -> bool:
    """Prompt user to update CAI (Rich chrome matches session banner: CAI green / #004433 / grey)."""
    from rich import box
    from rich.console import Console
    from rich.panel import Panel
    from rich.prompt import Confirm
    from rich.table import Table
    from rich.text import Text

    from cai.repl.ui.banner import CAI_GREEN

    _grey_mid = "#888888"
    _grey = "dim white"

    console = Console()

    title = Text()
    title.append(" CAI ", style="bold #0d1117 on #00ff9d")
    title.append(" New version available ", style="bold white on #004433")
    title.append(" ", style="on #004433")

    table = Table(
        show_header=False,
        box=box.SIMPLE_HEAD,
        border_style=_grey_mid,
        padding=(0, 1),
        collapse_padding=True,
    )
    table.add_column(style=_grey, no_wrap=True)
    table.add_column()
    table.add_row(
        "Installed",
        Text(update_info["current_version"], style="italic white"),
    )
    table.add_row(
        "Latest",
        Text(update_info["latest_version"], style=f"bold {CAI_GREEN}"),
    )

    panel = Panel(
        table,
        title=title,
        title_align="left",
        border_style=CAI_GREEN,
        expand=False,
        padding=(0, 1),
        subtitle="[dim white]From your Alias package index[/dim white]",
        subtitle_align="left",
    )

    console.print()
    console.print(panel)
    console.print()

    from rich.theme import Theme
    styled_console = Console(theme=Theme({
        "prompt.choices": f"bold {CAI_GREEN}",
        "prompt.default": CAI_GREEN,
    }))
    return Confirm.ask(
        f"[bold {CAI_GREEN}]Update now?[/bold {CAI_GREEN}] [dim white](default: no — explicit yes required)[/dim white]",
        default=False,
        console=styled_console,
    )


def perform_update(api_key: str) -> bool:
    """Perform the pip update for cai-framework.

    Args:
        api_key: The ALIAS_API_KEY for authentication against the private Alias
            package index. Ignored when ``CAI_LICENSE_OFF=1`` is set, in which
            case the update is fetched from public PyPI.

    Returns:
        True if update succeeded, False otherwise
    """
    from rich.console import Console
    from rich.panel import Panel
    from rich.progress import Progress, SpinnerColumn, TextColumn
    from rich.text import Text

    from cai.repl.ui.banner import CAI_GREEN

    console = Console()

    oss_mode = _license_off()
    if oss_mode:
        pip_args = [
            sys.executable,
            "-m",
            "pip",
            "install",
            "--upgrade",
            "cai-framework",
        ]
    else:
        index_url = f"https://packages.aliasrobotics.com:664/{api_key}/"
        pip_args = [
            sys.executable,
            "-m",
            "pip",
            "install",
            "--index-url",
            index_url,
            "--upgrade",
            "cai-framework",
        ]

    with Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        console=console,
    ) as progress:
        task = progress.add_task(
            f"[bold {CAI_GREEN}]Updating cai-framework…[/bold {CAI_GREEN}]",
            total=None,
        )

        result = subprocess.run(
            pip_args,
            capture_output=True,
            text=True,
        )

        progress.update(task, completed=True)

    if result.returncode == 0:
        ok_line = Text()
        ok_line.append("✓ ", style=f"bold {CAI_GREEN}")
        ok_line.append("Update completed", style="bold white")
        sub = Text()
        try:
            import importlib.metadata

            installed = importlib.metadata.version("cai-framework")
            sub.append(
                f"Installed cai-framework {installed} (same as cai --version). ",
                style="dim white",
            )
        except Exception:
            pass
        sub.append("Restart CAI to load the new version.", style="italic dim white")
        console.print(
            Panel(
                Text.assemble(ok_line, "\n", sub),
                border_style=CAI_GREEN,
                padding=(0, 1),
                title=Text.assemble(
                    (" CAI ", "bold #0d1117 on #00ff9d"),
                    (" Done ", "bold white on #004433"),
                    (" ", "on #004433"),
                ),
                title_align="left",
            )
        )
        return True

    err = Text()
    err.append("Update failed", style="bold white")
    err.append("\n", "")
    err.append(result.stderr or "(no details)", style="dim white")
    console.print(
        Panel(
            err,
            border_style="red",
            title="[bold white]CAI[/bold white]",
            title_align="left",
        )
    )
    return False
