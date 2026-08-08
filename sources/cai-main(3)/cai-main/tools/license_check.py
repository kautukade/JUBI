#!/usr/bin/env python3
"""
CAI License Check (single-machine binding with cryptographic signature verification)

Flow:
- Saves provided API key encrypted locally (OpenSSL AES-256-CBC with local key file).
- Computes a machine fingerprint.
- Calls Alias validation endpoint and verifies cryptographic signature.
- Server signs: system_hash + nonce + "true"/"false"
- Client verifies signature and only accepts if message ends with "true"
"""

import base64
import hashlib
import json
import os
import platform
import random
import shutil
import string
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Any, Dict

# Try to import from both pycryptodome (Crypto) and python3-pycryptodome (Cryptodome)
try:
    from Crypto.Hash import SHA256
    from Crypto.PublicKey import RSA
    from Crypto.Signature import pkcs1_15
except ImportError:
    try:
        from Cryptodome.Hash import SHA256
        from Cryptodome.PublicKey import RSA
        from Cryptodome.Signature import pkcs1_15
    except ImportError:
        print("Error: pycryptodome library not installed. Install with: pip3 install pycryptodome", file=sys.stderr)
        sys.exit(1)

# Embedded server public key
SERVER_PUBLIC_KEY_PEM = """-----BEGIN PUBLIC KEY-----
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

VALIDATION_URL = "https://api.aliasrobotics.com:665/v1/validate"
KEY_INFO_URL = "https://api.aliasrobotics.com:666/key/info"


class APIValidationError(Exception):
    pass


class CryptographyError(Exception):
    pass


def _project_root() -> Path:
    here = Path(__file__).resolve()
    for cand in here.parents:
        if (cand / "resources" / "keys").exists() or cand.name == "cai_installer":
            return cand
    return here.parent


ROOT = _project_root()
KEY_DIR = ROOT / "resources" / "keys"
ENC_FILE = KEY_DIR / "encrypted_api_key.enc"
KEY_FILE = KEY_DIR / "encryption_key.key"


def _ensure_command(cmd: str) -> None:
    if shutil.which(cmd) is None:
        raise APIValidationError(f"Required command '{cmd}' is not available on this system")


def _ensure_key_material() -> None:
    KEY_DIR.mkdir(parents=True, exist_ok=True)
    if not KEY_FILE.exists():
        KEY_FILE.write_bytes(os.urandom(32))
        os.chmod(KEY_FILE, 0o600)


def _openssl(args: list[str]) -> subprocess.CompletedProcess[str]:
    _ensure_command("openssl")
    proc = subprocess.run(["openssl", *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise CryptographyError(proc.stderr.strip() or "OpenSSL command failed")
    return proc


def save_encrypted_key(api_key: str) -> None:
    if not isinstance(api_key, str) or not api_key.strip():
        raise APIValidationError("API key cannot be empty")
    _ensure_key_material()
    with tempfile.NamedTemporaryFile("w", delete=False) as tmp_plain:
        tmp_plain.write(api_key.strip())
    tmp_plain_path = Path(tmp_plain.name)
    try:
        _openssl([
            "enc",
            "-aes-256-cbc",
            "-pbkdf2",
            "-salt",
            "-pass",
            f"file:{KEY_FILE}",
            "-in",
            str(tmp_plain_path),
            "-out",
            str(ENC_FILE),
        ])
    finally:
        try:
            tmp_plain_path.unlink()
        except FileNotFoundError:
            pass
    os.chmod(ENC_FILE, 0o600)


def load_encrypted_key() -> str:
    if not ENC_FILE.exists():
        raise CryptographyError(f"Encrypted key file not found: {ENC_FILE}")
    if not KEY_FILE.exists():
        raise CryptographyError(f"Encryption key file not found: {KEY_FILE}")
    with tempfile.NamedTemporaryFile(delete=False) as tmp_plain:
        tmp_plain_path = Path(tmp_plain.name)
    try:
        _openssl([
            "enc",
            "-d",
            "-aes-256-cbc",
            "-pbkdf2",
            "-pass",
            f"file:{KEY_FILE}",
            "-in",
            str(ENC_FILE),
            "-out",
            str(tmp_plain_path),
        ])
        return tmp_plain_path.read_text().strip()
    finally:
        try:
            tmp_plain_path.unlink()
        except FileNotFoundError:
            pass


def generate_system_hash() -> str:
    parts: list[str] = []
    try:
        nid = uuid.getnode()
        if nid:
            parts.append(f"mac:{nid:012x}")
    except Exception:
        pass
    for p in (Path("/etc/machine-id"), Path("/var/lib/dbus/machine-id")):
        try:
            if p.exists():
                val = p.read_text().strip()
                if val:
                    parts.append(f"machine:{val}")
                    break
        except Exception:
            pass
    for getter, label in ((platform.node, "node"), (platform.system, "system"), (platform.machine, "arch")):
        try:
            v = getter()
            if v:
                parts.append(f"{label}:{v}")
        except Exception:
            pass
    if not parts:
        rnd = ''.join(random.choices(string.ascii_letters + string.digits, k=16))
        parts.append("fallback:" + rnd)
    digest = hashlib.sha256('|'.join(sorted(parts)).encode()).hexdigest()
    return digest[:32]


def generate_nonce(n: int = 32) -> str:
    alphabet = string.ascii_letters + string.digits
    return ''.join(random.choice(alphabet) for _ in range(n))


def create_payload(api_key: str, system_hash: str, nonce: str) -> Dict[str, str]:
    return {"key": api_key, "system": system_hash, "query": nonce}


def send_validation_request(payload: Dict[str, Any]) -> Dict[str, Any]:
    _ensure_command("curl")
    data = json.dumps(payload)
    proc = subprocess.run([
        "curl", "-sS", "--connect-timeout", "5", "--max-time", "5", "-k",
        "-X", "GET",
        "-H", "Content-Type: application/json",
        "-d", data,
        VALIDATION_URL
    ], capture_output=True, text=True)
    if proc.returncode != 0:
        raise APIValidationError(proc.stderr.strip() or "Failed to contact validation server")
    try:
        return json.loads(proc.stdout.strip() or "{}")
    except json.JSONDecodeError as exc:
        raise APIValidationError(f"Invalid JSON response: {exc}")


def check_key_info_status(api_key: str) -> bool:
    """Confirm API key validity with the /key/info endpoint (HTTP 200 means valid)."""
    _ensure_command("curl")
    hdr = f"Authorization: Bearer {api_key}"
    proc = subprocess.run([
        "curl", "-sS", "--connect-timeout", "5", "--max-time", "5",
        "-H", hdr,
        "-w", "\n__STATUS__%{http_code}\n",
        KEY_INFO_URL
    ], capture_output=True, text=True)
    if proc.returncode != 0:
        raise APIValidationError(proc.stderr.strip() or "Failed to contact key info server")
    out = proc.stdout
    if "__STATUS__" in out:
        body, _, tail = out.rpartition("__STATUS__")
        code = tail.strip()
    else:
        code = "000"
    return code == "200"


def verify_signature(signature_b64: str, message: str) -> bool:
    """
    Verify RSA signature using server's public key.
    Returns True if signature is valid for the given message.
    """
    try:
        # Load public key
        public_key = RSA.import_key(SERVER_PUBLIC_KEY_PEM)
        
        # Decode signature
        signature_bytes = base64.b64decode(signature_b64)
        
        # Hash the message
        msg_hash = SHA256.new(message.encode('utf-8'))
        
        # Verify signature
        pkcs1_15.new(public_key).verify(msg_hash, signature_bytes)
        return True
    except Exception:
        # Signature verification failed
        return False


def validate_encrypted_api_key() -> bool:
    api_key = load_encrypted_key()
    system_hash = generate_system_hash()
    nonce = generate_nonce()
    
    # Send validation request to server
    response = send_validation_request(create_payload(api_key, system_hash, nonce))

    if not isinstance(response, dict):
        raise APIValidationError("Validation server returned unexpected payload")

    # Get signature from response
    signature = response.get("validation")
    if not signature or not isinstance(signature, str):
        raise APIValidationError("Server response missing 'validation' signature")

    # Construct expected message with "true" (server signs: system_hash + nonce + "true"/"false")
    expected_message_true = system_hash + nonce + "true"
    
    # Verify cryptographic signature
    if not verify_signature(signature, expected_message_true):
        raise APIValidationError("API key rejected: signature verification failed (hardware mismatch or invalid key)")

    # Additional check: verify key is valid via /key/info endpoint
    if not check_key_info_status(api_key):
        raise APIValidationError("API key validation failed (key/info rejected or invalid)")

    return True


def _main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("Error: API key not provided")
        return 1
    key = argv[1]
    try:
        print("Encrypting and saving API key...")
        save_encrypted_key(key)
        print("API key encrypted and saved successfully.")
        print("Validating API key...")
        if validate_encrypted_api_key():
            print("API key validation successful.")
            return 0
    except (CryptographyError, APIValidationError) as exc:
        print(f"Error: {exc}")
        return 1
    except Exception as exc:
        print(f"Unexpected error: {exc}")
        return 1
    print("API key validation failed.")
    return 1


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
