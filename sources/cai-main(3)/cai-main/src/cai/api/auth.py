"""Authentication helpers and storage for the CAI API backend.

This module implements a very small user database and per-device session
tokens, persisted to a JSON file under the CAI config directory
(`~/.cai/api_auth.json`). Both authentication flows described in the
sequence diagrams share the same users table:

- Flow 1 (device pairing):
  * The client calls the API with a device IP address.
  * The server creates a random username/password pair backed by the user
    database and issues a session token bound to that user and IP.

- Flow 2 (explicit user accounts):
  * Clients can register users with a chosen username/password.
  * Clients log in with username/password to obtain a session token.

For the rest of the API, the `session_token` is carried in the same
header the API already uses for API keys (by default `X-CAI-API-Key`).
The access control helper in `app.py` accepts either:

- The static root API key from `ALIAS_API_KEY` / `CAI_API_KEY`, or
- Any valid session token managed by :class:`AuthManager`.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import secrets
import threading
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Dict, Optional

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from cai.util import get_config_dir


@dataclass
class UserRecord:
    """Represents a single user in the auth database."""

    id: str
    username: str
    password_hash: str
    salt: str
    created_at: str


@dataclass
class SessionRecord:
    """Represents a session token issued to a device."""

    token: str
    user_id: str
    device_ip: Optional[str]
    method: str  # e.g. "ip_pairing" or "password_login"
    created_at: str
    expires_at: Optional[str]


class AuthError(Exception):
    """Base class for authentication errors."""


class UserAlreadyExistsError(AuthError):
    """Raised when attempting to create a user with an existing username."""


class InvalidCredentialsError(AuthError):
    """Raised when a login attempt fails."""


class AuthManager:
    """Simple auth manager with JSON-backed storage.

    Data layout on disk:
        {
            "users": [...],
            "sessions": [...]
        }
    """

    def __init__(self, db_path: Optional[Path] = None) -> None:
        if db_path is None:
            db_path = get_config_dir() / "api_auth.json"
        self._db_path = db_path
        self._lock = threading.Lock()
        self._crypto_key = self._load_or_create_key()
        # Session TTL in seconds; 0 or negative means "no expiry"
        self._session_ttl_seconds = self._load_session_ttl()
        self._users_by_username: Dict[str, UserRecord] = {}
        self._sessions_by_token: Dict[str, SessionRecord] = {}
        self._load_from_disk()
        # Ensure there is at least one default user per installation.
        # The credentials are generated once (when the DB is empty) and
        # persisted, so they remain stable across API restarts.
        if not self._users_by_username:
            self._ensure_default_user()

    @staticmethod
    def _load_session_ttl() -> int:
        """Return the configured session TTL in seconds.

        Controlled via the CAI_AUTH_SESSION_TTL_SECONDS env var.
        Defaults to 24 hours if not set or invalid.
        """
        default_ttl = 24 * 60 * 60
        raw = os.getenv("CAI_AUTH_SESSION_TTL_SECONDS")
        if raw is None:
            return default_ttl
        try:
            value = int(raw)
            return value if value > 0 else default_ttl
        except ValueError:
            return default_ttl

    # ------------------------------------------------------------------
    # Persistence helpers
    # ------------------------------------------------------------------
    def _load_or_create_key(self) -> bytes:
        """Load or create the symmetric key used to encrypt the auth DB.

        The key is stored alongside the DB file with a `.key` suffix, in
        base64-url encoding. While this does not protect against an attacker
        with full filesystem access, it prevents the auth DB from being
        stored in cleartext and allows operators to move/rotate the key if
        needed.
        """
        key_path = self._db_path.with_suffix(self._db_path.suffix + ".key")
        key_path.parent.mkdir(parents=True, exist_ok=True)

        if key_path.exists():
            try:
                raw = key_path.read_bytes().strip()
                if raw:
                    return base64.urlsafe_b64decode(raw)
            except Exception:
                # Fall through to regenerate a new key
                pass

        key = AESGCM.generate_key(bit_length=256)
        encoded = base64.urlsafe_b64encode(key)
        try:
            key_path.write_bytes(encoded)
        except Exception:
            # If writing fails we still return the in-memory key; the DB
            # contents will be inaccessible across restarts but the process
            # can continue.
            pass
        return key

    def _load_from_disk(self) -> None:
        """Populate in-memory structures from the auth DB, if present.

        The on-disk format is either:

        - Legacy cleartext JSON:
            { "users": [...], "sessions": [...] }

        - Encrypted JSON (AES-GCM, base64-wrapped):
            { "version": 1, "nonce": "...", "ciphertext": "..." }
        """
        if not self._db_path.exists():
            self._users_by_username = {}
            self._sessions_by_token = {}
            return

        try:
            with self._db_path.open("r", encoding="utf-8") as fh:
                raw_obj: Dict[str, Any] = json.load(fh)
        except Exception:
            # Corrupted or unreadable file; start with an empty DB.
            self._users_by_username = {}
            self._sessions_by_token = {}
            return

        # Detect encrypted vs legacy-plaintext format
        if "ciphertext" in raw_obj and "nonce" in raw_obj:
            try:
                nonce = base64.b64decode(raw_obj["nonce"])
                ciphertext = base64.b64decode(raw_obj["ciphertext"])
                aesgcm = AESGCM(self._crypto_key)
                plaintext = aesgcm.decrypt(nonce, ciphertext, associated_data=None)
                data: Dict[str, Any] = json.loads(plaintext.decode("utf-8"))
            except Exception:
                # Decryption failed; treat as empty DB rather than exposing
                # partial or corrupted data.
                self._users_by_username = {}
                self._sessions_by_token = {}
                return
        else:
            # Legacy cleartext structure
            data = raw_obj

        users_raw = data.get("users", [])
        sessions_raw = data.get("sessions", [])

        self._users_by_username = {}
        for entry in users_raw:
            try:
                rec = UserRecord(
                    id=str(entry["id"]),
                    username=str(entry["username"]),
                    password_hash=str(entry["password_hash"]),
                    salt=str(entry["salt"]),
                    created_at=str(entry.get("created_at", "")),
                )
            except KeyError:
                continue
            self._users_by_username[rec.username] = rec

        self._sessions_by_token = {}
        for entry in sessions_raw:
            try:
                rec = SessionRecord(
                    token=str(entry["token"]),
                    user_id=str(entry["user_id"]),
                    device_ip=entry.get("device_ip"),
                    method=str(entry.get("method", "unknown")),
                    created_at=str(entry.get("created_at", "")),
                    expires_at=entry.get("expires_at"),
                )
            except KeyError:
                continue
            self._sessions_by_token[rec.token] = rec

    def _save_to_disk_locked(self) -> None:
        """Persist current state to disk using authenticated encryption.

        Caller must hold ``self._lock``.
        """
        data = {
            "users": [asdict(u) for u in self._users_by_username.values()],
            "sessions": [asdict(s) for s in self._sessions_by_token.values()],
        }
        self._db_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = self._db_path.with_suffix(self._db_path.suffix + ".tmp")
        try:
            plaintext = json.dumps(data, separators=(",", ":")).encode("utf-8")
            aesgcm = AESGCM(self._crypto_key)
            nonce = secrets.token_bytes(12)
            ciphertext = aesgcm.encrypt(nonce, plaintext, associated_data=None)
            wrapped = {
                "version": 1,
                "nonce": base64.b64encode(nonce).decode("ascii"),
                "ciphertext": base64.b64encode(ciphertext).decode("ascii"),
            }
        except Exception:
            # As a last resort, fall back to writing cleartext JSON to avoid
            # losing data if encryption fails for some unexpected reason.
            wrapped = data

        with tmp_path.open("w", encoding="utf-8") as fh:
            json.dump(wrapped, fh, indent=2)
        tmp_path.replace(self._db_path)

    def _ensure_default_user(self) -> None:
        """Create a default random user on first use of the auth DB.

        This is called only when the auth database is empty. It generates
        a random username/password pair, persists it, and prints the
        credentials to stdout so the operator can log in or store them.
        """
        username = f"default-{secrets.token_hex(3)}"
        password = secrets.token_urlsafe(16)
        try:
            user = self.create_user(username, password)
        except Exception:
            # If something goes wrong, silently continue with an empty DB.
            return

        print(
            "[CAI API][auth] Default login created for this installation:\n"
            f"  username: {user.username}\n"
            f"  password: {password}\n"
            "  (These credentials are stored in the local auth database and "
            "will remain valid across API restarts.)",
        )

    # ------------------------------------------------------------------
    # Password hashing helpers
    # ------------------------------------------------------------------
    @staticmethod
    def _hash_password(password: str, salt_bytes: Optional[bytes] = None) -> tuple[str, str]:
        """Return (salt_hex, hash_hex) using PBKDF2-HMAC-SHA256."""
        if salt_bytes is None:
            salt_bytes = secrets.token_bytes(16)
        dk = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt_bytes, 100_000)
        return salt_bytes.hex(), dk.hex()

    @staticmethod
    def _verify_password(password: str, salt_hex: str, hash_hex: str) -> bool:
        try:
            salt_bytes = bytes.fromhex(salt_hex)
        except ValueError:
            return False
        _, computed_hash = AuthManager._hash_password(password, salt_bytes)
        return secrets.compare_digest(computed_hash, hash_hex)

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def has_users(self) -> bool:
        """Return True if there is at least one user in the DB."""
        with self._lock:
            self._load_from_disk()
            return bool(self._users_by_username)

    def create_user(self, username: str, password: str) -> UserRecord:
        """Create a new user with the given username and password."""
        normalized = username.strip()
        if not normalized:
            raise ValueError("username must not be empty")

        with self._lock:
            # Refresh from disk to avoid overwriting changes made by other processes.
            self._load_from_disk()
            if normalized in self._users_by_username:
                raise UserAlreadyExistsError(f"user '{normalized}' already exists")

            salt_hex, hash_hex = self._hash_password(password)
            now = datetime.now(timezone.utc).isoformat()
            user = UserRecord(
                id=secrets.token_hex(16),
                username=normalized,
                password_hash=hash_hex,
                salt=salt_hex,
                created_at=now,
            )
            self._users_by_username[normalized] = user
            self._save_to_disk_locked()
            return user

    def create_random_user_and_session_for_ip(
        self,
        ip_address: str,
    ) -> tuple[UserRecord, str, SessionRecord]:
        """Create a random user plus a session token bound to the IP.

        Returns a tuple of (user_record, plain_password, session_record).
        """
        # Generate a reasonably short but unique username.
        username = f"user-{secrets.token_hex(4)}"
        random_password = secrets.token_urlsafe(16)

        user = self.create_user(username, random_password)

        # Issue session token bound to this IP.
        session = self._create_session_for_user(user, device_ip=ip_address, method="ip_pairing")
        return user, random_password, session

    def login(
        self,
        username: str,
        password: str,
        *,
        device_ip: Optional[str] = None,
    ) -> SessionRecord:
        """Validate credentials and return a fresh session record."""
        normalized = username.strip()
        if not normalized:
            raise InvalidCredentialsError("invalid username or password")

        with self._lock:
            # Always reload latest users/sessions so that credentials created
            # from other processes (e.g. CLI /auth command) are honored.
            self._load_from_disk()
            user = self._users_by_username.get(normalized)
            if user is None:
                raise InvalidCredentialsError("invalid username or password")
            if not self._verify_password(password, user.salt, user.password_hash):
                raise InvalidCredentialsError("invalid username or password")

            session = self._create_session_for_user(user, device_ip=device_ip, method="password_login")
            return session

    def _create_session_for_user(
        self,
        user: UserRecord,
        *,
        device_ip: Optional[str],
        method: str,
    ) -> SessionRecord:
        now = datetime.now(timezone.utc)
        token = secrets.token_urlsafe(32)
        expires_at: Optional[str]
        if self._session_ttl_seconds > 0:
            expires_at = (now + timedelta(seconds=self._session_ttl_seconds)).isoformat()
        else:
            expires_at = None

        session = SessionRecord(
            token=token,
            user_id=user.id,
            device_ip=device_ip,
            method=method,
            created_at=now.isoformat(),
            expires_at=expires_at,
        )
        self._sessions_by_token[token] = session
        self._save_to_disk_locked()
        return session

    def validate_session_token(self, token: str) -> bool:
        """Return True if the token exists and is not expired.

        This also performs lazy expiry cleanup when tokens are past TTL.
        """
        with self._lock:
            # Refresh from disk so tokens created via CLI /auth add-ip or
            # other processes are visible to the API server.
            self._load_from_disk()
            record = self._sessions_by_token.get(token)
            if record is None:
                return False

            if record.expires_at:
                try:
                    expires = datetime.fromisoformat(record.expires_at)
                except ValueError:
                    # If expiry cannot be parsed, treat it as invalid and drop.
                    del self._sessions_by_token[token]
                    self._save_to_disk_locked()
                    return False

                now = datetime.now(timezone.utc)
                if expires <= now:
                    # Token has expired; remove it.
                    del self._sessions_by_token[token]
                    self._save_to_disk_locked()
                    return False

            return True
